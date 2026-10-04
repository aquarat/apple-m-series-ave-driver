# 95. Multi-pass encoding: the host interface

*2026-10-04, static analysis of the macOS 13.5 AVE firmware (M2 `H14G`,
M1 Pro `H13S`, M1 Max `H13C`), the AppleAVE2 kext (M2 kernelcache) and the
user-space encoder `AppleVideoEncoder.bundle`, plus Unicorn emulation of
firmware functions from the M2 snapshots of docs/93. No hardware was
touched.*

> **Update (docs/94, same day):** B-frames and multiple references now work
> (both motion-estimation units, wire 0xFCEA). Where this document says the
> adaptive B placement is blocked by the two-reference stall, that blocker
> is gone; the multi-pass tests below are still to be run.

## Result

| question | answer | label |
|---|---|---|
| Is there real 2-pass? | **Yes. It is VideoToolbox's multi-pass (`kVTCompressionPropertyKey_MultiPassStorage`): the host encodes every frame twice.** Pass 1 writes a 0x626-byte `S_AVE_MultiPassStats` record per frame into the **CodedHeader** buffer (+0x22638). Between passes, the host reorders and post-processes the records and builds a 0x108-byte sequence header. Pass 2 sends each frame again with a per-frame input buffer: header plus the first-pass records of a 10-frame lookahead window | C |
| Session switch | Start/INIT wire **0xFEFC** u8 `bEnableMultipass` = 1, wire **0xFF00** u32 pass = 1 (first) or 2 (final). The firmware turns pass 1 into **9** ("constant-QP first pass") when wire 0xFF04 (`MultiPassConstantQP`) ≥ 0 and 0xFF08 ≥ 0. Wire 0xFF04..0xFF10 are ConstantQP, QPModLevel, MaxQPModLevel (−1 → 6), Options (−1 → 0x1305) | C |
| Per-frame field | PICMGMT **+0x900** `sInput.MultiPassStatsInBuffer` (AVC Process wire **0x12C8**, HEVC_ENCODE wire **0x5EB0**). Pass 2 only. If it is 0 in pass 2 the firmware **asserts** in `ProcessPipeStart`, whatever the frame type | C (emulated) |
| Input buffer format | `[0x000,0x108)` sequence header, then records. **Frame 0: 11 records (frames 0..10), 0x44AA bytes. Frame N ≥ 1: one record, for frame N+10, 0x72E bytes.** The firmware keeps them in a 16-entry ring (`MPQueue<16>`) | C (emulated) |
| Second-pass entry | macOS sends **CAVE_CMD_RESET (id 3)**: the saved INIT parameters with wire VP+0xFEA0 forced to 2. The firmware runs `ResetBetweenPasses`, which re-runs `InitEncodingParameters`. A fresh session started with pass = 2 reaches the same code | C / I |
| What pass 2 does with the stats | Only with **frame type 5** (firmware decides): it loads the records, runs `CFrameType::FrameType(MPQueue)`, and places IDRs at the scene cuts the host marked. The rate controller allocates bits from first-pass complexity (`MpFinalPassAccumulate`, `finalPassSequenceLevel`, `finalPassSceneLevel`). Multi-pass **forces the bitrate RC arm**: there is no fixed-QP final pass | C |
| B-frames needed? | **No.** With BFrames (VP+0x18) = 0, the final-pass frame typing produces IDR and P only, scene IDRs included (emulated). Adaptive B placement (`LookAheadBFrames`) needs B-frames, which the two-reference stall blocks (docs/81) | C (emulated) |
| Firmware lookahead | No multi-frame lookahead in single pass. The M2's **LRME-RC** (H14G only) is a per-frame low-resolution pre-analysis (RC and weighted prediction). It is turned on by VP+0xFE6D `lrme_rc_pass_num` and **requires wire 0xFCE9 = 1** (async LRME pipe). docs/93 found that this flag hangs the M2 today | C / I |
| Benefit | Against AVE's 1-pass bitrate mode: most of the gap to fixed QP (docs/88: +31 % vs +21 % BD-rate against x265 `medium`), so roughly **5-10 %** in bitrate mode, with accurate size targeting. Against fixed QP: **0-3 %**, from scene-cut IDRs and complexity-based frame QPs. The larger lever, B-frames (~9 %, docs/92), stays blocked | I |

What the host has to compute between passes is the expensive part. The
user-space code that does it is located below (§2.4): scene detection,
scene accumulation, bit corrections, and the sequence header. It is
floating-point work, so it belongs in user space, not in the kernel driver.

---

## 0. Conventions and sources

- **fw** = H14G (M2) firmware VA unless marked `h13c`/`h13s` (file = VA + 0x4000).
  **kext** = M2 13.5 kernelcache VA. **UA** = `AppleVideoEncoder` VA
  (= file offset).
- **wire** = byte offset in the INIT (Start) command. **VP** = wire − 0x60.
  **RC** = wire − 0xFF30. **PICMGMT** = AVC Process wire − 0x9C8 (HEVC −0x55B0).
  `ctrl` = the `CAVCController` instance (the snapshots have it at
  0xffffffff80803760). **rec** = one `S_AVE_MultiPassStats` record.
