#!/bin/bash
# Run one tools/e3-run.sh step on the target from the host, and collect it.
# Addresses come from lab.env (git-ignored; see AGENTS.md).
#   tools/lab-run.sh NAME "ENV=.. ENV=.." module-params...
# e.g.
#   tools/lab-run.sh f37b "OVERLAY=5 OVERLAY_WAIT=20 HOLD=5" stop_after=16 ...
#
# Launches e3-run.sh detached on the target (a dropped SSH session cannot
# kill it half way), waits on the netconsole receiver - not on the target -
# until the run's end marker arrives, or the log has been quiet QUIET s,
# then copies results/ back and writes the run's slice of the receiver log
# to $LOGDIR/NAME.nc.log. Prints the outcome and the last lines received.
set -u
. "$(dirname "$0")/../lab.env" || { echo "lab.env missing (AGENTS.md)" >&2; exit 1; }
NC=$RX_IP:$RX_MAC
QUIET=${QUIET:-30}
MAXWAIT=${MAXWAIT:-600}
NAME=$1; ENVS=$2; shift 2
RX=$LOGDIR/ave-netconsole.log
cd "$(dirname "$0")/.."

systemctl is-active -q ave-netconsole || { echo "receiver service not running" >&2; exit 1; }
pre=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$TARGET" \
    'lsmod | grep -cE "^(apple_ave|ave_overlay)"; sudo dmesg | grep -c "Disabling IRQ"; systemctl is-active netconsole-ave') \
    || { echo "target unreachable" >&2; exit 1; }
read -r loaded irqs ncsvc <<<"$(echo $pre)"
[ "$loaded" = 0 ] || { echo "REFUSING: AVE modules already loaded this boot" >&2; exit 1; }
[ "$irqs" = 0 ] || { echo "REFUSING: an IRQ was disabled this boot" >&2; exit 1; }
[ "$ncsvc" = active ] || echo "warning: netconsole-ave is $ncsvc; e3-run.sh will load netconsole itself"

start=$(wc -l < "$RX")
t0=$(date +%s)
echo "$(date -Is) launching $NAME: $ENVS NETCONSOLE=$NC e3-run.sh $NAME $*"
ssh -o BatchMode=yes "$TARGET" "cd ~/Projects/apple-ave-driver; $ENVS NETCONSOLE=$NC \
    nohup setsid tools/e3-run.sh $NAME $* > /tmp/$NAME.out 2>&1 < /dev/null &" \
    || { echo "launch failed" >&2; exit 1; }

END='=== (saved debugfs|insmod failed)'
grep -q 'UNLOAD=1' <<<"$ENVS" && END='=== still alive 8 s after the unload'
outcome=""
while :; do
    sleep 1
    now=$(date +%s)
    if tail -n +$((start + 1)) "$RX" | grep -qE "$END"; then outcome=completed; break; fi
    last=$(stat -c %Y "$RX")
    if [ $((now - t0)) -gt 20 ] && [ $((now - last)) -ge "$QUIET" ]; then
        if ! ping -c 1 -W 2 "$TARGET" >/dev/null; then outcome="DIED (quiet ${QUIET}s, target not answering)"; break; fi
        running=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$TARGET" "pgrep -fc '[e]3-run.sh $NAME'" 2>/dev/null)
        case "$running" in
        0) outcome="ended without an end marker (target up)"; break ;;
        "") outcome="DIED? (quiet ${QUIET}s, pings but no ssh)"; break ;;
        esac   # still running, just quiet (long HOLD)
    fi
    [ $((now - t0)) -gt "$MAXWAIT" ] && { outcome="gave up after ${MAXWAIT}s"; break; }
done

tail -n +$((start + 1)) "$RX" > "$LOGDIR/$NAME.nc.log"
echo "$(date -Is) $NAME: $outcome after $(( $(date +%s) - t0 ))s, $(wc -l < "$LOGDIR/$NAME.nc.log") lines -> $LOGDIR/$NAME.nc.log"
grep -h 'RESULT' "$LOGDIR/$NAME.nc.log"
echo "--- last lines received:"
tail -4 "$LOGDIR/$NAME.nc.log"
case "$outcome" in
DIED*|ended*) echo "--- waiting for the target to come back"; tools/lab-reboot.sh --wait "$start" ;;
esac
if ping -c 1 -W 2 "$TARGET" >/dev/null; then
    rsync -a "$TARGET:Projects/apple-ave-driver/results/" results/
    ls -d results/$NAME-* 2>/dev/null
fi
