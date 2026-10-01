#!/bin/sh
# Put the known-good m1n1 stage 2 back when Linux no longer boots.
# Template: ave-m1n1-step.sh fills in GOOD_SHA/GOOD_SIZE and copies it to the ESP.
# Lives next to boot.bin on the ESP ("EFI - OMARC", nvme0n1p4 / disk0s4).
#
# macOS Terminal:
#   sudo diskutil mount B212DF28-A010-4BC5-9532-BC39966685A4
#   sudo sh "/Volumes/EFI - OMARC/m1n1/restore-m1n1.sh"
# recoveryOS Terminal (already root):
#   diskutil mount B212DF28-A010-4BC5-9532-BC39966685A4
#   sh "/Volumes/EFI - OMARC/m1n1/restore-m1n1.sh"
# If the UUID does not mount: diskutil list, find "EFI - OMARC" (500 MB),
# then diskutil mount disk0s4.
#
# Manual equivalent:  cp boot.bin.pre-ave boot.bin
# Older fallbacks in the same directory: boot.bin.orig, boot.bin.old (stock
# m1n1 with the stock kernel's device trees, from before fairydust).
set -e

GOOD_SHA=@GOOD_SHA@
GOOD_SIZE=@GOOD_SIZE@

DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | sed 's/^.*= *//'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        echo unknown
    fi
}

echo "restore-m1n1: working in $DIR"

if [ ! -f boot.bin.pre-ave ]; then
    echo "ERROR: boot.bin.pre-ave not found. Fallback: cp boot.bin.orig boot.bin"
    exit 1
fi

sum=$(sha256_of boot.bin.pre-ave)
if [ "$sum" = unknown ]; then
    echo "WARNING: no sha256 tool; boot.bin.pre-ave should be $GOOD_SIZE bytes:"
    ls -l boot.bin.pre-ave
elif [ "$sum" != "$GOOD_SHA" ]; then
    echo "ERROR: boot.bin.pre-ave has sha256 $sum, expected $GOOD_SHA"
    echo "Not touching boot.bin. Fallback: cp boot.bin.orig boot.bin"
    exit 1
fi

[ -f boot.bin ] && cp -f boot.bin boot.bin.failed
cp -f boot.bin.pre-ave boot.bin.new
sync
mv -f boot.bin.new boot.bin
sync

sum=$(sha256_of boot.bin)
if [ "$sum" != unknown ] && [ "$sum" != "$GOOD_SHA" ]; then
    echo "ERROR: boot.bin has sha256 $sum after the copy - run this again."
    exit 1
fi

echo "DONE: boot.bin is the known-good stage 2 again."
echo "Shut down, hold the power button, choose the Omarchy volume."
echo "Back in Linux: remove the line M1N1_UPDATE_DISABLED=1 from /etc/default/update-m1n1."
