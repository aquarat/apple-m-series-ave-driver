# The host ↔ firmware protocol

Apple shipped `AppleAVE2FW` with its **symbol table and C++ source paths
intact** — 1558 symbols, 69 source paths. A large part of the interface is
therefore recoverable statically, before any tracing.

> **Version note (2026-09-13).** Everything below was read from the **macOS
> 26.6.2** image and stands for 26.6.2. On **macOS 13.5**, the firmware this
> machine runs ([43](43-macos-13.5-firmware.md)): 1390 symbols and no
> `ProcessCmd_*`; `CmdProcessor` is at `0xd614` with a **14-entry** table at
> `0xe2e8` (ids 1..14, id 10 unhandled, names from the `eCAveCmdID` table at
> `0xed060`: 1 `CONFIG`, 2 `START`, 3 `RESET`, 4 `AVC_INIT`, 5 `HEVC_INIT`,
> 6 `UNINIT`, 7 `AVC_ENCODE`, 8 `HEVC_ENCODE`, 9 `LRME_STANDALONE`,
> 10 `MCTF_PROCESS`, 11 `AVE_FLUSH`, 12 `STOP`, 13 `COMPLETE`, 14 `POWERDOWN`).
> **Confirmed.** Command structs and sizes per version:
> [46](46-abi-13.5-commands-session.md); boot/IPC per version:
> [45](45-abi-13.5-boot-ipc.md).

Regenerate everything here with:

```sh
python3 tools/extract_protocol.py data/blobs/ave_h13c.bin
```

## Command set

Dispatcher: `CFlowControllerBase::CmdProcessor(void*, uint32_t, uint32_t*)`
at `0x28134`. It reads a **u16 command id at offset 0** of the command struct
(`ldrh w8, [x19]` at `0x28244`), rejects anything where `id - 1 > 0xb`, and
branches through a 12-entry jump table at `0x28c6c`:

| id | handler | notes |
|---:|---|---|
| 1 | `ProcessCmd_Config` | struct size `0x78` |
| 2 | `ProcessCmd_Halt` | |
| 3 | `ProcessCmd_Open` | session open — `client_id` appears here |
| 4 | `ProcessCmd_Close` | |
| 5 | *unused* | jump-table entry targets the default block |
| 6 | `ProcessCmd_Start_AVC` / `_HEVC` | codec chosen by field `+0x1c` (1=AVC, 2=HEVC) |
| 7 | `ProcessCmd_Stop` | |
| 8 | `ProcessCmd_Process_*` | engine chosen by field `+0x18` (AVC/HEVC/LRME/MCTF/DMV) |
| 9 | `ProcessCmd_Complete` | |
| 10 | `ProcessCmd_Priority` | |
| 11 | `ProcessCmd_Flush` | |
| 12 | `ProcessCmd_Reset` | struct size `0x13F08` (81,672 bytes) |

Every command except `Config` and `Reset` uses a `0x48`-byte struct.

`CFlowControllerBase::ProcessCmd_Start(uint64_t, uint32_t, void*)` at `0x31e44`
takes arguments and sits outside that run — it is a helper called by
`Start_AVC` / `Start_HEVC`, not a table entry.

> **Corrected.** An earlier revision of this document listed these handlers in
> `__text` emission order (Reset=0, Config=1, Open=2, …) and inferred that to be
> the wire ordering. It is not. The table above is the real dispatch, read out
> of the firmware's own jump table at `0x28c6c` and verified against the
> per-command size checks. See [07-commands-abi.md](07-commands-abi.md).

## Pipeline state machine

A contiguous `__cstring` run at `0x122a87`, used by `SendState()` and logged as
`remoteSystemState[%d] = %s`:

```
0 INVALID          5 LRME_RC_START     10 PIPE_DONE
1 AVE_READY        6 LRME_RC_DONE      11 XC_START
2 ENQUEUE          7 PIPE_RESET_READY  12 XC_DONE
3 LRME_FS_START    8 PIPE_RESET_DONE   13 CMD_ACK
4 LRME_FS_DONE     9 PIPE_START        14 CMD_READY
```

