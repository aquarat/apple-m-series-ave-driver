# `sCAveCmdAvcStart` — the session parameters a legal H.264 encode needs

*What the host must put in the 0x3180-byte `Start_AVC` body for the firmware to
accept it and emit a bitstream a decoder will parse. The answer turned out to be
much more concrete than "map 12 KB": the command carries a **complete H.264 SPS
and PPS parameter struct**, field for field, and the firmware contains its own
exp-Golomb writer that turns them into the NAL units. Get those two structs
right and the stream is conformant by construction.*

Every row is marked **confirmed** (read out of an instruction, VA cited),
**inferred** (a chain of reasoning over confirmed facts, stated as such) or
**unknown**, per [00-methodology.md](00-methodology.md).

Firmware VAs are **image virtual addresses** (`__TEXT` vmaddr 0, fileoff
`0x4000`). String addresses quoted from `strings -t x data/blobs/ave_h13c.bin`
are **file offsets** and are `VA + 0x4000`.

Reproduce any line with:

```sh
python3 tools/disas.py --fw   --addr 0x6b8b0 -n 0x2b90     # InitEncodingParameters
python3 tools/disas.py --fw   --addr 0x63c40 -n 0x21d4     # DebugInit (the field-name oracle)
python3 tools/disas.py --fw   --addr 0x41c8c -n 0x260      # AVC_SPS::seq_parameter_set_rbsp
python3 tools/disas.py --kext --addr 0xfffffe0008cbea9c -n 0x5d8
```

This document extends [20-command-structs.md](20-command-structs.md) §3, which
mapped the command at block granularity. Nothing in docs/20 §3 is contradicted;
several things are corrected or sharpened, see §10.

---

## 0. The result that shrinks the problem

**The Start command's H.264 conformance parameters are a plain
`H264_SEQUENCE_HEADER_PARAMS` followed by a plain `H264_PICTURE_HEADER_PARAMS`,
and the firmware's own `AVC_SPS` / `AVC_PPS` classes serialise them to RBSP.**

The translation key that unlocked this: `CAVCController::InitEncodingParameters`
(`0x6b8b0`) works off a pointer `x24 = [arg1 + 8]` — the *payload* of the Start
command, i.e. **`cmd + 0x68`**. It does

```
6c050:  mov w8, #0x28b4
6c054:  add x1, x24, x8            ; src = payload + 0x28B4  = cmd + 0x291C
6c020:  add x0, x28, #4            ; dst = this  + 0x247C4
6c024:  mov w2, #0x6b4             ; 1716 bytes
6c058:  bl  0x581c                 ; memcpy

6c05c:  ...  x0 = this + 0x24E78, x1 = x24 + 0x2F68 (= cmd + 0x2FD0)
6c070:  mov w2, #0x180             ; 384 bytes
6c074:  bl  0x581c
```

and `CAVCController::DebugInit` (`0x63c40`) then dumps `this + 0x247C4` field by
field with format strings named `SPSparams.*`, `CropParams.*` and `PPSParams.*`.
So a single disassembly gives the **name and offset of every conformance field**.

The `x24 = cmd + 0x68` identification is **confirmed** four independent ways:

| firmware load | payload off | + 0x68 | already known to be |
|---|---:|---:|---|
| `0x6c078` `ldr w8,[x24,#768]` → `[x20,#3960]`, printed `"width: %d"` (`0x646a4`) | 768 | `0x368` | `VideoParams.ui32Width` (docs/20 §3.3) |
| `0x6c0ac`/`b8`/`c4` `ldr w9,[x24,#472/476/480]` | 472 | `0x240` | `QP[I]/QP[P]/QP[B]` (docs/20 §3.3) |
| `0x6ba68` `ldrsw x8,[x24,#12524]` | 12524 | `0x3154` | `_S_AVE_EUInfo` index (docs/20 §3.4) |
| `0x6c054` memcpy source | 10420 | `0x291C` | AVC profile block (docs/20 §3.3) |

Everything below is stated in **command offsets**; convert to
`InitEncodingParameters` offsets by subtracting `0x68`.

The second half of the result: the two structs are also **produced verbatim by
the host from user settings**. `AVE_Client_Verify` memcpy's them out of
`AVE_SessionSettings_UserKernel_Data` (`+0x3398` → `client+0x4AD0`, `+0x3A4C` →
`client+0x5184`) at `0xfffffe0008b912a4`/`b8`, and the *kext* carries the same
`AVC_SPS::seq_parameter_set_rbsp` (`0xfffffe0008cbea9c`) and
`AVC_PPS::pic_parameter_set_rbsp` (`0xfffffe0008cbf5c4`). Both binaries were
built from the same source, so **every offset in §3 and §4 below is readable
twice, and both readings agree.** That is the strongest evidence class docs/00
recognises.

---

## 1. Minimal `Start_AVC` for 1080p, fixed QP, I-frames only, one session, NV12

Offsets into `sCAveCmdAvcStart`. `bzero` the whole 0x3180 bytes first; anything
not listed here is **not required for first light** (§9 says why).

```c
/* ---- common header (docs/07 §4, docs/20 §0) --------------------------- */
0x0000 u16  id            = 6
0x0008 u64  cnt           = <sequence>
0x0010 u64  cid           = <client id from Open>
0x0018 u32  client_type   = 1
0x001C u32  enc_type      = 1                 /* 1 = AVC; fw branches on it at 0x31f0c */
0x0020 u32  slot          = 6
0x0024 u32  priority      = 200
0x0030 [16] timeout

/* ---- per-client firmware working buffers (docs/20 §3.4) --------------- */
0x0048 u64  buf0_iova     0x0050 u32 buf0_size
0x0058 u64  buf1_iova     0x0060 u32 buf1_size

/* ---- session algorithm config (docs/20 §3.3) -------------------------- */
0x0220 u32  sComm.FrameRate                = 30       /* MUST be > 0, §5.4 */
0x0234 int  sRC.RCMode                     = 3        /* const-QP; INFERRED */
0x0240 int  sRC.QP[I]                      = 26
0x0244 int  sRC.QP[P]                      = 26       /* set anyway, see §5.9 */
0x0248 int  sRC.QP[B]                      = 26
0x0298 int  sRC.RCQPRange.min              = 0
0x029C int  sRC.RCQPRange.max              = 51
0x02B8 int  sGOP.MaxKeyFrameInterval       = 1        /* every frame an IDR */
0x02BC int  sGOP.StrictKeyFrameInterval    = 1
0x02E0 int  sRef.ReferenceNum[0..2]        = 0        /* 0x2E0/0x2E4/0x2E8 */

/* ---- geometry -------------------------------------------------------- */
0x0368 u32  VideoParams.ui32Width          = 1920
0x036C u32  VideoParams.ui32Height         = 1088     /* MB-aligned; see §12 risk 1 */

/* ---- DPB / reconstruction pool: 16 surfaces, stride 0x20 ------------- */
0x0390 u64  sBufSet.saRecon[0][0].iAddr    = <recon IOVA 0>
0x03B0 u64  sBufSet.saRecon[1][0].iAddr    = <recon IOVA 1>
 ...        (up to saRecon[15] at 0x0570; 2 are enough for I-only, see §6)

/* ---- coded-data pool: 30 entries, stride 0x10 ------------------------ */
0x0D50 u64  sBufSet.saCodedData[0].iAddr   = <bitstream IOVA>
0x0D58 u32  sBufSet.saCodedData[0].iSize   = <bitstream bytes>
0x0F50 u64  p_apscoded_dataHeader[0]       = <coded-header IOVA>

/* ---- slice map ------------------------------------------------------- */
0x25D4 s32  VideoParams.sSliceMap.iNum     = 1        /* one slice per frame */

/* ---- input chroma ---------------------------------------------------- */
0x26DC u32  input_chroma_format            = 1        /* 4:2:0; INFERRED name */

/* ===== H264_SEQUENCE_HEADER_PARAMS  (SPSparams), 0x6B4 bytes ========== */
0x291C u32  eProfile                       = 6        /* -> profile_idc 100, High */
0x2920 u32  constraint_set0_flag           = 0
0x2924 u32  constraint_set1_flag           = 0
0x2928 u32  constraint_set2_flag           = 0
0x292C u32  constraint_set3_flag           = 0
0x2930 u32  constraint_set4_flag           = 0
0x2934 u32  constraint_set5_flag           = 0
0x2938 u32  eLevel                         = 12       /* -> level_idc 40 (4.0) */
0x293C u32  seq_parameter_set_id           = 0        /* MUST be 0, §5.5 */
0x2940 u32  chroma_format_idc              = 1        /* 4:2:0 */
0x2944 u32  separate_colour_plane_flag     = 0
0x2948 u32  bit_depth_luma_minus8          = 0
0x294C u32  bit_depth_chroma_minus8        = 0
0x2951 u8   qpprime_y_zero_transform_bypass_flag = 0
0x2958 u8   seq_scaling_matrix_present_flag= 0
0x2D34 u32  log2_max_frame_num_minus4      = 0
0x2D38 u32  pic_order_cnt_type             = 2        /* 2 = display order == coding order.
                                                        1 is unusable, §5.6 */
0x2D3C u32  log2_max_pic_order_cnt_lsb_minus4 = 0     /* unused when poc_type == 2 */
0x2D44 u32  max_num_ref_frames             = 1        /* see §8 risk */
0x2D48 u8   gaps_in_frame_num_value_allowed_flag = 0
0x2D4C u32  pic_width_in_mbs_minus1        = 119      /* 1920/16 - 1 */
0x2D50 u32  pic_height_in_map_units_minus1 = 67       /* 1088/16 - 1 */
0x2D54 u32  frame_mbs_only_flag            = 1
0x2D58 u32  direct_8x8_inference_flag      = 1
0x2D5C u8   vui_parameters_present_flag    = 0
0x2DB4 u8   frame_cropping_flag            = 1
0x2DB8 u32  frame_crop_left_offset         = 0
0x2DBC u32  frame_crop_right_offset        = 0
0x2DC0 u32  frame_crop_top_offset          = 0
0x2DC4 u32  frame_crop_bottom_offset       = 4        /* (1088-1080)/2, CropUnitY=2 */
0x2DC8 u8   bFWCreatesHeader               = 1        /* MUST equal PPS's, see §5.1 */
0x2DCC u32  SPS header_len                 = 0        /* MUST be 0 on input, §5.2 */

/* ===== H264_PICTURE_HEADER_PARAMS  (PPSParams), 0x180 bytes =========== */
0x2FD0 u32  pic_parameter_set_id           = 0        /* forced to 0 anyway, §5.5 */
0x2FD4 u32  seq_parameter_set_id           = 0        /* forced to 0 anyway, §5.5 */
0x2FD8 u32  entropy_coding_mode_flag       = 1        /* 1 CABAC, 0 CAVLC; §5.8 */
0x2FDC u8   bottom_field_pic_order_in_frame_present_flag = 0
0x2FE0 u32  num_slice_groups_minus1        = 0        /* MUST be 0, §5.7 */
0x2FE8 u32  num_ref_idx_l0_default_active_minus1 = 0
0x2FEC u32  num_ref_idx_l1_default_active_minus1 = 0
0x2FF0 u8   weighted_pred_flag             = 0
0x2FF4 u32  weighted_bipred_idc            = 0
0x2FF8 s32  pic_init_qp_minus26            = 0
0x2FFC s32  pic_init_qs_minus26            = 0
0x3000 s32  chroma_qp_index_offset[0..7]   = 0        /* 0x3000..0x301C */
0x3020 u8   deblocking_filter_control_present_flag = 1
0x3021 u8   constrained_intra_pred_flag    = 0
0x3022 u8   redundant_pic_cnt_present_flag = 0
0x3023 u8   transform_8x8_mode_flag        = 1        /* legal only for profile >= High */
0x3024 u8   pic_scaling_matrix_present_flag= 0
0x3028 s32  second_chroma_qp_index_offset[0..7] = 0   /* 0x3028..0x3044 */
0x3048 u8   bFWCreatesHeader               = 1        /* MUST equal SPS's, see §5.1 */

/* ---- execution-unit selection (docs/20 §3.4) ------------------------- */
0x3154 s32  EUInfo index                   /* + 0x24 bytes of _S_AVE_EUInfo */
```

