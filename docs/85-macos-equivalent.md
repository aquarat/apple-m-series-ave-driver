# 85. A macOS-equivalent session, then bisect (`session_macos`)

2026-09-27. This is host-side static analysis. **Nothing here has run on hardware.**

**The problem (docs/81, docs/53 b4-b6c, bs1-bs7, hb2-hb3).** Any frame with two active references stalls the pipe after two macroblocks. That covers a B frame, and a P frame with RefSpacingP 2, in both codecs. Single-field moves toward macOS's values have each failed:
- CABAC;
- direct mode;
- search range;
- MaxMvsPer2Mb 16;
- the PMP clock;
- DART stream 15.

This document tries the other route. It lists every field where a plain macOS VideoToolbox H.264 session differs from ours. The target session is Main profile, CABAC, 1280x720 at 30 fps, B frames allowed (BFrames 1, AdaptB 1, RefSpacing 1/1/1, IdrPeriod 30). The driver can now send those fields in ten groups, one bit each. The lead can send them all in one run, then bisect.

Conventions follow docs/62, docs/72 and docs/81:
- **wire** is the byte offset in the command.
- **VP** is wire − 0x60 and **RC** is wire − 0xFF30.
- **PICMGMT** is AVC_ENCODE wire − 0x9C8.
- **SH** is the 0x984-byte slice block at AVC_ENCODE wire 0x40.
- **US** is an AppleVideoEncoder VA; it defaults to `AVE_SetEncoderDefault` sub_2985c unless named.
- **kext** is a 13.5 kernelcache VA (0xeaXXXX means 0xfffffe0008eaXXXX).
- **fw** is a 13.5 firmware image VA.
- Labels: **[C]** means read from an instruction, **[I]** inferred, **[U]** unknown.

Three delegated passes supplied most of §1.2 to §1.4. Each re-read its VAs; their scratch listings are under the session scratchpad (`kextA/`, `sliceB/`, `bufC/`).

## 0. Verdicts

| # | question | answer | label |
|---:|---|---|---|
| 1 | Does any **per-reference** host field differ from macOS? | **No.** Three results support it. First, macOS publishes the same DPB, LowResRef, LowResResult and colocated tables we do, with the same counts (3/3/4/3) and sizes (§1.3). Second, macOS leaves every PICMGMT reference/recon/LowRes/SrcNbr/entropy pointer **zero**, because a type-5 session returns from `AVE_CHM_SetDataInfo_FwBuf` before those groups (kext 0xeb0bcc-0xeb0bd4), and the firmware's `setRefPointers` fills them (§1.2). Third, the slice block's per-reference parts (weights, reordering, MMCO) are zero on macOS too (§1.4). | [C] per table, [I] overall |
| 2 | Then what differs? | Session scalars. The largest difference is the GOP configuration. We send **IdrPeriod 1** with two references. macOS never sends that: its own reference count is 0 when IdrPeriod is 1 (`AVE_Client_CalcRefNum_Ext` kext 0xec6618). The other differences are level 4.0 against 3.1, BFrames 0 against 1, AdaptB, pix_pck, three VP words whose readers are [U], and the SH block. | [C] |
| 3 | Is bs1 (P with RefSpacingP 2, BFrames 0) something macOS ever sends? | **No.** With BFrames 0, macOS's numRefs is 1 whatever RefSpacingP is (kext 0xec6618). It sends SPS max_num_ref_frames = 2 **only** for B sessions, where the kext overwrites user space's 1 (`AVE_Client_InitPS` 0xebdf04 / 0xebdf18). So the macOS-shaped two-reference case is b4 (a B frame). Run both (§3). | [C] |
| 4 | The PICMGMT+0x6D8 conflict between docs/76 and docs/81 | The kext writes `(Process commands since Start < iNumViews)`, which is **1 on the first Process only**, then 0 (kext 0xeab8f8-0xeab908, counter 0xf0783c/0xf05dc0). docs/81 R11 is right for every later frame. docs/76's "1" is right only for frame 0. | [C] |
| 5 | Frame type | macOS sends type **5** on every frame, and the firmware's `CFrameType`/`AdaptiveB` decide. We send explicit 3/1/2, which skips `GetFrameType` (fw 0x145d4). That is not a field a group can copy, because the V4L2 path must choose types itself. It is a proposed experiment in §3.3. | [C] |

