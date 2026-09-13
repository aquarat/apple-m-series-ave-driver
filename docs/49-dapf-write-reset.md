# Why the first Linux write to AVE's DAPF reset the machine

Static analysis of the E3a result in [48](48-e1-e3-procedure.md) ("attempts 2
and 3"): with overlay `variant=3` (apple-dart bound, TCR[0] `0x80`,
`ENABLED_STREAMS 0xffff`, `DAPF_LOCK 0`), every read of the AVE DART and of
its DAPF at AP-phys `0x40d044000` was harmless, and the machine died on the
first write, `writel(0, 0x40d044000 + 0x00)` (slot 0 r0).

Sources: m1n1 `main` (fetched 2026-09-13), Asahi Linux `asahi` branch
`drivers/iommu/apple-dart.c`, XNU `xnu-8796.141.3` open source, the 13.5
kernelcache `data/blobs/macos-13.5/kc.macho`, and both ADTs. Every claim is
marked **confirmed**, **inferred** or **unknown**. Nothing was run.

---

## 1. The block really is the DAPF (the wrong-block hypothesis is dead)

- **Apple names it.** `/defaults/pmap-io-ranges` in both the 13.5 and the
  26.6.2 ADT tags `0x40d044000 +0x4000` as `'DAPF'`, `0x40d040000` and
  `0x40d030000` as `'DART'`, `0x40d020000` as `'SMMU'`. ISP's `0x3860ec000`
  (the DAPF E1 read successfully) carries the same `'DAPF'` tag, as do AOP's
  `0x29380c000` and PMP's `0x28e304000`, which m1n1 programs. **Confirmed.**
- **The ADT layout rule holds for every DART with a DAPF.** The `instance`
  property is a list of 12-byte records {type, class, name}; a DART that has a
  DAPF is listed with type `DAPF`, and its DAPF is the last `reg` entry at
  first DART + `0x4000`:

  | node | instance records | DAPF reg | address | m1n1 index |
  |---|---|---|---|---|
  | dart-aop | `DAPF DART LLT` | reg[1] | `0x9380c000` (bus) | 1 |
  | dart-pmp | `DAPF DART` | reg[1] | `0x8e304000` | 1 |
  | dart-isp0 | `DAPF DART LLT`, `DART DART BULK`, `DART DART RT`, `SMMU SMMU BULK`, `SMMU SMMU RT` | reg[5] | `0x1860ec000` | 5 |
  | dart-ave0 | `DAPF CPUD ART` (CPUDART), `DART DART`, `SMMU SMMU` | reg[3] | `0x20d044000` | — |

  **Confirmed.** (m1n1 `src/dapf.c` `dapf_entries[]`, ADT decode.)
- **Apple's own register map matches ours.**
  `AppleT6000DART::_apfCaptureRegs` (13.5, `0xfffffe0009b17688`) snapshots,
  for each of 16 entries at `0x40` stride, 32-bit words at `+0x0, +0x4, +0x8,
  +0xc, +0x10, +0x14` (r0, r4, start lo/hi, end lo/hi) — the t8020 layout
  `ave_dapf.c` uses. It captures no lock or enable register in the DAPF block.
  **Confirmed.**

## 2. m1n1: how a DAPF is written successfully

- `dapf_init()` (`src/dapf.c`): if the DART node has `clock-gates`,
  `pmgr_adt_power_enable(path)`; write each ADT entry as r4, start, end, then
  r0; then `pmgr_adt_power_disable(path)`. **No unlock, lock or enable write
  anywhere.** **Confirmed.**
- `dart-ave0`'s `clock-gates` is PMGR device `0x1fc` `VENC-DART`, a virtual
  device (`psreg 0`) whose parent is `VENC_SYS` (`0x126`); ISP's is `0x1fb`
  `ISP-SYS-DART` under `ISP_SYS`. So "power the DART" means `VENC_SYS` on,
  which Linux had. **Power is not the difference.** **Confirmed** (ADT
  `pmgr/devices`).
- **When:** `dapf_init_all()` is called from `kboot_boot()` (`src/kboot.c`),
  the last hardware setup before jumping to Linux, after `usb_init()` and
  `pcie_init()`. m1n1 never configures the ISP, AOP or PMP DARTs itself, so
  every m1n1 DAPF write lands on a DART that no OS has configured yet.
  **Confirmed.**
- **Runtime use follows the same order.** m1n1's own experiment
  `proxyclient/experiments/aop.py` (commit `4555cc5148`, which added
  `dapf_init` to the proxy) does `p.dapf_init_all()` **first**, then
  `DART.from_adt(...)`, `dart.initialize()` (TCR[0..14] TRANSLATE, TCR[15]
  BYPASS_DART, TTBRs invalid, `ENABLED_STREAMS = 0`), and only then enables
  translation. **Confirmed.**
