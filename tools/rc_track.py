#!/usr/bin/env python3
"""Grade a rate-controlled run: does the firmware controller track its target?

    tools/rc_track.py results/<run>-load1 --bitrate 300000 [--fps 30]
                      [--qp-max 51] [--settle 3] [--tol 0.10]

Reads <dir>/frame.h264 (every frame: slice QP and coded size come from the
encoder's own output, via tools/h264_parse.py) and any <dir>/coded_hdr*.bin
(CODED_DATA_HDR, 0x23000 bytes; docs/66 §3.1, docs/76 §4).

Controls, checked before anything is graded (per AGENTS.md: controls first):

  C1  every coded_hdr*.bin present must satisfy
        (QpyInter + QpyIntra) / (I_MbCnt[0] + P_MbCnt[0]) == slice_qp
      of the frame it names (FrameNumberFromDriverReturned), with
      hdr+0x90 / +0x94 = QpyInter / QpyIntra (kext AVE_PrintRCStats names,
      docs/76 §4). f77 passes this on all four headers. If it fails, the
      header layout or the frame pairing is wrong, not the controller.
  C2  the per-frame byte count from CODED_DATA_HDR (slice[0].ui32BytesWritten
      at +0x180) must equal that frame's slice NAL size in frame.h264 (both
      exclude the SPS/PPS, which are counted separately below).

Verdicts (docs/76 §5):

  TRACKS      mean rate over frames [settle*fps, end) within +-tol of target
              and every whole 1-s window after settle within +-2.5*tol.
  CEILING     over target, and the last second's QP is at --qp-max: the
              target is unreachable at this content/resolution; not a fault.
  STALLS      over target by more than tol, QP below --qp-max and not rising
              over the last two seconds: the controller believes it is on
              target. This is f77's shape.
  UNDERSHOOT  under target by more than tol.
  SHORT       fewer than settle*fps + fps frames: cannot grade convergence.
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h264_parse import nal_units, parse_pps, parse_slice, parse_sps  # noqa: E402

HDR_SIZE = 0x23000

# CRateControl::ProcessInit's reference-bitrate table (fw 13.5, docs/76 §2.3):
# rows by macroblock count (0xd1a28), columns by frame rate (0xd182c),
# u64 bit/s at 0xd1848 + codec*240 + row*48 + col*8. AVC = codec 0.
REF_MBS = (3600, 6075, 8100, 10800, 32400)
REF_FPS = (0, 24, 30, 60, 120, 240)
REF_AVC = ((0, 6480000, 8100000, 14300000, 28700000, 38100000),
           (0, 6400000, 8000000, 16000000, 32000000, 64000000),
           (0, 12240000, 15300000, 23300000, 45000000, 76000000),
           (0, 12400000, 15500000, 31000000, 62000000, 124000000),
           (0, 36000000, 45000000, 76000000, 152000000, 304000000))


def ref_bitrate(mbs, fps, chroma_format_idc=1):
    """ProcessInit fw 0x40574-0x40734 and 0x40d68-0x40e3c (sCRCInitParams+176
    = 0, i.e. the non-p176 arm, which is what InitEncodingParameters leaves)."""
    if mbs < 0xE11:
        cls = 0
    elif (mbs >> 2) < 0x5EF:
        cls = 1
    elif mbs < 8101:
        cls = 2
    else:
        cls = 3 if mbs < 10801 else 4
    col = 1 if fps < 25 else 2 if fps < 31 else 3 if fps < 61 else \
        4 if fps < 121 else 5
    lo_row = REF_AVC[cls]
    r_lo, r_hi = lo_row[col - 1], lo_row[col]
    if mbs != REF_MBS[cls]:                         # 0x40d68: interpolate on MBs
        if mbs < 0xE10:
            b_mbs, b_row = 0, (0,) * 6
            t_mbs, t_row = REF_MBS[cls], REF_AVC[cls]
        elif mbs <= 32400:
            b_mbs, b_row = REF_MBS[cls - 1], REF_AVC[cls - 1]
            t_mbs, t_row = REF_MBS[cls], REF_AVC[cls]
        else:
            b_mbs, b_row = 8100, REF_AVC[2]
            t_mbs, t_row = REF_MBS[cls], REF_AVC[cls]
        k = (mbs - b_mbs) / (t_mbs - b_mbs)
        r_lo = int(b_row[col - 1] + (t_row[col - 1] - b_row[col - 1]) * k)
        r_hi = int(b_row[col] + (t_row[col] - b_row[col]) * k)
    if fps != REF_FPS[col]:                         # 0x40e10: interpolate on fps
        r_hi = (r_hi - r_lo) * (fps - REF_FPS[col - 1]) // \
            (REF_FPS[col] - REF_FPS[col - 1]) + r_lo
    if chroma_format_idc == 3:                      # 0x406ec / 0x4071c
        r_hi = int(r_hi * 1.2)
    elif chroma_format_idc == 0:                    # 0x40700: 0.9 at 0xd7fe8
        r_hi = int(r_hi * 0.9)
    return r_hi


def init_qp(bitrate, mbs, fps, chroma_format_idc=1):
    """The first frame's QP under ui32RCFlag = 1 (this+880, fw 0x40800-0x409c8)."""
    ref = ref_bitrate(mbs, int(fps), chroma_format_idc)
    b = bitrate
    if ref == 0 or b == ref:
        d = 0
    elif b > ref:
        n = sum(b >= ref << k for k in range(1, 6))
        pct = int((b >> n) * 100 / ref)
        d = -6 * n - sum(pct > t for t in (0x6F, 0x7C, 0x8C, 0x9D, 0xB1))
    else:
        n = sum(b <= ref >> k for k in range(1, 6))
        pct = int((b << (n + 1)) * 100 / ref)
        d = 3 * n + (pct < 0x7E) + (pct < 0x9F)
        d -= d != 0
    return max(0, min(48, 26 + d)), ref


