# Who owns `ASC+0x50000`, and how the AVE firmware is really delivered

Static answer to the question raised by the 2026-09-08 hardware runs in
[31](31-bringup-state.md): the firmware-base register at bank 1 `+0x50000`
already holds a value on a cold boot and ignores our writes.

Everything below is marked **confirmed** (read out of an instruction or a live
read-only file, VA/path cited), **inferred** (chain stated), or **unknown**.

Reproduce the kext lines with:

```sh
python3 tools/disas.py --kext --addr 0xfffffe0008c4073c -n 0x300
```

The wide scans use a full disassembly of AppleAVE2's `__TEXT_EXEC`
(kernelcache file offset `0x1b30bb0`, length `0x1a059c`, VA base
`0xfffffe0008b34bb0`):

```sh
python3 -c "d=open('data/blobs/kc.macho','rb').read(); open('/tmp/t.bin','wb').write(d[0x1b30bb0:0x1b30bb0+0x1a059c])"
objdump -D -b binary -m aarch64 --adjust-vma=0xfffffe0008b34bb0 /tmp/t.bin > /tmp/ave.asm
```

---

## 0. Summary

| Question | Answer | Confidence |
|---|---|---|
| Does AppleAVE2 write `bank1+0x50000`? | **Not on M1 Max.** The only writer is `AVE_IOP::Config`, and it returns early on this SoC | Confirmed |
| Does anything else in the kernel write it? | **No.** Zero sites outside AppleAVE2 in the whole 118 MB kernelcache | Confirmed |
| Is there a lock/enable/unlock register or sequence? | **Not in Apple's driver.** No companion register exists anywhere in the kext | Confirmed |
| Did Apple's *code* set bit 0 of the live value? | **No** — the kext's write formula cannot produce bit 0 | Confirmed |
| Who put the value there? | iBoot (or the hardware) | Inferred |
| How is the image delivered? | iBoot loads it into a DRAM carve-out below the OS memory map and publishes `pre-loaded` + `segment-ranges` on the live ADT | Confirmed for the mechanism, inferred for AVE specifically |
| Does the kext ever load an image? | **No** — dead code, established in [09](09-firmware-load.md) §1.6, re-checked here | Confirmed |
| Is there a re-load path? | Yes, but it re-copies the **DATA segment in place** and never re-points the base | Confirmed |
| Is the register writable? | **Unknown.** Nothing in Apple's software writes it on this part, so there is no reference sequence to imitate | — |

**The short version: the "tell the ASC where the firmware is" step does not
exist on Apple for this SoC. We invented it out of a code path Apple disables.**

---

## 1. The census: every reader and writer of `0x50000`

This is question 1, and [trap 3](00-methodology.md) (absence of a call site is
not absence of use) applies hardest here, so the search was done three ways.

### 1.1 By immediate

`grep 0x50000` over the full `__TEXT_EXEC` disassembly gives **exactly 40
hits**, all `mov w2, #0x50000` (the offset argument). Mapping each to its
enclosing symbol: they are two per `AVE_IOP_Config_<variant>` function, at
`+0xec` (a `Read64`) and `+0x17c` (a `Write64`), for all 20 non-Rhea variants
— `Ersa, Upis, Ares, Leto, Acis, Atlas, Castor, Gaia, Hera, Hypnos, Janus,
Nemesis, Nyx, Pan, Panda, Tethys, Themis, Uranus, Erebus, Aion`. Nothing else
in the kext names the offset. **Confirmed.**

### 1.2 By callee, which defeats table-driven offsets

`AVE_Reg::Write64` (`__ZN7AVE_Reg7Write64E14_E_AVE_RegTypeiy`,
`0xfffffe0008c53e88`) has **20 `bl` sites in the entire kext**, and all 20 are
the `AVE_IOP_Config_*` write with `w1 = 1` (bank) and `w2 = 0x50000`
(immediate). There is **no 64-bit register write anywhere else in AppleAVE2**,
table-driven or not.

`AVE_Reg::Read64` (`0xfffffe0008c53e24`) has 21 sites: the same 20, plus one
in `AVE_IOP_GetCurrTime64` at `0xfffffe0008c40118` whose offset comes from a
parameter.

