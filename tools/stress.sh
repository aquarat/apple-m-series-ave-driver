#!/bin/bash
# On the target, with apple-ave loaded (tools/ave-load.sh): the stability
# campaign of docs/84. Everything goes through the V4L2 node with v4l2-ctl.
#
#   tools/stress.sh [SOAK_MIN] [parts]      parts: any of 123456 (default all)
#
#   1 determinism soak: 7 configurations, repeated for SOAK_MIN minutes; every
#     repeat must be byte-identical to that configuration's first output
#   2 long streams: H.264 1080p decoded live here; HEVC 720p to a file for
#     the host to grade (Fedora's ffmpeg has no HEVC decoder)
#   3 open/close cycles: short streams alternating codecs, byte-exact
#   4 random sweep: random size/codec/QP/bitrate; frame count, size, and
#     for H.264 a PSNR floor
#   5 SIGKILL a client mid-stream, then a byte-exact reference stream
#   6 two clients at once: one completes byte-exact, the other gets EBUSY
#
# Results: ~/ave-stress/summary.txt (one line per check, PASS/FAIL) and the
# kernel log lines each part produced (dmesg-N.txt). Exit status 0 = no FAIL.
set -u
SOAK_MIN=${1:-60}; PARTS=${2:-123456}
REPO=$(dirname "$(readlink -f "$0")")/..	# before the cd: $0 may be relative
D=$HOME/ave-stress${STRESS_DIR_SUFFIX:-}; mkdir -p "$D"; cd "$D" || exit 1
SUM=$D/summary.txt; : > "$SUM"
NODE=${NODE:-apple-ave-enc}	# apple-ave1-enc: the second encoder (docs/82)
DEV=
for n in /sys/class/video4linux/video*/name; do
    grep -qx "$NODE" "$n" && DEV=/dev/$(basename "$(dirname "$n")")
done
[ -n "$DEV" ] || { echo "no $NODE node"; exit 1; }
FAILS=0
res() { echo "$(date +%T) $1 $2" | tee -a "$SUM"; [ "$1" = FAIL ] && FAILS=$((FAILS + 1)); true; }
MARK=0
dmesg_since() { sudo dmesg | tail -n +$((MARK + 1)); }
dmesg_mark() { MARK=$(sudo dmesg | wc -l); }
# Kernel lines that mean trouble. EBUSY at start is expected in part 6.
BAD='TIMEOUT|HANG|ASSERT|assert|Oops|BUG|WARNING|SError|fault|failed|MISMATCH|dropped|DROPPED'

