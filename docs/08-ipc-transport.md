# The host↔firmware shared-memory transport (`AVE_IPC`)

Recovered entirely by static disassembly of `AppleAVE2.kext` inside
`kc.macho`, cross-checked against `ave_h13c.bin`. **No hardware was used and
nothing here was observed at runtime.** Every offset and constant below cites
the VA of the instruction it was read from, so it can be re-checked with
`tools/disas.py`.

The prior finding in [06-kext.md](06-kext.md) is **confirmed**: the AVE data
path is a shared-memory ring with an MMIO doorbell, not RTKit endpoint
messaging. It is more specific than "a ring", though — `AVE_IPC` is a thin
wrapper over a **generic Apple `IOProcessorChannel` library that is statically
linked into both the kext and the firmware**, with byte-identical code on both
sides.

---

## 1. Layers

| Layer | Where | Role |
|---|---|---|
| `AVE_IPC` | kext, 19 methods | owns the shared surface, name→id map, address translation |
| `AppleAVEIOProcessorChannel` | kext, 4 methods | per-channel glue; 0x28 bytes, embedded in `AVE_IPC` |
| `_IOProcessorChannel*64` | kext, C library | the actual ring algorithm |
| `_IOProcessorChannel*` | firmware, same library | the peer, identical code |
| `AVE_SVECtrl` | kext, 17 methods | the MMIO block: doorbell, interrupt status, scratch mailbox |
| `AVE_Reg` | kext | maps the five ADT MMIO ranges, `Read32`/`Write32(bank, offset)` |

`AVE_IPC` never touches MMIO itself. It signals through
`AVE_SVECtrl::SetIPCIntr`, which is the doorbell.

---

## 2. The shared memory: one surface, `FwIPC`

`AVE_IPC::Init(AVE_DevInfo*, AVE_SurfaceMgr*, uint32_t, AVE_SVECtrl*, AVE_FwImg*)`
at `0xfffffe0008c4256c`:

| Fact | Value | Read from |
|---|---|---|
| Surface config index | `31` (`_E_AVE_SurfaceIdx`) | `0xfffffe0008c426c4` (`mov w0, #0x1f`) |
| Surface name | `"FwIPC"` | cfg table `0xfffffe0007ee1050 + 31*0x10` |
| Surface flags word | `0x10d18` | same table entry |
| Surface size | `0x1400000` (20 MiB) | `0xfffffe0008c426ec` (`mov w4, #0x1400000`) |
| 4th ctor arg range check | `<= 3` | `0xfffffe0008c42670` (`cmp w23, #3`) |

The surface is created via `AVE_SurfaceMgr::CreateSurface` (`0xfffffe0008c80e30`)
and is DART-mapped; the 4th `Init` argument (0..3) is the **instance index**
passed to `AVE_Surface::GetDARTAddr(index, 0)` everywhere afterwards. It is
stored at `AVE_IPC+0x10` (`0xfffffe0008c42d1c`).

An `AVE_ChkPool` chunk allocator is then created **over the surface's DART
address range**, not its kernel range:

```
0xfffffe0008c42820  bl AVE_Surface::GetDARTAddr        -> x25
0xfffffe0008c42848  mov x2, x25       ; base
0xfffffe0008c4284c  mov x3, x27       ; size (= AVE_Surface::GetSize)
0xfffffe0008c42854  bl AVE_ChkPool::CreateWithMem
```

`AVE_IPC::Alloc`/`Free`/`AllocChannelMem`/`FreeChannelMem` are wrappers over
that pool. `AllocChannelMem` (`0xfffffe0008c44020`) caches a single allocation
at `AVE_IPC+0x48` and returns it on every subsequent call.

`AVE_IPC::GetInfo(uint64_t* pAddr, int* pSize)` (`0xfffffe0008c43a70`) returns
`GetDARTAddr(surface, AVE_IPC+0x10, 0)` and `GetSize(surface)` — i.e. the
**IOVA and size of the `FwIPC` surface**. This is the value handed to the
firmware at boot (§6).

### `AVE_IPC` object layout (confirmed)