A full census of `AVE_Reg::Write32` (`0xfffffe0008c53e58`, 111 sites) and
`Read32` (`0xfffffe0008c53df0`, 58 sites) resolving both the `movz`/`movk`
pairs and the register-computed offsets gives the complete bank-1 map the kext
uses:

| bank 1 offset | access | sites |
|---|---|---|
| `0x8`, `0x10`, `0x18`, `0x20`, `0x28`, `0x30`, `0x38` | W32 (R32 on `0x8`) | 1 each |
| `0x44` / `0x400044` / `0x600044` | W32 | 2 / 22 / 18 — `CPU_CONTROL` |
| `0x48` / `0x400048` / `0x600048` | R32 | 1 / 11 / 9 — `CPU_STATUS` |
| `0x808`, `0x80c` | W32 / W32+R32 | 1 / 2+1 |
| `0x400044 + 0x7c4` = `0x400808` | W32 | 20 (`AVE_IOP_Start_*`) |
| `0x400044 + 0x3bc` = `0x400400` | W32 | 20 (`AVE_IOP_Start_*`) |
| **`0x50000`** | **R64 + W64** | **20 + 20** |

**There is nothing else anywhere in the `0x5xxxx` range** — no `0x50004`, no
`0x50008`, no companion, no status mirror, no lock register. **Confirmed.**

### 1.3 By constant, across the whole kernel

Scanning all `0x3748000` bytes of the kernelcache `__TEXT_EXEC` for the
encoding `movz Xd, #0x102, lsl #48` (`w & 0xFFFFFFE0 == 0xD2E02040`, the
`AVE_ASC_FW_BASE_TAG`) finds **18 hits, every one inside AppleAVE2**, and zero
anywhere else in the 118 MB image. So no RTKit host framework, no
`AppleIOPFamily`-style layer, and no other kext forms this value. **Confirmed.**

(18 not 20 because Leto and Hypnos build the tag slightly differently; both are
still in the 40-site list of §1.1.)

### 1.4 …and on M1 Max the one writer is disabled

`AVE_IOP::Config` (`0xfffffe0008c4073c`):

```
c4080c:  bl   AVE_DevInfo::GetChipType   ; 0xfffffe0008c4080c -> w21
c40814:  ldr  x0, [x19, #48]             ; AVE_IOP+0x30 = AVE_FwImg*
c40818:  ldrb w8, [x0]                   ; m_bIBootLoaded
c4081c:  tbz  w8, #0, 0xfffffe0008c40830 ; not iBoot-loaded -> do the write
c40820:  cmp  w21, #0x3
c40824:  b.eq 0xfffffe0008c40830         ; chipType == 3 -> do the write
c40828:  mov  w20, #0x0
c4082c:  b    0xfffffe0008c409d0          ; else RETURN 0, writing nothing
```

**Confirmed**, and it decides the whole question. The variant chain:

- `AVE_DevInfo::RetrieveDevID` reads ADT `soc-id`
  ([09](09-firmware-load.md) §3.1). `data/derived/adt-ave-nodes.txt`:
  `/arm-io/ave0 soc-id = "t6000"`, `/arm-io/ave1 soc-id = "t6001"`.
- `gsc_saAVE_DevCap` at `0xfffffe0007edba00`, stride `0x48`, fields
  `{DevID, DevType, ChipType}` — dumped in full:
  **DevID 11 (`t6000`) -> DevType 9, ChipType 6**;
  **DevID 12 (`t6001`) -> DevType 10, ChipType 7**.
- `gsc_saAVE_IOP_If` at `0xfffffe0007ee07f0`, stride `0x28`, indexed by
  **`chipType - 1`** (`mov w8,#0x28; mov x9,#-40; smaddl x8,w21,w8,x9` at
  `0xfffffe0008c40838`–`0xfffffe0008c40848`), slots
  `{Config, Start, NULL, CheckIdle, GetCurrTime}`.
  ChipType 6 -> **Castor**, ChipType 7 -> **Nyx**.

