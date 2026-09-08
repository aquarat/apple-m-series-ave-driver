# Open questions and roadmap

## Open questions, in priority order

Answered items have been removed; the history is in the git log and in the
individual docs. What remains:

1. ~~**Per-frame `Process` fields.**~~ **Answered** —
   [32](32-picmgmt-params.md). Frame type, input surface, output buffer,
   reconstruction target and the reference lists are located, and the useful
   negative result is that **there is no per-frame QP at all**: QP is
   session-scoped from `Start`, selected by slice type in
   `ConstantQpRateControl::processRateControl` (fw `0x8bd4`). The block also
   shrank from 0x5118 bytes to four copied sub-ranges; anything outside them
   is dropped before the encoder sees it. Implemented as the `AVE_PIC_*`
   offsets in `driver/ave_abi.h`.
2. **`_E_AVE_RCMode` / `_E_AVE_EncMode` enumerator names.** Neither binary
   contains a name-string array for them. Constant-QP is believed to be
   `RCMode = 3` but that is **inferred, not read**, and it may not be
   statically recoverable at all. Needed to select fixed-QP for first light.
   Now the **top blocking static question** — see [35](35-rc-modes.md).
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
| 3 — transport bring-up | **candidate fix pending hardware test** — [34](34-boot-handshake.md) |
| 4 — IPC transport live | spec in progress — [36](36-ipc-implementation.md) |
| 5 — session setup | struct map substantially done — [20](20-command-structs.md), [37](37-start-avc-session.md) |
| 6 — one encoded frame | per-frame block mapped — [32](32-picmgmt-params.md) |
| 7 — V4L2 driver | not started |

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
- ~~**Per-frame `Process` fields.**~~ **Done** — [32](32-picmgmt-params.md).
  The remaining gap before a first encode is no longer a struct map: it is
  that the firmware has not yet spoken to us at all (phase 3).
- **`reg[3]`** (`0x8E588000`, 36 bytes) — no call site found by anyone.
- **ADT interrupts 1024–1027** — unclaimed by the host driver; purpose unknown.

Phase 1 note: the original justification was opening a conversation with
upstream. That no longer applies. The firmware still has to reach
`/lib/firmware` for a driver to work, but a local extraction script covers it —
and per [09-firmware-load.md](09-firmware-load.md), iBoot pre-loads the image
and the kext adopts it via `segment-ranges`, so the Linux path may look more
like DCP/ISP than like a `request_firmware()` blob.

## Sequencing

Phases 0-2 are done. What follows is the route to `ffmpeg` encoding.

### Phase 3 — the firmware boots  *(current blocker)*

Everything up to starting the coprocessor works; it changes state and then says
nothing ([31](31-bringup-state.md)).

The leading suspect is now the **boot handshake**, not stream IDs, and two
independent lines of evidence agree on it ([34](34-boot-handshake.md),
[33](33-firmware-logging.md)): `StartUpIOP` writes `0x08042006` to scratch 0
plus a 56-byte boot-config IOVA before releasing the core, and on the firmware
side `AVE_Log_Output` is dead while `gs_psCfg` is NULL (`0xa9a0`) — a global
populated only from that same block. The handshake and the log therefore stand
or fall together, which is consistent with silence rather than being a second
unknown. Implemented; awaiting a hardware test.

That test needs a **power cycle**, not a module reload: nothing we have found
ever clears ASC `CPU_CONTROL`, so the core has been running continuously since
the first `asc-start` and a reload starts an already-started core. Earlier
"still silent" results taken that way measured the wrong thing — the same
shape of error as the address-translation bug ([30](30-address-translation-bug.md)).
`tools/handshake-test.sh` enforces this by recording `boot_id`.

Still on the list if the handshake is not the answer:

- stream IDs: `sids` is `0x8001` (SIDs 0 and 15) with `bypass = 0x8000`, so SID
  15 runs in bypass under Apple and `apple_dart_hw_reset` strips it on probe.
  If instruction fetch uses SID 15 it is translating through an empty table.
- watch for DART translation faults to confirm where the fetch goes

**Done when:** a scratch register changes, or an interrupt arrives, without us
having written it.

### Phase 4 — IPC transport live

- second mailbox exchange yields the channel descriptor array
- `CreateChannel` for `"IO"` and `"IO_T2H"`; ring send/receive with the
  phase-bit protocol ([08](08-ipc-transport.md))
- **get the firmware's syslog out.** It carries `AVE_Log` with per-subsystem
  levels and format strings that name fields verbatim; from here on it is the
  best oracle we have ([23](23-empirical-bringup.md))

**Done when:** we can send a command and read a reply.

### Phase 5 — session setup

Command ids, sizes and the 64-byte header are known and firmware-enforced
([07](07-commands-abi.md)). Bodies:

- `Config` (`0x78`) — mapped ([20](20-command-structs.md))
- `Open` (`0x48`) — mapped; the 8 bytes past the header are unused
- `Start_AVC` (`0x3180`) — key fields located: width `+0x368`, height `+0x36c`,
  QP `+0x240/244/248`, GOP `+0x2b8`, bitrate `+0x238`, RCMode `+0x234`

**Done when:** the firmware accepts `Config` -> `Open` -> `Start_AVC` without
rejecting them. It rejects wrong sizes outright, so mistakes are loud.

### Phase 6 — one encoded frame

The real remaining unknown. `Process` is `0x63D8` and its per-frame fields —
QP, frame type, input surface, output buffer — live in `AVE_PICMGMT_PARAMS`
(`0x5118` bytes) which is **not** mapped. Surface sizes and the 64-byte stride
rule are known ([14](14-frame-size-formulas.md)-[17](17-aux-engines-pools.md));
buffer publication is partly mapped ([21](21-buffer-publication.md)).

Fixed QP, I-frames only, one resolution. With the firmware log working, the
field hunt becomes differential testing against a self-describing oracle: set a
candidate offset, encode, parse the output, read back what changed
([23](23-empirical-bringup.md)).

**Done when:** `ffmpeg -v error -i out.h264 -f null -` is silent.

### Phase 7 — V4L2 M2M driver

Conventional work once the hardware path exists: `videobuf2`, m2m ops, format
negotiation enforcing the 64-byte stride and 2-plane rules, `V4L2_CID_MPEG_*`
controls onto the `Start_AVC` fields, and the capture-side buffer sized by
`ave_coded_data_size()`.

**Done when:** `ffmpeg -c:v h264_v4l2m2m` works with an unmodified ffmpeg.

### Phase 8 — beyond first light

Rate-control modes, B-frames and real GOP structures, HEVC, the second encoder
instance, concurrent sessions, zero-copy Interchange input, power management.
Then upstreaming: proper DT bindings, m1n1 support for the AVE node, and
firmware packaging via `asahi-fwextract`.

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
