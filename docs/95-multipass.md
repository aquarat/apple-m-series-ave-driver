# 95. Multi-pass encoding: the host interface

*2026-10-04, static analysis of the macOS 13.5 AVE firmware (M2 `H14G`,
M1 Pro `H13S`, M1 Max `H13C`), the AppleAVE2 kext (M2 kernelcache) and the
user-space encoder `AppleVideoEncoder.bundle`, plus Unicorn emulation of
firmware functions from the M2 snapshots of docs/93. No hardware was
touched.*

> **Update (docs/94, same day):** B-frames and multiple references now work
> (both motion-estimation units, wire 0xFCEA). Where this document says the
> adaptive B placement is blocked by the two-reference stall, that blocker
> is gone.
>
> **Update (same day): §12.** The final pass front-loads because the
> firmware clamps its first frame to QP 36 or below and then reaches the
> right level only through a buffer longer than the bench clips. The
> emulation reproduces every hardware frame QP. Two host-side table options
> (`ave2pass build --rc-scene 1 --scene-qscale bits`) flatten it under
> emulation; the hardware tests are §12.7.
>
> **Update (hardware, same day): §11.** The interface works on the M2 and
> the M1 Pro as mapped here (T1-T5, byte-identical across the two), with
> two corrections: the LRME collector is not unconditional (§2.3), and the
> final pass needs MaxKeyFrameIntervalDuration (§2.7). The benchmark says
> the firmware's 2-pass, driven this way, is **worse** than its own 1-pass
> VBR (+5 % VMAF / +11 % PSNR-Y BD-rate, and a worse size match): §11.4.

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
| Benefit | Expected: 5-10 % over 1-pass bitrate mode, with accurate size targeting [I]. **Measured (§11.4, H.264, M2): +5.2 % VMAF / +11.3 % PSNR-Y BD-rate against 1-pass VBR, i.e. worse, and a 13.5 % mean size miss against 7.3 %.** The final pass front-loads its bits: its first frame is clamped to QP 36 or below (§12). **With the host-side fix of §12 (`ave2pass build --rc-scene 1 --scene-qscale bits`): −0.6 % VMAF / −1.8 % PSNR-Y against 1-pass VBR, 4.3 % mean size miss (§12.9)** | C (measured) |

What the host has to compute between passes is the expensive part: scene
detection, scene accumulation, bit corrections, and the sequence header.
§2.4 describes macOS's code for it, and `tools/ave2pass/` reproduces it
byte for byte (Apple's functions under emulation, plus a pure-Python
port). It is floating-point work, so it belongs in user space, not in the
kernel driver.

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
    single-pass coded headers carry that part. [C] **Not so on hardware
    (§11.1): a single-pass coded header is written only in its first
    0x190 bytes; the record appears with the multi-pass enable.**
- `docs/54` #55 applies: with enable && (pass|8) == 9, the firmware asserts
  `sSVEMap.iNum == 1`. The driver already writes 1 (`ave_cmd.c:482`). [C]

### 2.4 Between passes: what the host computes

*Filled in from the user-space code (UA 0xb8270-0xbbc00, about 3 700
instructions) and by running it: `tools/ave2pass/` runs Apple's functions
under Unicorn (`mpemu.py`) and has a pure-Python port (`mpport.py`) that
matches the emulation byte for byte on every record set tried (§2.4.8).*

The user-space "MP" code (log prefix `MP:`) runs on every pass-1 record as
it completes, then once at the end of the pass. It is pure computation:
apart from memcpy/memmove, `operator new`/`delete` and logging, the only
imports it calls are `CFDataCreateMutable`/`CFDataAppendBytes`/`CFRelease`
and `VTMultiPassStorageSetDataAtTimeStamp`. There is no libm call: the
log10 complexity values arrive from the firmware already computed. [C]

| function (UA) | summary |
|---|---|
| `enqueue_first_pass` 0xb9a08 | 2-deep "fixup FIFO" for the bits correction, then a min-heap that releases records in display order (§2.4.3) |
| bits correction 0xb97b4 | adds rec+0x48 of the record that arrived **two** records later to rec+0x40, scales rec+0x44 to match (§2.4.3) |
| `scene_change_pipeline` 0xb9130 (`histogram_diff` 0xb8270, `scene_change_detect` 0xb8398) | earth mover's distance between consecutive LRME histograms, a 4-record window, a fixed decision tree; writes rec+0x4B0/0x4B8/0x4BC; holds each scene's first record back until the scene is complete (§2.4.4) |
| `accumulate_scene_info` 0xb84e8 | sequence totals into the header; folds each non-start record into its scene's first record (§2.4.5) |
| `FinalizeSeqRcInfo` 0xb9ff4 | reduces the 16-bin log10-complexity histogram to 4 counts and 4 values (§2.4.6) |

#### 2.4.1 Where it runs

| step | code | label |
|---|---|---|
| object | the frame receiver (0x45040 bytes, `operator new` at UA 0x9bd60, not zeroed) holds the MP object at **+8** (UA 0x87ef4); constructor UA 0x9c140, reset UA 0x9c1c8, which zeroes the header (UA 0x9c200) | C |
| per frame, pass 1 | `SendFrame` writes the frame's PTS over rec+0x04..0x1B, then calls UA 0x94f6c: take a free record from the pool (UA 0x94fcc), memcpy the firmware's 0x626 bytes into it, `enqueue_first_pass(MP, rec, flush = 0)`. That returns the record leaving the pipeline, or NULL. SendFrame copies it to FrameInfo+0x63A6 (UA 0x970e8) and stores it with `VTMultiPassStorageSetDataAtTimeStamp`, keyed by **the emitted record's own PTS** (the key is FrameInfo+0x63AA = record+0x04, UA 0x983c0-0x98408). Only when FrameInfo+0x0C == 0 (UA 0x970bc, 0x97e70) | C |
| end of pass | `DataType_RESETMULTIPASS` (UA 0x8b1d8) calls `FlushStats(MP, storage)` (UA 0xba86c). It feeds padding records with display order −1 until one of them comes out, storing every real record that comes out; then it resets the pool and queues (UA 0x9c220) and runs `FinalizeSeqRcInfo` | C |
| pass 2 | the header is MP+0x6398 (= FrameReceiver+0x63A0, copier UA 0x899d4), 0x108 bytes, copied in front of the records (UA 0xa6178-0xa61a8, 0xa6ae8-0xa6b08) | C |

All 13.5 callers pass flush = 0 (UA 0x94fa8, 0x95164, 0xba968); the flush = 1
arm of `enqueue_first_pass` is dead code here. [C] The debug dump
`DBUG_DumpMultiPassStats` (UA 0x950e0) writes each emitted record at file
offset 0x108 + display_order × 0x626 (UA 0x95174-0x9519c): the same
`header + rec[n]` layout as the table of §6.3. [C]

#### 2.4.2 The MP object (offsets from MP = FrameReceiver+8)

| MP+ | what | label |
|---|---|---|
| 0x0002 | 16 record slots, 0x626 apart | C (UA 0x9c27c-0x9c2b0) |
| 0x6268 / 0x62E8 | free pool: 16 pointers used as a stack (LIFO), and its count (16 after reset) | C |
| 0x62F0 / 0x6300 / 0x6304 | the 2-entry fixup FIFO, its length, the index of its older entry | C |
| 0x6308 | `std::vector<stats*>`: binary min-heap on display order, compared **unsigned** (UA 0xbb428, 0xbb48c) | C |
| 0x6328 | `std::deque<stats*>` D1: the scene-detection window | C |
| 0x6358 | `std::deque<stats*>` D2: scene starts held back | C |
| 0x6388 | u32: the next display order the heap may release | C |
| 0x6390 | the open scene's first record | C |
| 0x6398 | **the 0x108-byte sequence header** (§2.4.6), up to MP+0x64A0 | C |
| 0x64A0 | double: running sum of rec+0x614 (for avg_qscale) | C |
| 0x64A8 | u32 written in pass 2 (UA 0x899ec), unused here | C / U |

#### 2.4.3 `enqueue_first_pass` and the bits correction

```
FIFO len 0: fifo[idx] = rec; len = 1;   return NULL
FIFO len 1: fifo[!idx] = rec; len = 2;  return NULL
FIFO len 2: bits_correction(fifo[idx], rec.correction@0x48)
            heap.push(fifo[idx]); fifo[idx] = rec; idx = !idx
then        top = heap.top()
            if top.display_order != -1 and != next_display_order: return NULL
            next_display_order++; heap.pop()
            out = scene_change_pipeline(top)
            if out: back into the free pool (the caller copies it at once)
            return out
```

