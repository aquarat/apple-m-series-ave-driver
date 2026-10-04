#!/usr/bin/env python3
"""S_AVE_MultiPassStats (0x626 bytes) and the 0x108-byte sequence header:
offsets, a decoder for `dump`, the pass-2 input buffers, and synthetic records.
Offsets and labels follow docs/95 §2.4 and §2.6."""
import math
import random
import struct

REC = 0x626
HDR = 0x108
WINDOW = 10                 # records after the current one (frame 0 gets 0..10)
FIRST = HDR + 11 * REC      # 0x44AA
NEXT = HDR + REC            # 0x72E
assert FIRST == 0x44AA and NEXT == 0x72E

# rec+ fields the host code reads or writes
R_PTS = 0x04                # CMTime: value i64, timescale i32, flags u32, epoch i64 (host writes)
R_WIDTH, R_HEIGHT, R_FPS = 0x1C, 0x20, 0x24
R_FN = 0x2C                 # pic_info.display_order
R_SLICE, R_CLASS = 0x30, 0x34
R_BITS, R_HDRBITS, R_CORR = 0x40, 0x44, 0x48
R_FLAGS = 0x50              # bit 0: forceKeyFrame (as the host logs it)
R_HIST = 0xB0               # 256 x u32 LRME histogram
R_SCENE = 0x4B0             # u32 scene start (host)
R_HDIFF, R_HDIFF_MAX = 0x4B8, 0x4BC
R_ACT = 0x4C0               # f32, scales the histogram distance; scene-weighted mean (host)
R_SCNT = 0x4C4              # u32 frames in scene (host folds)
R_QS = 0x614                # f32 qscale
R_CPLX = 0x618              # 2 x f32 complexity
R_LCLASS = 0x624            # u16 NORMAL/MIN/MAX/BLANK

HEADER_FIELDS = [            # (offset, type, name)
    (0x00, "I", "total_scenes"), (0x04, "I", "cnt_All"), (0x08, "Q", "bits_All"),
    (0x10, "I", "cnt_class2"), (0x14, "Q", "bits_class2"),
    (0x1C, "I", "cnt_NORMAL"), (0x20, "Q", "bits_NORMAL"),
    (0x28, "I", "cnt_MIN"), (0x2C, "Q", "bits_MIN"),
    (0x34, "I", "cnt_MAX"), (0x38, "Q", "bits_MAX"),
    (0x40, "I", "cnt_BLANK"), (0x44, "Q", "bits_BLANK"),
    (0x4C, "f", "avg_qscale"), (0x50, "d", "qscale_sum_class2"),
    (0x58, "d", "current_complexity"), (0x60, "d", "totalcplxsum"),
]


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def s32(b, o): return struct.unpack_from("<i", b, o)[0]
def f32(b, o): return struct.unpack_from("<f", b, o)[0]


def decode_header(h):
    out = {}
    for o, t, n in HEADER_FIELDS:
        out[n] = struct.unpack_from("<" + t, h, o)[0]
    out["cplx_hist_count"] = list(struct.unpack_from("<16I", h, 0x68))
    out["cplx_hist_sum"] = list(struct.unpack_from("<16f", h, 0xA8))
    out["quant_count"] = list(struct.unpack_from("<4I", h, 0xE8))
    out["quant_value"] = list(struct.unpack_from("<4f", h, 0xF8))
    return out


