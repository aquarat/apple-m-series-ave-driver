# The boot handshake and channel setup

Phase 4 of the roadmap: the scratch-register mailbox, from ASC start to
`CreateChannel`, specified to the instruction.

Everything here was read out of `AppleAVE2.kext` (inside `kc.macho`) **and** out
of `ave_h13c.bin`. Where both sides agree the fact is marked **confirmed
(both)**, which is the strongest evidence class in
[00-methodology.md](00-methodology.md) after Apple's own manifests. Kext VAs are
`0xfffffe0008……`; firmware VAs are the small ones (`0xe4d34`). Nothing here was
observed on hardware.

Re-check any line with:

```sh
python3 tools/disas.py --kext --addr 0xfffffe0008c9161c -n 0x2d4
python3 tools/disas.py --fw   --addr 0xe4d34 -n 0x2c0
```

**This document supersedes two statements elsewhere.** [08-ipc-transport.md](08-ipc-transport.md)
§11 records that "there is no `ClearIntr` of bit 0 visible inside `RecvIOPMsg`" —
there is, at `0xfffffe0008c91754`. And [31-bringup-state.md](31-bringup-state.md)
says "the only call after `AVE_IOP::Start` is `RecvIOPMsg`, so the host does not
poke it awake" — true of the calls *after* Start, but the host writes three
scratch registers **before** `AVE_IOP::Config`, and the firmware reads them as
its first act. That is almost certainly why our core starts and then says
nothing. See §4.

---

## 1. The register block — confirmed from both sides

`AVE_SVECtrl` reaches MMIO through `AVE_Reg::Read32/Write32(bank, offset)` with
**bank 2**, using a per-chip offset table. For `chipType` 6 and 7 (t6000 /
t6001) that table is at `0xfffffe00072748b4`
(`AVE_SVECtrl_GetReg`, `0xfffffe0008c90e44`; stored to `AVE_SVECtrl+0x20` at
`0xfffffe0008c91010`).

| table idx | byte off | value | role | read from |
|---|---|---|---|---|
| 0 | +0x00 | `0x0C` | doorbell, host → AVE (`SetIntr`) | `0xfffffe0008c91538` |
| 1 | +0x04 | `0x10` | interrupt status, AVE → host, W1C | `0xfffffe0008c91658`, `0xfffffe0008c91954` |
| 2 | +0x08 | `8` | **number of scratch registers** | `0xfffffe0008c91284` |
| 3..10 | +0x0C..+0x28 | `0x18,0x1C,…,0x34` | scratch 0..7 | `0xfffffe0008c9129c` (`add x8,x8,#0xc`; index `idx*4`) |

**docs/08 §6 is confirmed, not corrected.** The firmware proves it
independently. `CPlatformGPIOManager::CPlatformGPIOManager` (`0xe1db8`)
constructs its `CGPIOManager` base with count **8** (`mov w3, #0x8`, `0xe1dc4`)
and stores a register offset of **`0x01050018`** (`mov w8,#0x18; movk w8,#0x105,lsl #16`,
`0xe1de8`/`0xe1dec`). `CPlatformGPIOManager::Read(idx)` (`0xe1e54`) is:

```c
if (idx < this->count)                       // count = 8
    return *(u32 *)(*(u64 *)0x2649c8 + this->off + idx*4);   // off = 0x01050018
```

The ADT gives `ave0` `reg[4] = 0x20C000000` and `reg[2] = 0x20D050000`.
`0x20C000000 + 0x01050018 = 0x20D050018` — **exactly bank 2 + 0x18**. Same for
`ave1`: `0x306000000 + 0x01050018 = 0x307050018 = reg[2] + 0x18`. Two
independently derived numbers landing on the same address is also the first
direct evidence that the `AVE_Reg` bank *n* ↔ ADT `reg[n]` mapping (inferred in
docs/08 §6, and the thing everything else rests on) is right.

| | ave0 | ave1 |
|---|---|---|
| doorbell (host → AVE) | `0x20D05000C` | `0x30705000C` |
| status (AVE → host), W1C | `0x20D050010` | `0x307050010` |
| scratch 0..7 | `0x20D050018`..`0x034` | `0x307050018`..`0x034` |

**Bit 0 of both registers is the scratch mailbox.** Bit 0 of `+0x0C` is what
`SendIOPMsg` rings; bit 0 of `+0x10` is what `RecvIOPMsg` waits on. Channel
doorbells use `descriptor[+0x44]`, and `AVE_HwC::ProcessIntr` dispatches
channels starting at `ch = 1` (`mov w20, #1`, `0xfffffe0008c1992c`) — bit 0 is
never handed to a channel. *Inferred:* descriptor bit indices are therefore ≥ 1.

---

## 2. The primitives — confirmed

`AVE_SVECtrl` layout, from `AVE_SVECtrl::Init` (`0xfffffe0008c90fbc`):

| off | field | read from |
|---|---|---|
| +0x00 | `const _S_AVE_Cfg *` | `0xfffffe0008c9101c` |
| +0x08 | `AVE_DevInfo *` | `0xfffffe0008c9101c` |
| +0x10 | `uint32` instance index (0..3) | `0xfffffe0008c91024` |
| +0x18 | `AVE_Reg *` | `0xfffffe0008c91020` + `0xfffffe0008c911f0` |
| +0x20 | register offset table | `0xfffffe0008c91010` |

### `WriteScratch(int idx, uint32 v)` — `0xfffffe0008c9127c`

```c
if (idx >= table[2]) return -1002;           // 0xfffffe0008c91288
Write32(bank2, table[3 + idx], v);           // 0xfffffe0008c912c8
return 0;
```

### `ReadScratch(int idx, uint32 *out)` — `0xfffffe0008c913c4`

Same bounds check, `Read32`.

### `SetIOPFlag(int idx)` — `0xfffffe0008c91210`

```c
Write32(bank2, table[3 + idx], 0x08042006);  // 0xfffffe0008c91258/0xfffffe0008c9125c
```

**`0x08042006` is the boot magic.** `ClearIOPFlag` (`0xfffffe0008c912e4`)
writes 0.

### `CheckIOPFlag(int idx)` — `0xfffffe0008c9134c`

