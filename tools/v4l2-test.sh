#!/bin/bash
# On the target, with apple-ave loaded v4l2=1: encode N frames of a test
# pattern through the V4L2 node and grade the result against the input.
#   [W=1920 H=1088] [CROP_H=1080] [FFARGS="-b:v 8M"] tools/v4l2-test.sh [frames] [ctl|ffmpeg]
# ctl: v4l2-ctl, fixed QP (RC off). ffmpeg: h264_v4l2m2m, which always turns
# RC on, so it gets FFARGS' bitrate (default 8M; ffmpeg's own default is 200k).
set -u
N=${1:-60}; MODE=${2:-ctl}
W=${W:-1280}; H=${H:-720}
# CROP_H=1080 with H=1088: encode a 1080-line picture from a 1088-line buffer
CROP_H=${CROP_H:-$H}
D=$HOME/ave-test; mkdir -p "$D"; cd "$D"
# v4l2-ctl reads frames of the crop size when an OUTPUT crop is set and
# pads them into the buffer (f71/f72 fed it 1088-line frames and graded the
# resulting misalignment as a broken encode), so the file is W x CROP_H.
IN=testsrc2-${W}x${CROP_H}-$N.nv12
[ -s "$IN" ] || ffmpeg -v error -f lavfi -i testsrc2=size=${W}x${CROP_H}:rate=30 \
    -frames:v "$N" -pix_fmt nv12 -f rawvideo "$IN"
DEV=
for n in /sys/class/video4linux/video*/name; do
    grep -q apple-ave-enc "$n" && DEV=/dev/$(basename "$(dirname "$n")")
done
[ -n "$DEV" ] || { echo "no apple-ave-enc node"; exit 1; }
echo "node $DEV"
v4l2-ctl -d "$DEV" --info | head -8
v4l2-ctl -d "$DEV" --list-formats-out --list-formats | grep -E "\[|NV12|H264"
rm -f out.h264 out.mp4
case $MODE in
ctl)
    timeout 60 v4l2-ctl -d "$DEV" \
        --set-fmt-video-out=width=$W,height=$H,pixelformat=NV12 \
        --set-fmt-video=pixelformat=H264 \
        --set-selection-output=target=crop,width=$W,height=$CROP_H \
        --stream-mmap --stream-out-mmap --stream-from="$IN" \
        --stream-to=out.h264 --stream-count="$N" 2>&1 | tail -3
    OUT=out.h264; FMT="-f h264 -framerate 30" ;;
ffmpeg)
    timeout 60 ffmpeg -v warning -y -f rawvideo -pix_fmt nv12 -s ${W}x$H -r 30 \
        -i "$IN" -c:v h264_v4l2m2m ${FFARGS:--b:v 8M} out.mp4 2>&1 | tail -5
    OUT=out.mp4; FMT= ;;
esac
ls -l "$OUT"
ffprobe -v error -show_entries stream=width,height,profile,level -of compact $FMT "$OUT"
echo "decoded frames: $(ffmpeg -v error $FMT -i "$OUT" -f rawvideo -pix_fmt nv12 - | wc -c | awk -v s=$((W*CROP_H*3/2)) '{print $1/s}')"
# -r 30 on the raw input: psnr pairs frames by timestamp, and rawvideo
# defaults to 25 fps (f66 graded a correct stream at 25 dB without it).
ffmpeg -v info -hide_banner $FMT -i "$OUT" -f rawvideo -pix_fmt nv12 -s ${W}x$CROP_H -r 30 -i "$IN" \
    -lavfi "[0:v][1:v]psnr" -f null - 2>&1 | grep -E "PSNR|frame=" | tail -2
