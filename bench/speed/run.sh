#!/bin/bash
# Encoder speed: per-frame hardware time for H.264 and HEVC at 720p, 1080p
# and 2160p, from the driver's own "enc:" line (Process round trip plus
# result handling, averaged over the frames of one stream; docs/87 §5).
#
#   sudo bench/speed/run.sh LABEL [FRAMES] >> bench/speed/results.csv
#
# Needs the driver loaded (apple-ave-enc) and what tools/v4l2-test.sh needs.
# Fixed QP via v4l2-ctl (no rate control), testsrc2 source. Each point is
# run twice; the second run is kept (the first warms caches and the PMP).
set -u
LABEL=${1:?label, e.g. "M2 (t8112)"}
N=${2:-120}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
echo "# $(date -u +%FT%TZ) $(uname -r) frames=$N" >&2
for codec in h264 hevc; do
    for r in "1280 720 720" "1920 1088 1080" "3840 2160 2160"; do
        set -- $r
        for pass in 1 2; do
            CODEC=$codec W=$1 H=$2 CROP_H=$3 "$HERE/tools/v4l2-test.sh" "$N" ctl >/dev/null 2>&1
        done
        us=$(dmesg | grep 'enc: ' | tail -1 | sed -n 's/.*whole frame avg \([0-9]*\) us.*/\1/p')
        echo "$LABEL,$codec,${1}x$3,$us,$(awk -v u="$us" 'BEGIN { printf "%.1f", 1e6 / u }')"
    done
done
