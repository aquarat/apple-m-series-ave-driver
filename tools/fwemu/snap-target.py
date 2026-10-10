# SPDX-License-Identifier: GPL-2.0-only
"""On the target, during a snap_hold: copy the encoder's device address space (docs/93).

  sudo python3 snap-target.py OUTDIR      (driver loaded with dva_debugfs=1)

fw.bin: DVA 0x800000000 + 0x1f8000 (t8112: TEXT, DATA and the firmware heap);
iova.bin: IOVA 0x80000000 + 2 GiB (the driver's buffers). Unmapped pages read
as zeros, so both compress to almost nothing.
"""
import sys, os
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
f = open("/sys/kernel/debug/apple_ave_dva", "rb", buffering=0)
for name, base, size in (("fw", 0x800000000, 0x1f8000), ("iova", 0x80000000, 0x80000000)):
    f.seek(base)
    with open(f"{out}/{name}.bin", "wb") as o:
        left = size
        while left:
            b = f.read(min(left, 1 << 20)); o.write(b); left -= len(b)
print("snap ok", out)
