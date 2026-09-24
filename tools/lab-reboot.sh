#!/bin/bash
# Reboot the target from the host and wait until it is usable again: its
# netconsole-ave boot marker has reached the receiver, and SSH answers.
#   tools/lab-reboot.sh            reboot (the driver has no .shutdown)
#   tools/lab-reboot.sh --wait [LINE]   only wait (after a crash-reset),
#                                          for a marker after receiver line LINE
set -u
. "$(dirname "$0")/../lab.env" || { echo "lab.env missing (AGENTS.md)" >&2; exit 1; }
RX=$LOGDIR/ave-netconsole.log
MAXWAIT=${MAXWAIT:-300}
start=${2:-$(wc -l < "$RX")}
t0=$(date +%s)
if [ "${1:-}" != --wait ]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$TARGET" 'sudo systemctl reboot' || true
    echo "$(date -Is) reboot issued"
fi
until tail -n +$((start + 1)) "$RX" | grep -q '=== netconsole-ave up'; do
    [ $(( $(date +%s) - t0 )) -gt "$MAXWAIT" ] && { echo "no boot marker after ${MAXWAIT}s" >&2; exit 1; }
    sleep 2
done
until ssh -o BatchMode=yes -o ConnectTimeout=5 "$TARGET" true 2>/dev/null; do
    [ $(( $(date +%s) - t0 )) -gt "$MAXWAIT" ] && { echo "marker seen, no ssh after ${MAXWAIT}s" >&2; exit 1; }
    sleep 2
done
echo "$(date -Is) back after $(( $(date +%s) - t0 ))s:"
tail -n +$((start + 1)) "$RX" | grep '=== netconsole-ave up' | tail -1 | grep -oE 'boot [0-9a-f-]+ [a-z-]*reachable=[a-z]+ after [0-9.]+s'
