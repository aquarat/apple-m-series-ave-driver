#!/bin/bash
# Copy the encoder firmware as iBoot left it (TEXT and DATA) out of RAM, for
# the pristine DATA blob the driver needs to start a core a second time in one
# boot (docs/51). Read-only: maps two ranges below Linux's memory and copies
# them; no AVE register, no power domain.
#
# Must run in a boot in which the encoder has NOT been started: the first run
# rewrites DATA.
#   sudo t8103/dump-fw.sh
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
REPO=$(cd "$(dirname "$0")/.." && pwd)
U=${SUDO_USER:-$(stat -c %U "$REPO")}
OUT=$REPO/data/blobs/t8103-iboot
if lsmod | grep -qE '^(apple_ave|ave_overlay) ' || dmesg | grep -q 'apple-ave .*stage 13'; then
    echo "REFUSING: the encoder was loaded in this boot; DATA is no longer pristine. Reboot first."
    exit 1
fi
[ "$(tr -d '\0' < /proc/device-tree/compatible | head -c 10)" = "apple,j313" ] || { echo "REFUSING: not a j313"; exit 1; }
mkdir -p "$OUT"
dump() {	# base size file
    insmod "$REPO/test/physdump.ko" base="$1" size="$2"
    cp /sys/kernel/debug/ave_physdump/window.bin "$OUT/$3"
    rmmod physdump
}
dump 0x8009f4000 0xcc000 text.bin
dump 0x8019b0000 0x128000 data.bin
chown -R "$U": "$OUT"
sha256sum "$OUT"/text.bin "$OUT"/data.bin
dmesg | grep physdump | tail -4
