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
P1=${1:-}; P2=${2:-$P1}

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

LOG=results/$NAME-$(date +%s).kmsg
{
    echo "=== $NAME $(date -Is) commit $(git rev-parse --short HEAD) ==="
    echo "=== load 1: $P1"
    echo "=== load 2: $P2"
} > "$LOG"; sync

sudo python3 tools/kmsg_capture.py "$LOG" &
CAP=$!
trap 'sudo kill $CAP 2>/dev/null' EXIT
sleep 1

step() { echo "=== $* ===" | sudo tee -a "$LOG" >/dev/null; sync; }

step "load 1: insmod $P1"
# shellcheck disable=SC2086
sudo insmod driver/apple-ave.ko $P1; RC1=$?
step "load 1 returned rc=$RC1"
sleep 3

# The Halt happens in here. If the core does not stop, rmmod is where the
# machine is most likely to wedge, so the marker goes in before the call.
step "unload 1 (the Halt is sent from ave_remove)"
sudo rmmod apple_ave; RCU=$?
step "unload 1 returned rc=$RCU"
sleep 2

if [ $RCU -ne 0 ]; then
    step "not attempting load 2: the unload failed"
    echo "$LOG"; exit 1
fi

step "load 2: insmod $P2"
# shellcheck disable=SC2086
sudo insmod driver/apple-ave.ko $P2; RC2=$?
step "load 2 returned rc=$RC2"
sleep 3

step "unload 2"
sudo rmmod apple_ave 2>/dev/null
step "done: load1=$RC1 unload1=$RCU load2=$RC2"
sleep 1

echo "$LOG"
