# Hardware bring-up results, 2026-09-07

Five attempts on an M1 Max (j314c), Fedora Asahi 7.1.6-400, with no serial
console. Attribution came from a marker file synced before each attempt; the
mechanism worked every time.

## Results

| Stage | Action | Result |
|---:|---|---|
| 1 | `ioremap` all five banks | **pass** |
| 2 | `dma_set_mask_and_coherent(42)` | **pass** |
| 3 | `platform_get_irq` (AIC 1031 → Linux irq 129) | **pass** |
| 4 | `devm_request_irq` | **pass** |
| 5 | attach 5 power domains, `PD_FLAG_ATTACH_POWER_ON` | **pass** |
| 6 | `pm_runtime_resume_and_get`, touch nothing | **pass** |
| 7a | read bank 2 (SVE) `+0x10` | **HANG** |
| 7b | read bank 1 (ASC) `+0x400048` | **HANG** |
| 7c | `reset_control_reset()` on `venc_sys` | **HANG** |

Every hang has the same signature: PMU reports `1 boot error, 0 panics`, no
oops, no stack trace, nothing in pstore. That is an SoC-level fabric hang reset
by the PMU watchdog, not a software fault.

## What is established

**Power management works.** After stage 5, `pm_genpd_summary` reports
`venc_sys`, `venc_dma`, `venc_pipe4`, `venc_pipe5` and `venc_me0` all `on`.
Stage 6 resumes the device and survives. So the domains, the DT references and
`apple-pmgr-pwrstate` are all doing their job.

**Everything up to and including power is repeatable and safe.** Stages 1–6 were
run several times across boots with no incident.

**Anything beyond power hangs, regardless of what it is.** Two different banks
and a PMGR-side reset all produce the identical failure. Note 7c is the
interesting one: `reset_control_reset()` writes *PMGR* registers, not AVE
registers, and it hung too. Whatever the gate is, it is not simply "AVE
registers are dead" — the block is in a state where interacting with it at all
wedges the fabric.

## Hypotheses eliminated

- **Unpowered block.** `PD_FLAG_ATTACH_POWER_ON` fixed the power state and
  genpd confirms it; the hang persists.
- **Wrong power domain for the DART.** `VENC-DART` (508) is a `psreg == 0`
  pseudo-device whose parent is `VENC_SYS` (294) — Linux's `venc_sys`. The
  reference was right.
- **A PMGR bridge gating the range.** No entry in the pmgr `device-bridges`
  list contains any VENC device.
- **The unmodelled `psreg == 0` pseudo-devices are required.** Decisive
  counter-example: `avd0`'s ADT gate list is *entirely* pseudo-devices, and
  Linux drives AVD successfully with one real domain and none of them.
- **Wrong register chosen.** A scan of all 169 `Read32`/`Write32` sites showed
  Apple's only unconditional accesses are bank 1 (`AVE_IOP_Start_*`) and the
  bank 2 `AVE_SVECtrl` methods. Both were tried; both hang.
- **Missing reset pulse.** `apple-avd` — a working video block on this same SoC
  — calls `reset_control_reset()`, and our node lacked `resets`. Adding it and
  pulsing also hangs.

## Further eliminations from static analysis (2026-09-07, after the reboots)

- **The `venc_sys` mapping is now proven, not assumed.** ADT `ps-regs[10]` is
  window 2 (base `0x8e580000`) at offset `0x300`; `VENC_SYS` has `psidx = 22`,
  so its PS register is `0x8e580000 + 0x300 + 22*8 = 0x8e5803b0`. Adding the
  `0x2_00000000` IO base that m1n1 applies gives `0x28e5803b0` — exactly
  Linux's `power-management@28e580000/power-controller@3b0`. We are powering
  the right device.

- **AXI2AF cannot be the missing enabler.** `AVE_AXI2AF::ReadReg`
  (`0xfffffe0008b5c9e8`) does `mov w1, #0x4` — it operates on **bank 4**, which
  is AVE's own address space. Configuring the fabric would require touching the
  very space that hangs, so it cannot be a precondition for that space
  responding. Hypothesis 2 below is therefore dead.

