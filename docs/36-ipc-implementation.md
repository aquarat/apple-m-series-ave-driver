# Implementing the AVE IPC transport

*Everything a C driver needs between "the coprocessor is executing" and "we
sent a command and read the reply": the scratch mailbox conversation, the
channel descriptor table the firmware writes into our memory, the ring
algorithm, the doorbell, and the interrupt. Struct layouts with byte offsets,
register offsets, and the host sequence in order.*

Every row is marked **confirmed** (read out of an instruction, VA cited),
**confirmed (both)** (read independently out of `AppleAVE2.kext` *and*
`ave_h13c.bin`), **inferred** (a chain over confirmed facts, chain stated) or
**unknown**, per [00-methodology.md](00-methodology.md).

Firmware VAs are **image virtual addresses** (`__TEXT` vmaddr 0, fileoff
`0x4000`; `__DATA` vmaddr `0x134000`, fileoff `0x138000`). Kext VAs are the
`0xfffffe0008……` ones. Nothing here was observed on hardware.

Builds on [08-ipc-transport.md](08-ipc-transport.md) and
[34-boot-handshake.md](34-boot-handshake.md); **corrects four statements in
them** — see §11.

Reproduce block at §13.

---

## 0. The results that matter

Three things were not known before this pass and each removes a runtime
unknown:

1. **The firmware builds the descriptor table with literal constants and we
   can read them.** `ChannelTableCreate` (fw `0xe0e70`) is a straight-line
   function with no data dependence except one flag, and that flag is a
   literal `wzr` at its only call site. So the entire channel layout is a
   compile-time constant of the firmware: **2 channels, `"IO"` and
   `"IO_T2H"`, doorbell bits 1 and 3, 992 and 993 slots, table size
   `0x1F280`.** §4.
2. **The ring is a ping-pong, not a one-way FIFO.** The phase bit written by
   `Send` is `type ^ 1`, a constant — there is no per-lap phase flip. A slot
   alternates ownership because the *peer* sends back into the same slot with
   the complementary constant. The host's own interrupt handler proves it:
   after receiving on `IO_T2H` it immediately `Send`s the same buffer back
   (kext `0xfffffe0008c19714`). §6.
3. **The firmware's `Set`/`Clear` register offsets confirm bank 2 `+0x0C` /
   `+0x10` a third time, and name the two registers on the other side of the
   pair**, `+0x08` and `+0x14` (fw `0xe2058`..`0xe2158`). §2.

---

## 1. This is not RTKit endpoint messaging

The prompt for this pass assumed a 64-bit RTKit mailbox message with endpoint
and type bitfields. **There is no such message anywhere in the host↔AVE
protocol.** The transport is: eight 32-bit scratch registers plus a one-bit
doorbell for boot, then shared-memory rings plus per-channel doorbell bits for
everything after.

The firmware *is* RTKit (`RTKit-3255.160.4.release`) and *does* contain a full
RTKit mailbox stack — `_RTK_mbi_endpoint_attach` (`0xf8210`),
`_RTK_mbi_endpoint_send_msg` (`0xf8280`), `_RTK_dev_mailbox128e_dispatch`
(`0x135338`), `PlatformIOPIPCManager::InitMailboxRoute` (`0xe66a4`). That
stack is used for **IOP↔IOP** routing: `InitMailboxRoute` walks a peer table,
skips its own index (`ldr w9,[x19,#8]; cmp x24,x9; b.eq` at `0xe674c`) and
logs `"PlatformIOPIPCManager %s AVE%d InitMboxInit Done status = 0x%x"`
(string file `0x135c0d`, VA `0x131c0d`) — i.e. AVE0↔AVE1, not AVE↔AP.

**The discriminating test** (per [00-methodology.md](00-methodology.md) trap
2):

```sh
grep -cE 'ailbox|RTKit|rtkit|ndpoint|RTK_' data/derived/symbols.txt       # 131
grep -cE 'ailbox|RTKit|rtkit|ndpoint|RTK_' data/derived/kext-symbols.txt  #   0
```

The firmware returns **131** symbols, `AppleAVE2.kext` returns **zero**. The
test demonstrably finds mailbox code when mailbox code is present, and finds
none on the host side. (Case-sensitive on purpose: `grep -i … IPI` matches 76
kext symbols, all of them the substring `iPi` in words like `MultiPiece`.
That variant of the test measures nothing.) Corroborating: the host's only IOP
primitives are
`AVE_IOP_{Config,Start,CheckIdle,GetCurrTime}_<variant>` — no send, no
receive, no endpoint — and the complete call list of `AVE_HwC::StartUpIOP`
([34](34-boot-handshake.md) §3) accounts for every step with scratch-register
and shared-memory calls only. **Confirmed.**

---

## 2. The register block — bank 2

Bank 2 = ADT `reg[2]`. `ave0`: **`0x20D050000`**, size `0x8000`.
`ave1`: `0x307050000`. (Bus addresses; `/arm-io` `ranges` translation already
applied — see [30-address-translation-bug.md](30-address-translation-bug.md)
and re-check with `tools/check_addrs.py` before any run.)

| off | who writes | who reads | role | evidence |
|---|---|---|---|---|
| `+0x08` | firmware | — | **IPI set, AVE → host**: write `1<<bit` to raise bit `bit` in `+0x10` | fw `0xe20a0`/`0xe20bc`, `0xe20e4` |
| `+0x0C` | host | — | **IPI set, host → AVE**: write `1<<bit` to raise bit `bit` in `+0x14` | kext `0xfffffe0008c91538`; fw `0xe209c` |
| `+0x10` | host (W1C) | host | **IPI status, AVE → host** | kext `0xfffffe0008c91908` (read), `0xfffffe0008c91954` (W1C) |
| `+0x14` | firmware (W1C) | firmware | **IPI status, host → AVE** | fw `0xe20b0`, `0xe2104`, `0xe2124`, `0xe213c` |
| `+0x18`..`+0x34` | both | both | scratch 0..7 | kext `0xfffffe0008c9129c`; fw `0xe1de8` |
| `+0x38` | host | — | `AVE_SVECtrl::SetIdle` | kext `0xfffffe0008c91cb0` |

**Confirmed (both).** The `+0x08`/`+0x14` row is new. `CPlatformIPIManager`'s
constructor (`0xe2058`) takes a `bool` and picks the register pair from it:

```
e2094:  cmp  w19, #0x0                 ; w19 = the bool
e209c:  mov  w9,  #0xc                 ; host-side  set  = 0x0C
e20a0:  csel w9, w9, w8, ne            ; w8 = 0x01050008  -> AVE-side set  = +0x08
e20ac:  mov  w10, #0x10                ; host-side  status = 0x10
e20b0:  csel w8, w10, w8, ne           ; w8 = 0x01050014  -> AVE-side status = +0x14
e20bc:  stp  w9, w8, [x0, #112]        ; this+112 = set off, this+116 = status off
```

`Set(bit)` is `str (1<<bit), [globalbase + this[112]]` (`0xe20e4`) — a plain
store with no read-modify-write. `Clear(bit)` stores `1<<bit` to the *status*
register (`0xe2104`). `Pending(bit)` is `(status >> bit) & 1` (`0xe213c`).
`globalbase` is `*(u64*)0x2649c8`, the same global `CPlatformGPIOManager` adds
`0x01050018` to in order to reach scratch 0 — so `globalbase = 0x20C000000`
and `0x01050008 → 0x20D050008 = bank2 + 0x08`.

