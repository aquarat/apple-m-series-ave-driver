# Open questions and roadmap

## Open questions, in priority order

1. **Mailbox endpoint ids.** Up to 8 RTKit routes; numbers are populated at
   runtime in BSS. Needs a hypervisor trace or deeper disassembly of
   `PlatformIOPIPCManager::InitMailboxRoute` (`0xe66a4`). *Blocks everything.*
2. **Which `reg` range is the ASC?** `0x20D050000`+`0x8000` is the candidate.
   Cheap to confirm once tracing works.
3. **Is `H13C` the right variant for `t6001`?** Inferred from file sizes.
   Confirm against `AppleAVE2.kext`'s selection logic.
4. **Shared-memory ABI.** `_S_AVE_Session_PFCfg`, `AVE_PICMGMT_PARAMS` etc.
   Names known, layouts not.
5. **Power sequencing.** Eleven power/clock gates per instance, order unknown.
6. **Input pixel formats.** Does AVE accept the "Interchange" tiled format
   shared by AVD/AGX/DCP? If so, zero-copy capture→encode is possible. Worth
   an early check but should **not** gate anything — an NV12 path will exist.
7. **Command wire ids.** Handler names confirmed; the enum ordering assumed
   from `__text` layout needs one trace to verify.

## Sequencing

**Phase 0 — static (done).** Firmware and ADT obtained and analysed; RTKit
established; command set and state machine recovered.

**Phase 1 — extraction plumbing.** Patch `asahi-fwextract` to collect
`AppleAVE2FW_*.im4p`. Small, self-contained, upstreamable, useful before any
driver exists, and a reasonable way to open the conversation with upstream.

**Phase 2 — tracing.** Requires a second machine for the m1n1 hypervisor
serial console. Run `ffmpeg -c:v h264_videotoolbox` under the hypervisor with
MMIO/DART tracing on `ave0`. Trace at **two levels**, not one:

- hypervisor MMIO + DART underneath, and
- `IOConnectCallMethod` interposition in userspace on macOS.

The upper trace matters because VideoToolbox does real work in userspace, and
the split between framework / kext / firmware is not yet known. MMIO traces
alone will not show it.

This should resolve questions 1, 2, 4 and 7 together.

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

The validation loop is also far friendlier than a decoder's. A decoder must
match a reference bit-exactly; an encoder only has to emit a *legal*
bitstream, and `ffmpeg` gives ground truth on every attempt. Quality can be
improved incrementally after first light.

Realistic estimate: months, not years — but Phase 2 needs hardware access and
a second machine, and that is the gate.

## Payoff

Software x264 on M1 Max P-cores handles 1080p adequately. The real wins are
power draw, and 4K / multi-stream / concurrent-session work where the two
hardware instances matter.
