#!/bin/sh
# Restore the known-good m1n1 stage 2 on this Mac's Asahi/Fedora ESP.
#
# Copies boot.bin.pre-ave (the stock stage 2 saved before the AVE DAPF
# experiments, docs/50) over boot.bin, keeping the current boot.bin as
# boot.bin.failed. Lives in the same directory as boot.bin.
#
# From macOS Terminal:
#   sudo diskutil mount 89A77CF4-32BA-4A03-8BCA-DB0F62925CA4
#   sudo sh "/Volumes/EFI - FEDRA/m1n1/restore-m1n1.sh"
#
# From recoveryOS Terminal (already root, no sudo):
#   diskutil mount 89A77CF4-32BA-4A03-8BCA-DB0F62925CA4
#   sh "/Volumes/EFI - FEDRA/m1n1/restore-m1n1.sh"
#
# If the UUID form does not mount it, use: diskutil list  (the ~500 MB
# "EFI - FEDRA" partition, normally disk0s4), then diskutil mount disk0s4.
# If the volume mounted under another name, `diskutil info disk0s4 | grep
# "Mount Point"` shows where; run the script from there.

set -e

GOOD_SHA=2227cf97eac6d5f2db08e43a81693ef96aea168532933bfec9602c8c580d94f7

DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"

say() { printf '%s\n' "$*"; }

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

say "restore-m1n1: working in $DIR"

if [ ! -f boot.bin.pre-ave ]; then
    say "ERROR: boot.bin.pre-ave not found in $DIR."
    say "Fallback: boot.bin.old in this directory is an older working stage 2:"
    say "  cp boot.bin.old boot.bin"
    exit 1
fi

sum=$(sha256_of boot.bin.pre-ave)
if [ "$sum" = unknown ]; then
    say "WARNING: no sha256 tool available; cannot verify boot.bin.pre-ave."
    say "It should be 5010668 bytes:"
    ls -l boot.bin.pre-ave
elif [ "$sum" != "$GOOD_SHA" ]; then
    say "ERROR: boot.bin.pre-ave has sha256 $sum"
    say "       expected                    $GOOD_SHA"
    say "Not touching boot.bin. Fallback: cp boot.bin.old boot.bin"
    exit 1
else
    say "boot.bin.pre-ave verified ($GOOD_SHA)"
fi

if [ -f boot.bin ]; then
    say "keeping the current boot.bin as boot.bin.failed"
    cp -f boot.bin boot.bin.failed
fi

cp -f boot.bin.pre-ave boot.bin.new
sync
mv -f boot.bin.new boot.bin
sync

sum=$(sha256_of boot.bin)
if [ "$sum" != unknown ] && [ "$sum" != "$GOOD_SHA" ]; then
    say "ERROR: boot.bin now has sha256 $sum - copy did not complete, run again."
    exit 1
fi

say "DONE: boot.bin restored to the known-good stage 2."
VOL=$(dirname "$DIR")
say "Now: cd /  then  diskutil unmount \"$VOL\"  (or just shut down),"
say "then hold the power button and choose the Fedora/Asahi volume."