- Labels: **[C]** read from an instruction (VA cited) or shown by emulation;
  **[I]** inferred, with the chain given; **[U]** unknown.
- Binaries: the firmware and kext as in docs/93. User space was fetched with
  `tools/fetch_userspace.py` (needs `pyfsapfs`; LZFSE via `liblzfse`) into
  `data/blobs/macos-13.5-userspace/` (git-ignored). SHA-256
  `b1f8fd38…834da803`, 1 271 712 bytes, the same build as docs/72.
- Scratch tools (not in the repo): `fd.py` (firmware disassembler with
  symbols), `ua.py` (user space, string xrefs), `kd.py` (kext), `ftemu.py`,
  `gftemu.py`, `emu_mp.py` (Unicorn, §3).

Firmware symbols present in all three images (H13C/H13S/H14G):
`CMultiPassControl::{Initialize, CollectMultiPassStats_{RC,LRME,MCPUs_AVC,MCPUs_HEVC},
MpFirstPassAccumulate, MpFinalPassAccumulate, MpConstantQpAccumulate,
finalPassSequenceLevel, finalPassSceneLevel, MpFinalPassSceneBitrates,
MpFinalPassUpdateSeqRcInfo, FirstPassStatsLookahead, InitCplxHistQuant,
QuantizeCplxHist, TranslateQpOffset}`, `CAVECommonController::ProcessFirstPassStats`,
`CFrameType::{FrameType(…MPQueue…), LookAheadBFrames, UpdateNextScene}`,
`{CAVC,CHEVC}Controller::ResetBetweenPasses`. The LRME-RC set
(`SetLRMERC*`, `StartLRMERC`, `ProcessLRMERCStart/Done`) exists only in
**H14G**; H13S has an 8-byte stub `AcquireAndTriggerLRMERC` (h13s 0x5e08). [C]

---

## 1. Three different things called "multi-pass"

| | what | host interface | fixed QP? | B-frames? | status |
|---|---|---|---|---|---|
| **(a) true 2-pass** | VideoToolbox `MultiPassStorage`: encode all frames, keep per-frame stats, encode all frames again | wire 0xFEFC/0xFF00 (+0xFF04..0xFF10); PICMGMT+0x900; CodedHeader+0x22638; RESET command | pass 1 may be constant QP (pass 9); **pass 2 is always bitrate RC** | not needed (IDR/P); adaptive B needs them | implementable now |
| **(b) LRME-RC** (M2 only) | per-frame low-res motion search run 1..n times before the main pipe; feeds RC and weighted prediction | VP+0xFE6D `lrme_rc_pass_num`, VP+0xFC89 (wire 0xFCE9) async LRME pipe | [U] | no | blocked: 0xFCE9 = 1 hangs (docs/93) |
| **(c) "lookahead"** | `LookAheadBFrames` / `MPQueue` | exists only inside (a)'s final pass, fed by the host's records | — | yes, by definition | blocked with B |

There is **no internal multi-frame lookahead** in a single-pass session.
`CFrameType::FrameType(…AdaptiveB…)` (fw 0x370c8) decides from past frames
only, and `MPQueue` is filled only by `ProcessFirstPassStats` (§2.5). [C]
There is no 13.5 host lookahead depth either (docs/81 §1.4).

---

## 2. True 2-pass, end to end

### 2.1 How macOS drives it (user space and kext)

1. The client sets `kVTCompressionPropertyKey_MultiPassStorage` (UA string
   "received kVTCompressionPropertyKey_MultiPassStorage", xref 0x16fa0).
   `AVE_ValidateEncoderParameters` then writes **VP+0xFE9C = 1 and
   VP+0xFEA0 = 1** (UA 0x2bec8-0x2bed8). It requires
   `RCModeDriver == AVE_RC_OFF`, i.e. firmware RC (UA 0x2bedc, assert
   string at 0x2bf18), and rejects DataRateLimits (VP+0xFE6E) with
   multi-pass (UA 0x2a9a0-0x2aa08). [C]
2. Private keys write the rest of the block. The handlers store at
   `x28 = S+0x104E0 = VP+0xFC80` (UA 0xefb0-0xefb4) [C]:

   | key | store (UA) | VP | wire |
   |---|---|---|---|
   | `EnableMultiPass` | 0x1e34c `strb [x28,#0x21c]` | 0xFE9C u8 | **0xFEFC** |
   | `SetMultiPassNum` | 0x1e38c `[x28,#0x220]` | 0xFEA0 u32 | **0xFF00** |
   | `MultiPassConstantQP` | 0x1e3cc `[x28,#0x224]` | 0xFEA4 s32 | 0xFF04 |
   | `MultiPassQPModLevel` | 0x1e40c `[x28,#0x228]` | 0xFEA8 s32 | 0xFF08 |
   | `MultiPassMaxQPModLevel` | 0x1e44c `[x28,#0x22c]` | 0xFEAC s32 | 0xFF0C |
   | `MultiPassOptions` | 0x1e48c `[x28,#0x230]` | 0xFEB0 s32 | 0xFF10 |
   | `lrmeRCPassNum` | 0x1dcf0 `strb [x28,#0x1ed]` | 0xFE6D u8 | 0xFECD |

   `AVE_SetEncoderDefault` sets 0xFEA4..0xFEB0 to −1 (UA 0x29ae8-0x29af4,
   docs/72). [C]