> **Correction to the rest of the repository.** `ave0` — the instance we drive,
> `reg[1] = 0x20D800000` — declares `soc-id = t6000`, so the kext runs the
> **Castor** variant on it, not Nyx. Docs 09/31/34 name Nyx (which is `ave1`).
> Nothing breaks: `AVE_IOP_Config_Castor` (`0xfffffe0008c30098`) and
> `AVE_IOP_Start_Castor` (`0xfffffe0008c302d0`) are instruction-for-instruction
> identical to the Nyx pair in every constant we use. This is exactly
> [trap 4](00-methodology.md) and is recorded so the next reader resolves the
> label rather than assuming it.

`m_bIBootLoaded` is set purely by the *presence* of the `pre-loaded` property:

```
beb460:  adrp x1, ... ; add x1, x1, #0x1bb   ; "pre-loaded"  (0xfffffe00072981bb)
beb470:  blraa <provider vtable +0x2f0>      ; getProperty
beb474:  cmp  x0, #0x0
beb478:  cset w8, ne
beb47c:  strb w8, [x19]                      ; m_bIBootLoaded
```
**Confirmed** (`AVE_FwImg::RetrieveInfo`, `0xfffffe0008beb374`).

So on `ave0`: `m_bIBootLoaded = 1`, `chipType = 6 != 3` -> **`AVE_IOP::Config`
returns 0 without touching the register.**

That `m_bIBootLoaded` is 1 on a real Mac is **inferred**, with this chain:
`AVE_FwImg_FindMap` (`0xfffffe0008beb0d8`) is a stub returning NULL, so
`UpdateBufImage` can only return `-1002` ([09](09-firmware-load.md) §1.6,
re-checked); the kext therefore has no way to place firmware bytes itself; AVE
demonstrably works on macOS; therefore the pre-loaded path is the live one.
It is corroborated by the register already holding a base with the `0x0102`
tag before Linux touches anything.

### 1.5 Answer to question 1

**AppleAVE2 reads `bank1+0x50000` once, for a log line, and never writes it on
M1 Max. Nothing else in the kernel touches it. The constant in `ave_hw.h` was
derived from a code path Apple disables on this SoC.**

---

## 2. Is it locked? (question 2)

Static analysis can say what Apple's software knows, not what the silicon does.

- **There is no lock register, enable register, key or companion in the kext.**
  The §1.2 census covers every MMIO access AppleAVE2 makes through `AVE_Reg`;
  the only thing in the `0x5xxxx` range is the one 64-bit word. **Confirmed.**
- **There is no ordering requirement to imitate**, because there is no write to
  order. On the SoCs where `Config` *does* run, it runs from `StartUpIOP` at
  `0xfffffe0008c1ddfc`, immediately before `AVE_IOP::Start` at
  `0xfffffe0008c1dee8` — i.e. with the core stopped. That is the only sequencing
  fact available, and it is already what our driver does.
- **Bit 0 of the live value did not come from Apple's code.** The write formula,
  read from Castor at `0xfffffe0008c30200`–`0xfffffe0008c30208` (identical in
  Nyx at `0xfffffe0008c35558`–`0xfffffe0008c3555c`):

  ```
  c30200:  and  x8, x20, #0x3fffffff800     ; base, bits 42:11
  c30204:  mov  x9, #0x102000000000000      ; tag
  c30208:  orr  x3, x8, x9
  ```

  Bits 10:0 are masked out, so **the kext can never write bit 0**. The live
  value `0x0102010000b28001` has bit 0 set. **Confirmed** — whoever programmed
  this register was not running this code.

Two readings remain, and nothing in either binary distinguishes them:

1. Bit 0 is a hardware "valid/enabled" latch the block sets itself, and the
   register is write-once or write-locked once valid.
2. Bit 0 is part of a different, iBoot-only encoding.

**Unknown**, and no Apple software path exists to resolve it. Tests are in §7.

---

## 3. How the firmware actually gets there (question 3)

### 3.1 The mechanism, confirmed from the live machine

`/proc/device-tree/memory@10000000000/reg` on this machine:

```
0x1000218c000  size 0x7ca9a4000
```

**Linux's RAM begins at `0x1000218c000`.** Immediately below it, four
`reserved-memory` nodes stack downward, all `compatible = "apple,asc-mem"`,
all `no-map`:

| node | phys | size | `iommu-addresses` target | IOVA |
|---|---|---|---|---|
| `asc-firmware@10000c68000` | `0x10000c68000` | `0x980000` | `isp@384000000` | `0x0` |
| `asc-firmware@1000163c000` | `0x1000163c000` | `0x1c000` | `sio@39bc00000` | `0x200000` |
| `asc-firmware@10001d70000` | `0x10001d70000` | `0x324000` | `isp@384000000` | `0x980000` |
| `asc-firmware@10002094000` | `0x10002094000` | `0xf8000` | `sio@39bc00000` | `0x220000` |

(phandles resolved by walking `/proc/device-tree`; all read-only.) **Confirmed.**

Three things follow, and they settle the mechanism:

- **ISP's two segments are physically discontiguous** (`0x10000c68000+0x980000
  = 0x100015e8000`, but DATA is at `0x10001d70000`) **and IOVA-contiguous**
  (`0` and `0x980000`). The coprocessor's address space is the DART's, and the
  DART is what makes the image look like one blob. This is the same TEXT-then-
  DATA-from-IOVA-0 layout `AVE_FwImg::InitCTRRImage` builds for AVE
  ([09](09-firmware-load.md) §1.4, assert `"firmware is not mapped to address
  0"` at `0xfffffe0007298609`). Two independent sources agreeing.
- m1n1 produces these nodes in `dt_reserve_asc_firmware`
  (`m1n1-src/src/kboot.c:1781`) by reading **`segment-ranges` off the live
  ADT** (`kboot.c:1810`) — the same property, same 32-byte layout, that
  `AVE_FwImg::RetrieveInfo` parses. m1n1 calls it for `isp`, `dcp`, `dcpext*`
  and `sio` only (`kboot.c:1868, 2239, 2876`). **There is no AVE call site**,
  which is the entire reason AVE has no carve-out in our device tree.
- **`segment-ranges` really is runtime-injected.** A byte search of the restore
  DeviceTree `data/blobs/adt.bin` finds `segment-ranges` **0 times** and
  `pre-loaded` once, at file offset `0x2f564`, on `iop-smc-nub`. Neither
  property is on `ave0` there. So the absence in
  `data/derived/adt-ave-nodes.txt` is expected and proves nothing — it is a
  *restore* tree. **Confirmed.** [09](09-firmware-load.md) §1.6 is **verified,
  not refuted.**

### 3.2 Where AVE's carve-out is

`0x10000b28000` — the base field of the live register — sits `0x140000`
(1.25 MiB) below the first ISP carve-out, in the 12.4 MiB of DRAM between the
start of memory (`0x10000000000`) and `0x10000c68000` that **Linux is never
given**. `/proc/iomem` has no entry below `0x10000c68000`. **Confirmed.**

That is why doc 31's before/after checksum of that memory was stable: nothing
in Linux can allocate it. It also means we may write it safely from Linux with
`memremap()`, since it is outside `System RAM` and has no `struct page`.

### 3.3 What is actually loaded there — and what that does *not* prove

`data/blobs/iboot-ave-stub-1k.bin` (1 KB read from physical `0x10000b28200`)
was compared against our firmware image:

- **The first 11 instructions are byte-identical** to `ave_h13c.bin` at file
  offset `0x4200`, i.e. image VA `0x200` (`__TEXT` is at file `0x4000`,
  `vmaddr 0`). Over the full 1 KB only **213/1024 bytes match**, first
  difference at `+0x2c`.
- Side-by-side disassembly shows the same routine with a different register
  allocation and a different image layout: same `msr vbar_el1` from `adr x0,0`,
  same `mpidr` unpack, same `cpacr_el1 = 0x300000`, same `sctlr_el1` M-bit
  clear, same `mair_el1` load, and the same distinctive
  `adr x0, 0x423c ; ldp w0,w1,[x0] ; add x0,x0,x1,lsl #32` idiom in both.
- **This does not identify it as AVE firmware.** All nineteen
  `AppleAVE2FW_*.im4p` variants match the stub equally well (213–214/1024),
  which is precisely the [trap 2](00-methodology.md) failure mode: the test
  cannot discriminate. What it shows is *the same source family, a different
  build* — consistent with the machine's installed firmware being a different
  macOS version from the IPSW we extracted.

