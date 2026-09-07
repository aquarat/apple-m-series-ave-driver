#!/bin/bash
# Staged AVE bring-up with crash attribution, for a machine with no console.
#
# Before each attempt the stage number is written to a marker file and synced.
# If the machine hangs, that file survives the reboot and names the stage that
# did it. Surviving stages are retested with rmmod/insmod in the same boot, so
# only a stage that actually hangs costs a reboot.
#
#   ./tools/bringup.sh 1        run stages 1..1
#   ./tools/bringup.sh 1 6      walk stages 1..6, stopping at the first failure
#   ./tools/bringup.sh --status what happened last time
#
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
MARK="$REPO/data/bringup-marker"
LOGDIR="$REPO/data/bringup-logs"
mkdir -p "$LOGDIR"

names=(none map-banks dma-mask get-irq request-irq power-attach \
       power-on reset read-asc-status asc-start read-sve-status fw-adopt \
       ipc-alloc start)

# Seconds to wait after syncing the marker before doing anything that can hang.
# The 2026-09-07 attempt lost both the marker update and the session transcript
# because the machine died too soon after the write. Give the filesystem and
# the terminal time to settle.
SETTLE=${SETTLE:-8}

if [ "${1:-}" = "--status" ]; then
    if [ -s "$MARK" ]; then
        s=$(cat "$MARK")
        echo "Last attempt was stage $s (${names[$s]})."
        echo "If the machine rebooted unexpectedly, THAT is the stage that hung."
    else
        echo "No marker: last run completed without hanging."
    fi
    exit 0
fi

first=${1:?usage: bringup.sh <first-stage> [last-stage]}
last=${2:-$first}

cleanup() { sudo rmmod ave_overlay 2>/dev/null; sudo rmmod apple_ave 2>/dev/null; }
trap cleanup EXIT

for n in $(seq "$first" "$last"); do
    echo "=============== stage $n (${names[$n]}) ==============="
    cleanup
    # Record intent BEFORE touching anything, and get it onto disk.
    echo "$n" > "$MARK"
    sync; sleep "$SETTLE"; sync
    sudo dmesg -C

    sudo insmod "$REPO/driver/apple-ave.ko" stop_after="$n" || { echo "insmod driver failed"; exit 1; }
    sudo insmod "$REPO/test/ave-overlay.ko" || { echo "insmod overlay failed"; exit 1; }
    sleep 2

    sudo dmesg | grep -E 'apple-ave|apple_ave|ave-overlay|video-encoder' \
        | tee "$LOGDIR/stage-$n.log"

    if sudo dmesg | grep -q "stage $n (${names[$n]}): OK"; then
        echo "--- stage $n OK ---"
    else
        echo "--- stage $n did NOT report OK; stopping ---"
        exit 1
    fi
done

# Survived everything we were asked to do: clear the marker.
: > "$MARK"; sync
echo "All requested stages completed. Marker cleared."