3. `VTCompressionSessionBeginPass` → `AVE_H264BeginPass` (UA 0x239f8). It
   increments the pass counter (maximum 2, UA 0x23bec-0x23bf8) and writes
   it to **VP+0xFEA0** (UA 0x23ee4-0x23eec). So the pass number is 1, then 2. [C]
4. **Pass 1.** Each frame is encoded normally, with frame type 5 as macOS
   always sends (docs/81). On completion, `SendFrame` takes
   **CodedHeader + 0x22638, 0x626 bytes** (UA 0x970c4-0x970e8). It writes
   the frame's CMTime PTS over rec+0x04..0x1B (UA 0x9709c-0x970b8). It then
   passes the record through the "MP" pipeline (§2.4) and stores it with
   `VTMultiPassStorageSetDataAtTimeStamp`, keyed by PTS. [C]
5. `VTCompressionSessionEndPass` → `AVE_H264EndPass` (UA 0x240b4).
   `FinalizeSeqRcInfo` builds the sequence header. If further passes are
   requested, the code calls `AppleAVEVA_ResetBetweenPasses` →
   `kAVEUserClientDriverResetBetweenPasses` → kext
   `AppleAVE2UserClient::Reset` (kext 0xfffffe0008e7fc80) → `AVE_Client_Reset`
   (0xfffffe0008eb5f28) → `ProcessCmd(…, 0xb)` (0xfffffe0008eb6568) →
   **`AVE_CHM_MakeFwCmd_Reset`** (0xfffffe0008e8fd14). The source frames
   for the next pass come back through `VTEncoderSessionSetTimeRangesForNextPass`. [C]
6. **Pass 2.** For each frame, `AVE_H264MultipassDataFetch` (UA 0x39a98)
   reads the stored records. The encode path copies them, behind the
   header, into the client IOSurface `pFirstPassStatsMapIOSurface` (UA
   0xa5ff4-0xa61a8) and sets FrameInfo+0x10 = 2 (UA 0xa5fac-0xa5fb4). The
   kext imports that surface as **`MultiPassStats`** (surface config 5)
   only when FrameInfo+0x10 == 2 (`AVE_Client_CreateMultiPassDataSurface`,
   kext 0xfffffe0008eaf094, test at 0x8eaf188, surface kept at
   FrameInfo+0x28, 0x8eaf354). `AVE_CHM_SetDataInfo_FwBuf` writes its DART
   address to **PICMGMT+0x900** (kext 0xfffffe0008e92fe0-0x8e92ff4). [C]

The HEVC path is the same: `AVE_HEVCBeginPass`/`EndPass`,
`AVE_MultipassDataFetch` (UA 0x857d0), the same 0x626 record and
11-record fetch (UA 0x85c90-0x85d88). [C]

### 2.2 The session block, as the firmware reads it

`CAVCController::InitEncodingParameters` (fw 0x4e6a0-0x4e73c; x27 = VP+0xF760):

```
ctrl+0x23FC4 u8  <- VP+0xFE9C   enable
ctrl+0x23FC8 u32 <- VP+0xFEA0   pass
ctrl+0x23FCC     <- VP+0xFEA4   ConstantQP
ctrl+0x23FD0     <- VP+0xFEA8   QPModLevel
ctrl+0x23FD4     <- VP+0xFEAC   (<0 -> 6)      MaxQPModLevel
ctrl+0x23FD8     <- VP+0xFEB0   (<0 -> 0x1305) Options
if (enable && pass == 1 && ConstantQP >= 0 && QPModLevel >= 0) pass = 9
```

[C]. (On H13C the same block is at ctrl+0x24D4C, docs/54 and docs/76.)
`CRateControl::ProcessInit` calls `CMultiPassControl::Initialize(codec,
chroma, pass, FEA4, FEA8, FEAC, FEB0)` when enable and pass are both
non-zero (fw 0x3ab84-0x3abac). It also forces the bitrate arm whatever
`ui32RCFlag` says (docs/76 §1.2). [C] `ProcessAccumulate` dispatches on the
pass (fw 0x3d048-0x3d0ac): **1 → `MpFirstPassAccumulate`, 2 →
`MpFinalPassAccumulate`, 9 → `MpConstantQpAccumulate`**. [C]
`ProcessRateControl` calls `finalPassSceneLevel` (fw 0x3ae9c) and
`finalPassSequenceLevel` (fw 0x3b128). [C]

