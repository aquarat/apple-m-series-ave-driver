#!/bin/bash
# One staged load of the AVE driver on the M1 (t8103). One step per boot.
#
#   sudo t8103/ave-run.sh C1   overlay + power domains on, NO encoder register access
#   sudo t8103/ave-run.sh C2   + Apple's first write, one ASC read, read-only DAPF dump
#   sudo t8103/ave-run.sh C3   + IPC buffers, firmware placement checks, boot arguments; core NOT started
#   sudo t8103/ave-run.sh C4   + start the encoder core and wait for its first message
#   sudo t8103/ave-run.sh C5   + IPC handshake and the Config command; no V4L2 node, no encode
#   sudo t8103/ave-run.sh C6   + Open and Start_AVC (a 1280x720 H.264 session); no frame encoded
#   sudo t8103/ave-run.sh C7   + encode ONE 1280x720 I-frame; the bitstream lands in results/
#   sudo t8103/ave-run.sh C8   normal load (V4L2 node), then 30 frames 1280x720 through v4l2-ctl, fixed QP (I + P frames)
#   sudo t8103/ave-run.sh C9   normal load, then 60 frames through ffmpeg h264_v4l2m2m with rate control (8 Mbit/s)
#
# Wraps the project's tools/e3-run.sh, which captures the kernel log to
# results/ as it goes. The modules are not unloaded; reboot afterwards.
# C8 is the step for normal use after a boot: it leaves /dev/videoN ready.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
OWNER=${SUDO_USER:-$(stat -c %U "$REPO")}
CACHE=$(getent passwd "$OWNER" | cut -d: -f6)/.cache/ave-m1
die() { echo "REFUSING: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run with sudo"
step=${1:-}
case "$step" in
    C1) params="stop_after=6 step_ms=100" ;;
    C2) params="stop_after=8 dapf_dump=1 step_ms=100" ;;
    C3) params="stop_after=12 step_ms=100" ;;
    C4) params="stop_after=14 step_ms=100" ;;
    C5) params="stop_after=16 session_selftest=1 session_config_only=1 step_ms=100" ;;
    C6) params="stop_after=16 session_selftest=1 step_ms=100" ;;
    C7) params="stop_after=16 session_frame=1 step_ms=100" ;;
    C8|C9) params="stop_after=16 step_ms=0" ;;
    *) die "unknown step '$step' (C1 .. C9)" ;;
esac

v=$(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version)
[ "$v" = v1.6.1-avedapf ] || die "stage 2 is $v, not v1.6.1-avedapf (step B2)"
[ "$(uname -r)" = 7.1.13-fairydust-1-ARCH ] || die "kernel $(uname -r) is not the one the modules were built for"
lsmod | grep -qE '^(apple_ave|ave_overlay) ' && die "an AVE module was already loaded in this boot; reboot first"
[ -e /proc/device-tree/soc/video-encoder@267100000 ] && die "the overlay is already applied; reboot first"
[ -f "$REPO/driver/apple-ave.ko" ] && [ -f "$REPO/test/ave-overlay.ko" ] || die "modules not built"
modinfo -F vermagic "$REPO/driver/apple-ave.ko" | grep -q "^$(uname -r) " || die "apple-ave.ko vermagic does not match"

# Stage 10 reads the image as a control copy; the core itself runs iBoot's.
FW=/lib/firmware/apple/ave_h13g.bin
FW_SRC=$REPO/data/blobs/macos-13.5-j313/ave_h13g.bin
FW_SHA=a5cec344d4212aa4990638e9fcedf48691732e22263a5ba9e2d02445d6c6fb6d
case "$step" in C3|C4|C5|C6|C7|C8|C9)
    if [ ! -f "$FW" ]; then
        [ "$(sha256sum "$FW_SRC" | cut -d' ' -f1)" = "$FW_SHA" ] || die "$FW_SRC missing or not the 13.5 H13G image"
        install -Dm644 "$FW_SRC" "$FW"
        echo "installed $FW"
    fi
    [ "$(sha256sum "$FW" | cut -d' ' -f1)" = "$FW_SHA" ] || die "$FW is not the 13.5 H13G image"
    ;;
esac

mkdir -p "$REPO/results"
sync
echo "Running $step: insmod apple-ave.ko $params (overlay variant=8)"
cd "$REPO"
# shellcheck disable=SC2086
HOLD=15
case "$step" in C8|C9) HOLD=3 ;; esac
OVERLAY=8 OVERLAY_WAIT=0 HOLD=$HOLD tools/e3-run.sh "t8103-$step" $params
sync

# The V4L2 runs: the project's test encodes testsrc2 and grades it (PSNR
# against the input). Its files go to ~/.cache/ave-m1/ave-test.
case "$step" in C8|C9)
    echo
    echo "--- V4L2 encode test ---"
    mode=ctl; n=30
    [ "$step" = C9 ] && { mode=ffmpeg; n=60; }
    echo "=== $step v4l2-test $n $mode ===" > /dev/kmsg
    mkdir -p "$CACHE"
    HOME=$CACHE tools/v4l2-test.sh $n $mode 2>&1 | tee "results/t8103-$step-v4l2.txt"
    echo "=== $step v4l2-test done ===" > /dev/kmsg
    dmesg | grep -E 'apple-ave|apple-dart 2670' | tail -40 > "results/t8103-$step-dmesg-tail.txt"
    chown -R "$OWNER": "$CACHE" results 2>/dev/null || true
    sync
    ;;
esac
chown -R "$OWNER": results 2>/dev/null || true
echo
echo "--- power domains ---"
grep -E 'venc|domain  ' /sys/kernel/debug/pm_genpd/pm_genpd_summary | grep -vE '^\s+/' || true
echo "Done. Leave the modules loaded and reboot before the next step."
