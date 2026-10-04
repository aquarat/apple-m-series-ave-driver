# 81. B-frames (H.264 first, then HEVC)

**Status (2026-10-04, later): UNBLOCKED (docs/94).** A frame with two
references needs both motion-estimation units, which the firmware programs
only when Start wire 0xFCEA (`iMultiMECnt`) is set; with it bs1, hb3 and b4
complete on the M2. Earlier the same day: parked; docs/92 (the M2 stalls identically) and docs/93
(the firmware's per-frame programming, emulated, is complete, and every write
reaches the hardware) narrow the cause to state outside the per-frame
registers.** Earlier: parked after docs/85. The macOS-equivalent
Start image (mq1/mq2) and the firmware's own frame-type path (mq4, type
5) both still stall on the second reference, so the cause is not in the
commands (docs/53 mq1-mq4, bs8).

**Earlier status:** b1-b3 pass (POC type 0, two DPB
references, several commands in flight). b4: the firmware reorders as
designed (P2 completes before the held B1). But **any frame with two
active references stalls the pipe after two macroblocks, in H.264 and in
HEVC** (docs/53 b4-b6c, bs1-bs6, hb2-hb3), and a B needs two. The host
data the firmware reads per reference has been checked against the kext
and the firmware without finding the cause; the switches used to get
there (`session_ref_spacing_p`, `session_search_range`,
`session_hevc_refs`, `session_bframes`, `session_direct_spatial`) stay in
the driver. docs/85 adds `session_macos`: macOS's own fields in bit
groups, for a send-everything-then-bisect test (not yet run). The rest of
this document is the plan as written before any run.

Conventions follow docs/65.
- **fw** = 13.5 firmware image VA (file = VA + 0x4000). **kext** = 13.5 kernelcache VA.
  **UA** = AppleVideoEncoder VA.
- **wire** = byte offset in the command. **VP** = wire − 0x60. **RC** = wire − 0xFF30.
- **PICMGMT** = AVC Process wire − 0x9C8 (HEVC: − 0x55B0). **SH** = the AVC slice-header block at
  Process wire 0x40. **S** = the HEVC slice block at HEVC_ENCODE wire 0x40.
- Labels: **[C]** read from an instruction (VA cited). **[I]** inferred, chain stated.
  **[U]** unknown.
- Every VA can be reproduced with `AVE_MACOS=13.5 python3 tools/disas.py --fw|--kext --addr VA -n LEN`.

Supporting notes, with more VAs:
- Appendix A: firmware queue, kext, user space.
- Appendix B: HEVC RPS programs, SetRpsVars, H265 DPB.

---

## 0. Verdicts

| # | question | answer | label |
|---:|---|---|---|
| 1 | Who reorders? | **The firmware does.** Every driver session already has it switched on, because it follows ui32RCFlag ≠ 0. `ProcessInitStage2` calls `CAVEPriorityQueue::SetClientUseFirmwareRC(…,1)` when wire 0xFF50 ≠ 0 (fw 0x143ec–0x14400 → `strb w2,[x8,#736]` 0x18078). `CommandQueue::Enqueue` then marks every frame of type 2/7 *not ready* (0x16700–0x1672c). `ReorderFrames` (0x160a8) moves the next anchor ahead of the waiting Bs and releases every B with frameNumber ≤ the anchor's. Dispatch skips a client whose head entry is not ready (0x18754). | [C] |
| 2 | Display or coding order? | **Display order**, with PICMGMT+0xCA8 frameNumber = display index. The frame type is explicit: 2 = B, 1 = P, 3 = IDR. Display I0 B1 P2 is coded and completed as I0 P2 B1. A B submitted in *coding* order waits for the **next** anchor, and would wait forever if none comes (no assert, silent stall). | [C] mechanism, [I] end-to-end |
| 3 | POC | Neither codec has a host POC field. **AVC:** SH+40 = frameNumber − frameNumber at the last IDR (fw 0x20ee8–0x20efc; the IDR latches it at 0x21164). It is coded only when SPS pic_order_cnt_type = 0 (0x19ffc → 0x1a2f4). **HEVC:** S+0x28 is overwritten from frameNumber (fw 0x211b4). | [C] |
| 4 | Driver today | SPS poc_type **2**, which cannot express reordering. max_num_ref_frames is hard-wired to 1. `ave_pic_check` refuses type 2 (`ave_cmd.c:1002`). The session is strictly one command in flight (`ave_session_cmd` waits for the reply). B_FRAMES is fixed at 0..0. The HEVC VPS/SPS reorder fields are never written. | [C] |
| 5 | Reference choice for B (AVC) | The firmware RC picks DPB indices by recency: non-reference B → L0[0] = index 1 (the past anchor), L1[0] = index 0 (the future anchor). `getFrameIndexReferenceFrameL00`/`L10` fw 0x43560/0x43870, walked in `ManageDPBBuffer` 0x2d644–0x2d884. This needs **max_num_ref_frames ≥ 2**, so 3 DPB slots. | [C] code, [I] list semantics |
| 6 | Smallest first step | IDR, then {B1, P2} submitted back to back, fixed QP, Main profile. It needs two commands in flight, so the session layer must gain async submit/collect first (§4 b3 before b4). | proposal |

---

## 1. Wire fields that control B-frames

### 1.1 Per-frame (both codecs; PICMGMT is shared, docs/77 §3.3)