In `Initialize` (fw 0x2c8f4), pass 9 combines ConstantQP and QPModLevel as
`(ConstantQP − base) × 10 + QPModLevel` and looks the result up in a
`{s16, u16}` table (fw 0x2cba8-0x2cc8c) to seed `QpUpdateByMultipass`. So
QPModLevel behaves like a tenth-of-a-QP refinement in pass 9 [I]. Options
bits 3, 9 and 11 select constants (fw 0x2c9c0-0x2ca34); their meaning is [U].

### 2.3 Pass 1: what the firmware produces

- **MCPU first-pass statistics.** `ConfigureMCPUs` sets **bit 24** of the
  MCPU parameter word when enable && (pass == 1 || pass == 9) (fw
  0x5268c-0x526ac). In the emulation this is the *only* register difference
  between pass 0 and pass 1 or 9 for a P frame. The seven MCPU parameter
  words change at 0x267408000, 0x267428000, 0x267448000, 0x267468000,
  0x267488000, 0x2674a8000 and 0x2674c8088 (§3.3). [C]
- **The record**, written into the frame's CodedHeader (mapped 0x22C60
  bytes; the driver allocates 0x23000, docs/54 #56):
  - `CollectMultiPassStats_RC` (fw 0x2d608): rec at **CodedHeader+0x22638**
    (fw 0x2d67c-0x2d688). It is gated on enable && (pass|8) == 9 in
    `ProcessRateControlAccumulateBits` (fw 0x536e0-0x536f4).
  - `CollectMultiPassStats_MCPUs_AVC` (fw 0x2d444) writes rec+0x54.. Same
    gate, in `CollectDataFromCpus` (fw 0x4da54-0x4da6c).
  - `CollectMultiPassStats_LRME` (fw 0x2ced8) writes rec+0xA0.. and a
    **1 KiB histogram at rec+0xB0** (fw 0x2cf30-0x2cf5c). It is called
    **unconditionally** from `ProcessLRMEDone` (fw 0x44084). So even
    single-pass coded headers carry that part. [C]
- `docs/54` #55 applies: with enable && (pass|8) == 9, the firmware asserts
  `sSVEMap.iNum == 1`. The driver already writes 1 (`ave_cmd.c:482`). [C]

### 2.4 Between passes: what the host computes

The user-space "MP" code (log prefix `MP:`) runs on every pass-1 record,
then once at the end. Locations [C]; algorithms [U] until ported:

| function (UA) | what it does (from its code and log strings) |
|---|---|
| `enqueue_first_pass` 0xb9a08 | reorders records from coding to display order (a heap keyed on rec+0x2C), with a small "fixup FIFO" |
| bits correction 0xb97b4 | adds the following record's rec+0x48 to rec+0x40 (`frame_bits`) and scales rec+0x44 (`hdr_bits`); updates the u64 sums at rec+0x4CC.. by class (rec+0x34) |
| `scene_change_pipeline` 0xb9130 (`histogram_diff` 0xb8270, `scene_change_detect` 0xb8398) | scene-cut detection from the LRME histograms; writes **rec+0x4B0** (scene-start flag) and rec+0x4B8/0x4BC (metrics, floats) |
| `accumulate_scene_info` 0xb84e8 | folds every non-start record into its scene's first record: rec+0x4C4 (frame count) and the u64 sums. Accumulates sequence totals: bits by class (rec+0x624: NORMAL/MIN/MAX/BLANK), complexity rec+0x614/0x618, and a 16-bin log10-complexity histogram (rec+0x574.., +0x5B4..) |
| `FinalizeSeqRcInfo` 0xb9ff4 | quantises the 16-bin histogram to 4 values and counts and closes the **sequence RC info**. Its 0x108 bytes at MP-object+0x63A0 are the pass-2 header (copier UA 0x899d4; fields printed by UA 0xb88c0: total_scenes, cnt/bits All/NORMAL/MIN/MAX/BLANK, avg_qscale, current_complexity, totalcplxsum) |

The firmware has its own `InitCplxHistQuant`/`QuantizeCplxHist`, so part of
this may be reproducible from the firmware's version too. [I]

### 2.5 Pass 2: the per-frame input buffer

The **gate** is enable && pass == 2, checked in three places:

- `CFlowControllerBase::SendCommandToQueue` calls `GetFrameType` only for
  PICMGMT+0xCAC == **5** (fw 0xef48-0xef68). Every other type skips the
  stats entirely. [C]
- `GetFrameType` (fw 0x1e7fc): for enable && pass == 2 (fw 0x1e97c-0x1e98c)
  it calls `ProcessFirstPassStats` (0x1e99c), then
  `CFrameType::FrameType(…MPQueue…)` (0x1e9c8). Otherwise it calls the
  AdaptiveB variant (0x1ea1c). [C]
- `ProcessPipeStart` (PipePrepareParam inlined) applies ui32RCFlag &&
  enable && pass == 2 (fw 0x45d14-0x45d38) for **every** frame type. If
  PICMGMT+0x900 is 0 it asserts (fw 0x45d40 → 0x46084; emulated, §3.3). [C]

