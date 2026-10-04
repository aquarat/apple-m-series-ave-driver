#!/usr/bin/env python3
"""Charts for the README and docs/91 from the benchmark CSVs in bench/.

  .venv/bin/python bench/plots.py            # writes docs/img/*.svg

Inputs (all in git):
  bench/speed/results.csv              per-frame encode time, by machine/codec/size
  bench/hevc-efficiency/results.csv    M1 Pro: AVE and x265 (docs/88)
  bench/hevc-efficiency/results-m2.csv M2: AVE (docs/91)
  bench/hevc-efficiency/results-m2-{poff,2ref,bframes,bframes2}.csv
                                       M2: P/B QP offsets, two references, B frames (docs/92, 94, 96)
  bench/hevc-efficiency/results-m2-h264{,-vbr}.csv, results-m2-2pass.csv
                                       M2: H.264 fixed QP, 1-pass VBR, 2-pass VBR (docs/95 §11)
  bench/hevc-efficiency/power.log      M1 Pro energy per frame (docs/88)
  bench/hevc-efficiency/power-m2.log   M2 energy per frame (docs/91)
BD-rates use bench.py's PCHIP implementation, the one docs/88's tables use.
"""
import csv, os, re, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import NullFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT = os.path.join(REPO, "docs", "img")
sys.path.insert(0, os.path.join(HERE, "hevc-efficiency"))
import bench  # noqa: E402

XIPH = ["crowd_run", "park_joy", "ducks_take_off", "in_to_tree", "old_town_cross"]
C = {"M1 Pro": "#1f77b4", "M1 Pro + PMP vote": "#0b3d91", "M2": "#d62728",
     "x265 medium": "#7f7f7f", "x265 slow": "#2ca02c", "x265 ultrafast": "#bcbd22",
     "x265 fast": "#9467bd", "B frames": "#ff7f0e"}
# AVE's best measured setting: one B frame per P, B QP = I QP + 3 (docs/96)
BEST_B = "ave-cqp-x-b1-boff3"


def m2_all():
    return [r for f in ("results-m2.csv", "results-m2-poff.csv", "results-m2-2ref.csv",
                        "results-m2-bframes.csv", "results-m2-bframes2.csv")
            for r in rows(os.path.join(HERE, "hevc-efficiency", f))]
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.3,
                     "svg.fonttype": "none", "figure.dpi": 100})


def rows(path):
    return list(csv.DictReader(open(path))) if os.path.exists(path) else []


def save(fig, name):
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, name)
    fig.savefig(p, bbox_inches="tight")
    if os.environ.get("PLOT_PNG_DIR"):   # previews for checking, not committed
        fig.savefig(os.path.join(os.environ["PLOT_PNG_DIR"], name[:-4] + ".png"), bbox_inches="tight", dpi=90)
    plt.close(fig)
    print("wrote", os.path.relpath(p, REPO))


def speed():
    r = rows(os.path.join(HERE, "speed", "results.csv"))
    if not r:
        return
    sizes = ["1280x720", "1920x1080", "3840x2160"]
    series = list(dict.fromkeys((x["machine"], x["codec"]) for x in r))
    fig, ax = plt.subplots(figsize=(8, 4))
    w = 0.8 / len(series)
    for i, (m, c) in enumerate(series):
        v = {x["size"]: float(x["fps"]) for x in r if x["machine"] == m and x["codec"] == c}
        xs = [k + (i - (len(series) - 1) / 2) * w for k in range(len(sizes))]
        ys = [v.get(s, 0) for s in sizes]
        col = C.get(m, "#888")
        b = ax.bar(xs, ys, w, label=f"{m}, {'H.264' if c == 'h264' else 'HEVC'}", color=col,
                   alpha=1.0 if c == "h264" else 0.55, edgecolor=col)
        ax.bar_label(b, labels=[f"{y:.0f}" if y else "" for y in ys], fontsize=8, padding=2)
    ax.axhline(60, color="k", lw=0.8, ls=":")
    ax.text(2.45, 62, "60 fps", fontsize=8, ha="right")
    ax.set_xticks(range(len(sizes)), ["720p", "1080p", "2160p (4K)"])
    ax.set_ylabel("frames per second (hardware, one stream)")
    ax.set_title("AVE encode speed, fixed QP")
    ax.legend(fontsize=8, ncol=2)
    save(fig, "speed.svg")