`sCAveCmdAvcStart` is `0x3180` and this tiles it: `0x291C + 0x6B4 = 0x2FD0`,
`0x2FD0 + 0x180 = 0x3150`, then `0x3150` u8, `0x3154`+`0x28` = `0x317C`.
The command size check the firmware performs (docs/07) will catch any error
here loudly.

---

## 2. Where each byte came from — the field-name oracle

`CAVCController::DebugInit` (`0x63c40`, 0x21D4 bytes) sets
`x23 = this + 0x247C4` at `0x63d00` / `0x64780` (`add x8, x19, #0x24, lsl #12;
add x23, x8, #0x7c4`) and then prints, one log line per field. Every row in §3
and §4 is one `ldr`/`ldrb` off `x23` immediately followed by an `adrp/add` of
its format string. Because `this + 0x247C4` is a byte-for-byte `memcpy` of
`cmd + 0x291C` (§0), the SPS offset *is* the command offset minus `0x291C`.

The kext half is `AVC_SPS::seq_parameter_set_rbsp` (`0xfffffe0008cbea9c`),
which reads the same offsets in H.264 syntax order through
`AVE_SyntaxWriter::WriteBit` / `WriteUE` / `WriteUEE` (ue(v)) / `WriteSEE`
(se(v)). The firmware's copy is `AVC_SPS::seq_parameter_set_rbsp` at `0x41c8c`.
Two independent readings, no disagreement.

Two spot checks that the offsets are right and not an artefact of the method:

* `AVC_SPS::SetDefaultParams` (`0xfffffe0008cbe704`) writes
  `{1072, 1076, 1080, 1084} = {39, 29, 1, 1}` — i.e. 640×480, `frame_mbs_only`,
  `direct_8x8_inference`. Those are exactly the four fields §3 names at those
  offsets.
* PPS `+81` and `+83` are printed by `AVE_Client_Enc_PrintAVC` (a *different*
  function, in a *different* binary) as `ConstrainedIntra` and
  `transform_8x8_mode_flag` (`0xfffffe0008b7d674`, `0xfffffe0008b7d864`).

---

## 3. `H264_SEQUENCE_HEADER_PARAMS` — `cmd + 0x291C`, 0x6B4 bytes

Struct offset = cmd offset − `0x291C`. **All rows confirmed** unless noted.
"DebugInit VA" is the load; the format string follows within 8 instructions.