`ProcessFirstPassStats` (fw 0x1e454) [C, and the emulation in §3.2 agrees]:

```
buf = MappedMemory(PICMGMT+0x900, frameNumber == 0 ? 0x44AA : 0x72E)
rec = buf + 0x108                       ; the 0x108-byte header comes first
queue = ctrl+0x24378: 16 x 0x626, count/head/tail at ctrl+0x2A5D8/DC/E0
frame 0:  push 11 records (rec, rec+0x626, ... ), FirstPassStatsLookahead() on each
frame N:  pop records with frameNumber < N, push ONE record (the host sends frame N+10)
          a record whose frameNumber (rec+0x2C) is 0 is pushed as invalid (fn = -1)
scene ring ctrl+0x2A5E4, 11 x 0x150: rec+0x4B0..0x5FF is copied when rec+0x4B0 != 0
```

`ProcessPipeStart` then does two things [C]:
- On the frame whose number equals ctrl+0xFBC (frame 0 of the pass) it
  copies the **0x108-byte header** to ctrl+0x7B4, which is
  `CMultiPassControl`+0x39C (the controller sits at ctrl+0x418, fw
  0x1e7cc) (fw 0x45d44-0x45d88).
- It copies the current scene's 0x150 block into ctrl+0x664 (fw
  0x45d94-0x45e40).

The HEVC path is fw 0x5d200-0x5d260, with a frame-window condition.

So the host-side buffer, per in-flight frame, is:

| frame (display index) | bytes | content |
|---|---|---|
| 0 | 0x44AA = 0x108 + 11×0x626 + 0x20 | header; records 0..10 (if the clip is shorter, macOS repeats the last record, UA 0x39fb0) |
| N ≥ 1 | 0x72E = 0x108 + 0x626 | header (re-sent; read only on frame 0); record N+10 (macOS repeats the last one at the end) |

Why N+10: after frame 0 the user-space cursor stands at frame 10, and each
later frame advances it once (UA 0x39ad8-0x3a0d8). The firmware queue pops
one record and pushes one, keeping 11. [C] for both halves; [I] that they
pair as stated.

**Second-pass entry.** `AVE_CHM_MakeFwCmd_Reset` (kext 0xfffffe0008e8fd14) [C]:
- id **3** (0x8e8fef8); +0x20 = 200 (0x8e8fed8); +0x28..0x37 = timeout
  (0x8e8fef0); +0x40 u8 = a client byte (0x8e8fee8).
- At +0x48: the saved INIT parameters, starting at VP: 0x10DB0 bytes AVC,
  0x32D68 HEVC (0x8e8ff2c-0x8e8ff48).
- **cmd+0xFEE8 = VP+0xFEA0 := 2** (0x8e8ff4c-0x8e8ff54).

The firmware (`CFlowControllerBase::ProcessQueue` 0xd5cc → vtable +0x170 =
`CAVCController::ResetBetweenPasses` 0x40390) [C]:
- logs "Clean Reset" when cmd+0x40 ≠ 0 (0x403c4) and forces VP+0xFEA0 = 2
  again (0x4042c);
- frees and re-creates the RC and LRME objects;
- calls `SetDefaultParameters`, then vtable +0x1D0 =
  `InitEncodingParameters` (0x4060c);
- answers **0xE03 RESET_DONE** (0xd5e8). It logs "hEncCmdQueue is supposed
  to be empty" (0xd570), so the host completes every frame first.

A new session started with pass = 2 goes through the same
`InitEncodingParameters` → `ProcessInit` → `Initialize(pass 2)` path. [I]

### 2.6 `S_AVE_MultiPassStats` (0x626 bytes), as far as read

| rec+ | what | who | label |
|---|---|---|---|
| 0x00-0x1B | firmware u64/u32; host overwrites 0x04..0x1B with the CMTime PTS | fw 0x2d70c, UA 0x9709c | C |
| 0x1C / 0x20 | width / height in pixels (MB count × 16) | fw 0x2d690-0x2d6dc (args from 0x53708-0x53728); `FrameType` rounds both to 32 (0x388e8) | C |
| 0x24 | frame rate, float (30.0 if unset) | fw 0x2d6e4-0x2d72c | C |
| 0x2C | display frame number; the host checks it against its count | fw 0x2d790; UA 0x39ee4 | C |
| 0x30 | slice type | fw 0x2d82c | C |
| 0x34 | frame class (host buckets it; `FrameType` counts it, 0x389fc) | | C read, meaning I |
| 0x40 / 0x44 / 0x48 | frame bits / header bits / correction for the previous frame | UA 0xb97b4 | I |
| 0x54.. | MCPU statistics | fw 0x2d4a0 | C |
| 0xA0, 0xB0-0x4AF | LRME data, 256 × u32 histogram | fw 0x2cf30 | C |
| 0x4B0 | scene start (host) | UA 0xb94ac; fw 0x38418 | C |
| 0x4B4 | scene's first frame [I] | fw 0x45de4 compares it with the current frame | I |
| 0x4B8 / 0x4BC | scene-change metrics, floats (host) | UA 0xb93a8 | C |
| 0x4C4 | frames in the scene (host accumulates). `UpdateNextScene` sets next scene = current + this | UA 0xb8630-0xb8774; fw 0x38434 | C |
| 0x4CC-0x5FF | scene sums (u64 bits by class, 16-bin log10-complexity counts at 0x574 and sums at 0x5B4) | UA 0xb8664-0xb8698 | C |
| 0x600-0x613 | firmware per-frame values | fw 0x2df70.. | C |
| 0x614 / 0x618 | complexity floats | UA 0xb85fc, 0xb8650 | C |
| 0x624 | u16 class 0..3 (NORMAL/MIN/MAX/BLANK) | UA 0xb869c | I |