| Offset | Type | Read from |
|---|---|---|
| `+0x00` | `AVE_DevInfo*` | `0xfffffe0008c42d18` |
| `+0x08` | `AVE_SurfaceMgr*` | `0xfffffe0008c42d18` |
| `+0x10` | `uint32` instance index (0..3) | `0xfffffe0008c42d1c` |
| `+0x18` | `AVE_SVECtrl*` | `0xfffffe0008c42d20` |
| `+0x20` | `AVE_FwImg*` | `0xfffffe0008c42d20` |
| `+0x28` | `AVE_Surface*` (`FwIPC`) | `0xfffffe0008c42d28` |
| `+0x30` | `AVE_ChkPool*` | `0xfffffe0008c42d28` |
| `+0x38` | `uint64` firmware base ("firmware offset") | `0xfffffe0008c45eac` |
| `+0x40` | `int` number of channels created | `0xfffffe0008c44584` |
| `+0x48` | channel-memory allocation | `0xfffffe0008c44038` |
| `+0x50` | `AppleAVEIOProcessorChannel[3]`, stride `0x28` | `0xfffffe0008c4434c`, `0xfffffe0008c45158` |
| `+0xC8` | `int[3]` channel-id → descriptor index | `0xfffffe0008c441d0`, `0xfffffe0008c44220` |
| `+0xD4` | `int[3]` channel-id → doorbell bit | `0xfffffe0008c441d4`, `0xfffffe0008c4423c` |

The two `int[3]` arrays are zeroed as `0xC8..0xD3` and `0xD4..0xDF` on the error
path at `0xfffffe0008c445f0`, which is how the count of **3** is fixed.

---

## 3. What a channel is

`AVE_IPC::CreateChannel(int nChannels, int version, unsigned long pDescArray)`
at `0xfffffe0008c440a0` walks an array of descriptors supplied by the firmware.

**Descriptor stride is `0x100`** (`add x24, x24, #0x100`, `0xfffffe0008c44350`).
This is the firmware's `ffwIOPChannelDescriptor`.

| Descriptor field | Meaning | Read from |
|---|---|---|
| `+0x00` | NUL-terminated name (passed straight to `strcmp`) | `0xfffffe0008c441f8` → `AVE_IPC_ChName2ID` |
| `+0x40` | direction code | `0xfffffe0008b4f7f0` (`ldr w8, [x1, #64]`) |
| `+0x44` | **doorbell / interrupt bit index** | `0xfffffe0008c44224`, `0xfffffe0008b4f7e8` |
| `+0x48` | slot count | `0xfffffe0008b4f808` (`ldr w8, [x1, #72]`) |
| `+0x4C` | firmware address of the slot array | `0xfffffe0008c441e0` (64-bit `ldur`) |

`+0x4C` is read as a **64-bit** value when `AVE_DevInfo::GetDevArch() == 0x40`
(`0xfffffe0008c441b8`) and as a 32-bit value otherwise
(`0xfffffe0008c44400`). M1 Pro/Max are arch `0x40` — see §7.

### Channel names and ids

`AVE_IPC_ChName2ID(char*, int*)` (`0xfffffe0008c4208c`) `strcmp`s the descriptor
name against a 3-entry table at `0xfffffe0007ee0b38` (loop bound
`cmp x21, #3` at `0xfffffe0008c420d8`):

| `_E_AVE_IPC_Ch` | name |
|---|---|
| 0 | `""` (empty string; never matches a real descriptor) |
| 1 | `"IO"` |
| 2 | `"IO_T2H"` |

`AVE_IPC::Send` and `AVE_IPC::Recv` both range-check `ch - 1 <= 1`, i.e. accept
**only ids 1 and 2** (`0xfffffe0008c44efc` and `0xfffffe0008c45650`).

So: **at most 3 channels, of which 2 are usable, named `IO` and `IO_T2H`.** The
actual count comes from the firmware at runtime (`CreateChannel` arg 1), so the
static image cannot say whether both are always created. `T2H` = target-to-host.

`CreateChannel`'s second argument is only ever consumed by log format strings in
the disassembled paths; the caller passes a value that was checked against
`0x100` earlier (`0xfffffe0008c1e6d0`), so it is most likely a protocol version.
**Inferred.**