| cmd | struct | type | field | DebugInit VA | first light |
|---:|---:|---|---|---|---|
| `0x291C` | `+0` | u32 | `eProfile` (`_E_AVC_Profile`, **not** profile_idc) | `0x64d04` | **yes** |
| `0x2920` | `+4` | u32 | `constraint_set0_flag` | `0x64d5c` | yes (0) |
| `0x2924` | `+8` | u32 | `constraint_set1_flag` | `0x64d88` | yes (0) |
| `0x2928` | `+12` | u32 | `constraint_set2_flag` | `0x64db4` | yes (0) |
| `0x292C` | `+16` | u32 | `constraint_set3_flag` | `0x64de0` | yes (0) |
| `0x2930` | `+20` | u32 | `constraint_set4_flag` | `0x64e0c` | yes (0) |
| `0x2934` | `+24` | u32 | `constraint_set5_flag` | `0x64e38` | yes (0) |
| `0x2938` | `+28` | u32 | `eLevel` (`_E_AVC_Level`, **not** level_idc) | `0x64d30` | **yes** |
| `0x293C` | `+32` | u32 | `seq_parameter_set_id` (ue) | `0x64e64` | yes (0) |
| `0x2940` | `+36` | u32 | `chroma_format_idc` (ue) | `0x64e90` | **yes** (1) |
| `0x2944` | `+40` | u32 | `separate_colour_plane_flag` | `0x64ebc` | yes (0) |
| `0x2948` | `+44` | u32 | `bit_depth_luma_minus8` (ue) | `0x64ee8` | **yes** (0) |
| `0x294C` | `+48` | u32 | `bit_depth_chroma_minus8` (ue) | `0x64f14` | **yes** (0) |
| `0x2950` | `+52` | u8 | `LOSSLESS` — printer-only name, never serialised | kext `0xfffffe0008b7d57c` | no (0) |
| `0x2951` | `+53` | u8 | `qpprime_y_zero_transform_bypass_flag` | `0x64f40` | yes (0) |
| `0x2954` | `+56` | u32 | `scaling_matrix` | `0x64f6c` | no |
| `0x2958` | `+60` | u8 | `seq_scaling_matrix_present_flag` | `0x64f98` | yes (0) |
| `0x2959`+i | `+61`+i | u8[12] | `seq_scaling_list_present_flag[i]` | `0x64fc4`… | no (0) |
| `0x2965`+i | `+73`+i | u8[12] | `UseDefaultScalingMatrixFlag[i]` (kext `cbec40`) | — | no |
| `0x2972` | `+86` | u16[6][16] | `ScalingList4x4`, stride `0x20` (kext `cbec44`) | — | no |
| `0x2A32` | `+278` | u16[6][64] | `ScalingList8x8`, stride `0x80` (kext `cbec58`) | — | no |
| `0x2D34` | `+1048` | u32 | `log2_max_frame_num_minus4` (ue) | `0x65200` | **yes** |
| `0x2D38` | `+1052` | u32 | `pic_order_cnt_type` (ue) | `0x6522c` | **yes** |
| `0x2D3C` | `+1056` | u32 | `log2_max_pic_order_cnt_lsb_minus4` (poc_type 0) | `0x65258` | yes |
| `0x2D40` | `+1060` | u32 | `delta_pic_order_always_zero_flag` (poc_type 1) | kext `cbef88` | no |
| `0x2D44` | `+1064` | u32 | `max_num_ref_frames` (ue) | `0x65284` | **yes** |
| `0x2D48` | `+1068` | u8 | `gaps_in_frame_num_value_allowed_flag` | `0x652b0` | yes (0) |
| `0x2D4C` | `+1072` | u32 | `pic_width_in_mbs_minus1` (ue) | `0x652dc` | **yes** |
| `0x2D50` | `+1076` | u32 | `pic_height_in_map_units_minus1` (ue) | `0x65308` | **yes** |
| `0x2D54` | `+1080` | u32 | `frame_mbs_only_flag` | `0x65334` | **yes** (1) |
| `0x2D58` | `+1084` | u32 | `direct_8x8_inference_flag` | `0x65360` | yes (1) |
| `0x2D5C` | `+1088` | u8 | `VUI.vui_parameters_present_flag` | `0x653d4` | **yes** (0) |
| `0x2D5D` | `+1089` | u8 | `VUI.aspect_ratio_info_present_flag` | `0x65400` | must be 0 |
| `0x2D5E` | `+1090` | u8 | `VUI.overscan_info_present_flag` | `0x6542c` | must be 0 |
| `0x2D5F` | `+1091` | u8 | `VUI.video_signal_type_present_flag` | `0x65458` | no |
| `0x2D60` | `+1092` | u32 | `VUI.video_format` | `0x65484` | no |
| `0x2D64` | `+1096` | u32 | `VUI.video_full_range_flag` | `0x654b0` | no |
| `0x2D68` | `+1100` | u8 | `VUI.colour_description_present_flag` | `0x654dc` | no |
| `0x2D6C` | `+1104` | u32 | `VUI.colour_primaries` | `0x65508` | no |
| `0x2D70` | `+1108` | u32 | `VUI.transfer_characteristics` | `0x65534` | no |
| `0x2D74` | `+1112` | u32 | `VUI.matrix_coefficients` | `0x65560` | no |
| `0x2D78` | `+1116` | u8 | `chroma_loc_info_present_flag` (kext `cbf19c`) | — | no |
| `0x2D7C`/`0x2D80` | `+1120`/`+1124` | u32 | `chroma_sample_loc_type_top/bottom_field` | kext `cbf1bc`/`cbf1d0` | no |
| `0x2D84` | `+1128` | u8 | `VUI.timing_info_present_flag` | `0x6558c` | no |
| `0x2D88` | `+1132` | u32 | `VUI.num_units_in_tick` u(32) | `0x655b8` | no |
| `0x2D8C` | `+1136` | u32 | `VUI.time_scale` u(32) | `0x655e4` | no |
| `0x2D90` | `+1140` | u8 | `VUI.fixed_frame_rate_flag` | `0x65610` | no |
| `0x2D91` | `+1141` | u8 | `VUI.nal_hrd_parameters_present_flag` | `0x6563c` | must be 0 |
| `0x2D92` | `+1142` | u8 | `VUI.vcl_hrd_parameters_present_flag` | `0x65668` | must be 0 |
| `0x2D93` | `+1143` | u8 | `VUI.pic_struct_present_flag` | `0x65694` | no |
| `0x2D94` | `+1144` | u8 | `VUI.bitstream_restriction_flag` | `0x656c0` | no |
| `0x2D98`…`0x2DB0` | `+1148`…`+1172` | u32×7 | bitstream-restriction group (kext `cbf28c`…`cbf304`) | — | no |
| `0x2DB4` | `+1176` | u8 | `CropParams.frame_cropping_flag` | `0x65734` | **yes** |
| `0x2DB8` | `+1180` | u32 | `frame_crop_left_offset` (ue) | `0x65760` | yes (0) |
| `0x2DBC` | `+1184` | u32 | `frame_crop_right_offset` (ue) | `0x6578c` | yes (0) |
| `0x2DC0` | `+1188` | u32 | `frame_crop_top_offset` (ue) | `0x657b8` | yes (0) |
| `0x2DC4` | `+1192` | u32 | `frame_crop_bottom_offset` (ue) | `0x657e4` | **yes** (4) |
| `0x2DC8` | `+1196` | u8 | **`bFWCreatesHeader`** | fw `0x6dfa0`, §5.1 | **yes** (1) |
| `0x2DCC` | `+1200` | u32 | SPS header length **in bits** | `0x64cd8` | only if flag 0 |
| `0x2DD0` | `+1204` | u8[512] | SPS RBSP byte buffer | kext `cbe79c` | only if flag 0 |

`mb_adaptive_frame_field_flag` is **not in this struct** — the writer takes it
from the `AVC_SPS` object (`this+20`, kext `cbf010`), so it is unreachable from
the command and effectively 0.

### `_E_AVC_Profile` — the enum, confirmed from the table

`AVC_FindProfileIdc` (fw `0xe7148`) requires `1 <= eProfile <= 9`, then indexes
a 9-entry, 24-byte table at fw `0x138a98` (kext `0xfffffe0007ed9190`) and
returns `[entry + 4]`. Out of range logs and returns **0**, which would emit
`profile_idc = 0`.

| eProfile | profile_idc | name (string in the table) |
|---:|---:|---|
| 1 | 44 | CAVLC 4:4:4 Intra |
| 2 | 66 | Baseline |
| 3 | 67 | Constrained Baseline |
| 4 | 77 | Main |
| 5 | 88 | Extended |
| **6** | **100** | **High** |
| 7 | 110 | High 10 |
| 8 | 122 | High 4:2:2 |
| 9 | 244 | High 4:4:4 |

Two enum traps. `eProfile = 3` yields `profile_idc = 67`; real Constrained
Baseline is `profile_idc = 66` with `constraint_set1_flag = 1`, so treat that
enumerator as suspect. `eLevel = 2` (Level 1b) writes `level_idc = 1` verbatim
where the spec wants `level_idc = 11` plus `constraint_set3_flag = 1`. Neither
affects the recommended `{6, 12}` pair. **Confirmed** from the tables.

The SPS writer emits the high-profile syntax group (`chroma_format_idc`,
`bit_depth_*`, `qpprime`, scaling lists) only when the enum is in
`{1, 6, 7, 8, 9}` — fw `0x41d5c`–`0x41d68` (`sub w10, w9, #6; cmp w10, #4;
b.cs skip`), kext `cbeb5c`–`cbecac`. **Confirmed.** So with `eProfile = 4`
(Main) the `chroma_format_idc` / `bit_depth` fields are simply not written and
the decoder infers 4:2:0 8-bit — which is also fine, but High is the safer
default because it is what `SetDefaultParams` picks.

### `_E_AVC_Level` — the enum, confirmed from the table

`AVC_FindLevelIdc` (fw `0xe71dc`) requires `1 <= eLevel <= 20` and indexes a
20-entry, 36-byte table at fw `0x11d050` (kext `0xfffffe0007238f40`),
`level_idc` at `[entry + 4]`.

| eLevel | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| level_idc | 10 | 1 (=1b) | 11 | 12 | 13 | 20 | 21 | 22 | 30 | 31 |

| eLevel | 11 | **12** | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 |
|---|---|---|---|---|---|---|---|---|---|---|
| level_idc | 32 | **40** | 41 | 42 | 50 | 51 | 52 | 60 | 61 | 62 |

The rest of each entry is the level limit set. For `eLevel = 12` (Level 4.0) the
words are `MaxMBPS 0x3C000` (245760), `MaxFS 0x2000` (8192 MBs), `MaxDpbMbs
0x8000` (32768), `MaxBR 0x1312D00` (20 Mbit/s) — the H.264 Table A-1 values,
which is an independent confirmation the table is what it looks like. 1080p is
`120 × 68 = 8160 ≤ 8192` MBs, so **Level 4.0 fits**. **Confirmed.**

---

## 4. `H264_PICTURE_HEADER_PARAMS` — `cmd + 0x2FD0`, 0x180 bytes

Struct offset = cmd offset − `0x2FD0`. The DebugInit prints are off the same
`x23` (SPS base), so the DebugInit offsets here are `+1716 …` — that is how the
two structs are proven adjacent: `1716 = 0x6B4` is exactly the SPS size, and
`cmd + 0x291C + 0x6B4 = cmd + 0x2FD0` is exactly where the second `memcpy`
block starts. **Confirmed.**

