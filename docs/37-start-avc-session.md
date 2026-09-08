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
five things are corrected or sharpened, see §10.

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
0x0220 u32  sComm.FrameRate                = 30       /* MUST be > 0, §5.7 */
0x0234 int  sRC.RCMode                     = 3        /* const-QP; INFERRED */
0x0240 int  sRC.QP[I]                      = 26
0x0244 int  sRC.QP[P]                      = 26       /* set anyway, see §5.4 */
0x0248 int  sRC.QP[B]                      = 26
0x0298 int  sRC.RCQPRange.min              = 0
0x029C int  sRC.RCQPRange.max              = 51
0x02B8 int  sGOP.MaxKeyFrameInterval       = 1        /* every frame an IDR */
0x02BC int  sGOP.StrictKeyFrameInterval    = 1
0x02E0 int  sRef.ReferenceNum[0..2]        = 0        /* 0x2E0/0x2E4/0x2E8 */

/* ---- geometry -------------------------------------------------------- */
0x0368 u32  VideoParams.ui32Width          = 1920
0x036C u32  VideoParams.ui32Height         = 1088     /* MB-aligned; see §8 risk */

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
0x293C u32  seq_parameter_set_id           = 0        /* MUST be 0, §5.8 */
0x2940 u32  chroma_format_idc              = 1        /* 4:2:0 */
0x2944 u32  separate_colour_plane_flag     = 0
0x2948 u32  bit_depth_luma_minus8          = 0
0x294C u32  bit_depth_chroma_minus8        = 0
0x2951 u8   qpprime_y_zero_transform_bypass_flag = 0
0x2958 u8   seq_scaling_matrix_present_flag= 0
0x2D34 u32  log2_max_frame_num_minus4      = 0
0x2D38 u32  pic_order_cnt_type             = 2        /* 2 = display order == coding order */
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

/* ===== H264_PICTURE_HEADER_PARAMS  (PPSParams), 0x180 bytes =========== */
0x2FD0 u32  pic_parameter_set_id           = 0
0x2FD4 u32  seq_parameter_set_id           = 0
0x2FD8 u32  entropy_coding_mode_flag       = 1        /* 1 CABAC, 0 CAVLC; §5.2 */
0x2FDC u8   bottom_field_pic_order_in_frame_present_flag = 0
0x2FE0 u32  num_slice_groups_minus1        = 0
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

PPS scaling lists live in the `AVC_PPS` object (kext `this+32/44/56`), not in
this struct, so they are unreachable from the command.

**`transform_8x8_mode_flag = 1` is only legal for `profile_idc >= 100`.** With
`eProfile = 6` that holds. If a driver drops to Main (`eProfile = 4`) it must
also clear `transform_8x8_mode_flag`, or the PPS will carry a syntax element
the decoder will not read — this is a real, silent stream corruption, not a
firmware error. (Standards requirement, not a firmware finding.)

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

### 5.2 Combined header size ≤ 2048 bytes → else **-1019**

`0x6d820`–`0x6d834`:

```
6d820:  ldr w8, [x28, #1204]       ; SPSparams + 1200 = SPS header length (bits)
6d824:  ldr w9, [x28, #1844]       ; PPSparams + 124 = PPS header length (bits)
6d828:  add w8, w9, w8
6d82c:  lsr w8, w8, #3
6d830:  cmp w8, #0x800
6d834:  b.hi 0x6e0c0               ; -> mov w0, #0xfffffc05 = -1019
```

**Confirmed.** With `bFWCreatesHeader = 1` the lengths are whatever the
generator produced, so this only bites on an absurd VUI/scaling-list load.

### 5.3 `entropy_coding_mode_flag` must be 0 or 1

`CAVCController::ProcessRateControlAccumulateBits` (`0x7075c`) reads the flag
and dispatches; any value other than 0 or 1 logs
`"wrong entropy_coding_mode_flag"` (file `0x12b9af`, VA `0x1279af`, referenced
at `0x70828` and `0x70878`) and falls back to 0 or 1 depending on the branch.
**Confirmed as a range check.** It does not abort the session, but a stream
built on the fallback would disagree with the emitted PPS.

### 5.4 `RCMode` is checked, and QP is bit-depth-shifted

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

### 5.5 `sSliceMap.iNum`

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

### 5.6 Width and height are *not* range-rejected at Start

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

## 7. `_S_AVE_PSContext` — `cmd + 0x27C8`, 0x154 bytes

The 0x154-byte block docs/20 §3.1 listed as "`memcpy` from `client + 0x417C`"
is the **parameter-set index table**, the output side of the header generator:
`AVC_SPS::Generate(_S_AVE_PSContext*)` and `AVC_PPS::Generate(…,
_S_AVE_PSContext*)` both take it (fw `0x41b24`, `0x42f10`; the AVC path passes
`x21 = this + 0xFAC` at `0x6d7d4`, its own copy).

