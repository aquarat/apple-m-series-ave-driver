#!/usr/bin/env python3
"""Predict the quantiser scaling-list registers the AVC firmware programs (docs/74).

Models, from the macOS 13.5 AVE firmware, what ends up in

  IntraEst    0x40D24A088..0x40D24A1C4   4x4 + 8x8 intra-luma list
  ReconLuma   0x40D28A0A0..0x40D28A31C   4x4 intra/inter Y, 8x8 intra/inter Y
  ReconChroma 0x40D2AA0A4..0x40D2AA5A0   4x4 Cb/Cr intra/inter, 8x8 x4
  ReconLuma   0x40D28A088/08C            mode_8x8_transform & 3, RECONL SKIPMODE
  ReconChroma 0x40D2AA08C                RECONC SKIPMODE
  cancel      0x40D24A240/244, 0x40D28A334..348, 0x40D2AA5C0..5D4

from the SPS scaling lists the host sends in Start_AVC (H264_SEQUENCE_HEADER_PARAMS
at wire 0x105B0: 4x4 lists u16[6][16] at +0x5A, 8x8 lists u16[6][64] at +0x11A)
and the 0xFCD8-block bytes, after first checking that the instructions the model
is built from are the bytes it expects.

The chain it models (every step cites a VA; docs/74 §2 has the reasoning):
  InitEncodingParameters memcpy(ctrl+0x2C1FC, wire 0x105B0, 0x6AC)   0x5ce68-0x5ce90
  InitEncodingParameters -> InitScalingListRegs                       0x5e154
  InitScalingListRegs: value = ((0x10000 / w) << 16 | w) & 0x3FFF00FF  0x5fb9c-0x5fbbc
     (A64 UDIV by zero returns 0: a zero list gives a zero register)
     stored as (offset, value) pairs at ctrl+0x2F010 / ctrl+0x2F390
  setPipe writes every pair to the MMIO window every frame            0x576b0-0x57814

  python3 tools/scaling_regs.py                 # our driver: lists all zero
  python3 tools/scaling_regs.py --lists flat    # Apple's default (userspace 0x2d038)
  python3 tools/scaling_regs.py --dump          # every register, one per line
"""
import argparse, os, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(REPO, "data/blobs/macos-13.5/ave_h13c.bin")
FILE_OFF = 0x4000
AP = 0x40C000000                       # fw MMIO offset X is AP 0x40C000000 + X

ANCHORS = [
    (0x5ce7c, 0x9107f120, "IEP: add x0,x9,#0x1fc  (dst ctrl+0x2C1FC)"),
    (0x5ce80, 0x5280d582, "IEP: mov w2,#0x6ac     (SPS copy size)"),
    (0x5ce90, 0x97fea1d6, "IEP: bl memcpy"),
    (0x196b0, 0x9101691a, "AVC_SPS writer: add x26,x8,#0x5a  (4x4 lists at SPS+0x5A)"),
    (0x1973c, 0x9104691a, "AVC_SPS writer: add x26,x8,#0x11a (8x8 lists at SPS+0x11A)"),
    (0x5e154, 0x94000684, "IEP: bl InitScalingListRegs (0x5fb64)"),
    (0x5fb64, 0x52941488, "ISLR: mov w8,#0xa0a4"),
    (0x5fb68, 0x52941409, "ISLR: mov w9,#0xa0a0"),
    (0x5fb6c, 0x5294110a, "ISLR: mov w10,#0xa088"),
    (0x5fb84, 0x72a02548, "ISLR: movk w8,#0x12a,lsl#16  -> ReconChroma 0x12AA0A4"),
    (0x5fb88, 0x72a02509, "ISLR: movk w9,#0x128,lsl#16  -> ReconLuma 0x128A0A0"),
    (0x5fb8c, 0x72a0248a, "ISLR: movk w10,#0x124,lsl#16 -> IntraEst 0x124A088"),
    (0x5fb94, 0x910ad9ad, "ISLR: add x13,x13,#0x2b6 (ctrl+0x2C2B6 = SPS+0xBA)"),
    (0x5fb9c, 0x52a0002f, "ISLR: mov w15,#0x10000"),
    (0x5fba0, 0x72a7fff0, "ISLR: movk w16,#0x3fff,lsl#16 (mask 0x3FFF00FF)"),
    (0x5fbb4, 0x1ac209e3, "ISLR: udiv w3,w15,w2 (0x10000 / weight)"),
    (0x5fcb0, 0x910c598c, "ISLR: add x12,x12,#0x316 (ctrl+0x2C316 = SPS+0x11A)"),
    (0x5fcb4, 0x910e41ad, "ISLR: add x13,x13,#0x390 (8x8 pairs at ctrl+0x2F390)"),
    (0x57698, 0x529e0009, "setPipe: mov w9,#0xf000 (4x4 pairs at ctrl+0x2F000+)"),
    (0x576bc, 0xb82c690d, "setPipe: str w13,[x8,x12] (MMIO base + pair offset)"),
    (0x57758, 0x9140bdca, "setPipe: add x10,x14,#0x2f,lsl#12"),
    (0x57760, 0x9116014a, "setPipe: add x10,x10,#0x580 (8x8 pair loop)"),
    (0x5cef4, 0x794a62e8, "IEP: ldrh w8,[x23,#1328] wire 0xFCF0 skip_mode"),
    (0x5e134, 0xb9142e69, "IEP: str w9,[x19,#5164] RECONL SKIPMODE = skip_mode bit 0"),
    (0x5e138, 0xb9143268, "IEP: str w8,[x19,#5168] RECONC SKIPMODE = skip_mode bit 1"),
    (0x5782c, 0xb9000109, "setPipe: str -> 0x128A088 (mode_8x8 & 3)"),
    (0x5783c, 0xb9000509, "setPipe: str -> 0x128A08C (RECONL SKIPMODE)"),
    (0x57874, 0xb82a6928, "setPipe: str -> 0x12AA08C (RECONC SKIPMODE)"),
]