So rec+0x48 corrects the record that arrived **two** records earlier, not
the previous one. [C] The heap restores display order; in IPPP it never
holds more than one record.

`bits_correction(r, c)` (UA 0xb97b4), integer only [C]:

```
if c == 0 or (s32)(r.bits@0x40 + c) < 1: return
hc = (s64)((u64)r.hdr_bits@0x44 * (s64)c) / (s64)(s32)r.bits     (sdiv; 0 if bits == 0)
r+0x4CC (u64) += c                 r+0x4DC (u64) += (s32)hc
r+0x40 = bits + c                  r+0x44 = hdr_bits + (s32)hc    (u32)
class@0x34 == 0: r+0x4EC += hc     class == 2: r+0x4E4 += hc
r+0x524, 0x52C, 0x534, 0x53C (u64): += c, each only if non-zero
```

#### 2.4.4 `scene_change_pipeline` (UA 0xb9130)

D1 holds the last four records, in display order. For each record `rec`:

1. If no scene is open (the very first record): rec+0x4B0 = 1; it opens the
   scene and goes onto D2.
2. Push rec onto D1. If D1 has one entry: rec+0x4B8 = rec+0x4BC = 0,
   return NULL.
3. **Distance to the previous record** `prev` (UA 0xb9280-0xb92b4), float32:
   `hdiff = max(histogram_diff(rec, prev) / (max(rec+0x4C0 + prev+0x4C0, 1) / 512), 0.01)`.
   `histogram_diff` (UA 0xb8270) is the earth mover's distance between
   the 256-bin LRME histograms at rec+0xB0, in double:
   Σ_i |Σ_{j≤i} (a_j − b_j)| / Σ_j a_j, in bins, returned as float. A
   padding record takes prev+0x4B8 instead.
   rec+0x4B8 = hdiff; rec+0x4BC = max(hdiff, prev+0x4B8).
4. With two entries the first record gets the second's 0x4B8/0x4BC and is
   accumulated (§2.4.5). With three: return NULL.
5. With four, the **candidate** is D1[1] (two records before rec):
   `m0` = D1[0]+0x4BC, `m1` = cand+0x4B8, `m2` = rec+0x4BC,
   `ratio_p = m1/m0`, `ratio_n = m2/m1`, `m0m2 = ratio_n/ratio_p` (float32).
   If rec is padding, or the candidate's display order is below 3
   (unsigned): cand+0x4B0 = rec+0x50 bit 0 (logged as `forceKeyFrame`),
   and 1 for a padding candidate. Otherwise cand+0x4B0 =
   `scene_change_detect()` or forceKeyFrame. The candidate is then
   accumulated (§2.4.5); if it starts a scene it goes onto D2 and becomes
   the open scene.
6. Pop D1's front. If it does not start a scene, return it. If it does,
   return D2's front instead, unless that is the open scene (then NULL).
   So **a scene's first record leaves only after the next scene has
   started**, carrying the sums of the whole scene.

`scene_change_detect` (UA 0xb8398), compares in double against the
constants at UA 0x119f30-0x119f68 [C]:

```
if m0m2 > 0.00272072:
    if m1 > 71.58768845: cut = ratio_p > 4.51769352  and m0m2 <= 0.03005953
    else:                cut = ratio_p > 23.24848175 and m1 > 26.7539587
else:                    cut = ratio_n <= 0.96605313 and ratio_p > 1.34009841
```

A hard cut is a single spike in the distance at the candidate: ratio_p is
large, ratio_n small, and the third arm fires. Consequences: frame 0
always opens a scene; cuts at display order 1 and 2 are never detected
(only forced); cuts in the last two frames are never detected, since the
record after them is padding; a cut one or two frames after another one is
not detected, since m0 still holds the first spike (emulated for one frame:
cuts at 500 and 501 give one scene start, at 500). [C, emulated]

#### 2.4.5 `accumulate_scene_info` (UA 0xb84e8)

Once per real record (padding returns at once); H = the header:

```
H.cnt_All++; if rec+0x4B0: H.total_scenes++
H.bits_All += rec.bits@0x40
if rec.class@0x34 == 2: H+0x10++, H+0x14 += bits, H+0x50 += (double)rec+0x614
MP+0x64A0 += (double)rec+0x614;  H.avg_qscale = (float)(MP+0x64A0 / cnt_All)
H+0x58 += (double)rec+0x618;  H+0x60 += (double)rec+0x61C
H+0x68[i] += rec+0x574[i] (u32);  H+0xA8[i] += rec+0x5B4[i] (float),  i < 16
switch (u16)rec+0x624: 0 NORMAL H+0x1C/0x20, 1 MIN H+0x28/0x2C,
                       2 MAX H+0x34/0x38, 3 BLANK H+0x40/0x44   (count++, bits +=)
```

If rec+0x4B0 == 0, rec is also folded into the open scene's first record
S: u32 += at 0x4C4 (frames) and 0x4C8; u64 += at 0x4CC, 0x4D4, 0x4DC,
0x4E4, 0x4EC, 0x50C, 0x524, 0x52C, 0x534, 0x53C; double += at 0x4F4, 0x4FC,
0x544, 0x54C, 0x554, 0x55C; S+0x564/0x56C += (double)rec+0x618/0x61C (the
per-frame floats, not rec+0x564); u32[4] += at 0x514; S+0x504 = min,
S+0x508 = max; u32[16] at 0x574 and float[16] at 0x5B4 +=; and
`S+0x4C0 = fma(S+0x4C0, old_count, rec+0x4C0 × rec_count) / new_count`,
a frame-weighted mean with one fused multiply-add. [C]

The pass-2 debug print (UA 0xa6b0c-0xa6c30) names part of that block:
"cnt" 0x4C4, "bits" 0x4CC, 0x4DC, 0x4E4, 0x4EC, "QScale" 0x4F4, 0x4FC
(double), 0x504, 0x508 (float). With §2.4.3 (0x4DC follows hdr_bits;
0x4E4/0x4EC follow it for class 2/0) these read as: total bits, header
bits, class-2 and class-0 header bits; qscale sum, a second qscale sum
(squares?), min, max. [C] for the names, [I] for the reading.

#### 2.4.6 `FinalizeSeqRcInfo` (UA 0xb9ff4) and the header

The 16-bin histogram (H+0x68 counts, H+0xA8 sums) covers log10 complexity
in bins of 0.1875: bin *j* = [0.1875 *j*, 0.1875 (*j*+1)), the binning the
firmware uses too (fw 0x2de9c-0x2deb4). Finalize reduces it to four values:

1. One entry {count, mean = sum/count, lo, hi} per non-empty bin; if there
   is none, {1, 1.5, 0, 3}.
2. While there are fewer than 4 entries: sort by count, descending (libc++
   `std::sort`, UA 0xbb550), and split the first entry at its mean into
   {c − c/2, (mean+hi)/2, mean, hi} and {c/2, (lo+mean)/2, lo, mean}.
3. Sort by mean (UA 0xbc194). Place four centroids evenly over
   [first.lo, last.hi], each {0, centre, lo, hi}.
4. Three rounds of `QuantizeData` (UA 0xba6a0): every centroid takes from
   every bin the overlapping part of its count (count × overlap / bin
   width) at the overlap's midpoint; count = (u32)(carry + Σw + 0.5),
   mean = Σ(w × mid) / Σw, carry = Σw − count. Then each boundary moves to
   the midpoint between neighbouring means.