Layout, from `AVE_PSInfo_Make(const u8*, _E_AVE_PSType, int, int,
_S_AVE_PSContext*)` (kext `0xfffffe0008c491f0`, fw `0xe8f64`):

```c
struct _S_AVE_PSContext {          /* 0x154 */
    /* 0x000 */ uint32_t iNum;                 /* entry count */
    /* 0x004 */ struct { uint32_t eType;       /* _E_AVE_PSType, checked 1..3 */
                         uint32_t iLayerID;    /* checked < 2 */
                         uint32_t iOffset;     /* bits  -- inferred */
                         uint32_t iSize; }     /* bits  -- inferred */
                saBuf[21];
};
```

`4 + 21*16 = 340 = 0x154` exactly. The `iOffset`/`iSize` names come from the
assert `(psPSInfo->saBuf[iNum].sBuf.iOffset + psPSInfo->saBuf[iNum].sBuf.iSize)
<= (int32_t)sizeof(psPSContext->iaPSData)` (file `0x136615`) and from the
running-offset computation `ldp w8,w9,[x8,#-8]; add w8,w8,w9` (kext
`0xfffffe0008c49568`); the *field-to-offset* binding is **inferred**, the size
arithmetic is confirmed.

**Not required for first light**: with `bFWCreatesHeader = 1` the firmware fills
its own copy. Leave the block zero. The firmware logs
`"%s::%s:%d PSInfo num %d"` (`0x6d868`) and `"no PSInfo"` (file `0x12bf1b`)
around it.

---

## 8. `_S_AVE_Session_PFCfg`

The roadmap lists this as unmapped. What is now established:

* It is **argument 4 of `H264VideoEncoderDPB::ManageDPBBuffer`** (fw `0x1b0f8`),
  alongside `AVE_PICMGMT_RC_UPDATE_DATA*` (arg 3) and `AVE_PICMGMT_PARAMS*`
  (arg 8). **Confirmed** from the mangled symbol.
* The kext has a matching family of printers — `AVE_GOP_PrintPFCfg`,
  `AVE_RC_PrintPFCfg`, `AVE_Ref_PrintPFCfg`, `AVE_QPMod_PrintPFCfg`,
  `AVE_MCTF_PrintPFCfg`, `AVE_GGM_PrintPFCfg`, `AVE_LambdaMod_PrintPFCfg`,
  `AVE_Enc_PrintPFCfg` — parallel to the `_S_AVE_*_Cfg` family that docs/20 §3.2
  mapped into `cmd + 0x208 … 0x367`. **Confirmed** that the family exists (the
  os_log format symbols are in `data/derived/kext-symbols.txt`).
* **Not required for first light.** Nothing in `ProcessCmd_Start`
  (`0x31e44`–`0x32800`) or `CAVCController::InitEncodingParameters`
  (`0x6b8b0`–`0x6e440`) reads a per-frame config family; the DPB manager is the
  only consumer found, it runs per frame, and every field the minimal recipe
  needs was located elsewhere. A driver can leave whatever region it occupies
  zero for a first bring-up.

Its **offset inside `sCAveCmdAvcStart` is unknown**. It is not in the SPS block,
not in the PPS block, and not in `_S_AVE_PSContext`; if it is in the command at
all it is inside `VideoParams` (`0x368 … 0x27C7`) or the `0x68 … 0x207` head.
Recording the honest answer rather than a guess: **unknown**, with the entry
point for the next pass being a caller of `0x1b0f8` — read what it passes in
`x4` and walk that pointer back.

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
* **`_S_AVE_PSContext`** (`0x27C8`) — §7.
* **`_S_AVE_Session_PFCfg`** — §8.
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
  height validation at Start at all. §5.6.
* **docs/32 §6.4** left `EncCommParams.bitstream_addr_dst[index]` unlocated. It
  is `sCAveCmdAvcStart + 0x0D50 + 16·index`. §6.
* **docs/04 roadmap** lists `_S_AVE_Session_PFCfg` as the unmapped struct to
  chase. It is a *per-frame* config consumed by the DPB manager, not part of the
  minimal Start path. §8.

---

## 11. Still unknown

* The offset of `_S_AVE_Session_PFCfg`, if it is in the command at all (§8).
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

Both are cheap and both discriminate.

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
2. **Header round-trip.** With `bFWCreatesHeader = 1`, the firmware writes the
   SPS/PPS bit lengths back to its own copy and logs
   `"%s::%s:%d PSInfo num %d"` (`0x6d868`) and, in `DebugInit`,
   `"SPS header length:%d"` / `"PPS header length:%d"` (`0x64cd8`, `0x65858`).
   A single Start with logging at level 8 for subsystem `0x8c` reports both
   lengths and the full `SPSparams.*` / `PPSParams.*` dump, which validates §3
   and §4 end to end without needing a frame to be encoded. That is the cheapest
   possible confirmation of this entire document.