Returns **0** if the register reads 0, **1** if it reads `0x08042006`, **−1**
otherwise (`0xfffffe0008c91398`..`0xfffffe0008c913ac`).

### `SendIOPMsg(uint a, uint b, uint c, uint d)` — `0xfffffe0008c9143c`

```c
if (table[2] >= 1) Write32(bank2, table[3], a);   // scratch0, 0xfffffe0008c91480
if (table[2] >= 2) Write32(bank2, table[4], b);   // scratch1, 0xfffffe0008c914a4
if (table[2] >= 3) Write32(bank2, table[5], c);   // scratch2, 0xfffffe0008c914c8
if (table[2] >= 4) Write32(bank2, table[6], d);   // scratch3, 0xfffffe0008c914ec
SetIntr(1);                                       // 0xfffffe0008c914f8 -> bank2+0x0C = 1
return 0;
```

**`SendIOPMsg` rings the doorbell itself** (`SetIntr(1)`, i.e. bit 0 of
`+0x0C`). docs/08 §9 did not record this; it is essential.

### `RecvIOPMsg(uint *p0, uint *p1, uint *p2, uint *p3)` — `0xfffffe0008c9161c`

**It polls. It is not interrupt-driven.** It also acknowledges.

```c
n = 0;
while ((Read32(bank2, table[1]) & 1) == 0) {          // 0xfffffe0008c91658 / tbnz w0,#0 @c91664
    if (n >= cfg->timeout_scale * 2000)               // 0xfffffe0008c91720 (mul), c91728
        goto timeout;                                 // PrintRegs, return -1017 (0xfffffe0008c918cc)
    IODelay(1000);                                    // 1000 us, 0xfffffe0008c9172c/c91730
    n++;
}
ClearIntr(1);                                         // 0xfffffe0008c91754 -> W1C bit 0 of +0x10
if (p0 && table[2] >= 1) *p0 = Read32(bank2, table[3]);   // scratch0, 0xfffffe0008c91778
if (p1 && table[2] >= 2) *p1 = Read32(bank2, table[4]);   // scratch1, 0xfffffe0008c917a4
if (p2 && table[2] >= 3) *p2 = Read32(bank2, table[5]);   // scratch2, 0xfffffe0008c917d0
if (p3 && table[2] >= 4) *p3 = Read32(bank2, table[6]);   // scratch3, 0xfffffe0008c917f8
return 0;
```

Answers to the four questions asked about it:

- **Registers read, in order:** the status register `bank2+0x10` (repeatedly,
  bit 0), then scratch 0, 1, 2, 3 — `bank2+0x18/0x1C/0x20/0x24`.
- **Wait:** status bit 0. Poll loop, `IODelay(1000)` per iteration.
- **Ack:** `ClearIntr(1)` writes 1 to `bank2+0x10` before any scratch is read.
  The W1C ack happens **before** the payload read, which is safe only because
  the firmware does not write the scratch registers again until the host
  answers.
- **The four outputs are just scratch 0..3.** Any pointer may be NULL and is
  skipped. Their *meaning* is per-message and given in §5–§9.

`cfg->timeout_scale` is `_S_AVE_Cfg + 0x18`. `AVE_Cfg_Default`
(`0xfffffe0008b637fc`) zeroes 384 bytes and then sets `+0x14 = +0x18 = 1`
(`movi v0.2s,#1; stur d0,[x0,#20]`, `0xfffffe0008b63844`/`0xfffffe0008b63848`).
So the **default timeout is 2000 × 1 ms = 2 s** (overridable by boot-args via
`AVE_Cfg_RetrieveBootArgs`, `0xfffffe0008b63864`).

### Firmware-side equivalents

| host | firmware |
|---|---|
| `ReadScratch(i)` | `CGPIOManager::instance` vtable `+40` = `CPlatformGPIOManager::Read(i)` (`0xe1e54`) |
| `WriteScratch(i,v)` | vtable `+48` = `Write(i,v)` (`0xe1f34`) |
| `SetIntr(1)` (host → fw doorbell) | `CIPIManager::instance` vtable `+56` polls it, `+48` clears it (`0xe5dd8`) |
| `RecvIOPMsg` wait on status bit 0 | `CIPIManager::instance` vtable `+40` **raises** it (`0xe4ebc`, `0xe544c`, `0xe5a54`) |

`0xe5dd8` is the firmware's "wait for the host's doorbell, then clear it"
helper, structurally identical to `RecvIOPMsg`.

---

## 3. Where `SendIOPMsg` is actually called from

Reconciling docs/08 §9 with docs/31: **both were right about different
things.** The complete call list of `AVE_HwC::StartUpIOP`
(`0xfffffe0008c1d354`), main path only, in address order — this extends the
table in docs/31, which stopped at the calls it had enumerated:

