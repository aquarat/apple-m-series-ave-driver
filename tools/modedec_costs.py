#!/usr/bin/env python3
"""Predict the ModeDecision candidate words and IntraEst per-QP words (docs/73).

Models, from the macOS 13.5 AVE firmware, what ends up in

  ModeDec  0x40D26A09C/0A0   (MD_IntraLambda / MD_InterLambda x nQuant)
  ModeDec  0x40D26A0A4..104  (the 25-word candidate record; 0x0AC.. is the
                              "23-word cost ladder" of docs/70)
  IntraEst 0x40D24A1D0/1D4/1D8 (the per-QP intra sub-mode words)

for a given slice type, QP and set of host fields, and first checks that the
instructions the model is built from are still the bytes it expects (so a
wrong blob or a wrong VA fails loudly instead of predicting nonsense).

Every step names the firmware VA it models; docs/73 has the reasoning.

  python3 tools/modedec_costs.py                    # our f39/f40 I frame
  python3 tools/modedec_costs.py --slice P          # a P frame, same fields
  python3 tools/modedec_costs.py --fce4 1           # wire 0xFCE4 = 1 (I_PCM)
  python3 tools/modedec_costs.py --fcec 2 --slice I # mode_8x8_transform = 2
"""
import argparse, os, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(REPO, "data/blobs/macos-13.5/ave_h13c.bin")
FILE_OFF = 0x4000                      # file offset = VA + 0x4000 (docs/43)
SEED = 0x01000001                      # fw 0x468b4 / 0x474a8 / 0x4f218

# (VA, expected little-endian word, what it is) - the model's anchors.
ANCHORS = [
    (0x468c8, 0x92829fe9, "SetDefaultParameters: mov x9,#-5376 (seed loop start)"),
    (0x474a0, 0xb1150129, "SetDefaultParameters: adds x9,x9,#0x540 (4 x 1344 B)"),
    (0x474b4, 0xb9150109, "SetDefaultParameters: str w9,[x8,#5376] (tail, 6 words)"),
    (0x4de30, 0x97ffe247, "ProcessInit: bl SetDefaultParameters before vt+536"),
    (0x5cf50, 0xb9452ee8, "IEP: ldr w8,[x23,#1324]  wire 0xFCEC -> ctrl+0xAB0"),
    (0x5cf58, 0x395492e8, "IEP: ldrb w8,[x23,#1316] wire 0xFCE4 -> ctrl+0x23FDE"),
    (0x5e524, 0x52994617, "IEP: mov w23,#0xca30 (struct0 + 4)"),
    (0x5e598, 0xf10046bf, "IEP: cmp x21,#0x11 (records 0..17 only)"),
    (0x5e5a8, 0x6f001420, "IEP: bic v0.4s,#0x1 (struct0 words 1..24)"),
    (0x5e590, 0xf100d2bf, "IEP: cmp x21,#0x34 (52 QPs)"),
    (0x5e7b8, 0xb94ab268, "IEP: ldr w8,[x19,#2736] mode_8x8_transform switch"),
    (0x481c4, 0x6f00e400, "PipePrepareParam: slice_type 2 (I) -> (0,0)"),
    (0x490e4, 0x0f000420, "PipePrepareParam: slice_type 0 (P) -> (1,1)"),
    (0x482c0, 0x5c47e840, "PipePrepareParam: slice_type 1 (B) -> [0xd7fc8] = (2,1)"),
    (0x4f218, 0x52800029, "ProcessPipeReset: mov w9,#1 (0x01000001 to 0x0AC..0x104)"),
    (0x570ec, 0x2ea11c40, "setPipe: bit v0.8b,v2.8b,v1.8b (per-QP bit 0 <- B[idx])"),
    (0x5714c, 0x6b0c017f, "setPipe: cmp w11,w12 (skip write when == 0x01000001)"),
    (0x57664, 0x3300012b, "setPipe: bfxil w11,w9,#0,#1 (0x0A4 bit 0 <- rec17 w0)"),
    (0x56330, 0xb94b232c, "setPipe: ldr w12,[x25,#2848] MD_IntraLambda -> 0x09C"),
    (0x56350, 0xb94b1f2c, "setPipe: ldr w12,[x25,#2844] MD_InterLambda -> 0x0A0"),
]
# IntraEst MCPU image (fw 0xe3350), Thumb halfwords at image offsets.
MCPU_ANCHORS = [
    (0xe3350, 0x270, 0x2401, "IntraEst: movs r4,#1"),
    (0xe3350, 0x272, 0xf2c0, "IntraEst: movt r4,#8 (first half) -> mask 0x00080001"),
    (0xe3350, 0x274, 0x0408, "IntraEst: movt r4,#8 (second half)"),
    (0xe37a0, 0x396, 0xf020, "ModeDec: bic.w r0,r0,#1 (clear candidate enable)"),
]


