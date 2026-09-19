#!/bin/bash
# Two firmware starts in one boot, with a Halt in between (docs/55).
#
#   tools/halt-run.sh NAME "load-1 params" "load-2 params"
#
# This is the experiment that decides whether an iteration still costs a
# reboot. It loads the driver, brings the firmware up, unloads it - which is
# where the Halt is sent, so load 1 MUST include fw_halt=1 - and then loads it
# again in the same boot. Load 2 should include fw_restore_data=1: a second
# start over drifted DATA is silent (docs/51), so without the restore a
# failure at load 2 would not tell us whether the Halt worked.
#
# Everything is captured to one log, including the gap between the two loads,
# so a freeze anywhere in the sequence leaves evidence on disk.
#
# Read the log, do not trust the exit status: the interesting lines are
#   halt: scratch 0 = 0x08042006      the firmware reached its wfi
#   halt: CPU_STATUS ... STOPPED      the core is stoppable after all
#   restore: ... read-back verified   the DATA restore actually wrote
#   Apple AVE video encoder ready     (twice) - what this is all for
set -u
cd "$(dirname "$0")/.."

NAME=$1; shift
[ $# -ge 1 ] || { echo "usage: $0 NAME 'load-1 params' ['load-2 params' ...]" >&2; exit 1; }
P1=$1

if sudo dmesg | grep -q "Disabling IRQ #"; then
    echo "REFUSING: an IRQ has been disabled; reboot first" >&2; exit 1
fi
if lsmod | grep -q '^apple_ave'; then
    echo "REFUSING: apple_ave is already loaded" >&2; exit 1
fi
case " $P1 " in
    *" fw_halt=1 "*) ;;
    *) echo "REFUSING: load 1 has no fw_halt=1, so no Halt would be sent" >&2; exit 1 ;;
esac
# Halt calls through the McpuController, which only Config creates (fw 0x10d44,
# 0xe84c); without it the firmware takes a NULL data abort instead of halting
# (h1, 2026-09-14). The driver now refuses to send it, so a load 1 without
# Config would only waste the boot.
case " $P1 " in
    *" session_selftest=1 "*) ;;
    *) echo "REFUSING: load 1 sends no Config (session_selftest=1), so Halt would be refused" >&2; exit 1 ;;
esac
# Halt with a client still open is [U] in docs/55 6. Warn rather than refuse:
# once the halt itself is proven, halting after an encode is what we want.
case " $P1 " in
    *" session_config_only=1 "*) ;;
    *) echo "WARNING: load 1 opens a client; halting with one open is untested" >&2
       echo "         (docs/55 6). Prove the halt with session_config_only=1 first." >&2 ;;
esac

LOG=results/$NAME-$(date +%s).kmsg
{
    echo "=== $NAME $(date -Is) commit $(git rev-parse --short HEAD) ==="
    i=0; for P in "$@"; do i=$((i + 1)); echo "=== load $i: $P"; done
} > "$LOG"; sync

sudo python3 tools/kmsg_capture.py "$LOG" &
CAP=$!
trap 'sudo kill $CAP 2>/dev/null' EXIT
sleep 1

step() { echo "=== $* ===" | sudo tee -a "$LOG" >/dev/null; sync; }

# The session publishes an encoded frame under debugfs, and remove() tears it
# down - so copy whatever is there before each unload, or a successful encode
# would leave nothing behind.
save_debugfs() {
    local d=/sys/kernel/debug/apple_ave out=${LOG%.kmsg}-$1
    if sudo test -d "$d" && [ -n "$(sudo ls -A "$d" 2>/dev/null)" ]; then
        mkdir -p "$out"
        for f in $(sudo ls "$d"); do sudo cat "$d/$f" > "$out/$f"; done
        step "saved debugfs output to $out: $(ls "$out" | tr '\n' ' ')"
    fi
}

# OVERLAY=N applies test/ave-overlay.ko variant=N inside the captured window,
# then waits OVERLAY_WAIT seconds (default 30) with heartbeats before load 1.
# F6 (2026-09-14) reset the machine about 10 s after the overlay was applied by
# hand and before this script's first marker, so there was no evidence which
# of the two did it. With the overlay in here, a death in that phase leaves
# fsync'd markers saying so.
if [ -n "${OVERLAY:-}" ]; then
    if lsmod | grep -q '^ave_overlay'; then
        step "overlay already applied this boot; OVERLAY=$OVERLAY ignored"
    else
        step "applying overlay variant=$OVERLAY"
        sudo insmod test/ave-overlay.ko variant="$OVERLAY"; RCO=$?
        step "overlay insmod returned rc=$RCO"
        [ $RCO -eq 0 ] || { echo "$LOG"; exit 1; }
        W=${OVERLAY_WAIT:-30}
        for ((t = 0; t < W; t += 5)); do
            step "overlay applied, t=${t}s of ${W}s before load 1"
            sleep 5
        done
    fi
fi

# One load per argument. Between loads the firmware is halted (fw_halt=1 in
# ave_remove) and the next load resets the block, restores DATA and starts it
# again - R4 proved two starts in one boot, and F3 showed the Halt works even
# after a pipe hang. That is what makes several hypotheses testable per reboot.
n=0
for P in "$@"; do
    n=$((n + 1))
    step "load $n: insmod $P"
    # Give the marker time to reach the disk: F6, f6b and f6c died within
    # milliseconds of insmod and the synced marker before it was lost each time.
    sleep 3
    # shellcheck disable=SC2086
    sudo insmod driver/apple-ave.ko $P; RC=$?
    step "load $n returned rc=$RC"
    sleep 3

    save_debugfs "load$n"
    step "unload $n (the Halt is sent from ave_remove)"
    sudo rmmod apple_ave; RCU=$?
    step "unload $n returned rc=$RCU"
    sleep 2
    if [ $RCU -ne 0 ]; then
        step "stopping: unload $n failed, the core may still be running"
        echo "$LOG"; exit 1
    fi
    if sudo dmesg | grep -q "Disabling IRQ #"; then
        step "stopping after load $n: an IRQ was disabled, fault reporting is gone"
        echo "$LOG"; exit 1
    fi
done
step "done: $n load(s)"
sleep 1

echo "$LOG"