### Direction

`AppleAVEIOProcessorChannel::Create` (`0xfffffe0008b4f7b4`) maps
`descriptor[+0x40]` to the library's channel *type*
(`0xfffffe0008b4f7f4`..`0xfffffe0008b4f804`):

| `desc[+0x40]` | type |
|---|---|
| 0 | 1 |
| 1 | 0 |
| ≥2 | 2 |

Only the low bit of the type matters to the ring: the side whose type is **odd**
initialises the slots and is the producer for that ring; the side whose type is
**even** is the consumer. Type 2 additionally routes `Receive` to
`_IOProcessorChannelUnidirectionalReceive64` (`tbnz w8, #1` at
`0xfffffe0008b4f89c`). Type 2 is not analysed further here.

---

## 4. The ring

`_IOProcessorChannelCreate64` = `0xfffffe0008cb4124`,
`Send64` = `0xfffffe0008cb43a8`, `Receive64` = `0xfffffe0008cb44e8`,
`MessageAvailable64` = `0xfffffe0008cb42e0`.

The firmware's `_IOProcessorChannelCreate` (`0xcd90c` in `ave_h13c.bin`) is the
same routine with the same `0x40` stride and the same handle layout — see
`0xcd998` (`add x19, x19, #0x40`) and `0xcd9e4` (`str d8, [x20, #32]`).

### Slot

**Slot size is `0x40` bytes** (`lsl x8, x8, #6` at `0xfffffe0008cb4324`,
`0xfffffe0008cb4400`, `0xfffffe0008cb4540`). Only the first 24 bytes are used:

| Slot offset | Content |
|---|---|
| `+0x00` | `uint64`: payload firmware address `\| phase`; bit 0 is the phase/ownership bit, bits 1..0 are masked off on receive (`and x8, x8, #~3`, `0xfffffe0008cb4590`) |
| `+0x08` | `uint64` holding a zero-extended `uint32` argument (`str x8, [x0, #8]`, `0xfffffe0008cb441c`) |
| `+0x10` | `uint64` holding a zero-extended `uint32` argument (`0xfffffe0008cb4424`) |
| `+0x18`..`+0x3F` | not written or read |

`AVE_IPC::Send` fills these as `(fwAddr, size, 0)`; `AVE_IPC::Recv` reads them
back as `(fwAddr, size, flags)` — see §5.

### Handle (host-private, 0x30 bytes, `kalloc`ed at `0xfffffe0008cb415c`)

| Offset | Content | Read from |
|---|---|---|
| `+0x00` | notify callback (PAC ctx `0x2abe`) | `0xfffffe0008cb4168` |
| `+0x08` | callback cookie | `0xfffffe0008cb4168` |
| `+0x10` | `int` type | `0xfffffe0008cb416c` |
| `+0x14` | `int` slot count | `0xfffffe0008cb416c` |
| `+0x18` | kernel VA of the slot array | `0xfffffe0008cb4170` |
| `+0x20` | `int` read index (`-1` = ring empty) | `0xfffffe0008cb41d0` |
| `+0x24` | `int` write index (`-1` = ring full) | `0xfffffe0008cb41d0` |
| `+0x28` | `int` receive counter | `0xfffffe0008cb4178`, `0xfffffe0008cb45dc` |
| `+0x2C` | `int` send counter | `0xfffffe0008cb4488` |

**The head/tail indices live in host-private memory, not in shared memory.**
This is the single most important structural fact for a Linux driver: there is
no producer/consumer index pair in the ring header to synchronise. The only
shared state is each slot's phase bit. Each side keeps its own indices and walks
them in lockstep with the peer.

Initial state (`0xfffffe0008cb4174`..`0xfffffe0008cb41d0`):

* type odd (producer): `read = -1`, `write = 0`; every slot's word0 is
  pre-initialised to `type` (`str x9, [x19]` at `0xfffffe0008cb41a0`).
* type even (consumer): `read = 0`, `write = -1`; slots untouched.

### Send (`_IOProcessorChannelSend64`)