The `bool` is `CPlatformEnvironment + 420`, and that byte is exactly the
standalone flag: `standalone = (GPIO::Read(0) != 0x08042006)` (fw `0xe115c`,
`strb w0,[x19,#420]` at `0xe1164`), passed at `0xe13cc`/`0xe13dc`. In the
host-driven boot the flag is **0**, so the firmware sets `+0x08` and polls
`+0x14`. **Confirmed (both).**

Consequences for the driver:

* `+0x0C` is **write-1-to-set, not read-modify-write** — inferred by symmetry
  with `+0x08`, whose set path (`0xe20e4`) is a bare store with no load. This
  closes [08](08-ipc-transport.md) §11's open question.
* `+0x10` is **write-1-to-clear**: `ClearIntr` writes the whole word it just
  read (kext `0xfffffe0008c9192c` → `Write32(bank2, 0x10, mask)`).
* Bit 0 of both `+0x0C`/`+0x10` is the scratch mailbox. Bits 1 and 3 are the
  two channels (§4). **Bit 2 is never used by either side on any path read
  here — unknown.**

`driver/ave_hw.h` currently defines `AVE_SVE_REG_08 0x08 /* table[2], purpose
unknown */`. That comment is wrong twice over: table index 2 is the *count* of
scratch registers (value 8), not an offset; and `+0x08` is a real register,
the firmware's IPI-set. Fix both.

---

## 3. The mailbox conversation, with predicted values

The full sequence — boot config block, five messages, ready flag — is already
specified instruction-by-instruction in
[34-boot-handshake.md](34-boot-handshake.md) §4–§13 and is unchanged. This
section adds only what is new: **what the firmware will actually put in those
registers.**

The firmware speaks first. `RealChannelCreateTarget` (fw `0xe4d34`) is the
whole conversation on the firmware side; its final `int` parameter is a
literal zero at its only call site (`str wzr,[sp,#40]` at `0xe1638`, call at
`0xe163c`), which fixes every constant below.

### Message 1 (firmware → host), scratch 0..3

| scratch | meaning | **predicted value** | evidence |
|---|---|---|---|
| 0 | channel count | **2** | `ChannelTableSizeGet` `0xe0da4` `csel x9,x10,x9,eq` with `w10 = 2` for flag 0; written at `0xe4e1c` |
| 1 | descriptor-table size, bytes | **`0x1F280`** (127 616) | `0xe0d84` `mov w10,#0xf280 / movk #1,lsl#16`; written at `0xe4e44` |
| 2 | protocol version | **`0x100`** | literal `mov w2,#0x100` at `0xe4e50` |
| 3 | firmware heap size, bytes | **`0xC0000`** (768 KiB) — *inferred* | `0xe5620`… see below; written at `0xe4e94` |

The heap size is `CPlatformEnvironment + 336`, set by the `CEnvironment`
constructor as `max(_sys_extra_heap_size_min, 0xC0000)`
(`cmp x9,#0xc0,lsl #12; csel x8,x9,x8,hi; str x8,[x0,#336]` at
`0xd9d18`..`0xd9d24`). `_sys_extra_heap_size_min` (fw `0x194008`, `__DATA`) is
**0** in the image, and `CPlatformEnvironment::Create` is called with size `0`
(`mov x0,xzr; bl 0xe1c24` at `0x5ad0`). Chain: image value 0 → `max(0,
0xC0000)` → `0xC0000`. It is **inferred** rather than confirmed only because a
runtime write to that `__DATA` word before `Create` cannot be excluded. The
driver reads it from scratch 3 regardless; treat `0xC0000` as the expected
value, not as a constant to hard-code.

`0xC0000` is already 16 KiB aligned, so `CreateFwHeap`'s round-up
(kext `0xfffffe0008c1c7e8`) is a no-op.

Cross-check on the size: `0x1F280` = `2*0x100` descriptors + `992*0x40` +
`993*0x40` + `0x40` of alignment slop = `0x200 + 0xF800 + 0xF840 + 0x40`.
That arithmetic closing exactly is the strongest available evidence that §4's
decode of `ChannelTableCreate` is right.

### Messages 2–5 and the ready flag

Unchanged from [34](34-boot-handshake.md) §7–§11. Summary of what the host
must send, in order:

| # | dir | scratch 0 | 1 | 2 | 3 |
|---|---|---|---|---|---|
| 2 | H→F | `FwIPC` IOVA lo | IOVA hi | `FwIPC` size | 0 |
| 3 | F→H | `fw_base` lo | `fw_base` hi | — | — |
| 4 | H→F | ipcinfo fw addr lo | hi | 0 | 0 |
| 5 | F→H | descriptor table fw addr lo | hi | client buffer size | — |

Before message 2 the host also writes the time-base delta to scratch 4 and 5.
After message 5 it writes `0x08042006` to scratch 3 and polls it back to 0.

One addition to [34](34-boot-handshake.md) §9's IPC info block: the firmware
reads **`ipcinfo + 0x4C` as a count of trailing `u32` words** and copies that
many words from `ipcinfo + 0x50` into an internal array
(`ldr w26,[x22,#76]` at `0xe58e8`; bounded copy `0xe5920`..`0xe5974`).
Apple's host `memset`s the whole `0x50` block to zero, so the count is 0 and
nothing is copied. **A driver must zero `+0x4C`.** Confirmed.

---

## 4. The channel descriptor table

### Who owns it

The host does not discover a firmware-owned table. It **allocates the memory
and hands the firmware a pointer** (message 4, `ipcinfo + 0x08`), the firmware
**writes the descriptors into that host memory**, and reports back where
(message 5). The host then asserts that the reported address translates back
to exactly the block it allocated (kext `0xfffffe0008c1fa14`).

Firmware side, confirmed: the channel-memory firmware address is reassembled
from `ipcinfo+0x08`/`+0x0C` at `0xe5798` (`orr x27, x28, x24, lsl #32`) and
passed straight to `ChannelTableCreate` at `0xe5988`; the return value is
written to scratch 0/1 at `0xe59d8`/`0xe5a00`.

### Allocation requirement

`ChannelTableCreate` aligns its base up to 64 (`add x8,x0,#0x3f; and
x19,x8,#~0x3f` at `0xe0e88`/`0xe0e8c`) and returns the *aligned* pointer. The
host compares that against its own unaligned allocation for equality. **So the
channel-memory block must be 64-byte aligned or the handshake fails at
message 5.** Confirmed. Size = scratch 1 of message 1 = `0x1F280`. Zero it
before message 4 (kext `bzero` at `0xfffffe0008c1effc`).

### Per-entry layout — stride `0x100`

| off | size | field | evidence |
|---|---|---|---|
| `+0x00` | 64 | name, `strncpy`-padded with NULs | fw `0xe0ea8` (`mov w2,#0x40`), copier `0xe00ec` |
| `+0x40` | u32 | direction code | fw `0xe0edc`; host `0xfffffe0008b4f7f0` |
| `+0x44` | u32 | doorbell / interrupt **bit index** | fw `0xe0eb0`; host `0xfffffe0008c44224` |
| `+0x48` | u32 | slot count | fw `0xe0ec4`; host `0xfffffe0008b4f808` |
| `+0x4C` | u64 | firmware address of the slot array, **unaligned split** into two `u32` stores | fw `0xe0ee0` (`stp w21,w9,[x19,#76]`); host `ldur x1,[x24,#76]` `0xfffffe0008c441e0` |
| `+0x54`..`+0xFF` | 172 | never written; zero because the host zeroed the block | — |

### The two entries the firmware will write — **confirmed**

With `base = ` the 64-byte-aligned channel-memory firmware address and
`slots = base + 0x200`:

| i | name | `+0x40` dir | `+0x44` bit | `+0x48` slots | `+0x4C` slot array |
|---|---|---|---|---|---|
| 0 | `"IO"` | **0** | **1** | **992** (`0x3E0`) | `base + 0x200` |
| 1 | `"IO_T2H"` | **1** | **3** | **993** (`0x3E1`) | `base + 0xFA00` |

Read out of `ChannelTableCreate`:

```
e0eb0:  str  w22, [x19, #68]     ; w22 = 1        desc0 +0x44 = 1
e0ec4:  str  w8,  [x19, #72]     ; w8  = 0x3e0    desc0 +0x48 = 992
e0edc:  str  wzr, [x19, #64]     ;                desc0 +0x40 = 0
e0ee0:  stp  w21, w9, [x19, #76] ; x21 = base+0x200
e0ee8:  add  x24, x21, x8        ; x8 = 0xf800 = 992*0x40
e0ef0:  mov  w8, #0x3e1
e0ef4:  str  w8,  [x19, #328]    ;                desc1 +0x48 = 993
e0efc:  str  w22, [x19, #320]    ;                desc1 +0x40 = 1
e0f04:  str  w24, [x19, #332]    ;                desc1 +0x4C = base+0xFA00
e0eb4/e0eec: bl 0xe00ec with x1 = "IO" (0x11e852) / "IO_T2H" (0x11e855), w2 = 0x40
```

Names verified by reading the image at file offsets `0x122852` and `0x122855`.

The `flag != 0` branch (`0xe0f08`..`0xe0f7c`) would add two more descriptors,
`"SHAREDMALLOC"` (dir 1, bit 3, 8 slots) and `"TERMINAL"` (dir 2, bit 0, 512
slots). **That branch is dead on this firmware** (flag is a literal zero,
§3) — and it would be fatal if taken, because the host's name table has only
`""`, `"IO"`, `"IO_T2H"` and an unmatched name aborts `CreateChannel` (§5).

### Total memory map of the channel block

```
base + 0x00000   desc[0] "IO"                     0x100
base + 0x00100   desc[1] "IO_T2H"                 0x100
base + 0x00200   IO      slot array   992 * 0x40 = 0x0F800
base + 0x0FA00   IO_T2H  slot array   993 * 0x40 = 0x0F840
base + 0x1F240   (end)          + 0x40 alignment slop = 0x1F280 requested
```

---

## 5. `CreateChannel` — what the host does for `"IO"` and `"IO_T2H"`

`AVE_IPC::CreateChannel(int nChannels, int version, unsigned long pDescArray)`
— kext `0xfffffe0008c440a0`. Guards: `nChannels != 0` (`0xc44198`),
`version >= 1` (`0xc4419c`), `pDescArray != NULL` (`0xc441a4`),
`GetDevArch() == 0x40` selects the 64-bit path (`0xc441b8`).

The host **allocates nothing** for a channel: the slot arrays already live
inside the block it handed the firmware in message 4. Per descriptor:

```c
for (i = 0; i < nChannels; i++) {                 /* d += 0x100, 0xc44350 */
    slots = Fw2KernelAddr(*(u64 *)(d + 0x4C));    /* 0xc441e0 / 0xc441e8  */
    if (!slots) return -err;                      /* 0xc441ec             */
    if (ChName2ID((char *)d, &id) != 0) return err;  /* 0xc441fc -> 0xc44814 */
    this->descIndex[id]   = i;                    /* this+0xC8, 0xc44220  */
    this->doorbellBit[id] = *(u32 *)(d + 0x44);   /* this+0xD4, 0xc4423c  */
    AppleAVEIOProcessorChannel::Create(
        &this->chan[i],        /* this+0x50 + i*0x28, 0xc4434c */
        d, slots,
        SetChIntr,             /* PAC ctx 0x2abe, 0xc44318..0xc44328 */
        this->instanceIndex,   /* this+0x10, 0xc44314 */
        this,                  /* 0xc44338 */
        1);                    /* is64, 0xc4433c */
}
this->nChannels = nChannels;   /* this+0x40, 0xc44584 */
```

**Correction to [34](34-boot-handshake.md) §10:** an unrecognised name is a
hard failure, not a skip. `cbnz w0, 0xc44814` at `0xc44200` jumps *forward
past* the loop back-edge at `0xc44358`, and `0xc44814` starts `mov x23, x0`,
where `w23` is the function's return register (`mov w23, #0` on the success
path at `0xc4457c`). Confirmed.

`AVE_IPC_ChName2ID` (`0xfffffe0008c4208c`) `strcmp`s against a 3-entry table
of **8-byte** pointers at `0xfffffe0007ee0b38` (`add x8,x22,x21,lsl #3` at
`0xc420c0`, `cmp x21,#3` at `0xc420d8`). Decoded per
[00-methodology.md](00-methodology.md) trap 5 (low 32 bits = image-relative
file offset): `0x275061 = ""`, `0x2a0757 = "IO"`, `0x2a075a = "IO_T2H"`.
Returns 0 and stores the index on a match (`str w21,[x19]`, `0xc4214c`).

`AppleAVEIOProcessorChannel::Create` (`0xfffffe0008b4f7b4`) is 0x28 bytes:

| off | field |
|---|---|
| `+0x00` | ring handle pointer |
| `+0x08` | u32 ring type |
| `+0x0C` | u8 is64 |
| `+0x10` | u32 instance index — **start of the notify cookie** |
| `+0x18` | `AVE_IPC *` |
| `+0x20` | u32 doorbell bit |

Direction → ring type (`0xfffffe0008b4f7f4`..`0xfffffe0008b4f804`):

| `desc[+0x40]` | host ring type | role |
|---|---|---|
| 0 | **1** (odd) | host is the *initiator*: it zero-initialises the slots and writes phase `0` |
| 1 | **0** (even) | host is the *responder*: slots are pre-initialised by the peer, host writes phase `1` |
| ≥2 | 2 | routes `Receive` to `UnidirectionalReceive64` (`tbnz w8,#1`, `0xfffffe0008b4f89c`) — not used on this firmware |

So concretely: **`"IO"` → host type 1, `"IO_T2H"` → host type 0.** The firmware
mirrors this via `TypeAdjust` (`0xe0f98`) whose lookup table at `0x11d028` is
`{1, 0, 0}` — i.e. it complements types 0 and 1. Confirmed (both).

---

## 6. The ring

`_IOProcessorChannelCreate64` `0xfffffe0008cb4124`, `Send64` `0xfffffe0008cb43a8`,
`Receive64` `0xfffffe0008cb44e8`, `MessageAvailable64` `0xfffffe0008cb42e0`.
Firmware peers: `IOProcessorChannelCreate` `0xcd90c`, `Send` `0xcdb78`,
`Receive` `0xcdc94` — **the firmware `Send` is instruction-for-instruction the
same algorithm** (compare `0xcdbbc`..`0xcdc40` with `0xfffffe0008cb43f0`..
`0xfffffe0008cb4490`). Confirmed (both).

### Slot — 64 bytes, first 24 used

| off | size | content |
|---|---|---|
| `+0x00` | u64 | `payload_fw_addr | phase`; bit 0 is the phase, bits 1..0 masked off on receive (`and x8,x8,#~3`, `0xfffffe0008cb4590`) |
| `+0x08` | u64 | zero-extended `u32` arg1 — **byte count** in AVE's use |
| `+0x10` | u64 | zero-extended `u32` arg2 — **flags**, firmware→host only |
| `+0x18`..`+0x3F` | 40 | never written, never read |

Stride `0x40` (`lsl x8,x8,#6` at `0xfffffe0008cb4324`, `…4400`, `…4540`).

### Host-private handle — 0x30 bytes, **not** in shared memory