| field | wire (AVC / HEVC) | B value | evidence | driver today |
|---|---|---|---|---|
| frame type `IMG_FRAME_TYPE` | PICMGMT+0xCAC = 0x1674 / 0x625C | **2** = B. 7 = "reference B" in hierarchical mode only: `getIsThisFrameMarkedAsNonReference` returns type == 2 when `getHierarchicalMode()` (rc+1940) ≠ 0 (fw 0x43364–0x43370; 0x439d4 returns rc+1940). The RC reference pickers test `== 2` only (0x43568, 0x43874), so **do not send 7** outside hierarchical mode | [C] | 0/1/3 only; `ave_pic_check` refuses 2 |
| frameNumber | PICMGMT+0xCA8 = 0x1670 / 0x6258 | **display index**; unique within the session | POC source (§0 #3); queue key (`Enqueue` 0x166fc, `Complete` 0x16c78); release test `fn ≤ anchor fn` (ReorderFrames 0x16384–0x16648) | = n (submission index); correct only while submission order = display order |
| re-anchor flag | PICMGMT+0x6D8 | **0** | when set, it re-anchors `m_iFirstFrameNumber` / rc+440. A B with fn below the anchor then hits `frameNumber >= m_iFirstFrameNumber` (CAVEDPB.cpp:963 fw 0x2d350; CRateControl.cpp:2751 fw 0x432e4) and spins | 0 |
| PICMGMT+0xCC0 | 0x1688 | **0** | → RCFrameInfo+16 (PipePrepareParam 0x48160 → DPB 0x2d2f4). With 0, the non-reference decision is the simple arm, and B is non-reference iff VP+0x20 = 0 (0x433cc–0x4349c) | 0 |
| forceKeyFrame / forceNonRef | PICMGMT+0x38 / +0x3C | 0 / 0 | dead with explicit types (docs/66 §2.3) | as today |
| coded index + command slot | PICMGMT+0xC04, header +0x1C = 21 + idx | **one distinct pair per in-flight frame** | docs/64 §3 | `idx = n % n_coded`; only one in flight |
| **direct_spatial_mv_pred_flag** (AVC) | **Process wire 0x7C** (SH+60) | 0 (temporal) or 1 (spatial). One test variable, §4 b5 | SH = cmd+0x40 (PipePrepareParam 0x4823c → ctrl+0x2E370+8·ctx, x25). PrepareSliceHeader never writes SH+60 (no store in 0x20d44–0x211b0). The writer codes it for B (0x1a07c). setPipe packs it into the slice config word (0x563a0, 0x56a64). PipePrepareParam branches on it for B (0x48c58 → temporal 0x49048). macOS's value is **[U]** | 0 (the block is zero) |
| HEVC S+0x28 POC lsb | 0x68 | anything; overwritten | fw 0x211b4 (unless PICMGMT+0xF54 ≠ 0) | frames since IDR |

The firmware writes everything else on the AVC slice (docs/65 §5.2). For B that covers:
- slice_type 1, nal_ref_idc = !nonref, nal_unit_type 1 (0x20e38–0x20e48);
- frame_num, incremented only after a reference picture (ctrl+0x235A7, 0x20e50/0x2118c);
- POC lsb;
- num_ref_idx_l0/l1 = count of non-null L0/L1 pointers − 1 (0x20e68–0x20ee0);
- the override flag;
- temporal-direct DistScaleFactor (SH+108, 0x20f00–0x20f90);
- QP from wire 0xFFBC (QP_B) under FIXQP. The jump table 0xcef78 sends types 2 and 7 to `[x0,#2304]`.

All of these are [C].

### 1.2 Start_AVC (session)

| wire | field | B session | evidence | today |
|---|---|---|---|---|
| **0x78** (VP+0x18) | `BFrames` | 1 (or 2) | → sCRCInitParams+64 (fw 0x5dab4/0x5dac0) → rc+700 (0x4030c). ProcessInit splits the frame rate into anchors and Bs: rc+4472 = (fps+B)/(B+1) (0x40a78–0x40a9c). **Only the RC budget uses it.** Also CFrameType init +0x10 (0x5dc38), which only affects type 5. Harmless at FIXQP | 0 |
| 0x7D (VP+0x1D) | `bEnableAdaptB` | **0** | AdaptiveB::Init (type-5 path); ORed into IntraEst mode-word bit 4 (0x5cf94). For HEVC the kext then writes **no SPS RPS sets** (kext 0xf50148) | 0 |
| 0x80 (VP+0x20) | "LowDelay" per the fw dumper; actually selects **reference B** | **0** | → sCRCInitParams+88 (0x5da64) → rc+828 (0x40258). If rc+828 = 0: B is non-reference (0x43494) and L1[0] = index 0 (0x4387c). If ≠ 0: the pyramid state machine (0x43574 ff). | 0 |
| 0x10574 / 78 / 7C (RC+0x644/648/64C) | RefSpacingP / B0 / B1 | 0 or 1 = one reference per list | → rc+812/816/820 (0x5da74–0x5da84 → ProcessInit 0x402a8–0x402c4). `…L01`/`L11` return the "no reference" sentinel unless the value is ≥ 2 (0x436bc, 0x438c8) | 0 (macOS 1/1/1) |
| 0xFF40 (RC+0x10) | `bAllowFrameReordering` | 1 (cosmetic) | Not read on any path found (docs/76 §3; hevc notes). **The reorder switch is 0xFF50, not this field.** | 0 |
| 0xFF50 | ui32RCFlag | 1 or 2, i.e. **non-zero** | turns the firmware reorder queue on (§0 #1) | 1/2 |
| 0xFFBC | QP_B | the B QP | docs/65 §5.1; PrepareSliceHeader 0x20dc0 | written (`ave_cmd.c:465`) |
| 0x109D0 | SPS pic_order_cnt_type | **0** | writer 0x197d4, [x8,#1056] | **2** |
| **0x109D4** | SPS log2_max_pic_order_cnt_lsb_minus4 | ≥ 4. POC lsb 8 bits is plenty for a GOP of up to 128 frames | writer 0x197f0 `ldr w1,[x8,#1060]` (x8 = SPS block 0x105B0); slice u(v) 0x1a2f4 | 0 |
| 0x109DC | SPS max_num_ref_frames | **2**, with `session_dpb=3` | ProvideReferenceFrames numRefs (docs/65 §1.1). The level limit is `AVC_Calc_max_dec_frame_buffering` (kext 0xfffffe0008ea3c88) | 1 (hard-wired, `ave_cmd.c:648`) |
| 0x109F4 | vui_parameters_present | 1 | writer 0x19924 | 0 |
| 0x109F5..0x10A1F | VUI aspect/overscan/video-signal/chroma-loc/timing/nal_hrd/vcl_hrd/pic_struct | **all 0** | The 13.5 VUI writer (0x192e4) omits dependent fields: no aspect_ratio_idc, no overscan_appropriate, no timing body, no low_delay_hrd_flag. Any 1 here makes a malformed SPS | 0 |
| **0x10A20** | bitstream_restriction_flag | 1 | 0x19420 | 0 |
| 0x10A24 / 28 / 2C / 30 / 34 | motion_vectors_over_pic_boundaries (u1), max_bytes_per_pic_denom, max_bits_per_mb_denom, log2_max_mv_length_h/v (ue) | 1, 0, 0, 15, 15. Spec defaults; 15 per H.264 E.2.1 is [I] | 0x19434–0x19474 | 0 |
| **0x10A38** | max_num_reorder_frames | **1** for any number of non-reference Bs; 2 for a pyramid | 0x19478 | 0 |
| **0x10A3C** | max_dec_frame_buffering | **2** (= max_num_ref_frames) | 0x1948c | 0 |
| 0x10C7C | PPS num_ref_idx_l1_default_active_minus1 | 0 | slice writer compares SH+72 with PPS+32 (0x1a0c0) | 0 |
| 0x10C84 | PPS weighted_bipred_idc | **0**. 1 codes pred_weight_table from `computeWeights`; 2 = implicit, hardware support [U] | 0x1a154–0x1a178 | 0 |
| 0x109F0 | direct_8x8_inference_flag | 1 | | 1 |
| SPS profile | profile_idc | **≥ 77**. B slices are illegal in Baseline | spec | V4L2 default High; self-test default 66 |
| colocated table 0xF6B0 | per-slot colocated MV | each slot's published pointer must have ≥ 1280 B of **our** memory below it | B-only reader: `Colocated_L1[0] + off − 20·64` (asserts 6860/6861, 6873/6874; docs/65 §3.2) | pointer = slot base (no pad) |

### 1.3 HEVC_INIT / HEVC_ENCODE

Details and VAs are in Appendix B.

| wire | field | B session | evidence |
|---|---|---|---|
| **0x78** | BFrames → GOP type ctrl+0x1218 = VP+0x18 + 1 (fw 0x84e30–0x84e38) | 1 = IbP, 2 = IbbP. **3/4 (pyramid) need wire 0xFD3C ≠ 0**, else assert+spin fw 0x6c8b0 (CHEVCController_H13C.cpp:2036). > 4 asserts too | `SetRpsVars` 0x6c6e0: set index from d = POC − POC(IDR), table 0xcd48. IbP 0x6c7e8, IbbP 0x6c808, IbBbP 0x6c854, IbBBPP 0x6c890 [C] |
| 0x2CFBC | SPS short-term RPS table | **every index the GOP type can produce, syntax plus derived fields.** IbP: 7 sets. IbbP: 10 | the index mapping matches kext `program_sps_rps_IbP` 0xfffffe0008f50e48 / `IbbP` 0xf51104 [C] |
| entry +0x30/+0x34, +0x38+2j, +0x58+j, +0x68+4j, +0xA8+j | num_neg, num_pos, dpoc_s0_m1 (u16), used_s0, **dpoc_s1_m1 (u32), used_s1** | per set | docs/77 §2.4 |
| entry +0xB8/+0xBC, +0xC0/+0xD0, +0xE0+4j/+0x120+4j, +0x160 | derived NumNeg/**NumPos**, UsedS0/**UsedS1**, DeltaPocS0/**S1**, NumDeltaPocs | must be filled. The **S1 half is new** | firmware counts refs from these (docs/77 §18, fw 0x6ce34–0x6cf7c) |
| 0xFD2C | numRefs / max_num_ref_frames | 2 | docs/77 §2.2 |
| VPS+0x67C/0x69C/0x6BC, SPS+0x278/0x294/0x2B0 (+4i) | max_dec_pic_buffering_minus1 / **num_reorder** / **max_latency_increase_plus1** | 2 / **1** (IbP, IbbP) / 0 | driver writes only the first (`ave_cmd.c:900/920`) |
| 0xFCF8, SPS TMVP, S+0x11C | TMVP | keep **0** | The HEVC colocated reader is gated by TMVP, not by B (asserts 12770/14322; fw 0x6f9b0, 0x74040) [C] |
| 0x7D | bEnableAdaptB | **0** | with it the kext writes no SPS sets (kext 0xf50148) |
| 0xFF34 | ui32IdrPeriod | ≠ 1; keep 30 or the GOP. It must be > the mini-GOP | docs/77 §16. Also `(fn/div) % IdrPeriod ≥ 2` gates on a transcode path at fw 0x77444/0x79c0c [C], meaning [U] |
| type / frameNumber | as §1.1 | type 2, frameNumber = display index = POC | H265 DPB builds L1 only for types 2/7 (fw 0x2fdec) and matches refs by POC. A miss logs "DPB ERROR L0/L1" and exits with an error (0x2fdcc/0x2ffe8) |

**Trimmed sets that fit a 3-slot DPB** [I, checked by hand against the RPS rules].
- IbP: every even index {0, 2, 4, 6} = P {−2}; every odd index {1, 3, 5} = B {−1 | +1}.
- IbbP: indices {0, 3, 6, 9} = P {−3}; {1, 4, 7} = B-first {−1 | +2}; {2, 5, 8} = B-second {−2 | +1}.
- Non-reference Bs appear in no set, so they drop out of the DPB.

macOS's own sets keep up to 4 references, which needs a 5-slot DPB with numRefs 4 (§5, R8). The IbP arm also has an index 7 when `w6 ≠ 0 && d ≤ 1` (0x6c7f4–0x6c800). w6 is [U] (docs/77 §18 open item). Writing an 8th set {−1 | +1} is cheap insurance [I].

### 1.4 What macOS sends

**H.264, plain VideoToolbox Main/High session:**
- `AllowFrameReordering` = 1 (UA 0x29bb4; property handler 0x10988).
- BFrames = the client's count, else 1. It is 3 when S+0x11D40 ≠ 0 (probably multipass [U]) (UA 0x3b514–0x3b540). Baseline clears BFrames and AdaptB (0x3b75c/0x3b760).
- bEnableAdaptB 1.
- RCFlag 1.
- RefSpacing 1/1/1.
- IdrPeriod 30.

So a typical GOP is **1 B**. A 2-B GOP comes only from the client's `…NumberOfBFrames` or the S+0x11D40 path. [C]

**Per frame:**
- type **5**, i.e. the firmware's `CFrameType`/`AdaptiveB` decides whether each frame is a B;
- sent in display order;
- frameNumber = display index.

That frameNumber is [I]. The kext `KernelFrameQueue` is keyed on frameNumber % 16 (kext 0xfffffe0008e96680), and the POC math only works that way.

**Completions:** the host maps them back through coded-header +0x10C FrameNumberFromDriverReturned and +0x110 FrameTypeReturned. User space stamps the DTS at completion. The "H264FrameRec: final decodeTimeStamp" string is [I].

**HEVC:**
- BFrames 1 → `program_sps_rps_IbP`.
- If AdaptB is 1, the kext writes 0 SPS sets (kext 0xf50148) and the firmware builds sets itself. That is [I]; the `ctrl+0x2C468` path is docs/77 §18.

**Lookahead or reorder mode:**
- The reordering is the queue in §0 #1.
- The type-5 path runs `CFrameType::FrameType` (0x3d23c), `AdaptiveB`, and `LookAheadBFrames` (0x3e618) with a 16-entry multipass stats queue (`MPQueue<16>`). It is only fed when multipass is on [I].
- There is no host lookahead depth on 13.5 (docs/35's LookAheadFrameCount is the 26.6.2 layout).
- **We never use type 5.** Explicit types skip `GetFrameType` entirely (0x145d4). Explicit Bs get B-rank 0 and stay FIFO among themselves (0x145ec–0x145f4, 0x16208) [C].

---

## 2. Bitstream side

| element | codec | needed for B | where it comes from | driver today | change |
|---|---|---|---|---|---|
| pic_order_cnt_type | H.264 | 0 | host SPS 0x109D0 | 2 | send 0 |
| log2_max_pic_order_cnt_lsb_minus4 | H.264 | ≥ 4 | host 0x109D4 | 0 (unused) | send 4 |
| pic_order_cnt_lsb | H.264 | display index − IDR index | firmware, from frameNumber | n/a | frameNumber = display index |
| max_num_ref_frames | H.264 | 2 | host 0x109DC | 1 | 2, and session_dpb 3 |
| VUI bitstream_restriction: num_reorder_frames, max_dec_frame_buffering | H.264 | 1, 2 | host 0x10A20..0x10A3C | VUI off | turn on with every other VUI flag 0 |
| profile | H.264 | Main/High | host | V4L2 High | refuse B with Baseline/CBP |
| slice_type B, direct_spatial_mv_pred_flag, override + l1 count, nal_ref_idc 0 | H.264 | yes | firmware (except direct_spatial = host SH+60) | n/a | set Process wire 0x7C |
| dec_ref_pic_marking absent for nal_ref_idc 0 | H.264 | yes | firmware writer 0x1a180 | — | none |
| sps_max_num_reorder_pics[0] (+VPS) | HEVC | 1 (IbP/IbbP) | host SPS+0x294, VPS+0x69C | 0 | write |
| sps_max_latency_increase_plus1 | HEVC | 0 | host | 0 | none |
| sps_max_dec_pic_buffering_minus1 | HEVC | ≥ 2 | host | = max_num_ref_frames | set max_num_ref_frames = 2 |
| st_ref_pic_set with num_positive_pics | HEVC | yes | host RPS table | 4 × IPPP sets | per-GOP table (§1.3) |
| slice_type B, RPS idx, POC lsb, num_ref_idx_l1 | HEVC | yes | firmware | n/a | none |
| nal_unit_type of a non-reference B | HEVC | TRAIL_N would be ideal | the kext uses TRAIL_R (1) for B too (kext Setup_B_Frame 0xfffffe0008f4e854) | — | none; TRAIL_R is legal [I] |
| mvd_l1_zero_flag, cabac_init_flag | HEVC | 0 | S block, offsets [U] | 0 | leave 0 |

Parser and grader gaps (tools only):
- `tools/h264_parse.py` `parse_slice` handles the override and list modification only for P/SP. For B it misses direct_spatial, the l1 override and the l1 list modification, so slice_qp is misparsed.
- `tools/ramp_psnr.py` pairs decoded frame i with source i. That is right if the self-test fills its source by display index and the decoder outputs in display order.
- `tools/hevc_parse.py --check` should grade num_positive and num_reorder.

---

## 3. V4L2

### 3.1 The contract

`Documentation/userspace-api/media/v4l/dev-encoder.rst` ("Encoding") says:
- raw frames go in **in display order**;
- CAPTURE buffers may come out in a different order;
- each CAPTURE buffer carries the **timestamp of the OUTPUT buffer it came from**, so "CAPTURE timestamps will not retain the order of OUTPUT timestamps";
- an OUTPUT buffer need not be returned until the frame is encoded.

So the encoder emits in decode order, and the client derives DTS.

### 3.2 How other stateful encoders handle it (Linux master)

| driver | B_FRAMES | who reorders | timestamp association |
|---|---|---|---|
| venus (`qcom/venus/venc*.c`, `helpers.c`) | 0..4 (`venc_ctrls.c:623`); `HFI_PROPERTY_PARAM_VENC_MAX_NUM_B_FRAMES` (`venc.c:805`) | firmware | a `tss[]` table saves each OUTPUT's timestamp; `venus_helper_get_ts_metadata()` restores it by the timestamp the firmware echoes |
| amphion (`amphion/venc.c`) | 0..4 (`:637`) | firmware | `vpu_find_buf_by_sequence(frame_id)` + `v4l2_m2m_buf_copy_metadata()` (`:787`) |
| wave5 (`wave5-vpu-enc.c`) | no control; flags B via `V4L2_BUF_FLAG_BFRAME` | firmware | `enc_src_idx` → `v4l2_m2m_src_buf_remove_by_idx()`; the source is held until its frame is done (`:271-305`) |
| mediatek (`mtk_vcodec_enc.c`) | none | — | 1:1 `dst.timestamp = src.timestamp` |
| ffmpeg `v4l2_m2m_enc.c` | **forces B_FRAMES = 0** and reads it back. A non-zero value makes it fail "DTS/PTS calculation for V4L2 encoding" (`v4l2_check_b_frame_support`) | — | — |

Every one of them has the firmware reorder, and all of them match a completion back to its source by an ID the firmware echoes. We have the same shape: the firmware reorders, and it echoes frameNumber at coded header +0x10C.

### 3.3 Proposed changes to `ave_v4l2.c` and `ave_session.c`

1. **Control.**
   - `V4L2_CID_MPEG_VIDEO_B_FRAMES` range 0..2, default **0**. ffmpeg relies on 0 (above).
   - Latch it at STREAMON.
   - Refuse, or clamp to 0 with a log, for H.264 Baseline/CBP and for `GOP_SIZE` ≤ B.
   - The kext asserts BFrames ≤ 3 (docs/66 §1.6). 3 needs the pyramid, so it is later.
2. **Mini-GOP batching in the driver.**
   - Keep the m2m one-job model, but make `device_run` accumulate sources.
   - A job whose frame will be a B takes its OUTPUT buffer off the m2m ready queue (`v4l2_m2m_src_buf_remove`) into a driver list and calls `job_finish` with no hardware work.
   - When the anchor's source arrives, submit the whole mini-GOP back to back **in display order** (B…, anchor), then collect 1 + nB completions.
   - The firmware never holds a B waiting for an anchor that might not come. That is the property that makes STREAMOFF and teardown safe (§5, R1).
3. **Anchor rules.** The last frame of a mini-GOP is P, or the IDR at a GOP boundary. The frame before an IDR in display order is **P**, never B (a closed GOP; an IDR flushes the DPB).
   - On `FORCE_KEY_FRAME`, the pending sources close as a mini-GOP whose last frame is P, and the forced frame opens the next GOP as the IDR.
   - On drain (`V4L2_ENC_CMD_STOP`), leftover sources close the same way: last one P, the rest B. Only then is `LAST` flagged.
4. **Completion handling.** Take a CAPTURE buffer per completion, in completion (= decode) order:
   - find the source by FrameNumberFromDriverReturned (+0x10C), then `v4l2_m2m_buf_copy_metadata(src, dst)`;
   - flags from FrameTypeReturned (+0x110): 3 → KEYFRAME, 1 → PFRAME, 2 → **BFRAME**, 4 → dropped (ERROR);
   - release that source (`v4l2_m2m_buf_done(src)`).

   CAPTURE `sequence` stays monotone. `job_ready` must require nB + 1 queued CAPTURE buffers before an anchor job runs. Otherwise the completions must stay in their coded slots until CAPTURE buffers arrive.
5. **Buffer counts.**
   - OUTPUT `queue_setup` minimum nB + 2.
   - Coded slots (`AVE_CODED_SLOTS` 4) ≥ nB + 1 in flight. Four covers nB ≤ 3.
6. **Session layer.** Split `ave_session_process()` into `submit(n_display, type)` and `collect()`.
   - `ave_sess_rx` becomes per-slot (slots 21..40): the IRQ hook routes ENCODE_DONE by header +0x1C.
   - Cross-check +0x10C against the frameNumber that was sent, and make a mismatch loud (docs/68 §5.5).
   - Per-slot `proc_cmd[]` and `coded_hdr[]` already exist, and the memset of `coded_hdr[idx]` moves to submit time.
   - For the self-test: a `session_bframes` module parameter. `ave_session_fill_input` indexes the ramp by **display** index.
7. **Colocated padding (AVC).** Allocate each slot's colocated buffer 1280 B larger and publish base + 1280 in wire 0xF6B0.
8. **What does not change:** Start is still sent once, and the DPB is still the firmware's (docs/64).

---

## 4. Hardware test plan (docs/53 style; one variable per run; repeat before believing)

Every run is `tools/lab-reboot.sh; tools/lab-run.sh NAME "OVERLAY=4 OVERLAY_WAIT=0 HOLD=5" <params>`, on a fresh boot.

Grading:
- `tools/h264_parse.py` / `tools/hevc_parse.py` read the headers; both need the B support in §2 first.
- `tools/ramp_psnr.py` grades every frame.
- `tools/check_frame.py` grades frame 0 (always the IDR).
- `ffprobe -show_frames -of compact` gives pict_type and coded_picture_number.

The parameter names below are **proposed**. Base H.264 self-test: `session_selftest=1 session_qp=30 session_profile=77 session_frame=1 session_dbg=0x20`.

| run | one variable | what "yes" looks like | what "no" looks like, and what it means |
|---|---|---|---|
| **b0** (host) | build, `abi_selftest` and `session_selftest` with pins for every new offset: 0x109D0=0, 0x109D4=4, VUI 0x109F4/0x10A20/0x10A38/0x10A3C, 0x7C, 0x10574.., HEVC S1 derived fields, and HEVC reorder | all pass. The binaries are rebuilt (the Makefiles delete them) | any failure means a layout overlap. Fix it before any boot |
| **b1** | IPPP control, 4 frames, with **only** SPS poc_type 0 / lsb_m4 4 | 4 ENCODE_DONE. h264_parse: poc_type 0, poc_lsb 0,1,2,3. ramp_psnr the same as the last IPPP run (≈44 dB class at QP 30) | poc_lsb absent or constant: the SPS copy is not what the writer reads (0x109D4 wrong). A decode error means POC math misread |
| **b1r** | repeat b1 | identical sizes | differences mean noise; do not proceed |
| **b2** | b1 + `session_dpb=3` + max_num_ref_frames 2, still IPPP | identical P sizes to b1 (RefSpacing 0 keeps one reference, 0x436bc) | 0xE00002BC at Start: the ProvideReferenceFrames level check. "DPB ERROR" in the log, or FrameTypeReturned 4: the DPB walk |
| **b3** | b2 + **two commands in flight, no B**: IDR0, then P1 and P2 submitted back to back, then collect 2 | completions in slot order P1, P2. +0x10C = 1, 2. PSNR as b2 | a timeout on the second: the rx/slot routing (driver). Completions swapped: fine if +0x10C matches, but record it |
| **b4** | b3 with frame 1 typed **B** and submitted **before** P2 (display order): {B1 type 2 fn 1, P2 type 1 fn 2} | **P2's ENCODE_DONE first, then B1's.** FrameTypeReturned 1 then 2. h264_parse (B-aware): B slice, nal_ref_idc 0, poc_lsb 1, l0 = l1 = 1. ffmpeg decodes 3 frames, display order matches the ramp, B PSNR ≥ P PSNR − 1 dB | **(a)** no completion for B1 after P2's: the reorder switch or release is misread; send FLUSH (id 11) as the diagnostic. **(b)** B1 completes first: reordering is off, so the B was coded with no future reference (§0 #1 wrong). **(c)** assert 6860/6861/6873/6874: the colocated L1 pointer or pad. **(d)** "Reference buffers are not in recon buffers!" (fw 0x488d4): a DPB slot mismatch. **(e)** PIPE HANG with L1 readers (0x40D128000 + 0x40·i, i = 1) unprogrammed: the L1 datapath; use `session_diag` reader dumps, as h3g did. **(f)** a decode error with a good firmware log: the SPS/VUI or slice syntax |
| **b4r** | repeat b4 | byte-identical | — |
| **b5** | b4 with Process wire 0x7C = 1 (spatial direct) | decodes; the B is a few % smaller or larger | a hang or decode error with 1 only: the spatial-direct path, so keep 0 |
| **b6** | b4 with 2 Bs: IDR0, {B1, B2, P3}, {B4, B5, P6}, 7 frames | completion order 3,1,2,6,4,5. All decode; frame_num stalls across the Bs | a B2 DPB ERROR: the reference choice for a second B (0x43560 state) |
| **b7** | b6 with 60 frames, IdrPeriod 30, IDR every 30 (frame 29 forced P) | 2 IDRs, no firmware-inserted IDR, POC resets | a decode break at the second GOP: the POC latch (fw 0x21164) or the closed-GOP rule |
| **b8** | wire 0x78 = 1 (macOS BFrames), else b6 | identical at FIXQP (RC-only use, 0x40a78) | a size change means BFrames reaches more than the RC. Record it |
| **b9** | V4L2: `v4l2-ctl … -c video_b_frames=1`, 60 frames, via `tools/v4l2-test.sh` | ffprobe pict_type IBPBP…, 60 frames, PSNR ≈ ctl baseline. Timestamps in the raw stream order are non-monotone | ffmpeg `h264_v4l2m2m` must still work (B stays 0) |
| **b10** | RC on (`session_bitrate=4000000 session_fps=30`) with b6 | achieved rate within ~2% of IPPP RC (f85: 99%) | QP_B runaway: RC B arms, see docs/76 |

HEVC. Base: the h3h/h4 parameters plus `session_codec=1 session_dpb=3`, with numRefs 0xFD2C = 2 and TMVP off.

| run | one variable | yes | no |
|---|---|---|---|
| **hb0** (host) | selftests pin: 7 IbP sets (syntax + derived, S0 and S1), num_reorder 1, 0x78 = 1 | pass | — |
| **hb1** | IDR0, {B1, P2}, wire 0x78 = 1, trimmed IbP sets | order P2, B1. hevc_parse: B slice, RPS idx 3 for B1 and 2 for P2, num_positive 1. Decodes, PSNR ≈ h3h | **PIPE HANG** with L1 readers idle: the S1 derived fields (+0xBC/+0xD0/+0x120/+0x160); as h3g. **"DPB ERROR L1"**: the POC of the future ref is missing (frameNumber is not the display index). **XCODE HANG**: re-check §20's QPMod pairing (unchanged) |
| **hb1r** | repeat | identical | — |
| **hb2** | IbbP (0x78 = 2, 10 sets), 7 frames | order 3,1,2,6,4,5 | a set-index miss: compare the RPS idx in each slice header to fw 0x6c808's mapping |
| **hb3** | V4L2 `CODEC=hevc … -c video_b_frames=1` | as b9 | — |

---

## 5. Risks, and how to catch them early

| # | risk | where it bit before / where it would bite | how to catch it before or at the first boot |
|---:|---|---|---|
| R1 | A **B held forever**: submitted with no later anchor, or a frameNumber that is not display-ordered. There is no assert; the queue head is blocked (0x18754) and the driver times out | new | Batch submission (§3.3 #2), so a B never goes out without its anchor in the same batch. The builder refuses a batch whose anchor fn is not greater than every B fn. On timeout the log says "IO ack did arrive": the firmware took it and is holding it |
| R2 | **ui32IdrPeriod = 1** turns the HEVC controller all-intra (the inter pipe is never set up) → PIPE HANG | h3 (docs/77 §16) | the builder refuses B with IdrPeriod ≤ mini-GOP |
| R3 | **RPS derived fields missing** → the firmware counts 0 references → reference readers are never programmed → PIPE HANG | h3g (docs/77 §18). For B the new half is S1: NumPositivePics +0xBC, UsedByCurrPicS1 +0xD0, DeltaPocS1 +0x120, and NumDeltaPocs = neg + pos | `abi_selftest` pins every derived field of every set. `session_diag` dumps the reader state (docs/77 §17) |
| R4 | **Set index out of range**: the firmware picks by d, and IbP needs indices 0..6, possibly 7 | docs/77 §18 (a single set failed from frame 2) | write every index. The builder checks `n_st_rps ≥` the program's count |
| R5 | **wire 0xFD3C = 0 with BFrames 3/4**, or GOP type > 5 → `CHEVCController_H13C.cpp:2036` assert and spin (fw 0x6c8b0) | new | the builder refuses BFrames > 2 until the pyramid is done |
| R6 | **cu_qp_delta vs QPMod** mismatch → XCODE HANG | h4a (docs/77 §20) | unchanged pairing; B changes nothing there |
| R7 | **S+0x54C/0x550 = 0** → data abort in WriteSliceHeadersHevc | h2b (docs/77 §15) | already pinned. B slices carry WPP entry points like P slices |
| R8 | **DPB too small**: AVC max_num_ref_frames 1 → `DPBGetFrameRequested` walks off the end → "DPB ERROR" (fw 0x2d740) and the frame is dropped (FrameTypeReturned 4). HEVC → "DPB ERROR L0/L1" error exit (0x2fdcc/0x2ffe8) | new | treat FrameTypeReturned 4 as a hard error in the self-test. grep the netconsole log for `DPB ERROR` |
| R9 | **AVC colocated L1 read 1280 B below the slot** (assert 6873/6874 checks the address; the read itself lands outside our buffer) → IOMMU fault or SError | docs/65 §3.2 (predicted) | pad and offset (§3.3 #7). Pin "published = base + 1280" in `session_selftest` |
| R10 | **HEVC TMVP turned on with B** → colocated reader asserts 12770/14322 unless the L1 colocated buffer is right | new | keep TMVP off for the whole B bring-up |
| R11 | **PICMGMT+0x6D8 set** → the re-anchor makes a B's fn fall below m_iFirstFrameNumber → CAVEDPB.cpp:963 / CRateControl.cpp:2751 assert + spin | new | never write it. Pin 0 |
| R12 | **Type 7** outside hierarchical mode: the slice says B, but RC reference selection treats it as non-B (L1 sentinel) → no L1, and L1 asserts or garbage | new, [I] | the builder accepts only 2 |
| R13 | **VUI dependent flags**: the writer silently omits fields → a malformed SPS. Decoders fail with no firmware error | new (VUI writer 0x192e4) | the builder forces every VUI byte in 0x109F5..0x10A1F to 0. `h264_parse` round-trips the SPS |
| R14 | **Pipelined completion misattribution**: the single-rx design reads the wrong coded buffer → plausible but wrong bytes (docs/64 §6 #5 class) | new | route by slot and verify +0x10C on every frame. b3 tests this with no B involved |
| R15 | A **held B's timer**: the per-command `_S_AVE_TimeOut` (header +0x28) or 0xE00 CH_FRAMEDONE_TIMEOUT may be armed at enqueue [U] | new | batches keep the hold at about one anchor's encode time (3–20 ms). Keep the header timeout generous |
| R16 | **Baseline profile with B** → a non-conformant stream (the firmware does not check) | — | refuse in V4L2 and in the self-test |

Signatures to grep in the netconsole receiver log:
- `PIPE HANG`, `XCODE HANG` (heartbeat);
- `ENC: StartCount a-b-c-d, Idle …` (LRMEFS-LRMERC-Pipe-xcode, docs/77 §17);
- `DPB ERROR`;
- `Reference buffers are not in recon buffers!`;
- `_bsp_assert_fail` lines with a `.cpp:NNNN`.

---

## 6. Implementation order (smallest diff first)

1. `ave_abi.h`, 13.5 layout. New offsets:
   - AVC: SPS `log2_max_poc_lsb_m4` 0x109D4, the VUI bytes 0x109F5..0x10A3C, PPS l1-default 0x10C7C and weighted_bipred 0x10C84;
   - `start_avc` `bframes` 0x78, `adapt_b` 0x7D, `ref_b` 0x80, `ref_spacing[3]` 0x10574;
   - `process_avc.sh_direct_spatial` 0x7C;
   - HEVC RPS S1 syntax/derived offsets (+0x68, +0xA8, +0xBC, +0xD0); `start_hevc` 0xFD3C;
   - the 26.6.2 layout gets AVE_OFF_NONE.
2. `ave_cmd.c`:
   - AVC SPS poc_type 0 + lsb 4, max_num_ref_frames from the session, VUI restriction;
   - `ave_pic_check` accepts type 2, but only with poc_type 0 and max_num_ref_frames ≥ 2;
   - HEVC: RPS table per GOP type (IbP/IbbP, trimmed), wire 0x78, VPS/SPS num_reorder;
   - `ave_cmd_build_process_*` writes 0x7C.
3. `ave_session.c`: per-slot rx and submit/collect, `session_bframes`, colocated pad, display-index frameNumber, self-test batches.
4. Tools: B-slice parsing in `h264_parse.py`; S1/num_reorder checks in `hevc_parse.py`.
5. `ave_v4l2.c`: §3.3.
6. Later: RC with B (b10), macOS equivalence (0x78, RefSpacing 1/1/1), pyramid (7, 0xFD3C, VP+0x20), multi-reference sets.

## 7. Unverified, stated plainly

- macOS's direct_spatial_mv_pred_flag (SH+60 in the user-space slice block) [U].
- Whether the firmware releases held Bs on STOP/Close cleanly [U]. The design avoids ever holding one.
- Whether a B's per-command timeout runs while it is held [U] (R15).
- HEVC: the meaning of VP+0xFCDC (wire 0xFD3C) [U]; the IbP index-7 arm (w6) [U]; the divisor ctrl+0x242F8 (assumed 1 from IPPP runs) [I]; whether any firmware code reads SPS num_reorder [U].
- `log2_max_mv_length` = 15 as the spec default [I].
- GStreamer `v4l2h264enc` with B-frames and DTS [U]. ffmpeg's V4L2 wrapper cannot use B at all (it forces 0).
- The end-to-end claim "display-order batch → decode-order completions" is [I]. It is built from [C] pieces: Enqueue/ReorderFrames/GetCommandQueueToDequeue, and the RC reference pickers. b4 is the run that settles it.

---

## Appendix A: firmware queue, kext and user space

## B-frame reordering: firmware / kext / user space (13.5), static only

All fw VAs are 13.5 image VAs; kext VAs 0xfffffe0008...; UA = AppleVideoEncoder VAs.
Listings: regenerate with `AVE_MACOS=13.5 python3 tools/disas.py` (full firmware, user space, the HEVC RPS init).

### 1. The firmware reorders. Host submits in DISPLAY order.

- Reorder switch = client+0x2E0 (queue+0x10), set by CAVEPriorityQueue::SetClientUseFirmwareRC
  (strb w2,[x8,#736] fw 0x18078), called from ProcessInitStage2 at 0x14400 iff wire 0xFF50
  (ui32RCFlag) != 0 (x22 = wire+0xFCE9+0x267 = 0xFF50, 0x14360/0x143ec). [C]
  -> Our driver sends RCFlag 2 (FIXQP) or 1 (RC): reorder is ON for every driver session. [C]/[I]
- CommandQueue::Enqueue fw 0x1664c: for ids 7/8, entry.ready (q+0x48i+0x48) =
  (type != 2 && type != 7) when the switch is set (0x16714-0x1672c), then ReorderFrames. [C]
  type = wire 0x1674 (AVC) / 0x625C (HEVC) = PICMGMT+0xCAC, read AFTER GetFrameType wrote
  its decision back (0x23eb8). So explicit host types 2/7 are held exactly like fw-decided ones. [C]
- ReorderFrames fw 0x160a8: when a non-B (anchor) arrives: q[32]=q[28]; q[28]=anchor.frameNumber
  (0x1614c/0x16154); the anchor is bubbled backwards past queued B entries whose frameNumber >
  previous anchor's (0x161d0-0x16288, 72-byte memcpy swaps); then every queued B with
  frameNumber <= q[28] is marked ready (0x16384-0x16648). Only for entries whose FrameInfo.w4
  (ctx index; AVC 0) == q[36]-1 (q[36] = client+756 = 1, ctor 0x170f8). [C]
  Between Bs: a B moves ahead of an earlier-queued B whose FrameInfo+4 u16 is larger (0x16208-0x16218);
  that u16 comes from CFrameType state+6 via GetFrameType (0x23eac -> sp+0x34 -> FrameInfo w2).
  Explicit types skip GetFrameType -> w1=w2=w3=0 (0x145ec-0x145f4) -> Bs stay FIFO. [C]/[I pyramid rank]
- Dispatch: CAVEPriorityQueue::GetCommandQueueToDequeue fw 0x186c4 looks only at the head
  (tail idx client+744) and skips the client if head.ready == 0 (ldrb [x0,#32]; cbz, 0x18754). [C]
  Dequeue 0x16af4 is strict FIFO on the (reordered) ring. [C]
- Therefore: display order I0 B1 B2 P3 -> coded/completed I0 P3 B1 B2 (decode order).
  Completion order == dispatch order == decode order. [I, from the above]
- Coding-order submission (I0 P3 B1 B2) with explicit types: P3 dispatched; B1/B2 held until the
  NEXT anchor (fn >= 2) arrives; no swap (fn 1,2 < q28=3). Works, but +1 mini-GOP latency and the
  last Bs need FLUSH (id 11) or COMPLETE: ProcessFlush -> PriorityQueue::Complete -> CommandQueue::
  Complete sets entry+0x48 (ready) = 1 for matching frameNumber (fw 0x16bf0, docs/64 §4.1). [C mechanism, I consequence]
- Hang risk: a B whose frameNumber is >= every later anchor's (e.g. frameNumber not display-ordered,
  or constant 0 per docs/64 §6) is never released -> queue head blocked forever, no assert. [I]

### 2. Frame types, frameNumber, POC

- 2 and 7 share AVE_H264_PrepareSliceHeader's B arm 0x20e38 (slice_type 1, nal_ref_idc = !nonref).
  CRateControl::getIsThisFrameMarkedAsNonReference 0x432ac: when rc+1940 != 0, nonref = (type == 2)
  (0x43368-0x43370) -> 2 = non-reference B, 7 = reference B (pyramid middle). Other arm [U]. [C]/[I]
  CFrameType::FrameType emits 7 at 0x3e12c/0x3e1e8, 2 at 0x3d7c0, 0x3dd28, 0x3dee8, 0x3df34. [C]
- frame_num: ++ only if previous picture was a reference (byte ctrl+0x235A7, 0x20e50/0x2118c). [C]
- AVC POC: SH+40 = FrameInfo.frameNumber - frameNumber@lastIDR (0x20ee8-0x20efc; IDR stores it at
  [x0,#2472] 0x21164, SH+40=0). Slice writer 0x1a2f4 codes SH+40 as pic_order_cnt_lsb, u(v) with
  v = SPS[1060]+4, only when SPS pic_order_cnt_type ([x22,#1056]) == 0 (0x19ffc/0x1a000). [C]
  => POC = display index since IDR, step 1. The host's frameNumber IS the POC source: must be the
  display index. Driver's SPS today: poc_type 2 -> must become 0 with log2_max_poc_lsb_minus4
  (13.5 wire 0x109d4 [I, = SPS+1060 next to poc_type 0x109d0]).
- Temporal direct DistScaleFactor computed per B from ref POCs (0x20f00-0x20f90 -> SH+108). [C]
  direct_spatial_mv_pred_flag = SH+60 byte, written only for slice_type 1 (0x1a07c); writer of SH+60 [U].
- B slice header: num_ref_idx_active_override (SH+64) + l0 (SH+68) + l1 (SH+72) when B (0x1a0b8-0x1a124). [C]

### 3. What configures the firmware GOP model (only used for type 5)

CFrameType::init 0x3d05c copies 56 B params to this+0x78; AVC caller 0x5dc34-0x5dc94
(x20 = VP, x11 = RC = VP+0xFED0, x23 = VP+0xF760):
  +0x00 u64 RC+0x08 | +0x08,+0x0C RC+4 IdrPeriod (wire 0xFF34) | +0x10 VP+0x18 BFrames (wire 0x78)
  +0x14 VP+0x20 LowDelay (wire 0x80) | +0x18 u8 1 (HEVC: VP+0x1C bClosedGOP, 0x84e80)
  +0x1A u16 numTemporalLayers (AVC 0; assert <8, CFrameType.cpp:322) | +0x20 swap(RC+0x18,RC+0x1C)
  +0x30 RC+0x14 | +0x34 AVC [VP+0xF760+0x764] = VP+0xFEC4 (iNumViews, wire 0xFF24). [C]
AdaptiveB::Init 0x3cc3c(BFrames=VP+0x18, VP+0xFE98 (wire 0xFEF8, [U]), bEnableAdaptB=VP+0x1D, codec). [C]
VP+0x1D also ORs IntraEst mode-word bit 4 (0x5cf94). HEVC: VP+0x18+1 = ctrl+0x1218 GOP type
(docs/77 §18) selects the RPS set index.

### 4. Kext

- AVE_Client_DecideFrameType: 5 in steady state, 3 when IDR forced (docs/64 §1.2). [C, prior]
- KernelFrameQueue (Init 0xfffffe0008e96310: 16 spots; getRequestedSpot 0xfffffe0008e96680:
  spot = frameNumber % 16, 0x72D8 B each): per-frame FrameInfo looked up by frameNumber, i.e.
  completions are matched by frameNumber, not arrival order. No kext-side reordering found. [C]/[I]
- Coded header +0x10C FrameNumberFromDriverReturned echoes the frame's frameNumber (kext asserts,
  docs/67 §5); +0x110 FrameTypeReturned (2/7 for B). This is how a host maps a decode-order
  completion back to its display index / PTS. [C]
- HEVC_RPS::Init 0xfffffe0008f50010: switch on VP+0x18 (0x...f502d4): 0 -> IPPP (or IPPP_HEIF),
  1 -> IbP, 2 -> IbbP, 3 -> IbBbP, 4 -> IbBBPP (3/4 require VP+0xFCDC != 0 [U name]), >4 -> -1000.
  IbP (0xfffffe0008f50e48): num_short_term_ref_pic_sets = 7 (8 if >1 layer); sets
  0 {4,0} dpoc -2,-4,-6,-8; 2 {1,0} -2; 4 {2,0}; 6 {3,0}; 1 {3,1}; 3 {1,1}; 5 {2,1}
  (neg,pos from consts 0xfffffe000723e7a0..7c8). Exact deltas per set [I].
- AVC_Calc_max_dec_frame_buffering 0xfffffe0008ea3c88: MaxDpbMbs(level)/(mbW*mbH), cap 16. [C]
- AVE_CalcNumOfInputQueue 0xfffffe0008eea698: capped at 10. [C] (inputs not decoded)

### 5. User space

- RC+0x10 bAllowFrameReordering = kVTCompressionPropertyKey_AllowFrameReordering
  (strb [x20,#0xc0] UA 0x10988/0x10990); default 1 (0x29bb4). [C]
- AVE_H264NewDefaults… UA 0x3b514-0x3b540: if RC+0x10: VP+0x18 = client count (S+0x11C90) unless
  0xCDCDCDCD, else (S+0x11D40 != 0 ? 3 : 1); if not: VP+0x18 = 0 (0x3b5d0). Baseline clears
  VP+0x18/0x1D (0x3b75c/0x3b760). [C]; S+0x11D40 meaning [U] (likely multipass) -> plain Main/High
  VT session: BFrames = 1, AdaptB = 1, reordering on, frame type 5 per frame.
- String "FIGAllowFrameReordering ON -> B will be = %d (FIGNumberOfBFrames %d)"; "H264FrameRec: final
  decodeTimeStamp" -> user space assigns DTS on completion and emits in completion (decode) order via
  VTEncoderSessionEmitEncodedFrame (UA 0x959c4, 0x98b50, 0x9989c). [I]

---

## Appendix B: HEVC RPS programs, SetRpsVars, H265 DPB

## HEVC B-frames: static notes (13.5 kext + firmware)

Kext VAs full; fw VAs are image VAs. Entry layout of an SPS short-term set (kext HEVC_RPS and wire RPS block alike): entry k at RPS+4+0x164*k; +0x30 num_negative, +0x34 num_positive, +0x38+2j delta_poc_s0_minus1 (u16), +0x58+j used_s0, +0x68+4j delta_poc_s1_minus1 (u32), +0xA8+j used_s1. RPS+0 = count.

### 1. Kext HEVC_RPS::Init (0xfffffe0008f50010) - set-program selector [C]
Args: x1 = VP, x2 = DRV, w3 = log2_max_poc_lsb_minus4, w4, w5.
- w5 != 0: validate caller-supplied sets only (update_sps_rps_internal_variables per set, 0xf50064-0xf50088).
- DRV+222 or DRV+223 set: count 0; LTR/temporal-layer path (needs VP+0xFE58 numTemporalLayers !=0 and VP+0xFE5A numBTemporalLayers >= 3, else -1001) (0xf5009c-0xf500c4).
- **VP+0x1D bEnableAdaptB != 0 -> count 0, no SPS sets at all** (0xf50148-0xf50158).
- w4 == 1 -> program_sps_rps_IntraOnly (0xf50294).
- else switch **VP+0x18 BFrames** (wire 0x78), jump table 0xf5045c (0xf502d4-0xf502fc):
  - 0 -> DRV+225 ? IPPP_HEIF : IPPP (0xf50304)
  - 1 -> IbP (0xf50320)
  - 2 -> IbbP (0xf50330)
  - 3 -> IbBbP, **only if VP+0xFCDC (wire 0xFD3C) != 0**, else -1000 (0xf5033c)
  - 4 -> IbBBPP, same VP+0xFCDC gate (0xf50354)
  - >4 -> -1000.
- Always: this+0x5A6C+884 = 1<<(w3+4) (MaxPocLsb) (0xf502a8-0xf502b4).
- VP+0xFCDC name [U]. docs/72 calls wire 0xFCDC..DE bDisableIntra*; this is VP+0xFCDC = wire 0xFD3C, a different field. Firmware IEP loads it as [x26,#1388] (x26 = VP+0xF770) at 0x84800 and 0x84e3c; 0x84e40 stores it to [x28,#1958] right after the GOP-type store ctrl+0x1218 (0x84e38). That this is ctrl+0x1232 is [I]: x28's base was not proven. For BFrames==3, 0x84800-0x8481c also sets a byte = (VP+0xFCDC == 0).
- iNumViews (VP+0xFEC4) > 1 adds 1 set (IPPP/IbP/IbbP) or 4 sets (IbBbP 9->13, IbBBPP 5->7). Ignore for single view.
- Constants (kernelcache __PRELINK_TEXT, read through disas.segments): 0xfffffe000723e7a0 {1,0} 7a8 {4,0} 7b0 {2,0} 7b8 {3,0} 7c0 {3,1} 7c8 {2,1} 7d0 u16{3,1,1,1} 7d8 {1,2} 7e0 {1,3} 7e8 u16{0,0,0,1}; 0xfffffe000723e610 u32{0,1}.

Decoded sets (deltas are POC offsets; all used unless noted):
- IPPP (0xf50c54), 4 sets: 0 {-1,-2,-3,-4}; 1 {-1}; 2 {-1,-2}; 3 {-1,-2,-3}.
- IbP (0xf50e48), 7 sets: P-sets 0 {-2,-4,-6,-8}, 2 {-2}, 4 {-2,-4}, 6 {-2,-4,-6}; B-sets 1 {-1,-3,-5|+1}, 3 {-1|+1}, 5 {-1,-3|+1}. So B is non-reference (P deltas step 2).
- IbbP (0xf51104), 10 sets: P 0 {-3,-6,-9,-12}, 3 {-3,-6,-9}, 6 {-3,-6}, 9 {-3}; first B (POC P-2) 1 {-1,-4,-7|+2}, 4 {-1,-4|+2}, 7 {-1|+2}; second B (POC P-1) 2 {-2,-5,-8|+1}, 5 {-2,-5|+1}, 8 {-2|+1}. The B frames are non-reference.
- IbBbP (0xf51470), 9 sets. Pyramid: B at POC 2 is a reference.
  - P: 0 {-4,-6,-8,-10}, 4 {-4,-6,-8}, 8 {-4}.
  - b1: 1 {-1,-3|+1,+3}, 5 {-1|+1,+3}.
  - B2: 2 {-2,-4,-6|+2}, 6 {-2|+2}.
  - b3: 3 {-1,-3,-5|+1}, 7 {-1,-3|+1}.
- IbBBPP (0xf51840), 5 sets: 0 {-1|+1,+2,+3}; 1 {-2|+2}; 2 {-1,-3|+1 (used_s1=0)}; 3 {-4}; 4 {-1,-2,-3,-5}. That implies coding order 0,4,2,3,1,5,... [I].
- Max refs in any macOS set = 4, so macOS's DPB = 5 slots and numRefs 4 (same as the IPPP note in docs/77 §18).

HEVC_Slice::Setup_B_Frame (0xfffffe0008f4e854) vs Setup_P_Frame (0xf4e6b0) [C]. The only difference is S+0x1C slice_type:
- S+8 nal_unit_type = 1 (TRAIL_R) for both. No TRAIL_N for non-reference B.
- S+0x1C = 1 (B) vs 0 (P).
- S+0x120/0x124 = {0, 0xFFFFFFFF} for both. L1 stays -1 even for B: the firmware recomputes it (docs/77 §18).
- S+0x424 = QP - 26 - init_qp - 6*bitdepth.
- S+0x28 = POC lsb argument (w4).
- No collocated_from_l0 or TMVP stores in either.

calc_rps/calc_poc (0xf51be4/0xf51f38) were not decoded [U]. The firmware side makes them moot unless PICMGMT+0xF54 is set (§2).

### 2. Firmware

#### SetRpsVars (0x6c6e0) [C]
Called from PipePrepareParam at 0x660bc and 0x661e4. w1 = the frame's POC ([x25,#40] = S+0x28 at 0x661c8). [x8,#236] = ctrl+0x2C500, the POC of the last IDR. d = POC - POC_IDR.
- PICMGMT+0xF54 (ctrl+0x58554, stored at 0x64ce0-0x64cec) != 0: sps_flag = 0 and the slice RPS is used as the host sent it (0x6c710-0x6c720).
- ctrl+0x2C468 != 0: sps_flag = 0 (firmware-built set).
- Otherwise switch on GOP type ctrl+0x1218 = VP+0x18+1 (0x84e30-0x84e38), table 0xcd48:
  - IPPP (0x6c7d4): docs/77 §18.
  - IbP (0x6c7e8):
    - d odd: d=1 -> 3, d=3 -> 5, d>=5 -> 1 (0x6c8f8).
    - d even: d>7 -> 0, else d&~1 (2, 4, 6) (0x6c964).
    - w6 != 0 and d<=1 -> 7.
  - IbbP (0x6c808), q = d/3:
    - r=1: d<=5 -> 3(2-q)+1 (7, 4), else 1.
    - r=2: d<6 -> 3(2-q)+2 (8, 5), else 2.
    - r=0: 12-d style (9, 6, 3), d>=12 -> 0.
  - IbBbP (0x6c854): **asserts if byte ctrl+0x1232 == 0** (0x6c8b0, line 2036 of the file string at 0xce051, spins at 0x6c8f4).
    - d&3 = 1: 5, else 1 (d>=4).
    - d&3 = 2: 6, else 2.
    - d&3 = 3: 7, else 3.
    - d&3 = 0: 8, 4, then 0.
  - IbBBPP (0x6c890): same ctrl+0x1232 assert; idx = (d-1) mod 5, and 0 on the IDR.
  - GOP type > 5: assert (0x6c8b0).
- These match the kext set numbering exactly, so the host must supply at least that many sets with the right shapes. The firmware indexes by **POC distance from the IDR**, so B frames must carry display-order POCs.

#### POC source [C]
AVE_HEVC_Update_POClsb_SliceType (0x211b4), called at 0x6ae3c:
- if frame type == 3, or the frame number is below ctrl+0x242F8: POC lsb 0 and rec+0x20 = 0.
- else if the flag (PICMGMT+0xF54, [x27,#264] with x27 = ctrl+0x5844C) is set: S+0x28 = rec[+5832] & (MaxLsb-1).
- else: **S+0x28 = frameNumber(PICMGMT+0xCA8)/ctrl[0x242F8] - ctrl[2472]**.
- ctrl[2472] is set on each IDR to frameNumber/div (PrepareSliceHeader IDR arm 0x215d4; the udiv is at 0x21358).
- Slice type (S+0x1C) comes from the frame type through table 0xcef88.
- Then 0x6ae40-0x6af04 derives the full POC (MSB wrap) into rec+5832 (ctrl+0xA70+0x60*ctx+5832).
- **So the host's S+0x28 is overwritten, and the POC comes from frameNumber.** For B, send frameNumber = display index (POC-consistent) in coding order.
- The divisor ctrl+0x242F8 has no writer found [U]. IPPP runs imply 1.
- The only order check on frameNumber is `frameNumber >= m_iFirstFrameNumber` (H265 DPB 0x2e748, CAVEDPB.cpp). There is no check between consecutive frames [C]. The firmware queue keys Complete/Dequeue on frameNumber (docs/64), so it must be unique. CommandQueue::ReorderFrames (0x160a8) may also key on it: **not traced here, parent's scope.**

#### H265VideoEncoderDPB::ManageDPBBuffer (0x2e65c) [C]
- The L1 list is built only for frame type 2 or 7 (`cmp w8,#7 / #2` at 0x2fdec-0x2fdf8).
- References are matched by POC (entry+32), valid (+40) and layer (+76).
- A miss logs "DPB ERROR L0:/L1:" (0x2fdcc/0x2ffe8) and takes the error exit 0x2e7a8, not an assert. POC is also resolved through getFrameIndexFromPOC ("Invalid POC", 0x31718).

#### HEVC setPipe
- L1 ref reader asserts, `sRef.*_L1[l1_me_ref_index]` non-zero and 64-aligned:
  - Low_Res_Y_L1 13453/13454 (0x72478/0x724c0);
  - Y_L1_LSB 13521/13522;
  - UV_L1_LSB 13540/13541;
  - UV_L1_MSB 13553/13554.
  All come from setRefPointers/DPB, so the host only needs DPB slots.
- Colocated reader `Colocated_L1[0]+off` asserts 12770/12771 (0x6f9b0) and 14322/14323 (0x74040). Gate: (S+0x1C != 2 && S+0x11C slice_temporal_mvp) || a ctrl flag; skipped if the pointer is 0. With TMVP off (the driver default) it is not read [C]. **Unlike AVC it is TMVP-gated, not B-gated.**

#### IdrPeriod (ctrl+0x1214)
- The inter arm needs != 1 (docs/77 §16).
- 0x77444-0x77460 and 0x79c0c-0x79c28: (frameNumber/div) % IdrPeriod >= 2 gates a transcode-side path (mode ==1 there) [C]. The meaning is [U]. It is POC/frameNumber-based, so it is unaffected by coding order only if frameNumber is the display index [I].
- Keep IdrPeriod (30) > GOP size [I].

### 3. VPS/SPS reorder fields (driver/ave_cmd.c:893-924)
- The driver writes vps/sps_sub_layer_ordering_info = 1 and max_dec_pic_buffering_minus1[0] = h->max_num_ref_frames.
- It **never writes** num_reorder (VPS+0x69C, SPS+0x294) or max_latency_increase_plus1 (VPS+0x6BC, SPS+0x2B0). They stay 0, which is right for IPPP and wrong for B.
- The offsets are already in ave_abi.h (vps_num_reorder 0x69c, sps_num_reorder 0x294, ...).
- Needed values, per the spec:
  - num_reorder: IbP 1, IbbP 1, IbBbP 2, IbBBPP 3.
  - max_dec_pic_buffering_minus1 >= max(num_reorder, refs in the largest set). With macOS's sets that is 4.
  - latency: 0.
- No firmware reader of SPS+0x294 was searched for [U]. The writer only codes it.

### Driver implications (HEVC)
1. Wire 0x78 (VP+0x18) = BFrames selects both the kext program and the fw set index. For IbBbP/IbBBPP also set wire 0xFD3C != 0, or the firmware asserts at 0x6c8b0 (spin).
2. Keep VP+0x1D bEnableAdaptB = 0 (with it macOS sends no SPS sets and the firmware must build them) [I].
3. Write the full set table for the GOP type: IbP 7, IbbP 10, IbBbP 9 sets, syntax plus derived fields (+0xB8/+0xBC/+0xC0/+0xD0/+0xE0/+0x120/+0x160 per docs/77 §18). With a 2-3 slot DPB, the sets could be trimmed to 1 L0 + 1 L1 ref, e.g. IbP P {-2}, B {-1|+1}. The indices must still exist [I].
4. Submit in coding order with frameNumber = display index. HEVC_ENCODE frame type 2 for B, 1 for P. Driver ave_cmd.c:1002 currently refuses type 2.
5. S+0x28 is cosmetic unless PICMGMT+0xF54 is set.

---

## 8. V4L2 implementation (2026-10-04)

After docs/94 (both ME units, Start wire 0xFCEA) made two-reference frames
work, the V4L2 node gets B frames and two-reference P frames. **Built and
checked offline only; nothing below has run on hardware yet.** The test
list is §8.6.

### 8.1 Controls

| control (v4l2-ctl name) | range | default | latched |
|---|---|---|---|
| `V4L2_CID_MPEG_VIDEO_B_FRAMES` (`video_b_frames`) | 0..2 | **0** | at STREAMON |
| `V4L2_CID_MPEG_VIDEO_REF_NUMBER_FOR_PFRAMES` (`reference_frames_for_a_p_frame`) | 1..2 | **1** | at STREAMON |
| `V4L2_CID_MIN_BUFFERS_FOR_OUTPUT` (read-only) | 1..4 | 1 | follows B_FRAMES: B + 2 |

Refused or clamped at STREAMON, with a kernel warning: B with H.264
Baseline/CBP becomes 0 (R16); a GOP_SIZE with no room for a mini-GOP and
the anchor before its IDR (GOP < B + 2) clamps B to GOP − 2. ffmpeg still
sets B_FRAMES 0 and reads 0 back (§3.2).

Either control above its default configures the session
(`ave_enc_start()`, `struct ave_enc_cfg.bframes/p_refs`):

| | H.264 | HEVC |
|---|---|---|
| DPB slots | 3 (`max(session_dpb, 3)`) | 3 |
| references | SPS max_num_ref_frames 2 | numRefs (0xFD2C) 2, VPS/SPS max_dec_pic_buffering_minus1 2 |
| two-reference P | RefSpacingP 2 (bs1) | RefSpacingP 2 (hb3) and the IPPP sets {-1,-2}; not with B |
| B | POC type 0, lsb 8 bits; profile >= Main (V4L2 default High); colocated pad (R9, as b4); BFrames on the wire (0x78) **only under rate control**: fixed QP keeps b4's 0 | BFrames (0x78) = B, the GOP type IbP/IbbP; the trimmed sets of §1.3; num_reorder 1; IdrPeriod 30 (R2) |
| MultiME (0xFCEA) | 1 | 1 |

The module parameters still win where they are set (`session_multi_me`,
`session_ref_spacing_p`, `session_hevc_refs`, `session_dpb` as a floor,
`session_poc0`), so experiments run as before. With both controls at
their defaults every one of these values is what it was: the Start and
Process commands are unchanged (abi_selftest pins the default commands as
before, and the new fields' zero state), and frames take the old
one-at-a-time path.

### 8.2 Mini-GOP batching (`driver/ave_gop.h`, `ave_v4l2.c`)

- **Holding.** A B source stays in the m2m OUTPUT ready queue: the held
  sources are always its head, oldest first, so STREAMOFF returns them like
  any queued buffer and a drain sees them as queued (the m2m core's
  `last_src_buf` can be a held one). A `job_ready` callback runs a job only
  when there is something to do and one CAPTURE buffer for every frame it
  will code. A hold job codes nothing.
- **The planner** (pure, `ave_gop.h`) decides per new source, in display
  order: hold it; send the held and it as {B.., P}; or close the held as
  {B.., P} and send it alone as an IDR. IDR: the first frame, a forced key
  frame, a GOP boundary (frames since the last IDR = GOP_SIZE). The frame
  before a GOP-boundary IDR is planned as an anchor, so a GOP ends
  `...B P P I` when it does not divide evenly. A drain closes the held with
  the last as P; when the STOP comes after the last source was already
  held, `ave_encoder_cmd` reschedules so that a CLOSE job runs. **The
  firmware never gets a B without its anchor in the same batch, and never
  holds one across jobs** (R1).
- **frameNumber is not the display index.** Every mini-GOP spans B + 1
  numbers whether full or not: the Bs at anchor + 1.., the anchor at
  anchor + B + 1. A short mini-GOP (drain, GOP end, forced key) leaves a
  POC gap, which both standards allow. The reason is HEVC: SetRpsVars picks
  a frame's set by its POC distance from the IDR (Appendix B §2), so a P
  must sit at a multiple of B + 1, or it would get a B set and an L1
  picture that does not exist. A regular stream numbers densely
  (frameNumber = display index). The H.264 firmware chooses references by
  recency (§0 #5) and does not care; both codecs number the same way.
- **Completions** (`ave_enc_encode_batch`, through
  `ave_session_process_batch` with per-frame source planes): routed by the
  reply's slot; the coded header's frameNumber (+0x10C) must equal that
  frame's, or the batch fails with -EPROTO (R14). CAPTURE buffers are
  filled in completion order (decode order), each takes
  `v4l2_m2m_buf_copy_metadata()` from its own source, and KEYFRAME / PFRAME
  / **BFRAME** from the type as coded. CAPTURE `sequence` is monotone,
  OUTPUT `sequence` display order. On a drain the batch's last frame in
  decode order carries LAST. A batch that fails returns its uncompleted
  frames in error, one CAPTURE buffer each; a timeout marks the firmware
  hung, as before.
- **Buffers.** `queue_setup` raises OUTPUT to B + 2 and CAPTURE to B + 1
  when B > 0. Coded slots stay `AVE_CODED_SLOTS` 4 >= B + 1: a batch's
  frameNumbers span at most B + 1 < 4, so its slots (n % 4) differ.
- **Test aid:** `v4l2_test_key_every=N` (0644) acts as FORCE_KEY_FRAME on
  every Nth OUTPUT buffer of a B stream; v4l2-ctl cannot set the control
  in the middle of a stream.

### 8.3 HEVC B

`ave_hevc_session.bframes` writes IbP (8 sets: even P {-2}, odd B
{-1|+1}, index 7 included) or IbbP (10 sets: index%3 = 0 P {-3}, 1 B
{-1|+2}, 2 B {-2|+1}), syntax and derived fields, the S1 half included.
The syntax S1 offsets (+0x68 delta_poc_s1_minus1 u32, +0xA8 used_s1) are
new in `ave_abi.h`; they come from the kext's entry layout (Appendix B
header) and sit exactly between the proven neighbours [I]. HEVC_ENCODE
accepts type 2 only in such a session, with sps_flag 1 / index 0 as for P
(the firmware picks the index). With B, P frames keep one reference. The
probe-time self-test now runs **hb1**: `session_bframes=1` with
`session_codec=1` (H.264's b4 Start is unchanged).

### 8.4 Checked offline

- `abi_selftest` 2241 checks (+281): BFrames 0x78, MultiME 0xFCEA (u16),
  B refusals (pyramid, Baseline, 26.6.2); HEVC IbP and IbbP, every set's
  syntax and derived S0/S1 fields at literal offsets, num_reorder at VPS
  0x10C4C / SPS 0x24920, the IPPP control unchanged, refusals (set count,
  one reference, IdrPeriod 1, 3 B, type 7); HEVC_ENCODE with B.
- `session_selftest` 650160 checks: the planner over B 1..2, GOP 0..13,
  1..40 frames, with and without forced keys and late STOPs: display order
  kept, every B before its anchor in its batch, frameNumbers rising, every
  P at a multiple of B + 1 from its IDR, every B's past anchor the one sent
  before it, no B before an IDR, no GOP overrun. Two deliberate planner
  mutations (no drain anchor; anchor numbered by the short count) fail it.
- `./session_selftest plan B GOP N [KEY_EVERY]` prints the expected
  display-order types for §8.6.

### 8.5 Not done, or uncertain

- **Nothing has run on hardware.** HEVC B has never run at all (hb1 was
  never reached); the S1 offsets and the trimmed sets are its risk (R3, R4).
- H.264 B together with RefSpacingP 2 is allowed but untested; H.264 B
  under rate control writes 0x78 (b8, b10).
- No H.264 VUI (bitstream_restriction, max_num_reorder_frames 1): decoders
  then assume the level's DPB for reordering, which is correct with more
  output delay. b4 decoded without it. R13 makes it a step of its own.
- `tools/h264_parse.py` still misparses B slices (§2): grade with
  ffprobe/ffmpeg. GStreamer with B is untried.
- CAPTURE through DMABUF needs a kernel mapping, as the one-frame path
  already does.

### 8.6 Hardware tests for the lead

Fresh boot, the driver loaded as for the usual V4L2 runs (no `session_*`
overrides), then on the target. Grade each run with what
`tools/v4l2-test.sh` prints (decoded frame count, PSNR against the source;
with `CTRLS` also the display-order frame types) and the kernel log
(`dmesg | grep -E "Start_AVC|HEVC_INIT|batch|v4l2:"`; after a failure, the
netconsole receiver for PIPE HANG / DPB ERROR). The expected type strings
come from `tools/session_selftest/session_selftest plan ...`.

| run | command | yes | no |
|---|---|---|---|
| v0 control | `tools/v4l2-test.sh 60 ctl`, then `... 60 ffmpeg` | file size and PSNR identical to the last baseline; no B/MultiME log line | anything different: the default path changed; stop |
| v1 H.264 2 refs | `CTRLS=reference_frames_for_a_p_frame=2 tools/v4l2-test.sh 60 ctl` | log "0 B frame(s) ... 2 reference(s) ... RefSpacingP 2, MultiME 1"; 60 frames; PSNR >= v0 | a hang at frame 2: MultiME not on the wire |
| v2 H.264 B=1 | `CTRLS=video_b_frames=1 tools/v4l2-test.sh 61 ctl` | types `IBPBP...BP` (61); 61 frames decode; PSNR >= v0 − 0.5 dB | timeout on the first batch: R1 (a B held); a "frameNumber" error: R14 |
| v2r | repeat v2 | identical file | - |
| v3 H.264 B=2 | `CTRLS=video_b_frames=2 tools/v4l2-test.sh 61 ctl` | `IBBPBBP...BBP` | DPB ERROR on the second B (b6) |
| v4 drain | `CTRLS=video_b_frames=2 tools/v4l2-test.sh 60 ctl`, `... 59 ctl`; `CTRLS=video_b_frames=1 ... 60 ctl` | B=2/60 ends `...BBPBP`, B=2/59 `...BBPP`, B=1/60 `...BPP`; every frame decodes; v4l2-ctl ends on LAST, well inside its 60 s timeout | fewer frames, or v4l2-ctl waiting: the drain close or LAST |
| v5 GOP | `CTRLS=video_b_frames=1,video_gop_size=30 tools/v4l2-test.sh 61 ctl` | `IBPB...BPPIBPB...BPPI`: I at 0, 30, 60; frame 29 P; decodes across both IDRs | a decode break at frame 30: the closed-GOP close |
| v6 forced key | `echo 6 > /sys/module/apple_ave/parameters/v4l2_test_key_every`; `CTRLS=video_b_frames=2 tools/v4l2-test.sh 40 ctl`; then back to 0 | `IBBPBPIBBPBPI...` (`plan 2 0 40 6`): IDR at 6, 12, ...; decodes | a B right before an I, or a decode error after an IDR |
| v7 timestamps | v2 with a client that prints CAPTURE timestamps (v4l2-ctl `--verbose`) | decode order: 0, 2, 1, 4, 3, ... frame periods | display order: the metadata copy is wrong |
| v8 HEVC 2 refs | `CODEC=hevc CTRLS=reference_frames_for_a_p_frame=2 tools/v4l2-test.sh 60 ctl` | as the verified module-parameter run: `refs l0 2` (hevc_parse) | - |
| v9 hb1 (self-test first) | `session_selftest=1 session_frame=1 session_codec=1 session_dpb=3 session_bframes=1 session_multi_me=1 session_frames=5` | completion order 0, 2, 1, 4, 3; B slices with num_positive 1; decodes I B P B P | **PIPE HANG** with the L1 readers idle: the S1 fields (R3); **DPB ERROR L1**: POC/frameNumber |
| v10 HEVC B=1 | after v9: `CODEC=hevc CTRLS=video_b_frames=1 tools/v4l2-test.sh 61 ctl`, then `... 60 ctl` (the drain's POC gap) | `IBPB...BP`; decodes; PSNR >= the HEVC baseline − 0.5 dB | as v9; on the 60-frame run only: the POC gap (frameNumber 60 for display 59) |
| v11 HEVC B=2 | `CODEC=hevc CTRLS=video_b_frames=2 tools/v4l2-test.sh 61 ctl` | `IBBP...` | a set-index miss (IbbP) |
| v12 ffmpeg | `tools/v4l2-test.sh 60 ffmpeg`, and with `CODEC=hevc` (B stays 0) | as v0 | ffmpeg refusing the node: the B_FRAMES range change |

One variable per run; v2 and v3 before v4-v7; v9 before v10 and v11.
Record each in docs/53.