| VA | call |
|---|---|
| `0xc1d468` | `AVE_DPM::SetIOP` |
| `0xc1d474` | `AVE_DPM::SetHw` |
| `0xc1d47c` | `AVE_FwImg::UpdateImage` |
| `0xc1d50c` | `AVE_IPC` ctor |
| `0xc1d528` | `AVE_IPC::Init` |
| `0xc1d690` | **`AVE_IPC::Alloc(0x38, &fwcfg)`** |
| `0xc1d978` | **`AVE_HwC::MakeFwCfg(fwcfg)`** |
| `0xc1da64` | **`AVE_SVECtrl::SetIOPFlag(0)`** |
| `0xc1db50` | **`AVE_IPC::Kernel2DARTAddr(fwcfg)`** |
| `0xc1dc7c` | **`AVE_SVECtrl::WriteScratch(1, lo32)`** |
| `0xc1dd10` | **`AVE_SVECtrl::WriteScratch(2, hi32)`** |
| `0xc1ddfc` | `AVE_IOP::Config` |
| `0xc1dee8` | `AVE_IOP::Start` |
| `0xc1dfe4` | `RecvIOPMsg(&w0,&w1,&w2,&w3)` — **msg 1** |
| `0xc1e208` | `AVE_IOP::GetCurrTime` |
| `0xc1e534` | `WriteScratch(4, lo32(tdelta))` |
| `0xc1e5cc` | `WriteScratch(5, hi32(tdelta))` |
| `0xc1e6e4` | `AVE_IPC::GetInfo(&iova,&size)` |
| `0xc1e784` | `AVE_HwC::CreateFwHeap(w3)` (if `w3 != 0`) |
| `0xc1eb88` | `SendIOPMsg(iova_lo, iova_hi, size, 0)` — **msg 2** |
| `0xc1ec1c` | `RecvIOPMsg(&lo,&hi,0,0)` — **msg 3** |
| `0xc1ee34` | `AVE_IPC::UpdateFwBaseAddr(lo|hi<<32)` |
| `0xc1ee48` | `AVE_IPC::AllocChannelMem(w1, &chanMem)` |
| `0xc1effc` | `bzero(chanMem, w1)` |
| `0xc1f00c` | `AVE_IPC::Alloc(0x50, &ipcinfo)` |
| `0xc1f1f0` | `AVE_IPC::Alloc(0x10000, &HwC[0x118])` |
| `0xc1f3c4…` | fill `ipcinfo` |
| `0xc1f76c` | `SendIOPMsg(ipcinfo_fw_lo, ipcinfo_fw_hi, 0, 0)` — **msg 4** |
| `0xc1f818` | `RecvIOPMsg(&d0,&d1,&d2,0)` — **msg 5** |
| `0xc1fa0c` | `AVE_IPC::Fw2KernelAddr(d0|d1<<32)` |
| `0xc1fa30` | `AVE_IPC::CreateChannel(nch, ver, descKVA)` |
| `0xc1fb58` | `AVE_IPC::Alloc(0xf0, &HwC[0x108])` |
| `0xc1fcbc` | `SetIOPFlag(3)` |
| `0xc1fcc8`/`0xc1fdac` | `CheckIOPFlag(3)` until it returns 0 |

So `SendIOPMsg` is called **twice**, both after `Start` and both after a
`RecvIOPMsg`. The firmware speaks first in every exchange; the host only ever
replies. But before `Config`/`Start` the host does write scratch 0/1/2 with raw
`SetIOPFlag`/`WriteScratch`, without a doorbell — that is the boot argument
block, not a message.

---

## 4. Before Start: the boot config block — confirmed (both)

This is the missing piece. The core is started with `scratch0 = 0x08042006` and
`scratch1:scratch2` holding the **DART address of a 56-byte `_S_AVE_Fw_Cfg`**.

### Host side

```
AVE_IPC::Alloc(0x38, &fwcfg_kva)   // 0xc1d688 (out ptr), 0xc1d68c (size 0x38)
AVE_HwC::MakeFwCfg(fwcfg_kva)      // 0xc1d978
SetIOPFlag(0)                      // 0xc1da64  -> scratch0 = 0x08042006
d = AVE_IPC::Kernel2DARTAddr(fwcfg_kva)   // 0xc1db50
WriteScratch(1, (u32)d)            // 0xc1dc74/0xc1dc7c
WriteScratch(2, d >> 32)           // 0xc1dd08/0xc1dd10
AVE_IOP::Config(); AVE_IOP::Start();
```

`AVE_IPC::Alloc` returns a **kernel VA**: `ChkPool::Alloc` yields a DART
address, and `0xfffffe0008c43338`..`0xfffffe0008c43360` converts it
`kva = dart - GetDARTAddr(surf) + GetKernelAddr(surf)`. So the allocation lives
in the 20 MiB `FwIPC` surface, and what goes into the scratch registers is its
IOVA.

### `_S_AVE_Fw_Cfg`, 56 bytes — `AVE_HwC::MakeFwCfg` (`0xfffffe0008c1cbf0`)

| off | size | value | read from |
|---|---|---|---|
| `+0x00` | u32 | `AVE_HwC+0x48` — instance index (0..3) [^inst] | `0xc1cc18` |
| `+0x04` | u32 | `AVE_DevInfo::GetDevID()` (11 = ave0, 12 = ave1) | `0xc1cc24` |
| `+0x08` | u32 | `GetDevNum()` (1 / 2) | `0xc1cc30` |
| `+0x0C` | u32 | `GetDevNumPerGroup()` | `0xc1cc3c` |
| `+0x10` | u64 | `GetDevSubIDFlag()` | `0xc1cc48` |
| `+0x18` | u32 | `GetDevRevision()` | `0xc1cc54` |
| `+0x1C` | u32 | — (not written) | |
| `+0x20` | u64 | **DART address of the firmware log surface** | `0xc1cc68`, via `AVE_FwLog::GetInfo` |
| `+0x28` | u32 | firmware log surface size | `0xc1cc6c` |
| `+0x2C` | u32 | — (not written) | |
| `+0x30` | u32 | `AVE_Cfg_Get()[+0x30]` (default 0) | `0xc1cc5c`/`0xc1cc60` |
| `+0x34` | u32 | — (not written) | |

[^inst]: The same value is `AVE_IPC::Init`'s third argument (`ldr w3,[x19,#72]`,
    `0xc1d51c`), range-checked `<= 3` at `0xfffffe0008c42670`, and is the index
    passed to `AVE_Surface::GetDARTAddr(index, 0)` everywhere. *Inferred:* it is
    the ADT `sve-id` property — `ave1` has `sve-id = 1` and `ave0` has none, and
    the firmware has an `AVE_DevInfo::GetSVEID` (fw `0x22070`). Not traced to the
    ADT read, so treat 0/1 as inference.

`AVE_FwLog::GetInfo(u64*, int*)` (`0xfffffe0008b40a20`) returns
`AVE_Surface::GetDARTAddr` and `GetSize` (`0xfffffe0008b40ac8`/`0xfffffe0008b40acc`,
`0xfffffe0008b40ad4`/`0xfffffe0008b40adc`) — a **DART address**, surface config
index **29** (`mov w0, #0x1d`, `0xfffffe0008b406b4`).

### Firmware side — `CPlatformEnvironment::CPlatformEnvironment` (`0xe0fec`)

This is the firmware's very first act after boot:

```
w21 = 0x08042006                       ; 0xe1020/0xe102c
w0  = GPIO::Read(0)                    ; 0xe1150
this->standalone = (w0 != 0x08042006)  ; 0xe115c..0xe1164
if (this->standalone) { ... nothing to read ... }   ; 0xe1170 tbz
else {
    hi = GPIO::Read(2);                ; 0xe11b8
    lo = GPIO::Read(1);                ; 0xe11e4
    map(sp+0x70, lo | hi<<32, 56, 0);  ; 0xe11f0..0xe11fc  <- 0x38 == 56
    p = ptr(sp+0x70);                  ; 0xe1204
    devIndex        = LE32(p+0);       ; 0xe1218..0xe1244
    devID           = LE32(p+4);       ; 0xe124c
    devNum          = LE32(p+8);       ; 0xe128c
    devNumPerGroup  = LE32(p+12);      ; 0xe1294
    devSubIDFlag    = LE64(p+16);      ; 0xe12f8
    devRevision     = LE32(p+24);      ; 0xe1300
    logAddr         = LE64(p+32);      ; 0xe1344
    logSize         = LE32(p+40);      ; 0xe1368
    SetLogBuffer(logAddr, logSize);    ; 0xe1378..0xe1380 -> 0xab8c
    SetIdentity(devIndex, devID, devNum, devNumPerGroup, devSubIDFlag, devRevision);  ; 0xe13a4
}
```

Both the 56-byte size and every field offset match `MakeFwCfg` exactly. The
firmware reads the struct byte-at-a-time (unaligned-safe), so no alignment
requirement on the struct beyond whatever the mapping needs.

**This is the strongest available explanation for docs/31's symptom.** Our
driver starts the core with all eight scratch registers zero. The firmware
reads `scratch0 != 0x08042006`, sets `standalone = 1`, and skips the entire
host handshake — no config, no log buffer, no identity, and it never sends
message 1. `CPU_STATUS` moving `0x2a → 0x2c` and then silence is exactly what
that path looks like from outside.

`SetLogBuffer` (`0xab8c`) starts `cbz x0, <skip>` (`0xaba0`), so **a zero log
address and size is handled**; the driver can defer the log surface.

---

## 5. Message 1: firmware → host, the channel offer — confirmed (both)

Host: `RecvIOPMsg(&w0, &w1, &w2, &w3)` at `0xc1dfe4`, results loaded at
`0xc1e0c8`/`0xc1e0cc`.

| scratch | meaning | host validation | firmware source |
|---|---|---|---|
| 0 | **channel count** | `count - 1 <= 2`, i.e. **1..3**, else `-1015` (`0xc1e1f0`, `0xc1e43c`) | `Write(0, …)` `0xe4e1c` |
| 1 | **channel-descriptor memory size**, bytes | must be `> 0` (`0xc1e1fc`) | `Write(1, w19)` `0xe4e44` |
| 2 | **protocol version — must be `0x100`** | `cmp w22,#0x100` `0xc1e6d0` (arch 0x40) and `0xc1e770` (arch 0x20) | `Write(2, #0x100)` literal at `0xe4e50` |
| 3 | **firmware heap size**, bytes; 0 = none | if non-zero → `CreateFwHeap(size)` (`0xc1e778`, `0xc1e784`) | `Write(3, w26)` `0xe4e94` |

`0x100` is a literal `mov w2, #0x100` in the firmware — this is a hard
constant, not an inference.

`AVE_HwC::CreateFwHeap(int size)` (`0xfffffe0008c1c7b4`) rounds `size` up to the
kernel page size, adds `0x3fff` and masks with `0xffffc000`
(`0xc1c7e8`..`0xc1c818`) — i.e. **16 KB granularity** — and creates surface
config index **30** (`mov w0, #0x1e`, `0xc1c7f0`), stored at `AVE_HwC+0xA8`.

---

## 6. Between messages 1 and 2: the time base — confirmed (both)

If scratch1 > 0 the host computes a timestamp correlation and publishes it in
scratch 4 and 5, **before** the reply:

```c
t = AVE_IOP::GetCurrTime();            // 0xc1e208, stored at AVE_HwC+0x128
HwC[0x128] = host_abs_time() - t;      // 0xc1e214..0xc1e220
WriteScratch(4, (u32)HwC[0x128]);      // 0xc1e52c/0xc1e534
WriteScratch(5, HwC[0x12C]);           // 0xc1e5c4/0xc1e5cc
```

The firmware reads them back in the same exchange: `Read(5)` at `0xe51c8`,
`Read(4)` at `0xe51f4`, recombined `w4 | w5<<32` at `0xe51fc` and passed to
`0x5570c`. *Inferred:* it is a host↔IOP monotonic-clock offset used for
timestamping. Not obviously required for correctness; unknown what breaks if it
is wrong.

---

## 7. Message 2: host → firmware, the shared surface — confirmed (both)

```c
AVE_IPC::GetInfo(&iova, &size);              // 0xc1e6e4 -> DART addr + size of FwIPC
SendIOPMsg((u32)iova, iova >> 32, size, 0);  // 0xc1eb7c..0xc1eb88
```

Argument order read at `0xc1eb7c` (`ldp w3,w1,[sp,#148]` → w3 = size, w1 = lo)
and `0xc1eb80` (w2 = hi); w4 = 0.

Firmware: `Read(0)` `0xe4f60`, `Read(1)` `0xe4f8c`, `Read(2)` `0xe5180`, then

- `iova_lo & 0x3fff` must be 0 — `tst x20, #0x3fff` at `0xe4f94`
- `size != 0` — `cbz w0` at `0xe5184`
- `size & 0x3fff` must be 0 — `and x8, x28, #0x3fff; cbnz` at `0xe518c`/`0xe5190`

**So the `FwIPC` IOVA and its size must both be 16 KB aligned.** Confirmed by
the firmware's own validation, which is authority class 2. `0x1400000` (20 MiB)
satisfies the size condition; the IOVA is on us.

The firmware then maps it (`0xe521c`, with the mapped base written to
`obj+0x218` and a second out-value to `obj+0x208`).

---

## 8. Message 3: firmware → host, the firmware base — confirmed (both)

```c
RecvIOPMsg(&lo, &hi, 0, 0);                  // 0xc1ec08..0xc1ec1c
fw_base = lo | ((u64)hi << 32);              // 0xc1ecfc
AVE_IPC::UpdateFwBaseAddr(fw_base);          // 0xc1ee34
```

