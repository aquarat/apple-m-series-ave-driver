# SPDX-License-Identifier: GPL-2.0-only
"""locate.py SNAP FRAME: the CAVCController instance and the sCmdInformation of FRAME."""
import struct, sys
from fwmem import Snap
s = Snap(sys.argv[1]); fr = int(sys.argv[2])
ctrl = s.find(struct.pack("<Q", 0xd1200), 8)          # __ZTV14CAVCController + 16
u32 = lambda va: struct.unpack("<I", s.read(va, 4))[0]
recs = []
for base, b, _ in s.regions():
    if base != 0:
        continue
    for off in range(0x160000, len(b) - 0x30, 8):
        p = struct.unpack_from("<Q", b, off + 0x10)[0]
        if p >> 32 != 0xffffffff:
            continue
        try:
            if u32(p + 0xca8) == fr:
                recs.append((off, p, struct.unpack_from("<I", b, off + 0x24)[0]))
        except KeyError:
            pass
print("ctrl", [hex(c) for c in ctrl])
for off, p, slot in recs:
    print(f"cmdinfo {off:#x} picmgmt {p:#x} +0x20 {struct.unpack_from('<I', s.fw, off + 0x20)[0]} slot {slot} type {u32(p + 0xcac)}")
