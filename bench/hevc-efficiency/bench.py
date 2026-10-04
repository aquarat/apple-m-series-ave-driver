#!/usr/bin/env python3
"""HEVC benchmark: Apple AVE (hevc_v4l2m2m) vs x265, rate-distortion + speed.

  bench.py run   CLIP [CLIP...]   encode + measure, appends to results.csv
  bench.py bd                     BD-rate tables from results.csv

Every encoder gets the same raw 8-bit 4:2:0 input, the same target bitrates
(1-pass ABR) and the same keyframe interval. Quality is measured on the
decoded output against the source: PSNR (Y, and all planes 4:1:1 weighted),
SSIM (ffmpeg), VMAF v0.6.1 (libvmaf 3.0.0 CLI).
"""
import csv, json, os, re, resource, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
E = os.environ.get
WORK = E("BENCH_WORK", os.path.join(HERE, "work"))      # large files live here, not in git
SRC = os.path.join(WORK, "src-yuv")                     # <clip>.yuv (I420) and <clip>.nv12
OUT = os.path.join(WORK, "out")
FF_ENC = E("FF_ENC", "ffmpeg")                          # needs hevc_v4l2m2m (Fedora's has it)
FF = E("FF_DEC", "ffmpeg")                              # needs an HEVC decoder (Fedora's has none)
X265 = E("X265", os.path.join(WORK, "src/x265/build-arm/x265"))      # x265 4.1, 8-bit
X265_10 = E("X265_10", os.path.join(WORK, "src/x265/build-10/x265"))  # x265 4.1, HIGH_BIT_DEPTH
VMAF = E("VMAF", os.path.join(WORK, "src/vmaf/libvmaf/build/tools/vmaf"))  # libvmaf 3.0.0 CLI
VMAF_MODEL = E("VMAF_MODEL", os.path.join(WORK, "src/vmaf/model/vmaf_v0.6.1.json"))
KEYINT = 250
RESULTS = E("RESULTS", os.path.join(HERE, "results.csv"))

# name: (yuv basename, w, h, fps, frames, target kbit/s ladder)
# cam-a / cam-b: two private H.264 surveillance-camera recordings (High,
# ~1.1-1.2 Mbit/s), decoded to raw. Not distributable; their rows in
# results.csv are kept as the "already-compressed source" case.
CLIPS = {
    "cam-a":          ("cam-a_2560x1440_20", 2560, 1440, 20, 200, [500, 1000, 2000, 4000]),
    "cam-b":          ("cam-b_2304x1296_15", 2304, 1296, 15, 181, [500, 1000, 2000, 4000]),
}
for c in ("crowd_run", "park_joy", "ducks_take_off", "in_to_tree", "old_town_cross"):
    CLIPS[c] = (f"derf/{c}_1080p50_200f", 1920, 1080, 50, 200, [2000, 4000, 8000, 16000])

ENCODERS = ["ave", "x265-ultrafast", "x265-fast", "x265-medium", "x265-medium-noB",
            "ave-cqp", "x265-medium-crf", "x265-slow-crf",
            "ave-cqp-main10", "ave-cqp-p010", "x265-medium-crf-10bit"]
# Quality-targeted modes use their own ladder instead of the clip's bitrate ladder:
# AVE fixed QP (same QP for I and P; the driver has no separate P QP), x265 CRF.
QUALITY_LADDER = {"ave-cqp": [22, 26, 30, 34, 38],
                  # 10-bit HEVC QP = 8-bit QP + 12 (QpBdOffset = 6 * (bitdepth - 8))
                  "ave-cqp-main10": [34, 38, 42, 46, 50],
                  "ave-cqp-p010": [34, 38, 42, 46, 50],
                  "x265-medium-crf-10bit": [18, 22, 26, 30, 34],
                  "x265-medium-crf": [18, 22, 26, 30, 34],
                  "x265-slow-crf": [18, 22, 26, 30, 34]}


def base_enc(enc):
    """ave-cqp-poff<N>[-<tag>]: ave-cqp with P QP = QP + N (docs/92); ave-cqp-x-<tag>:
    tags -b<N> (B frames) and -boff<N> (B QP offset, docs/96), or an AVE_EXTRA_CTRLS
    experiment. All use ave-cqp's QP ladder."""
    return "ave-cqp" if enc.startswith("ave-cqp-poff") or enc.startswith("ave-cqp-x-") else enc