- **Why AVE is not programmed:** it is simply not in `dapf_entries[]`.
  History: `4555cc5148` (2022, aop/mtp/pmp), `6072b8facf` (2023, reg indexing +
  isp), `e1421f6fef` (2023, isp0). No AVE consumer ever existed upstream.
  **Confirmed.**
- **m1n1 and Linux run at the same exception level.** m1n1 stage 2 runs at
  EL2; Asahi Linux runs at EL2 with VHE. The installed m1n1 is v1.6.1 for both
  stages (`/proc/device-tree/chosen/asahi,m1n1-stage{1,2}-version`), which
  includes the ISP DAPF support E1 observed. **Confirmed** for versions;
  **inferred** that EL equality means privilege is not the barrier.
- **What m1n1 would write for AVE:** the *live* ADT's
  `filter-data-instance-0`. For ISP the live list has an injected TEXT slot
  (E1: `0x10000c68000 - 0x100015e7ffc`, r0 `0x11`) absent from the restore
  ADT. iBoot preloads AVE's firmware the same way (`segment-ranges`, docs/42
  §3.1), so the live `dart-ave0` list very probably carries AVE's TEXT entry
  too. **Inferred** from the ISP analogy; the live ADT is not visible from
  Linux.

## 3. Asahi Linux

- No commit in AsahiLinux/linux mentions DAPF (GitHub commit search
  "dapf repo:AsahiLinux/linux": 0 results). apple-dart has no DAPF code.
  **Confirmed.**
- apple-dart's reset (`apple_dart_hw_reset`) disables every stream's TCR,
  clears all TTBRs, then writes `ENABLED_STREAMS = 0xffffffff`, clears the
  error register and invalidates the TLB; attaching a domain then sets
  TRANSLATE. It recognises a *locked* t8020 DART by `CONFIG` bit 15
  (`DART_T8020_CONFIG_LOCK`). Our CONFIG read `0x1e010000` (bit 15 clear) and
  `DAPF_LOCK` (`0xf0` bit 0) read 0. **Confirmed.**
- No Asahi issue or commit discusses a DAPF write resetting or hanging a
  machine. Issue m1n1#647's tracer output shows t8110 DARTs advertise
  `SUPPORT_REG_LOCK`, so register locking is a real DART feature; no t8020
  equivalent beyond CONFIG.LOCK and DAPF_LOCK is documented. **Confirmed**
  (absence within what was searched), **unknown** beyond it.

## 4. macOS 13.5

- `AppleT6000DART` parses `filter-data-instance-%d` (`_apfSetupInstance`,
  `0xfffffe0009b13ca0`) and captures the DAPF before power-down
  (`_apfCaptureRegs`), but the kext has **no DAPF write routine**
  (symbol list of the 13.5 kext: `_setup`, `_dartSetup`, `_apfSetupInstance`,
  `_smmuSetup`, `_powerUp`, `_powerDown`, `_recoverFromPowerDown`,
  `_apfCaptureRegs`, `_dartPrepareForPowerDown`, `_smmuPrepareForPowerDown`).
  **Confirmed.**
- `_recoverFromPowerDown` calls `0xfffffe000855dd00` with `w1 = 0x6007`.
  That address is `bti c; b 0xfffffe00083f8a00`, and `0x83f8a00` is
  `mov x15, #0x35; b 0xfffffe00083ebe94` inside a table of identical
  `mov x15, #N; b ...` stubs: XNU's **PPL trampoline table**. PPL call `0x35`
  with an ioctl-style selector `0x6007`; the kernel carries the panic strings
  `pmap_iommu_init_internal`, `pmap_iommu_map_internal`,
  `pmap_iommu_ioctl_internal`. So DART/DAPF restore on macOS is done by PPL's
  IOMMU driver, via `pmap_iommu_ioctl`. **Confirmed** for the trampoline;
  **inferred** that `0x35` is `pmap_iommu_ioctl` (by elimination from the
  strings; the index table is not open source).
- The DART/DAPF MMIO pages are PPL-owned on macOS (`pmap-io-ranges`, wimg
  `0x4007`). That protection is **software** (PPL page tables and, where
  present, GXF-guarded I/O filters, XNU `osfmk/arm64/iofilter.c`); Linux does
  not run PPL, and ISP's DAPF is in the same class yet m1n1 writes it. So PPL
  ownership **does not explain** a hardware-level reset. **Inferred.**
- The PPL IOMMU driver (`osfmk/arm/pmap/pmap_iommu.c` is a 33-line stub in
  open source) is not published, and a pattern scan of the kernel for the
  r4/start/end/r0 write shape found only struct copies. **When and in what
  order macOS writes the AVE DAPF relative to TCR/TTBR/ENABLED_STREAMS is
  unknown.**