def decode_record(r):
    val, ts, fl, ep = struct.unpack_from("<qiIq", r, R_PTS)
    return {
        "pts": (val, ts, fl, ep),
        "w": u32(r, R_WIDTH), "h": u32(r, R_HEIGHT), "fps": f32(r, R_FPS),
        "fn": s32(r, R_FN), "slice": u32(r, R_SLICE), "class": u32(r, R_CLASS),
        "bits": u32(r, R_BITS), "hdr_bits": u32(r, R_HDRBITS), "corr": s32(r, R_CORR),
        "force_key": u32(r, R_FLAGS) & 1,
        "scene": u32(r, R_SCENE), "hdiff": f32(r, R_HDIFF), "hdiff_max": f32(r, R_HDIFF_MAX),
        "act": f32(r, R_ACT), "scene_frames": u32(r, R_SCNT),
        "scene_bits": struct.unpack_from("<Q", r, 0x4CC)[0],
        "qscale": f32(r, R_QS), "cplx": struct.unpack_from("<2f", r, R_CPLX),
        "lclass": struct.unpack_from("<H", r, R_LCLASS)[0],
    }


def cmtime(i, fps):
    """PTS of display frame i as a CMTime (value, timescale, flags=valid, epoch 0)."""
    if float(fps).is_integer():
        ts, val = int(fps), i
    else:
        ts, val = int(round(fps * 1000)), i * 1000
    return struct.pack("<qiIq", val, ts, 1, 0)


def set_pts(rec, i, fps):
    b = bytearray(rec)
    b[R_PTS:R_PTS + 24] = cmtime(i, fps)
    return bytes(b)


def read_records(path):
    data = open(path, "rb").read()
    if len(data) % REC:
        raise ValueError("%s: %d bytes is not a multiple of 0x626" % (path, len(data)))
    return [data[i:i + REC] for i in range(0, len(data), REC)]


def read_table(path):
    data = open(path, "rb").read()
    if len(data) < HDR or (len(data) - HDR) % REC:
        raise ValueError("%s: %d bytes is not 0x108 + n*0x626" % (path, len(data)))
    return data[:HDR], [data[i:i + REC] for i in range(HDR, len(data), REC)]


def pass2_buffers(header, recs):
    """The per-frame pass-2 input buffers (PICMGMT+0x900), as macOS builds them.

    AVE_H264MultipassDataFetch (UA 0x39a98) and the IOSurface copy (UA
    0xa5ff4-0xa61a8): frame 0 gets the header and the records of frames
    0..10 (0x108 + 11*0x626 = 0x44AA bytes); frame k >= 1 gets the header
    and the record of frame k+10 (0x72E bytes). Past the end of the clip
    the last record is repeated (UA 0x39fb0 copies the previous slot;
    UA 0x39d08 steps back to the last time stamp)."""
    n = len(recs)
    if n == 0:
        raise ValueError("no records")
    out = []
    first = [recs[min(i, n - 1)] for i in range(WINDOW + 1)]
    out.append(header + b"".join(first))
    for k in range(1, n):
        out.append(header + recs[min(k + WINDOW, n - 1)])
    return out