def frames_from_stream(buf):
    """-> list of dicts {type, qp, idr, bytes, ps_bytes, ref_idc} per frame."""
    sps, pps, frames = {}, {}, []
    pending_ps = 0
    # nal_units() strips start codes and emulation bytes; recount sizes from
    # the raw stream so 'bytes' is what a bitrate meter would see.
    raw_sizes = []
    i, n, starts = 0, len(buf), []
    while i < n - 3:
        if buf[i] == 0 and buf[i + 1] == 0 and buf[i + 2] == 1:
            # a 4-byte start code's zero_byte belongs to this NAL
            starts.append(i - 1 if i and buf[i - 1] == 0 else i)
            i += 3
        else:
            i += 1
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else n
        raw_sizes.append(e - s)
    for (ref_idc, typ, rbsp), size in zip(nal_units(buf), raw_sizes):
        if typ == 7:
            sps = parse_sps(rbsp)
            pending_ps += size
        elif typ == 8:
            pps = parse_pps(rbsp)
            pending_ps += size
        elif typ in (1, 5) and sps and pps:
            sl = parse_slice(rbsp, typ, ref_idc, sps, pps)
            if sl["first_mb"] == 0 or not frames:
                frames.append({"type": sl["slice_type"], "idr": typ == 5,
                               "qp": sl["slice_qp"], "bytes": 0,
                               "slice_bytes": 0, "ps_bytes": pending_ps,
                               "ref_idc": ref_idc, "frame_num": sl["frame_num"]})
                pending_ps = 0
            f = frames[-1]
            f["bytes"] += size
            f["slice_bytes"] += size
    for f in frames:
        f["bytes"] += f["ps_bytes"]
    return frames, sps