## 5. Hypotheses for the reset, ranked

| # | Hypothesis | For | Against | Rank |
|---|---|---|---|---|
| H-order | A DAPF may only be (re)programmed while its DART is not in operation — before TCR/`ENABLED_STREAMS` are set — and a write to a live one raises a fatal error | Every known-good DAPF write (m1n1 `kboot_boot`, m1n1 `aop.py`) precedes DART configuration; ours followed apple-dart's reset + `ENABLED_STREAMS = 0xffffffff` + TRANSLATE | No document states it; nothing forbids it either | **best supported** |
| H-first | The first register written, or the entry state, matters: we wrote `r0 = 0` first into a slot holding garbage `r0 0x200` (bits m1n1 never writes; m1n1 always writes r4 first and r0 last with values ≤ `0xff`) | E2/E3a garbage has high r0 bits in many slots; our order was new (added in 6561e6e) | m1n1 on ISP also meets unprogrammed state after gating, at least on a cold boot | plausible |
| H-lock | A write lock outside `DAPF_LOCK`/`CONFIG.LOCK` is set for AVE (e.g. by iBoot, which preloads AVE) | iBoot does prepare AVE (RVBAR locked, DATA literal patched) | The DAPF reads as unprogrammed garbage, not as an iBoot configuration; ISP, equally iBoot-preloaded, is writable by m1n1 | possible |
| H-priv | EL2 Linux may not write DAPF MMIO at all | macOS writes only from PPL | m1n1 writes DAPFs from EL2; PPL protection is software | unlikely |
| H-block | Not a DAPF | — | Apple's `pmap-io-ranges` tag, the ADT instance rule, `_apfCaptureRegs` | **refuted** |

One observation would sharpen all of these and costs nothing: **did the
machine reboot by itself, or freeze until powered off?** This kernel has
`kernel.panic = 0` and `panic_on_oops = 0`, so an SError from a faulting
write would panic and *freeze*; a spontaneous reboot points to a SoC-level
reset instead (a fatal fabric/security violation, or the 10-minute systemd
hardware watchdog if it froze long enough). **Unknown.**

## 6. Next steps (proposed, not run)

### N0 — free discriminators before any rerun

- Ask the operator: reboot-by-itself or freeze? (See §5.)
- Run the next attempt **from a text VT** (Ctrl+Alt+F3, logged in, command
  launched there). A panic trace then prints on the framebuffer console and
  names the faulting access and exception class (SError vs data abort), which
  no disk log can capture on this machine (no pstore backend). Photograph it.

### N1 — Linux, mirror m1n1's order (recommended first)

Risk: another reset or freeze; recovery is a reboot (use the sysrq
`s`/`u`/`b` rule in docs/48 if it freezes; nothing persists). No boot-chain
change.

Change (driver, behind a new parameter, default off), in `variant=3`, at
stage 8 before anything else touches the DART:

1. Save TCR[0..15], `ENABLED_STREAMS`.
2. Put the DART into m1n1's pre-DAPF state: every TCR = 0, then
   `ENABLED_STREAMS = 0` (leave TTBRs alone so apple-dart's table survives).
3. Program only the needed slots in **exactly m1n1's order** — r4, start,
   end, r0 — with no `r0 = 0` pre-clear, and never write r0 bits above
   `0xff`. Readback.
4. Restore `ENABLED_STREAMS` and TCRs to the saved values.

Reading: survives with readback verified → H-order and/or H-first explain the
reset and Linux can program the DAPF; proceed with E3b/E3c using this path.
Resets → H-order/H-first refuted together; go to N3.

Optional science afterwards (only if N1 survives): N1a = step 3 alone, with
the DART left live, to separate H-order from H-first.

### N2 — Linux, before apple-dart ever resets the DART

Same as N1 but on `variant=2` (apple-dart never binds), so the DART is in
its post-gating state, not apple-dart's. Less useful than N1: a survivor
cannot then be used for E3, because apple-dart cannot bind later without a
new overlay and a reboot. Only if N1's reset needs a second data point.

### N3 — m1n1 programs the AVE DAPF at boot (the proven path)

Change: a custom m1n1 stage 2 with `{"/arm-io/dart-ave0", 3}` added to
`dapf_entries[]` (optionally gated by a new m1n1 variable). m1n1 then
programs the **live** ADT's list — very probably including iBoot's AVE TEXT
entry — before Linux, exactly as it does ISP's. The E1 evidence that ISP's
DAPF survives gating says the entries would persist for Linux.

Testing without a second machine is not possible: the m1n1 proxy and
hypervisor need a USB host. So this means replacing `m1n1/boot.bin` on the
ESP (`nvme0n1p4`, PARTUUID `89a77cf4-32ba-4a03-8bca-db0f62925ca4`), which is
**a boot-chain change and the operator's decision**:

- Keep the original as `m1n1/boot.bin.orig` on the ESP.
- If Linux no longer boots (e.g. the DAPF write resets from m1n1 too), boot
  macOS from the startup picker (hold the power button), `diskutil mount
  disk0s4` (verify the identifier first), and copy `boot.bin.orig` back — or
  do the same from 1TR's Terminal. Nothing in the APFS stub or in macOS is
  touched, so the machine stays recoverable.
- m1n1 prints `dapf: Initialized /arm-io/dart-ave0` on the boot framebuffer,
  so success or a pre-Linux hang is visible.

### N4 — macOS-derived sequence

Needs an m1n1 hypervisor trace of macOS with a DART tracer on `dart-ave0`
while AVE starts (e.g. a VideoToolbox encode), which also needs a second
machine. It would show macOS's exact DAPF/TCR ordering and the live TEXT
entry. Not possible with this setup; recorded for completeness.

## 7. What remains unknown

- Reset vs freeze on the E3a attempts.
- Whether H-order or H-first is the real cause (N1 decides together, N1a
  separately).
- The live ADT's `dart-ave0` filter list (does iBoot inject AVE's TEXT
  entry?).
