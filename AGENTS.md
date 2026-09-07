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

Only the human operator runs those, deliberately, having read
`docs/25-bringup-results.md` and `docs/24-incident-2026-09-07.md`. If your
analysis suggests an experiment, **write it down as a proposal**; do not
perform it.