def pts(rs, clip, enc, metric):
    s = sorted((float(x["kbps"]), float(x[metric])) for x in rs if x["clip"] == clip and x["encoder"] == enc)
    return [a for a, _ in s], [b for _, b in s]


def rd():
    m1 = rows(bench.RESULTS)
    m2 = m2_all()
    fig, axs = plt.subplots(1, len(XIPH), figsize=(16, 3.6), sharey=True)
    for ax, clip in zip(axs, XIPH):
        for rs, enc, lab in ((m1, "x265-medium-crf", "x265 medium"), (m1, "x265-slow-crf", "x265 slow"),
                             (m1, "ave-cqp", "M1 Pro"), (m2, "ave-cqp", "M2"), (m2, BEST_B, "B frames")):
            x, y = pts(rs, clip, enc, "vmaf")
            if x:
                ax.plot(x, y, marker="o", ms=3.5, lw=1.4, color=C[lab],
                        label=f"AVE {lab}" if lab.startswith("M") else
                        "AVE + B frames (B QP +3)" if lab == "B frames" else lab,
                        ls="--" if lab == "M2" else "-")
        ax.set_xscale("log")
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.set_title(clip, fontsize=10)
        ax.set_xlabel("kbit/s")
    axs[0].set_ylabel("VMAF")
    axs[0].legend(fontsize=8, loc="lower right")
    fig.suptitle("HEVC rate-distortion, Xiph 1080p50 clips (AVE fixed QP 22-38, x265 CRF 18-34)", y=1.02)
    save(fig, "rd-vmaf.svg")


def bdrate():
    m1 = rows(bench.RESULTS)
    m2 = m2_all()
    # the M1 Pro and the M2 are identical (docs/91 §2): one bar for both
    combos = [("M1 Pro", m1, "ave-cqp", "AVE, P frames only (M1 Pro = M2)"),
              ("B frames", m2, BEST_B, "AVE + B frames, B QP +3 (docs/96)")]
    fig, axs = plt.subplots(1, 2, figsize=(11, 3.8), sharey=True)
    for ax, (metric, qmax, mname) in zip(axs, (("vmaf", 99.0, "VMAF"), ("psnr_y", None, "PSNR-Y"))):
        w = 0.38
        for i, (lab, rs, enc, name) in enumerate(combos):
            vals = []
            for clip in XIPH:
                r1, q1 = pts(m1, clip, "x265-medium-crf", metric)
                r2, q2 = pts(rs, clip, enc, metric)
                v, ov = bench.bd_rate_pchip(r1, q1, r2, q2, qmax=qmax) if r2 else (float("nan"), 0)
                vals.append(v if ov >= 0.3 else float("nan"))
            xs = [k + (i - 0.5) * w for k in range(len(XIPH))]
            b = ax.bar(xs, [0 if v != v else v for v in vals], w, color=C[lab], label=name)
            ax.bar_label(b, labels=["n/a" if v != v else f"{v:+.0f}%" for v in vals], fontsize=8, padding=2)
        ax.axhline(0, color="k", lw=0.8)
        ax.set_xticks(range(len(XIPH)), [c.replace("_", "\n") for c in XIPH], fontsize=8)
        ax.set_title(f"by {mname}")
    axs[0].set_ylabel("extra bitrate vs x265 medium (CRF)\nfor the same quality (BD-rate)")
    axs[0].legend(fontsize=8)
    fig.suptitle("How much more bitrate AVE HEVC needs than x265 --preset medium", y=1.02)
    save(fig, "bdrate.svg")