| off | field | init (type odd) | init (type even) |
|---|---|---|---|
| `+0x00` | notify fn | — | — |
| `+0x08` | notify cookie | — | — |
| `+0x10` | `int type` | 1 | 0 |
| `+0x14` | `int nslots` | 992 | 993 |
| `+0x18` | slot array, host VA | — | — |
| `+0x20` | `int rd` (`-1` = nothing to read) | **-1** | **0** |
| `+0x24` | `int wr` (`-1` = full) | **0** | **-1** |
| `+0x28` | `int recv_count` | 0 | 0 |
| `+0x2C` | `int send_count` | 0 | 0 |

`movi d8, #0xffffffff00000000` / `#0xffffffff` then `str d8,[x19,#32]` at
`0xfffffe0008cb4174`/`…4180`/`…41d0` is where `rd`/`wr` are set; the type test
is `tbz w22,#0` at `0xfffffe0008cb417c`.

**There is no shared producer/consumer index.** Each side keeps its own pair
and walks in lockstep. The only shared synchronisation is the phase bit.

### Slot initialisation — only the odd side does it

```
0xfffffe0008cb4198:  ldrsw x9, [x19, #16]      ; x9 = type (1)
0xfffffe0008cb419c:  stp   xzr, xzr, [x20, #8] ; slot+0x08 = slot+0x10 = 0
0xfffffe0008cb41a0:  str   x9,  [x20]          ; slot+0x00 = type  -> phase 1
0xfffffe0008cb41c0:  add   x20, x20, #0x40
```

So the host must write `1` to word 0 of all **992** `"IO"` slots and zero the
next two words, and must **not** touch the 993 `"IO_T2H"` slots — the firmware
initialises those (its type there is 1). Confirmed.

### The phase bit is a constant, not a lap counter

```
0xfffffe0008cb4428:  ldrsw x8, [x19, #16]   ; type, reloaded every Send
0xfffffe0008cb442c:  eor   x8, x8, #0x1
0xfffffe0008cb4430:  orr   x8, x20, x8      ; payload | (type ^ 1)
0xfffffe0008cb4434:  str   x8, [x0]
```

and the receive test is

```
0xfffffe0008cb4574:  ldr x8, [x23]; and x8,x8,#1
0xfffffe0008cb457c:  ldr w9, [x19,#16]; and x9,x9,#1
0xfffffe0008cb4584:  cmp x8,x9 ; b.ne  -> return -1 (nothing available)
```

Both operands are constant for the life of the channel. A slot therefore holds
phase `type^1` when the peer has left something for us and phase `type` when
it has not. Since `Receive` **never writes the slot back**, the only thing
that returns a slot to the "empty" state is *the peer sending into it* with
its own complementary constant. The ring is a ping-pong. **Confirmed (both).**

The host's own interrupt handler is the direct proof:
`AVE_HwC::ProcessIntr_IPCCh` (`0xfffffe0008c196a0`), for `ch == 2`
(`"IO_T2H"`), does

```
0xfffffe0008c19708: ldr x0, [x19, #224]     ; AVE_IPC *
0xfffffe0008c1970c: ldur x2, [x29, #-40]    ; the buffer it just received
0xfffffe0008c19710: mov  w1, #0x2           ; channel 2
0xfffffe0008c19714: bl   AVE_IPC::Send      ; w3 = the size, set at 0xc196e8
```

— it echoes the buffer straight back on the same channel before doing anything
with it. For `ch == 1` (`"IO"`) there is no echo, because the firmware's reply
*is* the return of the slot.

### `Send64(h, payload_fw, arg1, arg2)`

```c
int i = (int)h->wr;
if (i == -1) return -1;                      /* ring full, 0xcb43f4  */
slot = h->slots + i * 0x40;
slot[0x08] = (u64)(u32)arg1;                 /* 0xcb441c */
slot[0x10] = (u64)(u32)arg2;                 /* 0xcb4424 */
slot[0x00] = payload_fw | (h->type ^ 1);     /* 0xcb4434 */
dsb st;                                      /* 0xcb4438 — the ONLY barrier */
/* clean(slot, 0x40) — hook is NULL on this driver, see below */
r = h->rd; w = h->wr;                        /* 0xcb4454 */
if (r == -1) { h->rd = w; r = w; }           /* 0xcb4460 */
w = (w == h->nslots - 1) ? 0 : w + 1;        /* csinc, 0xcb4478 */
h->wr = (r == w) ? -1 : w;                   /* csinv, 0xcb4480 */
h->send_count++;                             /* 0xcb448c */
h->notify(h->cookie);                        /* 0xcb44c8 — rings the doorbell */
return 0;
```

### `Receive64(h, &payload, &arg1, &arg2)`

```c
int i = (int)h->rd;
if (i == -1) return -1;                      /* 0xcb4534 */
slot = h->slots + i * 0x40;
/* invalidate(slot, 0x40) — hook is NULL */
if ((slot[0] & 1) != (h->type & 1)) return -1;   /* 0xcb4584 */
*payload = slot[0x00] & ~3ULL;               /* 0xcb4590 */
*arg1    = (u32)slot[0x08];                  /* 0xcb459c */
*arg2    = (u32)slot[0x10];                  /* 0xcb45a4 */
r = h->rd; w = h->wr;                        /* 0xcb45a8 */
if (w == -1) { h->wr = r; w = r; }           /* 0xcb45b4 */
r = (r == h->nslots - 1) ? 0 : r + 1;        /* 0xcb45c8 */
h->rd = (w == r) ? -1 : r;                   /* 0xcb45d4 */
h->recv_count++;                             /* 0xcb45e0 */
return 0;
```

`MessageAvailable64` is the phase test alone, without consuming
(`0xcb4358`..`0xcb436c`).

### Barriers and cache maintenance

`IOProcessorInit64` stores five hooks (`0xfffffe0008cb40f4`): entry hook,
exit hook, hook cookie, **clean(slot,0x40)**, **invalidate(slot,0x40)**.
`AVE_IPC::Init` calls both `IOProcessorInit` and `IOProcessorInit64` with **all
five arguments zero** (`0xfffffe0008c4268c`..`0xfffffe0008c426b8`, guarded by a
run-once byte at `0xfffffe000c69acf1`). The firmware's `IOProcessorInit` call
(`0xe4dcc`) passes real lock/unlock callbacks in `x2`/`x3`
(`ChannelInterruptLock` `0xe0fbc`, `Unlock` `0xe0fe4`) but **`x5 = x6 = 0`**,
i.e. its clean and invalidate hooks are NULL too.

**Neither side performs any cache maintenance on the ring.** The only ordering
primitive in the entire protocol is the `dsb st` in `Send`, between the slot
write and the index update / doorbell. **Confirmed (both).**

*Inferred:* the `FwIPC` surface must therefore be mapped so that plain loads
and stores on both sides are mutually visible — IO-coherent, or non-cacheable.
See §12; this is the riskiest thing in this document.

For Linux the safe translation is:

| Apple | Linux |
|---|---|
| `dsb st` after the slot write | `dma_wmb()` before the index update, and again before the doorbell `writel()` |
| (nothing) after the phase test | **add `dma_rmb()`** between reading the phase and reading the payload |
| `FwIPC` mapping | `dma_alloc_coherent()` |

The added `dma_rmb()` is a deliberate strengthening: Apple's code loads
`slot[0]` twice (`0xcb4574` for the phase, `0xcb458c` for the payload) with
nothing in between, which is only safe under a strongly-ordered mapping.

---

## 7. Doorbell and interrupt

### Ringing