```
i = handle[+0x24];  if (i == -1) return -1;              // ring full
slot = base + i*0x40;
slot[0x08] = (u64)(u32)arg1;
slot[0x10] = (u64)(u32)arg2;
slot[0x00] = payload | (type ^ 1);                       // flips the phase bit
dsb st;                                                  // 0xfffffe0008cb4438
r = handle[+0x20]; w = handle[+0x24];
if (r == -1) { handle[+0x20] = w; r = w; }
w = (w == nslots-1) ? 0 : w+1;
handle[+0x24] = (r == w) ? -1 : w;
handle[+0x2C]++;
handle[+0x00](handle[+0x08]);                            // ← rings the doorbell
```

The final indirect call is at `0xfffffe0008cb44c0`. **This is where the doorbell
is rung** — the ring library itself knows nothing about MMIO.

### Receive (`_IOProcessorChannelReceive64`)

```
i = handle[+0x20];  if (i == -1) return -1;              // nothing queued
slot = base + i*0x40;
if ((slot[0] & 1) != (type & 1)) return -1;              // phase mismatch: empty
*out_payload = slot[0x00] & ~3ULL;
*out_arg1    = (u32)slot[0x08];
*out_arg2    = (u32)slot[0x10];
   ... symmetric index advance, handle[+0x28]++ ...
```

`MessageAvailable64` is just the phase-bit test without consuming.

### Cache maintenance

`IOProcessorInit64` (`0xfffffe0008cb40f4`) stores five callbacks in globals at
`0xfffffe000c69a000 + {3896, 3904, 3912, 3920, 3928}`. Two of them are the
per-slot clean/invalidate hooks, called as `fn(slot, 0x40)`
(`0xfffffe0008cb41ac`, `0xfffffe0008cb434c`, `0xfffffe0008cb4448`,
`0xfffffe0008cb4568`).

`AVE_IPC::Init` calls **both** `IOProcessorInit` and `IOProcessorInit64` with all
five arguments zero (`0xfffffe0008c4268c`..`0xfffffe0008c426b8`), so on this
driver **no explicit cache maintenance is performed on the ring**; only the
`dsb st` in `Send64` remains. *Inferred:* the `FwIPC` surface must therefore be
mapped coherently (or the AVE DART path is IO-coherent).

---

## 5. `AVE_IPC::Send` / `Recv` / `SetChIntr` / `CheckChIntr`

### `Send(_E_AVE_IPC_Ch ch, void* pBuf, int size)` — `0xfffffe0008c44e08`

```
require 1 <= ch <= 2 and pBuf != NULL                      0xfffffe0008c44efc
fwAddr = Kernel2FwAddr(pBuf)                               0xfffffe0008c44f14
require fwAddr != 0 and (fwAddr & 1) == 0                  0xfffffe0008c44f18/f2c
idx  = this[0xC8][ch]                                      0xfffffe0008c45178
chan = this + 0x50 + idx*0x28                              0xfffffe0008c45188
AppleAVEIOProcessorChannel::Send(chan, fwAddr, size, 0)    0xfffffe0008c451a4
```

So what is transferred is **a firmware address plus a length** — the payload
itself lives elsewhere in the `FwIPC` surface. Log string:
`"sent IPC buffer %p %d %d<->%d 0x%llx %d"` (`0xfffffe00072f09f6`).

### `Recv(_E_AVE_IPC_Ch ch, void** ppBuf, int* pSize, uint32_t* pFlags)` — `0xfffffe0008c45550`

```
require 1 <= ch <= 2 and pSize != NULL                     0xfffffe0008c45650
chan = this + 0x50 + this[0xC8][ch]*0x28                   0xfffffe0008c45680
AppleAVEIOProcessorChannel::Receive(chan, &fwAddr, &n1, &n2)  0xfffffe0008c456ac
*ppBuf  = Fw2KernelAddr(fwAddr)                            0xfffffe0008c45994
*pSize  = n1                                               0xfffffe0008c459a0
if (pFlags) *pFlags = n2                                   0xfffffe0008c459ac
```

The fourth output is the slot's second `uint32`, logged as `0x%x`
(`0xfffffe00072f0b83`). Its meaning is **unknown**; `Send` always writes 0 there,
so it is set by the firmware only.