# ---------------------------------------------------------------- synthetic records
def synth(n, cuts=(), width=1280, height=720, fps=30.0, seed=1, bits=None,
          noise=3, blocks=3600, gop_idr=0, force_key=()):
    """Plausible pass-1 records of an IPPP clip: frame 0 is an I frame (slice
    type and class 2), the others P (0). The LRME histogram is a bump whose
    centre jumps at every cut; bits rise on the frame after a cut."""
    rnd = random.Random(seed)
    recs = []
    centre = 64
    cuts = set(cuts)
    for i in range(n):
        if i in cuts:
            centre = 64 + (centre + 97) % 128
        r = bytearray(REC)
        struct.pack_into("<IIf", r, R_WIDTH, width, height, fps)
        struct.pack_into("<I", r, R_FN, i)
        intra = i == 0 or (gop_idr and i % gop_idr == 0)
        st = 2 if intra else 0
        struct.pack_into("<II", r, R_SLICE, st, st)
        if bits is not None:
            fb = bits(i)
        else:
            fb = (180000 if intra else 22000) + rnd.randint(-3000, 3000)
            if i in cuts:
                fb = 150000 + rnd.randint(-5000, 5000)
        hb = 600 + rnd.randint(0, 200)
        struct.pack_into("<IIi", r, R_BITS, fb, hb, rnd.randint(-64, 64))
        struct.pack_into("<I", r, R_FLAGS, 1 if i in force_key else 0)
        # histogram: a bump around `centre`, total `blocks`
        h = [0] * 256
        for _ in range(blocks):
            b = int(rnd.gauss(centre, 12 + noise))
            h[max(0, min(255, b))] += 1
        struct.pack_into("<256I", r, R_HIST, *h)
        struct.pack_into("<f", r, R_ACT, 1.0)
        # per-frame scene sums as the firmware seeds them: count 1, bits, hdr bits, qscale
        q = 2.0 + 0.5 * rnd.random() + (1.0 if intra else 0.0)
        struct.pack_into("<II", r, R_SCNT, 1, 1 if intra else 0)
        struct.pack_into("<QQQ", r, 0x4CC, fb, 0, hb)
        struct.pack_into("<dd", r, 0x4F4, q, q * q)
        struct.pack_into("<ff", r, 0x504, q, q)
        lc = math.log10(fb / 1000.0)               # a log10 complexity in [0, 3)
        c2 = rnd.random()
        struct.pack_into("<dd", r, 0x564, lc, c2)
        b16 = max(0, min(15, int(lc / 0.1875)))
        struct.pack_into("<I", r, 0x574 + 4 * b16, 1)
        struct.pack_into("<f", r, 0x5B4 + 4 * b16, lc)
        struct.pack_into("<fff", r, R_QS, q, lc, c2)
        struct.pack_into("<H", r, R_LCLASS, 0)
        recs.append(set_pts(bytes(r), i, fps))
    return recs


def fuzz(n, seed=1, special=False):
    """Records with random values in every field the host code reads (for
    comparing the port with the emulation, not for plausibility)."""
    rnd = random.Random(seed)
    recs = []
    for i in range(n):
        r = bytearray(rnd.getrandbits(8) for _ in range(REC))
        struct.pack_into("<I", r, R_FN, i)
        struct.pack_into("<I", r, R_CLASS, rnd.choice((0, 1, 2, 2, 0, 7)))
        struct.pack_into("<I", r, R_BITS, rnd.choice((rnd.getrandbits(32), rnd.randint(0, 300000), 0, 5)))
        struct.pack_into("<i", r, R_CORR, rnd.choice((0, rnd.randint(-5000, 5000), s32(r, R_CORR))))
        struct.pack_into("<H", r, R_LCLASS, rnd.choice((0, 1, 2, 3, 4, rnd.getrandbits(16))))
        if rnd.random() < 0.5:   # keep most histograms sane so the scene logic is exercised
            c = rnd.randint(0, 255)
            h = [rnd.randint(0, 3) for _ in range(256)]
            h[c] += rnd.randint(0, 5000)
            struct.pack_into("<256I", r, R_HIST, *h)
            struct.pack_into("<f", r, R_ACT, rnd.choice((0.0, 1.0, rnd.random() * 50, -3.0)))
        if rnd.random() < 0.5:
            struct.pack_into("<I", r, R_SCNT, rnd.randint(0, 3))
        if rnd.random() < 0.3:   # sane complexity histogram entries
            for k in range(16):
                struct.pack_into("<I", r, 0x574 + 4 * k, rnd.randint(0, 3))
                struct.pack_into("<f", r, 0x5B4 + 4 * k, rnd.random() * 3)
        if special and rnd.random() < 0.5:   # NaNs (quiet and signalling), infinities, zeros, subnormals
            for o in (R_ACT, 0x504, 0x508, R_QS, R_CPLX, R_CPLX + 4) + tuple(0x5B4 + 4 * k for k in range(16)):
                if rnd.random() < 0.3:
                    struct.pack_into("<I", r, o, rnd.choice(SPECIAL32))
        recs.append(bytes(r))
    return recs


SPECIAL32 = (0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF812345, 0x7FA5A5A5, 0x7F800000, 0xFF800000,
             0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x3F800000, 0x7F7FFFFF)