## 1. The table

Scope and exclusions:
- **Excluded:** addresses we necessarily set differently (buffer IOVAs), the command header and counters.
- **Included:** sizes and counts where macOS's differ from ours.
- **"ours"** is the bs1 configuration: `session_profile=77 session_poc0=1 session_dpb=3 session_ref_spacing_p=2`, fixed QP, defaults otherwise (`session_lambda=1`, `session_scaling=16`, `session_skipmode=3`, `session_lsb=1`).
- **"grp"** is the `session_macos` group that sends the macOS value (§2). "—" means not sent, with the reason given.

### 1.1 AVC_INIT (session-level)

| wire | w | field | macOS | ours | citation | lbl | grp |
|---|---|---|---|---|---|---|---|
| 0xFF34 | u32 | ui32IdrPeriod | **30** | **1** (`session_idr_period`, the self-test and V4L2) | US 0x29b60; fw 0x5d9e8; RC `this+716 == 1` arms fw 0x40f08, 0x4315c; kext CalcRefNum 0 refs at 1 (0xec6618) | C | GOP |
| 0x78 | u32 | BFrames | 1 (Main/High), 0 Baseline | 0 | US 0x3b51c-0x3b540, 0x3b75c; fw 0x5dab4 (RC init), CFrameType init 0x5dc38 | C | GOP |
| 0xFF40 | u8 | bAllowFrameReordering | 1 | 0 | US 0x29bb4; no firmware reader found (docs/81 §1.2) | C | GOP |
| 0x10574 | u32 | RefSpacingP | 1 | 0, or 2 in bs1 | US 0x29ca4; fw 0x5da74. The group writes 1 only when the run asked for nothing | C | GOP |
| 0x10578 / 0x1057C | u32 | RefSpacingB0 / B1 | 1 / 1 | 0 / 0 | US 0x29ca4/0x29ca8; fw 0x5da7c/0x5da84 → rc+816/820; `getFrameIndexReferenceFrameL01` reads 816 (fw 0x436cc) | C | GOP |
| 0x7D | u8 | bEnableAdaptB | 1 (Main/High), 0 Baseline | 0 | US 0x29a84, 0x3b760; fw 0x5cf94 ORs it into the MCPU mode word bit 4 (MbInput/ME/IntraEst/ModeDec/ReconLuma DMem+0, docs/72 §2.3) | C | ADAPTB |
| SPS 0x105D0 | u32 | level_idc | **31** | 40 (`ave_level_for` floor) | docs/72 §4.1 (US 0x365fc) [I]. Firmware DPB = MaxDpbMbs/mbs: 5 at 3.1, 9 at 4.0 (fw 0x2d018, docs/66 §4.2) | I | ME (session layer) |
| 0x70 | u32 | MaxMvsPer2Mb | 16 (level ≥ 3.1), 32 (3.0), 64 below | 0 | US 0x37508/0x37534; fw 0x5d3c0. bs7 tried it alone | C | ME |
| 0x74 | u32 | MaxSubMbRectSize | 0 for Main/High (576 Baseline < 3.1) | 0 | US 0x37518-0x37550; fw 0x5cf20 | C | ME (Main: no change) |
| 0xFCE8 | u8 | pix_pck | 1 | 0 | US 0x29b1c; fw 0x5d118 → source format word bits 16+ (docs/62 §6.2). m2b: no change for 8-bit | C | PARAMS |
| 0xFD10 / 0xFD1C | u32 | sao_enb_config / sao_eo_bo_offset_config | 0xFFFF / 0xFFFFFFFF | 0 | US 0x29b28/0x29b20; not read by AVC InitEncodingParameters (docs/62 §6.5 scan) | C | PARAMS |
| 0xFD20 | u32 | input_bitdepth | 8 | 0 | US 0x29b18; not read by AVC (docs/72 §5.3) | C | PARAMS |
| 0xFDA4 | u32 | VP+0xFD44 | **1** | 0 | US 0x29aac; fw 0x5d10c → EncCommParams+368 (ctrl+0x24134); consumer **[U]** | C / U | PARAMS |
| 0xFDB4 | u32 | sSliceMap slice 0 end | **height** (720) | 0 | US 0x29abc/0x29ac4; fw memcpy 0x104 → client+0xCC (0x14414); consumer [U] | C / U | PARAMS |
| 0xFEB4 | u32 | VP+0xFE54 | **16** | 0 (AVC; HEVC already 16) | US 0x29acc; fw 0x5d008 → EncCommParams+380; consumer [U] | C / U | PARAMS |
| 0xFF04..0xFF10 | 4×u32 | multipass block | −1 ×4 | 0 | US 0x29af4; fw 0x5cdfc-0x5ce34 (< 0 → 6, 0x1305). They reach only `CMultiPassControl::Initialize`, which is skipped (docs/76 §1.2) | C | PARAMS |
| 0xFEC0 | u16 | src_mode | from the pixel buffer | 0 | US 0x2a124 (docs/72 §5.3) | U | — (value unknown) |
| 0xFF48 | u32 | RC+0x18 | 0xCDCDCDCD | 0 (FIXQP), fps_den (RC) | US 0x29c58; RC MiniGOP arm (docs/76 §2.4) | C | RC |
| 0xFF60 | u32 | RC+0x30 | 1.0f | 0 | US 0x29b74; DRL/CBR only (docs/76) | C | RC |
| 0xFF73 | u8 | bEnableVarianceQPMod | 1 (kept by macOS's FIXQP arm too) | 0 | US 0x29b8c; fw 0x5d188 → DMem 0x764 bit 9 | C | RC |
| 0xFF84 | u32 | RealTimeClient | 0xCDCDCDCD | 0 | US 0x29bd4; fw 0x5cee4 | C | RC |
| 0xFF88 | u32 | SoftMinQP | 0xCDCDCDCD (fw → 0) | 0 (FIXQP) / 10 (RC) | US 0x29bd4; fw 0x5d9f0 | C | RC |
| 0xFF50 | u32 | ui32RCFlag | 1 (RC on); 2 on macOS's own FIXQP arm | 2 | US 0x3aa74 / 0x3b8ac | C | — (`session_bitrate` switches it) |
| 0xFF70/72/78/80 | u8/u8/u8/u32 | QPMod, LamdaMod, QPModRefresh, eStaticAreasLowQpSel | 1/1/1/1 at RC on; macOS FIXQP clears all four | 0 | US 0x29b80-0x29b8c, 0x3aa84; clear 0x3b8e8-0x3b8fc | C | QPMOD (use with `session_bitrate`) |
| 0xFF30 / 0xFF58 | u32 | bitrate / alt bitrate | client / level default | 0 at FIXQP | docs/76 §3; FIXQP ignores both | C | — |
| 0xFFB4..BC | u32 | initial QPs | 26/26/26 at RC on | `session_qp` | docs/72 §5.2 | C | — (the test's QP) |
| SPS 0x109CC | u32 | log2_max_frame_num_minus4 | 1 | 0 | US 0x29dc4 (const 0x115fa0) | C | SPS |
| SPS 0x109D4 | u32 | log2_max_poc_lsb_minus4 | 2 | 4 (poc0) | US 0x29dc4 | C | SPS (with `session_poc0`) |
| SPS 0x109D0 | u32 | pic_order_cnt_type | 0 | 0 with poc0, else 2 | US 0x29dc4 | C | — (`session_poc0`) |
| SPS 0x109DC | u32 | max_num_ref_frames | **2** for a B session (kext), 1 without B | 2 (dpb 3) | kext InitPS 0xebdf04/0xebdf18 | C | — (same) |
| SPS 0x109F4..0x10A0C | | VUI bytes, `vui_parameters_present` 0 | 0x109F7=1, 0x109F8=5, 0x10A00=1, 0x10A04/08/0C=2 | 0 | US 0x29ddc-0x29dec; not coded while present = 0 | C | SPS |
| PPS 0x10C94..0x10CAF, 0x10CBC..0x10CD7 | | PPS+0x38..0x53, +0x60..0x7B | 0xCD fill | 0 | US 0x29e40-0x29e5c; not read by the PPS writer (fw 0x1997c) | C | SPS |
| PPS 0x10C68 | u32 | entropy_coding_mode | 1 | 0 in bs1 | US 0x29e00 | C | — (`session_cabac`; b6c tried it) |
| 0x10DE0, 0x10DE4, 0x10E0C, hdr +0x14 | | client scalars | client[0xDE30], bTranscodeOverlap, CHM index, client[212] | 0 | docs/62 §1.1 | U | — (values unknown) |
| **counts** (§1.3) | | recon / LowResRef / colocated / LowResResult | 3 / 3 / 3 / 4 | 3 / 3 / 3 / 4 | bufC | C | same |
| 0x5A8/0x5B0/0x5B8 | u64×2 + u32 | TranscodedData | **2** surfaces, align4K(CodedData/2) | none | kext 0xeaf280/0xeaf28c, count 0xea5b14; no AVC firmware reader found | C / I | BUFS |
| 0xF830 / 0xFA30 | | entropy table | 4 × **1** column (SetNum 1) | 4 × 4 | kext 0xea5b88 | C | BUFS |

### 1.2 AVC_ENCODE PICMGMT (per frame)

| PICMGMT (wire) | w | macOS | ours | citation | fw effect | lbl | grp |
|---|---|---|---|---|---|---|---|
| +0xCAC (0x1674) | u32 | **5** | 3/1/2 | kext 0xeaaa50 | type 5 runs GetFrameType (fw 0x23eb8) | C | — (§3.3) |
| +0x638 (0x1000) | s32 | **−1** | 0 | kext 0xeab944/948 | PipePrepareParam 0x486fc → ConfigureMCPUs 0x60c14: −1 = the firmware computes avgVar; anything else is "avgVar from driver", so ours is 0 on every frame | C | PIC |
| +0x6C4 / +0x6D0 | u32 | 0xFFFF / 100 | 0 | kext 0xeabb04/0xeabb10 | read only when eStaticAreasLowQpSel == 3 (never): dead | C | PIC |
| +0x6D8 (0x10A0) | u8 | 1 on the first Process, then 0 | 0 | kext 0xeab8f8-908 | re-anchors m_iFirstFrameNumber (fw 0x2d3b8); a no-op at fn 0 | C | PIC |
| +0x898..+0x8B8, +0xC20, +0x980..+0xBF8 | u64 | **0** (FwBuf returns early for type 5) | dummy recon arena, LowRes slot 0, SrcNbr, entropy | kext 0xeb0bcc-0xeb0bd4 | rebuilt by setRefPointers (fw 0x2c320, 0x2c98c-0x2cc0c). +0x8B8 is rewritten only if recon Y matches (0x2c4b0), so ours may survive | C | PIC (zeroed) |
| +0x08 (0x9D0) | u32 | per-frame bitrate (RC) | 0 | kext 0xeab8b8; fw 0x2d308, 0x3ff4c | RC only | C | — (FIXQP) |
| +0x10..+0x37 | | DRL pairs/count | 0 | kext 0xeab8d0/8d4 | DRL only | C | — |
| +0x6D9/+0x6DA/+0x6DC/+0x6E4/+0x6E8.. | | RC / throughput fields | mostly 0; +0x6DC 1 or 3, +0x6E4 3 | kext 0xeab910-0xeabb84 | no AVC reader for +0x6DC/+0x6E4; the rest are RC or second-pass | C | — |
| +0x964 | 8 B | FI+0xA4C every frame | 0 | kext 0xeb091c | setLRME 0x52350, compressed input only | C | — |
| +0x858/+0x860/+0x878 | u64 | 0 | 0 | — | no AVC reader | C | same |

### 1.3 Buffer tables and counts (AVC_INIT)

These were all read in the bufC pass. The counts come from `AVE_Client_CalcSurfaceInfo` (kext 0xec67c4), with numRefs = 2 from CalcRefNum.

| table | wire | macOS | ours | note |
|---|---|---|---|---|
| recon set 0 | 0x88 | 3 (numRefs+1, kext 0xea5034) | 3 | same |
| LowResRef set 0 | 0x2A8 | 3 (kext 0xea55d8; floored at 5 only if wire 0xFF14 ≠ 0) | 3 | same |
| LowResResult | 0x3B8 | 4 × ALIGN(4W,128)·⌈H/64⌉+1024 (kext 0xea5708) | 4, same size | same. Per-reference writer 0x40D1308C0+0x40i (fw 0x51e84-0x520b8) |
| LowResRCResult | 0x438 | **0** (needs 0xFECD and 0xFCE9, kext 0xea58c0) | 0 | its only reader, SetLRMERCPerPass (fw 0x62e78), is indexed per **pass**, not per reference |
| colocated set 0 | 0xF6B0 | 3 | 3 (+1280 B pad under B) | same |
| TranscodedData | 0x5A8 | **2** | 0 | BUFS; no AVC reader found |
| entropy | 0xF830 | 4 × 1 | 4 × 4 | BUFS |
| set 1 / arm 1 / CrcQPMod / MBInputCtrl / MCTF | | 0 | 0 | same |

### 1.4 The slice block at cmd+0x40 (per frame)

User space seeds it once in `AVE_SetEncoderDefault`, and the kext's `AVC_Slice` constructor (kext 0xf470b4) adjusts it. On a plain session nothing changes it per frame: `SetSliceHeader`'s full path needs bRCEnableDriver, and GenerateSlicesMap writes SH+1396 = 0 and a zero map at iNum 1. The firmware's `PrepareSliceHeader` (fw 0x20d44) overwrites +4, +8, +16, +28, +34, +36, +40, +64, +68, +72, +80, +108, +116 and +1400.

| SH (wire) | w | macOS | ours | fw reader | lbl | grp |
|---|---|---|---|---|---|---|
| +0x00 (0x40) | u32 | 0x984 | 0 | none found | C | SH |
| +0x04 (0x44) | u32 | 1 | 0 | overwritten | C | SH |
| +0x10 (0x50) | u32 | 2 | 0 | overwritten | C | SH |
| +0x24 (0x64) | u32 | 1 | 0 | idr_pic_id toggle, IDR only (fw 0x21104) | C | SH |
| **+0x3C (0x7C)** | u8 | **1** | 0 (1 only for B with `session_direct_spatial`) | **setPipe slice config word bit 2 for every slice type** (fw 0x563a0, 0x56a64); B-only direct branch (0x48c58) | C | SH |
| +0x980 (0x9C0) | u32 | 0xFFFFFFFF | 0 | fw 0x486f4 → CAVLC DMem 0x84, a divisor; same outcome for 0 and −1 [I] | C / I | SH |
| everything else | | 0 | 0 | weights (+0x370..), reordering (+0x1D4..), MMCO (+0x7C..), map (+0x472), MBs/slice (+0x574): all 0 on macOS too | C | same |

## 2. The groups (`session_macos`, bits)

`session_macos` is a module parameter, uint, **0644**, default 0. It is latched per stream at Start_AVC and applied to H.264 only, in both the self-test and V4L2. One log line at Start lists the groups: `session: Start_AVC: session_macos 0x3ff: macOS groups applied: GOP PARAMS SH ...`. The value 0 builds exactly what 96999b3 built: `abi_selftest` hashes the bs1 Start_AVC, IDR and P commands against that commit's builder.

The constants are in `ave_macos_start_avc_13_5[]` / `ave_macos_process_avc_13_5[]` (`driver/ave_abi.h`). Values that depend on the session are in `ave_cmd_build_start_avc()` / `_process_avc()`. On 26.6.2 there are no tables, and any bit is refused.

| bit | name | what it sends | why this rank |
|---:|---|---|---|
| 0 | **GOP** | IdrPeriod 30 (only if the run asked for ≤ 1), BFrames 1 (0 for Baseline), bAllowFrameReordering 1, RefSpacingP 1 (only if not asked), RefSpacingB0/B1 1 | **Top suspect.** IdrPeriod 1 with references is a configuration macOS never produces: its reference count is 0 at IdrPeriod 1 (kext 0xec6618). The firmware RC has `IdrPeriod == 1` arms (fw 0x40f08, 0x4315c), and HEVC at IdrPeriod 1 never set up the inter pipe (docs/77 §16). Against it: HEVC hb3 hung at IdrPeriod 30, so GOP alone may not be enough |
| 1 | PARAMS | pix_pck 1, SAO words, input_bitdepth 8, **0xFDA4 = 1, 0xFEB4 = 16**, slice-map end = height, multipass −1 ×4 | The two VP words with unknown firmware consumers (EncCommParams+368/+380), and the slice-map height (consumer [U]). These are shared with HEVC (0xFEB4 = 16 is already sent there) |
| 2 | SH | the six non-zero slice-block words, including direct_spatial 1 in **every** frame's slice config word | The only per-frame field that reaches a hardware register and differs on P frames (bit 2 of the slice config word) |
| 3 | RC | 0xFF48 / 0xFF84 / 0xFF88 = 0xCDCDCDCD, 0xFF60 = 1.0f, bEnableVarianceQPMod 1 | RC init inputs; VarianceQPMod reaches DMem 0x764 bit 9 in the MCPUs |
| 4 | ME | macOS's Annex A level (3.1 at 720p30, via `ave_level_macos` in the session layer), MaxMvsPer2Mb by level, MaxSubMbRectSize | The level changes the firmware's DPB length (9 → 5 at 720p). MaxMvs alone failed in bs7 |
| 5 | ADAPTB | bEnableAdaptB 1 (0 for Baseline) | MCPU mode-word bit 4 in five stages including MotionEst (fw 0x5cf94) |
| 6 | PIC | PICMGMT+0x638 = −1, +0x6C4/+0x6D0, +0x6D8 = (frame 0), and zero for recon/LowRes/SrcNbr/entropy pointers | avgVar "from driver" = 0 on every frame is ours today; the pointers are firmware-owned |
| 7 | SPS | 5-bit frame_num, 6-bit POC lsb (with poc0), VUI bytes (not coded), PPS 0xCD bytes (not coded) | Bitstream-only |
| 8 | BUFS | two TranscodedData surfaces (align4K(coded/2)) at 0x5A8/0x5B0/0x5B8; one entropy column at Start and per frame | Count parity. No AVC reader found for TranscodedData |
| 9 | QPMOD | bEnableQPMod, LamdaMod, QPModRefresh, eStaticAreasLowQpSel = 1 | RC-on only (macOS's own FIXQP clears them). Meaningful with `session_bitrate` |

`session_macos=0x3ff` sends every group.

Not sent by any group, with the reason:
- **frame type 5:** the driver must choose types (§3.3);
- **RCFlag 1:** that is `session_bitrate`;
- **src_mode:** value [U];
- **the header/client scalars at 0x10DE0/0x10DE4/0x10E0C/+0x14:** values [U];
- **CABAC and POC type 0:** they are their own parameters.

## 3. Hardware test plan (for the lead)

Every run follows AGENTS.md:
```
tools/lab-reboot.sh
tools/lab-run.sh NAME "OVERLAY=4 OVERLAY_WAIT=0 HOLD=5" <params>
```
Base (bs1): `session_selftest=1 session_frame=1 session_frames=3 session_poc0=1 session_dpb=3 session_profile=77 session_ref_spacing_p=2`.

What to check before believing a run:
- The Start line lists the groups.
- `h264_parse` shows level 31, `log2_max_frame_num` 5 bits and POC lsb 6 bits when ME and SPS are on.
- Frame 0 (IDR) and frame 1 (one-reference P) still complete.

The sessions differ from bs1 only in the listed parameters.

### 3.1 The runs

| run | params beyond base | yes | no |
|---|---|---|---|
| **mq1** | `session_macos=0x3ff` | frame 2 (two L0 references) completes. Record size and PSNR (`ramp_psnr`) | `PIPE HANG 3, 3` with the same ModeDec/ReconLuma 2 as bs1 → §3.3 |
| mq1r | repeat mq1 | same result | — |
| mq0 | `session_macos=0` | the control: bs1's hang, byte-identical commands | if it completes, the stall is intermittent; stop and repeat |

**If frame 2 completes, bisect in this order.** Each run is one variable, applied as the bit mask with that group removed from 0x3ff. Repeat the decisive one.

1. **0x3fe** (drop GOP). If this hangs, confirm with **0x001** alone.
2. **0x3fd** (drop PARAMS), then 0x002 alone.
3. **0x3fb** (drop SH), then 0x004.
4. 0x3ef (drop ME; its level also changes the firmware DPB). Then 0x3f7 (RC) and 0x3df (ADAPTB).
5. 0x3bf (PIC), 0x37f (SPS), 0x2ff (BUFS), 0x1ff (QPMOD). These are the least likely, and SPS/QPMOD are bitstream or RC only.

If several groups are needed together, the drop-one runs show it: more than one drop-one run will hang. Then pair them.

### 3.2 The macOS-shaped case, in parallel

bs1 has no macOS counterpart: macOS gives a P frame one reference unless BFrames is on (§0 row 3). The B frame is the case macOS actually sends:
- **mq2:** b4's parameters (`session_bframes=1 session_poc0=1 session_dpb=3 session_profile=77`, 3 frames) + `session_macos=0x3ff`.
- **Yes:** B1 completes after P2 (FrameTypeReturned 1 then 2).

Run mq2 even if mq1 passes. It is the configuration closest to macOS's.

### 3.3 If mq1 and mq2 still hang

Every host field this document could find now matches, except the ones marked [U] and the frame type. Next steps, in order:

1. **Frame type 5** (a new driver switch, not yet written). Send type 5 with RCFlag 1 (`session_bitrate`), BFrames 0 and RefSpacingP 2, and let `GetFrameType` decide. If a firmware-typed two-reference P still stalls, the explicit-type path is cleared. Watch for the firmware inserting an IDR every IdrPeriod.
2. **The [U] values.** `src_mode` (wire 0xFEC0) from `AVE_VerifyImageBuffer`'s output (US 0x2a0dc) is not decoded. The header +0x14 and 0x10DE0/0x10DE4 values are also undecoded; the kext reads them from client fields. Decode them before guessing.
3. **The firmware side of the second reference.** Nothing on the host is per-reference any more. Look at the per-reference DMA and ME programming in setPipe (fw 0x534c8-0x538a8) and at `MERefIdxMapping[i].MeRamEn` (asserts fw 0x24908/0x24950, docs/65 §2.5). Take a read-only register dump at the stall against the one-reference control (bs4 style).
4. **HEVC analogue.** HEVC_INIT shares the VP/RC half (docs/77 §2.2), so GOP, PARAMS, RC, ADAPTB, ME's MaxMvs and QPMOD apply at the same wire offsets. Porting them means setting `ave_hevc_session.vp.macos` in `ave_session_start_hevc()` and gating the AVC-only parts (the SPS/PPS patches, the level, TranscodedData, which HEVC already sends). hb3 is the run to repeat.

## 4. Corrections to earlier documents (annotations; nothing retracted)

- **docs/76 §4.1:**
  - PICMGMT+0x6D8 is 1 on the first Process only.
  - PICMGMT+0x08 **is** a per-frame bitrate on the non-CBR path (fw 0x3ff4c).
  - +0x6DC and +0x6E4 have no AVC reader.
- **docs/81 §1.2:** macOS sends SPS max_num_ref_frames **2** for a B session (kext 0xebdf18), not user space's 1. With BFrames 0 its numRefs is 1 whatever RefSpacingP is.
- **docs/47 §1.2 and docs/62 §2.1:** in a type-5 session the kext's sRef/recon/SrcNbr/entropy/LowRes PICMGMT stores are unreachable (kext 0xeb0bd4). The every-frame double is at +0x964; +0x8F8 is HEIF only.
- **docs/62 §1.3:**
  - The LowRes "second arm" is the TypeNum dimension, selected by wire 0xFF14.
  - The 0x40 stride is SetNum.
  - The driver already fills LowResResult.
- **docs/62 §2 row 4** ("no firmware read of cmd+0x40"): SH+0x3C reaches setPipe's slice config word (fw 0x563a0), and SH+0x980 reaches CAVLC DMem 0x84 (fw 0x486f4).
- **docs/65 §4.3:** the "0x38 discrepancy" is dpb vs ctx = dpb+0x38, so both name the same address.