NICE = ["nice", "-n", "10"]   # software encodes yield to whatever else the machine runs


def child_cpu():
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


def ave_dev():
    for n in sorted(os.listdir("/sys/class/video4linux")):
        if open(f"/sys/class/video4linux/{n}/name").read().strip() == "apple-ave-enc":
            return f"/dev/{n}"
    raise RuntimeError("no apple-ave-enc node")


def p010_padded(clip):
    """The clip as P010 in the driver's OUTPUT layout: stride 2*ALIGN(w,64), luma
    rows = the 16-aligned buffer height, chroma rows ALIGN(h16,64)/2, the h-line
    picture at the top (the crop). 8-bit samples become v<<6 (x<<2 in 10 bits)."""
    base, w, h, fps, n, _ = CLIPS[clip]
    dst = f"{SRC}/{base}.p010pad"
    if os.path.exists(dst):
        return dst
    h16 = (h + 15) // 16 * 16
    bpl = (w + 63) // 64 * 64 * 2
    luma, chroma = bpl * h16, bpl * ((h16 + 63) // 64 * 64 // 2)
    p = subprocess.Popen([FF, "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{w}x{h}",
                          "-i", f"{SRC}/{base}.yuv", "-pix_fmt", "p010le", "-f", "rawvideo", "-"], stdout=subprocess.PIPE)
    fl, fc = w * 2 * h, w * 2 * (h // 2)
    with open(dst + ".tmp", "wb") as o:
        for _ in range(n):
            y, uv = p.stdout.read(fl), p.stdout.read(fc)
            for plane, rows, size in ((y, h, luma), (uv, h // 2, chroma)):
                buf = bytearray(size)
                for r in range(rows):
                    buf[r * bpl:r * bpl + w * 2] = plane[r * w * 2:(r + 1) * w * 2]
                o.write(buf)
    p.wait()
    os.rename(dst + ".tmp", dst)
    return dst


def encode(enc, clip, kbps, out):
    base, w, h, fps, n, _ = CLIPS[clip]
    if enc == "ave" and h % 16:
        # ffmpeg's V4L2 m2m wrapper segfaults when the driver rounds the height
        # up (1080 -> 1088). Feed it a 16-aligned frame whose extra lines
        # repeat the last row (cheap to code); measure() crops back to h.
        h16 = (h + 15) // 16 * 16
        cmd = [FF_ENC, "-hide_banner", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
               "-s", f"{w}x{h}", "-r", str(fps), "-i", f"{SRC}/{base}.yuv", "-frames:v", str(n),
               "-vf", f"pad={w}:{h16}:0:0,fillborders=bottom={h16 - h}:mode=smear,format=nv12",
               "-c:v", "hevc_v4l2m2m", "-b:v", f"{kbps}k", "-g", str(KEYINT), "-f", "hevc", out]
    elif enc == "ave":
        cmd = [FF_ENC, "-hide_banner", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "nv12",
               "-s", f"{w}x{h}", "-r", str(fps), "-i", f"{SRC}/{base}.nv12", "-frames:v", str(n),
               "-c:v", "hevc_v4l2m2m", "-b:v", f"{kbps}k", "-g", str(KEYINT), "-f", "hevc", out]
    elif enc in ("ave-cqp-main10", "ave-cqp-p010"):
        # Main 10: NV12 in with the profile set (8-bit in, 10-bit coding), or
        # P010 in (which implies Main 10). Otherwise as ave-cqp.
        h16 = (h + 15) // 16 * 16
        p010 = enc.endswith("p010")
        cmd = ["v4l2-ctl", "-d", ave_dev(),
               f"--set-fmt-video-out=width={w},height={h16},pixelformat={'P010' if p010 else 'NV12'}",
               "--set-fmt-video=pixelformat=HEVC",
               f"--set-selection-output=target=crop,width={w},height={h}",
               f"--set-ctrl=video_gop_size={KEYINT},hevc_i_frame_qp_value={kbps},frame_level_rate_control_enable=0,hevc_profile=2",
               "--stream-mmap", "--stream-out-mmap",
               f"--stream-from={p010_padded(clip) if p010 else SRC + '/' + base + '.nv12'}",
               f"--stream-to={out}", f"--stream-count={n}"]
    elif base_enc(enc) == "ave-cqp":
        # v4l2-ctl, rate control off (the driver's default): fixed QP. A
        # 16-unaligned height goes through the OUTPUT crop, which v4l2-ctl
        # fills from h-line frames of the NV12 file.
        h16 = (h + 15) // 16 * 16
        cmd = ["v4l2-ctl", "-d", ave_dev(),
               f"--set-fmt-video-out=width={w},height={h16},pixelformat=NV12",
               "--set-fmt-video=pixelformat=HEVC",
               f"--set-selection-output=target=crop,width={w},height={h}",
               f"--set-ctrl=video_gop_size={KEYINT},hevc_i_frame_qp_value={kbps},frame_level_rate_control_enable=0"
               + (f",hevc_p_frame_qp_value={min(51, kbps + int(re.match(r'ave-cqp-poff(\d+)', enc)[1]))}"
                  if enc.startswith("ave-cqp-poff") else "")
               # -bN: N B frames per mini-GOP; -boffN: B QP = QP + N (docs/96)
               + (f",video_b_frames={re.search(r'-b(\d)(?!\w)', enc)[1]}" if re.search(r'-b(\d)(?!\w)', enc) else "")
               + (f",hevc_b_frame_qp_value={min(51, kbps + int(re.search(r'-boff(\d+)', enc)[1]))}"
                  if re.search(r'-boff(\d+)', enc) else "")
               + (("," + os.environ["AVE_EXTRA_CTRLS"]) if os.environ.get("AVE_EXTRA_CTRLS") else ""),
               "--stream-mmap", "--stream-out-mmap", f"--stream-from={SRC}/{base}.nv12",
               f"--stream-to={out}", f"--stream-count={n}"]
    else:
        preset = enc.split("-")[1]   # x265-<preset>[-crf][-noB][-10bit]
        cmd = NICE + [X265_10 if enc.endswith("-10bit") else X265, "--input", f"{SRC}/{base}.yuv",
                      *(["--input-depth", "8", "--output-depth", "10"] if enc.endswith("-10bit") else []), "--input-res", f"{w}x{h}", "--fps", str(fps),
                      "--frames", str(n), "--preset", preset,
                      *(["--crf", str(kbps)] if "-crf" in enc else ["--bitrate", str(kbps)]),
                      "--keyint", str(KEYINT), "--no-progress", "--log-level", "error", "-o", out]
        if "-noB" in enc:
            cmd += ["--bframes", "0"]
    c0, t0 = child_cpu(), time.monotonic()
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL if enc.startswith("ave-cqp") else None)
    return time.monotonic() - t0, child_cpu() - c0


def measure(clip, out):
    base, w, h, fps, n, _ = CLIPS[clip]
    ref = f"{SRC}/{base}.yuv"
    with tempfile.NamedTemporaryFile(dir=OUT, suffix=".yuv") as dec:
        # 10-bit outputs are scored at 8 bits against the 8-bit source; round,
        # don't dither (swscale's default dither lowers PSNR; -sws_dither none
        # rounds exactly, checked on a random 10-bit ramp)
        subprocess.run([FF, "-v", "error", "-y", "-i", out, "-frames:v", str(n), "-vf", f"crop={w}:{h}:0:0",
                        "-sws_dither", "none", "-pix_fmt", "yuv420p", "-f", "rawvideo", dec.name], check=True)
        got = os.path.getsize(dec.name) // (w * h * 3 // 2)
        if got != n:
            raise RuntimeError(f"{out}: decoded {got} frames, expected {n}")
        raw = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{w}x{h}"]
        r = subprocess.run(NICE + [FF, "-hide_banner"] + raw + ["-i", dec.name] + raw + ["-i", ref] +
                           ["-lavfi", "[0:v][1:v]psnr;[0:v][1:v]ssim", "-f", "null", "-"],
                           capture_output=True, text=True, check=True).stderr
        m = re.search(r"PSNR y:([\d.]+) u:([\d.]+) v:([\d.]+) average:([\d.]+)", r)
        s = re.search(r"SSIM .*All:([\d.]+)", r)
        with tempfile.NamedTemporaryFile(dir=OUT, suffix=".json") as js:
            subprocess.run(NICE + [VMAF, "-r", ref, "-d", dec.name, "-w", str(w), "-h", str(h), "-p", "420",
                                   "-b", "8", "-m", f"path={VMAF_MODEL}", "--threads", "4", "--json",
                                   "-o", js.name, "-q"], check=True, capture_output=True)
            vmaf = json.load(open(js.name))["pooled_metrics"]["vmaf"]["mean"]
    return dict(psnr_y=float(m[1]), psnr_u=float(m[2]), psnr_v=float(m[3]), psnr_avg=float(m[4]),
                ssim=float(s[1]), vmaf=vmaf)


def run(clips, encoders):
    new = not os.path.exists(RESULTS)
    with open(RESULTS, "a", newline="") as f:
        wr = csv.writer(f)
        if new:
            wr.writerow(["clip", "encoder", "target_kbps", "kbps", "wall_s", "cpu_s", "fps",
                         "psnr_y", "psnr_avg", "ssim", "vmaf"])
        for clip in clips:
            base, w, h, fps, n, ladder = CLIPS[clip]
            for enc in encoders:
                for kbps in QUALITY_LADDER.get(base_enc(enc), ladder):
                    out = f"{OUT}/{clip}.{enc}.{kbps}.hevc"
                    wall, cpu = encode(enc, clip, kbps, out)
                    rate = os.path.getsize(out) * 8 * fps / n / 1000
                    q = measure(clip, out)
                    row = [clip, enc, kbps, f"{rate:.1f}", f"{wall:.2f}", f"{cpu:.2f}", f"{n / wall:.1f}",
                           f"{q['psnr_y']:.3f}", f"{q['psnr_avg']:.3f}", f"{q['ssim']:.5f}", f"{q['vmaf']:.3f}"]
                    wr.writerow(row)
                    f.flush()
                    print(" ".join(map(str, row)), flush=True)


# --- Bjontegaard delta rate (cubic fit of log-rate vs quality; VCEG-M33) ---
def polyfit3(x, y):
    # least squares for y = a0 + a1 x + a2 x^2 + a3 x^3 via normal equations
    A = [[sum(xi ** (i + j) for xi in x) for j in range(4)] for i in range(4)]
    b = [sum(yi * xi ** i for xi, yi in zip(x, y)) for i in range(4)]
    for c in range(4):                       # Gaussian elimination with pivoting
        p = max(range(c, 4), key=lambda r: abs(A[r][c]))
        A[c], A[p], b[c], b[p] = A[p], A[c], b[p], b[c]
        for r in range(c + 1, 4):
            k = A[r][c] / A[c][c]
            A[r] = [A[r][i] - k * A[c][i] for i in range(4)]
            b[r] -= k * b[c]
    a = [0.0] * 4
    for r in range(3, -1, -1):
        a[r] = (b[r] - sum(A[r][i] * a[i] for i in range(r + 1, 4))) / A[r][r]
    return a


def integ(a, lo, hi):
    F = lambda x: sum(a[i] * x ** (i + 1) / (i + 1) for i in range(4))
    return F(hi) - F(lo)


def bd_rate(r1, q1, r2, q2):
    """% bitrate change of set 2 relative to set 1 at equal quality (negative = 2 is better)."""
    import math
    l1, l2 = [math.log10(r) for r in r1], [math.log10(r) for r in r2]
    a1, a2 = polyfit3(q1, l1), polyfit3(q2, l2)
    lo, hi = max(min(q1), min(q2)), min(max(q1), max(q2))
    if hi <= lo:
        return float("nan")
    avg = (integ(a2, lo, hi) - integ(a1, lo, hi)) / (hi - lo)
    return (10 ** avg - 1) * 100


def bd(ref_encs=None, test_encs=("ave", "ave-cqp"), metrics=("psnr_y", "vmaf")):
    rows = list(csv.DictReader(open(RESULTS)))
    clips = list(dict.fromkeys(r["clip"] for r in rows))
    have = set(r["encoder"] for r in rows)
    refs = [e for e in (ref_encs or ENCODERS) if e in have and not e.startswith("ave")]

    def pts(clip, e, metric):
        s = sorted((float(r["kbps"]), float(r[metric])) for r in rows if r["clip"] == clip and r["encoder"] == e)
        return [p[0] for p in s], [p[1] for p in s]

    for test in [t for t in test_encs if t in have]:
        for metric in metrics:
            print(f"\nBD-rate of {test} vs x265, {metric} (positive = {test} needs that much MORE bitrate for equal quality)")
            print(f"{'clip':16s}" + "".join(f"{e:>17s}" for e in refs))
            per = {e: [] for e in refs}
            for clip in clips:
                rt, qt = pts(clip, test, metric)
                line = f"{clip:16s}"
                for e in refs:
                    rx, qx = pts(clip, e, metric)
                    if len(rx) >= 4 and len(rt) >= 4:
                        v = bd_rate(rx, qx, rt, qt)
                        per[e].append(v)
                        line += f"{v:>16.1f}%"
                    else:
                        line += f"{'-':>17s}"
                print(line)
            print(f"{'mean':16s}" + "".join(f"{(sum(v) / len(v) if v else float('nan')):>16.1f}%" for v in per.values()))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if sys.argv[1] == "run":
        encs = os.environ.get("ENCODERS", ",".join(ENCODERS)).split(",")
        run(sys.argv[2:], encs)
    elif sys.argv[1] == "bd":
        bd()


# --- BD-rate with monotone piecewise-cubic (PCHIP, Fritsch-Carlson) interpolation,
# as in the JVET/AOM BD-rate tools; robust near metric saturation. ---
def _pchip(xs, ys):
    n = len(xs)
    h = [xs[i + 1] - xs[i] for i in range(n - 1)]
    d = [(ys[i + 1] - ys[i]) / h[i] for i in range(n - 1)]
    m = [0.0] * n
    m[0], m[-1] = d[0], d[-1]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] <= 0:
            m[i] = 0.0
        else:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])

    def f(x):
        i = max(0, min(n - 2, max(k for k in range(n - 1) if xs[k] <= x) if x >= xs[0] else 0))
        t = (x - xs[i]) / h[i]
        return ((2 * t**3 - 3 * t**2 + 1) * ys[i] + (t**3 - 2 * t**2 + t) * h[i] * m[i]
                + (-2 * t**3 + 3 * t**2) * ys[i + 1] + (t**3 - t**2) * h[i] * m[i + 1])
    return f