| cmd | struct | type | field | DebugInit VA | first light |
|---:|---:|---|---|---|---|
| `0x2FD0` | `+0` | u32 | `pic_parameter_set_id` (ue) | `0x65884` | yes (0) |
| `0x2FD4` | `+4` | u32 | `seq_parameter_set_id` (ue) | `0x658b0` | yes (0) |
| `0x2FD8` | `+8` | u32 | **`entropy_coding_mode_flag`** | `0x658dc` | **yes** |
| `0x2FDC` | `+12` | u8 | `bottom_field_pic_order_in_frame_present_flag` | `0x65908` | yes (0) |
| `0x2FE0` | `+16` | u32 | `num_slice_groups_minus1` (ue) | `0x65934` | **yes** (0) |
| `0x2FE4` | `+20` | u32 | `slice_group_map_type` (only if groups ≠ 0) | kext `cbf760` | no |
| `0x2FE8` | `+24` | u32 | `num_ref_idx_l0_default_active_minus1` (ue) | `0x65960` | yes (0) |
| `0x2FEC` | `+28` | u32 | `num_ref_idx_l1_default_active_minus1` (ue) | `0x6598c` | yes (0) |
| `0x2FF0` | `+32` | u8 | `weighted_pred_flag` | `0x659b8` | yes (0) |
| `0x2FF4` | `+36` | u32 | `weighted_bipred_idc` u(2) | `0x659e4` | yes (0) |
| `0x2FF8` | `+40` | s32 | `pic_init_qp_minus26` (se) | `0x65a10` | **yes** |
| `0x2FFC` | `+44` | s32 | `pic_init_qs_minus26` (se) | `0x65a3c` | yes (0) |
| `0x3000`+4i | `+48`+4i | s32[8] | `chroma_qp_index_offset[i]` (se) | `0x65a68`… | yes (0) |
| `0x3020` | `+80` | u8 | `deblocking_filter_control_present_flag` | `0x65d60` | **yes** |
| `0x3021` | `+81` | u8 | `constrained_intra_pred_flag` | `0x65d8c` | yes (0) |
| `0x3022` | `+82` | u8 | `redundant_pic_cnt_present_flag` | `0x65db8` | yes (0) |
| `0x3023` | `+83` | u8 | `transform_8x8_mode_flag` | `0x65de4` | **yes** |
| `0x3024` | `+84` | u8 | `pic_scaling_matrix_present_flag` | `0x64608` | yes (0) |
| `0x3028`+4i | `+88`+4i | s32[8] | `second_chroma_qp_index_offset[i]` (se) | `0x65a94`… | yes (0) |
| `0x3048` | `+120` | u8 | **`bFWCreatesHeader`** | fw `0x6df9c`, §5.1 | **yes** (1) |
| `0x304C` | `+124` | u32 | PPS header length **in bits** | `0x65858` | only if flag 0 |
| `0x3050` | `+128` | u8[256] | PPS RBSP byte buffer | kext `cbfc68` | only if flag 0 |

PPS scaling lists live in the `AVC_PPS` object (kext `this+32/44/56`, fw
`0x42bc4` onward), not in this struct, and the constructor zeroes them
(`0x4276c`), so setting `pic_scaling_matrix_present_flag = 1` would emit an
all-zero present-flag set. Unreachable from the command; leave the flag at 0.

Only **index 0** of `chroma_qp_index_offset[]` and
`second_chroma_qp_index_offset[]` ever reaches the bitstream: the AVC call site
passes array index 0 to `pic_parameter_set_rbsp` (`0x6d80c` `mov w4, wzr`).
**Confirmed.** The other seven slots exist for multi-PPS sessions.

**`transform_8x8_mode_flag = 1` is only legal for `profile_idc >= 100`.** With
`eProfile = 6` that holds. If a driver drops to Main (`eProfile = 4`) it must
also clear `transform_8x8_mode_flag`, or the PPS will carry a syntax element
the decoder will not read — this is a real, silent stream corruption, not a
firmware error. (Standards requirement, not a firmware finding.)

### 4.1 The slice header is entirely firmware-generated

Worth stating because it removes a whole category of "what else must I set?".
`AVC_Slice::slice_header(const H264_SEQUENCE_HEADER_PARAMS&, const
H264_PICTURE_HEADER_PARAMS&, int)` (`0x433a0`) is fed a
`H264_SLICE_HEADER_PARAMS` that is **firmware-internal**: it is filled by
`AVE_H264_PrepareSliceHeader` (`0x50f94`) and `AVE_H264_Update_POClsb_SliceType`
(`0x50ef4`) from `sCommonParams` and the DPB, per picture. The host supplies
none of it. **Confirmed.**

`sCommonParams` is `CAVCController + 0xF58` — `CAVCController::PipePrepareParam`
calls `AVE_H264_PrepareSliceHeader` at `0x5882c` with `x0 = x19 + 0xF58`.
**Confirmed.** That makes `sCommonParams + 6120 = CAVCController + 10048`, which
is exactly where `InitEncodingParameters` stores `QP[I] + 6·bit_depth_luma_
minus8` (`0x6c0b4`), tying docs/32 §1's QP finding to the Start command
end to end.

Two consequences for the driver:

* The only Start-command values that reach the slice header are `QP[I/P/B]`
  (through `slice_qp_delta = QP − PPS.pic_init_qp_minus26 − 26`, computed at
  `0x5121c`–`0x51234` and recomputed after rate control at `0x59274`) and the
  SPS/PPS fields that set field widths (`log2_max_frame_num_minus4`,
  `log2_max_pic_order_cnt_lsb_minus4`) and `pic_init_qp_minus26`.
* The slice-header writer is constructed with **emulation prevention off**
  (`0x43320`), writes **no** start code and **no** NAL header byte, and skips
  `first_mb_in_slice`. The hardware entropy engine supplies those while
  interleaving slice data. So a driver must not expect the coded-data buffer to
  be assembled by the CPU. **Confirmed.**

`sCommonParams` is **not** a straight copy of the Start payload — do not try to
translate other `sCommonParams` offsets by a constant. `InitEncodingParameters`
is 11 KB of field-by-field copying with arithmetic (the QP triple is stored
*offset by the bit depth*, not copied). The SPS and PPS blocks are verbatim
`memcpy`s, which is exactly why those translate cleanly and nothing else does.

---

## 5. What the firmware actually validates

This section is the highest-value part of the document: each item is a hard
fact about a legal range, read out of a compare.

### 5.1 `SPS.bFWCreatesHeader` must equal `PPS.bFWCreatesHeader`

`CAVCController::InitEncodingParameters`, `0x6d7b0`–`0x6d7c0`:

```
6d7b0:  ldrb w8,  [x28, #1200]     ; x28 = this+0x247C0, so SPSparams + 1196
6d7b4:  ldrb w10, [x28, #1840]     ; SPSparams + 1836 = PPSparams + 120
6d7bc:  cmp  w8, w10
6d7c0:  b.ne 0x6df8c               ; log, then return -1001
```