- **`AppleT6000` does reference VENC, but not usefully.** The strings are
  `"VENC0 RD"`, `"VENC0 WR"`, `"VENC0 DCS RD"`, `"VENC0 DCS WR"`, `"VENC1 RD"`,
  `"VENC1 WR"` — fabric **agent labels** used for error and performance
  counting, alongside `AppleT6000PlatformErrorHandler::_fabricCommands`. They
  confirm VENC is a distinct fabric agent (so our hang is most likely a fabric
  timeout on that agent) but they are not an enable sequence.

  *Method note:* the first version of this search claimed AppleT6000
  "references VENC0/VENC1" using a loose regex plus nearest-preceding-fileset
  attribution. Both were wrong — a re-run with NUL-terminated matching found
  zero hits, and proper segment attribution was needed to find the real
  strings. Trap 3 again.

**The important structural conclusion:** every register the AVE kext touches is
inside AVE's own address space (banks 0-4). Nothing AppleAVE2 does can be the
thing that makes that space respond. The enabler is therefore either the power
domains — which we now know we drive correctly — or something at platform level
outside this kext entirely.

## Remaining hypotheses, untested

1. **Incomplete power.** `venc_me1` has **no phandle** in the base device tree,
   so it cannot be referenced from an overlay, and it remains `off` while all
   its ancestors are `on`. A partially-powered block is a plausible cause of a
   fabric hang, and this is the one thing we have not been able to control.
   Testing it needs either a base-DT change or `of_find_node_by_path` plumbing
   in the driver.
2. ~~**Fabric / AXI2AF configuration.**~~ **Eliminated** — `AVE_AXI2AF`
   operates on bank 4, inside AVE address space. See above.
3. ~~**A tunables sequence inside the kext.**~~ Largely eliminated by the same
   argument: `AVE_DPE` is bank 0 and `AVE_AXI2AF` is bank 4, both inside the
   space that hangs.

4. **A difference between Asahi's `apple-pmgr-pwrstate` and macOS's PS
   handling.** Both write the same register, but macOS may write a different
   value, or perform additional steps (a second PS field, a settle poll, a
   related device). genpd reporting "on" means Asahi's driver read back what it
   expected — not necessarily that the block is fully enabled. **This is the
   most promising remaining avenue and it is pure static work**: compare
   `ApplePMGR`/`AppleARMIODevice`'s device power-up against the Asahi driver.

## PMGR register dump (read-only, no crash)

`test/psdump` reads the raw PMGR power-state registers and decodes them against
Asahi's `pmgr-pwrstate` field definitions. It touches only PMGR, never AVE
space, so it is safe. With the AVE domains **off** and AVD active as a control:

```
avd_sys   (WORKS) = 0x1f0003ff  target=f actual=f WAS_PWRGATED WAS_CLKGATED ps_auto=f
venc_sys          = 0x0f000300  target=0 actual=0 WAS_PWRGATED WAS_CLKGATED ps_auto=f
venc_dma          = 0x00000300  target=0 actual=0 WAS_PWRGATED WAS_CLKGATED ps_auto=0
venc_pipe4/5,
venc_me0/me1      = 0x00000300  (identical)
```

Asahi's driver writes `PS_TARGET` (bits 0-3), polls `PS_ACTUAL` (4-7), and
touches nothing else — leaving `DEV_DISABLE` (bit 10), `RESET` (bit 31),
`PS_MIN` (16-19) and `PS_AUTO` (24-27) alone. The hypothesis was that iBoot had
left `DEV_DISABLE` or `RESET` set on VENC, which would let genpd report "on"
while the block stayed dead.

**It has not.** Neither bit is set on any VENC domain. `WAS_PWRGATED` and
`WAS_CLKGATED` (`0x300`) are history flags, set on AVD too. The only difference
is `AUTO_ENABLE`/`PS_AUTO`, and that is explained by `avd_sys` being powered on
while the VENC domains are off — Asahi sets auto-enable after a successful
transition.

So the PMGR state is unremarkable and this hypothesis is eliminated, at the
cost of no reboots. The obvious follow-up — re-dumping with the VENC domains
powered on, to compare like with like — has not been done.

## Recommendation

**Stop hardware attempts here.** The information yield per reboot has dropped
sharply: the last three all failed at the same conceptual point by different
mechanisms, and every remaining hypothesis needs *observation* rather than
another binary guess. Bisection has taken this as far as it goes.

The m1n1 hypervisor console resolves all three remaining hypotheses directly, by
tracing what macOS itself does to this block before its registers become live.
It is the right next step and is worth waiting for.

Static analysis remains productive and unblocked in the meantime — the command
struct interiors, the `pPicParams` field join and the HEVC paths are all still
open and need no hardware.