- macOS PPL's exact write order and whether it quiesces the DART first.
- The meaning of r0/r4 bits and of the garbage high r0 bits.

## N0 answered (operator, 2026-09-13)

**The machine reset by itself** during E3a attempt 3; the next thing on
screen was the Fedora boot logo. With `kernel.panic = 0` an SError or oops
would have frozen the machine with a panic on screen, so this was a
**SoC-level reset**, not a CPU exception Linux took. **Confirmed** by
observation. That favours a hardware watchdog/protection mechanism tripped
by the write over a synchronous abort, and makes the order/state hypotheses
(H-order, H-first) the ones N1 tests.

## N1 as implemented (commit after e24431c)

New `apple-ave` parameters in `ave_dapf.c`:

| parameter | default | meaning |
|---|---|---|
| `dapf_order` | `m1n1` | `m1n1`: ADT order from slot 0, only the needed slots, r4/start/end/r0, **no pre-clear**, other slots untouched. `clear16-resets-machine`: the sequence that reset the machine (all 16 slots, r0 = 0 first). |
| `dapf_quiesce` | 1 | around the writes: save all 16 TCRs and `ENABLED_STREAMS`, write TCRs 0, `ENABLED_STREAMS` 0, program, then restore both |

How the implementation differs from N1 as first written above (review of
59bfe35): it runs **after stage 12**, not at stage 8, so apple-dart has
already done `ave_fw_load()`'s `iommu_map` (stream commands, TLB
invalidation) and TTBR[0][0] is VALID during the writes (m1n1's AOP
experiment had TTBRs invalid). Both parameters default **on**. The quiesce
is meant to neutralise that state; if N1 still resets, "stage 8, TTBRs
untouched" is the remaining untested difference (N2). `clear16` is now only
selectable as `dapf_order=clear16-resets-machine`.

Every quiesce and write step is a `STEP` marker, so `step_ms=` plus
`tools/e3-run.sh` pins any reset to one register write.

Registers-only run (fresh boot; core not started):

```sh
sudo insmod test/ave-overlay.ko variant=3
tools/e3-run.sh n1 stop_after=12 step_ms=1500 dapf_dump=1 \
    dapf_set=control fw_map_data=1 fw_map_text=2
```

Outcomes: survives with "programmed and verified by readback" and "DART
restored" → the reset was order/state, and E3 can proceed in `m1n1` order
(the negative control then needs a separate clean-slot design); resets at
a quiesce marker → touching this DART's TCR/`ENABLED_STREAMS` is itself
fatal (unlikely: apple-dart writes both); resets at the first slot write
even quiesced and in m1n1 order → Linux cannot write this DAPF at runtime,
go to N3 (m1n1).

## N1 result — 2026-09-13, `results/n1-1789303951.kmsg`: reset before any DAPF write

Same completed steps as E3a attempt 3 (iBoot checks, DATA `iommu_map`
verified, 13.5 scratch writes, stage 12, DART/DAPF reads), then "m1n1 order
leaves 14 non-empty slots beyond 2", then the last line on disk:
`STEP next: quiesce DART - all TCRs 0 (saved TCR[0] 0x80)`. **The machine
reset** during that 1.5 s hold or at the first `writel(0, cpudart + 0x100)`.
No DAPF register was written.

What this changes:

- apple-dart writes this same DART's TCRs at probe (`apple_dart_hw_reset`)
  and issues stream commands / TLB invalidations to it during stages 9-10 of
  this very run, without incident. **Our** first write to the block has now
  reset the SoC twice, at two different registers (DAPF slot 0 r0, TCR[0]).
- Both resets came about 3.1 s after the pre-start scratch-write marker, and
  both immediately after a 1.5 s marker hold. The logs cannot distinguish
  "the write resets" from "something earlier armed a reset a few seconds
  later, which landed during the hold".

Hypotheses now:

- **H-write**: any write by this driver to the CPUDART/DAPF block resets
  the SoC (mechanism unknown - apple-dart's writes to the same physical
  block do not).
- **H-delay**: an earlier step (13.5 scratch writes with no running firmware,
  or the RW DART mapping of iBoot DATA) arms a SoC-level reset that fires a
  few seconds later, independent of any DART write.

Discriminating run (N1b), one boot at most: identical steps through stage
12, then **no DART writes** and a 30 s hold with a marker every 2 s; then a
single same-value write (`TCR[0] <- its current 0x80`) with a marker after
it and a 10 s hold. Reset during the hold → H-delay; reset right at the
same-value write → H-write (value-independent); survival → the value (TCR
0 / DAPF r0 0) matters.

## N1b result — 2026-09-13, `results/n1b-1789304276.kmsg`: it is not the writes

The 30 s no-write hold ran to completion: 16 markers, TCR[0] `0x80` and
ENABLED_STREAMS `0xffff` read every 2 s. After the `t=30s` line nothing more
reached disk; the next code was a 2 s sleep, a TCR read, and a log line - the
same-value write was still 2 s further on. The operator saw **the UI freeze,
then a reset about 30 s later**. **Confirmed:** the machine freezes with no
DART or DAPF write from us. No other kernel line (DART faults, warnings)
appears in any of the three captures.

This is the 2026-09-07 signature (docs/24): a fabric-level hang that stalls
the CPUs, followed by a PMU reset. The earlier "reset at the first write"
readings were an artefact of the markers: each write followed a hold, and
the freeze landed in or just after a hold.

Freeze times are not fixed (about 3 s after the scratch-write marker in
E3a/N1, about 33 s in N1b), so the trigger is asynchronous or armed earlier.
Comparing runs:

| run | overlay | reached | froze? |
|---|---|---|---|
| 26.6 handshake runs (09-08, 09-13 10:21) | variant 0 | scratch writes (IOVA values), core started, storm ~2 min | no |
| E2 (variant 2), twice | variant 2 | stage 8 | no |
| E3a attempt 1 (oops) | variant 3 | stage 10, inside the literal check; sat powered for minutes | no |
| E3a attempt 3, N1, N1b | variant 3 | **iBoot DATA RW `iommu_map`**, **13.5 scratch values**, stage 12 | **yes** |

So the suspects are **(a)** the RW DART mapping of iBoot's DATA at DVA
`0xec000`, and **(b)** the 13.5 pre-start scratch values (`scratch0
0x08042006, scratch1 0, scratch2 0xe`, with no firmware running). Neither
has an obvious mechanism.

N1c, split them, no DART writes, `HOLD=120` (driver left loaded, heartbeat
every 5 s):

1. `stop_after=10 fw_map_data=1 fw_map_text=2` — (a) without (b).
2. if 1 survives: `rmmod apple_ave`, then `stop_after=11 fw_map_text=2`
   (no `fw_map_data`) — (b) without (a).
3. if both survive: `stop_after=11 fw_map_data=1 fw_map_text=2` — both.

## N1c result — 2026-09-13: neither suspect freezes the machine

All three steps on one boot (overlay `variant=3`), each left loaded for
120 s with a userspace heartbeat every 5 s, then `rmmod` (VENC gated,
`venc_sys off-0`), no faults, no disabled IRQ:

| step | params | result |
|---|---|---|
| 1 `results/n1c1-*.kmsg` | `stop_after=10 fw_map_data=1 fw_map_text=2` | 24/24 heartbeats, survived |
| 2 `results/n1c2-*.kmsg` | `stop_after=11 fw_map_text=2` | 24/24, survived |
| 3 `results/n1c3-*.kmsg` | `stop_after=11 fw_map_data=1 fw_map_text=2` | 24/24, survived |

**Confirmed:** the iBoot DATA mapping and the 13.5 scratch values, alone
or together, do not freeze the machine in two minutes.

What the three freezing runs had that N1c did not:

- `dapf_dump=1` and/or `dapf_probe`/`dapf_set`: `devm_ioremap` of CPUDART
  and DAPF and reads of their registers (TCR/TTBR/REMAP/ERROR/`0xf8`/
  `ENABLED_STREAMS`, all 16 DAPF slots) **while apple-dart owns the DART**;