The failure path logs `"%s::%s:%d invalid sps and pps bFWCreatesHeader %d %d"`
(file `0x12a20f`, VA `0x12620f`) with those two bytes as the two `%d`s, and
falls through to `mov w0, #0xfffffc17` = **-1001** at `0x6dfdc`. **Confirmed** —
this is both the offset and the name in one instruction pair. (The name is
independently corroborated by the HEVC dumper's `"SPSparams.bFWCreatesHeader
%d"` / `"PPSParams.bFWCreatesHeader %d"` strings, file `0x12d48f` / `0x12e265`.)

Semantics, **confirmed** from the branch that follows:

```
6d7c8:  cbz  w8, 0x6d87c           ; flag == 0 -> skip generation entirely
6d7cc:  add  x0, sp, #0x2d0
6d7d0:  add  x1, x28, #4           ; = SPSparams
6d7d8:  bl   0x41adc               ; AVC_SPS::AVC_SPS(H264_SEQUENCE_HEADER_PARAMS*)
6d7ec:  bl   0x4276c               ; AVC_PPS::AVC_PPS(H264_PICTURE_HEADER_PARAMS*)
6d7f8:  bl   0x41b24               ; AVC_SPS::Generate(_S_AVE_PSContext*)
6d81c:  bl   0x42f10               ; AVC_PPS::Generate(...)
```

so **`bFWCreatesHeader = 1` means the firmware serialises the SPS and PPS
itself from the two structs**, and `= 0` means the host must have supplied the
RBSP bytes and the bit lengths. For a driver, 1 is strictly less work and is
what `AVC_SPS::SetDefaultParams` (kext `0xfffffe0008cbe75c`) writes.

### 5.2 `SPS.header_len` must be zero on input → else **-1001**

`AVC_SPS::Generate` (`0x41b24`) constructs the bit writer over
`SPSparams + 0x4B4` with a 512-byte cap and emulation-prevention on
(`0x41b50`–`0x41b5c`), and then refuses to run if the length field is not clear:

```
41b68:  ldr  w8, [x8, #1200]      ; SPSparams + 1200 = header_len
41b6c:  cbz  w8, 0x41bcc          ; zero -> proceed to seq_parameter_set_rbsp
        ... log "%s::%s:%d %s | m_psParams->header_len(%d) %p" (0x123faa)
41bc4:  mov  w20, #0xfffffc17     ; -1001
```

**Confirmed.** A driver that memsets the command and never touches `0x2DCC`
is fine; one that "helpfully" fills in a length is not. `AVC_PPS::Generate`
(`0x42f10`) does **not** have this trap — it zeroes `PPSparams + 124` itself at
`0x42f9c` when its 5th argument is non-zero, and the AVC call site passes
`w5 = 1` (`0x6d818`). **Confirmed.**

### 5.3 Header size caps

Two separate caps, both **confirmed**:

* `AVC_PPS::pic_parameter_set_rbsp` returns **-1003** if the accumulated PPS
  length reaches `0x800` **bits** (`0x42ee4`–`0x42ef8`).
* `InitEncodingParameters` returns **-1019** if the two lengths together exceed
  `0x800` **bytes**:

```
6d820:  ldr w8, [x28, #1204]       ; SPSparams + 1200 = SPS header length (bits)
6d824:  ldr w9, [x28, #1844]       ; PPSparams + 124 = PPS header length (bits)
6d828:  add w8, w9, w8
6d82c:  lsr w8, w8, #3
6d830:  cmp w8, #0x800
6d834:  b.hi 0x6e0c0               ; -> mov w0, #0xfffffc05 = -1019
```

`AVE_PSInfo_Make` (`0xe8f64`) adds three more: `psType ∈ 1..3` (2 = SPS,
3 = PPS), the index argument `< 2`, `(bits & 7) == 0`, and the total
parameter-set payload `< 0x801` bytes (`0xe8fd8`, `0xe8fe4`, `0xe8fec`,
`0xe9198`). With `bFWCreatesHeader = 1` all of these are the generator's
problem, not the driver's.

### 5.4 `sComm.FrameRate` must be greater than zero

`CAVCController::InitFlatMbLowQPParams` (`0x6e504`, called from
`InitEncodingParameters` at `0x6d6b0`) panics:

```
6e690:  bl   0xe08fc               ; abort
6e6a4:  add  x19, x19, #0x2cf      ; "EncCommParams.sAlgCfg.sComm.iFrameRate > 0"
```

(string file `0x1262cf`, VA `0x1262cf`). **Confirmed.** `cmd + 0x220` must be
non-zero. `CAVECommonController::GetFrameType` also divides the per-frame
bitrate by it (`0x73610`), so zero would be a divide-by-zero later even without
the assert.

### 5.5 Parameter-set ids

`InitEncodingParameters` calls `AVC_PPS::Generate` with `w1 = 0, w2 = 0`
(`0x6d800`–`0x6d804`), and `Generate` stores those into
`PPSParams.seq_parameter_set_id` and `PPSParams.pic_parameter_set_id`
(`0x43000`, `0x43010`). So the PPS always references SPS **0** and always calls
itself PPS **0**, whatever the host wrote.

`SPS.seq_parameter_set_id` (`cmd + 0x293C`) is **not** overridden — it goes
straight to `WriteUE` at `0x41d50`. **So it must be set to 0**, or the PPS will
reference a sequence parameter set that does not exist and no decoder will
start. **Confirmed**, and this is a genuine foot-gun: the kext's
`AVC_SPS::SetDefaultParams` sets it to **1**.

### 5.6 `pic_order_cnt_type = 1` is unusable

`AVC_SPS::AVC_SPS` (`0x41adc`) zeroes the object fields the writer uses for
`offset_for_non_ref_pic`, `offset_for_top_to_bottom_field`,
`num_ref_frames_in_pic_order_cnt_cycle` and `offset_for_ref_frame[]`
(`stp wzr,wzr,[x19,#8]`/`[x19,#16]`; read back at `0x42374`–`0x423d8`), and
those fields are **not** in `H264_SEQUENCE_HEADER_PARAMS`. `mb_adaptive_frame_
field_flag` is the same (`0x423fc`). **Confirmed.** Use `pic_order_cnt_type`
0 or 2; 2 is right for I-only. The writer additionally errors `-1001` if
`num_ref_frames_in_pic_order_cnt_cycle > 256` (`0x423a8`) — unreachable from
the host.

### 5.7 `num_slice_groups_minus1` — a fatal combination

`AVC_Slice::slice_header` (`0x433a0`) with `x19 = H264_PICTURE_HEADER_PARAMS*`:

```
43750:  ldr w8, [x19, #16]         ; num_slice_groups_minus1
43754:  cbz w8, 0x437fc            ; 0 -> fine
43758:  ldr w8, [x19, #20]         ; slice_group_map_type
4375c:  sub w8, w8, #0x3
43760:  cmp w8, #0x2
43764:  b.hi 0x437fc               ; map type outside 3..5 -> fine
43768:  bl  0xe08fc                ; abort, then log + spin
```

**Confirmed.** `num_slice_groups_minus1 != 0` with `slice_group_map_type ∈
{3,4,5}` is a firmware abort, not an error return. Keep `cmd + 0x2FE0` at 0.

### 5.8 `entropy_coding_mode_flag` must be 0 or 1

`CAVCController::ProcessRateControlAccumulateBits` (`0x7075c`) reads the flag
and dispatches; any value other than 0 or 1 logs
`"wrong entropy_coding_mode_flag"` (file `0x12b9af`, VA `0x1279af`, referenced
at `0x70828` and `0x70878`) and falls back to 0 or 1 depending on the branch.
**Confirmed as a range check.** It does not abort the session, but a stream
built on the fallback would disagree with the emitted PPS.

### 5.9 `RCMode` is checked, and QP is bit-depth-shifted

`CRateControl::Check_RCMode` (`0xf830`) is documented in docs/20 §3.3: it
*rejects* 20 and 100 and accepts everything else. `RCMode = 3` for constant QP
remains **inferred**.

New here, and confirmed: `InitEncodingParameters` does

```
6c07c:  ldr w9, [x28, #48]         ; SPSparams + 44 = bit_depth_luma_minus8
6c08c:  add w9, w9, w9, lsl #1     ; *3
6c0a4:  lsl w8, w9, #1             ; *2  -> 6 * bit_depth_luma_minus8
6c0ac:  ldr w9, [x24, #472] ; + w8 -> [x20,#10048]     QP[I]  (cmd 0x240)
6c0b8:  ldr w9, [x24, #476] ; + w8 -> [x20,#10052]     QP[P]  (cmd 0x244)
6c0c4:  ldr w9, [x24, #480] ; + w8 -> [x20,#10056]     QP[B]  (cmd 0x248)
6c0fc:  ldr w9, [x24, #560] ; + w8 -> [x25,#1792]      RCQPRange.min (cmd 0x298)
6c108:  ldr w9, [x24, #564] ; + w8 -> [x25,#1796]      RCQPRange.max (cmd 0x29C)
```

So the QPs the driver writes are **8-bit-domain QPs (0..51)** and the firmware
offsets them by `6 * bit_depth_luma_minus8` internally. With
`bit_depth_luma_minus8 = 0` the shift is zero. This is an independent
confirmation of the `0x240/0x244/0x248` and `0x298/0x29C` offsets from docs/20
§3.3, read out of a completely different function. **Confirmed.**

### 5.10 `sSliceMap.iNum`

`AVE_CreateSliceMapRows(const _S_AVE_SliceMap*, _E_AVE_DevCap_Type, u8*)`
(`0x721d4`) is called from `InitEncodingParameters` at `0x6e014` with
`x0 = x24 + 0x256C` = **`cmd + 0x25D4`**, `w1 = 4`, `x2 = this + 0x2880`. It
asserts `(psSliceMap != NULL) && (0 <= eDevCapType < AVE_DevCap_Type_Max) &&
(piSliceMapRows != NULL)` (file `0x12baf8`) and returns immediately when
`[psSliceMap] < 2` (`0x7220c` `ldr w8,[x19]; cmp w8,#2; b.lt`). **Confirmed.**

The `CHEVCController` asserts `pInVideoParams->sSliceMap.iNum >= 1` (file
`0x13058a`, VA `0x12c58a`, referenced from `0xa2284`), which is where the field
name comes from. That assert is on the HEVC path; the AVC path does not
duplicate it. **Confirmed that the name is `VideoParams.sSliceMap.iNum`;
inferred that the AVC path wants the same `>= 1`.**

`cmd + 0x25D4` is `VideoParams + 0x226C`, and `ProcessCmd_Start` separately
copies 260 (`0x104`) bytes from `cmd + 0x25D4` into `client + 0xB4`
(`0x326b0`–`0x326c4`), so the struct is `0x104` bytes: `{ s32 iNum; entry[] }`
with 8-byte entries starting at `+8` whose first word is a row count
(`0x72264`, `0x72280`). **Struct size confirmed; the entry layout inferred.**

`iNum = 1` gives one slice per frame, and the derived slice count the encoder
reports is then `map[height_in_mbs - 1] + 1 = 1` (`0x5877c`, `0xb3fc8`).

### 5.11 Width and height are *not* range-rejected at Start

docs/20 §3.4 recorded `cmd[0x2708] == 1` as gating "the width ≤ 4096 path".
Reading it wide (`ProcessCmd_Start` `0x326c8`–`0x327fc`) shows it is **not a
rejection**: it computes a boolean

```
326e4:  ldr w8, [x20, #872]        ; cmd + 0x368 = width
326e8:  cmp w8, #0x1, lsl #12
326ec:  b.ls 0x327f0
326f0:  mov w8, #1
327f0:  ldr w8, [x20, #876]        ; cmd + 0x36C = height
327f4:  cmp w8, #0x1, lsl #12
327f8:  cset w8, hi
326fc:  strb w8, [x23, #1]         ; client + 1
```

i.e. `client[1] = (width > 4096 || height > 4096)`, gated on
`cmd[0x2708] == 1 && AVE_KeyFrame::GetInterval() == 1`. It is a
"large frame" flag, not a validator. **Confirmed** — and it means a wrong
width/height will not be caught at Start.

`ProcessCmd_Start` in fact contains **no range check on any payload field**.
The only aborts in it are two null-pointer asserts (`0x31f60`, `0x31ff4`, both
`bl 0xe08fc`). *(Negative-result discipline, docs/00 trap 2: the same scan over
`InitEncodingParameters` returns the four aborts in §5.1/§5.2 and the asserts at
`0x6e1e8`/`0x6e284`/`0x6e318`/`0x6e3ac`, so the test does discriminate.)*

---

## 6. Buffer pools declared at Start, and what later references them

These are inside `VideoParams`, so they were previously covered only by the
block map. All three arrays are copied straight into the controller by
`InitEncodingParameters`, and `DebugInit` names the destinations.

| cmd | count × stride | firmware destination | DebugInit name | copy VA |
|---|---|---|---|---|
| `0x0390` | 16 × `0x20` | `this + 7024` + 16·i | `sBufSet.saRecon[i][0].iAddr` (`0x64790`) | `0x6cb10`–`0x6cb8c` |
| `0x0D50` | 30 × `0x10` | `this + 6424` + 8·i | `sBufSet.saCodedData[i].iAddr` (`0x63d9c`) | `0x6c830`… |
| `0x0D58` | 30 × `0x10` | `this + 6664` + 4·i | `sBufSet.saCodedData[i].iSize` (`0x63dc4`) | `0x6c840`… |
| `0x0F50` | 30 × `0x10` | `this + 6784` + 8·i | `p_apscoded_dataHeader[i]` (`0x63dec`) | `0x6c850`… |

The element counts are read from `DebugInit`'s loop bounds (`cmp x24, #0x1e` at
`0x63d60` for the coded arrays; sixteen unrolled prints for `saRecon`). The
`0x20` stride of `saRecon` versus the `0x10` of `saCodedData` says each
`saRecon[i]` holds two `saIBuf`-style leaves, matching the
`{u64 iAddr; u32 iSize; u32 pad}` shape docs/32 §6.3 established elsewhere —
**inferred**, only `[i][0].iAddr` is read by name.

