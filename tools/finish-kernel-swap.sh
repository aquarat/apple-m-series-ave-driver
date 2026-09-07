#!/bin/bash
# Run this AFTER rebooting into 7.1.6. It removes the 7.0.13 kernel and its
# devel package, then rebuilds the modules against the running kernel.
#
# Held back deliberately: 7.0.13 is the entry GRUB fell back to after the
# bring-up crashes, so it should only be removed once 7.1.6 is confirmed
# healthy. This script refuses to run if you are still on 7.0.13.
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
RUNNING="$(uname -r)"

case "$RUNNING" in
  7.1.6-*) ;;
  *) echo "Running $RUNNING, expected 7.1.6-*. Reboot into 7.1.6 first."; exit 1 ;;
esac

echo "Running kernel: $RUNNING - safe to remove 7.0.13."
sudo dnf remove -y kernel-16k-7.0.13-400.asahi.fc44 \
                   kernel-16k-devel-7.0.13-400.asahi.fc44

echo
echo "Remaining kernels:"; rpm -q kernel-16k kernel-16k-devel | sort
echo
echo "Boot entries:"; sudo grubby --info=ALL 2>/dev/null | grep -E '^(index|kernel)'

echo
echo "Rebuilding modules against $RUNNING ..."
for d in driver test test/psdump; do
    make -C "$REPO/$d" clean >/dev/null 2>&1 || true
    make -C "$REPO/$d" >/dev/null 2>&1 && echo "  $d: OK" || echo "  $d: FAILED"
done
echo
echo "vermagic check:"
modinfo "$REPO/driver/apple-ave.ko" 2>/dev/null | grep vermagic
echo "  running: $RUNNING"
