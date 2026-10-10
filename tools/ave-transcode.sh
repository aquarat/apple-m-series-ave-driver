#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# ave-transcode.sh IN OUT.mkv [QP] [NODE] [B]: HEVC at fixed QP, with B frames, on one AVE
# encoder; audio and subtitles copied, container metadata (creation_time etc.) copied from IN.
#
#   QP    HEVC I-frame QP (default 24); B frames get QP + 3 (docs/96), P frames the I QP
#   NODE  the V4L2 node's name: apple-ave-enc, apple-ave1-enc, ... (one stream per encoder)
#   B     B frames per P: 0, 1 (default; -8..10 % bitrate, docs/96) or 2
#
# Why not ffmpeg's hevc_v4l2m2m: it cannot set a fixed QP or B frames, picks the encoder node
# itself, and ignores the line stride, so widths that are not a multiple of 64 (720) come out
# wrong. v4l2-ctl takes any width that is a multiple of 16 and crops the rest via the SPS.
# The elementary stream has no timestamps; with B frames only mkvmerge (MKVToolNix) rebuilds
# them, so it goes through mkvmerge before ffmpeg adds the other streams.
#
# Progressive input only: deinterlace an interlaced IN with FF_VF, e.g.
# FF_VF=bwdif=mode=send_frame:parity=auto:deint=all (same frame rate); a rate-doubling filter
# (mode=send_field) also needs FPS= set to the new rate (e.g. FPS=50). Needs root or the video group,
# v4l2-ctl, mkvmerge, ffmpeg/ffprobe (FF=, FP= to pick a build).
set -euo pipefail
IN=$1; OUT=$2; QP=${3:-24}; NODE=${4:-apple-ave-enc}; B=${5:-1}
FF=${FF:-ffmpeg}; FP=${FP:-ffprobe}
DEV=; for n in /sys/class/video4linux/video*; do [ "$(cat $n/name)" = "$NODE" ] && DEV=/dev/${n##*/}; done
[ -n "$DEV" ] || { echo "no V4L2 node named $NODE (driver loaded?)" >&2; exit 1; }
IFS=, read W H R < <($FP -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate -of csv=p=0 "$IN")
R=${FPS:-$R}
H16=$(( (H + 15) / 16 * 16 )); W16=$(( (W + 15) / 16 * 16 ))
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; mkfifo "$T/in.nv12"
# decode -> raw NV12 at the source size (v4l2-ctl fills the 16-aligned buffer from WxH frames via the crop)
$FF -hide_banner -v error -i "$IN" -map 0:v:0 -vf "${FF_VF:+$FF_VF,}scale=${W}:${H},format=nv12" -f rawvideo -y "$T/in.nv12" &
DEC=$!
v4l2-ctl -d "$DEV" --set-fmt-video-out=width=$W16,height=$H16,pixelformat=NV12 --set-fmt-video=pixelformat=HEVC \
    --set-selection-output=target=crop,width=$W,height=$H \
    --set-ctrl=video_gop_size=250,hevc_i_frame_qp_value=$QP,frame_level_rate_control_enable=0,video_b_frames=$B \
    --stream-mmap --stream-out-mmap --stream-from="$T/in.nv12" --stream-to="$T/v.hevc" >/dev/null 2>"$T/v4l2.log"
wait $DEC
# The elementary stream has no timestamps, and with B frames ffmpeg's raw HEVC demuxer cannot
# derive them; mkvmerge can (MKVToolNix). Then ffmpeg adds audio, subtitles and the metadata.
mkvmerge -q -o "$T/v.mkv" --default-duration "0:${R}fps" "$T/v.hevc"
$FF -hide_banner -v error -i "$T/v.mkv" -i "$IN" -map 0:v -map 1:a? -map 1:s? \
    -map_metadata 1 -c copy -y "$OUT"
