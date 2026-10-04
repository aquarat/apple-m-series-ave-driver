# tools/fwemu

Run the AVE firmware's own code under Unicorn from a snapshot of its live
memory, and log every register it programs (docs/93).

| file | where | what |
|---|---|---|
| `snap-target.py` | target | copy the encoder's device address space during a `snap_hold_frame` hold (driver `dva_debugfs=1`) |
| `mmio-target.py` | target | read back a list of registers the firmware wrote |
| `fwmem.py` | host | the firmware's address space from a snapshot (its own page tables) |
| `locate.py` | host | the `CAVCController` and a frame's `sCmdInformation` in a snapshot |
| `emu.py` | host | call a firmware function, log MMIO (needs `pip install unicorn`) |
| `diff.py` | host | compare two emulations' final register values |
| `synth.py` | host | call a firmware function on synthetic (zeroed) state, no snapshot: compare builds or settings (docs/89 §9.4; preset `avc-setpipe` for H13G/H13S) |
| `fwsyms.py` | host | function symbols and sizes from the firmware Mach-O |

`mp/` holds the multipass tools of docs/95: `fd.py` (firmware disassembly by
symbol), `ua.py`/`kd.py` (user space, kext), `ftemu.py`, `gftemu.py` and
`emu_mp.py` (§3), and `fpemu.py`, the final pass's rate control frame by frame
(§12.5): replay of a hardware run's per-frame sizes, which must give its
frame QPs, or a closed loop with a per-frame bits model.

Snapshots contain Apple's firmware: keep them out of git (as `data/blobs/`).
