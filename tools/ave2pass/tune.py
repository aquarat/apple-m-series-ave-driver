#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-side departures from macOS for the final pass (docs/95 §12). None of
these is what macOS does; `ave2pass.py build` applies them only when asked.

force_keys(recs, frames)
    Sets rec+0x50 bit 0 (forceKeyFrame) on the pass-1 records of the given
    display frames, before the MP pipeline. The pipeline then starts a scene
    there (§2.4.4), and the final pass puts an IDR there and runs its
    scene-level rate control (finalPassSceneLevel) on that frame.

rc_scene(recs, p, build)
    A scene start for the rate controller only, at display frame p, with no
    IDR. The table is built as usual; record p then gets the scene block
    (rec+0x4B0..0x5FF) that a forced key frame at p would have given it. The
    enclosing scene's first record keeps its rec+0x4C4, so CFrameType's scene
    chain (UpdateNextScene, fw 0x383c4) never lands on p; the firmware's scene
    ring (ProcessFirstPassStats, fw 0x1e5d8) takes every record with
    rec+0x4B0 != 0, and ProcessPipeStart copies p's block in on frame p
    (fw 0x45d94). §12.6.

bits_weighted_qscale(hdr, recs)
    Rewrites each scene's qscale sum (rec+0x4F4 of its first record) as
    frames x sum(bits x qscale) / sum(bits), and the header's avg_qscale
    (+0x4C) the same way over the clip. The final pass's start estimate is
    mean(qscale) x scene bits / target bits (fw 0x30464..0x305c4); with a
    first pass whose QP moved a lot (the VBR first pass ramps up from QP 21),
    the plain mean overstates it. §12.3.
"""
import struct

import recfmt

REC = recfmt.REC
SB, SE = 0x4B0, 0x600            # the scene block the firmware copies (fw 0x45e14: 0x150 bytes)
R_QSUM = 0x4F4                   # f64 scene qscale sum (host folds, §2.4.5)


def _fn(r):
    return recfmt.s32(r, recfmt.R_FN)


def force_keys(recs, frames):
    want = set(frames)
    out = []
    for r in recs:
        if _fn(r) in want:
            b = bytearray(r)
            struct.pack_into("<I", b, recfmt.R_FLAGS, recfmt.u32(b, recfmt.R_FLAGS) | 1)
            r = bytes(b)
        out.append(r)
    return out


def scene_starts(recs):
    return [i for i, r in enumerate(recs) if recfmt.u32(r, recfmt.R_SCENE)]


def rc_scene(recs, p, build):
    """recs: pass-1 records (arrival order); build(recs) -> (header, records in display order).
    Returns (header, records, note)."""
    hdr, out = build(recs)
    n = len(out)
    if not 1 <= p < n:
        raise ValueError("--rc-scene %d: needs 1 <= P < %d" % (p, n))
    if recfmt.u32(out[p], recfmt.R_SCENE):
        return hdr, out, "frame %d already starts a scene; nothing to add" % p
    s = max(i for i in scene_starts(out) if i < p)
    end = s + recfmt.u32(out[s], recfmt.R_SCNT)
    _, alt = build(force_keys(recs, [p]))
    if not recfmt.u32(alt[p], recfmt.R_SCENE):
        raise ValueError("a forced key frame at %d did not start a scene" % p)
    note = ""
    if p + recfmt.u32(alt[p], recfmt.R_SCNT) != end:
        note = ("the forced cut at %d changed the next scene start (%d -> %d); "
                "the block covers [%d, %d)" % (p, end, p + recfmt.u32(alt[p], recfmt.R_SCNT),
                                                p, p + recfmt.u32(alt[p], recfmt.R_SCNT)))
    r = bytearray(out[p])
    r[SB:SE] = alt[p][SB:SE]
    out = list(out)
    out[p] = bytes(r)
    return hdr, out, note


def bits_weighted_qscale(hdr, recs):
    recs = [bytearray(r) for r in recs]
    n = len(recs)
    bits = [recfmt.u32(r, recfmt.R_BITS) for r in recs]
    qs = [recfmt.f32(r, recfmt.R_QS) for r in recs]
    for s in scene_starts(recs):
        e = min(n, s + recfmt.u32(recs[s], recfmt.R_SCNT))
        b = sum(bits[s:e])
        if e > s and b:
            bq = sum(bits[i] * qs[i] for i in range(s, e))
            struct.pack_into("<d", recs[s], R_QSUM, (e - s) * bq / b)
    h = bytearray(hdr)
    b = sum(bits)
    if b:
        struct.pack_into("<f", h, 0x4C, sum(x * q for x, q in zip(bits, qs)) / b)
    return bytes(h), [bytes(r) for r in recs]