def levers():
    """BD-rate of each M2 setting against AVE's plain fixed QP, mean of the 5 clips."""
    m2 = m2_all()
    sets = [("P QP +3", "ave-cqp-poff3"), ("2 refs", "ave-cqp-x-2ref"),
            ("B=1", "ave-cqp-x-b1"), ("B=2", "ave-cqp-x-b2"),
            ("B=1, B QP +1", "ave-cqp-x-b1-boff1"), ("B=1, B QP +2", "ave-cqp-x-b1-boff2"),
            ("B=1, B QP +3\n(default)", BEST_B), ("B=1, B QP +4", "ave-cqp-x-b1-boff4"),
            ("B=2, B QP +4", "ave-cqp-x-b2-boff4")]
    sets = [(l, e) for l, e in sets if any(r["encoder"] == e for r in m2)]
    fig, ax = plt.subplots(figsize=(11, 3.8))
    w = 0.38
    for i, (metric, qmax, mname, col) in enumerate((("vmaf", 99.0, "VMAF", "#ff7f0e"),
                                                    ("psnr_y", None, "PSNR-Y", "#8c564b"))):
        vals = []
        for _, enc in sets:
            v = [bench.bd_rate_pchip(*pts(m2, c, "ave-cqp", metric), *pts(m2, c, enc, metric), qmax=qmax)[0]
                 for c in XIPH]
            vals.append(sum(v) / len(v))
        b = ax.bar([k + (i - 0.5) * w for k in range(len(sets))], vals, w, color=col, label=f"by {mname}")
        ax.bar_label(b, labels=[f"{v:+.1f}%" for v in vals], fontsize=7, padding=2)
    ax.axhline(0, color="k", lw=0.8)
    ax.set_xticks(range(len(sets)), [l for l, _ in sets], fontsize=8)
    ax.set_ylabel("bitrate vs AVE P frames only\nfor the same quality (BD-rate)")
    ax.legend(fontsize=8)
    ax.set_title("AVE HEVC compression settings on the M2, mean of five Xiph clips (negative = fewer bits)")
    save(fig, "levers.svg")


def multipass():
    """H.264 on the M2: 2-pass against 1-pass VBR, and how close each lands to its target."""
    rs = [r for f in ("results-m2-h264.csv", "results-m2-h264-vbr.csv", "results-m2-2pass.csv")
          for r in rows(os.path.join(HERE, "hevc-efficiency", f))]
    if not any(r["encoder"] == "ave264-2pass" for r in rs):
        return
    fig, axs = plt.subplots(1, 2, figsize=(12, 3.8))
    w = 0.38
    ax = axs[0]
    for i, (metric, qmax, mname, col) in enumerate((("vmaf", 99.0, "VMAF", "#ff7f0e"),
                                                    ("psnr_y", None, "PSNR-Y", "#8c564b"))):
        vals = [bench.bd_rate_pchip(*pts(rs, c, "ave264-vbr", metric), *pts(rs, c, "ave264-2pass", metric),
                                    qmax=qmax)[0] for c in XIPH]
        b = ax.bar([k + (i - 0.5) * w for k in range(len(XIPH))], vals, w, color=col, label=f"by {mname}")
        ax.bar_label(b, labels=[f"{v:+.0f}%" for v in vals], fontsize=8, padding=2)
    ax.axhline(0, color="k", lw=0.8)
    ax.set_xticks(range(len(XIPH)), [c.replace("_", "\n") for c in XIPH], fontsize=8)
    ax.set_ylabel("2-pass bitrate vs 1-pass VBR\nat equal quality (BD-rate)")
    ax.set_title("2-pass against 1-pass (positive = 2-pass needs more bits)")
    ax.legend(fontsize=8)
    ax = axs[1]
    for i, (enc, lab, col) in enumerate((("ave264-vbr", "1-pass VBR", "#9467bd"),
                                         ("ave264-2pass", "2-pass VBR", "#2ca02c"))):
        errs = []
        for clip in XIPH:
            e = [100 * (float(r["kbps"]) / float(r["target_kbps"]) - 1) for r in rs
                 if r["clip"] == clip and r["encoder"] == enc]
            errs.append(sum(abs(x) for x in e) / len(e) if e else float("nan"))
        b = ax.bar([k + (i - 0.5) * w for k in range(len(XIPH))], errs, w, color=col, label=lab)
        ax.bar_label(b, labels=[f"{v:.0f}%" for v in errs], fontsize=8, padding=2)
    ax.set_xticks(range(len(XIPH)), [c.replace("_", "\n") for c in XIPH], fontsize=8)
    ax.set_title("miss of the bitrate target (mean |error|, 2-16 Mbit/s)")
    ax.set_ylabel("%")
    ax.legend(fontsize=8)
    fig.suptitle("AVE H.264 on the M2: the firmware's 2-pass mode against its 1-pass VBR (docs/95 §11)", y=1.02)
    save(fig, "multipass.svg")