`0x0390` is `VideoParams + 0x28`, i.e. exactly the `_S_AVE_Buf_Set*` that
`AVE_CHM_SetFwBuf` is handed as its 4th argument (docs/20 §3.1). That guess is
now **confirmed**, and the first member of `_S_AVE_Buf_Set` is `saRecon[16]`.

### The `sOutput.Coded` assert, resolved

docs/32 §6.4 recorded that the firmware asserts
`pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]`. The
right-hand side is now pinned. `CAVCController::ProcessTranscodeStart`
(`0x6870c`), with `x22 = AVE_PICMGMT_PARAMS*` and `x23 = x22 + 0x4000`:

```
68910:  ldr  w8, [x23, #3828]      ; PICMGMT + 0x4EF4 = index
68914:  add  x9, x20, x8, lsl #2
68918:  ldr  w9, [x9, #6664]       ; saCodedData[index].iSize -> used as the size
68920:  add  x9, x20, x8, lsl #3
68924:  ldr  x8, [x22, #20216]     ; PICMGMT + 0x4EF8 = sOutput.Coded
68928:  ldr  x9, [x9, #6424]       ; saCodedData[index].iAddr
6892c:  cmp  x8, x9
68930:  b.ne 0x68a5c               ; assert failure
```

So per frame: **`sOutput.Coded` must equal `sCAveCmdAvcStart[0x0D50 + 16·n]`**
where `n = PICMGMT + 0x4EF4`, and the buffer size the firmware uses is
`sCAveCmdAvcStart[0x0D58 + 16·n]`. **Confirmed.** The alternate path
(`PICMGMT + 0x4EF0` bit 0 set) skips the identity check and instead requires
`sOutput.CodedBufSize > (3·W·H)/4` — docs/32 §6.4 already had that.

The corresponding statement for `sRecon` — that the per-frame recon planes must
come out of `saRecon[]` — is **inferred by symmetry**; no equivalent compare was
found. Do not rely on it being enforced; rely on it being required.

---

## 7. `_S_AVE_PSContext` — `cmd + 0x27C8`, 0x154 bytes of a 0x954 struct

The 0x154-byte block docs/20 §3.1 listed as "`memcpy` from `client + 0x417C`"
is the **parameter-set descriptor table**, the output side of the header
generator: `AVC_SPS::Generate(_S_AVE_PSContext*)` (`0x41b24`) and
`AVC_PPS::Generate(…, _S_AVE_PSContext*)` (`0x42f10`) both take one, and the AVC
call site passes its own copy at `this + 0xFAC` (`0x6d7d4`). The finished
context is `memcpy`'d to the hardware by
`CAVECommonController::SetPSContext(const _S_AVE_PSContext*, u64)` (`0x782a0`),
which moves `0x954` bytes to `hwbase + 0xB150` (`0x78338`).

Layout, from `AVE_PSInfo_Make(const u8*, _E_AVE_PSType, int, int,
_S_AVE_PSContext*)` (kext `0xfffffe0008c491f0`, fw `0xe8f64`):

```c
struct _S_AVE_PSContext {              /* 0x954 total */
    /* 0x000 */ uint32_t iNum;                 /* descriptor count */
    /* 0x004 */ struct { uint32_t eType;       /* _E_AVE_PSType, checked 1..3
                                                  (2 = SPS, 3 = PPS) */
                         uint32_t iLayerID;    /* checked < 2 */
                         uint32_t iOffset;     /* bytes  -- inferred */
                         uint32_t iSize; }     /* bytes  -- inferred */
                saBuf[21];                     /* ends at 0x154 */
    /* 0x154 */ uint8_t  iaPSData[2048];       /* the SPS/PPS NAL bytes */
};
```

`4 + 21*16 = 340 = 0x154` and `0x154 + 0x800 = 0x954`. **The Start command
carries only the first `0x154`** — the descriptor table, not the payload. That
resolves docs/20 §4.1's open question: `sCAveCmdAvcProcess + 0x48` is a `0x954`
block from the *same* source (`client + 0x417C`), i.e. **the Process command
carries the whole `_S_AVE_PSContext` including the parameter-set NAL bytes**,
and Start carries the header of it. `0x954 = 0x154 + 0x800` is the arithmetic
that makes docs/20 §4.1's "0x954-byte per-picture parameter block, Start sends a
prefix" concrete. **Inferred** from the two sizes and the shared source; the
`Process + 0x48` consumer was not disassembled.

The `iOffset`/`iSize` names come from the assert
`(psPSInfo->saBuf[iNum].sBuf.iOffset + psPSInfo->saBuf[iNum].sBuf.iSize) <=
(int32_t)sizeof(psPSContext->iaPSData)` (file `0x136615`) and from the
running-offset computation `ldp w8,w9,[x8,#-8]; add w8,w8,w9` (kext
`0xfffffe0008c49568`); the *field-to-offset* binding is **inferred**, the size
arithmetic is confirmed.

**Not required for first light**: with `bFWCreatesHeader = 1` the firmware
builds its own context from scratch. Leave `cmd + 0x27C8 … 0x291B` zero. The
firmware logs `"%s::%s:%d PSInfo num %d"` (`0x6d868`) and `"no PSInfo"`
(file `0x12bf1b`) around it.

---

## 8. `_S_AVE_Session_PFCfg` — located, and it is not in this command