Firmware: `Write(0, [obj+0x218])` `0xe5420`, `Write(1, [obj+0x218+4])`
`0xe5448`, then raise the host interrupt `0xe546c`.

### What `UpdateFwBaseAddr` is, and what it is not (question 4)

They are two completely different things and neither is the other.

| | `bank1 + 0x50000` | `AVE_IPC::UpdateFwBaseAddr` |
|---|---|---|
| what | ASC/IOP register written by `AVE_IOP::Config` before Start | a plain `str x1,[x0,#56]` — `AVE_IPC+0x38` (`0xfffffe0008c45eac`) |
| holds | where the **coprocessor fetches its Mach-O image** | where the **firmware mapped the `FwIPC` surface in its own address space** |
| direction | host → hardware, before start | firmware → host, after start |
| units | IOVA of the loaded image (we use 0) | firmware-private address, opaque to us |
| used for | instruction fetch | `Kernel2FwAddr` / `Fw2KernelAddr` arithmetic only |

On arch `0x20` the host computes `AVE_IPC+0x38` itself in `AVE_IPC::Init`
(`cmp w0,#0x20` at `0xfffffe0008c42b44`, then
`GetDARTAddr(surface) - AVE_FwImg::GetBaseAddr()`). On **arch `0x40` — which is
both M1 Max instances — that branch is not taken** and `+0x38` stays 0 until
this message arrives. docs/08 §8 is confirmed. Concretely:

```
Kernel2FwAddr(k) = (k - surfKernelBase) + fw_base
Fw2KernelAddr(f) = (f - fw_base) + surfKernelBase
```

A Linux driver has no "kernel base" distinct from its own CPU mapping, so the
two useful forms are:

```
fw_addr  = (cpu_va - fwipc_cpu_base) + fw_base
iova     = (cpu_va - fwipc_cpu_base) + fwipc_iova_base
```

`fw_base` is **not** the IOVA and must not be assumed equal to it.

---

## 9. Message 4: host → firmware, the IPC info block — confirmed (both)

Between messages 3 and 4 the host allocates:

```c
AVE_IPC::AllocChannelMem(w1_from_msg1, &chanMem);  // 0xc1ee44/0xc1ee48
bzero(chanMem, w1);                                // 0xc1effc
AVE_IPC::Alloc(0x50, &ipcinfo);                    // 0xc1f008/0xc1f00c   80 bytes
AVE_IPC::Alloc(0x10000, &HwC[0x118]);              // 0xc1f1ec/0xc1f1f0   64 KiB
HwC[0x120] = 0x10000;  bzero(HwC[0x118], 0x10000); // 0xc1f1dc, 0xc1f3ac
memset(ipcinfo, 0, 0x50);                          // 0xc1f1c8..0xc1f1d8 (stp q0)
```

`AllocChannelMem` (`0xfffffe0008c44020`) caches its single allocation at
`AVE_IPC+0x48` and returns it on every later call.

### The 80-byte IPC info block, **arch `0x40` layout**

Selected by `GetDevArch() == 0x40` at `0xc1f3bc`/`0xc1f3c0`.

| off | size | value | read from |
|---|---|---|---|
| `+0x00` | u64 | 0 (never written on this arch) | — |
| `+0x08` | u64 | `Kernel2FwAddr(chanMem)` — **firmware address of the channel-descriptor memory** | `0xc1f3cc`/`0xc1f3d0` |
| `+0x10` | u64 | `Kernel2FwAddr(logbuf)` (the 64 KiB block) | `0xc1f404`/`0xc1f408` |
| `+0x18` | u32 | `0x10000` | `0xc1f40c`/`0xc1f410` |
| `+0x1C` | u64 (unaligned `stur`) | `AVE_Surface::GetDARTAddr(fwHeap)` | `0xc1f3e4`/`0xc1f3e8` |
| `+0x24` | u32 | `AVE_Surface::GetSize(fwHeap)` | `0xc1f3f0`/`0xc1f3f4` |
| `+0x28` | u32 | `AVE_DevInfo::GetDevType()` (9 / 10) | `0xc1f418`/`0xc1f41c` |
| `+0x2C`.. | | zero | |

The heap fields are written only if `AVE_HwC+0xA8 != 0` (`0xc1f3d8`), i.e. only
if message 1 asked for a heap.

Then:

```c
f = Kernel2FwAddr(ipcinfo);
SendIOPMsg((u32)f, f >> 32, 0, 0);      // 0xc1f760..0xc1f76c
```

Firmware: `Read(0)` `0xe5494`, `Read(1)` `0xe54c0`, recombine `0xe54c8`, then
`ldp w9,w8,[x22,#28]` (`0xe54cc`) → the heap address at `+0x1C`, checked
16 KB-aligned (`tst x0,#0x3fff`, `0xe54d4`); `ldr w1,[x22,#36]` (`0xe5570`) →
heap size at `+0x24`, also checked 16 KB-aligned (`0xe557c`); and
`ldp w28,w24,[x22,#8]` (`0xe5574`) → the channel memory address at `+0x08`.
Field offsets match the host exactly.

The arch-`0x20` layout is different (all 32-bit, base address at `+0x00`,
`0xc1f4a8`..`0xc1f50c`) and does not apply to M1 Pro/Max.

---

## 10. Message 5 and `CreateChannel` — confirmed (both)

```c
RecvIOPMsg(&d0, &d1, &d2, 0);                       // 0xc1f804..0xc1f818
descFw = *(u64 *)&d0;                               // ldur x20,[x29,#-112], 0xc1f8d8
gClientBufferSize = d2;                             // 0xc1fca0/0xc1fca8
if (d2 > 0x13C000) fail;                            // cmp w8,#0x13c,lsl #12, 0xc1fcac
descKVA = Fw2KernelAddr(descFw);                    // 0xc1fa0c
if (descKVA != chanMem) fail;                       // 0xc1fa14..0xc1fa1c
AVE_IPC::CreateChannel(nChannels, version, descKVA); // 0xc1fa30
```

`nChannels` and `version` are **scratch0 and scratch2 of message 1**, carried in
`x21`/`x22` since `0xc1e0c8`; every intervening write to those registers is
inside a logging block on an error path.