The reason to believe it is AVE's is structural, not byte-level: **the register
that names the address lives inside AVE's own MMIO window** (`reg[1] +
0x50000` = `0x40D850000`), and the AVE core's instruction fetches were observed
at exactly that value `+ 0x200`. That is **inferred**, on two independent legs.

### 3.4 An inconsistency that has to be recorded

Our image is `__TEXT` `0x134000` at VA 0 and `__DATA` `0x134000` at VA
`0x134000` (2.5 MiB total). The space below the ISP carve-out is only
`0x140000`. The loaded build references `adrp x0, 0x14c000` at stub offset
`0x33c`, i.e. `base + 0x14c358`, which would land inside ISP's region.

**So TEXT and DATA cannot both be physically contiguous from `0x10000b28000`.**
Combined with §3.1 (ISP's segments are physically split and joined by the
DART), that says the address in the register is a **device address the DART is
expected to translate**, which merely happens to equal a physical address
because iBoot set up an identity view for its own load.

That collides head-on with the observed 32-bit input address space of
`40d040000` (`AS 32 -> 42`), which can never translate `0x10000b28200`, and
with `InitCTRRImage`'s assert that the kext maps the image at IOVA **0**.

Both facts are solid; the reconciliation is **unknown**. The candidates:

1. Apple's DART configuration for the fetch stream is not the one Linux leaves
   behind — `apple_dart_hw_reset()` clears per-stream bypass/TCR state on probe.
   `dart-ave0` carries a `bypass-address` blob (32 bytes, only byte 30 = `0x02`)
   that nobody has decoded; doc 31's reading of it as `0x200000000` does not
   match the bytes and should not be relied on.
2. The register is consulted only by an iBoot-era boot path and is inert once
   the host has mapped the image at IOVA 0 — in which case *something else*
   supplied the `0x10000b28200` fetch address we saw, which no one has
   identified.

Do not pick one and build on it.

### 3.5 `base + 0x200` is a parking loop — in **both** images

At image VA `0`: `0x14000081` = `b 0x204` — the entry, matching doc 31's
reading. At image VA `0x200`: `0x14000000` = **`b .`**, an infinite self-loop.
The iBoot copy has the identical word there (it is byte 0 of
`iboot-ave-stub-1k.bin`). **Confirmed for both.**

`0x200` is also exactly the AArch64 `VBAR + 0x200` slot — *current EL with
SP_ELx, synchronous* — and the shim installs `vbar_el1` = image base at
`+0x234`. So `b .` at `+0x200` is the standard "hang on unexpected exception"
vector.

This matters twice:

- If the core's reset vector really is `base + 0x200`, then **even a perfect
  mapping of a perfect image parks the core immediately**, producing exactly
  the observed symptom set.
- Doc 31's liveness test — "no page of the 16 MiB changed, therefore the core
  is not executing" — **cannot distinguish a core spinning in `b .` from a core
  that never fetched.** That conclusion should be downgraded to *undetermined*.
  A tight `b .` also explains a fault storm pinned to one constant address.

---

## 4. What the kext does at startup instead (question 4)

`AVE_HwC::StartUpIOP` (`0xfffffe0008c1d354`), every call in address order,
resolved from the disassembly:

| VA | call | meaning |
|---|---|---|
| `0xc1d468` / `0xc1d474` | `AVE_DPM::SetIOP` / `SetHw` | perf floor; inert on j314c per [28](28-ave-init-order.md) |
| `0xc1d47c` | `AVE_FwImg::UpdateImage` | -> `RestoreCTRRData`: memcpy the pristine DATA snapshot back over the **physical** DATA carve-out |
| `0xc1d50c` / `0xc1d528` | `AVE_IPC` ctor / `Init` | allocates the 20 MiB `FwIPC` surface; stores `IPC+0x38 = FwIPC_IOVA − FwImg::GetBaseAddr()` (`0xfffffe0008c42b64`–`0xc42b6c`) |
| `0xc1d690` | `AVE_IPC::Alloc` | |
| `0xc1d978` | `AVE_HwC::MakeFwCfg(_S_AVE_Fw_Cfg*)` | builds the boot-config block |
| `0xc1da64` | `AVE_SVECtrl::SetIOPFlag(0)` | writes bank 2, `scratch[0] = 0x08042006` (`0xfffffe0008c91258`–`0xc9125c`) |
| `0xc1db50` | `AVE_IPC::Kernel2DARTAddr(cfg)` | cfg block -> IOVA |
| `0xc1dc7c` | `AVE_SVECtrl::WriteScratch(1, lo32(cfgIOVA))` | |
| `0xc1dd10` | `AVE_SVECtrl::WriteScratch(2, hi32(cfgIOVA))` | `lsr x2, x21, #32` at `0xc1dd08` |
| `0xc1ddfc` | `AVE_IOP::Config` | **no-op on this SoC** (§1.4) |
| `0xc1dee8` | `AVE_IOP::Start` | the four bank-1 writes |
| `0xc1dfe4` | `AVE_SVECtrl::RecvIOPMsg` | **wait for the firmware to speak first** |
| `0xc1eb88` / `0xc1ec1c` | `SendIOPMsg` / `RecvIOPMsg` | second exchange |
| `0xc1ee34` | `AVE_IPC::UpdateFwBaseAddr` | see below |
| `0xc1ee48` onward | `AllocChannelMem`, `Alloc`, `CreateChannel`, `SetIOPFlag`, `CheckIOPFlag` | channel bring-up |

