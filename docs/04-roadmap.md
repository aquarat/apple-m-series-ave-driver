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
4. **Which `reg` range is the ASC?** `0x20D050000`+`0x8000` is the candidate.
5. **Is `H13C` the right variant for `t6001`?** Inferred from file sizes.
   `AVE_FwImg::RetrieveInfo` / `AVE_DevInfo` in the kext should settle it —
   static.
6. **Struct layouts.** `sCAveCmdOpen` is 120 bytes; the rest
   (`AVE_PICMGMT_PARAMS`, `_S_AVE_Session_PFCfg`, `_S_AVE_FrameInfo`) need
   disassembly of their accessors. Static, but laborious.
7. **Power sequencing.** Eleven gates per instance; `AVE_PMGR`'s
   `SetPSDependencyUp/Down` and `CheckPeerUp/Down` encode the order — static.
8. **Input pixel formats.** Does AVE accept the "Interchange" tiled format
   shared by AVD/AGX/DCP? If so, zero-copy capture→encode is possible. Worth
   an early check but should **not** gate anything — an NV12 path will exist.


## Status

| Phase | State |
|---|---|
| 0 — static recon | **done** |
| 1 — fwextract plumbing | not started |
| 2 — static host-side analysis | **~25%** — architecture mapped, no values recovered |
| 2b — tracing | not started (no longer on the critical path) |
| 3 — transport bring-up | not started |
| 4 — first light | not started |
| 5 — V4L2 driver | not started |

What exists today is a **map, not a specification**. Every finding so far is a
name or a structural relationship. There are no numbers a driver could use:
no command ids, no struct field offsets, no register offsets within the five
MMIO ranges, no ring format, no power-up order, no firmware load procedure.
No driver code has been written.

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