The ring library knows nothing about MMIO; the doorbell is the notify callback
at `handle+0x00`. `CreateChannel` signs `AVE_IPC::SetChIntr`
(`0xfffffe0008c43bc8`) with PAC context `0x2abe` and passes the channel object's
`+0x10` as the cookie. `SetChIntr` walks `cookie[+0x08] → AVE_IPC`,
`AVE_IPC[+0x18] → AVE_SVECtrl`, and calls
`SetIPCIntr(cookie[+0x10] = desc[+0x44])` (`0xfffffe0008c43c44`,
`…c43c4c`, `…c43e00`), which is `SetIntr(1u << bit)`
(`lsl w1,w8,w1`, `0xfffffe0008c91c48`) → `Write32(bank2, 0x0C, 1<<bit)`.

For a driver that is simply:

```c
dma_wmb();
writel(1u << ch->doorbell_bit, sve + 0x0C);   /* bit 1 for IO, 3 for IO_T2H */
```

**One doorbell per `Send`.** Including the credit-return `Send` on `IO_T2H`.

### Receiving

`AVE_HwC::ProcessIntr` (`0xfffffe0008c1982c`), confirmed:

```c
if (this[+0xC0] != 3) return 0;                  /* 0xc19858 — state gate */
GetIntr(this->sve, &status);                     /* read bank2+0x10, 0xc1986c */
ClearIntr(this->sve, status);                    /* W1C the WHOLE word, 0xc19928 */
for (ch = 1; ch != 3; ch++) {                    /* 0xc1992c, 0xc199f8 */
    if (CheckChIntr(ipc, ch, status) == 0)       /* 0xc19954 */
        ProcessIntr_IPCCh(this, ch);             /* 0xc199f0 */
}
return 0;
```

`CheckChIntr(ch, status)` (`0xfffffe0008c43fac`) is
`((status >> doorbellBit[ch]) & 1) ? 0 : -1000` (`0xc44000`); if `status` is 0
it re-reads it itself (`0xc43fd8`).

`AVE_HwC::ProcessIntr_IPCCh(ch)` (`0xfffffe0008c196a0`), confirmed:

```c
while (AVE_IPC::Recv(ipc, ch, &buf, &size, &flags) == 0) {   /* 0xc196d4, 0xc1973c */
    if ((unsigned)(ch - 1) > 1) return -1002;                /* 0xc196e0 */
    if (ch == 1)
        AVE_HwC::ProcessIntr_CmdAck(this, buf, size);        /* 0xc19700 */
    else {
        AVE_IPC::Send(ipc, 2, buf, size);                    /* 0xc19714 — credit return */
        AVE_HwC::ProcessIntr_Cmd(this, buf, size, flags);    /* 0xc19724 */
    }
}
```

Note it **drains the channel in a loop** until `Recv` returns non-zero.

Two hazards, both restated from [34](34-boot-handshake.md) §14 because they
are still live:

* `ProcessIntr` W1Cs the *whole* status word, bit 0 included, so a live IRQ
  during the scratch handshake will steal `RecvIOPMsg`'s completion. Request
  the IRQ only after the ready flag clears.
* `RecvIOPMsg` acks before it reads the scratch registers
  (`0xfffffe0008c91754` precedes `0xfffffe0008c91778`). Do not reorder.

The AVE IRQ is ADT `interrupts` index 0 — `AVE_Drv::IO_start` registers exactly
one handler (`mov w4,#0x0` at `0xfffffe0008bdb214`); AIC 1031 for `ave0`.

---

## 8. A command, end to end

```
AVE_IPC::Alloc(size, &cmd_kva)          kext 0xfffffe0008c42f00   /* from the FwIPC pool */
AVE_HwC::MakeFwCmd_<X>(cmd_kva)                                   /* docs/07, docs/20 */
AVE_HwC::SendFwCmd(cmd_kva, size)       kext 0xfffffe0008c04ba4
  ├─ if (!buf || size <= 0)        -> error          0xc04c34/0xc04c38
  ├─ if (HwC[+0xC0] != 3)          -> error          0xc04c40
  └─ AVE_IPC::Send(ipc, 1 /* "IO" */, buf, size)     0xc04c50/0xc04c5c
       ├─ require 1 <= ch <= 2, buf != NULL          0xc44f04 / 0xc44f08
       ├─ fw = Kernel2FwAddr(buf); require fw != 0   0xc44f14 / 0xc44f18
       ├─ chan = this+0x50 + descIndex[1]*0x28       0xc45178..0xc45194
       └─ IOProcessorChannelSend64(h, fw, size, 0)   0xc451a4 -> 0xcb43a8
            └─ notify -> writel(1<<1, bank2+0x0C)
─────────────────────────────────────────────────────────────────────────
  firmware executes the command, then Sends the same buffer back on "IO"
  with phase 1, and raises host status bit 1 via bank2+0x08
─────────────────────────────────────────────────────────────────────────
IRQ -> AVE_HwC::ProcessIntr
  ├─ status = readl(bank2+0x10); writel(status, bank2+0x10)
  ├─ status bit 1 set -> ProcessIntr_IPCCh(1)
  │    └─ AVE_IPC::Recv(1, &buf, &size, &flags) -> ProcessIntr_CmdAck(buf, size)
  └─ status bit 3 set -> ProcessIntr_IPCCh(2)
       └─ Recv(2, ...) -> Send(2, buf, size) -> ProcessIntr_Cmd(buf, size, flags)
```

`AVE_HwC::SendFwCmd_Config` bypasses the `SendFwCmd` wrapper and calls
`AVE_IPC::Send(ipc, 1, buf, 0x78)` directly (`0xfffffe0008c05c30`..`0xc05c40`)
— same channel, same path. Confirmed. Command ids and struct sizes are in
[07-commands-abi.md](07-commands-abi.md); nothing there changes.

**Everything is asynchronous.** `SendFwCmd` returns as soon as the slot is
posted; the reply arrives on the interrupt. There is no polling wait anywhere
in the command path.

`AVE_IPC::Recv` (`0xfffffe0008c45550`) converts the slot's payload back with
`Fw2KernelAddr` (`0xc45994`), which range-checks against the `FwIPC` surface
and returns 0 if the firmware ever hands back an address outside it.

---

## 9. C definitions

```c
/* ---- bank 2, ave0 base 0x20D050000 ------------------------------------ */
#define SVE_IPI_SET_FW      0x08   /* AVE -> host set   (firmware writes)  */
#define SVE_IPI_SET_HOST    0x0C   /* host -> AVE set   (we write, W1S)    */
#define SVE_IPI_STAT_HOST   0x10   /* AVE -> host status (we read, W1C)    */
#define SVE_IPI_STAT_FW     0x14   /* host -> AVE status (firmware's)      */
#define SVE_SCRATCH(i)     (0x18 + 4*(i))          /* i = 0..7             */
#define SVE_IDLE            0x38

#define AVE_IPI_MBOX_BIT    0                      /* scratch mailbox      */

/* ---- channel descriptor, stride 0x100 --------------------------------- */
struct ave_ch_desc {
	char     name[0x40];      /* "IO" / "IO_T2H", NUL-padded             */
	__le32   direction;       /* 0 -> host type 1 ; 1 -> host type 0     */
	__le32   doorbell_bit;    /* 1 for IO, 3 for IO_T2H                  */
	__le32   nslots;          /* 992 / 993                               */
	__le32   slots_fw_lo;     /* +0x4C : 64-bit fw address, split        */
	__le32   slots_fw_hi;     /* +0x50                                   */
	u8       pad[0xAC];       /* +0x54 .. +0xFF, zero                    */
} __packed;                   /* sizeof == 0x100                          */

/* ---- ring slot, stride 0x40 ------------------------------------------- */
struct ave_ring_slot {
	__le64 payload;           /* fw address | phase (bit 0)              */
	__le64 arg1;              /* byte count                              */
	__le64 arg2;              /* flags, fw -> host only                  */
	u8     unused[0x28];
} __packed;                   /* sizeof == 0x40                           */

/* ---- host-private channel state --------------------------------------- */
struct ave_ring {
	struct ave_ring_slot *slots;   /* CPU VA inside the FwIPC mapping    */
	u32   nslots;
	u32   type;                    /* 1 for "IO", 0 for "IO_T2H"         */
	u32   doorbell_bit;            /* 1 / 3                              */
	s32   rd;                      /* -1 = nothing to read               */
	s32   wr;                      /* -1 = full                          */
};

/* ---- 80-byte IPC info block, arch 0x40 (docs/34 §9) ------------------- */
struct ave_ipc_info {            /* memset 0, then:                       */
	__le64 zero;             /* +0x00 never written on arch 0x40       */
	__le64 chanmem_fw;       /* +0x08 fw addr of the channel block     */
	__le64 log_fw;           /* +0x10 fw addr of the 64 KiB log block  */
	__le32 log_size;         /* +0x18 0x10000                          */
	__le32 heap_iova_lo;     /* +0x1C unaligned u64: heap DART addr    */
	__le32 heap_iova_hi;     /* +0x20                                  */
	__le32 heap_size;        /* +0x24                                  */
	__le32 dev_type;         /* +0x28 9 (ave0) / 10 (ave1)             */
	u8     rsvd[0x20];       /* +0x2C .. +0x4B zero                    */
	__le32 extra_count;      /* +0x4C MUST be 0 (fw 0xe58e8)           */
} __packed;                  /* sizeof == 0x50                            */
```

