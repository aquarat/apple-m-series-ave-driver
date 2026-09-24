# Rules for automated agents working in this repository

**This repository can hard-hang the machine it is checked out on.**

An M1 Max was reset six times during this project's bring-up work, and once by
a subagent that was doing pure static analysis and decided to validate a
finding on hardware. Treat the following as binding.

## Never run these

- `tools/bringup.sh` — loads kernel modules that hang the machine. It now
  refuses to run without `AVE_I_MEAN_IT=1`; **do not set that variable.**
  `tools/bringup.sh --status` is safe and touches nothing.
- `insmod` / `modprobe` / `rmmod` of anything in `driver/` or `test/`.
- Anything that applies a device-tree overlay.
- Any write to `/dev/mem`, `/sys/kernel/debug/...`, or PMGR registers.

## Safe

- Reading and disassembling `data/blobs/*` — this is the main work.
- `make -C driver` / `make -C test` — compiling is fine; **loading is not.**
- Reading `/proc/device-tree`, `/sys/kernel/debug/pm_genpd/pm_genpd_summary`,
  `journalctl`, and anything else read-only.
- Fetching from the network.

## Why the rule is here and not only in the prompt

A safeguard that relies on every prompt remembering to state it is not a
safeguard. If you are writing a prompt for another agent, still say it — but
the interlock in `tools/bringup.sh` is what actually enforces it.

## Hardware experiments

Only the operator, or the lead agent the operator has delegated them to
(next section), runs those, deliberately, having read
`docs/25-bringup-results.md` and `docs/24-incident-2026-09-07.md`. Any
other agent whose analysis suggests an experiment **writes it down as a
proposal**, and does not perform it.

## Operating the hardware (2026-09-24)

The operator runs this project from a separate host and has delegated
hardware runs **and reboots** to the lead agent on that host, working over
SSH into the target. That delegation is to the lead agent only. **Subagents
never touch the target**: they do static analysis on the host, and their
proposals come back as proposals. A subagent on the target would die with
it anyway.

Machine names, addresses and MACs are deliberately not in this repository.
They live in two git-ignored files at the repo root on the host:
`lab.local.md` (for people) and `lab.env` (sourced by the scripts). If they
are missing, ask the operator.

### The rules

- **One load per boot, no unload.** The teardown (Stop, Close, Halt, unmap,
  gate; docs/63) works, but a clean unload was once followed by a reset
  (s3-9). Reboot between runs, survive or die. The driver has no
  `.shutdown`, so `systemctl reboot` does not run the teardown.
- **Local logs on the target lose their tail on a hard reset**, `fsync` or
  not. Only the netconsole receiver on the host says where a run died, and
  only the wired receiver has been shown to keep the tail: the old Wi‑Fi
  path lost the last ~8 ms of every f33-f36 run (docs/53, f38).
- **One variable per run, and repeat before believing.** Record every run
  in docs/53: what it is for, and what a "no" looks like.
- **The step-hold parameter is `step_ms`.** `ave_step_ms=` is an unknown
  parameter, ignored with a kernel warning; it was ignored in f32-f38.
- Every new register read must be shown to lie inside a real block, with a
  citation. A read of AP `0x40D348000`, which is in no block, was an SError
  and a reset (f38).

### How a run goes

```
tools/lab-reboot.sh                      # reboot; waits for the boot marker and SSH
tools/lab-run.sh NAME "OVERLAY=4 OVERLAY_WAIT=0 HOLD=5" <module params>
```

`lab-run.sh` refuses if an AVE module is already loaded or an IRQ was
disabled. It launches `tools/e3-run.sh` detached on the target, waits on the
receiver (not the target) for the end marker or silence, and after a death
waits for the target to come back. Then it copies `results/` and writes the
run's slice of the receiver log next to the receiver's log file. The
target's netconsole service starts on every boot by itself. The receiver
runs `tools/nc-receiver.py` as a service on the host.

### How this project has gone wrong, so you do not repeat it

The pattern, many times over: a confident conclusion from a count with no
control, later refuted (all in docs/53).

- A `u8` tally wrapped and reported 1100 distinct values of a byte (F17).
- "IntraEst never ran" from counters that a *running* stage also zeroes
  (F21; the control was in the same log line).
- "Deterministic regression" from local logs that stop where storage
  persisted, not where execution stopped (f25-f30).
- "Died in the colocated scan" (f33-f36) from a receiver that dropped the
  tail, plus a step-hold parameter that was never applied. The killer was a
  misaddressed diagnostic read (f38).

The cure each time: what does a *known-good* case read? What does the null
hypothesis predict? Repeat the identical run before changing a variable.
`tools/check_frame.py` refuses to grade until its own controls pass; hold
everything to that standard. And **read what you already have before
spending a reboot**: the QP was in the bitstream all along.

### Tools

| tool | what |
|---|---|
| `tools/lab-run.sh`, `tools/lab-reboot.sh` | one run / one reboot from the host, end to end |
| `tools/e3-run.sh` | on the target: one load, capture, overlay, netconsole, debugfs copy, optional unload |
| `tools/nc-receiver.py` | the netconsole receiver: raw log and an arrival-timestamped log |
| `tools/check_frame.py <results dir>` | decode the frame, grade it against the source, with controls |
| `tools/h264_parse.py <frame.h264>` | SPS/PPS/slice headers: geometry, QP, slice type |
| `tools/disas.py` | `AVE_MACOS=13.5 python3 tools/disas.py --kext\|--fw ...`. **The env var is mandatory** |
| `tools/modedec_costs.py` | models the ModeDec/IntraEst configuration registers from the blob (docs/73) |
| `tools/fetch_userspace.py` | range-reads macOS 13.5's user-space encoder out of the IPSW (docs/72) |
| `tools/abi_selftest/`, `tools/session_selftest/` | `make && ./abi_selftest`: 856 and 209 checks. Both Makefiles delete the binary before rebuilding, because a stale binary once passed three times in one day |

Module parameters worth knowing: `session_costs` (post-frame register
groups, default 0, each behind a step marker), `session_flat_luma`,
`session_qp`, `session_frames`, `session_coded_kb`, `session_dbg`
(firmware print gate, wire `0xFCD8`), `session_ipcm` (I_PCM in I slices,
wire `0xFCE4`), `session_lambda` (macOS's λ block), `step_ms`, and
`core_reset=2 fw_restore_data=1` (recover a halted core; **never**
automatic, because a cold core reads STOPPED too, s2-9).

### Things that are decided

- DMA mask **32 bits** (docs/71 §6).
- Overlay **variant=4**: streams 0 and 1 on both DARTs, stream 15 off
  (docs/71). variant=5 exists for experiments.
- Interface: V4L2 stateful M2M, NV12 in, H.264 out (docs/68). Not started.
- Teardown order: Stop, Close, Halt, unmap while powered, gate last
  (docs/63). Works; not yet trusted (see the rules).
- Multi-frame, P-frames, LowResResult, rate control and exact bitstream
  assembly are implemented (docs/64-67) and untested beyond frame 0.
