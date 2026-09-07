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

# ---------------------------------------------------------------------------
# SAFETY INTERLOCK - DO NOT REMOVE
#
# This script loads kernel modules that have hard-hung this machine six times
# (docs/25-bringup-results.md). It must never run by accident, and in
# particular must never be run by an automated agent exploring the repository.
#
# On 2026-09-07 a subagent doing static analysis ran this script and reset the
# machine. The agent had been told not to run git; it had not been told not to
# touch hardware. The fix is here rather than in the prompt, because a
# safeguard that depends on everyone remembering an instruction is not one.
#
# To run deliberately:   AVE_I_MEAN_IT=1 ./tools/bringup.sh <stage>
# ---------------------------------------------------------------------------
if [ "${AVE_I_MEAN_IT:-}" != "1" ] && [ "${1:-}" != "--status" ]; then
    cat >&2 <<'WARN'
REFUSING TO RUN.

tools/bringup.sh loads kernel modules that hang this machine. It is not safe
to run casually, and automated agents must not run it at all.

Read docs/25-bringup-results.md first. If you really intend to attempt a
bring-up stage on hardware:

    AVE_I_MEAN_IT=1 ./tools/bringup.sh <first-stage> [last-stage]

"./tools/bringup.sh --status" is always allowed and touches nothing.
WARN
    exit 2
fi
REPO="$(cd "$(dirname "$0")/.." && pwd)"
MARK="$REPO/data/bringup-marker"
LOGDIR="$REPO/data/bringup-logs"
mkdir -p "$LOGDIR"

names=(none map-banks dma-mask get-irq request-irq power-attach \
       power-on write-sve-idle read-asc-status asc-start read-sve-status fw-adopt \
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