The shape of an encode is legible from this: enqueue → motion estimation
(full-search, then rate-control pass) → pipe → transcode (`XC`, entropy
coding) → done. Same caveat: ordering is the string order, which is probably
but not certainly the enum order.

## Hardware engines

`AVE_EventTrace(_S_AVE_HWEventTrace*, int*, _E_AVE_EngineID, _E_AVE_HWEMode,
int, _E_AVE_HWEState, const unsigned*)` confirms an engine-id enum. Engines
named across the firmware: **LRME** (with FS and RS controllers —
`CLRMEFSController`, `CLRMERSController`), **MCTF**, **DMV**
(`CDMVController`), **PIPE**, **XC** (transcode/entropy), and **MCPU**
(`CAVEPipeMcpuController`, `CAvePipeMcpu`) — a per-pipe microcontroller.
Scheduling across them is `CAVEHWEnginesScheduler`.

## What lives in firmware

Confirmed from the source paths — these are things the driver does **not**
implement:

```
AppleAVE2FW/Firmware/Algorithm/RateControl/CRateControl.cpp
AppleAVE2FW/Firmware/Algorithm/RateControl/CAVEMultiPass.cpp
AppleAVE2FW/Firmware/Algorithm/GOP/CFrameType.cpp
AppleAVE2FW/Firmware/Algorithm/Reference/CAVEDPB.cpp
AppleAVE2FW/Firmware/Algorithm/Reference/CAVERefManager.cpp
AppleAVE2FW/Firmware/Algorithm/QPModulation/CStaticAreasLowQP.cpp
AppleAVE2FW/External/Algorithm/RateControl.cpp
```

Rate control, GOP/frame-type decisions, DPB and reference management, and QP
modulation are all firmware-side. **This settles the driver model: AVE is a
V4L2 _stateful_ (M2M) encoder.** FFmpeg and GStreamer already speak that
interface.

`ConstantQpRateControl` exists as a mode alongside `CRateControl`. That matters
for bring-up: a fixed-QP, I-frame-only first light does not require defeating
or understanding rate control.

Bitstream header generation is also in firmware — `H264_headers.cpp`,
`Hevc_headers.cpp`, `Slice_headers_util.cpp`, with `AVC_SPS`, `AVC_PPS`,
`AVC_Slice`, `HEVC_VPS/SPS/PPS/PTL/RPS/Slice`, `AVE_SyntaxWriter`.

## Codec support

H.264 (AVC) and HEVC, including **MV-HEVC** (multiview, i.e. spatial video) —
`ctrl->numTemporalLayers <= HEVC_AVE2_MAX_TLAYER` indicates temporal layers
are supported. ProRes is **not** here; it is a separate hardware block.

## Session model

Multi-client: `CAVEClient`, `client_id` (64-bit), `CAVEPriorityQueue`,
`ProcessCmd_Priority`, and per-client state in `CFlowControllerBase`
(`PrintHwClientStatus(uint32_t, int)`). Combined with two hardware instances,
concurrent encode sessions look like a first-class design point.

## ABI type names

`data/derived/types.txt` has all 167. The ones that will matter for the
shared-memory ABI:

```
_S_AVE_Session_PFCfg        AVE_PICMGMT_PARAMS / CAVE_PICMGMT_PARAMS
AVE_PICMGMT_RC_UPDATE_DATA  _S_AVE_RefManager_FrameInfo
ReferenceFrameInfoData      _S_AVE_HWEventTrace
_E_AVE_EngineID             _E_AVE_HWEMode / _E_AVE_HWEState
_E_AVE_FwStats              _E_AVE_MCTFStats
```

Field layouts are **not** recoverable from symbols — those need tracing or
disassembly of the accessors.