---

## 3. Emulation (tools/fwemu style, M2 snapshot S3)

Every run started from the docs/93 snapshot (a one-reference P session,
fixed QP), with patches applied in Unicorn memory only.

### 3.1 `CFrameType::FrameType(MPQueue)` alone (`ftemu.py`)

I put synthetic records in `ctrl`'s MPQueue (frames N..N+10) and called the
function once per display frame, with the session's own CFrameType object
(ctrl+0x1B0; IdrPeriod patched to 30):

| BFrames (VP+0x18 → ft+0x88) | host scene marks | types chosen |
|---|---|---|
| 0 | none | IDR, then P×29, IDR at 30 |
| 1 | none | IDR B P B P … |
| 3 | none | IDR B B B P … |
| 0 | cuts at 12 and 50 (rec+0x4B0 = 1, rec+0x4C4 = frames to the next cut) | IDR at **0, 12, 29, 50**: the cut-aligned IDRs, and the regular one moved to split the 12..50 scene evenly (`UpdateNextScene` 0x38474-0x384a4) |
| 1 | cuts at 12 and 50 | B/P with IDRs at 12, 29, 50 |

**With BFrames = 0 the final pass never chooses a B.** [C, emulated]

### 3.2 `GetFrameType` in final-pass mode (`gftemu.py`)

Patches: ctrl+0x23FC4 = 1, ctrl+0x23FC8 = 2, every frame type 5, and
PICMGMT+0x900 pointing at a synthetic buffer (0x108 zero header +
records). `MappedMemory::HwToTarget` is stubbed to return that buffer.

- Frame 0: the firmware maps **0x44AA** bytes and the queue holds fn 0..10.
  Frames 1, 2, …: it maps **0x72E** bytes, the head advances, and the tail
  receives fn 11, 12, …
- With a cut marked at 17: IDR at 0 and **17**, P elsewhere.
- No assert and no unmapped access over 40 frames. [C, emulated]

### 3.3 `ProcessPipeStart` per pass (`emu_mp.py`)

RC flag (ctrl+0xB0C) patched to 1 in every run:

| run | result |
|---|---|
| pass 0 | 2964 MMIO writes (as docs/93) |
| pass 1, and pass 9 | identical, except **MCPU parameter bit 24** in 7 words (§2.3) |
| pass 2, PICMGMT+0x900 set | identical to pass 0 for this P frame (the header is read only on frame 0) |
| pass 2, PICMGMT+0x900 = 0 | **`_bsp_assert_fail` from 0x46088** after 158 writes |

The snapshot's RC object was built for a fixed-QP session and
`Initialize` never ran, so these runs prove structure and gates, **not** QP
values. [C, emulated, structure only]

---

## 4. LRME-RC (M2 only)

- User space: `lrmeRCPassNum` → VP+0xFE6D (§2.1). If non-zero,
  `AVE_ValidateEncoderParameters` sets **VP+0xFC89 = 1** and RC+0x38 bit 0
  ("LRMERC enabled -> must run in LRME-pipe async", UA 0x2ca48-0x2caf4). [C]
- Firmware: VP+0xFE6D → ctrl+0x23415 and VP+0xFE6C → ctrl+0x23414, but
  only when VP+0xFC89 ≠ 0 (fw 0x4e8a8-0x4e900). `ProcessLRMERCStart`
  (0x53788) prints `lrmeRCPassNum` and runs `SetLRMERC` → `SetLRMERCPerRef`
  → `SetLRMERCPerPass` (0x53858-0x539cc). `SetLRMERCPerRef` programs
  per-reference weighted-prediction scale/offset (MMIO window
  +0x1154144, +0x1128450). `ProcessLRMERCDone` (0x544f4) counts the passes
  and notifies the flow controller. User-space timing tables name up to six
  passes (P0..P5). Results surface: `LowResRCResult` (slot 13), read through
  PICMGMT+0x4F58 (docs/32). [C] for the code, [I] for "RC + weighted
  prediction pre-analysis".
- **Wire 0xFCE9 is what the driver calls `session_src_bit3`** (docs/69). On
  the M2, setting it alone hangs a one-reference frame (docs/93 §3). LRME-RC
  therefore waits for the async LRME pipe to work. It does not depend on
  B-frames or on 2-pass. [C]/[I]

---

## 5. Fixed QP, rate control and B-frames