# src W H N fmt -> prints the raw file (P010 padded to the buffer layout,
# which v4l2-ctl reads as sizeimage bytes a frame: tools/v4l2-test.sh)
src() {
    local w=$1 h=$2 n=$3 fmt=$4 f
    f=src-${w}x$h-$n.$fmt
    [ -s "$f" ] || ffmpeg -v error -f lavfi -i testsrc2=size=${w}x$h:rate=30 \
        -frames:v "$n" -pix_fmt "$([ "$fmt" = p010 ] && echo p010le || echo nv12)" \
        -f rawvideo "$f"
    if [ "$fmt" = p010 ]; then
        [ -s "$f.padded" ] || python3 - "$f" "$f.padded" "$w" "$h" <<'PY'
import sys
src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
bpl = ((w + 63) // 64) * 64 * 2
luma, chroma = bpl * h, bpl * (((h + 63) // 64 * 64) // 2)
fl, fc = w * 2 * h, w * 2 * (h // 2)
with open(src, 'rb') as f, open(dst, 'wb') as o:
    while True:
        y = f.read(fl)
        if len(y) < fl:
            break
        uv = f.read(fc)
        pad = lambda b, rows, size: b''.join(b[r * w * 2:(r + 1) * w * 2].ljust(bpl, b'\0') for r in range(rows)).ljust(size, b'\0')
        o.write(pad(y, h, luma) + pad(uv, h // 2, chroma))
PY
        f=$f.padded
    fi
    echo "$f"
}

# enc codec fmt W H CROP_H N ctrls out [extra v4l2-ctl args...]
enc() {
    local codec=$1 fmt=$2 w=$3 h=$4 ch=$5 n=$6 ctrls=$7 out=$8; shift 8
    local pix=H264 vpix=NV12 in
    [ "$codec" = hevc ] && pix=HEVC
    [ "$fmt" = p010 ] && vpix=P010
    in=$(src "$w" "$ch" 60 "$fmt")
    timeout 600 v4l2-ctl -d "$DEV" \
        --set-fmt-video-out=width=$w,height=$h,pixelformat=$vpix \
        --set-fmt-video=pixelformat=$pix \
        --set-selection-output=target=crop,width=$w,height=$ch \
        ${ctrls:+-c "$ctrls"} \
        --stream-mmap --stream-out-mmap --stream-from="$in" --stream-loop \
        --stream-to="$out" --stream-count="$n" "$@" >/dev/null 2>&1 || return 1
	# v4l2-ctl can exit 0 after a refused STREAMON: no bytes is a failure
	[ "$out" = /dev/null ] || [ -p "$out" ] || [ -s "$out" ]
}
# md5 of a file, or a marker no real output can match
sum() { [ -s "$1" ] && md5sum < "$1" | cut -c1-32 || echo "empty-$RANDOM"; }

# h264ok file W H N: decodes with no error, N frames of W x H
h264ok() {
    local got
    got=$(ffmpeg -v error -f h264 -i "$1" -f rawvideo -pix_fmt nv12 - 2>/dev/null | wc -c)
    [ "$got" -eq $(( $2 * $3 * 3 / 2 * $4 )) ] && \
        [ -z "$(ffmpeg -v error -f h264 -i "$1" -f null - 2>&1 | head -1)" ]
}
# h264psnr file src W H: Y PSNR against the (first N frames of the) source
h264psnr() {
    ffmpeg -hide_banner -f h264 -i "$1" -f rawvideo -pix_fmt nv12 -s "$3x$4" -i "$2" \
        -lavfi "[0:v][1:v]psnr=shortest=1" -f null - 2>&1 | grep -o "y:[0-9.]*" | head -1 | cut -d: -f2
}
# hevcok file W H N: N slices-of-picture and the size, by our own parser
hevcok() {
    local p
    p=$(python3 "$REPO/tools/hevc_parse.py" "$1" 2>/dev/null)
    [ "$(echo "$p" | grep -cE 'IDR|TRAIL')" -eq "$4" ] && echo "$p" | grep -q "${2}x${3} display"
}

part_dmesg() { # N: collect, and fail on bad lines
    dmesg_since > dmesg-$1.txt
    local n
    n=$(grep -E "$BAD" dmesg-$1.txt | grep -vcE "${2:-^$}")
    [ "$n" -eq 0 ] && res PASS "part $1 kernel log clean" ||
        res FAIL "part $1 kernel log: $n bad line(s), e.g. $(grep -E "$BAD" dmesg-$1.txt | grep -vE "${2:-^$}" | head -1 | cut -c1-160)"
}

# ---------------------------------------------------------------- configs
# name codec fmt W H CROP_H N ctrls
CONFIGS=(
  "h264-1080  h264 nv12 1920 1088 1080 120 -"
  "hevc-1080  hevc nv12 1920 1088 1080 120 -"
  "hevc10-720 hevc p010 1280 720 720 120 hevc_profile=2"
  "h264-4k    h264 nv12 3840 2160 2160 60 -"
  "hevc-4k    hevc nv12 3840 2160 2160 60 -"
  "h264-rc    h264 nv12 1920 1088 1080 120 video_bitrate_mode=0,video_bitrate=8000000,frame_level_rate_control_enable=1"
  "hevc-rc    hevc nv12 1280 720 720 120 video_bitrate_mode=0,video_bitrate=4000000,frame_level_rate_control_enable=1"
)
declare -A REF
runcfg() { # line out -> md5
    local out=$2 ctrls
    set -- $1
    ctrls=$8; [ "$ctrls" = - ] && ctrls=
    enc "$2" "$3" "$4" "$5" "$6" "$7" "$ctrls" "$out" && md5sum < "$out" | cut -c1-32
}

reference() { # establish REF[name] from a first, checked encode
    local line name out m
    for line in "${CONFIGS[@]}"; do
        set -- $line; name=$1
        out=ref-$name.$2
        m=$(runcfg "$line" "$out") || { res FAIL "ref $name: encode failed"; continue; }
        REF[$name]=$m
        if [ "$2" = h264 ]; then
            if h264ok "$out" "$4" "$6" "$7"; then
                res PASS "ref $name: $(stat -c %s "$out") bytes, decodes, $7 frames"
            else
                res FAIL "ref $name: does not decode to $7 frames"
            fi
        elif hevcok "$out" "$4" "$6" "$7"; then
            res PASS "ref $name: $(stat -c %s "$out") bytes, $7 pictures parse"
        else
            res FAIL "ref $name: $7 pictures do not parse"
        fi
    done
}

# ---------------------------------------------------------------- parts
p1() {
    local end=$(( $(date +%s) + SOAK_MIN * 60 )) it=0 bad=0 line name m
    while [ "$(date +%s)" -lt "$end" ]; do
        it=$((it + 1))
        for line in "${CONFIGS[@]}"; do
            set -- $line; name=$1
            m=$(runcfg "$line" soak.$2) || m=encode-failed
            if [ "$m" != "${REF[$name]}" ]; then
                bad=$((bad + 1))
                res FAIL "soak it $it $name: $m != ${REF[$name]}"
                cp soak.$2 "soak-bad-$it-$name.$2" 2>/dev/null
            fi
        done
    done
    [ $bad -eq 0 ] && res PASS "soak: $it iterations x ${#CONFIGS[@]} configs, all byte-identical"
}

p2() {
    local n=60000 got
    # H.264 1080p, decoded as it is produced
    rm -f long.fifo; mkfifo long.fifo
    ffmpeg -v error -f h264 -i long.fifo -f null - > long-h264.err 2>&1 &
    local dec=$!
    enc h264 nv12 1920 1088 1080 $n "" long.fifo; local rc=$?
    wait $dec
    got=$(grep -c . long-h264.err)
    [ $rc -eq 0 ] && [ "$got" -eq 0 ] && res PASS "long h264 1080p: $n frames, decoder silent" ||
        res FAIL "long h264 1080p: rc $rc, $got decoder error line(s)"
    rm -f long.fifo
    # HEVC 720p to a file; the host decodes it (docs/84)
    enc hevc nv12 1280 720 720 $n "" long.h265 && res PASS "long hevc 720p: $n frames, $(stat -c %s long.h265) bytes (grade on host)" ||
        res FAIL "long hevc 720p: rc $?"
}

p3() {
    local i bad=0 m ref1 ref2
    enc h264 nv12 640 480 480 10 "" c.h264; ref1=$(sum c.h264)
    enc hevc nv12 640 480 480 10 "" c.h265; ref2=$(sum c.h265)
    h264ok c.h264 640 480 10 || res FAIL "cycles: h264 reference does not decode"
    for i in $(seq 1 300); do
        if [ $((i % 2)) -eq 0 ]; then
            enc h264 nv12 640 480 480 10 "" c.h264; m=$(sum c.h264); [ "$m" = "$ref1" ] || bad=$((bad + 1))
        else
            enc hevc nv12 640 480 480 10 "" c.h265; m=$(sum c.h265); [ "$m" = "$ref2" ] || bad=$((bad + 1))
        fi
    done
    [ $bad -eq 0 ] && res PASS "cycles: 300 open/stream/close, all byte-identical" ||
        res FAIL "cycles: $bad of 300 differ"
}

p4() {
    local i w h codec ctrls n=10 out bad=0 psnr
    RANDOM=84
    for i in $(seq 1 150); do
        w=$(( (RANDOM % 58 + 3) * 64 ))          # 192..3840
        h=$(( (RANDOM % 130 + 6) * 16 ))         # 96..2160
        codec=h264; [ $((RANDOM % 2)) -eq 0 ] && codec=hevc
        if [ $((RANDOM % 3)) -eq 0 ]; then
            ctrls="video_bitrate_mode=0,video_bitrate=$(( (RANDOM % 20 + 1) * 1000000 )),frame_level_rate_control_enable=1"
        else
            ctrls="${codec}_i_frame_qp_value=$((RANDOM % 31 + 15))"
        fi
        out=sweep.$codec
        if ! enc $codec nv12 $w $h $h $n "$ctrls" $out; then
            bad=$((bad + 1)); res FAIL "sweep $i $codec ${w}x$h $ctrls: encode failed"; continue
        fi
        if [ $codec = h264 ]; then
            if ! h264ok $out $w $h $n; then
                bad=$((bad + 1)); res FAIL "sweep $i h264 ${w}x$h $ctrls: bad decode"; continue
            fi
            psnr=$(h264psnr $out "$(src $w $h 60 nv12)" $w $h)
            if [ -z "$psnr" ] || [ "${psnr%.*}" -lt 25 ]; then
                bad=$((bad + 1)); res FAIL "sweep $i h264 ${w}x$h $ctrls: PSNR ${psnr:-none}"
            fi
        elif ! hevcok $out $w $h $n; then
            bad=$((bad + 1)); res FAIL "sweep $i hevc ${w}x$h $ctrls: does not parse as $n pictures"
            cp $out sweep-bad-$i.h265
        else
            cp $out sweep-$i-${w}x$h.h265     # the host grades a sample
        fi
        rm -f src-${w}x$h-60.nv12              # sizes rarely repeat
    done
    [ $bad -eq 0 ] && res PASS "sweep: 150 random configurations"
}

p5() {
    local i bad=0 ref m pid
    enc h264 nv12 640 480 480 10 "" k.h264; ref=$(sum k.h264)
    for i in $(seq 1 40); do
        enc h264 nv12 1920 1088 1080 100000 "" /dev/null &
        pid=$!
        sleep "0.$((RANDOM % 9 + 1))$((RANDOM % 10))"
        [ $((i % 4)) -eq 0 ] && sleep 2
        # the client itself, not the timeout(1) wrapper around it
        pkill -9 -f "^v4l2-ctl -d $DEV "; wait $pid 2>/dev/null
        enc h264 nv12 640 480 480 10 "" k.h264; m=$(sum k.h264)
        [ "$m" = "$ref" ] || { bad=$((bad + 1)); res FAIL "kill $i: next stream differs"; }
    done
    [ $bad -eq 0 ] && res PASS "kill: 40 clients SIGKILLed mid-stream, the next stream byte-identical each time"
}

p6() {
    local i bad=0 ref ra rb ok
    enc hevc nv12 1280 720 720 60 "" x.h265; ref=$(sum x.h265)
    for i in $(seq 1 20); do
        enc hevc nv12 1280 720 720 60 "" xa.h265 & local a=$!
        enc hevc nv12 1280 720 720 60 "" xb.h265 & local b=$!
        wait $a; ra=$?; wait $b; rb=$?
        ok=0
        [ $ra -eq 0 ] && [ "$(sum xa.h265)" = "$ref" ] && ok=$((ok + 1))
        [ $rb -eq 0 ] && [ "$(sum xb.h265)" = "$ref" ] && ok=$((ok + 1))
        # one owner at a time (docs/68 §7): at least one completes exactly
        [ $ok -ge 1 ] || { bad=$((bad + 1)); res FAIL "contend $i: rc $ra/$rb, neither byte-identical"; }
        rm -f xa.h265 xb.h265
        enc hevc nv12 1280 720 720 60 "" x.h265
        [ "$(sum x.h265)" = "$ref" ] || { bad=$((bad + 1)); res FAIL "contend $i: the stream after differs"; }
    done
    [ $bad -eq 0 ] && res PASS "contend: 20 rounds of two clients; one completes exactly, and the device is fine after"
}

echo "=== stress $(date -Is) soak ${SOAK_MIN} min, parts $PARTS, $DEV ===" | tee -a "$SUM"
dmesg_mark
reference; part_dmesg 0
for p in 1 2 3 4 5 6; do
    case $PARTS in *$p*) ;; *) continue ;; esac
    dmesg_mark
    echo "=== part $p $(date +%T) ===" | tee -a "$SUM"
    p$p
    if [ $p = 6 ]; then part_dmesg $p 'start .* failed: -16'; else part_dmesg $p; fi
done
echo "=== done $(date -Is): $FAILS FAIL ===" | tee -a "$SUM"
[ $FAILS -eq 0 ]
