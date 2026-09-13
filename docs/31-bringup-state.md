# Bring-up state and the remaining gap

Current as of 2026-09-08. This is the "where are we actually" document; the
per-topic docs stay as they are.

## Apple's start sequence, verified

`AVE_HwC::StartUpIOP` (`0xfffffe0008c1d354`), in address order — every entry
read from the disassembly:

| VA | call | do we? |
|---|---|---|
| `0xc1d468` | `AVE_DPM::SetIOP` | no |
| `0xc1d474` | `AVE_DPM::SetHw` | no |
| `0xc1d47c` | `AVE_FwImg::UpdateImage` | effectively yes |
| `0xc1d50c` | `AVE_IPC` constructor | n/a |
| `0xc1d528` | `AVE_IPC::Init` | partly |
| `0xc1d690` | `AVE_IPC::Alloc` | yes (`dma_alloc`) |
| `0xc1ddfc` | `AVE_IOP::Config` | **yes** |
| `0xc1dee8` | `AVE_IOP::Start` | **yes** |
| `0xc1dfe4` | `AVE_SVECtrl::RecvIOPMsg` | not yet |

Two things this settles:

- **`AVE_IPC::Init` runs before Config and Start**, and it does *not* perform a
  handshake. Its calls are `AVE_GetSurfaceCfg(FwIPC)`,
  `AVE_SurfaceMgr::CreateSurface`, `GetDARTAddr`/`GetSize`, and finally
  `AVE_FwImg::GetBaseAddr`. It allocates the 20 MiB shared region and works out
  the firmware base. Nothing is written to scratch registers.
- **Apple waits for the firmware to speak first.** The only call after
  `AVE_IOP::Start` is `RecvIOPMsg`. So the boot handshake is firmware-initiated;
  the host does not poke it awake.

`AVE_FwImg::UpdateImage` calls only `RestoreCTRRData` (restore the pristine
DATA segment) and `UpdateBufImage`, which `docs/09` established is dead code
because `AVE_FwImg_FindMap` is a stub. We copy the whole image fresh on every
load, so this is covered.

`AVE_DPM::SetIOP`/`SetHw` set performance levels. `docs/28` found
`function-set_perf_state_floor` is absent from the j314c ADT and
`AVE_PMGR::SetPerfState` bails on the null check, so the DPM ladder is inert on
M1 Max. Believed not required; not proven.

## What the hardware does

All of this is reproducible, on a machine that no longer crashes:

| step | result |
|---|---|
| map five banks | OK |
| DMA mask, IRQ, power attach, resume | OK |
| write `SVE+0x38` (Apple's first access) | OK |
| read `ASC+0x400048` | `CPU_STATUS = 0x2a` |
| DART bind | both instances init; `iommu group 16` |
| 20 MiB `dma_alloc_coherent` | `iova 0xfe000000` |
| firmware load, contiguous, `iommu_map` at IOVA 0 | OK, `0x268000` bytes |
| `Write64(bank1, 0x50000, tag)` | readback `0x0102010000b28001` |
| ASC start sequence | OK |
| **after start** | `CPU_STATUS 0x2a -> 0x2c`, scratch[0..7] all zero and static |

So: the coprocessor's state register **changes** when we start it, but nothing
else moves. Apple's own next step is to wait for a message that never arrives.

The `0x50000` readback is worth noting — we wrote `0x0102000000000000` and read
back `0x0102010000b28001`. The tag survives; the hardware populates the low
bits. That offset is a live status register, not storage.

## Why the firmware may not be running

Ranked by how well each fits the evidence. None is established.

1. **Stream ID / bypass.** The ADT gives `dart-ave0` `sids = 0x8001` (SIDs 0 and
   15) and `bypass = 0x8000` (SID 15 in bypass). We map firmware at IOVA 0 on
   SID 0. If the coprocessor fetches instructions on SID 15, then under Apple it
   is in bypass — physical addressing — and `apple_dart_hw_reset` strips bypass
   on probe, so the fetch would translate through an empty page table instead.
   This is the best fit: the core starts, faults on its first fetch, and goes
   quiet. Testable by mapping SID 15 as well, or by checking for DART faults.
2. **`AVE_DPM::SetIOP`/`SetHw`.** Believed inert on this SoC, but unproven, and
   they are the only calls in `StartUpIOP` we skip outright.
3. **Memory attributes.** `dma_alloc_attrs` gives non-cacheable coherent memory.
   An RTKit image may expect normal cacheable memory, and the DATA segment
   certainly wants to be writable-back.
4. **Something in `AVE_IOP::Config` beyond the `0x50000` write.** We implement
   the one write we found; `AVE_IOP_Config_Nyx` is larger than that.

## What is left to do generally

- Resolve the above and get the firmware to send its first message.
- Implement `RecvIOPMsg` and the channel setup that follows it.
- The command layer: `docs/07`/`20` have the ids, sizes and header; the
  per-frame `AVE_PICMGMT_PARAMS` fields are still unmapped.
- V4L2 M2M glue.

## Addendum, 2026-09-08 (later)

### The firmware-base register already holds a value, and our write is ignored

Reading `ASC+0x50000` *before* writing it:

```
BEFORE any write = 0x0102010000b28001   (base field 0x10000b28000)
we write          0x0102000000000000
readback          0x0102010000b28001    - UNCHANGED
```

Two things follow. The tag `0x0102…` was **already present**, so something set
this register before us. And **our write had no effect at all**.

The extracted base, `0x10000b28000`, sits 1.25 MiB below the first
`asc-firmware@10000c68000` reserved region and is not inside any of the four
`asc-firmware` carveouts that m1n1 emits (those belong to other coprocessors).
So it is not obviously a pointer to a region Linux knows about.

Candidate readings, none established:

- The register is **read-only, or write-locked while the core is running**.
  `AVE_IOP_Config_Nyx` clearly does `Write64(bank1, 0x50000, base|tag)`, so
  Apple writes it — but Apple writes it *before* starting the core.
- The field decomposition is wrong and `0x10000b28000` is not a base at all.

### Our recent tests are not independent

Nothing in the driver ever clears `CPU_CONTROL`, and `docs/09` established that
Apple never does either — shutdown is by sending `Halt` and polling scratch 0,
which we do not implement. **So the coprocessor stays started across module
unload and reload.**

Every stage-11 and stage-14 run since the first successful `asc-start` has
therefore been executed against an already-running core, and `CPU_STATUS = 0x2c`
is that persistent post-start state rather than a fresh result. This weakens
the recent observations: a clean test of "does the core start" now requires a
**reboot** between attempts, not just a module reload.

That also offers a simple explanation for the ignored write: Apple configures
the base while the core is stopped, and we are writing to a core that has been
running since an earlier experiment.

### The SID 15 hypothesis is weakened

`dart-isp0` has **identical** `sids = 0x8001` and `bypass = 0x8000`, and
`mapper-isp0` is `reg = 0` exactly like `mapper-ave0`. Linux drives ISP
successfully referencing SID 0 only, on three DARTs. So SID 15 sitting in
bypass is the normal arrangement for this class of block and is unlikely to be
what distinguishes AVE.

The real structural difference between the two remains firmware delivery: ISP's
image is iBoot-preloaded and m1n1 reserves it with `dt_reserve_asc_firmware`,
whereas we load ours from Linux.

### Next test, once it can be run cleanly

Reboot, then in one boot: read `ASC+0x50000` before anything else (does it hold
a base on a fresh machine?), write the base, verify the write takes, then start.
That distinguishes "write-locked while running" from "read-only".


---

## Kernel change, 2026-09-08

The machine's default boot entry is now
`7.1.13-401.asahi.vrr1.fc44.aarch64+16k`, a locally built kernel carrying the
unmerged Asahi VRR patches
([AsahiLinux/linux#477](https://github.com/AsahiLinux/linux/pull/477)), booted
with `appledrm.force_vrr=1`. Written up in the `fedora-asahi-remix-notes`
repo under `projects/promotion-vrr-kernel.md`. Three kernels are installed:
7.0.13 (safety net), 7.1.6 (what this driver was developed against) and the
new 7.1.13.

Both `driver/apple-ave.ko` and `test/ave-overlay.ko` rebuild against it with
no warnings and no API changes, and `tools/handshake-test.sh` now checks
vermagic against the running kernel and rebuilds rather than failing at
`insmod`, since a mismatch there produces a much less legible error than
saying so up front.

**One caveat on comparability.** The patches themselves are display-only and
have no bearing on AVE, but this is a 7.1.6 -> 7.1.13 jump, so `apple-dart`,
genpd and the IOMMU core have all moved under us. Earlier bring-up results in
this document were measured on 7.1.6. If something behaves differently after
the reboot, "the kernel changed" is a live hypothesis and not a lazy one -
that is the same class of mistake as
[30](30-address-translation-bug.md), where eight experiments were run
faithfully against an apparatus nobody had checked.


---

## Correction, 2026-09-08: the "core stays started" premise was wrong

This document claimed that nothing ever clears ASC `CPU_CONTROL`, that the
core had therefore been running continuously since the first `asc-start`, and
that no result was clean without a power cycle. **That is false**, and it was
contradicted by material already in the repository:

- `ave_asc_start()` writes `CPU_CONTROL = 0` as its *second* register write,
  matching the kext at `0xfffffe0008c356f4`. The true statement — which
  `ave_hw.h` and `ave_drv.c` both make correctly — is that nothing clears it
  *again after* start. Dropping that one word inverted the meaning.
- `pm_genpd_summary` shows every `venc_*` domain `off-0` while the module is
  unloaded, so genpd power-gates the block on `rmmod` regardless.
- Every stage-8 read in this boot's journal returned `CPU_STATUS = 0x2a`,
  never a persistent `0x2c` — fourteen reads, including one an hour after the
  last `asc-start`.

So a module reload *is* a clean experiment and the reboot requirement was
self-imposed. `tools/handshake-test.sh` no longer refuses a second run.

The reasoning this invalidates: the earlier reading of the ignored `0x50000`
write as "we are writing to a core that has been running since an earlier
experiment" has no support. And the real cause of the silence is now
[40](40-firmware-io-base.md) — the firmware's I/O base is zero in the image we
load, so it was never addressing the registers this document describes.

---

## Run of 2026-09-08, first with a non-zero I/O base

Kernel `7.1.13-401.asahi.vrr1`, commit `3aa5493` plus the tag-stride fix.
Logs: `results/handshake-20260908-2116*.log`.

**No hang, no crash, no reset.** Stages 1-15 all completed and the module
unloaded cleanly afterwards.

What changed and what did not:

| | |
|---|---|
| `IOBA` patch | applied: `0x0 -> 0x20c000000`, `IOSZ -> 0x2000000` |
| firmware | mapped at IOVA 0, `0x268000` bytes, phys `0x10008800000` |
| boot config | staged; scratch0 `0x08042006`, scratch1 = cfg IOVA |
| `CPU_STATUS` | `0x2a -> 0x2c` (the core starts) |
| firmware message | **none**, 2 s timeout |
| `gs_psCfg` | still 0 |
| `__rtk_crashlog_local_buffer` | still 0 |
| log ring | `rd=0 wr=0` |

So the I/O base was necessary but not sufficient. Note the crashlog buffer is
*also* zero: this is not a firmware that ran and faulted, it is a firmware
that appears not to have executed our image at all.

### The next lead: the ASC firmware-base register write is ignored

Stage 12 writes the image base to `ASC+0x50000` and reads it straight back:

```
ASC+0x50000 BEFORE any write = 0x102010000b28001  (base field 0x10000b28000)
writing fw base 0x102000000000000 ...
readback                     = 0x102010000b28001   <- unchanged
```

**On a cold boot, before we have written anything, the register already holds
a fully-formed value**: tag `0x0102` — exactly `AVE_ASC_FW_BASE_TAG` — bit 0
set, and a base field of `0x10000b28000`. That is iBoot's programming, and our
write does not take.

Two things follow. First, our image is at phys `0x10008800000`, so whatever
that register points at, **it is not what we loaded**. Second, `0x10000b28000`
is not in `/proc/device-tree/reserved-memory` either — the `asc-firmware`
carve-outs on this machine begin at `0x10000c68000` — so it is not simply a
pointer to a preserved iBoot copy of the AVE firmware sitting in a region we
could adopt.

The register being pre-programmed and write-ignored is consistent with it
being locked at a higher security level than Linux runs at, which is the class
of thing m1n1 exists to set up. **Confirmed:** the write is ignored (readback).
**Unknown:** whether it is locked, whether `0x50000` is the write path at all
as opposed to a status mirror, and whether the core is executing anything.

Do not read the silence as evidence about the handshake, the DART mapping or
the boot config. If the core is not fetching our image, none of those have
been tested yet.

### Resolved: the core runs, and it fetches at an IOVA we never mapped

The DART log settles it. During and after the run:

```
apple-dart 40d040000.iommu: translation fault: status:0x80000800
    stream:0 code:0x800 (unknown) at 0x10000b28200
```

`0x10000b28200` is the fw-base register's base field (`0x10000b28000`) plus
`0x200`. So:

- **The core is executing.** It is not dead, not held in reset, and it did not
  fault internally - it is issuing instruction fetches.
- **The register's base field is an IOVA, not a physical address.** The fetch
  goes through DART `40d040000` and faults because we mapped the image at
  IOVA 0 and it is reading at `0x10000b28000`.
- **Instruction fetch uses stream 0**, not stream 15. That retires the
  standing SID-15/bypass hypothesis in [04](04-roadmap.md) phase 3 as the
  explanation for the silence: fetch translates, on SID 0, and the DART is
  configured well enough to report the fault.
- **We cannot repoint it**, since the register ignores our writes. So the
  driver must map the image *where the register says*, which is what
  `ave_fw.c` now does - it reads the base out of the register instead of
  choosing IOVA 0.

This also explains the silence completely without needing the handshake, the
boot config or cache coherency to be wrong: the core never executed a single
instruction of our image, so none of that had been exercised.

Note the first fetch is at `base + 0x200`, not `base`. Unexplained; possibly a
vector table or header offset. Worth watching once it boots.

### Hazard found the hard way: the fault storm outlives the driver

Left alone, the faulting core generated roughly **260,000 DART interrupts per
second** and kept doing so after `rmmod`, because the staged probe takes a
runtime-PM reference at stage 6 and `ave_stop()` only unwinds a fully started
device. `ave_remove()` now halts the core (`CPU_CONTROL = 0`) and drops that
reference, so unbinding really does stop it.

### The fetch cannot be satisfied by translation: the DART is 32-bit in

```
apple-dart 40d040000.iommu: DART [pagesize 4000, 16 streams,
    bypass support: 1, bypass forced: 0, locked: 0, AS 32 -> 42] initialized
apple-dart 40d030000.iommu: DART [... bypass support: 0, ... AS 32 -> 42]
```

`AS 32 -> 42` is a **32-bit input address space**. The core fetches at
`0x10000b28200` — about 1.1 TB — which no page table on this DART can ever
describe. Three experiments, all with `iommu_iova_to_phys()` verifying the
mapping was present and correct:

| image mapped at | result |
|---|---|
| IOVA `0` | fault at `0x10000b28200` |
| IOVA `0x10000b28000` (wide field) | map verified, still fault at `0x10000b28200` |
| IOVA `0xb28000` (low 32 bits) | map verified, still fault at `0x10000b28200` |

So the software mapping being right is not the issue, and re-reading the
register's base field differently does not help: the DART faults on the
address the *core* emits, and the core emits a 44-bit one.

**Therefore the fetch is not meant to be translated at all — it is meant to
bypass.** `40d040000` reports `bypass support: 1` (and `40d030000` reports
`0`, which is presumably why the fetch goes through the former). In bypass the
address passes to physical memory unchanged, and physical `0x10000b28000` is
where iBoot placed a firmware image before handing the machine over. Linux's
`apple_dart_hw_reset` clears bypass on probe, which is exactly the mechanism
[04](04-roadmap.md) phase 3 suspected — the correction is that it applies to
**stream 0 on the fetch DART**, not to SID 15.

**Next step:** enable bypass for stream 0 on `40d040000` and see whether the
core executes iBoot's image. Note what that implies if it works: the core runs
firmware we did not load, so the `IOBA` patch in
[40](40-firmware-io-base.md) becomes irrelevant to instruction fetch (iBoot's
copy already has it filled in), and our own image is then only useful if we
can get the fetch translated — which needs the fw-base register to hold an
address inside 4 GiB, and that register ignores our writes.

### Bypass works: zero faults, and iBoot's image is a full bootstrap

Overlay corrected to reference only the real DART, then:

```
$ echo identity | sudo tee /sys/kernel/iommu_groups/16/type    # now accepted
```

Result: **zero DART translation faults**, where every previous run produced a
continuous storm at `0x10000b28200`. The core fetches successfully for the
first time. Identity is visible in the driver log too - `iova
0x1000bc00000 -> phys 0x1000bc00000`.

What it is fetching is not a shim. Reading 1 KB at the branch target
(`data/blobs/iboot-ave-stub-1k.bin`, gitignored):

```
 200:  14000000  b  0x200          ; a parking loop, not the entry
 204:  d5384241  mrs x1, currentel ; <- entry, from the b +0x204 at offset 0
 20c:  f1000c3f  cmp x1, #0x3
 218:  d51e4021  msr elr_el3, x1
 224:  d69f03e0  eret              ; drop EL3 -> EL1
 234:  d518c000  msr vbar_el1, x0  ; install vectors
 238:  d53800a0  mrs x0, mpidr_el1
 250:  d5181040  msr cpacr_el1, x0
 284:  d518a200  msr mair_el1, x0  ; MMU attributes
 2a8:  f0000740  adrp x0, 0xeb000  ; image is at least ~960 KB
```

That is a complete coprocessor bring-up sequence, so iBoot leaves a full
firmware at physical `0x10000b28000` - not a loader stub that wants our image.

**Consequences for the driver.** In bypass there is nothing useful to map: the
core runs iBoot's image, not the one we load, so `ave_fw_load()` now skips the
`iommu_map()` on an identity domain. It also means the firmware-globals
diagnostic from [33](33-firmware-logging.md) is dead in this mode - those
offsets are into *our* image, which the core never executes, so they will read
zero regardless and must not be cited as evidence of anything.

**Still silent.** `CPU_STATUS` remains `0x2c` and no scratch register changes.
So fetch was necessary and is not sufficient. Open questions, in order:

1. Is the core actually progressing, or parked? Nothing currently distinguishes
   "executing happily" from "spinning in the `b 0x200` loop". A
   before/after diff of physical memory around the image would settle it,
   since a running firmware writes *something*.
2. The boot config now sits at physical `0x10473518000` and we pass it as
   scratch1 `0x73518000` / scratch2 `0x104`. The split is our convention, not
   a confirmed one.
3. Whether this image expects the same `0x08042006` handshake at all - it is
   not the image [34](34-boot-handshake.md) was derived from.

### Correction: bypass does not fix the fetch, it silences it

The "zero faults" result above was read too generously. Adding a liveness test
- checksum every page of iBoot's image before starting the core and again
after - shows **no page changes**, across 16 MiB and a 6 second wait. The core
is not executing.

So the absence of faults did not mean the fetch succeeded. It meant no fetch
was happening.

Two variables had been changed at once (the single-DART overlay *and* the
identity domain), which was careless. Separating them:

| overlay | domain | DART faults | image modified |
|---|---|---|---|
| single DART | DMA | **yes**, at `0x10000b28200` | no (cannot execute) |
| single DART | identity | none | no |

The overlay correction is therefore sound - the core still fetches with it in
place - and **the identity domain is what stops the fetch**. Bypass is not the
answer, or at least not this bypass.

A candidate explanation, unverified: the ADT carries
`bypass-address = 0x200000000` on `dart-ave0`, the same value as the
`/arm-io` bus translation. If Apple's bypass path applies that offset and
Linux's does not, the fetch in bypass lands somewhere that simply does not
respond - no translation, so no fault, and no instructions, so no writes.
That is consistent with everything observed but is not yet evidence.

**What is now solid:**

- The core issues instruction fetches at `0x10000b28200`, a 44-bit address.
- This DART has a 32-bit input address space, so no page table can ever
  satisfy that fetch.
- The fw-base register that supplies the address ignores our writes.
- iBoot left a real firmware at that physical address.

Those four facts do not have a resolution on the Linux side alone: we cannot
make the DART translate a 44-bit address, and we cannot change the address.
The remaining routes are to find how the register becomes writable (it is
plausibly locked above EL1, which is what m1n1 exists to handle), or to find
the configuration under which Apple's own bypass path routes the fetch
correctly.

## Run 2026-09-13: liveness with controls, and what is in the window

Log: `results/handshake-20260913-102149-c4e58389.log`, commit `03a66c4`,
overlay variant 0 (DART, DMA domain).

### CPU_STATUS discriminates, and says "not running"

2000 samples each, bit names from m1n1 `hw/asc.py` (`IRQ_NOT_PEND` and
`FIQ_NOT_PEND` are m1n1's own guesses):

| core | CPU_CONTROL | CPU_STATUS histogram |
|---|---|---|
| **DCP** (38bc00000, known running — positive control) | `0x10` | `0x2d` RUNNING\|IDLE… x1905, `0x2c` x74, `0x28` x20, `0x08` x1 |
| AVE, powered, before start (negative control) | `0x0` | `0x2a` STOPPED\|IDLE… x2000 |
| AVE, started | `0x10` | `0x2c` IDLE… x2000 — **RUNNING never seen** |
| AVE, `CPU_CONTROL` then written 0 | `0x0` | `0x2c` x2000 — **not STOPPED again** |

- The discriminator passes its controls: a live RTKit core sits in
  `RUNNING|IDLE` (in WFI between interrupts) and occasionally wakes (`0x08`);
  a held core reads `STOPPED`. The started AVE core is neither: released
  from `STOPPED`, never `RUNNING`, while the DART reports fetch faults at
  `+0x200` (and occasionally `+0x280`) at ~70k/s. **Measured:** its status is
  not the status of a running RTKit core. **Inferred** from the DCP control,
  not shown: a core executing a `b .` park loop would read `RUNNING` and not
  `IDLE`, so this is more likely a core that never completes an instruction
  fetch than one parked at `+0x200`. The bit names are m1n1's, partly guessed,
  so this inference is only as good as they are.
- **Writing `CPU_CONTROL = 0` does not stop a started core.** Status stayed
  `0x2c` and faults continued until `rmmod` gated VENC. Consistent with
  Apple never clearing it ([09](09-firmware-load.md) §2.5). The earlier
  "asc_start clears CPU_CONTROL, so runs are independent" reasoning was right
  in conclusion (power gating resets it) but wrong in mechanism.

### The ASC timer is not a liveness signal

`bank1+0x178000` advanced ~2.5 M per ~100 ms at `freq = 24000000` in all
three states, halted included. It is a free-running 24 MHz timebase, as the
negative control was there to catch ([42](42-asc-firmware-ownership.md) §7
corrected accordingly).

### Scanning the 16 MiB window from `0x10000b28000`

The same scan over our own image finds IOBA 1, IOSZ 1, `AppleAVE2FW/Firmware`
15, `CmdProcessor` 1 — so the patterns are findable. In the window:

| hit | where | payload | in |
|---|---|---|---|
| `CmdProcessor` x3 | `+0xbca9b`..`+0xbee47` | — | the AVE slot (TEXT-like) |
| IOBA / IOSZ | `0x10000c6544c` (`+0x13d44c`) | **0 / 0** | the AVE slot, `0x2bb4` below the ISP carve-out |
| `CmdProcessor` x3 | `0x10001522480`..`0x10001590247` | — | ISP TEXT carve-out |
| IOBA / IOSZ | `0x1000165ec4c` | 0 / 0 | a 104 KiB run just after the SIO TEXT carve-out (unreserved) |
| IOBA / IOSZ | `0x10001a93bdc` | **`0x40c000000`** / 0 | an isolated 88 KiB run, `0x10001a90000`-`0x10001aa6000` (unreserved) |
| `AppleAVE2FW/Firmware` | — | — | **0 hits anywhere** |

Non-zero extent of the AVE slot: `0x10000b28000` to the ISP carve-out at
`0x10000c68000`, nearly all of it.

What this does and does not show:

- **`CmdProcessor` does not identify AVE**: three hits sit inside the ISP
  TEXT carve-out. The IOBA/IOSZ pair occurs **three times**, twice outside
  the AVE slot in DRAM no carve-out claims. So "an IOBA hit means AVE"
  ([42](42-asc-firmware-ownership.md) §7.1) is not a test either: it cannot
  say which copy is live, or rule out another RTKit firmware using the same
  tag convention. A trap 2 non-test, caught by running it.
- The tag in the AVE slot is `0x2bb4` bytes below the ISP boundary. Our
  image carries 92 KiB of initialised DATA after offset 0 (IOBA at DATA
  `+0x1f5`), so a comparable DATA segment starting there **cannot fit in the
  slot**. Either this build's DATA is laid out very differently, or what sits
  at `+0x13d000` is not the live DATA.
- The 88 KiB run at `0x10001a90000` is about the size of our initialised DATA
  (92 KiB), and its IOBA holds `0x40c000000` — exactly the AP-physical base of
  AVE's own I/O window (overlay `reg[4]`). No other firmware has a reason to
  carry that value. **Inferred, not shown:** this is the AVE DATA segment,
  physically discontiguous from TEXT the way ISP's is, which would support
  §3.4's reading that the reset-vector address is meant to be translated.
- `AppleAVE2FW/Firmware` source paths are absent from the whole window, so the
  loaded build differs from our IPSW extraction at least in string content.

Next: copy the window out (`test/physdump.ko`, read-only, touches no
hardware) and compare offline — DATA at `0x10001a90000` against our DATA,
TEXT in the slot against our TEXT — rather than adding more printk.

### Side effects on the machine

The started core stormed the DART for ~2 minutes while the module sat
loaded, which made the desktop stutter. `rmmod` gated VENC and the last fault
was reported ~2 s later; the DART's shared IRQ line then stayed asserted with
nothing to report for ~9 s, and the kernel disabled IRQ 129 ("nobody cared").
Nothing else shares that line. Consequences:

- the probe now powers off at the end of stage 15 instead of leaving a
  started core faulting;
- `tools/handshake-test.sh` refuses to run on a boot where any IRQ has been
  disabled, since fault reporting is what it measures;
- **the next AVE hardware run on this machine needs a reboot first.**

## The dump: what iBoot actually loaded (2026-09-13)

`test/physdump.ko` copied the same 16 MiB window to
`data/blobs/iboot-window-16m.bin` (gitignored). It reproduces the in-kernel
scan exactly — first word `0x14000081`, the three IOBA hits at the same
addresses — so the copy is sound. Everything below is offline analysis of it.

### 1. The running firmware is macOS 13.5's, not the one we analysed

Every RTKit firmware in the window (AVE, ISP, SIO, GPU) carries
`RTKit-2062.141.1.release`. Our extracted `ave_h13c.bin` is
`RTKit-3255.160.4.release`, from the **macOS 26.6.2** IPSW (build 25G83), and
`kc.macho` is the matching xnu-12377. `/proc/device-tree/chosen` says why:
`asahi,os-fw-version = 13.5`. The coprocessor firmware iBoot loads is chosen
by the OS firmware bundle the Asahi installer pinned, independent of the
system firmware (`asahi,system-fw-version = 26.4`).

The AVE text in memory is clearly the same product — `CAVCController_H13C.cpp`,
`CAVE_CMD_*`, `PlatformIOPIPCManager` — but only ~5% of 32-byte code samples
match our image, and 1087 of its ~2200 strings are shared. (This section
first cited the string `"Host and FW Interface is mismatched, please ensure
the versions are aligned."` as AVE's. **It is not**: it sits at dump
`+0x13e7a3`, inside the GPU firmware fragment, and occurs in neither AVE image
nor either kext. AVE has no interface-version check at all — a wrong command
size makes its firmware spin — see [46](46-abi-13.5-commands-session.md).) **Every host/firmware ABI detail derived from the 26.6.2 firmware
and kext (docs 07, 20, 32, 35–39) is unverified against the firmware that
would actually run, and should be re-derived from the 13.5 AppleAVE2FW and
AppleAVE2 kext before it is trusted.** This is the same constraint Asahi's DCP
driver lives under.

### 2. RTKit tag lists identify each segment

`tools/rtkit_tags.py` decodes the boot-argument tag lists (STKG … IOBA, IOSZ)
that the loader fills in. Three live lists, all with `SOC_ = 0x6001`,
`SOCR = 0x11`:

| list at | CpAd | WrAd | IOBA | RTSZ | owner |
|---|---|---|---|---|---|
| `0x10000c652a0` | `0x406000000` | `0x406400000` | 0 | `0xcc000` | **GPU** (`gpu@406400000`) |
| `0x1000165eaa0` | `0x285000000` | `0x285400000` | 0 | `0x50c000` | unidentified; no Linux node at that address |
| `0x10001a93a30` | `0x40d800000` | `0x40dc00000` | **`0x40c000000`** | `0x220000` | **AVE** (ASC bank; WrAd = bank + `AVE_ASC_BASE`) |

Confirmed consequences:

- **AVE's DATA segment is at `0x10001a90000`** (the 88 KiB run), iBoot-filled.
  AVE's TEXT is in the region at `0x10000b28000`. They are **physically
  discontiguous**, like ISP's — so [42](42-asc-firmware-ownership.md) §3.4's
  reading stands: the firmware expects a translated address space
  (`RTSZ = 0x220000`) that joins them.
- **The region at `0x10000b28000` is not an AVE carve-out.** Its last
  ~64 KiB is GPU firmware DATA. The earlier "slot" framing, and the IOBA hit
  at `0x10000c6544c`, were someone else's.
- **IOBA is AP-physical.** iBoot writes `0x40c000000`; our patch wrote bus
  `0x20c000000` ([40](40-firmware-io-base.md) §4 corrected, `ave_fw.c` fixed).
  Moot while RVBAR is locked to iBoot's image, but it was wrong.

### 3. What this changes

The fetch blocker is unchanged — a locked reset vector, a 41-bit fetch, a
38-bit DART — but it is now better posed. The live image is TEXT at
`0x10000b28000` and DATA at `0x10001a90000`, joined in some address space of
size `0x220000`. Whatever makes the reset fetch work on macOS must also
produce that join, which is a stronger constraint than before.

Separately and more urgently for everything after stage 3: obtain the
**macOS 13.5** `AppleAVE2FW_H13C.im4p` and AppleAVE2 kext, and re-verify the
ABI docs against them. Otherwise even a booting core will be spoken to in a
dialect it rejects.

> **Correction (2026-09-13, [44](44-reset-fetch-path.md)).** Throughout this
> document "the fetch cannot be satisfied by translation" / "untranslatable"
> misreads the fault. `code:0x800` is `NO_DAPF_MATCH`: the CPUDART's address
> filter (DAPF, AP `0x40d044000`, never programmed by Linux or m1n1) rejected a
> *physical* TEXT fetch. The identity-domain "no execution" result is
> explained by the firmware reaching DATA at `0x1f0000ec000`, a DART window,
> which bypass passes out untranslated. In "the dump", the list at
> `0x1000165eaa0` is **ANE** (`ane0` at bus `0x84000000`).

## 2026-09-13 19:25 — THE AVE FIRMWARE BOOTS AND SENDS MESSAGE 1

`results/n3-start-1789323963.kmsg`. Patched m1n1 stage 2 (`v1.6.1-1-g1ae6361`,
docs/50) had programmed AVE's DAPF from the live ADT: slot 0 TEXT
`0x10000b28000-0x10000c13ffc` r0 `0x11`, slot 1 the `0x1f0` window, slot 2 the
ADT MMIO entry. Linux made **no** DAPF writes. Overlay `variant=3`, `apple-ave
stop_after=15 fw_map_data=1 fw_map_text=2`:

- 13.5 pre-start scratch: `scratch0=0x8042006 scratch1=0x0 scratch2=0xe`;
  RVBAR untouched (`0x102010000b28001`).
- Halted control: CPU_STATUS `0x2a` STOPPED.
- ASC start at 252.365; **9 ms later** the IRQ captured the firmware's first
  message: status `0x1`, **`MSG 1: 0x00000007 0x00009bc0 0x00000100
  0x000c0000`** — exactly the macOS 13.5 values predicted statically
  (docs/45: 7 channels, `0x9BC0`-byte channel block, protocol `0x100`,
  `0xC0000`).
- **19 pages changed** in the 16 MiB window, first at `+0xf6a000` =
  physical `0x10001a92000`, inside iBoot's AVE DATA segment: the firmware is
  executing and writing its data.
- **Zero DART faults**, no SError, no disabled IRQ; the machine stayed up
  through a 30 s hold and the stage-15 power-off.

**Confirmed, closing the fetch-path question (docs/44):** with the DAPF
admitting TEXT physically and nothing mapped at DVA `0xb28000`, the reset
fetch succeeded — the admitted physical TEXT fetch passes through the DART
untranslated (H1). DATA is reached through the `0x1f0` window via the DART
mapping DVA `0xec000` → `0x10001a90000`. The locked RVBAR was never the
problem; the missing DAPF entry was.

After message 1, CPU_STATUS sampled `0x2c` (IDLE, not RUNNING) for 200 ms:
the firmware is waiting for the host's message 2, which `stop_after=15` does
not send. Next: `stop_after=16`, the 13.5 handshake (messages 2-5, seven
channels, TERMINAL log).

## 2026-09-13 19:30 — second start in the same boot: silent (DATA not restored)

`results/n3-handshake-1789324173.kmsg`, `stop_after=16`, same setup as the
successful run, same boot, after that run's power-off. ASC released at
462.76; **no message 1** in 6 s (stage 14) nor 2 s more (handshake:
`-ETIMEDOUT`); CPU_STATUS `0x2c`; **no page of the 16 MiB window changed**;
zero DART faults. The probe failed cleanly (IRQ quiesced, powered off, iBoot
DATA mapping removed).

**Inferred cause:** the first start modified iBoot's DATA (19 pages), and the
second start ran on that state. macOS restores DATA before every start -
13.5 `AVE_Firmware::UpdateImage` (`0xfffffe0008f11ae4`, docs/45 row 32), 26.6.2
`AVE_FwImg::UpdateImage` → `RestoreCTRRData` (docs/42 §4-5). We do not.

Pristine DATA is reconstructible: in the pre-boot dump
(`data/blobs/iboot-window-16m.bin`, taken before any successful start) DATA's
first `0x64000` bytes equal the 13.5 image's DATA except iBoot's 147 filled
bytes, and every page from `0x64000` to the end of the dump (`0x98000`) is
zero (bss, initialised by the firmware; the remaining `0x9c000` bytes are
assumed zero - inferred).

Next: (1) confirm by running `stop_after=16` as the **first** start of a
fresh boot; (2) implement the restore - copy the pristine `0x64000` bytes
plus zero fill over physical DATA before every start, as macOS does.

## 2026-09-13 19:36 — FULL macOS 13.5 HANDSHAKE; THE FIRMWARE LOGS OVER TERMINAL

`results/n3-handshake2-1789324537.kmsg`: fresh boot (pristine DATA), patched
m1n1, overlay `variant=3`, `stop_after=16 fw_map_data=1 fw_map_text=2`,
**first** firmware start of the boot.

| step | observed | predicted (docs/45) |
|---|---|---|
| msg 1 | `nch=7 chanmem=0x9bc0 ver=0x100 heap=0xc0000` | same |
| msg 2 (host) | FwIPC iova `0xff800000`, size `0x700000` | size `0x700000` |
| msg 3 | `fw_base 0xffffffff80000000`; fw heap (surface 23) 768 KiB at `0xff300000` | heap after msg 3 |
| msg 4 | info block fw `0xffffffff80009bc0`, chanmem `0xffffffff80000000` (`0x9bc0`), dev_type 11 | dev_type 11 |
| msg 5 | desc fw `0xffffffff80000000`, client buffer `0xb4000` (max `0x100000`) | `0xb4000` literal |
| channels | TERMINAL id4 dir2 bit0 512 slots; IO id1 dir0 bit1 32; DEBUG id5 dir0 bit1 8; BUF_H2T id6 dir0 bit2 1; BUF_T2H id7 dir1 bit3 1; SHAREDMALLOC id3 dir1 bit3 8; IO_T2H id2 dir1 bit3 32 | same table |
| ready | `macOS 13.5 handshake complete: 7 channel(s)`; `coprocessor up`; `Apple AVE video encoder ready` | |
| log | **`fw[0]\| FW Cfg: prod, tag: AppleAVE2FW-6070.11.1, SHA: c86a10b71`** over TERMINAL | log over TERMINAL |

Zero DART faults, no SError, AVE DART fault IRQs at 0 interrupts; the driver
stayed loaded with the firmware idle. **Phase 4 (IPC transport live) is
reached** except for sending a command and reading a reply.

**Confirmed:** every element of the 13.5 boot/IPC ABI reconstructed
statically (docs/45, `driver/ave_abi_boot.h`, `ave_ipc.c`) matches the live
firmware, down to the firmware build tag `AppleAVE2FW-6070.11.1` identified
in docs/43.

Open items before commands: restore pristine DATA before every start (the
second start in a boot is silent otherwise), then Config → Open → Start_AVC
(docs/46) over the IO channel.

## 2026-09-13 20:37 — with the patched m1n1, VENC is no longer power-cycled

Trying to run docs/51's negative control (start the firmware, unload, dry-run
the restore to measure drift) produced a different result:

```
[halted ]   0x0028 x2000 FIQ_NOT_PEND? IDLE
fw_restore_data: REFUSING - core is not halted (CPU_CONTROL 0x0, CPU_STATUS 0x28)
```

`CPU_STATUS` is `0x28`: **not** `STOPPED`, where a fresh boot reads `0x2a`. And
`pm_genpd_summary` shows `venc_sys` **on** with zero devices after `rmmod`,
while its children (`venc_dma`, `venc_pipe4/5`, `venc_me0/1`) are `off-0` and
both AVE DARTs are `suspended`. The block is genuinely live — the core kept
running after the module was removed.

**Confirmed:** on this boot the driver's power-off no longer gates `venc_sys`,
so the core is not reset between loads and a second start cannot work,
independent of DATA drift. The restore's halted-core gate correctly refused,
which is the gate doing its job.

**Inferred:** the patched m1n1 leaves `VENC_SYS` on (its power-down of the
`VENC-DART` clock gate is a no-op because that PMGR device is virtual —
docs/50, review finding F2), so Linux's genpd sees it on from boot and nothing
gates it afterwards; `genpd: Disabling unused power domains` runs at 0.23 s,
long before the overlay exists. **Unknown:** why genpd does not power it off
when the last consumer suspends — both DARTs are suspended and no device is
attached.

**Consequences:**

- The 19:30 silent second start is *not* re-explained by this: that run read
  `0x2a STOPPED` before starting, so the core there *was* power-cycled and the
  DATA-drift hypothesis (docs/51) still stands for it.
- Until VENC can be gated on demand, **each firmware start needs a fresh
  boot**, and docs/51 step (b) (reload without rebooting) cannot run.
- Candidate fixes, none tried: an explicit `reset_control_reset()` of the
  block at probe when the core is not STOPPED (the DT gives `resets = <0x1d>`
  and an earlier run showed the call returns 0 without hanging); or finding
  what keeps `venc_sys` on and gating it properly.

## 2026-09-13 20:43 — CONFIG AND OPEN ACCEPTED; Start_AVC asserts on a null address

`results/cmd1-1789328606.kmsg`: fresh boot, patched m1n1, overlay `variant=3`,
`stop_after=16 fw_map_data=1 fw_map_text=2 fw_restore_data=2 session_selftest=1`.

**Restore dry run (nothing written):** all gates passed — core halted
(`CPU_STATUS 0x2a`), blob sha256 verified, TEXT matched the 13.5 image over
3 x 0x4000, destination outside System RAM/`/memory`/`/reserved-memory`. Drift
was **exactly 8 bytes in 1 page at DATA+0x3a38**, live STKG
`0xa52fda7ba8e5cb00` vs blob `0x816ea533007323bc`. **Confirmed:** docs/51 §2.3
is right — `STKG` is a per-boot random stack guard, and it is the only
fresh-boot difference. The "expected exception" the review added is what
happened.

**Commands over IO, replies on IO_T2H:**

| command | sent | reply | status |
|---|---|---|---|
| Config | 112 bytes (0x70) | id `0x0e01`, cid 0, slot `0xffffffff` | **`0xee0000` ACCEPTED** |
| Open | 64 bytes (0x40) | id `0x0e02`, cid 1, slot 3 | **`0xee0000` ACCEPTED** |
| Start_AVC | 69136 bytes (0x10E10) | none | **firmware assert** |

**Confirmed:** the 13.5 command ABI reconstructed statically (docs/46) is
right for Config and Open — sizes, ids, the client-id echo, and the
`0xEE0000` success status all match on hardware. The channel split the
review corrected (commands on IO, completions on IO_T2H) is also confirmed:
every reply arrived on IO_T2H, and the IO ack arrived too.

**Start_AVC** was accepted by the dispatcher (the IO ack arrived; the
firmware's own history log shows `Start AVC 1` after `Open 1` and `Config`)
and then asserted:

```
fw[3]| SCRATCH_REG32_RD: addr fffffffff5050034 value 00000001
fw[3]| SCRATCH_REG32_WR: addr fffffffff5050034 value 40000001
fw[3]| ASSERT: ./AppleAVE2FW/utils/MappedMemory.cpp, 39: paddr != 0
```

**Inferred:** this is the `reg_dart_addr` field the session code sends as 0
(Config +0x48, `AVE_Reg::GetDARTAddr(3)`), which docs/46 flagged as unknown
and `ave_session.c` warns about; `ProcessInitStage2` loads it and passes it on
(fw `0x13e3c`). A zero there reaching a `MappedMemory` constructor matches the
assert exactly. **Not yet confirmed** — it could equally be one of the buffer
addresses Start_AVC carries. The next step is to read `MappedMemory.cpp:39`'s
caller in the 13.5 firmware and identify which field it maps.

The firmware log is now a working oracle: it names the source file, line and
assertion, and dumps a command history with client ids.

## 2026-09-13 21:13 — START_AVC ACCEPTED: an encoder session is open

`results/cmd2-1789330374.kmsg`, fresh boot, `stop_after=16 fw_map_data=1
fw_map_text=2 session_selftest=1` (no DATA restore; first start of the boot).
The only change since the previous run is the parameter-sets buffer
([52](52-start-avc-assert.md)).

| command | size | reply | status |
|---|---|---|---|
| Config | 0x70 | id `0x0e01`, cid 0, slot `0xffffffff` | **`0xee0000` ACCEPTED** |
| Open | 0x40 | id `0x0e02`, cid 1, slot 3 | **`0xee0000` ACCEPTED** |
| **Start_AVC** | **0x10E10** | id `0x0e04`, cid 1, slot 6 | **`0xee0000` ACCEPTED** |

Session: 1280x720 (coded 1280x720), fixed QP 30, I-only, profile 66 level 40,
1 recon / 1 coded / 1 coded-header buffer. No assert, no DART fault, no
SError, no disabled IRQ; the firmware logged only its startup banner.

**Confirmed:** the macOS 13.5 command ABI reconstructed statically in
[46](46-abi-13.5-commands-session.md)/[47](47-abi-13.5-frame-rc-surfaces.md)
is right for the whole session-setup path - command ids, sizes, the header
layout, the client-id echo, the `0xEE0000` status, the fixed-QP selector
(`ui32RCFlag = 2`), the SPS/PPS fields, and the buffer tables. **Phase 5
(session setup) is done**: the firmware accepts Config -> Open -> Start_AVC
without rejecting anything.

**Confirmed by the fix:** the earlier `MappedMemory.cpp:39 paddr != 0` was
the parameter-sets buffer at Start_AVC +0xFB30/+0xFB38, exactly as
[52](52-start-avc-assert.md) argued from the disassembly, and `Config +0x48`
(`reg_dart_addr`) is genuinely unused on 13.5 - we still send 0 and the
session starts.

Next: phase 6, one encoded frame. `Process` (13.5 id 7, 0x1940 bytes,
picture block at +0x9C8) with an input surface, then read the coded buffer
back and check it with ffmpeg.

## 2026-09-13 22:11 — first Process: the firmware encodes into the AVC path, asserts on LowResOutput

`results/frame1-1789333891.kmsg`, fresh boot, `session_selftest=1
session_frame=1`. Config, Open and Start_AVC accepted again (with the
SrcNeighbor tables now filled: 80 KiB per slot for 80 MB columns, from the
docs/47 formula). Then:

```
session: Process: 1280x720, luma 0xfe800000/0xe1000 chroma 0xfe780000/0x70800,
         frame_type 3, slot 21; sending 6464 bytes on IO
fw[0]| Uncompress Ref is not supported
fw[3]| ASSERT: ./AppleAVE2FW/kf_controller/H9/CAVCController_H13C.cpp, 5782:
       (pPicParams->sLowResOutput.LowResSrcLumaScaled + EncCom...   [clamped]
```

and the firmware's history gained two entries beyond the session ones:

```
6  client 1  LogID 128  line 2906  FrameType 3   AVC
7  client 1  LogID 131  line 446   FrameType 3   7 0xffffffff807fd2a8 0 0 0 0
```

**Confirmed:** `Process` (13.5 id 7, 0x1940 bytes) is accepted by the
dispatcher and reaches `CAVCController_H13C`'s per-frame setup - the command
id, size, picture-management block offset and the fields set so far are right
far enough to get into the encoder proper. The IO ack arrived and the
reply-id filter added after the review reported "0 other completion(s)", so
the timeout is a real absence, not a mis-captured message.

**The next missing field:** `sLowResOutput.LowResSrcLumaScaled` - a
low-resolution scaled luma buffer for the motion-estimation (LRME) pass,
which we send as zero. The assert text is clamped at ~120 characters by the
firmware's own log path (docs/45), so the full predicate must be read out of
the image at `CAVCController_H13C.cpp:5782`.

**Unknown:** whether LRME can be switched off for an I-only session (which
would be simpler than supplying the buffers), what the rest of the
`sLowResOutput` group is, and whether `Uncompress Ref is not supported`
matters or is informational.