def load():
    d = open(FW, "rb").read()
    bad = 0
    for va, want, what in ANCHORS:
        got = struct.unpack_from("<I", d, va + FILE_OFF)[0]
        if got != want:
            print(f"ANCHOR MISMATCH {va:#x}: {got:#010x} != {want:#010x}  {what}")
            bad += 1
    for img, off, want, what in MCPU_ANCHORS:
        got = struct.unpack_from("<H", d, img + off + FILE_OFF)[0]
        if got != want:
            print(f"ANCHOR MISMATCH {img:#x}+{off:#x}: {got:#06x} != {want:#06x}  {what}")
            bad += 1
    if bad:
        sys.exit(f"{bad} anchor(s) differ: wrong blob, or the model is stale")
    return d


def tables(d):
    u = lambda va, fmt, n: struct.unpack_from(f"<{n}{fmt}", d, va + FILE_OFF)
    return dict(t0=u(0xd80d0, "H", 52),          # IEP 0x5e76c (0xA4 bits 4..15)
                t1=u(0xd8138, "I", 52),          # IEP 0x5e780 (0xA8 bits 16..31)
                t2=u(0xd8208, "I", 52),          # IEP 0x5e790 (0xA8 bits 4..15)
                pre_p=u(0xd8048, "I", 17),       # IEP 0x5e458 (P presets, recs 0..16)
                pre_b=u(0xd808c, "I", 17),       # IEP 0x5e45c (B presets)
                nq=u(0xd8414, "I", 40))          # setPipe nQuant table (docs/70)


def model(d, a):
    T = tables(d)
    # SetDefaultParameters: three 1800-byte structs (I, P, B) = 18 records
    # x 25 words, the 2 x 3-word B[] entries and 52 x 3 per-QP words, all SEED.
    st = [[SEED] * 450 for _ in range(3)]
    B = [[SEED] * 3 for _ in range(2)]
    pq = [[SEED] * 3 for _ in range(52)]
    pair = [[SEED, SEED] for _ in range(52)]
    clr = lambda v: v & ~1
    # InitEncodingParameters
    if a.vp10 and a.vp10 < 0x11:                # ctrl+0x23FD3, fw 0x5d3c0/0x5e44c
        for i in range(17):
            for s, pre in ((1, T["pre_p"][i]), (2, T["pre_b"][i])):
                for k in range(25):              # fw 0x5e480..0x5e4fc
                    w = 25 * i + k
                    st[s][w] = (st[s][w] & ~1) | ((pre >> k) & 1)
    for i in range(52):
        if i <= 17:                              # fw 0x5e598
            for k in range(1, 25):               # struct0 words 1..24, 0x5e5a0-0x5e690
                st[0][25 * i + k] = clr(st[0][25 * i + k])
            for k in range(9, 22):               # struct1 words 9..21, 0x5e694-0x5e724
                st[1][25 * i + k] = clr(st[1][25 * i + k])
            if a.fce2:                           # disable_skip_mode, 0x5e728
                for s in (1, 2): st[s][25 * i + 1] = clr(st[s][25 * i + 1])
            if a.fce3:                           # disable_intra_mode, 0x5e748
                for s in (1, 2): st[s][25 * i + 0] = clr(st[s][25 * i + 0])
        w0, w1 = pair[i]                         # 0x5e76c-0x5e7b4
        w0 = (w0 & ~0xfff0) | (((a.md_intra_offset + T["t0"][i]) & 0xfff) << 4)
        w1 = (w1 & 0xf) | ((T["t1"][i] & 0xffff) << 16) | ((T["t2"][i] & 0xfff) << 4)
        pair[i] = [w0, w1]
    if a.fcec == 0:                              # 0x5e7ec: no 8x8
        B[0][1], B[1][1] = clr(B[0][1]), clr(B[1][1])
    elif a.fcec == 1:                            # 0x5e7c8: 8x8 only
        for k in (0, 1): B[k][0], B[k][2] = clr(B[k][0]), clr(B[k][2])
    if a.fce4:                                   # 0x5e808: "Disabling Intra Modes"
        B[0] = [clr(x) for x in B[0]]
    # PipePrepareParam: slice type -> (struct, B) index
    s, b = {"I": (0, 0), "P": (1, 1), "B": (2, 1)}[a.slice]
    q = a.qp
    nq = T["nq"][max(q, 12) - 12] << 4
    regs = {}
    # setPipe 0x570a4-0x57130: per-QP words, bit 0 from B[b]
    for j, off in enumerate((0x1D0, 0x1D4, 0x1D8)):
        regs[0x40D24A000 + off] = (pq[q][j] & ~1) | (B[b][j] & 1)
    # ProcessPipeReset seeds 0x0AC..0x104, setPipe 0x57128-0x57638 overwrites
    for k in range(23):
        v = st[s][25 * 17 + 2 + k]
        regs[0x40D26A0AC + 4 * k] = SEED if v == SEED else v
    # setPipe 0x57638-0x576a8: 0x0A4/0x0A8 = per-QP pair, bit 0 from record 17
    regs[0x40D26A0A4] = (pair[q][0] & ~1) | (st[s][425] & 1)
    regs[0x40D26A0A8] = (pair[q][1] & ~1) | (st[s][426] & 1)
    # setPipe 0x56330-0x5635c
    regs[0x40D26A09C] = (a.md_intra_lambda * nq + 0x200) >> 10 & 0xffffffff
    regs[0x40D26A0A0] = (a.md_inter_lambda * nq + 0x200) >> 10 & 0xffffffff
    return regs, nq


