#!/usr/bin/env python3
"""Self-test for ave2pass: synthetic records through the port, invariants on
the table and the pass-2 buffers; and, when the macOS binary is present,
the port against Apple's own code under Unicorn, byte for byte.

  .venv/bin/python tools/ave2pass/selftest.py [--emu-cases N]
"""
import argparse
import math
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recfmt  # noqa: E402
import mpport  # noqa: E402
import ave2pass  # noqa: E402
import tune  # noqa: E402

REC, HDR = recfmt.REC, recfmt.HDR
fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def u32(r, o): return struct.unpack_from("<I", r, o)[0]


def test_scene_cut():
    n, cut = 40, 12
    recs = recfmt.synth(n, cuts=(cut,), seed=7)
    hdr, out = ave2pass.build(recs)
    check(len(hdr) == HDR, "header is 0x108 bytes")
    check(len(out) == n, "one record per frame")
    check([recfmt.s32(r, recfmt.R_FN) for r in out] == list(range(n)), "records in display order")
    starts = [i for i, r in enumerate(out) if u32(r, recfmt.R_SCENE)]
    check(starts == [0, cut], "scene starts at 0 and %d (got %s)" % (cut, starts))
    check(u32(out[0], recfmt.R_SCNT) == cut, "scene 0 has %d frames (rec+0x4C4 = %d)" % (cut, u32(out[0], recfmt.R_SCNT)))
    check(u32(out[cut], recfmt.R_SCNT) == n - cut, "scene 1 has %d frames" % (n - cut))
    check(all(u32(r, recfmt.R_SCNT) == 1 for i, r in enumerate(out) if i not in (0, cut)),
          "non-start records keep their own count")
    h = recfmt.decode_header(hdr)
    check(h["total_scenes"] == 2, "total_scenes 2 (got %d)" % h["total_scenes"])
    check(h["cnt_All"] == n, "cnt_All %d" % n)
    check(h["bits_All"] == sum(u32(r, recfmt.R_BITS) for r in out), "bits_All = sum of corrected frame bits")
    check(h["cnt_class2"] == 1 and h["bits_class2"] == u32(out[0], recfmt.R_BITS), "one class-2 (I) frame: frame 0")
    check(h["cnt_NORMAL"] == n, "every frame NORMAL")
    check(sum(h["cplx_hist_count"]) == n, "16-bin histogram counts every frame")
    check(sum(h["quant_count"]) == n, "quantised counts add up to the frames (%s)" % h["quant_count"])
    qv = h["quant_value"]
    check(all(qv[i] <= qv[i + 1] for i in range(3)), "quantised values ascending")
    # scene sums folded into the scene's first record
    sb = struct.unpack_from("<Q", out[cut], 0x4CC)[0]
    own = u32(out[cut], recfmt.R_BITS)          # synth seeds rec+0x4CC with the frame bits
    check(sb == own + sum(struct.unpack_from("<Q", r, 0x4CC)[0] for r in out[cut + 1:]),
          "scene 1 bit sum folded into rec %d" % cut)
    # metrics: the cut has the largest histogram distance
    hd = [recfmt.f32(r, recfmt.R_HDIFF) for r in out]
    check(max(range(1, n), key=lambda i: hd[i]) == cut, "largest rec+0x4B8 at the cut")
    # bits correction: frame k gets rec[k+2].0x48 (arrival order), for k <= n-3
    ok = all(u32(out[k], recfmt.R_BITS) == (u32(recs[k], recfmt.R_BITS) + recfmt.s32(recs[k + 2], recfmt.R_CORR)) & 0xFFFFFFFF
             for k in range(n - 2))
    check(ok, "rec+0x40 += rec+0x48 of the record two arrivals later")


def test_no_cut_and_force():
    out = ave2pass.build(recfmt.synth(30, seed=3))[1]
    check([i for i, r in enumerate(out) if u32(r, recfmt.R_SCENE)] == [0], "no cut: one scene")
    out = ave2pass.build(recfmt.synth(30, seed=3, force_key=(20,)))[1]
    check([i for i, r in enumerate(out) if u32(r, recfmt.R_SCENE)] == [0, 20], "rec+0x50 bit 0 forces a scene start")
    out = ave2pass.build(recfmt.synth(30, cuts=(2,), seed=3))[1]
    check(not u32(out[2], recfmt.R_SCENE), "no detected cut on display order < 3 (UA 0xb9430)")


def test_buffers():
    for n in (1, 5, 11, 12, 40):
        hdr, out = ave2pass.build(recfmt.synth(n, seed=n))
        bufs = recfmt.pass2_buffers(hdr, out)
        check(len(bufs) == n, "n=%d: one buffer per frame" % n)
        check(len(bufs[0]) == 0x44AA, "frame 0 buffer 0x44AA")
        check(bufs[0][:HDR] == hdr, "frame 0 starts with the header")
        first = [bufs[0][HDR + i * REC:HDR + (i + 1) * REC] for i in range(11)]
        check(first == [out[min(i, n - 1)] for i in range(11)], "n=%d: frame 0 carries records 0..10, last repeated" % n)
        for k in range(1, n):
            b = bufs[k]
            if len(b) != 0x72E or b[:HDR] != hdr or b[HDR:] != out[min(k + 10, n - 1)]:
                check(False, "n=%d frame %d buffer" % (n, k))
                break
        else:
            check(True, "")