- `stop_after=12` with the probe still running (long marker holds inside
  probe) when the freeze came.

Against "the reads alone": E3a attempt 1 did the full stage-8 dump on
`variant=3` and then sat powered for minutes without freezing (its probe
had died at stage 10). So the leading remaining candidate is **those reads
combined with the later stages** (after the DATA map and scratch writes),
or a long-running probe. N1d: repeat N1c step 3 adding `dapf_dump=1`
(stage-8 reads only), `HOLD=120`; if it survives, N1e = N1b's configuration
with its 30 s read loop replaced by a no-read hold.

## N1d, N1e — 2026-09-13: survived; the remaining factor is DART/DAPF reads late in probe

| run | params | result |
|---|---|---|
| N1d `results/n1d-*.kmsg` | N1c step 3 + `dapf_dump=1` (one full DART/DAPF read dump at stage 8) | 120 s, 24/24 heartbeats, survived |
| N1e `results/n1e-*.kmsg` | N1b's config (`stop_after=12 dapf_dump=1 fw_map_data=1 fw_map_text=2`), 30 s in-probe hold with **no** register access (`dapf_probe=2`), then stage-12 snapshot | 120 s, survived |

Excluded, each **confirmed** by a surviving run: the iBoot DATA mapping, the
13.5 scratch values, a single stage-8 read dump, a long-running probe, the
stage-12 path and snapshot.

The only thing every freezing run did and no surviving run did is **read
CPUDART/DAPF registers again after stage 11**: E3a and N1 re-ran the full
dumps, the TCR check and the per-slot reads; N1b read TCR[0] and
ENABLED_STREAMS every 2 s and froze after ~16 such pairs. Freezes came
~1.5 s to ~32 s after those reads began.

Two readings remain: the reads themselves hang the fabric, either
cumulatively or at some low rate (surviving runs made ~100 reads, freezing
ones considerably more); or reads only hang once the later stages have run.
N1f discriminates directly: N1b without the write, reading TCR[0] and
ENABLED_STREAMS every 2 s for 120 s (`dapf_probe=3`).

## N1f — 2026-09-13, `results/n1f-*.kmsg`: late reads alone do not freeze it

N1b's configuration with its read loop extended to 120 s and no write
(`dapf_probe=3`): 61 TCR[0]/ENABLED_STREAMS read pairs, stage-12 snapshot,
30 s hold. **Survived.** N1b's freeze is not reproduced by its reads.

Every factor tested in isolation now survives, on one boot (N1c1-N1f, six
insmods, ~2 h uptime). The three freezes (E3a attempt 3, N1, N1b) were each
the **first AVE insmod of a fresh boot** that read CPUDART/DAPF after stage
11. The surviving runs that read CPUDART/DAPF late (N1d-N1f) were all later
insmods on a boot where VENC had already been powered up and gated at least
once. Counter-evidence: the E3a oops run was also a first insmod with the
stage-8 dump and did not freeze - but its probe died at stage 10, before any
late read.

**Inferred, weakly (3 freezes, small samples):** the hazard depends on the
first VENC power session after boot (apple-dart just probed; the DART and
DAPF first powered by us), not on any single operation we have isolated.

N1g: N1f's exact command (`dapf_probe=3`, no write) as the **first** AVE
insmod after a fresh boot with overlay `variant=3`. A freeze makes
first-session dependence the leading hypothesis; a survival makes the three
freezes look probabilistic, and the next step is repetition, not bisection.

## N1g, N1g2, N1h — 2026-09-13

- **N1g** (`results/n1g-*.kmsg`): N1f's command as the **first** AVE insmod of
  a fresh boot. 61 late read pairs, survived. First-session dependence is
  not supported.
- **N1g2** (`results/n1g2-*.kmsg`): repeated at kernel uptime 306-431 s, to
  span the ~310 s at which E3a attempt 3 and N1b froze. Survived. No systemd
  timer falls near 5 min after boot.
- A correction to how the captures are read: a fabric hang blocks **all**
  MMIO, NVMe included, so `tools/kmsg_capture.py` can read a line printed
  just before a hang but its write/fsync never completes. **The last line
  on disk precedes the fatal access by at least one fsync.**
- **N1h** (`results/n1h-*.kmsg`): N1b again (`dapf_probe=1`) with a 5 s
  sleep between printing "next: same-value write" and the write. The machine
  reset. The last line on disk is again `t=30s`; the announcement never
  landed despite the 5 s sleep after it. Same stopping point as N1b, twice.

