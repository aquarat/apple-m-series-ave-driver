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
| `fwsyms.py` | host | function symbols and sizes from the firmware Mach-O |

Snapshots contain Apple's firmware: keep them out of git (as `data/blobs/`).
