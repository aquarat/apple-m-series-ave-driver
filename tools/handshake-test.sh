#!/bin/bash
# Post-reboot boot-handshake test.
#
# MUST BE RUN ON A FRESH BOOT. Nothing in the driver, and nothing we have
# found in the hardware, ever clears ASC CPU_CONTROL. The core has therefore
# been running continuously since the first asc-start of the previous boot,
# and a module reload does not undo that: it starts an already-started core,
# which is not the experiment we want to run. Only a power cycle gives the
# firmware a first instruction to execute.
#
# Everything lands in results/handshake-<boot>.log so it survives the reboot
# and can be read back later.
#
#   AVE_I_MEAN_IT=1 ./tools/handshake-test.sh
#
set -u

if [ "${AVE_I_MEAN_IT:-}" != "1" ]; then
    echo "REFUSING TO RUN. See tools/bringup.sh for why. Use AVE_I_MEAN_IT=1." >&2
    exit 1
fi

cd "$(dirname "$0")/.."
REPO=$PWD
OUT=$REPO/results
mkdir -p "$OUT"
BOOT=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | cut -c1-8)
LOG=$OUT/handshake-$(date +%Y%m%d-%H%M%S)-$BOOT.log
MARK=$OUT/handshake.marker

# --- Refuse to run on a boot that has already started the core -------------
if [ -f "$MARK" ] && [ "$(cat "$MARK" 2>/dev/null)" = "$BOOT" ]; then
    cat >&2 <<EOF
REFUSING TO RUN.

This boot ($BOOT) has already started the AVE core. Nothing clears ASC
CPU_CONTROL, so a second run would be starting an already-running core and
the result would not mean anything.

Reboot, then run this again.
EOF
    exit 1
fi

exec > >(tee -a "$LOG") 2>&1
echo "=== AVE boot handshake test ==="
echo "date      : $(date -Is)"
echo "boot_id   : $BOOT"
echo "uptime    : $(cut -d' ' -f1 /proc/uptime)s   <-- must be small; this must be a fresh boot"
echo "kernel    : $(uname -r)"
echo "commit    : $(git -C "$REPO" rev-parse --short HEAD)"
echo "log       : $LOG"
echo

if ! lsmod | grep -q ave_overlay; then
    echo "!! ave_overlay is not loaded - the AVE node does not exist yet."
    echo "   Load it first, then re-run:  sudo insmod test/ave_overlay_mod.ko"
    exit 1
fi

echo "$BOOT" > "$MARK"; sync; sync

# Marker + settle. The sync is not decorative: if the next step hangs the
# machine, the log above is the only thing that survives.
echo ">>> syncing and settling before the risky part"
sync; sync; sleep 8

dmesg -C 2>/dev/null || true

echo ">>> insmod apple-ave.ko stop_after=15"
sudo insmod driver/apple-ave.ko stop_after=15 &
INS=$!

# Capture continuously rather than once at the end: if the machine dies mid
# probe, whatever reached the ring buffer before the hang is already on disk.
for _ in $(seq 1 60); do
    dmesg 2>/dev/null | grep -a 'apple-ave\|ave ' > "$OUT/.live" && sync
    kill -0 $INS 2>/dev/null || break
    sleep 0.2
done
wait $INS 2>/dev/null
sleep 2
dmesg | grep -a 'apple-ave\|ave ' || echo "(no driver output in dmesg)"
sync

echo
echo "=== done. full log: $LOG ==="