### `SetChIntr` — the doorbell callback — `0xfffffe0008c43bc8`

`CreateChannel` signs `0xfffffe0008c43bc8` (= `AVE_IPC::SetChIntr`) with PAC
context `0x2abe` and passes it as the ring's notify callback
(`0xfffffe0008c44318`..`0xfffffe0008c44328`). The cookie is *not* the `AVE_IPC*`
directly: `AppleAVEIOProcessorChannel::Create` passes `this + 0x10`
(`mov x4, x0` / `str w8, [x4, #16]!` at `0xfffffe0008b4f7dc`), whose layout is

| Channel object offset | Content |
|---|---|
| `+0x10` | `AVE_IPC+0x10` (instance index) |
| `+0x18` | `AVE_IPC*` |
| `+0x20` | `descriptor[+0x44]` (doorbell bit) |

`SetChIntr(void* p)` therefore does:

```
pIPC = p[+8]                                  0xfffffe0008c43c44
pSVE = pIPC[+0x18]                            0xfffffe0008c43c4c
AVE_SVECtrl::SetIPCIntr(pSVE, p[+0x10])       0xfffffe0008c43e00
```

i.e. **`SetIPCIntr(descriptor[+0x44])`**.

### `CheckChIntr(_E_AVE_IPC_Ch ch, uint32_t status)` — `0xfffffe0008c43fac`

```
if (status == 0) AVE_SVECtrl::GetIntr(this[+0x18], &status)  0xfffffe0008c43fd8
bit = this[0xD4][ch]                                          0xfffffe0008c43ffc
return ((status >> bit) & 1) ? 0 : -1000                      0xfffffe0008c44000
```

`AVE_HwC::ProcessIntr` (`0xfffffe0008c19908`+) reads the interrupt status, calls
`AVE_SVECtrl::ClearIntr(status)` (`0xfffffe0008c19928`) to acknowledge, then
loops from `ch = 1` (`mov w20, #1`, `0xfffffe0008c1992c`) calling
`CheckChIntr(ch, status)` and, on a hit, `AVE_HwC::ProcessIntr_IPCCh(ch)`
(`0xfffffe0008c196a0`).

**So both directions use the same interrupt-bit numbering, taken from the
channel descriptor.**

---

## 6. The doorbell register

`AVE_SVECtrl::SetIPCIntr(int ch)` (`0xfffffe0008c91c38`) is
`SetIntr(1u << ch)` (`lsl w1, w8, w1`, `0xfffffe0008c91c48`).

`AVE_SVECtrl::SetIntr(uint32 mask)` (`0xfffffe0008c91510`):

```
0xfffffe0008c91534  ldp x0, x8, [x19, #24]   ; x0 = AVE_Reg*, x8 = reg-offset table
0xfffffe0008c91538  ldr w2, [x8]             ; offset = table[0]
0xfffffe0008c9153c  mov w1, #2               ; register bank 2
0xfffffe0008c91544  bl  AVE_Reg::Write32
```

`AVE_Reg::Write32(bank, offset, value)` (`0xfffffe0008c53e58`) is literally

```
if (bank > 5) return;
*(volatile u32*)(this[0x40 + bank*8] + offset) = value;
```

(`0xfffffe0008c53e70`..`0xfffffe0008c53e78`). `AVE_Reg::Init`
(`0xfffffe0008c532d0`) fills `this[0x40 + i*8]` with the virtual address of the
`i`-th device memory range, mapped in ADT order (`mov x1, x23` = loop counter at
`0xfffffe0008c5335c`), and uses **5** ranges unless `GetDevType() == 0x1e`
(`0xfffffe0008c53338`). M1 Pro/Max `DevType` is 9/10 (§7), so **5 banks — exactly
the five `reg` entries of the ADT `ave0`/`ave1` node**.

*Inferred but well-supported:* `_E_AVE_RegType` bank `n` = ADT `reg` entry `n`.
That makes **bank 2 = `0x20D050000` size `0x8000`** for `ave0` and
`0x307050000` for `ave1`.

### The offset table