- **Pass 2 is always bitrate RC**: enable && pass ≠ 0 forces the bitrate
  arm (docs/76 §1.2). So multi-pass cannot be layered on a fixed-QP
  session. The closest to "fixed QP" is: pass 1 = constant-QP first pass
  (pass 9, wire 0xFF04 = QP, 0xFF08 = 0), then pass 2 at a bitrate equal
  to pass 1's total bits. That spends the same bits distributed by
  complexity, a CRF-like encode. [C] mechanism, [I] behaviour.
- Pass 1 does not need type 5. Explicit IDR/P types still collect stats
  (the collectors sit in RC accumulate, §2.3). [I]
- Pass 2 **must use type 5 for every frame, including frame 0**. That is
  what feeds `MPQueue` and runs the first-pass frame typing, and docs/53
  mq3 shows that an explicit IDR on frame 0 breaks CFrameType's GOP state.
  Type 5 already works on hardware (docs/53 mq4). Its only failure there
  was the B the firmware chose because BFrames was 1. [C]
- **B-frames are not needed** with VP+0x18 = 0 and VP+0x1D = 0 (§3.1). The
  firmware's adaptive B (`LookAheadBFrames` 0x384ac) needs BFrames > 0, and
  hence the two-reference fix (docs/81, docs/93). [C]

---

## 6. Proposed driver design

### 6.1 ABI (`ave_abi.h`, 13.5 layouts)

```c
/* start_avc / start_hevc (shared VP): */
u32 mp_enable;        /* wire 0xFEFC u8  */
u32 mp_pass;          /* wire 0xFF00 u32: 0, 1, 2 (9 is derived by fw) */
u32 mp_const_qp;      /* wire 0xFF04 s32, -1 = off */
u32 mp_qpmod;         /* wire 0xFF08 s32, -1 */
u32 mp_max_qpmod;     /* wire 0xFF0C s32, -1 (fw: 6) */
u32 mp_options;       /* wire 0xFF10 s32, -1 (fw: 0x1305) */
u32 lrme_rc_passes;   /* wire 0xFECD u8, H14G only; needs 0xFCE9 */
/* process: */
u32 pic_mp_in;        /* AVC 0x12C8, HEVC 0x5EB0, u64 IOVA */
/* coded header: */
#define AVE_MP_REC_OFF   0x22638
#define AVE_MP_REC_SIZE  0x626
#define AVE_MP_HDR_SIZE  0x108
#define AVE_MP_WINDOW    10       /* records after the current one */
#define AVE_MP_IN_FIRST  0x44AA   /* 0x108 + 11*0x626 + 0x20 */
#define AVE_MP_IN_NEXT   0x72E
/* RESET: id 3, AVC 0x48 + 0x10DB0, HEVC 0x48 + 0x32D68; reply 0xE03 */
```

`tools/abi_selftest` gets checks for every one of these (VAs above).

### 6.2 Session and buffers

- `mp_pass` is chosen at session start. Pass 1 needs firmware RC: the
  bitrate path (`session_bitrate` / V4L2 bitrate mode); the constant-QP
  first pass also sets `mp_const_qp`.
- Pass 2: RC on, BFrames 0, AdaptB 0, frame type 5 for every frame, and one
  **MP input buffer per command slot**: 0x44AA rounded to 0x8000, 32-bit
  IOVA like every other buffer (docs/71). Before each Process, the driver
  fills it (header + 1 or 11 records) from a stats table it holds, then sets
  PICMGMT+0x900. **The driver must refuse pass 2 without a table**, since a
  zero address is a firmware assert.
- After each completion in pass 1, copy CodedHeader+0x22638 (0x626 bytes)
  out. In pass 2, report the firmware's chosen type (coded header
  `FrameTypeReturned`, docs/81 §1.4) as the V4L2 keyframe flag.
- Second pass: first as a **new session with mp_pass = 2** (no new command
  needed). Then as `CAVE_CMD_RESET` (id 3, header as above, cmd+0x40 = 1)
  after a drain, to avoid re-allocating.

### 6.3 User interface

There is no float math in the kernel. The kernel moves blobs; user space
(a small `tools/ave-2pass` helper, or the application) ports the five
UA functions of §2.4.
- **Lab path (first):** module/debugfs. `session_mp_pass=1|2`; debugfs
  `apple_ave_mp` returns the pass-1 records (frame-indexed) and accepts the
  pass-2 table (header + records) for the probe-time self-test.
- **V4L2 path (later):** private controls `AVE_CID_MP_PASS` (menu 0/1/2),
  `AVE_CID_MP_CONST_QP`, `_QPMOD`, `_MAXQPMOD`, `_OPTIONS` (defaults −1),
  and `AVE_CID_MP_TABLE_FD`, a dma-buf (e.g. udmabuf over a memfd) laid out
  as `header[0x108] + rec[n_frames][0x626]`. In pass 1 the driver writes
  record n at its display index; in pass 2 it reads from it. That keeps
  per-frame data out of control races in the M2M job flow. ffmpeg's
  v4l2m2m has no 2-pass, so the helper drives V4L2 directly.

