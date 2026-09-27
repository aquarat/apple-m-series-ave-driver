#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Load the AVE H.264 encoder on the target, outside the lab harness:
# the V4L2 core modules, the device-tree overlay (the Asahi DT has no AVE
# node yet), then apple-ave with its defaults, which register /dev/videoN.
#
#   sudo tools/ave-load.sh [params] # load, optionally with module parameters
#   sudo tools/ave-load.sh unload   # rmmod apple-ave (Stop + Close any stream)
#   sudo OVERLAY_ARGS=pmp_venc=1 tools/ave-load.sh pmp_report=1   # docs/78 R3
#   sudo VARIANT=4 tools/ave-load.sh   # ave0 only; the default 7 loads both
#   encoders, /dev nodes apple-ave-enc and apple-ave1-enc (docs/82)
#
# Reloading in the same boot works: a load after an unload resets the core
# and restores its DATA by itself (driver parameter reload, default on;
# docs/84 §4).
set -eu
cd "$(dirname "$0")/.."

if [ "${1:-}" = unload ]; then
    rmmod apple_ave
    echo "apple-ave unloaded"
    exit 0
fi
lsmod | grep -q '^apple_ave' && { echo "apple-ave is already loaded"; exit 0; }
[ -f driver/apple-ave.ko ] || { echo "build it first: make -C driver" >&2; exit 1; }
[ -f test/ave-overlay.ko ] || { echo "build the overlay first: make -C test" >&2; exit 1; }

modprobe -a videodev v4l2-mem2mem videobuf2-common videobuf2-v4l2 videobuf2-dma-contig
lsmod | grep -q '^ave_overlay' || insmod test/ave-overlay.ko variant=${VARIANT:-7} ${OVERLAY_ARGS:-}	# 7: both encoders (docs/82)
insmod driver/apple-ave.ko "$@"	# module parameters, e.g. rc_nondrop=1

WANT=1; [ "${VARIANT:-7}" = 7 ] && WANT=2
for i in $(seq 1 100); do
    found=$(grep -lE '^apple-ave1?-enc$' /sys/class/video4linux/video*/name 2>/dev/null)
    if [ "$(echo "$found" | grep -c .)" -ge $WANT ]; then
        for n in $found; do
            echo "encoder $(cat "$n"): /dev/$(basename "$(dirname "$n")")"
        done
        exit 0
    fi
    sleep 0.1
done
echo "apple-ave loaded but no encoder node appeared; see dmesg" >&2
exit 1