Firmware: the descriptor array address is produced at `0xe5988`, the channels
are built by `RealChannelCreate` (`0xe43d0`) at `0xe59a8`, then
`Write(0, lo)` `0xe59d8`, `Write(1, hi)` `0xe5a00`,
`Write(2, GetClientBufferSize())` `0xe5a30` (`GetClientBufferSize` = fw
`0x3d370`), then raise the host interrupt `0xe5a54`.

**So the host learns the descriptor array's address in message 5, and it is
required to be the block the host itself allocated in `AllocChannelMem`** — the
firmware writes the descriptors *into host memory* and reports back where. That
answers question 3's "how does the host learn that array's address": it does not
discover a firmware-owned array; it hands the firmware a buffer (message 4,
`+0x08`) and the firmware returns a pointer into it.

### `AVE_IPC::CreateChannel(int nChannels, int version, unsigned long pDescArray)` — `0xfffffe0008c440a0`

Guards: `nChannels != 0` (`0xc44198`), `version >= 1` (`0xc4419c`),
`pDescArray != NULL` (`0xc441a4`), `GetDevArch() == 0x40` selects the 64-bit
path (`0xc441b8`/`0xc441c0`).

Per descriptor `d`, stride **`0x100`** (`add x24,x24,#0x100`, `0xc44350`):

```c
slotsKVA = Fw2KernelAddr(*(u64 *)(d + 0x4C));   // ldur x1,[x24,#76] @0xc441e0, call @0xc441e8
if (!slotsKVA) fail;
if (AVE_IPC_ChName2ID((char *)d, &chId) != 0) skip;   // 0xc441fc; strcmp vs {"", "IO", "IO_T2H"}
this->descIndex[chId] = i;                      // this+0xC8, 0xc44220
this->doorbellBit[chId] = *(u32 *)(d + 0x44);   // this+0xD4, 0xc44224/0xc4423c
AppleAVEIOProcessorChannel::Create(&this->chan[i], ...);  // consumes d+0x40, d+0x44, d+0x48
```

Descriptor fields, unchanged from docs/08 §3 and re-verified here:

| off | meaning | read from |
|---|---|---|
| `+0x00` | NUL-terminated name (`""` / `"IO"` / `"IO_T2H"`) | `0xc441f8` |
| `+0x40` | direction code → ring type | `0xfffffe0008b4f7f0` |
| `+0x44` | doorbell / interrupt bit index | `0xc44224`, `0xfffffe0008b4f7e8` |
| `+0x48` | slot count | `0xfffffe0008b4f808` |
| `+0x4C` | firmware address of the slot array, **64-bit** on arch `0x40` | `0xc441e0` |

Ring mechanics (slot size `0x40`, phase bit, host-private indices) are unchanged
— see docs/08 §4.

---

## 11. The ready flag — confirmed (both)

```c
SetIOPFlag(3);                                   // 0xc1fcb8/0xc1fcbc -> scratch3 = 0x08042006
n = 0;
while (CheckIOPFlag(3) != 0) {                   // 0xc1fcc8, 0xc1fdac
    if (n > cfg->timeout_scale * 20000) fail;    // 0xc1fd8c..0xc1fd94 (w22 = 20000 @0xc1fcec)
    n++;
    IODelay(100);                                // 0xc1fd9c/0xc1fda0
}
return 0;                                        // 0xc1fdb4
```

Default timeout `1 × 20000 × 100 µs` = **2 s**.

The firmware clears it: the `CFlowControllerBase` constructor does `Write(3, 0)`
(`mov w2, wzr; mov w1, #3` at `0x24afc`/`0x24b00`, indirect call at `0x24b20`)
and `Write(7, 1)` (`0x24b38`..`0x24b58`). Scratch 7 is presumably
the heartbeat counter docs/06 records; scratch 6 is unaccounted for.

**So `StartUpIOP` completes only when the firmware's flow controller has been
constructed.** The final flag is the "encoder ready" signal.

---

## 12. Scratch register map, whole boot

| reg | addr (ave0) | before Start | msg 1 | msg 2 | msg 3 | msg 4 | msg 5 | ready |
|---|---|---|---|---|---|---|---|---|
| 0 | `…050018` | H: `0x08042006` | F: channel count | H: iova lo | F: fw_base lo | H: info lo | F: desc lo | |
| 1 | `…05001C` | H: fwcfg iova lo | F: chanmem size | H: iova hi | F: fw_base hi | H: info hi | F: desc hi | |
| 2 | `…050020` | H: fwcfg iova hi | F: `0x100` | H: size | | | F: client buf size | |
| 3 | `…050024` | | F: heap size | H: 0 | | | | H: `0x08042006`, F: 0 |
| 4 | `…050028` | | | H: tdelta lo (before msg 2) | | | | |
| 5 | `…05002C` | | | H: tdelta hi (before msg 2) | | | | |
| 6 | `…050030` | | | | | | | unknown |
| 7 | `…050034` | | | | | | | F: 1 |

H = written by host, F = written by firmware.

---

## 13. Implementable sequence

Pseudocode. `SVE` = bank 2 (`ave0`: `0x20D050000`). `ASC` = bank 1 `+0x400000`.
All scratch writes are 32-bit.

