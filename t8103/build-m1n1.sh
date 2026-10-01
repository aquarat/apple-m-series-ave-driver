#!/bin/bash
# Build the m1n1 stage 2 candidates for the AVE bring-up on this machine.
# Nothing is installed; ave-m1n1-step.sh does that, one step per boot.
#
#   t8103/build-m1n1.sh
#
# Output in t8103/boot/ (gitignored):
#   boot.bin.stock         what update-m1n1 would write now (packaged m1n1)
#   boot.bin.A-selfbuilt   m1n1 v1.6.1 built here, unpatched
#   boot.bin.B1-aveprobe   + 0001: export the encoder's segment-ranges to /chosen
#   boot.bin.B2-avedapf    + 0002: program the encoder's DAPF
#   SHA256SUMS             what ave-m1n1-step.sh verifies against
#
# Each candidate is the stock image with only the leading m1n1.bin replaced,
# so device trees and U-Boot are the same bytes as update-m1n1's. Run it again
# after a kernel, m1n1 or uboot-asahi update: the old candidates carry the old
# device trees.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/boot
WORK=${XDG_CACHE_HOME:-$HOME/.cache}/ave-m1/m1n1
VER=1.6.1
ARTWORK=80d14f8b6f485b310e305a84b4b806361518ddd1
# From the m1n1 PKGBUILD (asahi-alarm)
M1N1_SHA=7099fcf6e94b6a6a8e57f8a3d434e7a66dbe476737f0f02f7bb3349586aa3b85
ART_SHA=f2b18744a38d1369a8e9e59acddd12551c5500435fbd9e709fe7a6b212a070ac

die() { echo "ERROR: $*" >&2; exit 1; }
fetch() {	# url file sha256
    [ -f "$2" ] || curl -fsSL -o "$2" "$1"
    [ "$(sha256sum "$2" | cut -d' ' -f1)" = "$3" ] || die "$2 has the wrong sha256"
}

pkg=$(pacman -Q m1n1 | cut -d' ' -f2)
[ "${pkg%%-*}" = "$VER" ] || die "installed m1n1 is $pkg, the patches are against $VER"

mkdir -p "$WORK" "$OUT"
cd "$WORK"
fetch "https://github.com/AsahiLinux/m1n1/archive/v$VER.tar.gz" m1n1.tar.gz $M1N1_SHA
fetch "https://github.com/AsahiLinux/artwork/archive/$ARTWORK.tar.gz" artwork.tar.gz $ART_SHA

build() {	# name tag patches...
    local name=$1 tag=$2 p
    shift 2
    rm -rf "$name"
    mkdir -p "$name/artwork"
    tar -xzf m1n1.tar.gz -C "$name" --strip-components=1
    tar -xzf artwork.tar.gz -C "$name/artwork" --strip-components=1
    cp "$HERE/m1n1/rust-toolchain.toml" "$name/"
    for p in "$@"; do
        patch -s -p1 -d "$name" < "$p"
    done
    echo "building $name ($tag)"
    ( cd "$name" && M1N1_VERSION_TAG=$tag make ARCH= RELEASE=1 BUILDSTD=1 -j"$(nproc)" >build.log 2>&1 ) ||
        die "$name did not build, see $WORK/$name/build.log"
}
build stock "v$VER"
build probe "v$VER-aveprobe" "$HERE/m1n1/0001-probe-export-ave-segment-ranges.patch"
# 0002 is the project's tools/m1n1 patch plus the t8103 node names
build ave "v$VER-avedapf" "$HERE/m1n1/0001-probe-export-ave-segment-ranges.patch" \
    "$HERE/m1n1/0002-dapf-program-dart-ave-t8103.patch"

# The stock image, by update-m1n1's own recipe and settings.
SRC=/usr/lib/asahi-boot
DTBS=
# shellcheck disable=SC1091
[ -e /etc/default/update-m1n1 ] && . /etc/default/update-m1n1
: "${DTBS:=$(/bin/ls -d /lib/modules/*-ARCH | sort -rV | head -1)/dtbs/*.dtb}"
# shellcheck disable=SC2086
{ cat "$SRC/m1n1.bin" $DTBS; gzip -c "$SRC/u-boot-nodtb.bin"; [ -e /etc/m1n1.conf ] && grep -E '^(chosen\..*=|display=|mitigations=)' /etc/m1n1.conf || true; } > "$OUT/boot.bin.stock"

n=$(stat -c %s "$SRC/m1n1.bin")
cmp -s -n "$n" "$OUT/boot.bin.stock" "$SRC/m1n1.bin" || die "stock image does not start with the packaged m1n1.bin"
assemble() {	# tree out
    local bin=$WORK/$1/build/m1n1.bin
    # m1n1 finds its payloads at _payload_start (raw link), which must be the file's end
    [ "$(stat -c %s "$bin")" = "$n" ] || die "$1: m1n1.bin is $(stat -c %s "$bin") bytes, the packaged one $n"
    [ $((16#$(nm "$WORK/$1/build/m1n1-raw.elf" | awk '$3 == "_payload_start" { print $1 }'))) = "$n" ] ||
        die "$1: _payload_start is not at the end of m1n1.bin"
    { cat "$bin"; tail -c +$((n + 1)) "$OUT/boot.bin.stock"; } > "$OUT/$2"
}
assemble stock boot.bin.A-selfbuilt
assemble probe boot.bin.B1-aveprobe
assemble ave boot.bin.B2-avedapf

( cd "$OUT" && sha256sum boot.bin.stock boot.bin.A-selfbuilt boot.bin.B1-aveprobe boot.bin.B2-avedapf > SHA256SUMS && cat SHA256SUMS )
if [ -f /boot/efi/m1n1/boot.bin ]; then
    cur=$(sha256sum /boot/efi/m1n1/boot.bin | cut -d' ' -f1)
    echo "ESP boot.bin is: $(grep "^$cur " "$OUT/SHA256SUMS" | cut -d' ' -f3 || true)"
fi
