#!/bin/bash
# Sweep Start_AVC wire 0xFEC0 (session_src_mode) across loads in ONE boot.
#
#   tools/src-sweep.sh NAME value [value ...]
#
# The two registers it feeds sit 0x80 apart in the source-reader block -
# 0x40D120050 in the luma half, 0x40D1200D0 in the chroma half, the same
# offset within each - so this is very likely ONE per-plane field, luma in
# v & 3 and chroma in v >> 2, not two unrelated knobs (docs/62 6.2).
#
# Each value gets its own load, encode, and clean unload (Stop, Close, Halt,
# unmap, gate). The source is a constant 200, so the verdict per run is a
# single number: the decoded luma. 200 means the source DMA finally read our
# buffer; 130 means it did not, which is what every run so far has produced.
#
# Stops at the first sign the machine is no longer trustworthy.
set -u
cd "$(dirname "$0")/.."
NAME=$1; shift

BASE="stop_after=16 fw_map_data=1 fw_map_text=2 smmu_watch=1 session_selftest=1
      session_frame=1 session_lsb=1 power_me1=1 dpe_tunables=1 session_coloc=1
      session_entropy_size=1 session_flat_luma=200"
SUM=results/$NAME-summary.txt
: > "$SUM"

for v in "$@"; do
    if sudo dmesg | grep -q "Disabling IRQ #"; then
        echo "STOPPING: an IRQ was disabled; reboot before continuing" | tee -a "$SUM"; exit 1
    fi
    if lsmod | grep -q '^apple_ave'; then
        echo "STOPPING: apple_ave is still loaded; the last unload did not finish" | tee -a "$SUM"; exit 1
    fi
    # An unclean teardown unloads successfully while leaking its buffers and
    # abandoning the me1 holder. Loading again on top of that would be
    # measuring a machine that has already been told to reboot.
    if [ -n "${LOG:-}" ] && grep -q "teardown was not clean" "$LOG"; then
        echo "STOPPING: the previous unload was not clean; reboot before continuing" | tee -a "$SUM"; exit 1
    fi
    # No recovery flags here on purpose. A load that finds a core halted by
    # an earlier load recovers by itself now (auto_recover, ave_core_reset);
    # passing core_reset=2 from the second sweep value onwards was this
    # script's own bug - it counted loads within the SWEEP, while what
    # matters is loads within the BOOT, so the first value in a sweep that
    # followed any earlier load died at stage 13 with "ASC did not become
    # idle (status 0x2e)". That is s1-9 (docs/53).
    # shellcheck disable=SC2086
    LOG=$(OVERLAY=4 HOLD=5 UNLOAD=1 tools/e3-run.sh "$NAME-$v" $BASE session_src_mode="$v")
    DIR=${LOG%.kmsg}-load1
    REG=$(grep -o "mode +0x50 0x[0-9a-f]* +0xD0 0x[0-9a-f]*" "$LOG" | tail -1)
    BYTES=$(grep -o "frame 0: [0-9]* bytes" "$LOG" | tail -1 | tr -cd '0-9')
    LUMA="(no frame)"
    if [ -f "$DIR/frame.h264" ]; then
        if ffmpeg -v error -y -f h264 -i "$DIR/frame.h264" -pix_fmt gray \
                  -f rawvideo /tmp/sweep.yuv 2>/dev/null; then
            LUMA=$(python3 -c "
d=open('/tmp/sweep.yuv','rb').read()
s=set(d)
print(f'{len(s)} distinct, first {d[0] if d else -1}')" 2>/dev/null || echo "(decode failed)")
        fi
    fi
    printf 'src_mode=%-6s %-34s coded=%-6s decoded luma: %s\n' \
           "$v" "${REG:-no register line}" "${BYTES:-none}" "$LUMA" | tee -a "$SUM"
    # A load that did not even reach an encode means the sweep is measuring
    # the harness, not the hardware. Stop and say so rather than producing a
    # column of "(no frame)".
    if [ -z "${BYTES:-}" ]; then
        echo "STOPPING: src_mode=$v produced no frame; see $LOG" | tee -a "$SUM"
        exit 1
    fi
done
echo "--- $SUM ---"