def load():
    d = open(FW, "rb").read()
    bad = 0
    for va, want, what in ANCHORS:
        got = struct.unpack_from("<I", d, va + FILE_OFF)[0]
        if got != want:
            print(f"ANCHOR MISMATCH {va:#x}: {got:#010x} != {want:#010x}  {what}")
            bad += 1
    if bad:
        sys.exit(f"{bad} anchor(s) differ: wrong blob, or the model is stale")
    return d


def recip(w):
    return (0x10000 // w) & 0xFFFF if w else 0     # A64 UDIV by 0 -> 0


def entry(w, rw=None):
    """InitScalingListRegs value: bfi recip into [31:16] of w, & 0x3FFF00FF."""
    rw = w if rw is None else rw
    return ((recip(rw) << 16) | (w & 0xFFFF)) & 0x3FFF00FF


def model(d, lists4, lists8, a):
    zz4 = struct.unpack_from("<32H", d, 0xd84b4 + FILE_OFF)   # (row, col) per zigzag k
    zz8 = struct.unpack_from("<128H", d, 0xd84f4 + FILE_OFF)
    IE, RL, RC = 0x124A088, 0x128A0A0, 0x12AA0A4
    regs = {}
    # loop 1, fw 0x5fba4-0x5fc98: k = zigzag index, idx = raster byte offset
    for k in range(16):
        idx = ((zz4[2 * k + 1] + (zz4[2 * k] << 2)) & 0xFFFF) << 2
        L = [lists4[i][k] for i in range(6)]
        regs[IE + idx] = entry(L[0])                 # [x12+0]   list 0 Intra Y
        regs[RL + idx] = entry(L[0])                 # [x12+128] list 0 Intra Y
        regs[RL + 0x40 + idx] = entry(L[3])          # [x12+256] list 3 Inter Y
        regs[RC + idx] = entry(L[1])                 # [x12+384] list 1 Intra Cb
        regs[RC + 0x40 + idx] = entry(L[2])          # [x12+512] list 2 Intra Cr
        regs[RC + 0x80 + idx] = entry(L[4])          # [x12+640] list 4 Inter Cb
        regs[RC + 0xC0 + idx] = entry(L[5])          # [x12+768] list 5 Inter Cr
    # loop 2, fw 0x5fcc4-0x5fdc8 (lists 6..11 = SPS+0x11A + 128*j), modelled
    # instruction by instruction, including its cross-list pairing at
    # 0x5fd30/0x5fd58/0x5fd5c (only visible with non-uniform lists)
    for k in range(64):
        idx = ((zz8[2 * k + 1] + (zz8[2 * k] << 3)) & 0xFFFF) << 2
        L6, L7, L8, L9, L10, L11 = (lists8[j][k] for j in range(6))
        regs[IE + 0x40 + idx] = entry(L6)            # [x13+0]
        regs[RL + 0x80 + idx] = entry(L6)            # [x13+512]
        regs[RL + 0x180 + idx] = entry(L9)           # [x13+1024]
        regs[RC + 0x100 + idx] = entry(L7, L8)       # [x13+1536] 0x5fd30
        regs[RC + 0x200 + idx] = entry(L8, L10)      # [x13+2048] 0x5fd5c
        regs[RC + 0x300 + idx] = entry(L10, L9)      # [x13+2560] 0x5fd58
        regs[RC + 0x400 + idx] = entry(L11)          # [x13+3072]
    # setPipe 0x57818-0x57874; IEP 0x5e124-0x5e138
    regs[0x128A088] = a.fcec & 3
    regs[0x128A08C] = a.skip_mode & 1
    regs[0x12AA08C] = (a.skip_mode >> 1) & 1
    # InitCoeffCancelCostRegs 0x5f218-0x5f33c, written by setPipe 0x57928-0x579d0
    en = 1 if a.qcoeff else 0
    if a.fcf2 != 0xFFFF:
        l, c = a.fcf2 & 1, (a.fcf2 >> 1) & 1
    else:
        l, c = int(regs[0x128A08C] == 0), int(regs[0x12AA08C] == 0)
    regs[0x124A240], regs[0x124A244] = 0x40000, 0x50000   # literal pool 0xd8020
    for base, v in ((0x128A334, l), (0x12AA5C0, c)):
        regs[base + 0], regs[base + 4], regs[base + 8] = 0x40000 | en, 0x40000 | en, 0x40000 | v
    for base, v in ((0x128A340, l), (0x12AA5CC, c)):
        regs[base + 0], regs[base + 4], regs[base + 8] = 0x50000 | en, 0x50000 | en, 0x50000 | v
    return regs


RANGES = [("IntraEst 4x4 intra Y", 0x124A088, 16), ("IntraEst 8x8 intra Y", 0x124A0C8, 64),
          ("ReconLuma 4x4 intra Y", 0x128A0A0, 16), ("ReconLuma 4x4 inter Y", 0x128A0E0, 16),
          ("ReconLuma 8x8 intra Y", 0x128A120, 64), ("ReconLuma 8x8 inter Y", 0x128A220, 64),
          ("ReconChroma 4x4 intra Cb", 0x12AA0A4, 16), ("ReconChroma 4x4 intra Cr", 0x12AA0E4, 16),
          ("ReconChroma 4x4 inter Cb", 0x12AA124, 16), ("ReconChroma 4x4 inter Cr", 0x12AA164, 16),
          ("ReconChroma 8x8 (x4)", 0x12AA1A4, 256)]
SINGLE = [(0x128A088, "ReconLuma mode_8x8_transform & 3"), (0x128A08C, "RECONL SKIPMODE"),
          (0x12AA08C, "RECONC SKIPMODE"), (0x124A240, "IntraEst cancel 0"),
          (0x124A244, "IntraEst cancel 1"), (0x128A334, "ReconLuma cancel 0x334"),
          (0x128A33C, "ReconLuma cancel 0x33C"), (0x128A340, "ReconLuma cancel 0x340"),
          (0x128A348, "ReconLuma cancel 0x348"), (0x12AA5C0, "ReconChroma cancel 0x5C0"),
          (0x12AA5C8, "ReconChroma cancel 0x5C8")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lists", choices=("zero", "flat"), default="zero",
                    help="SPS scaling lists sent: zero (our driver) or flat 16 (Apple default)")
    ap.add_argument("--skip-mode", type=lambda x: int(x, 0), default=0, help="wire 0xFCF0 (u16)")
    ap.add_argument("--fcf2", type=lambda x: int(x, 0), default=0, help="wire 0xFCF2 (u16)")
    ap.add_argument("--qcoeff", type=lambda x: int(x, 0), default=0, help="wire 0xFCF4 qcoeff_cancel")
    ap.add_argument("--fcec", type=lambda x: int(x, 0), default=0, help="wire 0xFCEC mode_8x8_transform")
    ap.add_argument("--dump", action="store_true", help="print every register")
    a = ap.parse_args()
    d = load()
    w = 0 if a.lists == "zero" else 16
    regs = model(d, [[w] * 16 for _ in range(6)], [[w] * 64 for _ in range(6)], a)
    if a.dump:
        for off in sorted(regs):
            print(f"AP {AP + off:#011x}  DPE +{AP + off - 0x40D100000:#08x}  {regs[off]:#010x}")
        return
    print(f"SPS lists = {a.lists}; wire 0xFCF0 = {a.skip_mode}, 0xFCF2 = {a.fcf2:#x}, "
          f"0xFCF4 = {a.qcoeff}, 0xFCEC = {a.fcec}")
    for name, base, n in RANGES:
        vals = {regs[base + 4 * i] for i in range(n)}
        v = (f"all {vals.pop():#010x}" if len(vals) == 1 else
             "mixed " + " ".join(f"{regs[base + 4 * i]:#x}" for i in range(4)) + " ...")
        print(f"  {name:26s} AP {AP + base:#011x}..{AP + base + 4 * n - 4:#x}  {v}")
    for off, name in SINGLE:
        print(f"  {name:26s} AP {AP + off:#011x}  {regs[off]:#010x}")


if __name__ == "__main__":
    main()