`AVE_SVECtrl::Init` stores `AVE_SVECtrl_GetReg(chipType)` at `AVE_SVECtrl+0x20`
(`0xfffffe0008c91010`). `AVE_SVECtrl_GetReg` (`0xfffffe0008c90e44`) indexes a
21-entry pointer table at `0xfffffe0007ee0f80` with `chipType - 1`. For
`chipType` 6 and 7 (t6000, t6001 — §7) the entry is the table at
**`0xfffffe00072748b4`**:

| Table index | Byte offset | Value | Used by |
|---|---|---|---|
| 0 | +0x00 | **`0x0C`** | `SetIntr` → **doorbell** |
| 1 | +0x04 | **`0x10`** | `GetIntr` (read) / `ClearIntr` (write) → **interrupt status, W1C** |
| 2 | +0x08 | `8` | number of scratch registers |
| 3..10 | +0x0C..+0x28 | `0x18,0x1C,0x20,0x24,0x28,0x2C,0x30,0x34` | scratch registers 0..7 |
| 67 | +0x10C | `0x38` | `SetIdle` |

(`SetIntr` uses `table[0]`: `0xfffffe0008c91538`. `GetIntr` uses `table[1]`:
`0xfffffe0008c91908`. `ClearIntr` uses `table[1]`: `0xfffffe0008c91954`.
`WriteScratch` bounds-checks against `table[2]` and indexes `&table[3]`:
`0xfffffe0008c91284`..`0xfffffe0008c912c0`. `SetIdle` uses `[table+268]`:
`0xfffffe0008c91cb0`.)

### Result

| | ave0 | ave1 |
|---|---|---|
| Doorbell (host → AVE), write `1 << bit` | **`0x20D05000C`** | **`0x30705000C`** |
| Interrupt status (AVE → host), read; write-1-to-clear | **`0x20D050010`** | **`0x307050010`** |
| Scratch mailbox regs 0..7 | `0x20D050018` .. `0x20D050034` | `0x307050018` .. `0x307050034` |

`bit` is `descriptor[+0x44]` of the channel being signalled. Bit **0** of the
status register is the scratch-mailbox "IOP has replied" flag — `RecvIOPMsg`
polls `Read32(bank2, 0x10)` and tests bit 0 (`0xfffffe0008c91658`,
`tbnz w0, #0` at `0xfffffe0008c91664`), retrying up to 2000 times
(`mov w27, #0x7d0`, `0xfffffe0008c91684`).

Two further scratch-register constants, for bring-up:
`SetIOPFlag(idx)` writes **`0x08042006`** to scratch `idx`
(`0xfffffe0008c91258` + `0xfffffe0008c9125c`); `ClearIOPFlag` writes 0;
`CheckIOPFlag` reads it back (`0xfffffe0008c91394`).

**Not determined:** whether writing bit *b* of `0x…05000C` is a pure
edge/set-only register or read-modify-write (the kext only ever writes a single
mask, never reads it), and what the other bits of `0x…050010` mean beyond bit 0
and the per-channel bits.

---

## 7. Which chip this applies to

`AVE_DevInfo::RetrieveDevID` (`0xfffffe0008baad50`) reads the ADT property
**`soc-id`** (string at `0xfffffe000728f03a`) and looks it up in a name table at
`0xfffffe0007edf0f0` (16-byte entries of `{const char* prefix, int number}`,
formatted `"%s%d"`). The resulting `devID` indexes a 0x48-byte-stride table at
`0xfffffe0007edba00` (`0xfffffe0008ba9eac`, `mov w8, #0x48`).

The committed ADT dump gives `ave0: soc-id = t6000` and
`ave1: soc-id = t6001, sve-id = 1` (`data/derived/adt-ave-full.txt`).

| node | `soc-id` | devID | devType | chipType | arch | devNum |
|---|---|---|---|---|---|---|
| `ave0` | `t6000` | 11 | 9 | 6 | `0x40` | 1 |
| `ave1` | `t6001` | 12 | 10 | 7 | `0x40` | 2 |

(`devType`/`chipType` from the entry's `+4`/`+8`; `arch` and `devNum` from the
sub-struct pointed to by entry `+0x10`, fields `+0x0C` and `+0x04`, read via
`0xfffffe0008baa96c`.)