---

## 7. Hardware test plan (M2; the lead agent only; one variable per run)

Each run comes from a fresh boot with the usual harness (`tools/lab-run.sh`)
and the probe-time self-test at 1280×720. Repeat each one before believing it.

| # | change (one) | expect (success) | a "no" looks like |
|---|---|---|---|
| T0 | none (control): current bitrate-mode self-test, N = 30, plus a dump of CodedHeader+0x22638..+0x22C5E per frame | frames encode as today; rec+0xB0..0x4AF (LRME histogram) non-zero, because that collector is unconditional; rec+0x2C..0x40 **zero or stale** | histogram zero too → the LRME collector did not run; recheck before T1 |
| T1 | wire 0xFEFC = 1, 0xFF00 = 1 (0xFF04..0xFF10 = −1) | all 30 frames complete; rec+0x2C = frame index, rec+0x1C/0x20 = 1280/720, rec+0x24 = the frame rate as a float, rec+0x40 ≈ 8 × coded bytes; optionally read back MCPU word 0x267408000 = bit 24 set (a register the firmware wrote, so safe, docs/93 §4) | hang or assert (watch `sSVEMap`), or rec+0x2C still zero → the gate is not what §2.3 says |
| T2 | T1 + 0xFF04 = 30, 0xFF08 = 0 (pass 9) | records as in T1; slice QP ≈ 30 in every frame (`h264_parse.py`) | QPs still move under RC → pass 9 not taken |
| T3 | second session: 0xFF00 = 2, every frame type 5, BFrames 0, PICMGMT+0x900 → buffer built from T1's records and a **zero** header | 30 frames complete, IDR then P (FrameTypeReturned 3, 1, …), no assert | assert/hang (0x900 not seen, or the header zero is fatal); QP pinned at min/max → the header is needed (go to T4) |
| T4 | T3 with the header and scene fields from the user-space port (§2.4) | file size within a few % of target; QPs vary with content | size miss ≫ 1-pass RC |
| T5 | T4 on a clip with one hard cut, the cut marked | IDR exactly at the cut | no IDR → rec+0x4B0/0x4C4 semantics wrong |
| T6 | T4 via `CAVE_CMD_RESET` instead of a new session | RESET_DONE 0xE03, then identical output to T4 | |
| T7 | BD-rate bench (docs/88): 2-pass at pass-1-size vs 1-pass bitrate mode vs fixed QP | §8 | |

Do not set wire 0xFCE9 or `lrme_rc_pass_num` in any of these runs (§4).

---

## 8. Expected compression benefit [I]

- **Against AVE's 1-pass bitrate mode**: the final pass knows every
  frame's complexity and the scene structure, so it allocates bits nearly
  like constant quality. docs/88 measures bitrate mode at +31.2 % (PSNR-Y)
  BD-rate against x265 `medium`, versus +20.9 % for fixed QP. 2-pass should
  recover most of that ~8-10 % and hit a size target. That is the main
  value.
- **Against fixed QP** (today's best): small, about 0-3 %. It comes from
  IDRs aligned to scene cuts (zero on the 4-second single-scene bench
  clips) and from complexity-driven frame QPs. A constant-QP first pass
  followed by a same-size final pass is the test of that (T7).
- **Not available without B-frames:** adaptive B (~9 % from B-frames,
  docs/92). LRME-RC's weighted prediction (fades) waits on the async LRME
  pipe.

## 9. Open questions

- Exact header layout and scene-detection thresholds: port UA
  0xb8270-0xba5d0 (about 10 KB of code). [U]
- Whether a zero or approximate header is safe (T3 answers it), and the
  meaning of Options bits and QPModLevel outside pass 9. [U]
- Writer of rec+0x4B4. Whether the firmware uses the PTS at rec+0x04. [U]
- Whether the RESET header byte cmd+0x40 is "clean reset" = 1 for macOS
  (it comes from a client byte, kext 0x8e8fee8). [I]

## 10. Reproduce

```sh
S=tools/fwemu/mp
.venv/bin/python $S/fd.py h14g __ZN20CAVECommonController21ProcessFirstPassStats  # fw
.venv/bin/python $S/ua.py xref "sizeof(S_AVE_MultiPassStats)"                     # user space
.venv/bin/python $S/kd.py __Z23AVE_CHM_MakeFwCmd_ResetP10                          # kext
.venv/bin/python $S/ftemu.py  <snap>/S3 64 idr=30 bframes=0 scene=12,50
.venv/bin/python $S/gftemu.py <snap>/S3 40 idr=30 scene=17
PATCH="0xffffffff8080426c:01000000;0xffffffff80827724:01;0xffffffff80827728:01000000" \
  .venv/bin/python $S/emu_mp.py <snap>/S3 __ZN14CAVCController16ProcessPipeStartEPv \
  0xffffffff80803760 0x1a2b00 > rc1.txt     # then tools/fwemu/diff.py rc0.txt rc1.txt
```