So `dapf_probe=1` stops at the same point on two boots, while
`dapf_probe=3`, whose loop is identical and simply continues past 32 s,
survived 120 s three times. Between the `t=30s` line and the announcement
the code does only `msleep(2000)`, one `readl(TCR[0])` and the `dev_info` -
all of which the looping version does too. Either the announcement's fsync
was still pending when the write hung the bus (btrfs's 30 s commit interval
could make one fsync slow, and the loop ends ~32 s in), which would make the
write the trigger; or something about that point in the code is unexplained.

N1i removes the fsync ambiguity: after the announcement, wait 60 s printing
a line every 2 s **with no register access** (N1e showed such holds are
safe), then write, then print. Lines landing through the wait followed by a
reset pins it to the write; a reset during the wait pins it elsewhere.

## N1i — 2026-09-13, `results/n1i-*.kmsg`: the write hangs; the capture loses ~5 s

N1b's 30 s read loop, then a 60 s wait printing every 2 s with **no register
access**, then `PROBE N1i writing TCR[0] now`, `msleep(3000)`, the same-value
write. **The machine reset.** Every wait line through `t=60s` is on disk;
"writing now" is not.

Lining up the three same-value-write runs by how long before the write each
line was printed:

| run | last line on disk (printed before the write) | first lost line |
|---|---|---|
| N1b | `t=30s`, ~4 s | "next", ~2 s |
| N1h | `t=30s`, ~7 s | "next", ~5 s |
| N1i | `t=60s`, ~5 s | "writing now", ~3 s |

**Anything printed within about 5 s of the hang is lost from disk**, even
though the capture fsyncs each line. **Inferred:** the SoC reset discards
data the Apple NVMe controller has acknowledged but not yet persisted. This
retracts the earlier reading of N1b as "froze before any write": it wrote,
and the evidence of it fell in the loss window.

With that window every run is consistent:

- **all five runs that wrote to CPUDART or its DAPF hung the machine**
  (E3a attempt 3: DAPF slot 0 r0; N1: TCR[0] ← 0; N1b, N1h, N1i: TCR[0] ←
  its own value `0x80`);
- **all eight runs that did not write survived** (N1c1-3, N1d, N1e, N1f,
  N1g, N1g2), including 120 s of the same reads.

**Confirmed, to the capture's ~5 s resolution:** a write by this driver to
the AVE CPUDART register block - even a same-value TCR write - hangs the
fabric, in the state after stage 11 with apple-dart bound. apple-dart's own
writes to the same block (STREAM_COMMAND TLB invalidation during stages
9-10 of the same runs) did not. Why is unknown.

Next discriminators, each needing waits of 15 s or more around the write:

- **N1j**: the same-value TCR[0] write at **stage 8**, right after the
  read dump and before the DATA map or scratch writes (`variant=3`).
  Survives → the hazard depends on later stages or elapsed session time;
  hangs → any write of ours to the block hangs.
- **N1k** (if N1j hangs): the same write on `variant=2` (no apple-dart) at
  stage 8. Separates "apple-dart bound" from "any write at all".
- A static question for the reviewer: how our `devm_ioremap` mapping and
  `writel` differ from apple-dart's for the same register (mapping
  attributes, access width, the regulator/clock/PM state apple-dart holds).

## N1h2 on a text console — 2026-09-13: it is an SError, not a hang

Rerun of N1h (`dapf_probe=1`) by the operator on tty3 with
`dmesg -n 8`, so every kernel line printed on screen
(`results/n1h2-vt-*.kmsg`; screen photographed). The console's last driver
line: `[2557.213340] PROBE next: same-value write TCR[0] <- 0x80 (in 5 s)`.
Then drm_panic's screen: **"KERNEL PANIC! Please reboot your computer.
Asynchronous SError Interrupt"**, with a QR code.

The QR (version 35, no URL configured) was decoded from the photo with
zxing-cpp; it holds the plain-text tail of the panic log
(`results/n1h2-vt-panic-qr.txt`):

```
[2562.397362] Tainted: [S]=CPU_OUT_OF_SPEC, [M]=MACHINE_CHECK, [O]=OOT_MODULE, [E]=UNSIGNED_MODULE
  ...
  arm64_serror_panic+0x78/0x90
  arm64_is_fatal_ras_serror+0x8c/0x98
  do_serror+0x38/0x60
  el1h_64_error_handler+0x40/0x68
  el1h_64_error+0x84/0x88
  dev_driver_string+0x0/0x48 (P)
  _dev_info+0x6c/0xa0
  ave_dapf_write_probe+0x178/0x390 [apple_ave]
```

**Confirmed:**

- The same-value `writel(0x80, cpudart + 0x100)` at ~2562.21 (5 s after the
  announcement) raised an **asynchronous, fatal RAS SError**, taken ~180 ms
  later inside the `dev_info` that follows the write. Kernel tainted
  `MACHINE_CHECK`. The CPUDART register block **rejects the write with a
  bus error**; it does not hang.