def energy():
    def parse(path):
        out = {}
        if not os.path.exists(path):
            return out
        for line in open(path):
            m = re.match(r"(\S+)\s+(\d+)k\s+(\S+)\s+[\d.]+\s+[\d.]+\s+([\d.]+)\s+([\d.]+)", line)
            if m:
                out[(m[1], m[3])] = (float(m[4]), float(m[5]))
        return out
    m1 = parse(os.path.join(HERE, "hevc-efficiency", "power.log"))
    m2 = parse(os.path.join(HERE, "hevc-efficiency", "power-m2.log"))
    bars = [("x265 medium", m1.get(("crowd_run", "x265-medium"))),
            ("x265 fast", m1.get(("crowd_run", "x265-fast"))),
            ("x265 ultrafast", m1.get(("crowd_run", "x265-ultrafast"))),
            ("AVE M1 Pro", m1.get(("crowd_run", "ave"))),
            ("AVE M2", m2.get(("crowd_run", "ave")))]
    bars = [(n, v) for n, v in bars if v]
    fig, ax = plt.subplots(figsize=(7, 3.2))
    cols = [C.get(n.replace("AVE ", ""), C.get(n, "#888")) for n, _ in bars]
    b = ax.barh([n for n, _ in bars], [v[1] * 1000 for _, v in bars], color=cols)
    ax.bar_label(b, labels=[f"{v[1] * 1000:.0f} mJ  ({v[0]:.0f} fps)" for _, v in bars], fontsize=8, padding=3)
    ax.set_xlabel("energy per 1080p frame above idle, whole machine (mJ)")
    ax.set_title("crowd_run 1080p50 at 8 Mbit/s, HEVC (x265 on the M1 Pro's CPU)")
    ax.invert_yaxis()
    ax.set_xlim(0, max(v[1] for _, v in bars) * 1000 * 1.35)
    save(fig, "energy.svg")


def device():
    """Quality per bitrate per hardware device: the same encoder on the M1 Pro and the M2."""
    m1 = rows(bench.RESULTS)
    m2 = rows(os.path.join(HERE, "hevc-efficiency", "results-m2.csv"))
    if not m2:
        return
    fig, axs = plt.subplots(1, len(XIPH), figsize=(16, 3.6), sharey=True)
    print("BD-rate, AVE M2 vs AVE M1 Pro (positive = the M2 needs more bitrate)")
    for ax, clip in zip(axs, XIPH):
        line = f"  {clip:15s}"
        for enc, lab, ls in (("ave-cqp", "8-bit", "-"), ("ave-cqp-main10", "Main10 (P010 source)", ":")):
            for dev, rs in (("M1 Pro", m1), ("M2", m2)):
                x, y = pts(rs, clip, enc, "vmaf")
                if x:
                    ax.plot(x, y, marker="o" if dev == "M1 Pro" else "x", ms=4, lw=1.3, ls=ls,
                            color=C[dev], label=f"{dev}, {lab}")
            for metric, qmax in (("vmaf", 99.0), ("psnr_y", None)):
                v, ov = bench.bd_rate_pchip(*pts(m1, clip, enc, metric), *pts(m2, clip, enc, metric), qmax=qmax)
                line += f"  {enc} {metric} {v:+6.2f}% [{ov:.0%}]"
        print(line)
        ax.set_xscale("log")
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.set_title(clip, fontsize=10)
        ax.set_xlabel("kbit/s")
    axs[0].set_ylabel("VMAF")
    axs[0].legend(fontsize=7, loc="lower right")
    fig.suptitle("AVE HEVC per device: M1 Pro (t6000) vs M2 (t8112), same QP ladder. "
                 "The curves coincide: all 50 points are identical (BD-rate 0.00 %)", y=1.02)
    save(fig, "rd-per-device.svg")


if __name__ == "__main__":
    speed()
    rd()
    device()
    bdrate()
    levers()
    multipass()
    energy()
