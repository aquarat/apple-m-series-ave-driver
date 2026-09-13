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
| `dapf_order` | `m1n1` | `m1n1`: ADT order from slot 0, only the needed slots, r4/start/end/r0, **no pre-clear**, other slots untouched. `clear16`: the sequence that reset the machine (all 16 slots, r0 = 0 first). |
| `dapf_quiesce` | 1 | around the writes: save all 16 TCRs and `ENABLED_STREAMS`, write TCRs 0, `ENABLED_STREAMS` 0, program, then restore both |

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