- The "freeze, then reset ~30 s later" is the panic (`kernel.panic = 0`)
  followed by the Apple SoC watchdog, whose timeout is 30 s
  (`/sys/class/watchdog/watchdog0/timeout`). No PMU-level mechanism is
  needed to explain it.
- The ~5 s of missing lines in every capture is the persistence loss
  measured in N1i, now corroborated by the on-screen log.

What the rejection depends on: apple-dart rewrites this DART's TCRs on every
runtime resume (`apple_dart_resume` → `hw_reset` + restore), which happens at
the start of each of our power sessions and did so successfully in
N1c-N1g. Our writes fail only **late**, after stage 11. Stage 11 writes
`scratch0 = 0x08042006`, the IOP host-mode flag Apple writes in
`AVE_SVECtrl::SetIOPFlag` - and Apple's DART/DAPF restore happens at power-up,
before it. **Leading hypothesis (inferred):** setting the IOP flag (or
something between stage 8 and stage 12: IPC alloc, the DATA map) locks the
CPUDART/DAPF register block against host writes, so they must be done
before stage 11.

Next: program the DAPF **at stage 8**, before the IPC allocation, the DATA
map and the scratch writes (m1n1 order, no quiesce, readback). Survival both
supports the hypothesis and is the step E3 actually needs.

## N1j — 2026-09-13, `results/n1j-*.kmsg`: DAPF writes are rejected at stage 8 too

`stop_after=8 dapf_dump=1 dapf_early=1 dapf_set=control dapf_quiesce=0
fw_map_text=2`: DAPF programming at stage 8, before the IPC allocation, the
DATA map and the stage-11 IOP flag. **The machine reset.** Last line on disk
(allowing the ~5 s loss window): `STEP next: first write to DAPF slot 0 (r4,
m1n1 order)`. The IOP-flag hypothesis is **refuted** for DAPF writes.

Not a lock apple-dart applies: apple-dart's lock handling is for
bootloader-locked DARTs, detected from CONFIG bit 15 (clear here, "locked:
0"), and the PROTECT registers exist only on t8110. It engages nothing on
this t8020.

Remaining ordering difference: apple-dart's successful TCR writes happen at
runtime resume, immediately after power-on. Every rejected write of ours
came after **stage 7**, which writes `SVE+0x38 = 1` - Apple's
`AVE_SVECtrl::SetIdle(1)` from `AVE_PMGR::SetClockGating(true)`. Mapping
does not require TLB invalidation, so apple-dart may not write this DART at
all after stage 6. **Hypothesis:** stage 7 enables clock gating that makes
the DART/DAPF reject writes (reads still work).

N1k: `dapf_early=2` programs the DAPF at the end of stage 6, before the
stage-7 write, with `stop_after=6`.

## N1k — 2026-09-13, `results/n1k-*.kmsg`: rejected immediately after power-on

`stop_after=6 dapf_dump=1 dapf_early=2 dapf_set=control dapf_quiesce=0
fw_map_text=2`: DAPF programming at the end of stage 6, straight after
runtime resume and **before** stage 7's SVE idle write. **The machine
reset.** Last line on disk: `STEP next: first write to DAPF slot 0 (r4,
m1n1 order)`. The stage-7 hypothesis is refuted.

**Confirmed across N1j/N1k/E3a: Linux cannot write the AVE DAPF at any point
of a power session** - right after power-on, at stage 8, or late; m1n1 order
or with a pre-clear; quiesced DART or not. Every first write raises a fatal
SError. Reads always work.

Why m1n1's DAPF writes at boot succeed and ours do not is unexplained. Ruled
out on the m1n1 side (source, `src/dapf.c`, `src/kboot.c`):

- power: `dapf_init()` enables the DART node's `clock-gates` device first,
  but for `dart-ave0` that is `VENC-DART` (508, `0x1fc`), a `psreg == 0`
  virtual device (docs/25) whose parent `VENC_SYS` we already hold on;
- access width: m1n1 uses `write32`/`write64`, as we do;
- a later lock by m1n1: `kboot_boot()` calls `dapf_init_all()` late, and
  the only lock m1n1 applies is `dart_lock_adt("/arm-io/dart-disp0")`.

Two ways forward:

- **ISP control (no boot-chain change):** a same-value write to one of
  ISP's live DAPF slots from Linux, camera streaming. SError → DAPF writes
  are refused from Linux for every DART, i.e. something between m1n1's
  programming and the running kernel locks them, and only m1n1 can do it
  (N3). Survives → the refusal is AVE-specific, and the difference is
  something ISP has and AVE lacks.
- **N3:** add `{"/arm-io/dart-ave0", 3}` to m1n1's `dapf_entries[]`
  (boot-chain change; recovery via macOS/1TR).
