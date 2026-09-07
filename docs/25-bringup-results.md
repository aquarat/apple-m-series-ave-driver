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

## Remaining hypotheses, untested

1. **Incomplete power.** `venc_me1` has **no phandle** in the base device tree,
   so it cannot be referenced from an overlay, and it remains `off` while all
   its ancestors are `on`. A partially-powered block is a plausible cause of a
   fabric hang, and this is the one thing we have not been able to control.
   Testing it needs either a base-DT change or `of_find_node_by_path` plumbing
   in the driver.
2. **Fabric / AXI2AF configuration.** The pmgr carries an `axi2af-axi-config`
   property and AVE has an `AVE_AXI2AF` class with `ApplyTunables`, `SetParity`
   and `CheckIdle`. If the interconnect path to the block is unconfigured,
   nothing on it will respond.
3. **A tunables sequence.** `AVE_DPE::ApplyTunables`, `AVE_AXI2AF::ApplyTunables`
   and `AVE_RegCfg_Apply` all exist and all run before normal operation. Apple
   may be applying a register-config blob we know nothing about.

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
