#!/bin/bash
# AVE bring-up test: overlay (with the DART) + staged driver probe.
#
# Does NOT need a fresh boot. ave_asc_start writes CPU_CONTROL = 0 before
# starting the core, the driver halts the core on remove, and the VENC domains
# gate off on rmmod, so every run starts from a halted, power-cycled core. (An
# earlier version of this header claimed the opposite; that was wrong.)
#
# Everything lands in results/handshake-<time>-<boot>.log, synced as it goes,
# so it survives a hang.
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

# Previous runs on this boot are recorded but no longer refused - see above.
if [ -f "$MARK" ] && [ "$(cat "$MARK" 2>/dev/null)" = "$BOOT" ]; then
    echo ">>> note: this boot has already run the handshake test at least once."
    echo "    That is fine: the VENC domains gate off on rmmod and asc_start"
    echo "    clears CPU_CONTROL, so the core is power-cycled between runs."
fi

exec > >(tee -a "$LOG") 2>&1
echo "=== AVE boot handshake test ==="
echo "date      : $(date -Is)"
echo "boot_id   : $BOOT"
echo "uptime    : $(cut -d' ' -f1 /proc/uptime)s"
echo "kernel    : $(uname -r)"
echo "commit    : $(git -C "$REPO" rev-parse --short HEAD)"
echo "log       : $LOG"
echo

# Several kernels are installed (the 7.1.13 VRR builds; dnf installonly_limit
# is 5). A module built for one will not load on another, and the resulting
# insmod error is far less clear than saying so here - so check, and rebuild
# rather than fail.
WANT=$(uname -r)
for m in driver/apple-ave.ko test/ave-overlay.ko; do
    HAVE=$(modinfo "$m" 2>/dev/null | awk '/^vermagic:/{print $2}')
    if [ "$HAVE" != "$WANT" ]; then
        echo ">>> $m was built for ${HAVE:-nothing}, running kernel is $WANT - rebuilding"
        KD=/lib/modules/$WANT/build
        [ -e "$KD" ] || { echo "!! no kernel-devel for $WANT; install it and re-run"; exit 1; }
        make -C "$(dirname "$m")" KDIR="$KD" >/dev/null || { echo "!! rebuild failed"; exit 1; }
    fi
done
echo ">>> modules match running kernel $WANT"

# The overlay creates the AVE node. It has no module_exit (apple_dart_remove
# resets the DART with no runtime resume and hangs the machine), so it loads
# once per boot and stays.
if lsmod | grep -q '^ave_overlay'; then
    echo ">>> overlay already loaded"
else
    echo ">>> insmod test/ave-overlay.ko"
    sudo insmod test/ave-overlay.ko || { echo "!! overlay insmod failed"; exit 1; }
    sleep 1
fi
if [ ! -e /sys/bus/platform/devices/*.ave ] 2>/dev/null; then :; fi
echo ">>> AVE platform device: $(ls -d /sys/bus/platform/devices/*ave* 2>/dev/null || echo NONE)"

echo "$BOOT" > "$MARK"; sync; sync

# Marker + settle. The sync is not decorative: if the next step hangs the
# machine, the log above is the only thing that survives.
echo ">>> syncing and settling before the risky part"
sync; sync; sleep 8

sudo dmesg -C 2>/dev/null || true

# A failed probe still leaves the module loaded, so the next insmod fails
# with "File exists" and the run is wasted. Unload first if present.
if lsmod | grep -q '^apple_ave'; then
    echo ">>> apple_ave already loaded - removing it first"
    sudo rmmod apple_ave || { echo "!! rmmod failed"; exit 1; }
    sleep 2
fi

# Positive control for the liveness test: DCP's ASC, whose firmware is running.
# The driver only READS it, but a read of a power-gated block hangs the fabric,
# so pass the address only when DCP is bound and its domain reports on.
CTL=0x38bc00000
CTL_DEV=38bc00000.dcp
CTL_DOM=$(sudo awk -v d="$CTL_DEV" '
    /^[^ ]/ { dom=$1; st=$2 }
    $1 == d { print dom, st; exit }' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null)
PARAMS="stop_after=15 asc_timer=1"
if [ -e /sys/bus/platform/devices/$CTL_DEV/driver ] && [ "${CTL_DOM#* }" = "on" ]; then
    PARAMS="$PARAMS ctl_asc=$CTL"
    echo ">>> positive control: $CTL_DEV bound, domain ${CTL_DOM% *} on"
else
    echo ">>> positive control SKIPPED: $CTL_DEV domain '${CTL_DOM:-not found}'"
fi

echo ">>> insmod apple-ave.ko $PARAMS"
sudo insmod driver/apple-ave.ko $PARAMS &
INS=$!

# Capture continuously rather than once at the end: if the machine dies mid
# probe, whatever reached the ring buffer before the hang is already on disk.
for _ in $(seq 1 60); do
    sudo dmesg 2>/dev/null | grep -a 'apple-ave\|ave ' > "$OUT/.live" && sync
    kill -0 $INS 2>/dev/null || break
    sleep 0.2
done
wait $INS 2>/dev/null
sleep 2
sudo dmesg | grep -a 'apple-ave\|ave ' || echo "(no driver output in dmesg)"
sync

echo
echo "=== done. full log: $LOG ==="