5. H+0xE8 = the four counts, H+0xF8 = the four means (logged as "log10_cplx
   quantized histogram : values … counts …").

The **header** (MP+0x6398, 0x108 bytes). Names are from the print at UA
0xb88c0 where it prints them [C]; every field is packed, no gaps:

| H+ | type | field | label |
|---|---|---|---|
| 0x00 | u32 | total_scenes | C |
| 0x04 | u32 | cnt_All | C |
| 0x08 | u64 | bits_All (Σ rec+0x40 after the correction) | C |
| 0x10 | u32 | frames with rec+0x34 == 2 (not printed; intra frames [I]) | C / I |
| 0x14 | u64 | their bits | C |
| 0x1C / 0x20 | u32 / u64 | cnt_NORMAL / bits_NORMAL (rec+0x624 == 0) | C |
| 0x28 / 0x2C | u32 / u64 | cnt_MIN / bits_MIN (== 1) | C |
| 0x34 / 0x38 | u32 / u64 | cnt_MAX / bits_MAX (== 2) | C |
| 0x40 / 0x44 | u32 / u64 | cnt_BLANK / bits_BLANK (== 3) | C |
| 0x4C | f32 | avg_qscale = Σ rec+0x614 / cnt_All | C |
| 0x50 | f64 | Σ rec+0x614 over the rec+0x34 == 2 frames (not printed) | C |
| 0x58 | f64 | current_complexity = Σ rec+0x618 | C |
| 0x60 | f64 | totalcplxsum = Σ rec+0x61C | C |
| 0x68 | u32[16] | log10-complexity histogram counts (Σ rec+0x574) | C |
| 0xA8 | f32[16] | histogram sums (Σ rec+0x5B4) | C |
| 0xE8 | u32[4] | quantised counts | C |
| 0xF8 | f32[4] | quantised values | C |

What the final pass does with each field is [U]; the firmware copies the
header to `CMultiPassControl`+0x39C on frame 0 (§2.5) and has its own
`InitCplxHistQuant`/`QuantizeCplxHist`.

#### 2.4.7 Quirks a faithful tool keeps

- **Stale padding records.** FlushStats takes a slot from the pool and sets
  only display order = −1. That slot is the one last returned, so it still
  holds an earlier record, and its rec+0x48 corrects the last two real
  frames. With fewer than 7 frames the padding comes from never-used slots,
  whose contents depend on whether `operator new` returned zeroed memory;
  the port and the emulation assume zeros. [C; U for N < 7]
- A missing or repeated display order stalls the heap for good, and
  FlushStats would then loop until the pool is empty. `ave2pass.py`
  refuses such input.
- Arithmetic: float32 where the code uses s registers, float64 where it
  uses d registers, one fused multiply-add, the AArch64 NaN rules
  (FPCR.DN = 0 assumed for macOS [I]). The port implements them explicitly,
  so its output does not depend on the host CPU.

#### 2.4.8 Verification

`tools/ave2pass/selftest.py`: 50 invariant checks (a 40-frame clip with a
hard cut at 12 gives rec+0x4B0 = 1 at 0 and 12 only, rec+0x4C4 = 12 and 28,
total_scenes 2; forced key frames; no detected cut below display order 3;
pass-2 buffers for 1..40 frames), then the port against the emulation on
synthetic clips of 1-300 frames (cuts, varied frame bits, IDR periods) and
on random records with NaNs, infinities and subnormals in every float field
the code reads: 0 differences in 600+ record sets. A 3000-frame clip
builds in about 0.6 s with either backend. [C, emulated]

Still [U]: how the firmware fills the fields this code reads (rec+0x4C0,
the class values at rec+0x34 and rec+0x624, rec+0x50, rec+0x4C8 and the
other scene-block seeds), and whether any of the stale-slot effects matter
to the final pass. The T1 records (§7) answer the first; `ave2pass.py dump`
prints them.

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
| 0 | 0x44AA = 0x108 + 11×0x626 | header; records 0..10 (if the clip is shorter, macOS repeats the last record, UA 0x39fb0) |
| N ≥ 1 | 0x72E = 0x108 + 0x626 | header (re-sent; read only on frame 0); record N+10 (macOS repeats the last one at the end) |

There is **no padding**: 11 × 0x626 = 0x43A2, and the IOSurface is
0x43A2 or 0x626 bytes plus the header (UA 0xa5ff4-0xa601c: `frame == 0 ?
0x43A2 : 0x626`), filled by `memcpy(surface, header, 0x108)` and
`memcpy(surface + 0x108, records, size)` (UA 0xa618c-0xa61a8, 0xa6aec-0xa6b08).
(An earlier version of this table read 0x44AA as 0x108 + 11×0x626 + 0x20.) [C]

Why N+10: `AVE_H264MultipassDataFetch` (UA 0x39a98) on frame 0 copies the
record at the frame's own PTS, checks its display order against the frame
number (UA 0x39ee4-0x39ef0, "bytePtr->pic_info.display_order ==
encoderPrivateStorage->frameNumber"), then steps
`kVTMultiPassStorageStep_GetNextTimeStamp` ten times, copying each record;
when the next time stamp is not valid (flags & 0x1D != 1) it copies the
previous slot again (UA 0x39fb0). So the cursor stands at frame 10. Each
later frame steps once more and copies that record; at the end
(GetNextTimeStamp not valid) it steps `GetPreviousTimeStamp` from there
(UA 0x39d08-0x39d2c, 0x3a14c-0x3a184), which with VideoToolbox's ordering of
an invalid time after every valid one gives the last record again [I]. The
firmware queue pops one record and pushes one, keeping 11. [C] for both
halves; [I] that they pair as stated. `tools/ave2pass/ave2pass.py frames`
builds these buffers.

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
| 0x34 | frame class: the host counts class 2 separately (header +0x10/0x14/0x50) and the bits correction books class 0 and 2 header bits apart; matches the H.264 slice-type numbering (0 P, 2 I) [I] | UA 0xb85d8, 0xb9980; `FrameType` counts it, 0x389fc | C read, meaning I |
| 0x40 / 0x44 / 0x48 | frame bits / header bits (u32) / signed correction, added to the record that arrived **two** records earlier | UA 0xb97b4, 0xb9b7c (§2.4.3) | C |
| 0x50 | bit 0: forced key frame, as the host logs it; forces a scene start | UA 0xb94a4, 0xb952c | C |
| 0x54.. | MCPU statistics | fw 0x2d4a0 | C |
| 0xA0, 0xB0-0x4AF | LRME data, 256 × u32 histogram (the scene detector's input) | fw 0x2cf30; UA 0xb8270 | C |
| 0x4B0 | scene start (host writes it for every record) | UA 0xb921c, 0xb94ac-0xb94ec; fw 0x38418 | C |
| 0x4B4 | scene's first frame [I]; the host never touches it | fw 0x45de4 compares it with the current frame | I |
| 0x4B8 / 0x4BC | histogram distance to the previous frame / max of it and the previous frame's (host, float) | UA 0xb93a8-0xb93ac (§2.4.4) | C |
| 0x4C0 | float that scales the distance (÷ (Σ of two frames)/512); a scene's first record ends up with the frame-weighted mean | UA 0xb9280, 0xb87bc-0xb87d0 | C use, meaning U |
| 0x4C4 | frames in the scene (host accumulates). `UpdateNextScene` sets next scene = current + this | UA 0xb8630-0xb8774; fw 0x38434 | C |
| 0x4C8-0x5F3 | scene block, summed over the scene into its first record (§2.4.5): bits 0x4CC, header bits 0x4DC, class-2/0 header bits 0x4E4/0x4EC, qscale sums 0x4F4/0x4FC, min/max 0x504/0x508, complexity 0x564/0x56C, 16-bin log10-complexity counts 0x574 and sums 0x5B4 | UA 0xb8744-0xb8884, 0xa6b58 | C (names I) |
| 0x600-0x613 | firmware per-frame values | fw 0x2df70.. | C |
| 0x614 | qscale (float; avg_qscale is its mean) | UA 0xb85fc, 0xb8628 | C |
| 0x618 / 0x61C | complexity floats (header current_complexity / totalcplxsum) | UA 0xb8650 | C |
| 0x624 | u16 class 0..3 (NORMAL/MIN/MAX/BLANK); other values are not counted | UA 0xb869c | C |

### 2.7 IDR period in the final pass

*Hardware (M2, M1 Pro): a final pass of a 90-frame 30 fps single-scene
clip, sent with ui32IdrPeriod (wire 0xFF34) = 180, put IDRs at 0, 30, 60.*

The final pass's longest IDR interval is **not** ui32IdrPeriod alone:

```
max_interval = min(IdrPeriod,                                   wire 0xFF34 (RC+0x04)
                   Duration != 0 ? (u32)(Duration * FrameRate)  wire 0xFF38 (RC+0x08, double, seconds)
                                 : FrameRate)                   wire 0xFF4C (RC+0x1C, u32 Hz)
```

With wire 0xFF38 = 0, as the driver sends it, the final pass puts an IDR at
least every *FrameRate* frames: one per second. [C, emulated]

Evidence (H14G; H13S is the same code at the addresses in brackets):

- `CAVCController::InitEncodingParameters` builds `sCFrameTypeInitParams`
  from x23 = VP+0xFED0 (the RC block, wire 0xFF30; fw 0x4e70c-0x4e710
  [h13s 0x4cd0c-0x4cd20]) and calls `CFrameType::init` (fw 0x4f43c-0x4f498
  [0x4da18-0x4da74]); init copies the 0x38 bytes to ft+0x78 (fw 0x36f10).
  So ft+0x78 (double) = RC+0x08, ft+0x80 = ft+0x84 = RC+0x04 (IdrPeriod),
  ft+0x98 = RC+0x1C (frame rate; the pair RC+0x18/0x1C is swapped by
  `rev64`). [C]
- `CFrameType::FrameType(…MPQueue…)` sets ft+1 = (ft+0x78 != 0.0)
  (fw 0x3883c-0x3885c). [C]
- `CFrameType::UpdateNextScene` (fw 0x383c4 [h13s 0x37008]):
  `w8 = ft+0x98; if ft+1: w8 = fcvtzu(ft+0x78 × ucvtf(w8)); w8 = min(ft+0x80, w8)`
  (fw 0x383c8-0x383ec). The next IDR is then min(next scene start,
  current + w8) (fw 0x38448-0x38460). If the next scene starts less than 10
  frames beyond that limit and the limit is at least 11, the IDR goes to
  current + ((distance / 2) & ~3) + 1 instead, splitting the scene
  (fw 0x38464-0x384a4; that is the IDR at 29 in §3.1). [C]
- Emulation (`ftemu.py <snap>/S3 200 bframes=0 idr=… dur=… fps=…`, snapshot
  frame rate 30) [C, emulated]:

  | IdrPeriod | wire 0xFF38 | wire 0xFF4C | scene marks | IDRs (200 frames) |
  |---|---|---|---|---|
  | 180 | 0 | 30 | none | 0 30 60 90 120 150 180 (the hardware result) |
  | 180 | 0 | 60 | none | 0 60 120 180 |
  | 180 | 1.5 | 30 | none | 0 45 90 135 180 |
  | 180 | 2.0 | 30 | none | 0 60 120 180 |
  | 180 | 1000.0 | 30 | none | 0 180 |
  | 1000 | 1000.0 | 30 | none | 0 |
  | 1000 | 1000.0 | 30 | cut at 120 | 0 120 |

**macOS user space.** The RC block is S+0xB0 (docs/72 §1.3).
`AVE_SetEncoderDefault` writes RC+0x04 = 30 (UA 0x29b5c-0x29b60) and leaves
RC+0x08 = 0 (bzero at creation). `kVTCompressionPropertyKey_MaxKeyFrameInterval`
writes RC+0x04 = value, 0 meaning 30 (UA 0x10e44-0x10e54);
`kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration` writes the double
to RC+0x08 and to the driver block DRV+0x00 (UA 0x12978-0x12980). So a macOS
2-pass encode with default properties also gets an IDR at least every
min(30, frame rate) frames; a client that wants long GOPs sets both keys,
and the final pass uses min(MaxKeyFrameInterval, Duration × ExpectedFrameRate,
truncated). [C]

**Driver.** To get IDRs only at frame 0 and at marked scene cuts, send
IdrPeriod ≥ the clip length and wire 0xFF38 ≥ IdrPeriod / FrameRate seconds.
A constant does it, in the multi-pass block of the Start builder:

```c
wr64(w, 0xff38, 0x4130000000000000ULL); /* RC+0x08 MaxKeyFrameIntervalDuration = 2^20 s (double): final-pass key interval = IdrPeriod (docs/95 §2.7) */
```

(2^20 × frame rate saturates at 0xFFFFFFFF in `fcvtzu`, so it never limits.)
Send it in pass 2 only: the single-pass `FrameType(…AdaptiveB…)` also reads
ft+0x78 (fw 0x37118-0x37228, a time-stamp based key interval), which
explicit frame types bypass but type-5 single-pass sessions would not. [I]
The HEVC path builds the same init block (`CHEVCController::InitEncodingParameters`
calls `CFrameType::init` at fw 0x6dfb0); its source offsets were not
checked. [U]

---

## 3. Emulation (tools/fwemu style, M2 snapshot S3)

Every run started from the docs/93 snapshot (a one-reference P session,
fixed QP), with patches applied in Unicorn memory only.

### 3.1 `CFrameType::FrameType(MPQueue)` alone (`ftemu.py`)

I put synthetic records in `ctrl`'s MPQueue (frames N..N+10) and called the
function once per display frame, with the session's own CFrameType object
(ctrl+0x1B0; IdrPeriod patched to 30; the snapshot's frame rate 30 and
duration 0 cap the interval at 30 anyway, §2.7):

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
#define AVE_MP_IN_FIRST  0x44AA   /* 0x108 + 11*0x626, no padding */
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
computes the table: `tools/ave2pass/ave2pass.py build` (§2.4), or the
application with the same code.
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
| T0 | none (control): current bitrate-mode self-test, N = 30, plus a dump of CodedHeader+0x22638..+0x22C5E per frame | frames encode as today; rec+0xB0..0x4AF (LRME histogram) non-zero, because that collector is unconditional; rec+0x2C..0x40 **zero or stale** | histogram zero too → the LRME collector did not run; recheck before T1. **Run: all zero (§11.1)** |
| T1 | wire 0xFEFC = 1, 0xFF00 = 1 (0xFF04..0xFF10 = −1) | all 30 frames complete; rec+0x2C = frame index, rec+0x1C/0x20 = 1280/720, rec+0x24 = the frame rate as a float, rec+0x40 ≈ 8 × coded bytes; optionally read back MCPU word 0x267408000 = bit 24 set (a register the firmware wrote, so safe, docs/93 §4) | hang or assert (watch `sSVEMap`), or rec+0x2C still zero → the gate is not what §2.3 says |
| T2 | T1 + 0xFF04 = 30, 0xFF08 = 0 (pass 9) | records as in T1; slice QP ≈ 30 in every frame (`h264_parse.py`) | QPs still move under RC → pass 9 not taken |
| T3 | second session: 0xFF00 = 2, every frame type 5, BFrames 0, PICMGMT+0x900 → buffer built from T1's records and a **zero** header | 30 frames complete, IDR then P (FrameTypeReturned 3, 1, …), no assert | assert/hang (0x900 not seen, or the header zero is fatal); QP pinned at min/max → the header is needed (go to T4) |
| T4 | T3 with the header and records from `tools/ave2pass/ave2pass.py build` on T1's records (§2.4), buffers from `ave2pass.py frames` | file size within a few % of target; QPs vary with content | size miss ≫ 1-pass RC |
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

- ~~Exact header layout and scene-detection thresholds~~: done, §2.4
  (`tools/ave2pass/`). ~~What the final pass does with each header
  field~~: mostly done, §12.3 (cnt_All sets the sequence budget and
  window; the scene blocks set the start estimate; the quantised
  complexity feeds class rates whose meaning is open). The firmware-side
  meaning of rec+0x4C0, rec+0x34 and rec+0x624. [U]
- Whether a zero or approximate header is safe (T3 answers it), and the
  meaning of Options bits and QPModLevel outside pass 9. [U]
- ~~Writer of rec+0x4B4~~: the firmware, with the frame's display
  number; ProcessPipeStart compares it to the frame (§12.6). The PTS at
  rec+0x04 and the frame rate at rec+0x24 do not change the final pass
  (§11.5, §12.1). [C, emulated]
- Whether macOS's frame receiver memory is zero when a clip shorter than 7
  frames uses never-touched pool slots for padding (§2.4.7). [U]
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
# hardware (§11, root, the driver loaded with V4L2): pass 1, table, pass 2
echo 1 > /sys/module/apple_ave/parameters/session_mp_pass   # then encode (bitrate mode)
cat /sys/kernel/debug/apple_ave_mp_rec > recs.bin
python3 tools/ave2pass/ave2pass.py build recs.bin $(( $(stat -c%s recs.bin) / 0x626 )) -o table.bin
cat table.bin > /sys/kernel/debug/apple_ave_mp_table
echo 2 > /sys/module/apple_ave/parameters/session_mp_pass   # encode again, same settings
# the bench: ENCODERS=ave264-cqp,ave264-vbr,ave264-2pass python3 bench/hevc-efficiency/bench.py run ...
A=tools/ave2pass/ave2pass.py                # §2.4: pass-1 records -> pass-2 table and buffers
.venv/bin/python $A synth 40 -o recs.bin --cuts 12
.venv/bin/python $A build recs.bin 40 -o table.bin --backend both   # port == Apple's code under Unicorn
.venv/bin/python $A dump table.bin; .venv/bin/python $A frames table.bin out/
.venv/bin/python tools/ave2pass/selftest.py
# §12: the final pass's rate control under emulation (snapshot S3 of docs/93)
.venv/bin/python $A build recs.bin 200 -o tuned.tab --rc-scene 1 --scene-qscale bits
.venv/bin/python $S/fpemu.py <snap>/S3 table.bin --sizes final.sizes --hwqp final.qp      # replay
.venv/bin/python $S/fpemu.py <snap>/S3 tuned.tab --model p1.qp:p1.sizes final.qp:final.sizes vbr.qp:vbr.sizes
tools/h264_mbqp final.h264 > final.qp       # per-frame macroblock QPs of a stream
```

## 11. Hardware results (2026-10-04)

M2 (t8112, H14G) unless noted; the M1 Pro (t6000, H13S) repeated T1, T3
and T5 with **byte-identical** records and streams. Driver: the lab path of
§6.3 (`session_mp_pass`, `session_mp_const_qp`, `session_mp_qpmod`,
`session_mp_rec`; debugfs `apple_ave_mp_rec` and `apple_ave_mp_table`),
H.264 only. Pass 1 and pass 2 are separate V4L2 sessions (no RESET); the
self-test covers T0-T2.

### 11.1 The tests

| # | what | result |
|---|---|---|
| T0 | bitrate-mode self-test, 30 frames, records kept, no multi-pass | encodes as before; **every record is zero**, LRME histogram included: the coded header is written only in its first 0x190 bytes. §2.3's "unconditional" LRME collector is gated after all |
| T1 | wire 0xFEFC = 1, 0xFF00 = 1 | all 30 frames; every record filled: rec+0x2C = 0..29, rec+0x1C/0x20 = 1280/720, rec+0x24 = 30.0f, **rec+0x40 = 8 × the coded bytes exactly** on every frame, rec+0x34 = 2 on the IDR and 0 on P, the LRME histogram sums to 57600 (1280 × 720 / 16, one count per 4x4 low-res pixel) |
| T2 | T1 + ConstantQP 30, QPModLevel 0 (pass 9) | slice QP 30 on every frame (T1's moved 21 → 16 under the rate controller): pass 9 is taken |
| T3 | final pass (0xFF00 = 2, type 5, PICMGMT+0x900) with T1's records and a zero header | all frames, no assert, no hang. Two driver fixes found here: type-5 frames must take FrameTypeReturned for the parameter sets (the stream had no SPS/PPS), and IdrPeriod 1, which the V4L2 path sent, makes **every** frame an I frame |
| T4 | the table from `tools/ave2pass` (§2.4) | 720p `testsrc2`, 2 and 4 Mbit/s: 493 KB for a 500 KB target, 1041 KB for 1000 KB; 90 frames at 2 Mbit/s: 730 KB for 750 KB. IPPP |
| T5 | scene cuts marked by hand at 12 and 20 | IDRs at exactly 0, 12, 20 |
| T6 | the RESET command | not run: a new session per pass works |
| T7 | bench | §11.4 |

### 11.2 Two things the driver must send

- **IdrPeriod (wire 0xFF34) > the clip.** CFrameType reads 1 as "every
  frame a key frame". The driver sends twice the table length in the final
  pass.
- **MaxKeyFrameIntervalDuration (wire 0xFF38, f64 s) non-zero.** At 0 (the
  default, also macOS's) the final pass caps the key interval at the frame
  rate: an IDR every second whatever IdrPeriod says (§2.7; found on
  hardware as IDRs at 0, 30, 60 of a 90-frame 30 fps stream, then traced).
  The driver sends 2^20 s in the final pass; one IDR per 90 frames after.

### 11.3 The bench

`bench/hevc-efficiency`, five Xiph 1080p50 clips, 200 frames, H.264 High
CABAC through V4L2 (`v4l2-ctl`, with `--set-output-parm=50`: without it
the rate controller assumes 30 fps and every bitrate lands 1.67x high).
`ave264-vbr` = 1-pass VBR, `ave264-2pass` = pass 1 at the same target,
`ave2pass build`, pass 2 (`results-m2-h264-vbr.csv`,
`results-m2-2pass.csv`; fixed QP in `results-m2-h264.csv`).

![2-pass vs 1-pass](img/multipass.svg)

### 11.4 Result

| | crowd | park | ducks | tree | town | mean |
|---|---|---|---|---|---|---|
| 2-pass vs 1-pass VBR, BD-rate at equal VMAF | +4.2 | +9.2 | −2.3 | +9.0 | +5.9 | **+5.2 %** |
| same, at equal PSNR-Y | +6.7 | +28.8 | +10.4 | +4.6 | +6.0 | **+11.3 %** |
| size miss, 1-pass VBR (mean of 4 rates) | 9 | 2 | 5 | 8 | 13 | 7.3 % (max 17.8) |
| size miss, 2-pass | 11 | 10 | 13 | 14 | 19 | 13.5 % (max 46.2) |

Positive = 2-pass needs more bits. Two passes also halve the throughput
(48 against 98 fps for the whole V4L2 loop).

### 11.5 Why: the final pass front-loads

Per 40 frames of `park_joy` at 8 Mbit/s: 1-pass VBR spends 0.96, 0.65,
0.73, 0.80, 0.81 MB; the final pass 1.48, 1.16, 0.28, 0.20, 0.32 MB.
`crowd_run` is the same shape; `in_to_tree` is not. What was ruled out:

- **The first pass's start-up.** Pass 1 does spend 4 MB on its first 40
  frames before it settles, but a constant-QP first pass (pass 9, QP 30
  or 36) leaves the final pass's shape almost unchanged (park_joy 1.46,
  0.99, 0.27, 0.27, 0.45 MB).
- **The PTS in the records.** Tables with 30 fps, 50 fps or the
  firmware's own PTS give byte-identical final passes.
- **The tool.** `ave2pass` matches Apple's own host code under emulation
  (§2.4.8); a hand-made single-scene table behaves the same.

Left at the time: how the firmware's final-pass controller
(`MpFinalPassAccumulate`, `finalPassSequenceLevel`, `finalPassSceneLevel`,
§2.2) spends the header's totals over the clip. §12 answers it: a clamp on
the first frame's QP and a slow buffer, not the header, the PTS or the
RESET path. Until §12.7's tests pass, the lab path stays a lab path:
nothing in the V4L2 interface exposes it, and AVE's best mode remains
fixed QP with B frames (docs/96).

## 12. Why the final pass front-loads

*2026-10-04, static analysis of H14G and H13S plus Unicorn emulation from
snapshot S3, fed with the hardware evidence of §11.4 (park_joy, crowd_run
and in_to_tree at 8 Mbit/s: the pass-1 records, the tables, the three
streams and their per-frame sizes). No hardware was touched.*

### 12.1 Result

| question | answer | label |
|---|---|---|
| What sets the final pass's QP? | One QP per frame, from one number: the controller's qscale (CMultiPassControl+0xC). It is looked up in a 99-entry {QP, mod level, qscale} table, and ProcessRateControl returns the result unchanged. Every macroblock of a frame has that QP. The slice header keeps 26 and the first macroblock's mb_qp_delta carries the rest. No stream shows per-MB modulation: not the final pass, not pass 1, not 1-pass VBR (§12.2). §11.5's "acts per macroblock" came from the slice header's 26 | C (code; every MB of 1800 hardware frames decoded) |
| Can the emulation be trusted? | **Yes.** Fed the hardware's per-frame sizes, the firmware's own code under emulation gives the hardware's frame QP on **all 600 frames** of the three clips (0 differences). With a per-frame bits model instead (closed loop), it reproduces the hardware's per-40-frame sizes within a few %. Independently, it also reproduces the pass-9 run of §11.5 (§12.5) | C (emulated) |
| Root cause | **The first frame's qscale is clamped to QP 36 or below** (`MpFinalPassSceneBitrates`, fw 0x307c4-0x308fc). The clamp applies only on the sequence-level call at frame 0. The firmware's own estimate for park_joy is QP 45-46 (qscale 47.9), the content needs about QP 42 at 8 Mbit/s (1-pass VBR settles there), and the clamp starts it at 36. After that, q moves only through a virtual buffer of 1.65 × the scene's duration (6.6 s and 52.8 Mbit for a 4-s clip). The buffer starts at the bottom of its q range, and going from QP 36 to 42 means absorbing about 31 Mbit, the whole clip's budget. What finally moves q is the sequence buffer's correction (2-s window, amplified up to 7× when over budget). It overshoots to QP 51 and pays back for the rest of the clip | C (code), emulated |
| Why in_to_tree differs | Its estimate (qscale 8.5, QP 33) lies between QP 30 and 36, and the clamp sets such a start to exactly QP 30 (5.80). That happens to be the level in_to_tree needs at 8 Mbit/s (its pass 1 settles at QP 30), so nothing has to be paid back | C (emulated) |
| Other suspects | None of these is involved. rec+0x24: the records say 30.0 fps, and a replay with 50.0 is identical. Wire 0xFF48 (1 or 50), the QP floor (10 or 0) and the PTS: identical. The header's cnt is 200, the number of frames coded. The budgets use the session's 50 fps. The RESET path re-runs the same `InitEncodingParameters` → `ProcessInit` → `Initialize` (§2.5), so it starts the controller in the same state [I]. A constant-QP first pass does not help, because the clamp sets the start, not the estimate (§12.5) | C (emulated) / I |
| H13S | The same code. `finalPassSequenceLevel`, `finalPassSceneLevel` and `MpFinalPassSceneBitrates` have identical instruction multisets once registers are renamed. `MpFinalPassAccumulate`, `Initialize`, `ProcessRateControl` and `ProcessAccumulate` differ only in instruction scheduling and store pairing. The qscale tables and constants are byte-identical (h13s 0xb1fd4 = h14g 0xb7314) | C |
| Host-side fix | Two table options, both host-side. `ave2pass build --rc-scene 1` adds a rate-control scene start at frame 1 with no IDR, so the start comes from the scene-level path, which has no clamp. `--scene-qscale bits` makes the scene qscale sums bits-weighted, so that path's estimate is right after a VBR first pass. Closed loop: park_joy 0.67 0.89 0.86 0.90 0.81 MB per 40 frames, +3 % size, QP 40-42 after frame 0; crowd_run −2 %, in_to_tree −0.3 % (§12.6). Hardware tests: §12.7 | C (emulated), I (bits model) |

![final pass, park_joy](img/finalpass.svg)

### 12.2 How the QP reaches the frame

`CRateControl::ProcessRateControl` (fw 0x3acbc) takes a multipass path
when RC+0x2979 (sCRCInitParams+177, the enable) is set and the pass is
non-zero (fw 0x3ad3c-0x3adc8). On the first frame it calls
`finalPassSequenceLevel` (fw 0x3b128). On later frames it calls
`finalPassSceneLevel` when the current scene's start
(CMultiPassControl+0x250) is at or before the frame (fw 0x3ae88-0x3ae9c).
Then it returns RC+0x394 as the frame's QP (fw 0x3b12c-0x3b2cc; RC+0x794
is 0 from ProcessInit). Nothing else adjusts the QP on this path. [C]

RC+0x394 is written only through `CRateControl::QpUpdateByMultipass`
(fw 0x3d888), which sets:

- RC+0x394 = the table entry's QP (+ RC+0x31C, which is 0 for AVC);
- RC+0x1164 = the entry's mod level;
- RC+0x3B4 = the qscale.

The entry is the first whose qscale is ≥ q, a lower bound (e.g. fw
0x2f1b4-0x2f568). The AVC table (codec ≠ 1) is at fw 0xb7314: 99 entries
{s16 QP, u16 mod, f32 qscale}, with QP 0-51, mod 4-7 and qscale 0.213-255.
Some entries: QP 36/6 = 12.91, 30/6 = 5.80, 42/6 = 30.82, 51/7 = 255. [C]

The mod level would feed QP modulation, which is off (wire 0xFF70 = 0).
On the hardware, every macroblock of every frame has the same QP
(`tools/h264_mbqp.c`, libavcodec's per-MB QP export, 9 streams × 200
frames). [C]

The controller updates q after each frame (`MpFinalPassAccumulate`,
called from `ProcessAccumulate` at fw 0x3d078). So frame n's QP comes from
frames < n, and on a scene-start frame from the scene-level call that
runs first. [C]

### 12.3 The controller

Notation:

- R = the session bitrate (RC+0x2B8); f = the frame rate (RC+0x538,
  float).
- M = the CMultiPassControl object (ctrl+0x418); H = the header (M+0x39C).
- The current scene block (rec+0x4B0..0x5FF of the scene's first record)
  sits at M+0x24C, so M+x there is rec+(x+0x264).
- N_s = rec+0x4C4 (frames in the scene), B_s = Σ bits (rec+0x4CC),
  Q_s = Σ qscale (rec+0x4F4).

**Four virtual buffers.** `Initialize` (fw 0x2c8f4) sets them up through
a helper (fw 0x2ccf8, "init(rate, f, window, q_lo, q_hi)") [C]:

- M+0x10, the *sequence* buffer;
- M+0x98, the *scene* buffer;
- M+0x120 and M+0x1A8, in macroblock units, not on the default path.

Each buffer has a size S = window × rate, a fullness F (16.16 bits)
started at S/2, a drain of rate/f per frame, and a q range [q_lo, q_hi].
q follows F as:

```
q(F) = q_lo·K / (K − F'),   K = q_hi·S / (q_hi − q_lo),   F' = clamp(F, 0, S)       fw 0x2f264-0x2f2c4, 0x313ac
       clamped to [0.2395, 255], then to the table's [0.213, 255]                   fw 0x2f2c8-0x2f2e0
```

So q runs from q_lo at an empty buffer to q_hi at a full one, along a
hyperbola that is flat near q_lo. Adding a frame (fw 0x2ecc8) books its
bits at once, or, in "list mode" (+0x48 = 1), spreads them: an I frame
over round(f) frames, a P frame over 4. [C]

`Initialize` also sets M+0xC = 1.889. That is the qscale of QP 21: the
first pass's start, the 21 of §11.5's pass-1 streams. From Options (wire
0xFF10, default 0x1305) it sets [C]:

- bit 3 → M+0x4F4/0x4F8 = {0.5, 2.0} or {0.9, 1.1};
- bits 9 and 11 → M+0x500 = 2, 4 or 5.

**Frame 0: `finalPassSequenceLevel`** (fw 0x2faac):

```
T = cnt_All·R/f,  D = cnt_All/f                                          fw 0x2fb9c-0x2fbc0
window0 = Options bit 12 ? min(D/2, 300) : 300                           fw 0x2fbc4-0x2fbec
R0 = Options bit 0 ? max(31/32·T, T − window0·R/2) / D : R               fw 0x2fbf8-0x2fcb8
sequence buffer: rate R0, size window0·R0, F0 = S/2, list mode, no clamps  fw 0x2fcbc-0x2fde0
divisor = (u32)(f·window0/2) + 1                                         fw 0x2fd40-0x2fd78
```

For the bench clips (200 frames at 50 fps) this gives D = 4 s,
window0 = 2 s, R0 = 7.75 Mbit/s (31/32 of the target), S = 15.5 Mbit and
divisor 51 (emulated). [C] It then calls
`MpFinalPassSceneBitrates(f, first = 1)`.

**Scene level: `MpFinalPassSceneBitrates`** (fw 0x3007c) runs at frame 0
and at every later scene start. At a later start, `finalPassSceneLevel`
(fw 0x2fee0) first subtracts the new scene's block from H
(`MpFinalPassUpdateSeqRcInfo`, fw 0x30a78), and skips the rest if no
frames remain.

```
class rates from H's quantised complexity counts (first call only)       fw 0x300b8-0x302ac   [C], meaning [U]
R_s = the scene's rate: class rates weighted by the scene's classes      fw 0x302b0-0x30460   [C]
T_s = R_s·N_s/f;   Hd = header-bit estimate from rec+0x4DC/0x4E4/0x4EC   fw 0x30464-0x304e8   [C], terms [I]
q_raw = (Q_s/N_s) · (B_s − Hd) / (T_s − Hd)                              fw 0x304f4-0x305c4   [C]
        for a scene that starts on an I frame (rec+0x5F4 = 2); otherwise B_s also gets
        the expected extra cost of the IDR the final pass will put there (fw 0x30508)
        clamped to the table's range, >= 0.2521
scene buffer (Options bit 8): rate R_s, window clamp(1.65·N_s/f, 0.5, 30) s,
        q range [q_raw/3.75, 4·q_raw]                                    fw 0x306f8-0x307bc   [C, emulated]
first call only:                                                         fw 0x307c4-0x308fc   [C, emulated]
        qA = qscale(first entry with QP·10+mod >= 300) = 5.80 (QP 30)
        qB = qscale(first entry with QP·10+mod >= 366) = 12.91 (QP 36)
        q0 = q_raw > qB ? qB : q_raw > qA ? qA : q_raw     (a q_raw > 4·qA branch is dead for AVC)
later calls: q0 = q_raw
M+0xC = q0;  scene F = K − q_lo·K/q0  (so that q(F) = q0)                fw 0x30900-0x3091c   [C]
```

This is a 2-pass estimator in the usual form, with bits ∝ 1/qscale: the
pass-1 mean qscale, scaled by pass-1 bits over target bits. Its weak spot
is the plain mean of qscale. After a first pass whose qscale moved a lot,
Σq/N × ΣB is not ΣqB, and the VBR first pass ramps from qscale 1.9 to 30
while spending most of its bits early. [I]

**Every frame: `MpFinalPassAccumulate`** (fw 0x2ef08). `ProcessAccumulate`
(fw 0x3d078) passes it the frame index, the display index, the class (0 P,
2 I), the frame's bits, and the CABAC-minus-estimate correction of two
frames back.

```
if display order == 0: finalPassSequenceLevel()                           fw 0x2f068, 0x2f304
if M+0x250 <= display order: finalPassSceneLevel()                        fw 0x2f06c-0x2f07c
sequence: add(bits); F0 += corr − R0/f                                     fw 0x2f080-0x2f0f0
c = ((F0 >> 16) − F0_start) / divisor                                     fw 0x2f0f4-0x2f108
Options bit 2: r = min(3·(F0 − F0_start)/S0, 2);  c ×= ((r+3)² + 3) / ((r−3)² + 3)   fw 0x2f10c-0x2f154
        (×1 at r = 0, ×7 at r = 2, ×0.37 at r = −1: overspending is corrected hard,
         underspending gently)
Options bit 8: scene: add(bits); F += corr − R_s/f; F += c               fw 0x2f1fc-0x2f27c
q = q(F) (scene buffer), clamped; M+0xC = q; QpUpdateByMultipass          fw 0x2f280-0x2f568
```

So the frame QP follows the scene buffer. The sequence buffer acts on it
only through c, an integral term with a 2-s horizon. [C]

### 12.4 park_joy, frame by frame

A replay feeds the hardware's per-frame sizes in and reads the QPs out;
they equal the hardware's:

| frame | QP | q | sequence F / S (Mbit) | scene F / S (Mbit) |
|---|---|---|---|---|
| 0 | 36 | 12.91 | 7.6 / 15.5 | 0.5 / 52.8 |
| 20 | 36 | 13.34 | 9.5 / 15.5 | 2.6 / 52.8 |
| 40 | 37 | 15.06 | 13.0 / 15.5 | 9.1 / 52.8 |
| 60 | 39 | 20.03 | 15.5 / 15.5 | 21.3 / 52.8 |
| 80 | 43 | 37.82 | 16.3 / 15.5 | 38.3 / 52.8 |
| 100 | 49 | 115.4 | 14.7 / 15.5 | 50.7 / 52.8 |
| 120 | 51 | 191.5 | 12.3 / 15.5 | 52.9 / 52.8 |
| 160 | 49 | 122.6 | 7.7 / 15.5 | 50.6 / 52.8 |

How the clip goes [C, emulated]:

1. The scene buffer's range is [12.76, 191.5] (q_raw = 47.9). q0 = 12.91
   sits at its floor, so the buffer starts nearly empty.
2. At 8 Mbit/s the content needs q ≈ 31 (QP 42). This hyperbola reaches
   31 only at F ≈ 0.59·S = 31 Mbit, the clip's whole budget.
3. Frames at QP 36 cost about 30 kB against a 20 kB budget, so the
   sequence buffer fills.
4. From frame ~40 its amplified correction drives the scene buffer up. q
   passes 31 near frame 75, but the sequence buffer is already over its
   size, and the correction keeps pushing.
5. The scene buffer fills (QP 51, 4 kB frames at 100-135) and then drains
   slowly. The clip ends 12.7 % under target.

crowd_run goes the same way with q_raw = 41.4.

### 12.5 Emulation (`tools/fwemu/mp/fpemu.py`)

The run starts from snapshot S3. `CRateControl::Init` gets the snapshot's
own sCRCInitParams copy (RC+0x1C0), patched to the bench session:

- 1920×1088, 8 Mbit/s, 50 fps, wire 0xFF48 = 1;
- IdrPeriod 400, MaxKeyFrameIntervalDuration 2^20 s, QP 10-51;
- multipass 1/2/−1/−1/6/0x1305.

Then, per frame:

1. `GetFrameType`, with type 5 and the real multipass buffers.
2. The two copies of `ProcessPipeStart`, done in Python: the header on
   frame 0, and one scene block per frame (fw 0x45d44-0x45e40).
3. `CRateControl::RateControl`.
4. `UpdateBits` for both engines, with the frame's bits.
5. `CRateControl::Accumulate`.

`_RTK_lock_lock`/`unlock` are stubbed, because their LSE `cas` faults in
Unicorn.

- **Replay**, with the hardware's sizes as bits: the frame QPs equal the
  hardware's on 200/200 frames of each clip. [C, emulated]
- **Closed loop**, with bits from a per-frame model
  log2(bits_f) = a_f − QP/k. The model is fitted on the three hardware
  streams of each clip (pass 1, final pass, 1-pass VBR). k = 4.3 for
  park_joy and 5.0 for crowd_run; in_to_tree's is fixed at 5, because its
  streams span only QP 29-33. The mean |QP error| is 0.1-0.3. The default
  table gives these MB per 40 frames:

  | clip | closed loop | hardware |
  |---|---|---|
  | park_joy | 1.41 1.24 0.34 0.20 0.31 | 1.48 1.16 0.28 0.20 0.32 |
  | crowd_run | 1.42 0.99 0.51 0.35 0.39 | 1.45 0.95 0.50 0.35 0.41 |
  | in_to_tree | 0.78 0.81 0.79 0.81 0.92 | 0.77 0.81 0.80 0.81 0.90 |

  A constant-QP first pass at 36, synthesised from the model, gives
  park_joy 1.38 1.07 0.31 0.28 0.46. §11.5's hardware pass-9 run was
  1.46 0.99 0.27 0.27 0.45. [C, emulated; the model is I]

### 12.6 Host-side changes, closed loop

The tables were built with `tools/ave2pass` and run through the
emulation with the bits model. All three options are off by default, and
none of them is what macOS does. Each cell gives MB per 40 frames, the
size against the 4 MB target, and the QP range after frame 0. There is
one IDR, at frame 0, unless noted.

| table | park_joy | crowd_run | in_to_tree |
|---|---|---|---|
| macOS (default) | 1.41 1.24 0.34 0.20 0.31, −12.7 %, 36-51 | 1.42 0.99 0.51 0.35 0.39, −8.5 %, 36-46 | 0.87 0.70 0.73 0.82 0.88, 0.0 %, 29-30 |
| `--scene-qscale bits` | 1.37 1.03 0.30 0.31 0.50, −12.1 % | 1.37 0.83 0.44 0.42 0.55, −9.6 % | unchanged |
| `--key 2` (IDR at 0 and 2) | 0.52 0.70 0.76 0.90 0.94, −4.3 %, 35-44 | 0.71 0.64 0.69 0.75 0.80, −10.4 % | 0.74 0.62 0.73 0.87 0.90, −3.3 % |
| `--key 2 --scene-qscale bits` | 0.71 0.84 0.86 0.90 0.81, +3.0 %, 35-42 | 0.93 0.71 0.72 0.78 0.77, −2.1 % | 0.83 0.68 0.73 0.85 0.88, −0.8 % |
| `--rc-scene 1` | 0.48 0.70 0.75 0.90 0.94, −5.6 %, 42-44 | 0.64 0.63 0.69 0.75 0.80, −12.2 % | 0.69 0.62 0.73 0.88 0.93, −3.6 % |
| **`--rc-scene 1 --scene-qscale bits`** | **0.67 0.89 0.86 0.90 0.81, +3.1 %, 40-42** | **0.88 0.75 0.75 0.76 0.77, −2.1 %, 40-41** | **0.77 0.70 0.78 0.86 0.88, −0.3 %, 29-31** |
| Options 0x1205 (bit 8 off) | 0.78 0.99 1.01 0.93 0.79, +12.4 %, 40-43 | 0.99 0.93 0.91 0.84 0.80, +11.9 % | 0.66 0.65 0.71 0.81 0.92, −6.2 % |

Longer clips were made from the 200 frames played forwards and
backwards, 1000 frames in one scene. The host detects a cut at each turn,
so there are IDRs at 398 and 796. Per 200 frames:

| clip | default | both options |
|---|---|---|
| park_joy | 7.36 2.48 2.43 2.68 3.64 MB (−7 %) | 4.25 3.57 4.15 3.58 4.16 (−1.5 %) |
| crowd_run | 6.12 3.66 2.32 2.84 3.39 | 4.15 3.83 3.84 3.88 3.90 |

The start transient is a fixed ~100 frames: long clips dilute it but do
not remove it. [C, emulated; I]

- **`--key N`** sets rec+0x50 bit 0 (forceKeyFrame) on the pass-1 record
  of frame N, so the host pipeline starts a scene there (§2.4.4). The
  scene-level call at N has no clamp, and it starts the scene buffer at
  q_raw with its range centred on it. Cost: a second IDR.
  [C, emulated]
- **`--rc-scene P`** gives record P the scene block that a cut at P would
  give it, and leaves the enclosing scene's rec+0x4C4 alone. P then gets
  a scene-level call but no IDR [C, emulated]:
  - CFrameType puts IDRs only where its scene chain lands: next scene =
    current + rec+0x4C4 (`UpdateNextScene`, fw 0x383c4-0x38450; the IDR
    at fw 0x38ba0). The chain does not land on P.
  - The firmware's scene ring takes every record with rec+0x4B0 ≠ 0
    (fw 0x1e5d8-0x1e648).
  - ProcessPipeStart copies P's block in on frame P (fw 0x45d94-0x45e40;
    rec+0x4B4 = P is the firmware's own frame number).

  The header is unchanged.
- **`--scene-qscale bits`** rewrites each scene's Q_s as N_s·Σ(b·q)/Σb,
  and H+0x4C the same way. The firmware reads Q_s for q_raw (M+0x290,
  fw 0x30464); otherwise Q_s and H+0x4C only feed
  `MpFinalPassUpdateSeqRcInfo`'s avg_qscale upkeep. With a constant-QP
  first pass the option changes nothing. After the VBR first pass it
  brings park_joy's q_raw from 47.9 to 29.3 (the bits-weighted mean is
  0.61 of the plain one; crowd_run 0.60, in_to_tree 0.82). Alone it does
  little, because the clamp, not q_raw, sets the start. [C, emulated]
- **Options bit 8 off** starts the scene buffer at max(clamp, q_raw/2),
  with a 30-s window and no sequence correction. The QP is flat, but the
  size follows q_raw's error (+12 %). It is not a fix. [C, emulated]

A constant-QP first pass with `--rc-scene 1` does as well as the VBR
first pass with both options (park_joy 0.65 0.84 0.86 0.90 0.83,
+2.3 %), so the first pass can stay as it is. [emulated, I]

### 12.7 Hardware tests (lead only; one variable per run, in this order)

Each test is the §11.3 bench setup with the table built differently. Only
`MP_BUILD` changes; the driver does not. Two checks go with every run:

- Afterwards, check with `tools/h264_mbqp` that the final pass's frame
  QPs are the ones §12.6 predicts.
- Run the replay (`fpemu.py SNAP TABLE --sizes S --hwqp Q`). It must
  report 0 differing frames. That shows the edited table reached the
  firmware as intended.

| # | change | expect (park_joy 8 Mbit/s) | a "no" looks like |
|---|---|---|---|
| T8 | `--rc-scene 1` | IDR at 0 only. Frame 0 at QP 36, frames 1-17 at 44, then 43 and 42; 0.48 0.70 0.75 0.90 0.94 MB per 40 frames, −5.6 %. crowd_run: QP 43 from frame 1 | QP 36 held for ~35 frames as before: the ring copy or the scene-level call did not happen at frame 1. An IDR at 1: CFrameType chains on rec+0x4B0 after all (then run T10) |
| T9 | `--rc-scene 1 --scene-qscale bits` | Frame 1 at QP 41, then 40-42; 0.67 0.89 0.86 0.90 0.81 MB, +3 %. crowd_run QP 40-41, −2 %; in_to_tree −0.3 % | as T8, with QP 44 at frame 1: Q_s did not reach the firmware |
| T10 | `--key 2 --scene-qscale bits` (only if T8 fails) | IDRs at 0 and 2; frame 1 at QP 35, then 41; 0.71 0.84 0.86 0.90 0.81 MB, +3 % | |
| T11 | the bench, five clips, four rates, with T9's table | BD-rate against 1-pass VBR near or below 0, instead of +5 % VMAF / +11 % PSNR-Y; a size miss near 3 % | still positive: then quality, not allocation, separates the passes |
| T12 | `--scene-qscale bits` alone (optional) | park_joy 1.37 1.03 0.30 0.31 0.50, little change: the clamp is the cause | |

Commands (T9 shown):

```sh
# on the target, root, the driver loaded with V4L2:
RESULTS=bench/hevc-efficiency/results-m2-2pass-t9.csv \
MP_BUILD="python3 tools/ave2pass/ave2pass.py build --rc-scene 1 --scene-qscale bits" \
ENCODERS=ave264-2pass python3 bench/hevc-efficiency/bench.py run park_joy crowd_run in_to_tree
# on the host: the frame QPs of one output (build h264_mbqp as its header says),
# then the replay, which must report "0 of 200 frame QPs differ"
tools/h264_mbqp OUT.h264 > OUT.qp
.venv/bin/python tools/fwemu/mp/fpemu.py <snap>/S3 OUT.h264.tab --sizes OUT.sizes --hwqp OUT.qp
```

### 12.8 What is still open

- The class rates (M+0x514..0x51C), and what rec+0x624's
  NORMAL/MIN/MAX classes mean. The header's quantised complexity feeds
  only the class rates. [U]
- The two macroblock-unit buffers (M+0x120, M+0x1A8), and the mode switch
  on M+0x230 that the Options bit-8-off path uses (fw 0x2f31c-0x2f474).
  [U]
- Whether macOS clients see the same start. The clamp is in the
  firmware, and macOS's table equals ours, so a macOS 2-pass of a short
  single-scene clip should front-load the same way. In content with cuts,
  each later scene restarts unclamped. [I]

### 12.9 Hardware results (2026-10-04, M2)

T8-T12 of §12.7, H.264 at 8 Mbit/s, then the full bench. Every run was
checked against the emulation: the per-frame QPs of the hardware streams
(`tools/h264_mbqp.py`, a Python equivalent of `tools/h264_mbqp.c` for an
FFmpeg that cannot be linked against) equal `fpemu.py`'s replay of the same
table on **all 1200 frames** of T8 and T9 (and on the 600 frames of the
original runs), so each table reached the firmware as written.

| run | park_joy | crowd_run | in_to_tree |
|---|---|---|---|
| 1-pass VBR | 7905 kbit/s, VMAF 61.23 | 7472, 62.41 | 7322, 89.21 |
| final pass, macOS-faithful table | 6865 (−14 %), 51.23; MB per 40 frames 1.48 1.16 0.28 0.20 0.32 | 7320, 57.62 | 8164, 89.92 |
| T8 `--rc-scene 1` | 7589 (−5.1 %), 59.74; 0.56 0.69 0.76 0.88 0.90 | 7041 (−12 %), 61.42 | 7836, 89.39 |
| **T9 `--rc-scene 1 --scene-qscale bits`** | **8139 (+1.7 %), 62.58; 0.75 0.85 0.91 0.81 0.75** | **7840 (−2.0 %), 64.89** | **8079 (+1.0 %), 89.78** |
| T12 `--scene-qscale bits` alone | 6987 (−12.7 %), 54.30; 1.44 0.92 0.27 0.33 0.53 | 7245, 58.66 | 8173, 89.90 |

T9's frame 1 is at QP 41 (park_joy) as §12.6 predicted; T12 still
front-loads, so the clamp, not the qscale sums, is the cause. T10 was not
needed: `--rc-scene 1` marks the scene without an IDR.

**The bench (T11)**, five clips × 2/4/8/16 Mbit/s (`ave264-2pass-tuned` in
`bench.py`, `results-m2-2pass-tuned.csv`), PCHIP BD-rate against 1-pass
VBR, negative = fewer bits:

| | crowd | park | ducks | tree | town | mean |
|---|---|---|---|---|---|---|
| macOS-faithful 2-pass, VMAF | +4.2 | +9.2 | −2.3 | +9.0 | +5.9 | +5.2 % |
| **tuned 2-pass, VMAF** | −3.3 | −2.3 | −1.8 | +3.2 | +1.3 | **−0.6 %** |
| macOS-faithful 2-pass, PSNR-Y | +6.7 | +28.8 | +10.4 | +4.6 | +6.0 | +11.3 % |
| **tuned 2-pass, PSNR-Y** | −3.9 | −4.6 | −3.0 | +1.5 | +1.1 | **−1.8 %** |

| mean size miss (max) | 1-pass VBR | macOS-faithful 2-pass | tuned 2-pass |
|---|---|---|---|
| all 20 points | 7.3 % (17.8) | 13.5 % (46.2) | **4.3 % (14.9)** |

**The M1 Pro** (H13S) ran the same bench, all three modes
(`results-m1pro-2pass.csv`): **all 60 points are identical to the M2's**
in bitrate, PSNR, SSIM and VMAF, so the clamp and the fix behave the same
on both SoCs, as the firmware comparison in §12 predicted. Its loop is
faster (VBR 136 against 98 fps; 2-pass 68 against 47).

So the fix turns the final pass from 5-11 % worse than 1-pass VBR into
slightly better (best on the fast clips, where the clamp did the damage),
with a closer size match. It is still a bitrate mode: fixed QP with B
frames (docs/96) stays AVE's most efficient setting, and the 2-pass gain is
small next to B frames' 8-10 %. The useful property is the size match.

![2-pass vs 1-pass](img/multipass.svg)

![park_joy frame by frame](img/finalpass.svg)