The roadmap lists this as unmapped. It is now placed: **`_S_AVE_Session_PFCfg`
is `AVE_PICMGMT_PARAMS + 0x0000 .. 0x1737`** — the *per-frame* head that lives
at `sCAveCmdAvcProcess + 0x12C0`, which docs/32 §4 already partly mapped without
having a name for it. The "Session" in the name means "this session's per-frame
config"; it is the per-frame sibling of the `_S_AVE_*_Cfg` family docs/20 §3.2
mapped into the Start command. **It is not in `sCAveCmdAvcStart` at all.**

Four independent confirmations:

1. `CAVCController::PipePrepareParam` stores the `AVE_PICMGMT_PARAMS*` into the
   `_S_AVE_Session_PFCfg*` slot: `0x586b8` `mov w9,#0x37d8` / `add x9,x19,x9` /
   `0x586c4` `str x21,[x9,x8,lsl#3]`, where `x21` is provably the PICMGMT
   pointer (it is dereferenced at `+20328` = `sFrameInfo.FrameNum` @ `0x4F68`
   and `+20392` = fps @ `0x4FA8`, both from docs/32 §3). `CAVCController::
   setLRME` does the same at `0xb3f64`.
2. `CAVCController::ManageDPB` loads argument 4 of `ManageDPBBuffer` out of that
   array (`0x59588` `ldr x4,[x11,#14296]`) and passes the current PICMGMT as
   argument 8 (`0x595c4`).
3. The three offsets `ManageDPBBuffer` reads off `x4` — `0x000` (`0x1b17c`),
   `0x360` (`0x1b188`), `0x3A0` (`0x1b174`) — are exactly the offsets docs/32
   §4 had already named as the firmware-read fields of the PICMGMT head.
4. `CAVECommonController::GetThroughputMode` (`0x813d8`) dereferences the same
   array and reads `+960` = `0x3C0` = `eThroughputMode`, which docs/32 §4 named
   from the host-side log.

Host side: `AVE_CHM_SetDataInfo_RC` does `memcpy(PICMGMT+0, FrameInfo+0x5CB8,
0x1738)` (`0xfffffe0008b69184`), and `sRCUpdateData` starts at PICMGMT `0x1738`
— so **`sizeof(_S_AVE_Session_PFCfg) = 0x1738`**, and only its first `0x3D0`
bytes ever reach the AVC firmware (docs/32 §0). **Confirmed.**

### Layout, for whoever needs it later

`_S_AVE_Alg_PFCfg` sits at `+0x348`. The anchor is `_S_AVE_GOP_PFCfg` at
`_S_AVE_FrameInfo + 0x6050` = PFCfg `+0x398` (three consumers agree:
`AVE_MD_SVE::DispatchCmd` `0xfffffe0008c8f1a0`, `AVE_LAGOP::GOPDecision`
`0xfffffe0008c347e8`, `AVE_MD::ProcessInputCmd_Process` `0xfffffe0008b4d370`),
and `AVE_Alg_PrintPFCfg` (`0xfffffe0008cd41d4`) puts GOP at `Alg + 0x50`.

| PFCfg off | member | status |
|---:|---|---|
| `0x000` | `u64` session feature word; firmware reads bits 0 and 1 | confirmed |
| `0x348` | `_S_AVE_Alg_PFCfg` | inferred from the `+0x50` anchor |
| `0x350` | `int FrameRate` | confirmed (`"%p FrameRate: %d"`) |
| `0x358` | `_S_AVE_RC_PFCfg`: `+0 u64 OpFlag`, `+8 int Bitrate`, `+0xC int QP`, `+0x10 int DRL`, `+0x18..+0x30` 4×double, `+0x38 double CRFScale` | confirmed (`AVE_RC_PrintPFCfg` `0xfffffe0008bffe98`) |
| `0x398` | `_S_AVE_GOP_PFCfg`: `+0 u32 OpFlag`, `+4 int GOPSwitch` | confirmed |
| `0x3A0` | `_S_AVE_Ref_PFCfg`: `+0 u32 OpFlag` (firmware reads bit 1) | confirmed |
| `0x3A4` | `_S_AVE_QPMod_PFCfg`: `OpFlag`, `+4 Feature` | confirmed |
| `0x3AC` | `_S_AVE_LambdaMod_PFCfg`: `OpFlag`, `+4 Feature` | confirmed |
| `0x3B4` | `_S_AVE_ModeDec_PFCfg`: `OpFlag` | confirmed |
| `0x3C0` | `int eThroughputMode` | confirmed (docs/32 §4) |
| `0x3D0` | `_S_AVE_GGM_PFCfg` — mode enum, host checks `∈ {1,2}` | confirmed offset, fields unnamed |

None of the `*_PrintPFCfg` functions has a caller anywhere in the kext (a `bl`
scan over `__TEXT_EXEC` finds none), which is why this family was never reached
from `AVE_Client_Enc_PrintAll` the way the `_Cfg` family was.

### Verdict: leave it zero

The AVC firmware reads exactly five things out of these 5944 bytes:

| PFCfg off | field | read at |
|---:|---|---|
| `0x000` bits 0,1 | session feature word | `0x73634`, `0x1b17c`, `0xb39b0` |
| `0x360` | `sAlg.sRC.Bitrate` | `0x73610` (divided by the session frame rate), `0x1b188`, `0xb399c` |
| `0x3A0` bit 1 | `sAlg.sRef.OpFlag` | `0x73624`, `0x1b174`, `0xb3994` |
| `0x3AA` bit 0 | byte 2 of `sAlg.sQPMod.Feature` | `CAVCController::setPipe` `0xb73f8` |
| `0x3C0` | `eThroughputMode` | `0x81404`; falls back to the session value at `0x81434` when 0 |

All-zero is not merely tolerated, it is the safest value: both feature bits off,
the Ref bit off, `setPipe` on its plain path, and `GetThroughputMode` falling
back to the Start-time session setting. No range check or assert reads any of
them. **`sAlg.sRC.QP` at `0x364` has no observed firmware read at all** — a
per-frame QP field exists in the wire block, and the AVC firmware ignores it,
which sharpens docs/32 §1's "per-frame QP does not exist" into "it exists and is
dead".

---

## 9. Explicitly *not* required for first light

Stated so the next person does not spend a day on them.

* **All VUI** (`0x2D5C`–`0x2DB0`). Set `vui_parameters_present_flag = 0` and the
  whole group is skipped. A player will assume unspecified colour and no timing;
  `ffmpeg` parses it silently.
* **All scaling lists** (`0x2958`–`0x2D33`, and PPS `0x3024`). Flat default
  matrices are what you get with the present flags at 0.
* **HRD** (`0x2D91`, `0x2D92`). The writer emits the flags but **never emits an
  `hrd_parameters()` body** — control goes straight from the flag write to the
  next field (kext `cbf248` → `cbf258`). Setting either to 1 produces a
  malformed SPS. **Must be 0.** Same for `aspect_ratio_info_present_flag`
  (`0x2D5D`) and `overscan_info_present_flag` (`0x2D5E`): the bits are written,
  the payloads are not.
* **`_S_AVE_PSContext`** (`0x27C8`) — §7. Leave zero; note in particular that
  `SPS.header_len` (`0x2DCC`) must stay zero or `AVC_SPS::Generate` refuses
  (§5.2).
* **`_S_AVE_Session_PFCfg`** — it is not in this command at all; it is the head
  of the per-frame `AVE_PICMGMT_PARAMS`, and all-zero is the correct value there
  too. §8.
* **The slice header.** Entirely firmware-generated (§4.1). There is nothing to
  set and no buffer to provide.
* **`sQPMod`** (`0x300`–`0x367`), `sRef.ReferenceGap[]`, `sAlgCfg.sComm.Feature`
  bits other than bit 1 (frame drop), `SEIFeature`, `VUIFeature`.
* **The `0x68`–`0x207` head** and the bulk of `VideoParams`. `InitEncodingParameters`
  reads exactly 46 distinct scalars out of the payload below `0x2588`; every one
  that matters for a fixed-QP I-only encode is in §1.
* **`sCAveCmdAvcStart` `0x2588`–`0x2704`** apart from `sSliceMap` (`0x25D4`) and
  `input_chroma_format` (`0x26DC`).
* **`SPS.LOSSLESS`** (`0x2950`). It is printer-only; it is never serialised, and
  it is a *different byte* from `qpprime_y_zero_transform_bypass_flag`
  (`0x2951`). Do not conflate them.

---

## 10. Corrections and sharpenings to earlier documents

* **docs/20 §3.3** listed `cmd+0x291C` as "profile", `0x2938` "level",
  `0x2D44` "max_num_ref_frames", `0x2FD8` "entropy mode". All four are right,
  and all four are now placed in a fully named struct — §3, §4.
* **docs/20 §3.3** did not say that `profile` and `level` are **enums, not
  `profile_idc`/`level_idc`**. `eProfile = 100` would be out of range and
  `AVC_FindProfileIdc` would return 0. This is the single most likely way to
  write a driver that produces an unparseable stream. §3.
