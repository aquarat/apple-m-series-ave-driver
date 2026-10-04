"""On the target, during a snap_hold: read back registers listed in a file (docs/93).

  sudo python3 mmio-target.py REGS.txt OUT.txt   (driver loaded with dva_debugfs=1)

REGS.txt: one physical address per line, taken from an emulation's writes, so
every read is of a register the firmware itself wrote (an arbitrary read can
be an SError, docs/53 f38). The debugfs file refuses anything outside the
encoder's own register banks.
"""
import os, struct, sys
fd = os.open("/sys/kernel/debug/apple_ave_mmio", os.O_RDONLY)
with open(sys.argv[2], "w") as out:
    for l in open(sys.argv[1]):
        a = int(l, 16)
        try:
            v = struct.unpack("<I", os.pread(fd, 4, a))[0]; out.write(f"{a:#012x} {v:#x}\n")
        except OSError as e:
            out.write(f"{a:#012x} ERR {e.errno}\n")
print("mmio ok")