`AVE_IOP_Start_Castor` (`0xfffffe0008c302d0`), the variant `ave0` uses, in
order — `w20 = 0x400044`:

```
c30394:  W32(bank1, 0x400808, 1)
c303ac:  W32(bank1, 0x400044, 0)          ; CPU_CONTROL = 0
c303c0:  W32(bank1, 0x400400, 0x10000)
c303d8:  W32(bank1, 0x400044, 0x10)       ; CPU_CONTROL = RUN
```

**Confirmed**, and identical to what `driver/ave_drv.c` already does. The
apparatus check ([trap 7](00-methodology.md)) passes.

### The direction of the base information is firmware -> host

```
c1ec1c:  bl   AVE_SVECtrl::RecvIOPMsg(&out1, &out2, 0, 0)
c1ecf4:  ldur w8, [x29, #-148]        ; out2
c1ecf8:  ldr  w9, [sp, #152]          ; out1+4
c1ecfc:  orr  x8, x9, x8, lsl #32     ; 64-bit value
c1ed00:  stur x8, [x29, #-144]
...
c1ee2c:  ldr  x0, [x19, #224]         ; AVE_HwC->m_pcIPC
c1ee30:  ldur x1, [x29, #-144]
c1ee34:  bl   AVE_IPC::UpdateFwBaseAddr
```

The log at `0xfffffe000729ef10` names it: `"<- IPC memory firmware addr 0x%lx
0x%x 0x%x"`. `AVE_IPC::UpdateFwBaseAddr` (`0xfffffe0008c45e90`) is
`str x1, [x0, #56]` plus a log line and `return 0` — **a pure host-side field
store, no MMIO** (`0xfffffe0008c45eac`). **Confirmed.**

So the host never tells the coprocessor where anything is except through
`scratch[1..2]`. The coprocessor tells the *host* where its IPC window is, and
the host adjusts its `Fw2KernelAddr`/`Kernel2FwAddr` offset accordingly.

**Answer to question 4: by the time the kext runs, iBoot has already loaded the
image, published it on the ADT, and (on this SoC) programmed whatever the base
register needs. The kext's job starts at "restore DATA, hand over a config
block, start the core, listen."**

---

## 5. The re-load path (question 5)

Callers, found by scanning the full `__TEXT_EXEC` for `bl` to each entry point:

- `AVE_Drv::StartUp` -> `AVE_HwC::StartUp` (`0xfffffe0008be38b8`)
- `AVE_HwC::ProcessReadyCmd_StartIOP` -> `AVE_HwC::StartUp`
  (`0xfffffe0008c0ed60`) — the **client-triggered restart**
- `AVE_HwC::StartUp` -> `StartUpIOP` (`0xfffffe0008c1cea0`), then
  `StartFwHeartBeatTimer`, `AVE_MCC::Enable`, `SendFwCmd_Config`
- `AVE_Drv::PowerOff` -> `AVE_HwC::ShutDown` -> `ShutDownIOP` -> `AVE_IOP::Stop`
- `AVE_Drv::PowerOn` -> `AVE_HwC::PowerOn` (`0xfffffe0008c209ac`) — whose calls
  are `AVE_DPM::PowerOn`, `AVE_DPE::Reset`, `AVE_AXI2AF::ApplyTunables`,
  `SetParity`, `AVE_DART::SetActive(true)`, `AVE_SurfaceMgr::EnableOp`.
  **It does not start the core and does not touch the firmware base.**