### Ring init

```c
static void ave_ring_init(struct ave_ring *r, const struct ave_ch_desc *d,
			  void *slots_va)
{
	u32 dir = le32_to_cpu(d->direction);

	r->slots        = slots_va;
	r->nslots       = le32_to_cpu(d->nslots);
	r->doorbell_bit = le32_to_cpu(d->doorbell_bit);
	r->type         = (dir == 1) ? 0 : (dir == 0) ? 1 : 2;

	if (r->type & 1) {                 /* "IO": we initialise the slots  */
		unsigned i;
		for (i = 0; i < r->nslots; i++) {
			r->slots[i].payload = cpu_to_le64(r->type);
			r->slots[i].arg1    = 0;
			r->slots[i].arg2    = 0;
		}
		r->rd = -1;  r->wr = 0;
	} else {                           /* "IO_T2H": peer initialises     */
		r->rd = 0;   r->wr = -1;
	}
	dma_wmb();
}
```

### Send / receive

```c
static int ave_ring_send(struct ave_device *ave, struct ave_ring *r,
			 u64 payload_fw, u32 arg1, u32 arg2)
{
	s32 i = r->wr, rd, w;

	if (i < 0)
		return -EAGAIN;                       /* ring full             */

	r->slots[i].arg1    = cpu_to_le64(arg1);
	r->slots[i].arg2    = cpu_to_le64(arg2);
	r->slots[i].payload = cpu_to_le64(payload_fw | (u64)(r->type ^ 1));
	dma_wmb();                                    /* Apple: dsb st         */

	rd = r->rd;  w = r->wr;
	if (rd < 0) { r->rd = w; rd = w; }
	w = (w == (s32)r->nslots - 1) ? 0 : w + 1;
	r->wr = (rd == w) ? -1 : w;

	writel(1u << r->doorbell_bit, ave->sve + SVE_IPI_SET_HOST);
	return 0;
}

static int ave_ring_recv(struct ave_ring *r, u64 *payload, u32 *arg1, u32 *arg2)
{
	s32 i = r->rd, rd, w;
	u64 word0;

	if (i < 0)
		return -ENOMSG;
	word0 = le64_to_cpu(r->slots[i].payload);
	if ((word0 & 1) != (r->type & 1))
		return -ENOMSG;                       /* peer has not filled it */
	dma_rmb();                                    /* stronger than Apple   */

	*payload = word0 & ~3ULL;
	*arg1    = (u32)le64_to_cpu(r->slots[i].arg1);
	*arg2    = (u32)le64_to_cpu(r->slots[i].arg2);

	rd = r->rd;  w = r->wr;
	if (w < 0) { r->wr = rd; w = rd; }
	rd = (rd == (s32)r->nslots - 1) ? 0 : rd + 1;
	r->rd = (w == rd) ? -1 : rd;
	return 0;
}
```

### Interrupt handler

```c
static irqreturn_t ave_irq(int irq, void *data)
{
	struct ave_device *ave = data;
	u32 status = readl(ave->sve + SVE_IPI_STAT_HOST);
	int ch;

	if (!status)
		return IRQ_NONE;
	writel(status, ave->sve + SVE_IPI_STAT_HOST);   /* W1C, whole word    */

	for (ch = AVE_CH_IO; ch <= AVE_CH_IO_T2H; ch++) {
		struct ave_ring *r = &ave->ring[ch];
		u64 fw; u32 size, flags;

		if (!r->slots || !(status & (1u << r->doorbell_bit)))
			continue;

		while (ave_ring_recv(r, &fw, &size, &flags) == 0) {
			void *buf = ave_fw_to_cpu(ave, fw);   /* NULL if OOB   */

			if (ch == AVE_CH_IO) {
				ave_cmd_ack(ave, buf, size);
			} else {
				ave_ring_send(ave, r, fw, size, 0); /* credit  */
				ave_fw_cmd(ave, buf, size, flags);
			}
		}
	}
	return IRQ_HANDLED;
}
```

### Address translation (unchanged, [34](34-boot-handshake.md) §8)

```c
#define FW_ADDR(p)   ((u64)((u8 *)(p) - ave->fwipc_cpu) + ave->fw_base)
#define IOVA_OF(p)   ((dma_addr_t)((u8 *)(p) - ave->fwipc_cpu) + ave->fwipc_iova)
#define FW_TO_CPU(f) ((void *)(ave->fwipc_cpu + ((f) - ave->fw_base)))
```

`fw_base` comes from message 3 and is **not** the IOVA.

---

## 10. The host sequence, in order

Steps 0–9 are [34](34-boot-handshake.md) §13 and are unchanged; this is the
list with the new constants and the post-`CreateChannel` steps filled in.

```
 0. power up; load firmware; allocate FwIPC (0x1400000, IOVA and size
    16 KiB aligned, dma_alloc_coherent)
 1. build the 56-byte boot config block in FwIPC;
    scratch0 = 0x08042006, scratch1:2 = its IOVA
 2. AVE_IOP::Config, AVE_IOP::Start        (IRQ still NOT requested)
 3. recv msg 1 -> nch, chanmem_sz, ver, heap_sz
       expect  2,  0x1F280,  0x100,  0xC0000
       reject  nch outside 1..3; chanmem_sz <= 0; ver != 0x100
       if heap_sz: allocate ALIGN(heap_sz, 0x4000) coherent, 16 KiB aligned
 4. scratch4:5 = time-base delta; send msg 2 = (FwIPC IOVA, size, 0)
 5. recv msg 3 -> fw_base
 6. chanmem = FwIPC alloc(chanmem_sz), 64-BYTE ALIGNED, zeroed
    logbuf  = FwIPC alloc(0x10000), zeroed
    ipcinfo = FwIPC alloc(0x50), zeroed; fill per §9; +0x4C must stay 0
    send msg 4 = (FW_ADDR(ipcinfo), 0, 0)
 7. recv msg 5 -> desc_fw, client_buffer_size
       reject  client_buffer_size > 0x13C000
       reject  FW_TO_CPU(desc_fw) != chanmem        (64-byte alignment check)
 8. for i in 0..nch-1:                       d = chanmem + i*0x100
       id = name_to_id(d)                    "" -> 0, "IO" -> 1, "IO_T2H" -> 2
       if id < 0: ABORT (Apple aborts too)
       ave_ring_init(&ring[id], d, FW_TO_CPU(get_unaligned_le64(d + 0x4C)))
    -> ring[1] = IO,     type 1, bit 1, 992 slots, host writes all 992 slots
    -> ring[2] = IO_T2H, type 0, bit 3, 993 slots, host writes none
 9. scratch3 = 0x08042006; poll until it reads 0 (fw's CFlowControllerBase
    ctor clears it), timeout 20000 * 100 us
10. NOW request the AIC irq (index 0)
11. cmd = FwIPC alloc(0x78); build sCAveCmdConfig; ave_ring_send(ring[1],
    FW_ADDR(cmd), 0x78, 0)
12. reply arrives as an interrupt: status bit 1 set, Recv on ring[1] returns
    the same fw address, size, flags
```

