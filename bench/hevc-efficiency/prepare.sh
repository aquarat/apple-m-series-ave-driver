#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Fetch the public test clips and convert them to what bench.py reads.
#
#   BENCH_WORK=/big/disk bench/hevc-efficiency/prepare.sh
#
# Xiph "derf" SVT 1080p50 sequences (https://media.xiph.org/video/derf/y4m/),
# frames 0-199 of each (4 s, 8-bit 4:2:0), fetched with HTTP range requests
# (~620 MB each instead of 1.55 GB). Writes $BENCH_WORK/src-yuv/derf/
# <name>_1080p50_200f.{yuv,nv12}. Needs curl and an ffmpeg (FF_DEC).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${BENCH_WORK:-$HERE/work}
FF=${FF_DEC:-ffmpeg}
D=$WORK/src-yuv/derf
mkdir -p "$D" && cd "$D"
FB=$((1920 * 1080 * 3 / 2 + 6))   # "FRAME\n" + one frame
for c in crowd_run park_joy ducks_take_off in_to_tree old_town_cross; do
    if [ ! -s "${c}_1080p50_200f.yuv" ]; then
        url=https://media.xiph.org/video/derf/y4m/${c}_1080p50.y4m
        hl=$(curl -s -r 0-199 "$url" | head -1 | wc -c)
        end=$((hl + 200 * FB - 1))
        curl -s --retry 5 -r 0-$end -o "$c.y4m" "$url"
        [ "$(stat -c%s "$c.y4m")" -eq $((end + 1)) ] || { echo "$c: short download"; exit 1; }
        "$FF" -v error -y -i "$c.y4m" -pix_fmt yuv420p -f rawvideo "${c}_1080p50_200f.yuv"
        rm -f "$c.y4m"
    fi
    [ -s "${c}_1080p50_200f.nv12" ] || "$FF" -v error -y -f rawvideo -pix_fmt yuv420p -s 1920x1080 \
        -i "${c}_1080p50_200f.yuv" -pix_fmt nv12 -f rawvideo "${c}_1080p50_200f.nv12"
    echo "$c: $(( $(stat -c%s "${c}_1080p50_200f.yuv") / (1920 * 1080 * 3 / 2) )) frames"
done
