#!/bin/sh
# Install one m1n1 stage 2 candidate for the AVE encoder, or put the stock
# one back. One step per boot. Candidates come from build-m1n1.sh.
#
#   sudo t8103/ave-m1n1-step.sh A        self-built, unpatched v1.6.1
#   sudo t8103/ave-m1n1-step.sh B1       + read-only probe (exports the firmware placement)
#   sudo t8103/ave-m1n1-step.sh B2       + AVE DAPF programming (what the driver needs)
#   sudo t8103/ave-m1n1-step.sh restore  back to the stock boot.bin, update-m1n1 active again
#   t8103/ave-m1n1-step.sh status        what is installed, no changes
#
# After a kernel, m1n1 or uboot-asahi update the candidates are stale (old
# device trees): restore, sudo update-m1n1, build-m1n1.sh, then B2 again.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BOOT=$HERE/boot
ESP=/boot/efi/m1n1
DEFAULTS=/etc/default/update-m1n1

sha() { sha256sum "$1" | cut -d' ' -f1; }
die() { echo "ERROR: $*" >&2; exit 1; }
want() {	# sha256 of a built candidate, from SHA256SUMS
    awk -v f="$1" '$2 == f { print $1 }' "$BOOT/SHA256SUMS"
}
[ -f "$BOOT/SHA256SUMS" ] || die "no candidates in $BOOT; run t8103/build-m1n1.sh"
STOCK_SHA=$(want boot.bin.stock)
[ -n "$STOCK_SHA" ] || die "SHA256SUMS has no boot.bin.stock"

name_of() {
    n=$(awk -v s="$1" '$1 == s { print $2 }' "$BOOT/SHA256SUMS")
    case "$n" in
        boot.bin.stock) echo "stock (packaged m1n1)" ;;
        boot.bin.A-selfbuilt) echo "A: self-built, unpatched" ;;
        boot.bin.B1-aveprobe) echo "B1: read-only AVE probe" ;;
        boot.bin.B2-avedapf) echo "B2: AVE DAPF" ;;
        *) echo "unknown to the current candidates (${1%"${1#????????}"}...)" ;;
    esac
}

status() {
    echo "boot.bin on the ESP:   $(name_of "$(sha "$ESP/boot.bin")")"
    if [ -f "$ESP/boot.bin.pre-ave" ]; then
        echo "boot.bin.pre-ave:      $(name_of "$(sha "$ESP/boot.bin.pre-ave")")"
    else
        echo "boot.bin.pre-ave:      missing"
    fi
    printf 'running stage 2:       '
    tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version; echo
    if grep -q '^M1N1_UPDATE_DISABLED=1' "$DEFAULTS" 2>/dev/null; then
        echo "update-m1n1:           disabled (pacman will not replace boot.bin)"
    else
        echo "update-m1n1:           active"
    fi
}

install_file() {	# $1 source, $2 expected sha
    [ "$(sha "$1")" = "$2" ] || die "$1 does not have the expected sha256"
    cp "$1" "$ESP/boot.bin.new"
    sync
    [ "$(sha "$ESP/boot.bin.new")" = "$2" ] || { rm -f "$ESP/boot.bin.new"; die "copy to the ESP is corrupt; boot.bin untouched"; }
    mv -f "$ESP/boot.bin.new" "$ESP/boot.bin"
    sync
    [ "$(sha "$ESP/boot.bin")" = "$2" ] || die "boot.bin does not verify after the rename; run '$0 restore' NOW"
}

step=${1:-status}
[ "$step" = status ] && { status; exit 0; }
[ "$(id -u)" = 0 ] || die "run with sudo"
mountpoint -q /boot/efi || die "/boot/efi is not mounted"

case "$step" in
    A|B1|B2)
        case "$step" in
            A) f=boot.bin.A-selfbuilt ;;
            B1) f=boot.bin.B1-aveprobe ;;
            B2) f=boot.bin.B2-avedapf ;;
        esac
        # The known-good backup is this build's stock image, and nothing else:
        # the candidates are that image with another m1n1 in front.
        if [ "$(sha "$ESP/boot.bin")" = "$STOCK_SHA" ]; then
            cp -p "$ESP/boot.bin" "$ESP/boot.bin.pre-ave"
            sync
        fi
        [ -f "$ESP/boot.bin.pre-ave" ] && [ "$(sha "$ESP/boot.bin.pre-ave")" = "$STOCK_SHA" ] ||
            die "neither boot.bin nor boot.bin.pre-ave on the ESP is the stock image the candidates were built from. Run '$0 restore', 'sudo update-m1n1', then t8103/build-m1n1.sh."
        size=$(stat -c %s "$ESP/boot.bin.pre-ave")
        sed -e "s/@GOOD_SHA@/$STOCK_SHA/" -e "s/@GOOD_SIZE@/$size/" "$HERE/restore-m1n1.sh" > "$ESP/restore-m1n1.sh"
        # Keep the pacman hook from replacing boot.bin (and rotating the
        # custom file into boot.bin.old) while a candidate is installed.
        grep -q '^M1N1_UPDATE_DISABLED=1' "$DEFAULTS" 2>/dev/null ||
            echo 'M1N1_UPDATE_DISABLED=1' >> "$DEFAULTS"
        install_file "$BOOT/$f" "$(want "$f")"
        echo "Installed step $step. Reboot, then run: $HERE/ave-check.sh"
        ;;
    restore)
        [ -f "$ESP/boot.bin.pre-ave" ] || die "no boot.bin.pre-ave on the ESP"
        # Verified against the sha written into the ESP's restore script when
        # the backup was made, so this works with stale candidates too.
        good=$(sed -n 's/^GOOD_SHA=//p' "$ESP/restore-m1n1.sh" 2>/dev/null)
        [ -n "$good" ] || die "the ESP's restore-m1n1.sh has no GOOD_SHA"
        install_file "$ESP/boot.bin.pre-ave" "$good"
        sed -i '/^M1N1_UPDATE_DISABLED=1$/d' "$DEFAULTS"
        echo "Stock boot.bin restored, update-m1n1 active again."
        ;;
    *) die "unknown step '$step' (A, B1, B2, restore, status)" ;;
esac
status