---

## 11. Corrections to existing documents

1. **[08](08-ipc-transport.md) §5** — `AVE_IPC::Send` "require `fwAddr != 0`
   and `(fwAddr & 1) == 0` `0xfffffe0008c44f18/f2c`". The second half is
   wrong: `0xc44f18` is the `fwAddr != 0` check (`cbz x0`), but `0xc44f2c` is
   `tbz w0,#0` on the return of `AVE_Log_CheckLevel` — [00-methodology.md](00-methodology.md)
   **trap 1**. There is no alignment check on the payload in `Send`. (One is
   still wise: `Receive` masks the low **two** bits, so a payload must be at
   least 4-byte aligned to survive the round trip.)
2. **[08](08-ipc-transport.md) §11** — "the exact semantics of `0x…05000C` —
   write-only set register vs. RMW" is now answered: write-1-to-set (§2).
   "The meaning of status bits other than bit 0": bits 1 and 3 are the two
   channels; bit 2 is unused by either side.
3. **[34](34-boot-handshake.md) §1** — "*Inferred:* descriptor bit indices are
   therefore ≥ 1". True for this firmware, but only because the two live
   channels use 1 and 3; the dead `"TERMINAL"` descriptor uses bit **0**
   (fw `0xe0f58`). The inference happens to hold; the reasoning does not
   generalise.
4. **[34](34-boot-handshake.md) §10** — "`if (ChName2ID(...) != 0) skip;`". It
   is not a skip; `CreateChannel` returns the error (§5).
5. **`driver/ave_hw.h`** — `AVE_SVE_REG_08 0x08 /* table[2], purpose unknown */`
   conflates the scratch-register *count* with a register offset. `+0x08` is
   the firmware's IPI-set register (§2).
6. **`driver/ave_ipc.c`** — the ring implementation is structurally wrong and
   must be replaced by §9: it flips the phase on wrap (there is no per-lap
   flip), it uses a single `head`/`tail` pair per channel with `next == tail`
   as "full" instead of Apple's `-1` sentinels (which loses a slot *and*
   mis-models the producer's `rd`, which tracks outstanding replies, not
   consumed messages), and it never returns credit on `IO_T2H`.

---

## 12. Must be discovered at runtime, and what to log

Nothing in §3–§7 needs runtime discovery to *implement*, but four values are
firmware-supplied and must be read, not assumed:

| value | where from | expected |
|---|---|---|
| channel count | msg 1 scratch 0 | 2 |
| descriptor-table size | msg 1 scratch 1 | `0x1F280` |
| firmware heap size | msg 1 scratch 3 | `0xC0000` (*inferred*) |
| `fw_base` | msg 3 | opaque; **not** the IOVA |
| client buffer size | msg 5 scratch 2 | unknown; `<= 0x13C000` |
| doorbell bits, slot counts, slot addresses | descriptor table | 1/3, 992/993, `base+0x200` / `base+0xFA00` |

### Log this on the first successful boot

```
[ave] pre-start scratch: %08x %08x %08x %08x %08x %08x %08x %08x
[ave] msg1: nch=%u chanmem=%#x ver=%#x heap=%#x        expect 2 1f280 100 c0000
[ave] msg3: fw_base=%#llx (fwipc iova=%pad)
[ave] msg5: desc_fw=%#llx cbuf=%#x  (chanmem cpu=%p fw=%#llx)
[ave] desc[%u] '%s' dir=%u bit=%u nslots=%u slots_fw=%#llx
        expect  0 IO      0 1 992 <base+0x200>
                1 IO_T2H  1 3 993 <base+0xfa00>
[ave] ready flag cleared after %u us
[ave] irq: status=%#x                    (every interrupt, first 32)
[ave] ring[%u] recv: payload=%#llx size=%u flags=%#x rd=%d wr=%d
```

The single highest-value line is the descriptor dump. If the four constants
per descriptor match the table in §4, then `ChannelTableCreate` was decoded
correctly, `fw_base` arithmetic works, and the channel-memory alignment is
right — three independent things confirmed by one printk.

The second is `irq: status`. It settles whether bit 2 is ever used and whether
the firmware really signals bit 1 for command acks (§7 assumes it does,
because that is the bit the host tests for channel 1).

### Still unknown

* **Whether the `FwIPC` mapping needs to be uncached.** See §12's risk note
  below. Not answerable from the binaries.
