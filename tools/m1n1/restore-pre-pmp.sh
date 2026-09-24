#!/bin/sh
# Undo the PMP experiment (docs/78): put back the stage 2 that was live
# before it, the DAPF-patched m1n1 with the stock device trees.
#
# Copies boot.bin.pre-pmp over boot.bin, keeping the current boot.bin as
# boot.bin.failed-pmp. Lives in the same directory as boot.bin. Run it from
# Linux (sudo sh /boot/efi/m1n1/restore-pre-pmp.sh) or, if Linux no longer
# boots, from macOS / recoveryOS exactly like restore-m1n1.sh
# (RESTORE-README.txt), with this script's name in place of that one.

set -e

GOOD_SHA=73577b9903d8b2735bb439e42787341821ae83a5e2c5fd016778a4dbfdd049fd

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

say "restore-pre-pmp: working in $DIR"

if [ ! -f boot.bin.pre-pmp ]; then
    say "ERROR: boot.bin.pre-pmp not found in $DIR."
    say "Fallback: sh restore-m1n1.sh (stock m1n1, no AVE DAPF patch)."
    exit 1
fi

sum=$(sha256_of boot.bin.pre-pmp)
if [ "$sum" = unknown ]; then
    say "WARNING: no sha256 tool available; cannot verify boot.bin.pre-pmp."
    say "It should be 5010668 bytes:"
    ls -l boot.bin.pre-pmp
elif [ "$sum" != "$GOOD_SHA" ]; then
    say "ERROR: boot.bin.pre-pmp has sha256 $sum"
    say "       expected                    $GOOD_SHA"
    say "Not touching boot.bin. Fallback: sh restore-m1n1.sh"
    exit 1
else
    say "boot.bin.pre-pmp verified ($GOOD_SHA)"
fi

if [ -f boot.bin ]; then
    say "keeping the current boot.bin as boot.bin.failed-pmp"
    cp -f boot.bin boot.bin.failed-pmp
fi

cp -f boot.bin.pre-pmp boot.bin.new
sync
mv -f boot.bin.new boot.bin
sync

sum=$(sha256_of boot.bin)
if [ "$sum" != unknown ] && [ "$sum" != "$GOOD_SHA" ]; then
    say "ERROR: boot.bin now has sha256 $sum - copy did not complete, run again."
    exit 1
fi

say "DONE: boot.bin restored to the pre-PMP stage 2."
