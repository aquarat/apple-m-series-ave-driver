# Open questions and roadmap

## Open questions, in priority order

Answered items have been removed; the history is in the git log and in the
individual docs. What remains:

1. **Per-frame `Process` fields.** QP, frame type, input surface and output
   buffer live in `AVE_PICMGMT_PARAMS` inside `sCAveCmdAvcProcess`. The block
   map is done ([20](20-command-structs.md)) but the field-to-offset join is
   not. **This is the last thing between here and a first encode.**
2. **`_E_AVE_RCMode` / `_E_AVE_EncMode` enumerator names.** Neither binary
   contains a name-string array for them. Constant-QP is believed to be
   `RCMode = 3` but that is **inferred, not read**, and it may not be
   statically recoverable at all. Needed to select fixed-QP for first light.
3. **PMGR phandles.** The ADT gives power-gate *indices*; they must be
   cross-referenced against the t6001 PMGR nodes. Not disassembly work.
4. **`iAddr` alignment.** [21](21-buffer-publication.md) reports 64-byte for AVC
   and 128-byte for HEVC, but no `% 128` assertion exists in either binary and
   the claim could not be reproduced. Either find the enforcing instruction or
   drop it. The 64-byte *stride* and *plane-offset* rules are confirmed and
   independent of this.
5. **`reg[3]`** (`0x8E588000`, 36 bytes) — no call site found by anyone.
   Map it and ignore it.
6. **ADT interrupts 1024–1027** — unclaimed by Apple's own driver.
7. **Remaining struct interiors** — `_S_AVE_Session_PFCfg`, the rest of
   `AvcStart` (~11.5 KB), the HEVC variants, the `pPicParams` name join.
   Laborious but not blocking.

## Status

| Phase | State |
|---|---|
| 0 — static recon | **done** |
| 1 — fwextract plumbing | not started (see note below) |
| 2 — static host-side analysis | **substantially done** |
| 2b — tracing | not started (not on the critical path) |
| 3 — transport bring-up | **blocked** — see [25](25-bringup-results.md) |
| 4 — first light | not started |
| 5 — V4L2 driver | not started |

Phase 2 has produced most of what a driver needs to reach a first `Open`:
the power-up order, the firmware load contract, the ASC start sequence, the
IPC ring and doorbell, the interrupt path, the wire command ids and the
command header. Still no driver code exists, and the following are still
missing before an encode can be attempted:

- ~~**Surface size and alignment formulas.**~~ **Done** — see docs 14–17.
  Frame-size primitives, the enforced 64-byte stride rule, the 35-slot
  InfoSet, and per-surface sizes at 1920x1080.
- ~~**Coded-output buffer size.**~~ **Answered** — it is a closed form. See
  [18-coded-data-sizing.md](18-coded-data-sizing.md); implemented as
  `ave_coded_data_size()` in `driver/ave_abi.h`.
- **Per-frame `Process` fields.** `Open`, `Config`, `Reset` and the session
  parameters in `Start` are mapped ([20](20-command-structs.md),
  [21](21-buffer-publication.md)). What is still missing is the per-frame QP,
  frame type, input surface and output buffer, all inside `AVE_PICMGMT_PARAMS`.
  This is now the last thing between here and a first encode.
- **`reg[3]`** (`0x8E588000`, 36 bytes) — no call site found by anyone.
- **ADT interrupts 1024–1027** — unclaimed by the host driver; purpose unknown.

Phase 1 note: the original justification was opening a conversation with
upstream. That no longer applies. The firmware still has to reach
`/lib/firmware` for a driver to work, but a local extraction script covers it —
and per [09-firmware-load.md](09-firmware-load.md), iBoot pre-loads the image
and the kext adopts it via `segment-ranges`, so the Linux path may look more
like DCP/ISP than like a `request_firmware()` blob.

## Sequencing

**Phase 0 — static (done).** Firmware and ADT obtained and analysed; RTKit
established; command set and state machine recovered.

**Phase 1 — extraction plumbing.** Patch `asahi-fwextract` to collect
`AppleAVE2FW_*.im4p`. Small, self-contained, upstreamable, useful before any
driver exists, and a reasonable way to open the conversation with upstream.

**Phase 2 — static host-side analysis (no hardware).** *In progress.* The
kext is extracted and its 91 classes mapped, which established the transport
model and the wire command set. What remains is the disassembly that turns
names into values: `AVE_HwC`, `AVE_IPC`, `AVE_PMGR`, `AVE_FwImg` and the
`AVE_CHM_MakeFwCmd_*` builders. This was previously assumed
to require tracing; it does not. See [06-kext.md](06-kext.md). Expected to
yield the wire ids, the channel layout, the power sequence and the bring-up
order.

**Phase 2b — tracing, if still needed.** Requires a second machine for the
m1n1 hypervisor serial console. Run `ffmpeg -c:v h264_videotoolbox` under the hypervisor with
MMIO/DART tracing on `ave0`. Trace at **two levels**, not one:

- hypervisor MMIO + DART underneath, and
- `IOConnectCallMethod` interposition in userspace on macOS.

The upper trace matters because VideoToolbox does real work in userspace, and
the split between framework / kext / firmware is not yet known. MMIO traces
alone will not show it.

Best treated as confirmation of the static work rather than the primary
source, and as the way to close anything Phase 2 could not.

**Phase 3 — transport.** Bring up `apple-rtkit` against AVE: boot the firmware,
attach endpoints, get crashlog and syslog endpoints responding. Standard RTKit
endpoints exist in this firmware (`RTK_crashlog_*`, `RTK_tracekit_*`), so
there is a self-check available before any encode is attempted.

**Phase 4 — first light.** `Open` → `Config` → `Start_AVC` → `Process_AVC` →
`Complete`, fixed QP, I-frames only, one resolution. Success criterion: the
output decodes in `ffmpeg` without error. It does not need to look good.

**Phase 5 — V4L2 stateful M2M driver.** Then FFmpeg and GStreamer work without
new userspace.

## Reality check

AVD took a very capable person years, part-time. AVE has more surface area.
But two things make it less bad than that comparison suggests:

- The transport is standard RTKit, not a bespoke bare-metal shim.
- The firmware kept its symbols, so the protocol is substantially readable
  statically rather than purely by inference from traces.
- **Both sides are available.** The kext supplies the host half — 1198 AVE
  methods with meaningful names. Between the two, most of the protocol is
  recoverable without ever powering on the hardware.

The validation loop is also far friendlier than a decoder's. A decoder must
match a reference bit-exactly; an encoder only has to emit a *legal*
bitstream, and `ffmpeg` gives ground truth on every attempt. Quality can be
improved incrementally after first light.

Realistic estimate: months, not years. The second machine is no longer the
gate on progress — Phase 2 can proceed entirely offline, and hardware is only
needed once there is something to run.

## Payoff

Software x264 on M1 Max P-cores handles 1080p adequately. The real wins are
power draw, and 4K / multi-stream / concurrent-session work where the two
hardware instances matter.