def read_hdr(path):
    d = open(path, "rb").read()
    if len(d) < 0x22180:
        return None
    u = lambda o: struct.unpack_from("<I", d, o)[0]  # noqa: E731
    return {"path": os.path.basename(path), "frame_num": u(0x10C),
            "frame_type": u(0x110), "i_mb": u(0x00), "p_mb": u(0x10),
            "skip_mb": u(0x20), "qpy_inter": u(0x90), "qpy_intra": u(0x94),
            "bytes0": u(0x180), "remove0": struct.unpack_from("<b", d, 0x38C)[0]}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--bitrate", type=int, required=True, help="target, bit/s")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--qp-max", type=int, default=51)
    ap.add_argument("--settle", type=float, default=3.0, help="seconds ignored")
    ap.add_argument("--tol", type=float, default=0.10)
    ap.add_argument("-v", action="store_true", help="print every frame")
    a = ap.parse_args()

    buf = open(os.path.join(a.dir, "frame.h264"), "rb").read()
    frames, sps = frames_from_stream(buf)
    if not frames:
        print("no frames in frame.h264")
        return 2
    fps = a.fps
    target = a.bitrate / 8.0 / fps
    mbs = sps.get("mbs_wide", 0) * sps.get("map_units_high", 0)

    # ---- controls -------------------------------------------------------
    # C0 (reported, not blocking): the first frame's QP is ProcessInit's
    # table-driven initial QP. f77 (300 kbit/s, 720p30): predicted 39, coded 39.
    pq, ref = init_qp(a.bitrate, mbs, a.fps)
    print(f"{'C0 ok ' if frames[0]['qp'] == pq else 'C0 BAD'} frame 0 QP "
          f"{frames[0]['qp']} vs ProcessInit model {pq} "
          f"(reference {ref} bit/s for {mbs} MBs at {a.fps:g} fps)")
    ok = True
    hdrs = [h for h in (read_hdr(p) for p in
            sorted(glob.glob(os.path.join(a.dir, "coded_hdr*.bin")))) if h]
    for h in hdrs:
        fn = h["frame_num"]
        if fn >= len(frames):
            print(f"C1 {h['path']}: frame {fn} not in stream ({len(frames)} frames)")
            ok = False
            continue
        n = h["i_mb"] + h["p_mb"]
        qp = (h["qpy_inter"] + h["qpy_intra"]) / n if n else -1
        f = frames[fn]
        c1 = n == mbs and abs(qp - f["qp"]) < 1e-9
        c2 = h["bytes0"] - h["remove0"] == f["slice_bytes"]
        print(f"{'C1 ok ' if c1 else 'C1 BAD'} {'C2 ok ' if c2 else 'C2 BAD'} "
              f"{h['path']}: frame {fn} MBs {n}/{mbs} "
              f"Qpy-mean {qp:.3f} vs slice_qp {f['qp']}; "
              f"hdr bytes {h['bytes0']} vs slice NAL {f['slice_bytes']}")
        ok &= c1 and c2
    if not hdrs:
        print("C1/C2: no coded_hdr*.bin; grading on the bitstream alone")
    if not ok:
        print("CONTROLS FAILED: not grading")
        return 3

    # ---- per frame and per second ---------------------------------------
    if a.v:
        for k, f in enumerate(frames):
            print(f"{k:4d} {f['type']}{'(IDR)' if f['idr'] else '     '} "
                  f"qp {f['qp']:2d} bytes {f['bytes']:6d} "
                  f"({f['bytes'] / target:5.2f}x target)")
    per_s = int(round(fps))
    print(f"target {a.bitrate} bit/s = {target:.0f} B/frame at {fps:g} fps; "
          f"{len(frames)} frames")
    wins = []
    for w in range(len(frames) // per_s):
        seg = frames[w * per_s:(w + 1) * per_s]
        rate = sum(f["bytes"] for f in seg) * 8 * fps / len(seg)
        qps = [f["qp"] for f in seg]
        wins.append((rate, min(qps), max(qps), qps[-1]))
        print(f"  second {w:3d}: {rate / 1000:8.1f} kbit/s "
              f"({rate / a.bitrate:5.2f}x)  QP {min(qps)}..{max(qps)} last {qps[-1]}")

    settle = int(a.settle * fps)
    if len(frames) < settle + per_s:
        print(f"VERDICT SHORT: {len(frames)} frames < settle {settle} + one second")
        return 0
    tail = frames[settle:]
    mean = sum(f["bytes"] for f in tail) * 8 * fps / len(tail)
    ratio = mean / a.bitrate
    tail_wins = wins[int(a.settle):]
    print(f"after {a.settle:g} s: mean {mean / 1000:.1f} kbit/s = {ratio:.3f}x target")
    last_qp = [f["qp"] for f in frames[-per_s:]]
    prev_qp = [f["qp"] for f in frames[-2 * per_s:-per_s]] or last_qp
    if abs(ratio - 1) <= a.tol and all(abs(r / a.bitrate - 1) <= 2.5 * a.tol
                                       for r, *_ in tail_wins):
        v = "TRACKS"
    elif ratio > 1 + a.tol and max(last_qp) >= a.qp_max:
        v = "CEILING"
    elif ratio > 1 + a.tol and max(last_qp) <= max(prev_qp):
        v = "STALLS"
    elif ratio < 1 - a.tol:
        v = "UNDERSHOOT"
    else:
        v = "OVER-BUT-RISING (not settled; run longer)"
    print(f"VERDICT {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
