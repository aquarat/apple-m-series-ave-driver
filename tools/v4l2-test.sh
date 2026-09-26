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
# SRCFMT=p010 (HEVC only, docs/83): a 10-bit P010 source, which also sets
# the HEVC profile to Main 10. Graded on the host (no HEVC decoder here).
SRCFMT=${SRCFMT:-nv12}
case $SRCFMT in
nv12) VPIX=NV12; FFPIX=nv12; GSTFMT=nv12 ;;
p010) VPIX=P010; FFPIX=p010le; GSTFMT=p010-10le
      [ "$CODEC" = hevc ] || { echo "SRCFMT=p010 needs CODEC=hevc"; exit 1; }
      [ "$MODE" = ffmpeg ] && { echo "ffmpeg's V4L2 encoder cannot send P010 (no v4l2_fmt.c entry)"; exit 1; } ;;
*) echo "SRCFMT must be nv12 or p010"; exit 1 ;;
esac
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
IN=testsrc2-${W}x${CROP_H}-$N.$SRCFMT
[ -s "$IN" ] || ffmpeg -v error -f lavfi -i testsrc2=size=${W}x${CROP_H}:rate=30 \
    -frames:v "$N" -pix_fmt $FFPIX -f rawvideo "$IN"
# v4l2-ctl does not know P010's layout and reads sizeimage bytes a frame:
# luma, chroma, then the chroma rows the driver rounds up to 64 (HEVC). Pad
# each frame to that, or every frame after the first is misaligned (m5).
if [ "$SRCFMT" = p010 ] && [ "$MODE" = ctl ]; then
    PAD=$IN.padded
    [ -s "$PAD" ] || python3 - "$IN" "$PAD" "$W" "$H" "$CROP_H" <<'PY'
import sys
src, dst, w, h, ch = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:])
bpl = ((w + 63) // 64) * 64 * 2
luma, chroma = bpl * h, bpl * (((h + 63) // 64 * 64) // 2)
fl, fc = w * 2 * ch, w * 2 * (ch // 2)
with open(src, 'rb') as f, open(dst, 'wb') as o:
    while True:
        y = f.read(fl)
        if len(y) < fl:
            break
        uv = f.read(fc)
        pad = lambda b, rows, size: b''.join(b[r * w * 2:(r + 1) * w * 2].ljust(bpl, b'\0') for r in range(rows)).ljust(size, b'\0')
        o.write(pad(y, ch, luma) + pad(uv, ch // 2, chroma))
PY
    IN=$PAD
fi
DEV=
for n in /sys/class/video4linux/video*/name; do
    grep -q apple-ave-enc "$n" && DEV=/dev/$(basename "$(dirname "$n")")
done
[ -n "$DEV" ] || { echo "no apple-ave-enc node"; exit 1; }
echo "node $DEV codec $CODEC"
v4l2-ctl -d "$DEV" --info | head -8
v4l2-ctl -d "$DEV" --list-formats-out --list-formats | grep -E "\[|NV12|P010|H264|HEVC"
rm -f out.h264 out.h265 out.mp4
case $MODE in
ctl)
    timeout 60 v4l2-ctl -d "$DEV" \
        --set-fmt-video-out=width=$W,height=$H,pixelformat=$VPIX \
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
        rawvideoparse width=$W height=$CROP_H format=$GSTFMT framerate=30/1 ! \
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
