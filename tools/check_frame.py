#!/usr/bin/env python3
"""Decide whether a run's encoded frame actually contains our source picture.

    tools/check_frame.py results/<run>-load1 [--width 1280] [--height 720]

After F17 this was done by hand: dump the buffers, count distinct byte values,
squint at a decoded PNG. The counting was wrong the first time (a u8 counter in
the driver wrapped), and "the picture looks flat" is not a measurement. So:
decode the bitstream, compare it against the source buffer the driver says it
handed to the hardware, and print a verdict with the number behind it.

The verdict is only worth something if it can also say no, so --selftest runs
three controls before any real comparison:

    planted positive   source vs itself            -> MATCH
    negative control   source vs a different ramp  -> MISMATCH
    blank control      source vs uniform 128       -> BLANK

If any control comes out wrong the tool refuses to grade the run (docs/00:
validate a discriminator against a negative control).
"""
import argparse, os, subprocess, sys, tempfile
from collections import Counter

# Above this the decoded picture is the source; below it is something else.
# An honest encode at fixed QP lands far above; F17's flat grey lands at ~7 dB.
PSNR_MATCH_DB = 20.0


def psnr(a: bytes, b: bytes) -> float:
    n = min(len(a), len(b))
    if not n:
        return 0.0
    se = sum((a[i] - b[i]) ** 2 for i in range(n))
    if se == 0:
        return float("inf")
    return 10.0 * __import__("math").log10(255.0 * 255.0 * n / se)


def grade(ref: bytes, got: bytes) -> tuple[str, float]:
    """MATCH / MISMATCH / BLANK, and the PSNR behind it."""
    p = psnr(ref, got)
    if len(set(got)) <= 2:
        return "BLANK", p
    return ("MATCH" if p >= PSNR_MATCH_DB else "MISMATCH"), p


def rows(buf: bytes, stride: int, width: int) -> bytes:
    """The visible part of a strided buffer, rows packed to @width."""
    out = bytearray()
    for off in range(0, len(buf) - stride + 1, stride):
        out += buf[off:off + width]
    return bytes(out)


def describe_layout(buf: bytes, name: str) -> None:
    """What was written where - the question recon_luma.bin raised in F17."""
    runs, cur, n = [], buf[0], 0
    for b in buf:
        if b == cur:
            n += 1
        else:
            runs.append((cur, n)); cur, n = b, 1
    runs.append((cur, n))
    nz = sum(1 for b in buf if b)
    print(f"  {name}: {len(buf)} bytes, {len(set(buf))} distinct, "
          f"{nz} non-zero ({100.0 * nz / len(buf):.1f}%)")
    # Written islands separated by zero gaps: report their pitch and width.
    offs, widths, p = [], [], 0
    for v, ln in runs:
        if v:
            offs.append(p); widths.append(ln)
        p += ln
    if 1 < len(offs) < len(buf) // 8:
        gaps = Counter(offs[i + 1] - offs[i] for i in range(len(offs) - 1))
        pitch, cnt = gaps.most_common(1)[0]
        print(f"  {name}: {len(offs)} written islands, pitch {pitch} "
              f"({cnt}/{len(offs) - 1} gaps agree), "
              f"widths {Counter(widths).most_common(3)}")


def decode(h264: str, width: int, height: int) -> bytes:
    with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as t:
        out = t.name
    try:
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "h264",
                            "-i", h264, "-pix_fmt", "gray", "-f", "rawvideo", out],
                           capture_output=True, text=True)
        if r.returncode:
            print(f"ffmpeg failed: {r.stderr.strip()}", file=sys.stderr)
            return b""
        with open(out, "rb") as f:
            return f.read(width * height)
    finally:
        os.unlink(out)


def selftest(ref: bytes) -> bool:
    ok = True
    for what, got, want in (
            ("planted positive", ref, "MATCH"),
            ("negative control", bytes((255 - b) for b in ref), "MISMATCH"),
            ("blank control", bytes([128]) * len(ref), "BLANK")):
        verdict, p = grade(ref, got)
        good = verdict == want
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} {what}: {verdict} "
              f"({p:.1f} dB), wanted {want}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--src-stride", type=int, default=0, help="default: --width")
    a = ap.parse_args()
    stride = a.src_stride or a.width

    src_path = os.path.join(a.dir, "input_luma.bin")
    if not os.path.exists(src_path):
        print(f"no input_luma.bin in {a.dir}", file=sys.stderr)
        return 2
    src = rows(open(src_path, "rb").read(), stride, a.width)

    print("self-test:")
    if not selftest(src):
        print("REFUSING to grade: the discriminator failed its own controls")
        return 2

    print("buffers:")
    describe_layout(open(src_path, "rb").read(), "input_luma")
    rec = os.path.join(a.dir, "recon_luma.bin")
    if os.path.exists(rec):
        describe_layout(open(rec, "rb").read(), "recon_luma")

    h264 = os.path.join(a.dir, "frame.h264")
    if not os.path.exists(h264):
        print("no frame.h264: nothing was encoded")
        return 1
    dec = decode(h264, a.width, a.height)
    if not dec:
        return 1
    n = min(len(dec), len(src))
    verdict, p = grade(src[:n], dec[:n])
    print(f"decoded: {len(dec)} bytes, {len(set(dec))} distinct")
    print(f"VERDICT: {verdict}  (PSNR {p:.1f} dB over {n} compared bytes)")
    return 0 if verdict == "MATCH" else 1


if __name__ == "__main__":
    sys.exit(main())