* **Bit 2 of the IPI registers.** Never set, never tested, on any path read.
* **The `flags` word** (slot `+0x10`, `Recv`'s fourth output). The host always
  sends 0; only the firmware sets it. Its consumer is
  `AVE_HwC::ProcessIntr_Cmd(buf, size, flags)` (`0xfffffe0008c17c34`), not
  analysed.
* **`client_buffer_size`** (msg 5 scratch 2, `GetClientBufferSize` fw
  `0x3d370`). Stored to a global and bounded at `0x13C000`; what it sizes is
  not established.
* **`AVE_HwC + 0xC0 == 3`**, the state gate on both `SendFwCmd` and
  `ProcessIntr`. Presumably "running"; the state machine was not traced.
* **The time-base delta's units and consequence** — unchanged from
  [34](34-boot-handshake.md) §15.
* **Type 2 (`UnidirectionalSend/Receive64`)** — still not disassembled. Not
  reachable on this firmware.

### The riskiest assumption

**That the `FwIPC` surface is coherent between the AVE DART path and the CPU
without any explicit cache maintenance.** Every other claim here was read out
of an instruction, usually on both sides. This one is read out of an
*absence*: both `IOProcessorInit` calls pass NULL clean/invalidate hooks, so
Apple's driver relies on the mapping alone. Which mapping that is, is not
stated anywhere in either binary.

If it is wrong, the failure mode is nasty and looks like a protocol bug: the
firmware never sees the boot config block, or sees a stale phase bit and the
ring silently stalls with no error anywhere. Mitigation, and what the driver
should do on the first attempt: allocate `FwIPC`, the firmware heap and the
log surface with `dma_alloc_coherent()` so the mapping is non-cacheable
whatever the DART turns out to be. That costs encoder throughput later and
buys an unambiguous first light now.

Second riskiest: that `bank2 + 0x0C` really is write-1-to-**set** rather than
write-the-whole-mask. The evidence is the firmware's mirror register at
`+0x08`, which is a bare store (`0xe20e4`), plus the host only ever writing a
single-bit mask. If it were a plain write, ringing channel 1 would clear a
pending channel 2 doorbell. Both readings produce identical behaviour for the
one-bit-at-a-time usage Apple actually has, so this cannot go wrong until we
send on both channels concurrently.

---

## 13. Reproduce

```sh
# The firmware's whole handshake, including ChannelTableSizeGet/Create
python3 tools/disas.py --fw --addr 0xe4d34 -n 0x200      # msg 1
python3 tools/disas.py --fw --addr 0xe4f1c -n 0x2c0      # wait, msg 2
python3 tools/disas.py --fw --addr 0xe51d8 -n 0x2c0      # map, msg 3
python3 tools/disas.py --fw --addr 0xe5494 -n 0x160      # ipcinfo read
python3 tools/disas.py --fw --addr 0xe5900 -n 0x1a0      # ChannelTableCreate call, msg 5
python3 tools/disas.py --fw --addr 0xe5dd8 -n 0xa0       # wait-for-host-doorbell

# The channel table itself — the source of every constant in §4
python3 tools/disas.py --fw --addr 0xe0d80 -n 0x220      # SizeGet + Alloc + Create
python3 tools/disas.py --fw --addr 0xe00ec -n 0x40       # the strncpy(.,.,0x40)

# The IPI register pair, and the standalone flag that selects it
python3 tools/disas.py --fw --addr 0xe2058 -n 0x110
python3 tools/disas.py --fw --addr 0xe1130 -n 0x60
python3 tools/disas.py --fw --addr 0xe1388 -n 0x90       # ctor call, bool = [x19,#420]

# Flag = 0 at the only call site of the handshake
python3 tools/disas.py --fw --addr 0xe15c0 -n 0xa0
python3 tools/disas.py --fw --addr 0xe1c24 -n 0x44       # Create(0)
python3 tools/disas.py --fw --addr 0xd9d10 -n 0x24       # heap = max(global, 0xC0000)

# The ring, both sides
python3 tools/disas.py --kext --addr 0xfffffe0008cb40f4 -n 0x100   # Init64 + Create64
python3 tools/disas.py --kext --addr 0xfffffe0008cb42e0 -n 0x360   # Avail/Send/Receive
python3 tools/disas.py --fw   --addr 0xcdb78 -n 0x120              # firmware Send
python3 tools/disas.py --fw   --addr 0xcd8f0 -n 0x20               # firmware Init

# Host channel binding and the command path
python3 tools/disas.py --kext --addr 0xfffffe0008c440a0 -n 0x2d0   # CreateChannel
python3 tools/disas.py --kext --addr 0xfffffe0008b4f70c -n 0x1c0   # channel glue
python3 tools/disas.py --kext --addr 0xfffffe0008c4208c -n 0x70    # ChName2ID
python3 tools/disas.py --kext --addr 0xfffffe0008c04ba4 -n 0x140   # SendFwCmd
python3 tools/disas.py --kext --addr 0xfffffe0008c196a0 -n 0x270   # ProcessIntr_IPCCh
python3 tools/disas.py --kext --addr 0xfffffe0008c19920 -n 0x100   # ProcessIntr loop
python3 tools/disas.py --kext --addr 0xfffffe0008c42680 -n 0x50    # NULL cache hooks
```

Strings and tables:

```sh
python3 - <<'EOF'
d = open('data/blobs/ave_h13c.bin','rb').read()          # VA + 0x4000 = file off
for va in (0x11e852, 0x11e855, 0x131531, 0x13153e):
    print(hex(va), d[va+0x4000:va+0x4040].split(b'\0')[0])
import struct
print('TypeAdjust', struct.unpack_from('<3i', d, 0x11d028+0x4000))     # (1,0,0)
print('extra_heap_min', struct.unpack_from('<Q', d, 0x138000+(0x194008-0x134000))[0])
EOF
```

The host's channel-name table (8-byte entries, chained fixups — trap 5):

```sh
python3 - <<'EOF'
d = open('data/blobs/kc.macho','rb').read()
for off in (0x275061, 0x2a0757, 0x2a075a):               # low 32 bits of the ptrs
    print(hex(off), d[off:off+16].split(b'\0')[0])
EOF
```

---

## 14. Proposed experiment (do not run — for the operator)

None beyond [34](34-boot-handshake.md) §16 experiment 1. This document adds no
new hardware requirement; it adds *predictions* that experiment 1 will confirm
or refute for free. If the boot handshake reaches message 1 and scratch 0..3
read `2 / 0x1F280 / 0x100 / 0xC0000`, §3 and §4 are validated end to end
without a second reboot.


---

## Verification pass (what was independently re-derived, and what was not)

Re-read from the bytes by the integrating session, not taken on trust:

| Claim | Evidence | Verdict |
|---|---|---|
| Only **two** channels exist | `0xe0f08: cbz w20, 0xe0f80` skips descriptors 2 and 3; the flag is `wzr` at the call site | confirmed |
| Descriptor stride `0x100` | descriptors written at `x19+0`, `+0x100`, `+0x200`, `+0x300` | confirmed |
| Field layout `+0x40` dir, `+0x44` IPI bit, `+0x48` count, `+0x4C/+0x50` ring base | `str` offsets 64/68/72/76/80 on desc 0 and the identical 320/324/328/332/336 on desc 1 | confirmed |
| `"IO"` dir 0 bit 1 992 slots; `"IO_T2H"` dir 1 bit 3 993 slots | the stored immediates, plus names at VA `0x11e852` / `0x11e855` | confirmed |
| Slot size is `0x40` | `x24 = x21 + 0xF800` is the `IO_T2H` ring base and `0xF800 / 992 = 64` | confirmed |
| Rings begin at `base + 0x200` | `add x21, x19, x8, lsl #8` with `x8` = channel count 2 | confirmed |
| Table size `0x1F280` | `0xe0d84: mov w10,#0xf280` + `movk #1,lsl#16`, selected on the flag | confirmed |

**The size deserves its own note**, because it does *not* equal the sum of its
parts and that looks like an error until you check the other branch. Content
ends at `0x1F240`; the literal is `0x1F280`, i.e. `0x40` of slop. The
four-channel literal in the same instruction pair is `0x27680`
(`mov w9,#0x7680` + `movk #2,lsl#16`), and four channels of content come to
`0x400 + 0xF800 + 0xF840 + 0x200 + 0x8000 = 0x27640` — **the same `0x40` of
slop**. Two independent constants reconciling with an identical remainder is
what promotes this from arithmetic that nearly works to a confirmed layout.

The two flag-gated channels are `"SHAREDMALLOC"` and `"TERMINAL"` (VA
`0x131531` / `0x13153e`). `TERMINAL` is worth remembering: if that flag can be
set, it is a second firmware output path independent of the log ring in
[33](33-firmware-logging.md). Not pursued — the flag is a literal zero on the
only path we have.

### Not independently re-derived

- **The ping-pong ring** (§9) — the claim that the phase bit is constant, that
  `Receive` never writes the slot back, and that the peer returns credit by
  sending into the slot. What *was* checked is that the kext's echo call
  (`0xfffffe0008c19714`) really does precede the processing call
  (`0xfffffe0008c17c34`), and that its callee opens with a log site consistent
  with a send path. The algorithm itself rests on the agent's reading that the
  firmware's `Send` is instruction-identical to the kext's.
- **The predicted message-1 values.** The driver reads them from the scratch
  registers rather than assuming them, so a wrong prediction costs a mismatch
  warning, not a failed boot. Treat them as a check, not an input.

### Consequence for the existing driver

`driver/ave_ipc.c`'s ring is reported structurally wrong (per-lap phase flip,
one head/tail pair, no credit return on `IO_T2H`). It is deliberately **not**
rewritten yet: the pending hardware test exercises the boot handshake only
(stages 1-15) and never reaches the ring, so rewriting it now would mean
shipping a second untested layer into an experiment whose whole purpose is to
isolate the first. The rewrite follows the boot result.
