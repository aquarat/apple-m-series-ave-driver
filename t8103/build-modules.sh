#!/bin/bash
# Build apple-ave.ko and ave-overlay.ko for the running kernel. Nothing is
# loaded; ave-run.sh does that.
#
#   t8103/build-modules.sh
#
# Uses /lib/modules/<release>/build when the headers package is installed.
# Otherwise it unpacks the headers package built next to the fairydust kernel
# (~/PKGBUILDs/linux-asahi-fairydust) into ~/.cache/ave-m1/kheaders.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
REL=$(uname -r)
KDIR=/lib/modules/$REL/build
if [ ! -d "$KDIR" ]; then
    CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/ave-m1/kheaders
    KDIR=$CACHE/usr/lib/modules/$REL/build
    if [ ! -d "$KDIR" ]; then
        pkg=$(ls "$HOME"/PKGBUILDs/linux-asahi-fairydust/linux-asahi-fairydust-headers-*.pkg.tar.* 2>/dev/null | sort -V | tail -1)
        [ -n "$pkg" ] || { echo "ERROR: no kernel headers for $REL" >&2; exit 1; }
        rm -rf "$CACHE"; mkdir -p "$CACHE"
        bsdtar -xf "$pkg" -C "$CACHE"
        [ -d "$KDIR" ] || { echo "ERROR: $pkg is not for $REL" >&2; exit 1; }
    fi
fi
echo "kernel build tree: $KDIR"
make -C "$REPO/driver" KDIR="$KDIR"
make -C "$REPO/test" KDIR="$KDIR"
modinfo -F vermagic "$REPO/driver/apple-ave.ko"