def test_numerics():
    # one rounding: 1 + (1 + 2^-23)(1 - 2^-23) = 2 - 2^-46 -> 2.0 single-rounded
    a = 1.0 + 2.0 ** -23
    b = 1.0 - 2.0 ** -23
    check(mpport.fma32(-1.0, a, b) == -(2.0 ** -46), "fma32 is fused")
    check(mpport.fdiv32(0.0, 0.0) != mpport.fdiv32(0.0, 0.0), "0/0 is NaN")
    nan = mpport.fdiv32(0.0, 0.0)
    b4 = bytearray(4)
    mpport.st32(b4, 0, nan)
    check(bytes(b4) == bytes.fromhex("0000c07f"), "default NaN is +0x7FC00000")
    check(mpport.fmaxnm32(nan, 0.5) == 0.5 and math.isnan(mpport.fmax32(nan, 0.5)), "fmax/fmaxnm")


def test_tune():
    """docs/95 §12: --key, --rc-scene, --scene-qscale bits."""
    n = 40
    recs = recfmt.synth(n, seed=3, bits=lambda k: 20000 + (k * 7919) % 50000)
    hdr, out = ave2pass.build(recs)
    # forced key frame: a scene start (and so an IDR in the final pass) at 5
    _, out_k = ave2pass.build(tune.force_keys(recs, [5]))
    check(tune.scene_starts(out_k) == [0, 5], "--key 5: scenes start at 0 and 5 (%s)" % tune.scene_starts(out_k))
    # rate-control scene at 1: frame 1 carries the block a cut at 1 would, frame 0 keeps its 0x4C4
    h2, out_r, note = tune.rc_scene(recs, 1, ave2pass.build)
    check(note == "", "--rc-scene 1: no note (%s)" % note)
    check(h2 == hdr, "--rc-scene: header unchanged")
    check(tune.scene_starts(out_r) == [0, 1], "--rc-scene 1: scene marks at 0 and 1")
    check(u32(out_r[0], recfmt.R_SCNT) == n, "--rc-scene: frame 0 still spans the clip (CFrameType's chain)")
    check(u32(out_r[1], recfmt.R_SCNT) == n - 1, "--rc-scene: frame 1's block has %d frames" % (n - 1))
    check(struct.unpack_from("<Q", out_r[1], 0x4CC)[0] == sum(u32(r, recfmt.R_BITS) for r in out[1:]),
          "--rc-scene: frame 1's block sums the bits of frames 1..%d" % (n - 1))
    check(all(out_r[i] == out[i] for i in range(n) if i != 1), "--rc-scene: every other record unchanged")
    # bits-weighted qscale sums
    hb, out_b = tune.bits_weighted_qscale(hdr, out)
    b = [u32(r, recfmt.R_BITS) for r in out]
    q = [recfmt.f32(r, recfmt.R_QS) for r in out]
    want = n * sum(x * y for x, y in zip(b, q)) / sum(b)
    got = struct.unpack_from("<d", out_b[0], 0x4F4)[0]
    check(abs(got - want) < 1e-9 * want, "--scene-qscale bits: scene sum %.6g, want %.6g" % (got, want))
    check(all(out_b[i] == out[i] for i in range(1, n)), "--scene-qscale bits: only scene-start records change")
    flat = [bytearray(r) for r in recs]
    for r in flat:
        struct.pack_into("<f", r, recfmt.R_QS, 7.5)
        struct.pack_into("<d", r, 0x4F4, 7.5)      # the firmware seeds the scene sum with the frame's qscale
    h3, out3 = ave2pass.build([bytes(r) for r in flat])
    _, out3b = tune.bits_weighted_qscale(h3, out3)
    check(abs(struct.unpack_from("<d", out3b[0], 0x4F4)[0] - struct.unpack_from("<d", out3[0], 0x4F4)[0]) < 1e-6,
          "--scene-qscale bits: a constant qscale gives the macOS sum")


def test_emulation(cases):
    try:
        import mpemu
        if not os.path.exists(mpemu.P):
            raise FileNotFoundError(mpemu.P)
    except (ImportError, FileNotFoundError) as e:
        print("emulation cross-check skipped (%s)" % e)
        return
    rnd = random.Random(1)
    sets = [recfmt.synth(40, cuts=(12,), seed=7)]
    for i in range(cases):
        n = rnd.choice((1, 2, 3, 6, 7, 11, 40, 120))
        kind = i % 3
        if kind == 0:
            cuts = sorted(rnd.sample(range(1, max(2, n)), min(3, max(1, n - 1)))) if n > 2 else ()
            sets.append(recfmt.synth(n, cuts=cuts, seed=i, noise=rnd.choice((0, 3, 40)),
                                     gop_idr=rnd.choice((0, 30)),
                                     bits=rnd.choice((None, lambda k: 5000 + (k * 7919) % 90000, lambda k: 1))))
        else:
            sets.append(recfmt.fuzz(n, seed=i, special=kind == 2))
    bad = 0
    for recs in sets:
        if mpemu.run(recs) != mpport.run(recs):
            bad += 1
    check(bad == 0, "port == emulation on %d record sets (%d differ)" % (len(sets), bad))
    print("emulation cross-check: %d record sets, %d differ" % (len(sets), bad))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu-cases", type=int, default=60)
    a = ap.parse_args()
    test_scene_cut()
    test_no_cut_and_force()
    test_buffers()
    test_numerics()
    test_tune()
    test_emulation(a.emu_cases)
    print("%d checks, %d failed" % (checks, fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