def bd_rate_pchip(r1, q1, r2, q2, qmax=None, steps=1000):
    """% bitrate of set 2 vs set 1 at equal quality; points with quality >= qmax dropped (saturation)."""
    import math
    def prep(r, q):
        p = sorted((qq, math.log10(rr)) for rr, qq in zip(r, q) if qmax is None or qq < qmax)
        return [a for a, _ in p], [b for _, b in p]
    x1, y1 = prep(r1, q1); x2, y2 = prep(r2, q2)
    if len(x1) < 2 or len(x2) < 2:
        return float("nan"), 0.0
    lo, hi = max(x1[0], x2[0]), min(x1[-1], x2[-1])
    if hi <= lo:
        return float("nan"), 0.0
    f1, f2 = _pchip(x1, y1), _pchip(x2, y2)
    xs = [lo + (hi - lo) * k / steps for k in range(steps + 1)]
    diff = sum((f2(x) - f1(x)) for x in xs) / len(xs)
    overlap = (hi - lo) / max(x1[-1] - x1[0], x2[-1] - x2[0])
    return (10 ** diff - 1) * 100, overlap


def report(pairs, metrics=(("psnr_y", None), ("vmaf", 99.0)), groups=None):
    rows = list(csv.DictReader(open(RESULTS)))
    clips = list(dict.fromkeys(r["clip"] for r in rows))
    def pts(clip, e, metric):
        s = [(float(r["kbps"]), float(r[metric])) for r in rows if r["clip"] == clip and r["encoder"] == e]
        return [p[0] for p in s], [p[1] for p in s]
    for metric, qmax in metrics:
        print(f"\nPCHIP BD-rate, {metric}{' (points >= %g dropped)' % qmax if qmax else ''}: "
              f"test vs reference, positive = test needs MORE bitrate; [overlap of quality ranges]")
        print(f"{'clip':15s}" + "".join(f"{t + ' vs ' + r:>34s}" for t, r in pairs))
        acc = {p: {} for p in pairs}
        for clip in clips:
            line = f"{clip:15s}"
            for t, r in pairs:
                v, ov = bd_rate_pchip(*pts(clip, r, metric), *pts(clip, t, metric), qmax=qmax)
                acc[(t, r)][clip] = (v, ov)
                line += f"{v:>25.1f}% [{ov:4.0%}]" if v == v else f"{'n/a':>34s}"
            print(line)
        for gname, gclips in (groups or {}).items():
            line = f"{'mean ' + gname:15s}"
            for p in pairs:
                vals = [acc[p][c][0] for c in gclips if c in acc[p] and acc[p][c][0] == acc[p][c][0] and acc[p][c][1] >= 0.3]
                line += f"{(sum(vals) / len(vals)) if vals else float('nan'):>25.1f}%  (n={len(vals)})" if vals else f"{'n/a':>34s}"
            print(line)
