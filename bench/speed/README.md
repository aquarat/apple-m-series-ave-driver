# bench/speed

`run.sh LABEL [FRAMES]` encodes `testsrc2` through `tools/v4l2-test.sh` (fixed
QP, v4l2-ctl) as H.264 and HEVC at 720p, 1080p (1088 buffer, 1080 crop) and
2160p, twice each, and prints the driver's `enc:` per-frame time of the
second run as `machine,codec,size,us_per_frame,fps`. Append to
`results.csv`; `bench/plots.py` draws it (docs/91).

The M1 Pro rows were not produced by this script: they are docs/87 §5 (t1 no
vote, t3 PMP vote; H.264, 60 frames, 1920x1088 for 1080p).
