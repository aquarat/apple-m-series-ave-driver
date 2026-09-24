#!/bin/bash
# Load the AVE H.264 encoder on the target, outside the lab harness:
# the V4L2 core modules, the device-tree overlay (the Asahi DT has no AVE
# node yet), then apple-ave with its defaults, which register /dev/videoN.
#
#   sudo tools/ave-load.sh          # load
#   sudo tools/ave-load.sh unload   # rmmod apple-ave (Stop + Close any stream)
#
# Once per boot: the driver unloads cleanly, but loading it a second time in
# the same boot does not work yet (docs/53 f56/f58). Reboot to reload.
set -eu
cd "$(dirname "$0")/.."

if [ "${1:-}" = unload ]; then
    rmmod apple_ave
    echo "apple-ave unloaded; reboot before loading it again"
    exit 0
fi
lsmod | grep -q '^apple_ave' && { echo "apple-ave is already loaded"; exit 0; }
if dmesg | grep -q 'apple-ave.*powered off (remove)'; then
    echo "apple-ave was unloaded earlier this boot; reloading needs a reboot" >&2
    exit 1
fi
[ -f driver/apple-ave.ko ] || { echo "build it first: make -C driver" >&2; exit 1; }
[ -f test/ave-overlay.ko ] || { echo "build the overlay first: make -C test" >&2; exit 1; }

modprobe -a videodev v4l2-mem2mem videobuf2-common videobuf2-v4l2 videobuf2-dma-contig
lsmod | grep -q '^ave_overlay' || insmod test/ave-overlay.ko variant=4
insmod driver/apple-ave.ko

for i in $(seq 1 50); do
    for n in /sys/class/video4linux/video*/name; do
        if grep -q apple-ave-enc "$n" 2>/dev/null; then
            echo "H.264 encoder: /dev/$(basename "$(dirname "$n")")"
            exit 0
        fi
    done
    sleep 0.1
done
echo "apple-ave loaded but no encoder node appeared; see dmesg" >&2
exit 1
