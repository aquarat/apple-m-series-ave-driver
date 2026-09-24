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
# NETCONSOLE=ip[:mac] mirrors every kernel line to a UDP receiver as it is
# printed. Local capture is worthless for the crash itself: f33 (2026-09-22)
# left ONE line on local disk and 307 on the receiver, because a PMU hard
# reset discards whatever the NVMe still holds, fsync or not. Receiver:
#   socat -u UDP-RECV:6666 CREATE:ave-netconsole.log
# The module does not survive a reboot, so this is re-done every run.
if [ -n "${NETCONSOLE:-}" ]; then
    NC_IP=${NETCONSOLE%%:*}
    NC_MAC=${NETCONSOLE#*:}; [ "$NC_MAC" = "$NETCONSOLE" ] && NC_MAC=$(ip neigh show "$NC_IP" | awk '{print $5}')
    NC_DEV=$(ip route get "$NC_IP" | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}' | head -1)
    NC_SRC=$(ip route get "$NC_IP" | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}' | head -1)
    ping -c 1 -W 2 "$NC_IP" >/dev/null || { echo "REFUSING: receiver $NC_IP does not answer" >&2; exit 1; }
    # The wired adapter (AX88179, cdc_ncm) batches tx frames for up to
    # tx_timer_usecs (400) and flushes them from a timer. A hang inside that
    # window loses exactly the tail we want. 0 sends every frame at once.
    NCM=/sys/class/net/$NC_DEV/cdc_ncm/tx_timer_usecs
    [ -e "$NCM" ] && echo 0 | sudo tee "$NCM" >/dev/null
    lsmod | grep -q '^netconsole' || sudo modprobe netconsole \
        "netconsole=6666@$NC_SRC/$NC_DEV,6666@$NC_IP/$NC_MAC"
    sudo dmesg -n 8
    echo "=== $NAME netconsole up: $NC_SRC/$NC_DEV -> $NC_IP/$NC_MAC $(date -Is) ===" | sudo tee /dev/kmsg >/dev/null
fi

LOG=results/$NAME-$(date +%s).kmsg
{ echo "=== $NAME $(date -Is) commit $(git rev-parse --short HEAD): insmod driver/apple-ave.ko $* ==="; } > "$LOG"; sync
sudo python3 tools/kmsg_capture.py "$LOG" &
CAP=$!
sleep 3	# let the header reach the disk before insmod (F6: lost otherwise)

step() { echo "=== $* ===" | sudo tee -a "$LOG" /dev/kmsg >/dev/null; sync; }

# OVERLAY=N applies test/ave-overlay.ko variant=N inside the captured window
# and then waits, with heartbeats, before loading the driver. F6 reset the
# machine about 10 s after the overlay was applied by hand and before any
# marker reached the disk, so there was no evidence which of the two did it.
if [ -n "${OVERLAY:-}" ]; then
    if lsmod | grep -q '^ave_overlay'; then
        step "overlay already applied this boot; OVERLAY=$OVERLAY ignored"
    else
        step "applying overlay variant=$OVERLAY"
        sudo insmod test/ave-overlay.ko variant="$OVERLAY"; RCO=$?
        step "overlay insmod returned rc=$RCO"
        [ $RCO -eq 0 ] || { sudo kill $CAP 2>/dev/null; echo "$LOG"; exit 1; }
        # Default 0 since f32 (2026-09-21). The 20 s wait was added after F6
        # so heartbeats would catch a death in this phase - and s2-9, f31 and
        # f31 again then died in exactly this phase, 15-20 s after the
        # overlay bound apple-dart to the DARTs, with our driver never
        # loaded. f32 loaded immediately and got past it. Set OVERLAY_WAIT
        # if you want to observe this window on purpose.
        W=${OVERLAY_WAIT:-0}
        for ((t = 0; t < W; t += 5)); do
            step "overlay applied, t=${t}s of ${W}s before insmod"
            sleep 5
        done
    fi
fi

step "insmod driver/apple-ave.ko $*"
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

# The session publishes its buffers under debugfs and remove() tears them
# down, so copy them before anything can unload - a successful encode would
# otherwise leave nothing behind.
DBG=/sys/kernel/debug/apple_ave
OUT=${LOG%.kmsg}-load1
# Only if THIS load produced them. A leaked teardown used to leave the
# previous load's entries in place, and f19b copied F18's frame under its own
# name before the driver learned to take them down (2026-09-20).
if [ $RC -ne 0 ]; then
    step "insmod failed (rc=$RC); not copying debugfs - anything there is not ours"
elif sudo test -d "$DBG" && [ -n "$(sudo ls -A "$DBG" 2>/dev/null)" ]; then
    mkdir -p "$OUT"
    for f in $(sudo ls "$DBG"); do sudo cat "$DBG/$f" > "$OUT/$f"; done
    sync
    step "saved debugfs to $OUT: $(ls "$OUT" | tr '\n' ' ')"
fi

# UNLOAD=1 attempts the docs/63 teardown AFTER the evidence is on disk, so a
# reset costs only the reboot and not the run.
if [ "${UNLOAD:-0}" = "1" ]; then
    step "unloading (Stop, Close, Halt, unmap while powered, gate last)"
    sudo rmmod apple_ave; RCU=$?
    step "rmmod returned rc=$RCU"
    sleep 3
    step "still alive 3 s after the unload"
    sleep 5
    step "still alive 8 s after the unload"
fi

step "e3-run done: $NAME"
sudo kill $CAP 2>/dev/null
echo "$LOG"