Both chip types select the same register table, so §6 holds for both instances.
`arch == 0x40` selects the 64-bit descriptor field and
`IOProcessorChannelCreate64`.

---

## 8. Address spaces and the four translation helpers

Three address spaces are in play:

| Name | What it is |
|---|---|
| **kernel** | the host's CPU virtual mapping of the `FwIPC` surface (`AVE_Surface::GetKernelAddr`) |
| **DART** | the IOVA the AVE hardware sees (`AVE_Surface::GetDARTAddr(instance, 0)`) |
| **firmware** | what the firmware calls an address: `kernelOffsetInSurface + AVE_IPC[+0x38]` |

```
Kernel2FwAddr(k)   = (k - surfKernelBase) + fw_base       0xfffffe0008c454d4
Fw2KernelAddr(f)   = (f - fw_base) + surfKernelBase       0xfffffe0008c44cf4
Kernel2DARTAddr(k) = (k - surfKernelBase) + surfDartBase  0xfffffe0008c45e14
DART2KernelAddr(d) = (d - surfDartBase) + surfKernelBase  0xfffffe0008c45cac
```

All four bounds-check the input against `[base, base + GetSize)` and return 0
otherwise (e.g. `0xfffffe0008c45444`..`0xfffffe0008c4544c`). `surfDartBase` is
always `GetDARTAddr(AVE_IPC[+0x10], 0)`.

`fw_base` = `AVE_IPC[+0x38]`, and it is set in exactly two places:

* `AVE_IPC::Init`, **only when `GetDevArch() == 0x20`**
  (`cmp w0, #0x20` at `0xfffffe0008c42b44`):
  `fw_base = GetDARTAddr(surface) - AVE_FwImg::GetBaseAddr()`
  (`0xfffffe0008c42b68`/`0xfffffe0008c42b6c`). On arch `0x40` the branch is taken
  to `0xfffffe0008c42d14` and `+0x38` is left as constructed.
* `AVE_IPC::UpdateFwBaseAddr(uint64 v)` (`0xfffffe0008c45e90`), which is a plain
  `str x1, [x0, #56]` (`0xfffffe0008c45eac`). Its log string is
  `"firmware offset %p %d 0x%llx"` (`0xfffffe00072f0cf1`) — the same string
  `Init` uses.

So on M1 Pro/Max the firmware base is **supplied by the firmware at boot**, and
"firmware address" is whatever base the firmware reports plus the offset into the
`FwIPC` surface.

---

## 9. Bring-up sequence (`AVE_HwC::StartUpIOP`, `0xfffffe0008c1d354`)

`AVE_HwC+0x78` is the `AVE_SVECtrl*`, `AVE_HwC+0xE0` the `AVE_IPC*`.

1. An earlier `RecvIOPMsg` yields four words; they are loaded as
   `w21, w23, w22, w24` at `0xfffffe0008c1e0c8`/`0xfffffe0008c1e0cc`.
   `w22` is compared against `0x100` (`0xfffffe0008c1e6d0`) and gates the whole
   arch-`0x40` path. *Inferred:* `w22` is a protocol version and `w21` the
   channel count later handed to `CreateChannel`.
2. `AVE_IPC::GetInfo(&iova, &size)` — `0xfffffe0008c1e6e4`.
3. Split `iova` into low/high halves (`0xfffffe0008c1e930`) and
   `AVE_SVECtrl::SendIOPMsg(lo, hi, size, 0)` — `0xfffffe0008c1eb88`.
   `SendIOPMsg` (`0xfffffe0008c9143c`) writes its four arguments to scratch
   registers 0..3, i.e. **bank 2 offsets `0x18`, `0x1C`, `0x20`, `0x24`**
   (`0xfffffe0008c91478`, `0xfffffe0008c91498`, `0xfffffe0008c914bc`,
   `0xfffffe0008c914e0`), each guarded by the scratch count.
4. `AVE_SVECtrl::RecvIOPMsg(&lo, &hi, 0, 0)` — `0xfffffe0008c1ec1c`.
5. Recombine `lo | hi<<32` (`0xfffffe0008c1ecfc`) and
   `AVE_IPC::UpdateFwBaseAddr(that)` — `0xfffffe0008c1ee34`.
