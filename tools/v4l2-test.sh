#!/bin/bash
# On the target, with apple-ave loaded v4l2=1: encode N frames of a test
# pattern through the V4L2 node and grade the result against the input.
#   [CODEC=h264|hevc] [W=1920 H=1088] [CROP_H=1080] [FFARGS="-b:v 8M"] \
#       tools/v4l2-test.sh [frames] [ctl|ffmpeg|gst]
# ctl: v4l2-ctl, fixed QP (RC off). ffmpeg: h264_v4l2m2m / hevc_v4l2m2m,
# which always turns RC on, so it gets FFARGS' bitrate (default 8M; ffmpeg's
# own default is 200k). gst: GStreamer v4l2h264enc / v4l2h265enc, its own
# defaults (no crop).
# CODEC=hevc (docs/77 §19) writes a raw .h265 in every mode and decodes it
# with -f hevc; CODEC=h264 (the default) is unchanged.
set -u
N=${1:-60}; MODE=${2:-ctl}
CODEC=${CODEC:-h264}
W=${W:-1280}; H=${H:-720}
# CROP_H=1080 with H=1088: encode a 1080-line picture from a 1088-line buffer
CROP_H=${CROP_H:-$H}
case $CODEC in
h264) PIX=H264; FFENC=h264_v4l2m2m; GSTENC=v4l2h264enc; GSTPARSE=h264parse
      GSTCAPS=video/x-h264; RAW=out.h264; RAWFMT=h264 ;;
hevc) PIX=HEVC; FFENC=hevc_v4l2m2m; GSTENC=v4l2h265enc; GSTPARSE=h265parse
      GSTCAPS=video/x-h265; RAW=out.h265; RAWFMT=hevc ;;
*) echo "CODEC must be h264 or hevc"; exit 1 ;;
esac
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
echo "node $DEV codec $CODEC"
v4l2-ctl -d "$DEV" --info | head -8
v4l2-ctl -d "$DEV" --list-formats-out --list-formats | grep -E "\[|NV12|H264|HEVC"
rm -f out.h264 out.h265 out.mp4
case $MODE in
ctl)
    timeout 60 v4l2-ctl -d "$DEV" \
        --set-fmt-video-out=width=$W,height=$H,pixelformat=NV12 \
        --set-fmt-video=pixelformat=$PIX \
        --set-selection-output=target=crop,width=$W,height=$CROP_H \
        --stream-mmap --stream-out-mmap --stream-from="$IN" \
        --stream-to=$RAW --stream-count="$N" 2>&1 | tail -3
    OUT=$RAW; FMT="-f $RAWFMT -framerate 30" ;;
ffmpeg)
    if [ "$CODEC" = h264 ]; then
        OUT=out.mp4; FMT=
    else
        # raw Annex-B: no muxer between the encoder and the grade
        OUT=$RAW; FMT="-f $RAWFMT -framerate 30"
    fi
    timeout 60 ffmpeg -v warning -y -f rawvideo -pix_fmt nv12 -s ${W}x$H -r 30 \
        -i "$IN" -c:v $FFENC ${FFARGS:--b:v 8M} \
        $([ "$OUT" = "$RAW" ] && echo "-f $RAWFMT") $OUT 2>&1 | tail -5 ;;
gst)
    # the element name assumes this is the only V4L2 encoder of the codec
    timeout 60 gst-launch-1.0 -q filesrc location="$IN" ! \
        rawvideoparse width=$W height=$CROP_H format=nv12 framerate=30/1 ! \
        $GSTENC ! $GSTPARSE ! $GSTCAPS,stream-format=byte-stream ! \
        filesink location=$RAW 2>&1 | tail -5
    OUT=$RAW; FMT="-f $RAWFMT -framerate 30" ;;
*) echo "mode must be ctl, ffmpeg or gst"; exit 1 ;;
esac
ls -l "$OUT"
ffprobe -v error -show_entries stream=codec_name,width,height,profile,level -of compact $FMT "$OUT"
echo "decoded frames: $(ffmpeg -v error $FMT -i "$OUT" -f rawvideo -pix_fmt nv12 - | wc -c | awk -v s=$((W*CROP_H*3/2)) '{print $1/s}')"
# -r 30 on the raw input: psnr pairs frames by timestamp, and rawvideo
# defaults to 25 fps (f66 graded a correct stream at 25 dB without it).
ffmpeg -v info -hide_banner $FMT -i "$OUT" -f rawvideo -pix_fmt nv12 -s ${W}x$CROP_H -r 30 -i "$IN" \
    -lavfi "[0:v][1:v]psnr" -f null - 2>&1 | grep -E "PSNR|frame=" | tail -2