* **docs/20 §3.1** called `cmd + 0x390` an `_S_AVE_Buf_Set` "internal layout not
  read". Its first member is `saRecon[16]`, stride `0x20`. §6.
* **docs/20 §3.4** described `cmd[0x2708]` as gating "the width ≤ 4096 path",
  which reads as validation. It is a large-frame boolean; there is no width or
  height validation at Start at all. §5.11.
* **docs/32 §6.4** left `EncCommParams.bitstream_addr_dst[index]` unlocated. It
  is `sCAveCmdAvcStart + 0x0D50 + 16·index`. §6.
* **docs/04 roadmap** lists `_S_AVE_Session_PFCfg` as the unmapped struct to
  chase. It is `AVE_PICMGMT_PARAMS + 0x0000 .. 0x1737`, i.e. the per-frame head
  docs/32 §4 had already partly mapped — not part of `sCAveCmdAvcStart` at all.
  §8.
* **docs/32 §1** says "per-frame QP does not exist". Sharper: it *does* exist,
  at `_S_AVE_Session_PFCfg + 0x364` = `AVE_PICMGMT_PARAMS + 0x364`, and the AVC
  firmware never reads it. The operational conclusion (QP is session-scoped) is
  unchanged. §8.
* **docs/20 §4.1** described `sCAveCmdAvcProcess + 0x48` (0x954 bytes) as "a
  per-picture parameter block of which Start sends a prefix". It is
  `_S_AVE_PSContext`: a `0x154` descriptor table plus a `0x800` parameter-set
  NAL payload. §7.
* **docs/32 §1** lists `sOutput.Coded` as "must equal the Start-time
  CodedData[slot] IOVA". The array is now at a known command offset and the
  index is `AVE_PICMGMT_PARAMS + 0x4EF4`, not the command slot. §6.

---

## 11. Still unknown

* `cmd + 0x26DC` as `input_chroma_format`: the AVC path stores it to the same
  controller field (`x25 + 1828`, `0x6c24c`) that `COFController::
  InitEncodingParameters` stores its `input_chroma_format` to before asserting
  `EncCommParams.chroma_format <= EncCommParams.input_chroma_format` (file
  `0x12ef22`, assert at `0x87dbc`–`0x87dc4`). That is a name by analogy across
  two controllers, so **inferred**, not read. Compounding the doubt: the AVC
  path *overwrites* `x25 + 1828` at `0x6c378` with `SPSparams.chroma_format_idc`.
* **The NV12 / pixel-format selector is not located.** Neither the SPS block, the
  PPS block, nor `_S_AVE_PSContext` contains one; `chroma_format_idc` describes
  the *coded* format, not the memory layout. The per-frame `bInputCompressed`
  (docs/32 §5) covers AGX tiling but not plane order. Candidates are the
  `0x68`–`0x207` head and the unmapped bulk of `VideoParams`; the kext's
  `AVE_Client_Enc_PrintAVC` prints `input_compress` from `cmd + 0x25A4`
  (`0xfffffe0008b7dd20`), which is the nearest thing found.
* Stride / plane-layout fields: not found in the Start command. `0x6d904`–
  `0x6d94c` derives DMA strides arithmetically from width, height and
  `pic_width_in_mbs_minus1`, so there may be none to set.
* Whether `width`/`height` at `0x368`/`0x36C` are display or MB-aligned
  dimensions — see §12.
* `cmd + 0x2588`, `0x2598`, `0x25A8`–`0x25CC`, `0x26E4`, `0x2704`, `0x0370`
  (read as a `u64` at `0x6c1e4`, though docs/20 lists a `u8` firmware-RC flag at
  the same offset), `0x0378`, `0x0380`–`0x0388`, `0x0338`/`0x033C`,
  `0x0308`/`0x030C`, `0x0310`, `0x0090`/`0x0094`, `0x0079`/`0x007C` — read by
  `InitEncodingParameters` but not named.
* HEVC: `sCAveCmdHevcStart` has the same shape (a `SPSparams`/`PPSParams` pair
  dumped by `CHEVCController::DebugInit`, strings at file `0x12cd5d` onward) and
  is one disassembly session away by the same method.

---

## 12. Proposed hardware experiments (do not run — for the operator)

All three are cheap and all three discriminate.

1. **Height ambiguity.** The one genuinely uncertain value in §1 is
   `VideoParams.ui32Height`. The kext's own trace computes
   `nmb = (width >> 4) * (height >> 4)` (`0xfffffe0008b7d464`–`78`), which is
   only correct if `height` is already a multiple of 16 — so 1088 with
   `frame_crop_bottom_offset = 4`. But nothing cross-checks `height` against
   `pic_height_in_map_units_minus1`, so 1080 with the same crop would also be
   accepted and would silently under-read the source buffer by 8 rows. Submit
   the §1 command once with `height = 1088` and once with `1080`, keeping
   everything else identical, and compare the emitted SPS: the correct one gives
   `pic_height_in_map_units_minus1 = 67` and a decoded 1920×1080.
2. **`bFWCreatesHeader` and the parameter-set ids.** The two most dangerous
   values in §1 are `SPS.seq_parameter_set_id` (the kext's own
   `SetDefaultParams` uses **1**, and the PPS is force-set to reference SPS
   **0** — §5.5) and `SPS.header_len` (must be 0 on input — §5.2). Both fail
   loudly: the first gives a stream `ffmpeg` refuses, the second gives Start
   status **-1001**. Worth submitting one deliberately-wrong Start of each kind
   to confirm the error codes surface to the host at all, before debugging a
   silent failure.
3. **Header round-trip.** With `bFWCreatesHeader = 1`, the firmware writes the
   SPS/PPS bit lengths back to its own copy and logs
   `"%s::%s:%d PSInfo num %d"` (`0x6d868`) and, in `DebugInit`,
   `"SPS header length:%d"` / `"PPS header length:%d"` (`0x64cd8`, `0x65858`).
   A single Start with logging at level 8 for subsystem `0x8c` reports both
   lengths and the full `SPSparams.*` / `PPSParams.*` dump, which validates §3
   and §4 end to end without needing a frame to be encoded. That is the cheapest
   possible confirmation of this entire document.


---

## Verification pass (independent re-derivation)

| Claim | Evidence | Verdict |
|---|---|---|
| SPS block is at `Start + 0x291C`, `0x6B4` bytes | `0x6c024: mov w2,#0x6b4`; src `0x6c054: add x1, x24, #0x28b4` with `x24 = cmd + 0x68` -> `0x68 + 0x28B4 = 0x291C` | confirmed |
| ...copied to controller `+0x247C4` | `0x6c010: add x9, x20, #0x24, lsl #12` / `add x28, x9, #0x7c0` / `0x6c020: add x0, x28, #0x4` | confirmed |
| PPS block is at `Start + 0x2FD0`, `0x180` bytes | `0x6c070: mov w2,#0x180`; src `add x1, x24, #0x2f68` -> `0x68 + 0x2F68 = 0x2FD0` | confirmed |
| ...copied to controller `+0x24E78` | `0x6c05c: mov w9,#0x4e78` + `movk #0x2,lsl#16` | confirmed |
| `SPS.header_len` must be 0 on input | `0x41b68: ldr w8,[x8,#1200]` / `cbz w8, 0x41bcc`; the fall-through logs at subsystem 195 level 4 and takes the error path | confirmed |
| `bFWCreatesHeader` must match between SPS and PPS | `0x6d7b0/0x6d7b4: ldrb` from `+1200` and `+1840` / `0x6d7bc: cmp w8,w10` / `b.ne 0x6df8c` | confirmed |

The `x24 = cmd + 0x68` premise is not assumed here - it is *validated* by the
result: two independent source offsets both land exactly on the claimed block
addresses under the same bias. A wrong bias would have to be wrong by the same
amount twice to produce that.

### Correction owed to [32](32-picmgmt-params.md) §1

Doc 32 states the per-frame block "carries no QP field that was found". The
sharper statement is that a per-frame QP field **exists** at
`AVE_PICMGMT_PARAMS + 0x364` - inside copied slice 0, so it does reach the
firmware - and the AVC path **never reads it**. *(Field location and the
negative are this agent's reading; not independently re-derived here.)*

The practical guidance is unchanged and is now better supported rather than
weaker: set QP once at `Start` and vary only `FrameType` per frame. But the
distinction matters for a driver author, because "the field is absent" and
"the field is present, accepted, and silently ignored" fail very differently -
the second looks like the hardware disobeying you.

### Not independently re-derived

The profile/level enum tables, the `_S_AVE_Session_PFCfg` placement, the
slice-group abort, and the header-size limits are the agent's readings. The
NV12 pixel-format selector remains **unlocated** - see the risk note below.