F40 = {0x40D24A1D0: 0x01000001, 0x40D24A1D4: 0x01000000, 0x40D24A1D8: 0x01000001}
F40.update({0x40D26A0AC + 4 * k: 0x01000000 for k in range(22)})
F40[0x40D26A104] = 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--slice", choices="IPB", default="I")
    ap.add_argument("--qp", type=int, default=30)
    n = lambda x: int(x, 0)
    ap.add_argument("--fcec", type=n, default=0, help="wire 0xFCEC u32 mode_8x8_transform")
    ap.add_argument("--fce4", type=n, default=0, help="wire 0xFCE4 u8 all-intra-off (I_PCM)")
    ap.add_argument("--fce2", type=n, default=0, help="wire 0xFCE2 u8 disable_skip_mode")
    ap.add_argument("--fce3", type=n, default=0, help="wire 0xFCE3 u8 disable_intra_mode")
    ap.add_argument("--vp10", type=n, default=0, help="wire 0x70 u32 (preset records)")
    ap.add_argument("--md-intra-lambda", type=n, default=0, help="wire 0xFFA8 u32")
    ap.add_argument("--md-inter-lambda", type=n, default=0, help="wire 0xFFA4 u32")
    ap.add_argument("--md-intra-offset", type=n, default=0, help="wire 0xFFAC u32")
    a = ap.parse_args()
    d = load()
    print(f"{len(ANCHORS) + len(MCPU_ANCHORS)} instruction anchors match")
    regs, nq = model(d, a)
    print(f"slice {a.slice}  QPY {a.qp}  nQuant {nq:#x}")
    ours = (a.slice, a.qp, a.fcec, a.fce4, a.fce2, a.fce3, a.vp10,
            a.md_intra_lambda, a.md_inter_lambda, a.md_intra_offset) == ("I", 30, 0, 0, 0, 0, 0, 0, 0, 0)
    for ap_ in sorted(regs):
        tag = ""
        if ours and ap_ in F40:
            tag = "  f40 " + ("match" if F40[ap_] == regs[ap_] else f"READ {F40[ap_]:#010x}")
        print(f"  {ap_:#x}  {regs[ap_]:#010x}{tag}")


if __name__ == "__main__":
    main()
