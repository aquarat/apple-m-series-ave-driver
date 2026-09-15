#!/bin/bash
# One E3 step with crash-surviving capture (docs/48).
#   tools/e3-run.sh NAME insmod-params...
# Refuses if an IRQ was disabled or apple_ave is already loaded.
# HOLD=seconds keeps the driver loaded after probe with a 5 s heartbeat.
set -u
cd "$(dirname "$0")/.."
NAME=$1; shift
if sudo dmesg | grep -q "Disabling IRQ #"; then echo "REFUSING: IRQ disabled; reboot" >&2; exit 1; fi
if lsmod | grep -q '^apple_ave'; then echo "REFUSING: apple_ave loaded" >&2; exit 1; fi
LOG=results/$NAME-$(date +%s).kmsg
{ echo "=== $NAME $(date -Is) commit $(git rev-parse --short HEAD): insmod driver/apple-ave.ko $* ==="; } > "$LOG"; sync
sudo python3 tools/kmsg_capture.py "$LOG" &
CAP=$!
sleep 3	# let the header reach the disk before insmod (F6: lost otherwise)
sudo insmod driver/apple-ave.ko "$@"
RC=$?
echo "=== insmod returned rc=$RC ===" >> "$LOG"; sync
# HOLD=N: keep the driver loaded N seconds after probe, with a userspace
# heartbeat in the kernel log every 5 s, so an asynchronous freeze is timed.
HOLD=${HOLD:-3}
for ((t = 0; t < HOLD; t += 5)); do
    echo "e3-run heartbeat $NAME t=${t}s" | sudo tee /dev/kmsg >/dev/null
    sleep 5
done
echo "=== insmod rc=$RC ===" >> "$LOG"; sync
sudo kill $CAP 2>/dev/null
echo "$LOG"