```c
#define SVE_DOORBELL   0x0C          /* host -> AVE, bit 0 = mailbox */
#define SVE_STATUS     0x10          /* AVE -> host, W1C, bit 0 = mailbox */
#define SVE_SCRATCH(i) (0x18 + 4*(i))/* i = 0..7 */
#define IOP_MAGIC      0x08042006u
#define IOP_VERSION    0x100u
#define IOP_TIMEOUT_MS 2000          /* cfg[0x18] default 1 -> 2000 x 1ms */

static int recv_iop_msg(u32 *w0, u32 *w1, u32 *w2, u32 *w3)
{
    unsigned n = 0;
    while (!(readl(sve + SVE_STATUS) & 1)) {
        if (++n > IOP_TIMEOUT_MS) return -ETIMEDOUT;   /* Apple returns -1017 */
        udelay(1000);
    }
    writel(1, sve + SVE_STATUS);                       /* W1C ack, before reading */
    if (w0) *w0 = readl(sve + SVE_SCRATCH(0));
    if (w1) *w1 = readl(sve + SVE_SCRATCH(1));
    if (w2) *w2 = readl(sve + SVE_SCRATCH(2));
    if (w3) *w3 = readl(sve + SVE_SCRATCH(3));
    return 0;
}

static void send_iop_msg(u32 a, u32 b, u32 c, u32 d)
{
    writel(a, sve + SVE_SCRATCH(0));
    writel(b, sve + SVE_SCRATCH(1));
    writel(c, sve + SVE_SCRATCH(2));
    writel(d, sve + SVE_SCRATCH(3));
    wmb();
    writel(1, sve + SVE_DOORBELL);                     /* bit 0 */
}

int ave_boot(void)
{
    u32 nch, chanmem_sz, ver, heap_sz, lo, hi, sz, cbsz;
    u64 fw_base, desc_fw;

    /* --- 0. everything docs/09 and docs/22 already cover --------------- */
    ave_power_up();
    ave_load_firmware_at_iova0();
    /* FwIPC: 0x1400000 bytes, IOVA and size both 16 KiB aligned (fw checks) */
    fwipc = ave_alloc_fwipc(0x1400000);

    /* --- 1. boot config block, BEFORE Config/Start --------------------- */
    struct ave_fw_cfg *c = fwipc_alloc(0x38);          /* 64-byte-granule pool */
    memset(c, 0, 0x38);
    c->dev_index         = inst;        /* AVE_HwC+0x48; *inferred* 0 / 1     */
    c->dev_id            = 11 + inst;   /* confirmed: ave0 11, ave1 12        */
    c->dev_num           = 1 + inst;    /* confirmed: ave0 1, ave1 2          */
    c->dev_num_per_group = <GetDevNumPerGroup>;        /* see docs/08 §7 table */
    c->dev_subid_flag    = <GetDevSubIDFlag>;
    c->dev_revision      = <GetDevRevision>;
    c->log_addr          = 0;           /* fw handles 0 (fw 0xaba0); a DART   */
    c->log_size          = 0;           /* address of a log surface if wanted */
    c->cfg30             = 0;

    u64 cfg_iova = fwipc_iova_of(c);
    writel(IOP_MAGIC,          sve + SVE_SCRATCH(0));
    writel((u32)cfg_iova,      sve + SVE_SCRATCH(1));
    writel(cfg_iova >> 32,     sve + SVE_SCRATCH(2));
    wmb();

    /* --- 2. start the core (docs/09 §; unchanged) ---------------------- */
    ave_iop_config();                   /* incl. Write64(bank1, 0x50000, base|tag) */
    ave_iop_start();                    /* 0x400808<-1, 0x400044<-0,
                                           0x400400<-0x10000, 0x400044<-0x10 */

    /* --- 3. msg 1: the firmware's offer -------------------------------- */
    if (recv_iop_msg(&nch, &chanmem_sz, &ver, &heap_sz)) return -ETIMEDOUT;
    if (nch < 1 || nch > 3)      return -EPROTO;       /* Apple: -1015 */
    if ((int)chanmem_sz <= 0)    return -EPROTO;       /* Apple: -1015 */
    if (ver != IOP_VERSION)      return -EPROTO;
    if (heap_sz) fw_heap = ave_alloc_surface(ALIGN(heap_sz, 0x4000));

    /* --- 4. time base, then msg 2: hand over FwIPC --------------------- */
    writel((u32)tdelta,      sve + SVE_SCRATCH(4));
    writel(tdelta >> 32,     sve + SVE_SCRATCH(5));
    send_iop_msg((u32)fwipc_iova, fwipc_iova >> 32, fwipc_size, 0);

    /* --- 5. msg 3: the firmware's view of FwIPC ------------------------ */
    if (recv_iop_msg(&lo, &hi, NULL, NULL)) return -ETIMEDOUT;
    fw_base = lo | ((u64)hi << 32);     /* Kernel2FwAddr/Fw2KernelAddr base   */

    /* --- 6. msg 4: the IPC info block ---------------------------------- */
    void *chanmem = fwipc_alloc(chanmem_sz);   memset(chanmem, 0, chanmem_sz);
    void *logbuf  = fwipc_alloc(0x10000);      memset(logbuf,  0, 0x10000);
    struct ave_ipc_info *ii = fwipc_alloc(0x50);  memset(ii, 0, 0x50);
    ii->chanmem_fw = FW_ADDR(chanmem);                 /* +0x08 */
    ii->log_fw     = FW_ADDR(logbuf);                  /* +0x10 */
    ii->log_size   = 0x10000;                          /* +0x18 */
    if (heap_sz) {
        put_unaligned_le64(fw_heap_iova, (u8 *)ii + 0x1C);  /* 16 KiB aligned */
        put_unaligned_le32(fw_heap_size, (u8 *)ii + 0x24);  /* 16 KiB aligned */
    }
    ii->dev_type = 9 + inst;                           /* +0x28: ave0 9, ave1 10 */
    u64 iif = FW_ADDR(ii);
    send_iop_msg((u32)iif, iif >> 32, 0, 0);

    /* --- 7. msg 5: the channel descriptors ----------------------------- */
    if (recv_iop_msg(&lo, &hi, &cbsz, NULL)) return -ETIMEDOUT;
    desc_fw = lo | ((u64)hi << 32);
    if (cbsz > 0x13C000)                       return -EPROTO;
    if (FW_TO_CPU(desc_fw) != (uintptr_t)chanmem) return -EPROTO;
    client_buffer_size = cbsz;

    /* --- 8. bind the channels ------------------------------------------ */
    for (i = 0; i < nch; i++) {
        u8 *d = (u8 *)chanmem + i * 0x100;
        int id = name_to_id((char *)d);        /* "" -> 0, "IO" -> 1, "IO_T2H" -> 2 */
        if (id < 0) continue;
        ch[id].slots      = FW_TO_CPU(get_unaligned_le64(d + 0x4C));
        ch[id].type       = dir_to_type(le32(d + 0x40));   /* 0->1, 1->0, >=2->2 */
        ch[id].bit        = le32(d + 0x44);
        ch[id].nslots     = le32(d + 0x48);
        ioproc_channel_init(&ch[id]);          /* docs/08 §4 */
    }

    /* --- 9. wait for "encoder ready" ----------------------------------- */
    writel(IOP_MAGIC, sve + SVE_SCRATCH(3));
    for (n = 0; ; n++) {
        u32 v = readl(sve + SVE_SCRATCH(3));
        if (v == 0) break;                     /* fw's CFlowControllerBase ctor */
        if (n > 20000) return -ETIMEDOUT;
        udelay(100);
    }
    return 0;                                  /* now enable the AIC irq */
}
```

