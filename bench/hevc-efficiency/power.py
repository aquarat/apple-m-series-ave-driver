#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Energy per frame: AVE vs x265 presets, measured at the wall (SMC 'Total System Power').

For each (clip, encoder) the same encode as bench.py is looped for PHASE seconds;
the SMC sensor (updates ~1 Hz) is sampled every 0.25 s. Idle is measured before
and after (the machine's other work keeps running throughout). Energy per frame
= (mean power - idle) * elapsed / frames encoded.

POWER_TESTS="crowd_run:8000,..." and POWER_ENCS="ave,..." override the lists
(the M2 run, docs/90 §11, used POWER_TESTS=crowd_run:8000 POWER_ENCS=ave).
"""
import os, statistics, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench

def find_sensor():
    """macsmc_hwmon's "Total System Power" (µW): hwmon3 on the M1 Pro, hwmon1 on the M2."""
    base = "/sys/class/hwmon"
    for h in sorted(os.listdir(base)):
        for f in os.listdir(os.path.join(base, h)):
            if f.endswith("_label"):
                p = os.path.join(base, h, f)
                if open(p).read().strip().lower() == "total system power":
                    return p[:-len("_label")] + "_input"
    sys.exit("no 'Total System Power' hwmon sensor (macsmc_hwmon)")


SENSOR = find_sensor()
PHASE = 60
TESTS = [(c, int(k)) for c, k in (t.split(":") for t in
         os.environ.get("POWER_TESTS", "cam-a:2000,crowd_run:8000").split(","))]
ENCS = os.environ.get("POWER_ENCS", "ave,x265-ultrafast,x265-fast,x265-medium").split(",")


def watts():
    return int(open(SENSOR).read()) / 1e6


def sample(stop, out):
    while not stop.is_set():
        out.append(watts())
        time.sleep(0.25)


def phase(fn):
    samples, stop = [], threading.Event()
    t = threading.Thread(target=sample, args=(stop, samples)); t.start()
    t0 = time.monotonic(); frames, cpu0 = fn(), bench.child_cpu()
    el = time.monotonic() - t0
    stop.set(); t.join()
    return statistics.mean(samples), el, frames


def idle():
    time.sleep(PHASE)
    return 0


def loop(enc, clip, kbps):
    n = bench.CLIPS[clip][4]
    def run():
        frames, t0 = 0, time.monotonic()
        while time.monotonic() - t0 < PHASE:
            bench.encode(enc, clip, kbps, f"{bench.OUT}/power.{enc}.hevc")
            frames += n
        return frames
    return run


print(f"{'test':34s} {'W mean':>7s} {'W over idle':>11s} {'fps':>7s} {'J/frame':>8s} {'CPU s/frame':>11s}")
i0 = phase(idle)[0]
print(f"{'idle (before)':34s} {i0:7.2f}", flush=True)
res = []
for clip, kbps in TESTS:
    for enc in ENCS:
        c0 = bench.child_cpu()
        w, el, frames = phase(loop(enc, clip, kbps))
        cpu = (bench.child_cpu() - c0) / frames
        res.append((f"{clip} {kbps}k {enc}", w, el, frames, cpu))
        print(f"{clip + ' ' + str(kbps) + 'k ' + enc:34s} {w:7.2f} {w - i0:11.2f} {frames / el:7.1f} "
              f"{(w - i0) * el / frames:8.3f} {cpu:11.4f}", flush=True)
i1 = phase(idle)[0]
print(f"{'idle (after)':34s} {i1:7.2f}   (J/frame above use the before-idle; with the mean idle "
      f"{(i0 + i1) / 2:.2f} W they shift by {(i0 - (i0 + i1) / 2):+.2f} W x time/frame)")