So the *only* re-load in the kext is `AVE_FwImg::UpdateImage` ->
`RestoreCTRRData` (`0xfffffe0008becdf4`): a `memcpy` of the pristine DATA
snapshot into the kernel mapping of the **physical DATA carve-out**
(`0xfffffe0008becebc`), taken by `AcquireCTRRData` before the core first ran.
The TEXT segment is never rewritten — that is what CTRR is for.

**There is no path, anywhere, at any privilege visible in this kext, that
re-establishes the base address.** Answer to question 5.

---

## 6. What our driver should be doing instead

1. **Stop writing `bank1+0x50000`.** It is not a step Apple performs on this
   SoC. Keep the read as a diagnostic; delete the write, or gate it behind an
   explicit experiment flag so it never runs by default. The current write is
   also the source of a false model in `ave_hw.h` — annotate the constant with
   "written only when `!pre-loaded || chipType == 3`; never on t600x".
2. **Work with iBoot's image, not ours.** The kext cannot load an image, and
   neither, effectively, can we: the fetch address is fixed by a register we
   cannot write. The image at `0x10000b28000` is the one that must run.
3. **We can still control its contents.** That memory is outside Linux's
   `/memory` node and outside `/proc/iomem`, so `memremap()` can map it and
   nothing else in the kernel can allocate it. If we ever need our own build,
   the move is to **overwrite iBoot's copy in place** rather than to repoint the
   register — and, better, to do that from **m1n1**, before Linux boots and
   before any CTRR/MCC locking applies. That sidesteps the register entirely.
4. **Reserve it properly and describe it the way m1n1 describes ISP.** The
   template is confirmed above: an `apple,asc-mem` `no-map` reserved-memory node
   per segment, `iommu-addresses = <&ave_mapper 0 IOVA 0 SIZE>` with TEXT at
   IOVA 0 and DATA immediately after, and `memory-region` on the AVE node. That
   is what `of_iommu_get_resv_regions()` turns into direct IOMMU mappings, and
   it is what apple-isp relies on. (**Inferred** for AVE; **confirmed** as the
   live shape for ISP and SIO.)
5. **Retire two conclusions.** (a) Doc 31's "the core is not executing, because
   no page changed" — a `b .` at `base+0x200` changes nothing either (§3.5).
   (b) Doc 31's `bypass-address = 0x200000000`; the property is a 32-byte blob
   whose only non-zero byte is byte 30, and it has not been decoded.
6. **Do not treat `0x0102…` as ours.** The tag is correct, but the live value's
   bit 0 proves the programming came from outside the kext, so the register's
   full field layout is only partly known.

---

## 7. Proposed experiments — for the operator, not for an agent

Per `AGENTS.md`, these are written down, not run.

1. **Identify the image (highest value, read-only).** Dump physical
   `0x10000b28000 .. 0x10000c68000` and search it for the length-prefixed
   `IOBA` tag (`41 42 4f 49`, "IOBA" byte-reversed) and `IOSZ`
   (`5a 53 4f 49`) described in [40](40-firmware-io-base.md) §2, and for the
   strings `Erebus` / `CmdProcessor`. If `IOBA` is present with a **non-zero**
   payload, that settles three things at once: it is the AVE firmware, iBoot
   filled the I/O base, and we learn the bus address iBoot chose. This is the
   test that discriminates; the 1 KB byte-compare in §3.3 does not.
2. **Find the extent.** From the same dump, locate the last non-zero page. If
   the region is ~`0x134000` and stops well before `0x10000c68000`, the DATA
   segment is elsewhere and §3.4's "device address, not physical" reading is
   confirmed.
3. **Is bit 0 clearable?** On a fresh boot, with the block powered and
   `CPU_CONTROL` known to be 0, write `0` to the whole 64-bit word and read
   back. If even bit 0 survives, the register is read-only to EL1 and the
   question is closed. If bit 0 clears but the base does not, bit 0 is a
   separate, writable enable and the base is latched while it is set — in which
   case try clear-then-write-then-set.