### What "the firmware is alive" looks like

The first observable is **`bank2 + 0x10` bit 0 becoming 1** within ~2 s of
`CPU_CONTROL <- 0x10`, with `bank2 + 0x20` reading **`0x100`**. `0x100` in
scratch 2 is the single best liveness check: it is a literal in the firmware
(`0xe4e50`) and cannot be a leftover.

If bit 0 never sets, read the eight scratch registers. All zero means the
firmware never ran or never reached `CPlatformEnvironment`. Scratch 0 still
reading `0x08042006` unchanged after several seconds means the core is not
executing at all (nothing overwrote it) — as distinct from the current
observation, where scratch 0 was *zero* because we never wrote the magic.

---

## 14. Constraints a driver must satisfy

Each is enforced by the firmware's own code, which is authority class 2.

| constraint | enforced at |
|---|---|
| `FwIPC` IOVA 16 KiB aligned | fw `0xe4f94` |
| `FwIPC` size non-zero and 16 KiB aligned | fw `0xe5184`, `0xe518c` |
| firmware heap DART address 16 KiB aligned | fw `0xe54d4` |
| firmware heap size 16 KiB aligned | fw `0xe557c` |
| boot config block is exactly 56 bytes at the IOVA in scratch 1:2 | fw `0xe11f8` |
| scratch 0 must read `0x08042006` at core start, or the whole handshake is skipped | fw `0xe115c`..`0xe1170` |
| protocol version must be `0x100` | host `0xc1e6d0`; fw literal `0xe4e50` |
| channel count 1..3 | host `0xc1e1f0` |
| client buffer size ≤ `0x13C000` | host `0xc1fcac` |
| descriptor array must land inside the host's own `AllocChannelMem` block | host `0xc1fa14` |

### Two hazards

**The interrupt handler will eat the mailbox.** `AVE_HwC::ProcessIntr` reads the
status word and immediately calls `ClearIntr(status)` with the *whole* word
(`0xfffffe0008c19928`) before dispatching channels from `ch = 1`
(`0xfffffe0008c1992c`). If the AIC irq is live during the handshake it will W1C
bit 0 out from under a concurrent `RecvIOPMsg`, which will then spin to its
2 s timeout. **Inferred, not confirmed:** Apple must therefore run `StartUpIOP`
with the interrupt not yet enabled, or on a workloop that serialises the two.
Nothing in the disassembly states this, and no masking write was found. The
safe implementation is to request the irq only after step 9.

**`RecvIOPMsg` acks before it reads.** The W1C at `0xfffffe0008c91754` precedes
the four scratch reads. That is only safe because the firmware never writes the
scratch registers again until the host has replied — which the firmware's own
"raise, then wait for the host doorbell" structure (`0xe5dd8`) guarantees. Do
not reorder.

---

## 15. Still unknown

- **Scratch 6.** Nothing writes or reads it in either binary on any path
  examined here.
- **`_S_AVE_Fw_Cfg + 0x30`**, i.e. `AVE_Cfg_Get()[+0x30]`. Zero by default
  (`AVE_Cfg_Default` zeroes 384 bytes and sets only `+0x10`, `+0x11`, `+0x14`,
  `+0x18`, `+0x1C`), so writing 0 matches an un-boot-arg'd macOS. What it means
  is not known.
- **`GetDevNumPerGroup`, `GetDevSubIDFlag`, `GetDevRevision`** for t6001. They
  come from the 0x48-byte device table at `0xfffffe0007edba00` (docs/08 §7
  decoded `devType`, `chipType`, `arch`, `devNum` from it but not these three).
  A decode of the two table rows would close this; until then a driver should
  read them out of the table rather than guess.
- **The time-base delta's units and consequence.** The host computes
  `host_abs_time() - AVE_IOP::GetCurrTime()`; whether a wrong value degrades
  timestamps or breaks something harder is not established.
- **Whether the DPM calls matter.** Unchanged from docs/31 — still the only
  `StartUpIOP` step we skip outright, and still unproven either way.
- **Whether `bank2+0x0C` is set-only or read-modify-write.** The kext only ever
  writes a single-bit mask. Unchanged from docs/08 §6.
- **What the firmware does in standalone mode** (`scratch0 != 0x08042006`). It
  clearly proceeds — `CPlatformEnvironment` continues past the branch at
  `0xe1170` with zeroed identity — but that path was not followed, so "the core
  is up but idle" is a plausible reading of our current hardware state and not a
  proven one.

## 16. Proposed experiments (do not run — for the operator)

Ordered by information per reboot. Each is read-mostly plus scratch-register
writes to `bank2`, a block we already write (`SVE+0x38`) without incident.

1. **The one that matters.** Reboot. Before `Config`/`Start`, write
   `0x08042006` to `SVE+0x18` and the IOVA of a 56-byte block to `SVE+0x1C` /
   `SVE+0x20`. Start. Then poll `SVE+0x10` for 3 s and dump all eight scratch
   registers. Success looks like `SVE+0x10` bit 0 set and `SVE+0x20 == 0x100`.
   This is the whole hypothesis of §4 in one boot.
2. If (1) times out: repeat with the log surface populated
   (`_S_AVE_Fw_Cfg+0x20/+0x28` pointing at a DART-mapped page) and dump that
   page. The firmware's own log is the best oracle available and costs one
   allocation.
3. **Free with either of the above:** read `SVE+0x10` and all eight scratch
   registers *before* any write, on a fresh boot. That settles whether iBoot or
   a previous run left the mailbox in a state, which docs/31's addendum shows is
   a live concern for `ASC+0x50000`.

None of these requires a new DART, a new device binding, or a write outside
bank 2 — the failure mode of [24-incident-2026-09-07.md](24-incident-2026-09-07.md)
and [30-address-translation-bug.md](30-address-translation-bug.md). But the
addresses must still be checked against `/proc/device-tree` translations before
the run, per trap 7.
