#!/bin/bash
# Load the AVE H.264 encoder on the target, outside the lab harness:
# the V4L2 core modules, the device-tree overlay (the Asahi DT has no AVE
# node yet), then apple-ave with its defaults, which register /dev/videoN.
#
#   sudo tools/ave-load.sh [params] # load, optionally with module parameters
#   sudo tools/ave-load.sh unload   # rmmod apple-ave (Stop + Close any stream)
#   sudo OVERLAY_ARGS=pmp_venc=1 tools/ave-load.sh pmp_report=1   # docs/78 R3
#   sudo VARIANT=8 tools/ave-load.sh   # t6000 (M1 Pro), its one encoder (docs/87)
#   sudo VARIANT=9 tools/ave-load.sh   # t8103 (M1), its one encoder (docs/89)
#   sudo VARIANT=4 tools/ave-load.sh   # ave0 only; the default 7 loads both
#   encoders, /dev nodes apple-ave-enc and apple-ave1-enc (docs/82)
#   sudo VARIANT=11 tools/ave-load.sh  # t6002 (M1 Ultra) ave0; 12 die 0's two,
#   13 ave0 + ave2 (die 1), 14 all four: apple-ave-enc, apple-ave1..3-enc (docs/98)
#
# Without VARIANT the SoC picks it: t6000 8, t6001 7, t8103 9, t8112 10,
# t6002 11 (ave0 only until the docs/98 plan has validated the others).
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

if [ -z "${VARIANT:-}" ]; then
    soc=$(tr '\0' '\n' < /proc/device-tree/compatible | grep -m1 -E '^apple,t[0-9]+$' | cut -d, -f2)
    case "$soc" in
    t6000) VARIANT=8 ;; t6001) VARIANT=7 ;; t8103) VARIANT=9 ;; t8112) VARIANT=10 ;;
    t6002) VARIANT=11 ;;
    *) echo "no AVE overlay for SoC '$soc'; set VARIANT" >&2; exit 1 ;;
    esac
fi
case "$VARIANT" in
7|12|13) WANT=2 ;;
14) WANT=4 ;;
*) WANT=1 ;;
esac

modprobe -a videodev v4l2-mem2mem videobuf2-common videobuf2-v4l2 videobuf2-dma-contig
lsmod | grep -q '^ave_overlay' || insmod test/ave-overlay.ko variant=$VARIANT ${OVERLAY_ARGS:-}
insmod driver/apple-ave.ko "$@"	# module parameters, e.g. rc_nondrop=1

for i in $(seq 1 100); do
    found=$(grep -lE '^apple-ave[0-9]?-enc$' /sys/class/video4linux/video*/name 2>/dev/null)
    if [ "$(echo "$found" | grep -c .)" -ge $WANT ]; then
        for n in $found; do
            echo "encoder $(cat "$n"): /dev/$(basename "$(dirname "$n")")"
        done
        exit 0
    fi
    sleep 0.1
done
echo "apple-ave loaded but fewer than $WANT encoder node(s) appeared; see dmesg" >&2
exit 1