4. **Is it locked by power state?** Read the register with the AVE power domain
   off and on. A value that survives power-gating is held somewhere unusual and
   is more likely to be fused or secure-world state.
5. **Distinguish "parked" from "dead" (§3.5).** With the fetch satisfied by
   whatever configuration produces no faults, read `CPU_STATUS`
   (`bank1+0x400048`) repeatedly and watch the AVE cycle counter at
   `bank1+0x178000` (the `GetCurrTime64` counter, `0xfffffe0008c40118`). A
   counter that advances proves the core is clocked and running; the image
   checksum cannot.
   **Correction (2026-09-13):** that overstates the counter.
   `AVE_IOP_GetCurrTime64` divides it by a frequency register (`0x160020`)
   to get microseconds, so it is a timebase, and a timebase can tick with the
   CPU held in reset. It proves the block is *clocked*, not that the core
   *executes*, unless it is also shown to stop when `CPU_CONTROL` is 0 — which
   is the negative control the test must include.

---

## 8. What this document does not establish

- It does not show that the image at `0x10000b28000` is the AVE firmware. It
  shows the register naming it is AVE's, and that the code there is from the
  same source family as `AppleAVE2FW`. Experiment 1 is the discriminating test.
- It does not explain how a 44-bit fetch address is meant to reach memory
  through a 32-bit-input DART (§3.4). That remains the central unknown, and it
  is *not* resolved by anything in the kext.
- It says nothing about whether the handshake in [34](34-boot-handshake.md),
  the DART mapping, or the boot-config block are correct. If the core is not
  executing our instructions, none of them has been tested.

---

## Verification pass (independent re-derivation)

| Claim | Evidence | Verdict |
|---|---|---|
| The kext's one writer is gated off on this SoC | `0xc40814: ldr x0,[x19,#48]` / `ldrb w8,[x0]` / `0xc4081c: tbz w8,#0` / `cmp w21,#0x3` / `b.eq` / `mov w20,#0` / `b` — returns 0 when the iBoot-loaded flag is set and chipType != 3 | confirmed |
| The kext could not have written the live value | `0xc30200: and x8, x20, #0x3fffffff800` then `orr` with `0x102000000000000`, written to offset `0x50000` (`mov w2,#0x50000`). **The mask excludes bit 0**, which is set in the live register | confirmed |
| `+0x200` is a park loop in *both* images | our image VA `0x200` = `14000000 b 0x200`; iBoot's at `base+0x200` = the same word, and the following six instructions match byte for byte | confirmed |

The bit-0 argument is the strongest of the three and deserves restating,
because it does not depend on finding or not finding a call site — the kind of
reasoning that has misled this project before ([00](00-methodology.md) trap 3).
The kext's formula is structurally incapable of producing a value with bit 0
set. The live register has it set. Whoever programmed it was therefore not
running this code, regardless of what any call-site census says.

## Correction owed to [31](31-bringup-state.md)

`base + 0x200` is `VBAR + 0x200` — the AArch64 **synchronous exception vector
for the current EL with SP_ELx** — and it contains `b .`.

That retires the liveness test as a discriminator. A core that aborts on its
first fetch, vectors to `+0x200` and spins there executes indefinitely while
writing **nothing**, which is indistinguishable from a core that never ran at
all by the page-checksum method. The repeating fault pattern at
`+0x200`/`+0x240`/`+0x280` is exactly the shape of a prefetch-abort loop.

So the retraction in doc 31 was itself too strong. What is actually supported:

- **confirmed:** with a translating domain the core issues fetches it cannot
  satisfy, and nothing in iBoot's image is modified
- **not supported:** "the core is not executing it" — the evidence cannot
  distinguish not-executing from executing-a-park-loop

Finding a test that *can* discriminate is now an open item. Watching
`CPU_STATUS` or the ASC's own error/exception registers is more promising than
watching memory, since a parked core still has architectural state.

## Encouraging side-finding

Our image and iBoot's are byte-identical for the first instructions at
`+0x200`. So iBoot's image is very probably the same AVE firmware, and if the
fetch can be made to work it will run the right thing. Note per §7 that this
does **not** identify *which* variant — the sequence matches all nineteen
equally, so it is a trap-2 non-test for that question.
