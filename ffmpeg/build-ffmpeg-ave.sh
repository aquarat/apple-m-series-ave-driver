#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# build-ffmpeg-ave.sh [PREFIX]: an ffmpeg for archival transcoding with the AVE
# encoder (hevc_v4l2m2m / h264_v4l2m2m with B frames, constant QP, Matroska),
# built natively on Fedora Asahi Remix (aarch64). Default PREFIX: $HOME/ffmpeg-ave.
#
# What it needs (Fedora 44):
#   sudo dnf install gcc make git pkgconf-pkg-config diffutils kernel-headers zlib-ng-compat-devel
# (nasm is not needed on arm64.) The result is static and self-contained:
# PREFIX/bin/ffmpeg and PREFIX/bin/ffprobe; the system ffmpeg is untouched.
#
# It clones FFmpeg at FFMPEG_REF (default n8.1) from FFMPEG_REPO into
# PREFIX/src/ffmpeg, applies ffmpeg/patches/*.patch, configures a lean build
# - every decoder, demuxer and parser (any legacy source: MPEG-1/2, MPEG-4, DV,
# MJPEG, H.264, HEVC, VC-1/WMV, ProRes, ...), the V4L2 encoders, FLAC/PCM/AAC
# for audio, Matroska/MP4/MPEG-TS/NUT muxers, the filters archival work uses
# (bwdif, yadif, idet, fieldmatch, decimate, scale, pad, crop, format, ...) -
# builds it at low priority, runs the encoder helpers' unit test and installs.
#
# Environment:
#   FFMPEG_REPO  default https://git.ffmpeg.org/ffmpeg.git
#   FFMPEG_REF   default n8.1 (a tag or branch; the series is made on FFmpeg 8.1
#                plus the v4l2-request series, and applies to plain n8.1 as is)
#   JOBS         make -j (default: the number of CPUs)
#   NICE         nice level for the build (default 10)
#   EXTRA_CONFIGURE  more ./configure options, e.g. "--enable-gpl --enable-filter=pullup"
# Re-running starts again from a clean checkout of FFMPEG_REF.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${1:-${PREFIX:-$HOME/ffmpeg-ave}}
FFMPEG_REPO=${FFMPEG_REPO:-https://git.ffmpeg.org/ffmpeg.git}
FFMPEG_REF=${FFMPEG_REF:-n8.1}
JOBS=${JOBS:-$(nproc)}
NICE=${NICE:-10}
SRC=$PREFIX/src/ffmpeg
BUILD=$PREFIX/src/build
PATCHES=("$HERE"/patches/*.patch)

[ -e "${PATCHES[0]}" ] || { echo "no patches in $HERE/patches" >&2; exit 1; }
for tool in gcc make git pkg-config; do
    command -v "$tool" >/dev/null || { echo "missing $tool: see the dnf line at the top" >&2; exit 1; }
done

echo "== FFmpeg $FFMPEG_REF from $FFMPEG_REPO into $SRC"
mkdir -p "$PREFIX/src"
if [ ! -d "$SRC/.git" ]; then
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$FFMPEG_REF" "$FFMPEG_REPO" "$SRC"
else
    git -C "$SRC" fetch --quiet --depth 1 origin "$FFMPEG_REF"
    git -C "$SRC" am --abort >/dev/null 2>&1 || true
    git -C "$SRC" reset --quiet --hard FETCH_HEAD
    git -C "$SRC" clean --quiet -fdx
fi

echo "== applying ${#PATCHES[@]} patches"
git -C "$SRC" -c user.name=ave-build -c user.email=ave-build@invalid \
    am --quiet --whitespace=nowarn "${PATCHES[@]}"
git -C "$SRC" log --oneline -n "${#PATCHES[@]}"

# Programs, decoders, demuxers, parsers, bitstream filters and the file
# protocols stay as FFmpeg enables them; the rest is chosen.
ENCODERS=(
    h264_v4l2m2m hevc_v4l2m2m          # the AVE
    flac alac aac ac3 eac3             # audio
    pcm_s16le pcm_s16be pcm_s24le pcm_s24be pcm_s32le pcm_f32le pcm_u8
    ffv1 rawvideo wrapped_avframe png mjpeg   # lossless intermediates, cover art, tests
    srt subrip ass ssa webvtt dvdsub dvbsub mov_text   # subtitles
)
MUXERS=(
    matroska webm mp4 mov ipod mpegts nut null
    h264 hevc rawvideo framecrc framemd5 md5 crc
    wav w64 flac ac3 eac3 adts
    srt ass webvtt image2
)
FILTERS=(
    bwdif yadif idet fieldmatch decimate separatefields weave setfield fieldorder
    scale pad crop format setsar setdar setparams fps framerate
    null copy split trim setpts settb select showinfo
    hflip vflip transpose rotate
    aformat aresample anull atrim asetpts pan channelmap volume amerge
    testsrc2 color sine anullsrc
)
join() { local IFS=,; echo "$*"; }

echo "== configure (prefix $PREFIX)"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
# shellcheck disable=SC2086
nice -n "$NICE" "$SRC/configure" \
    --prefix="$PREFIX" \
    --disable-doc --disable-network --disable-autodetect \
    --enable-v4l2-m2m --enable-zlib --enable-iconv \
    --disable-hwaccels --disable-indevs --enable-indev=lavfi --disable-outdevs \
    --disable-encoders --enable-encoder="$(join "${ENCODERS[@]}")" \
    --disable-muxers --enable-muxer="$(join "${MUXERS[@]}")" \
    --disable-filters --enable-filter="$(join "${FILTERS[@]}")" \
    ${EXTRA_CONFIGURE:-} \
    > configure.log || { tail -20 configure.log; exit 1; }
grep -q '^#define CONFIG_HEVC_V4L2M2M_ENCODER 1' config_components.h ||
    { echo "hevc_v4l2m2m not enabled: is linux/videodev2.h there (kernel-headers)?" >&2; exit 1; }

echo "== make -j$JOBS (nice $NICE)"
nice -n "$NICE" make -j"$JOBS" >make.log 2>&1 || { tail -40 make.log; exit 1; }

echo "== unit test: v4l2_enc_utils"
nice -n "$NICE" make -j"$JOBS" libavcodec/tests/v4l2_enc_utils >>make.log 2>&1
./libavcodec/tests/v4l2_enc_utils

echo "== install"
make install >>make.log 2>&1
"$PREFIX/bin/ffmpeg" -hide_banner -h encoder=hevc_v4l2m2m | grep -E '^ +-(qp|device|bf)?' || true
echo
echo "Done: $PREFIX/bin/ffmpeg"
echo "  e.g. $PREFIX/bin/ffmpeg -i in.mkv -c:v hevc_v4l2m2m -qp 24 -bf 1 -g 250 -c:a copy out.mkv"