6. `AVE_IPC::AllocChannelMem(size, &chanMem)` — `0xfffffe0008c1ee48`.
7. A second `SendIOPMsg` / `RecvIOPMsg` pair (`0xfffffe0008c1f76c`,
   `0xfffffe0008c1f818`) returns a 64-bit firmware address in
   `[x29-112]`.
8. `Fw2KernelAddr` of that address is compared for equality against the
   `AllocChannelMem` result (`0xfffffe0008c1fa14`..`0xfffffe0008c1fa1c`) — i.e.
   **the descriptor array lives inside the channel memory the host allocated**,
   and the host verifies the firmware points at it.
9. `AVE_IPC::CreateChannel(count, version, pDescKernelVA)` —
   `0xfffffe0008c1fa30`.

---

## 10. Firmware-side cross-check

| Firmware symbol | VA | Note |
|---|---|---|
| `_IOProcessorChannelCreate` | `0xcd90c` | same algorithm, same `0x40` slot stride (`0xcd998`), same handle layout (`0xcd95c`..`0xcd9e4`) |
| `_IOProcessorChannelSend` / `Receive` | `0xcdb78` / `0xcdc94` | the peer halves |
| `CChannelManager::Init(ulong, char const*, CChannelManagerType, ffwIOPMessageEntry*, ulong)` | `0xdb858` | |
| `CChannelManager::DoorBellSet(void(*)(void*), void*)` | `0xdb850` | 8 bytes — a plain setter |
| `CRealChannel::DoorBellRingThrottled(void*)` | `0xe3230` | takes a lock, increments a pending counter at `this+196`; if the throttle word at `this+192` is 0, calls a virtual method (vtable `+40`) on a global object at `0x19e690` with `this+208` as the argument (`0xe328c`..`0xe32ac`) |
| `RealChannelCreate(CObject*, ulong, ffwIOPChannelDescriptor*, bool, int)` | `0xe43d0` | consumes the same descriptor type the host is handed |
| `PlatformIOPIPCManager::SendNotification(uchar, ulong long)` | `0xe6acc` | not analysed |

`CRealChannel+208` is the mirror of the host's `descriptor[+0x44]`: the firmware
also signals by **bit index**, through a platform singleton, exactly as
`AVE_IPC::SetChIntr` → `AVE_SVECtrl::SetIPCIntr` does on the host.

---

## 11. What is still open

* **Which channels actually get created.** The count comes from the firmware at
  runtime. Statically we know only: ≤ 3 slots exist, the name table has
  `""`/`IO`/`IO_T2H`, and `Send`/`Recv` accept ids 1 and 2.
* **Slot counts and firmware addresses** of the rings — both are firmware-supplied
  (`descriptor[+0x48]`, `descriptor[+0x4C]`).
* **The third word of a received message** (`Recv`'s `pFlags`). `Send` always
  writes 0; only the firmware sets it.
* **The exact semantics of `0x…05000C`** — write-only set register vs. RMW — and
  the meaning of status bits other than bit 0.
* **The scratch mailbox protocol** beyond the two exchanges in `StartUpIOP`:
  `SendIOPMsg` writes up to four words with no visible sequence number or
  command id in the disassembled call sites, and `RecvIOPMsg`'s completion is a
  poll on status bit 0. There is no `ClearIntr` of bit 0 visible inside
  `RecvIOPMsg` in the range examined — that was not chased down.
* **`descriptor[+0x40] >= 2` (type 2, "unidirectional")** — the
  `UnidirectionalSend/Receive64` variants were not disassembled.
* **`AVE_Reg` bank ↔ ADT `reg` index** is inferred from the sequential mapping
  loop and the count of 5, not from a literal in the image. Everything in §6
  depends on it. A single m1n1 read of `0x20D050010` on a booted machine would
  settle it.
* `AVE_IPC::Print` (`0xfffffe0008c45f88`) and `DestroyChannel`
  (`0xfffffe0008c42d30`) were not analysed beyond the object-layout facts above.
