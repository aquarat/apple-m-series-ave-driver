#!/usr/bin/env python3
"""Full-frame PSNR of every decoded frame against the driver's own source.

check_frame.py grades against the 256 KiB input_luma debugfs dump, one
frame. This rebuilds each source frame from ave_session_fill_input()'s
formula (driver/ave_session.c) - frame n is the ramp shifted by n*8, or a
constant with --flat - and compares all three planes of every frame
ffmpeg decodes.

  tools/ramp_psnr.py results/<run>-load1 [--flat 200] [--width 1280 --height 720]
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile


def source(w, h, n, flat):
    if flat:
        y = bytes([flat]) * (w * h)
    else:
        sh = n * 8
        y = bytes(16 + (((x + sh) % w) * 219) // w + ((r // 16) & 7)
                  for r in range(h) for x in range(w))
    u = bytes([128]) * (w * h // 4)
    v = bytes(128 + (((2 * c) // 32) & 15) - 8
              for r in range(h // 2) for c in range(w // 2))
    return y, u, v


def psnr(a, b):
    mse = sum((p - q) ** 2 for p, q in zip(a, b)) / len(a)
    return (float("inf") if mse == 0 else 10 * math.log10(255 * 255 / mse)), \
        max(abs(p - q) for p, q in zip(a, b))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--flat", type=int, default=0)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    a = ap.parse_args()
    w, h = a.width, a.height
    fs = w * h * 3 // 2
    with tempfile.NamedTemporaryFile(suffix=".yuv") as t:
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "h264", "-i",
                            os.path.join(a.dir, "frame.h264"), "-pix_fmt",
                            "yuv420p", "-f", "rawvideo", t.name],
                           capture_output=True, text=True)
        if r.returncode:
            print("ffmpeg failed:", r.stderr.strip())
            return 1
        d = open(t.name, "rb").read()
    n = len(d) // fs
    print(f"{n} frame(s) decoded{' (trailing bytes)' if len(d) % fs else ''}")
    worst = float("inf")
    for i in range(n):
        f = d[i * fs:(i + 1) * fs]
        planes = (f[:w * h], f[w * h:w * h * 5 // 4], f[w * h * 5 // 4:])
        res = [psnr(p, s) for p, s in zip(planes, source(w, h, i, a.flat))]
        worst = min(worst, res[0][0])
        print(f"frame {i}: " + "  ".join(
            f"{nm} {p:6.2f} dB (max {m})" for nm, (p, m) in zip("YUV", res)))
    return 0 if n and worst > 35 else 2


if __name__ == "__main__":
    sys.exit(main())
