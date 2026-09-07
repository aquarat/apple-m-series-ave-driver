# Open questions and roadmap

## Open questions, in priority order

1. **Numeric wire command ids.** The command *set* is known from both sides
   (`AVE_HwC::SendFwCmd_*`, `CFlowControllerBase::ProcessCmd_*`). The ids are
   written into the command struct by `AVE_CHM_MakeFwCmd_*`; recovering them
   means following those field stores. **Static work — no hypervisor needed.**
2. **Shared-memory channel layout.** `AVE_IPC` (`CreateChannel`, `Send`/`Recv`,
   `Kernel2FwAddr`) is the real data path. Disassembling `AVE_IPC::Init` and
   `CreateChannel` should give the ring layout. **Also static.**
3. **RTKit mailbox endpoint ids.** Up to 8 routes, populated at runtime in BSS.
   Now known to gate *bring-up only*, not the data path — and RTKit's own
   management endpoint enumerates endpoints during the boot handshake, which
   `apple-rtkit` already parses. So this may resolve itself on first boot
   rather than needing a trace.
4. ~~**Which `reg` range is the ASC?**~~ **Answered** — `reg[1] + 0x400000`.
   `reg[2]` is the SVE control block (doorbell, status, scratch, idle).
   `reg[3]` (36 bytes) remains unidentified.
5. ~~**Is `H13C` the right variant for `t6001`?**~~ **Answered** — yes, from the
   IPSW `BuildManifest.plist`. See `data/derived/board-to-ave-firmware.txt`.
6. **Struct layouts.** `sCAveCmdOpen` is 120 bytes; the rest
   (`AVE_PICMGMT_PARAMS`, `_S_AVE_Session_PFCfg`, `_S_AVE_FrameInfo`) need
   disassembly of their accessors. Static, but laborious.
7. ~~**Power sequencing.**~~ **Answered** — see [10-power.md](10-power.md).
   `AVE_PMGR` performs no MMIO; it drives `AppleARMIODevice` by ADT
   `power-gates` index, so Linux's `apple-pmgr-pwrstate` covers it. The eleven
   domains and their dependency graph are recovered.
8. ~~**Input pixel formats.**~~ **Answered** — see
   [12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md). AVE accepts Interchange
   (lossless variant only on M1 Pro/Max), so zero-copy capture→encode is
   possible; and plain `420v`/`420f` exists on both codecs, so the simple NV12
   bring-up path is available. Surface size/alignment formulas are the
   remaining gap.


## Status

| Phase | State |
|---|---|
| 0 — static recon | **done** |
| 1 — fwextract plumbing | not started (see note below) |
| 2 — static host-side analysis | **substantially done** |
| 2b — tracing | not started (not on the critical path) |
| 3 — transport bring-up | not started |
| 4 — first light | not started |
| 5 — V4L2 driver | not started |

Phase 2 has produced most of what a driver needs to reach a first `Open`:
the power-up order, the firmware load contract, the ASC start sequence, the
IPC ring and doorbell, the interrupt path, the wire command ids and the
command header. Still no driver code exists, and the following are still
missing before an encode can be attempted:

- **Surface size and alignment formulas** (`AVE_Work_Enc_CalcSurfaceInfo` and
  five siblings, undisassembled). You cannot allocate a frame buffer without
  these. This is the single largest remaining gap.
- **Command struct interiors** beyond the common `0x40` header — including the
  81,672-byte `Reset` payload and the ~104 KB userspace `Prepare`/`Start` blob.
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
