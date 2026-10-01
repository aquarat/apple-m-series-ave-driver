#!/bin/sh
# After booting a candidate: which stage 2 ran, and what the probe exported.
# Read-only, no root needed.
C=/proc/device-tree/chosen

printf 'stage 2 version: '
tr -d '\0' < $C/asahi,m1n1-stage2-version; echo

if [ ! -e $C/asahi,ave-segment-ranges ]; then
    echo "no asahi,ave-segment-ranges in /chosen (steps A and stock do not export it)"
    exit 0
fi

python3 - <<'EOF'
import struct
C = "/proc/device-tree/chosen/"
seg = open(C + "asahi,ave-segment-ranges", "rb").read()
try:
    pre = open(C + "asahi,ave-pre-loaded", "rb").read()
    print("pre-loaded:", pre.hex())
except FileNotFoundError:
    print("pre-loaded: property absent")
if not seg:
    print("segment-ranges: EMPTY - iBoot did not place AVE firmware on this machine")
else:
    # struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; }
    for i in range(len(seg) // 32):
        phys, iova, remap, size, unk = struct.unpack_from("<QQQII", seg, i * 32)
        print(f"segment {i}: phys {phys:#x}  iova {iova:#x}  remap {remap:#x}  size {size:#x}  unk {unk:#x}")
    print("expected for H13G: TEXT size 0xcc000, DATA size 0x128000 at iova 0xcc000")
EOF
