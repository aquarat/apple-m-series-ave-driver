# The overlay reset: three silent machine deaths after `ave-overlay variant=4`

Prompted by `results/f6-1789395385.kmsg`, `results/f6b-1789397677.kmsg` and
`results/f6c-1789483860.kmsg` (f6c run 2026-09-15 15:51, by the operator's
agent on explicit instruction, faithful replication of f6b at commit `9ab1894`;
modules rebuilt for `vrr3`, no source changes).

Evidence labels per [00](00-methodology.md): **[C]** read directly from a log,
journal or live sysfs, **[I]** inferred (chain stated), **[U]** unknown.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| What does f6c add? | f6b's reset reproduces exactly: death ~31 s after the overlay, driver never loaded | C |
| Where exactly did f6b/f6c die? | Between the `tee` writing the **load-1 marker** and the `sudo insmod driver/apple-ave.ko` audit record — a sub-second window containing only `sync` | C |
| Why does the on-disk log end at `t=25s`? | The load-1 marker was written to page cache but never flushed; the marker's `sudo tee` is the dead boot's **last journal record** | C |
| Is the death tied to a script step or a 30 s timer? | No: F6 died ~10 s after a hand-applied overlay, during runner startup (`sudo dmesg` was the last journal record) | C |
| Kernel change (vrr1→vrr3)? | Dead: F3, F4, F5, F6, f6b and f6c **all** ran `7.1.13-401.asahi.vrr3` | C |
| Overlay change? | Dead: `test/` unchanged since `0b45961` (F4's commit) | C |
| Is overlay-on-a-never-driven-boot deterministically lethal? | No: F4's boot sat **9m24s** in overlay-only state (13:02:46→13:12:10) and survived | C |
| F4's "reset within ~2 s" | The f4 log is truncated at `unload 1 returned rc=0`; load 2 never ran — death in halt-run's `sleep 2` gap, 0–2 s after `remove()` powered the VENC domains down | C |
| Do the deaths log anything? | No panic, no SError, no oops, no shutdown records; auditd/journald write normally one second, nothing the next | C |
| Do apple-dart devices runtime-suspend? | Yes — several DARTs sit `suspended` in sysfs on a normal boot | C |
| Leading hypothesis | The AVE DARTs' runtime-PM suspend after probe power-gates `venc_sys` (their `power-domains = <0x1d>`), an ON→OFF cycle of an island whose state iBoot/m1n1 set up; the machine dies seconds later | I |
| Counter-evidence to that | F4's boot survived the same nominal state for 9m24s — the gating either did not happen there or is intermittently survivable | C/U |
| Status of the F6 fixes (`session_lsb`, `session_sve_ungate`) | **Still untested** — every attempt since F5 has died before load 1 | C |

---

## 1. The three deaths, precisely

Death points recovered from the dead boots' journals (`journalctl -b -N`),
which outlive the reset. Kernel-time correlation via the audit trail.

| run | boot span | overlay applied | last journal record | death after overlay | driver |
|---|---|---|---|---|---|
| F6 | Sep 14 15:10→15:16:25 | 15:16:15 (by hand) | 15:16:25 — the runner's opening `sudo dmesg` | ~10 s | never loaded |
| f6b | Sep 14 15:17→15:55:09 | 15:54:38 (script) | 15:55:09 — a sudo session close, by position the load-1 marker `tee` [I] | ~31 s | never loaded |
| f6c | Sep 15 ~11:16→15:51:31 | 15:51:01 (script) | 15:51:31 — `COMMAND=/usr/sbin/tee -a results/f6c-…kmsg` (the load-1 marker), full audit session including close | ~31 s | never loaded |

**[C]** for every timestamp. The signature in all three: auditd and journald
are writing normally up to the last second (sudo PAM accounting, session
open/close all complete), then nothing — no kernel panic, no SError, no oops,
no systemd shutdown sequence. An abrupt, silent, system-wide death while
userspace is fully alive.

For f6b and f6c the last record is the **load-1 marker's `tee`** — the script's
transition out of the 30 s overlay wait. The next action would be
`sudo insmod driver/apple-ave.ko …`, which never produced its audit record:
the machine died inside a sub-second window whose only content is `sync`.
**[C]** for f6c (COMMAND line inspected), **[I]** for f6b (position only).

The on-disk logs ending at `t=25s of 30s` are an artifact: the load-1 marker
reached page cache but was never flushed. Do not read "t=25s" as the death
point. **[C]** (journal audit record postdates the last on-disk marker).

## 2. The survivors, precisely

Same kernel, same overlay, same machine.

| boot | overlay | driver | outcome |
|---|---|---|---|
| F3-day (Sep 14 ~10:xx→12:59:00) | 11:05:54 | probe at 11:05:57 — **3 s** gap; then many load/unload cycles through F3 | orderly shutdown 12:59:00 |
| F4's (12:59→13:12:17) | 13:02:46 | load 1 probe at 13:12:10 — **9m24s** overlay-only, never-driven, survived | died 0–2 s after load 1's `remove()` (§3) |
| F5's (14:09→15:09:39) | 14:21:33 | probe at 14:21:45 — 12 s gap; **module never unloaded** (heartbeat still printing 15:09:38) | orderly shutdown 15:09:39 |
| post-F4 idle (13:12:17→14:08:57) | none | none | orderly shutdown after 57 min idle |

**[C]** throughout. Consequences:

- The overlay-only, never-driven state survived ≥ 9.4 min once (F4's boot) —
  the deaths are **not deterministic**.
- The two boots that survived with the driver loaded both bound it **within
  12 s** of the overlay (3 s and 12 s). Every attempt that waited 30 s died.
  F6 died at ~10 s — inside that same window — so "bind fast enough" may be a
  race the operator has been winning unknowingly, not a safety property.
  **[I]**.
- F5's boot never entered the overlay-only state after a session (the module
  stayed loaded until shutdown), so **nothing can be claimed from F5 about
  post-run overlay-only survival**. Corrects the tempting reading of docs/31's
  F5 entry.

## 3. F4's death, re-dated

`results/f4-1789387926.kmsg` ends at `=== unload 1 returned rc=0 ===` with no
load-2 markers. **[C]**. halt-run.sh's next actions are `sleep 2` then the
load-2 insmod. So the machine died in that 2 s gap — 0–2 s after load 1's
`ave_remove()` halted the firmware and powered the VENC domains down, on a
session whose Process had left the SMMU storming IRQ 127 (docs/31's F4 entry).
docs/31's "Halt and unload were clean, then the machine reset within ~2 s
(cause unknown)" is this gap; **load 2 never ran.** **[C]**.

The same boot had already survived 9m24s of overlay-only before the run. So
F4's death correlates with the `remove()` power-off transition, not with the
overlay's presence. **[I]**.

## 4. What the deaths are not

- **Not the kernel version** — every boot from F3 through f6c ran
  `7.1.13-401.asahi.vrr3`. **[C]** (`journalctl -b -N` "Linux version" per boot).
- **Not the overlay build** — `test/` last changed in `0b45961`, before F4.
  **[C]** (git log).
- **Not the driver** — it never loaded in three of the four deaths. **[C]**.
- **Not a fixed timer or a script step** — 10 s (mid-runner-startup, only
  `sudo dmesg` running) vs ~31 s (mid-wait). **[C]**.
- **Not a clean reboot** — no shutdown records; journald ends mid-write-cycle.
  **[C]**.
- **Not idle-machine instability** — this machine has hundreds of idle hours
  on overlay-less boots (including the 23 h 51 min before the f6c run) with no
  death. **[I]**, but three-for-three within tens of seconds of the overlay is
  not coincidence-class evidence.

## 5. Power-domain baseline and the leading hypothesis

Live, read-only, on the current boot (no overlay ever applied):

- `pm_genpd_summary`: `venc_sys` and every `venc_*`/`venc1_*` domain `off-0`;
  `pmp-afnc4-ioa` `off-0`; `afnc4_ioa` **on**. `PM: genpd: Disabling unused
  power domains` runs at boot. **[C]**
- This corrects the 2026-09-13 observation (docs/31) that the patched m1n1
  leaves `venc_sys` on with nothing gating it: on this boot genpd's boot-time
  cleanup leaves it **off**. Whether the death-boot had it on or off at
  overlay time is **[U]** (no surviving sysfs).
- apple-dart devices use runtime PM: on this normal boot several DARTs sit
  `suspended` (`…/power/runtime_status`), e.g. the ISP ones. **[C]**

The overlay's DART nodes carry `power-domains = <0x1d>` (`venc_sys`). So a
fresh overlay application implies, in sequence: island powered ON for the DART
probes (they read real registers, so it must be on), `apple_dart_hw_reset` on
both instances, iommu group 16 formed, and then — seconds later, timing set by
runtime PM — the DARTs suspend and genpd gates `venc_sys` **off** again: the
first Linux-driven ON→OFF cycle of an island whose prior state belongs to
iBoot/m1n1 (DAPF entries live inside it, docs/55 §10). **[I]** for the
sequence, **[C]** for each ingredient.

Why that would kill, when gating ISP DARTs' domains is routine: **[U]**. The
candidates are (a) a teardown-ordering fault — a DART register access racing
the island gate — which would be naturally intermittent; (b) an island
collapse while some part of the VENC fabric is not quiescent in the way Linux
assumes. F4's death fits the same shape one level over: `remove()`'s explicit
power-off, 0–2 s before death. **[I]**.

The counterexample is F4's boot's 9.4-min overlay-only survival: either the
DARTs never suspended there (something held them active — **[U]**), or the
gate happened and was survivable, or the mechanism is different entirely.

## 6. Ranked experiments (proposals; AGENTS.md applies)

1. **Fast-bind, then the F6 fixes.** Apply overlay variant=4 and `insmod
   driver/apple-ave.ko … session_config_only=1 fw_halt=1` within ~3 s (the
   F3-day pattern), hold 60 s, then run the F6b parameters
   (`session_lsb=1`, and `+session_sve_ungate=1` as load 2) with the driver
   already loaded rather than behind a 30 s wait. Two boots survived exactly
   this shape. If it survives to a PIPE HANG result, the project unblocks and
   the death becomes a boot-time curiosity. Discriminates nothing by itself —
   it is the unblocking move.
2. **Watch run (read-only, expects a 4th reset).** Overlay variant=4, no
   driver; from a second SSH session, every 2 s with fsync: full
   `pm_genpd_summary`, `/sys/bus/platform/devices/40d0{3,4}0000.iommu/power/`
   runtime state, `dmesg` tail. The death then leaves the last-observed state
   on disk and the transition (if any) is named. This is the experiment that
   converts §5 from hypothesis to fact or kills it.
3. **Variant=3 bisect.** Overlay variant=3 (single DART; every Sep-13 boot)
   + 60 s idle wait. If v3 survives where v4 died, the second DART node
   (`40d030000`, the bypass-less datapath DART) is implicated; if v3 also
   dies, the CPUDART attach alone suffices.
4. **cpuidle test.** Boot with `cpuidle.off=1`, overlay variant=4, 60 s wait.
   Tests the deep-idle island-gating variant of §5.

## 7. Corrections to existing documents

1. docs/31 F4 entry — add: the f4 log truncates at `unload 1 returned rc=0`;
   load 2 never ran; the boot's overlay-only window was 9m24s (13:02:46→
   13:12:10) and survived; the reset followed `remove()`'s power-off, not the
   overlay's presence.
2. docs/31 F5 entry / machine-state notes — F5's boot never unloaded the
   driver (last heartbeat 15:09:38, shutdown 15:09:39); no post-run
   overlay-only survival can be claimed from it.
3. docs/31 2026-09-13 20:37 — "the patched m1n1 leaves VENC_SYS on … nothing
   gates it" does not hold on the 2026-09-15 boot: `venc_sys` is `off-0` from
   boot-time genpd cleanup.

## 8. Reproduce (all read-only)

```sh
journalctl --list-boots                       # boot index map
journalctl -b -1 -n 20                        # the f6c boot's last records
journalctl -b -1 --since "15:51:01" | grep kernel   # silence after the overlay
journalctl -b -3 | grep -m2 "apple-dart 40d0"       # F6's overlay time
journalctl -b -4 | grep -E "ave-overlay: applied|apple-ave.*probe: staged"  # F5 gaps
sudo cat /sys/kernel/debug/pm_genpd/pm_genpd_summary | grep -E "venc|afnc"
for d in $(ls /sys/bus/platform/devices/ | grep iommu); do
  echo "$d: $(cat /sys/bus/platform/devices/$d/power/runtime_status)"; done
```
