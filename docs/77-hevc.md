# HEVC (Main, 8-bit 4:2:0) on the macOS 13.5 ABI: the commands, the layouts, and a plan

This is the static-analysis groundwork for adding HEVC to the driver, which
today encodes H.264 through `AVC_INIT` (id 4) and `AVC_ENCODE` (id 7). It
covers the HEVC commands and their layouts, the parameter-set blocks and
their firmware writers, scaling lists, CTU size, buffer tables and sizing, the
per-frame command, the coded header and output assembly, what macOS's user
space sends for HEVC, the HEVC asserts, and a staged implementation plan
against the current code.

**Nothing here was run on hardware.**

Conventions are those of [62](62-kext-field-map.md), [72](72-userspace-video-params.md)
and [74](74-residual-path.md):

- **Firmware VAs** are 13.5 image VAs (file offset = VA + `0x4000`).
- **Kext VAs** are 13.5 kernelcache VAs; `…eaXXXX` means `0xfffffe0008eaXXXX`.
- **User-space VAs** are VAs in `AppleVideoEncoder` (the bundle, docs/72 §1),
  with `__TEXT` at 0.
- **wire** is a byte offset in the command as sent on IPC channel 1.
- **VP** is `AVE_VIDEO_PARAMS`, at wire `0x60` in both INIT commands, so wire =
  VP + `0x60`. **RC** is `AVEFWRCSettings`, at wire `0xFF30`.
- **S** is the per-frame HEVC slice block (§3.2); **PICMGMT** is
  `AVE_PICMGMT_PARAMS`.
- Labels follow [00](00-methodology.md): **[C]** read from an instruction (VA
  cited); **[I]** inferred, with the chain stated; **[U]** unknown.
  *(d)* marks a row produced by a delegated analysis and not re-read line by
  line here. §12 lists what was re-read.

Reproduce anything with
`AVE_MACOS=13.5 python3 tools/disas.py --fw|--kext --addr VA -n LEN`.

---

## 0. Verdicts

| # | question | answer | label |
|---:|---|---|---|
| 1 | Which commands? | **`HEVC_INIT` id 5, `0x32DC8` bytes** (fw size check `0xd970`–`0xd97c`; kext builder `AVE_CHM_MakeFwCmd_Start_HEVC` `…ea9ec4`, `strh #5` at `…ea9fcc`). **`HEVC_ENCODE` id 8, `0x6838` bytes** (fw `0xdb30`; builder `…eacc18`). Both reply with the same `INIT_DONE` / `ENCODE_DONE` as AVC. Config, Open, Stop and Close are codec-free. HEVC does **not** reuse `AVC_ENCODE`. | [C] |
| 2 | Codec selection | Header `+0x18` = **1** (AVC 0) in every command of the session (kext `client[220]`, `…ea9fb0`). The id itself also names the codec. Id 9 (`LRME_STANDALONE`, same layout) is chosen only when client byte `0xE0C95` = VP+`0xFE5D` (wire `0xFEBD`) is non-zero (`cinc` `…eace14`). Keep that byte 0. | [C]; wire offset of the byte [I] |
| 3 | Is `HEVC_INIT` built like `AVC_INIT`? | **Yes, up to wire `0x105B0`.** The same `AVE_VIDEO_PARAMS` (`0xFED0`) sits at `0x60` and the same `AVEFWRCSettings` (`0x680`) at `0xFF30`. The kext's `AVE_CHM_SetFwBuf` is codec-free (`…eaed44`), and the firmware reads almost every AVC field at the same wire offset (§2.2). Past `0x105B0` the AVC SPS/PPS blocks are replaced by **VPS `0x140DC` @ `0x105B0`, SPS[0..1] `0x1EF4` @ `0x2468C`/`0x26580`, PPS[0..1] `0x25A4` @ `0x28474`/`0x2AA18`, RPS `0x5DD8` @ `0x2CFBC`**, then the same tail (u32 `0x32D94`, u8 `0x32D98`, `sSVEMap` `0x32D9C`, index `0x32DC0`). | [C] both sides |
| 4 | Who writes VPS/SPS/PPS? | **The firmware**, as for AVC, when both `bFWCreatesHeader` bytes are set: SPS[0]+`0x1CE9` (wire `0x26375`) and PPS[0]+`0x2198` (wire `0x2A60C`). If they differ, `INIT` fails with `0xEE0005`, the same code as AVC. The bytes go to the **same param-sets buffer** (wire `0xFB30`/`0xFB38`) in the order VPS, SPS, PPS, each with a 4-byte start code. The total bit count lands in coded-header `+0x98`, as for AVC. | [C] |
| 5 | **Scaling lists: the AVC blank-frame trap?** | **Does not apply.** The HEVC quantiser scale registers are not derived from SPS/PPS list bytes at all. They come from a per-frame mode, **PICMGMT+`0x6F4`** (Process wire `0x5CA4`). **Mode 0**, which is what a zero-filled command gives, writes flat `0x10000010` (= w 16) into every register (fw `0x8d0a8`, `0x8dfe4`). Mode 1 gives the HEVC default matrices; modes 3–7 and >9 assert (`CHEVCController_H13C.cpp:8032` / `:8965`). The host must only keep the bitstream consistent: **SPS `scaling_list_enabled_flag` = 0 with mode 0.** (macOS instead signals the default matrices: enabled = 1, data_present = 0, lists left zero.) | [C] |
| 6 | CTU size | **32×32, hard-wired in the pipe**: `ceil(w/32)·ceil(h/32)` (fw `0x70800`–`0x70840`), `(w+31)>>4 & ~1` (`0x836ec`), slice map `>>5` (`0x85d78`). The SPS CB/TB fields also go into the transcoder's config register (fw `0x75ae0`–`0x75b34`). So the SPS must say what macOS says: **min CB 8, CTB 32 (`log2_min_cb−3` = 0, diff = 2), TB 4..32 (0, 3), depth inter 1 / intra 0.** | [C] arithmetic; [I] "hard-wired" |
| 7 | Entropy | Always CABAC. The PPS flags `cu_qp_delta_enabled`, `diff_cu_qp_delta_depth`, `transquant_bypass`, `transform_skip`, `constrained_intra_pred` and **`entropy_coding_sync_enabled` (WPP)** are also packed into a transcoder register (fw `0x75b38`–`0x75b98`). They are hardware configuration as well as syntax. macOS sends WPP = 1. | [C] |
| 8 | Rate control and λ | Same RC block and offsets: `ui32RCFlag` `0xFF50` (2 = FIXQP), QPs `0xFFB4..BC`, frame rate `0xFF4C`, IDR period `0xFF34`, λ scales `0xFF98..`, per-QP tables `0xFFC0..`. HEVC reads the ME λ (RC+`0x68..0x70`) but not the MD λ or the per-QP table. It uses its own QP→λ tables when wire `0xFF72` = 0. The slice QP is `QP − 26 − init_qp_minus26 − 6·bitdepth` (fw `0x212d8`). macOS's HEVC λ tables are **byte-identical** to its H.264 ones. | [C] |
| 9 | Per-frame command | `HEVC_ENCODE` = header, **HEVC slice-header params S (`0x5570`) at `0x40`**, **PICMGMT (`0xF68`) at `0x55B0`**, 12 bytes at `0x6518`, **slice short-term RPS (`0x16C`) at `0x6524`**, long-term RPS (`0x1A8`) at `0x6690`. The firmware copies only S[`0..0xD6C`) (fw `0xfcc4`–`0xfd0c`). **PICMGMT is the same struct at the same field offsets as AVC** (§3.3). | [C] |
| 10 | Output | **The firmware writes HEVC slice headers itself, into host-supplied SliceHeader buffers**, not into the coded buffer. Their IOVAs are 256 u64 at S+`0x568` (`IOVA + i·0x400`). Per slice record (stride `0x220`), `+0x210` = header IOVA and `+0x218` = header byte length (fw `0x7f1ac`/`0x7f1cc`). **Frame bytes = Σ(written + hdrlen − removed)**; kext `AVE_RetrieveRCStats` adds `+0x218` only for HEVC (`…ec4ed8`). Assembly per slice is header bytes, then coded bytes. `cabac_zero_words` (`+0xF0`) is never written for HEVC. | [C]; order [I] |
| 11 | Buffers that change | **Recon LSB planes are mandatory** (setPipe asserts all four recon planes ≠0 and 128-aligned: 13921/13934/13955/13968). **128-byte alignment** for SrcNbr, entropy and coded buffers (AVC 64). **Entropy: 2 rows, not 4** (ctrl+3768 = 2). HEVC-only Start tables: TranscodedData (VP+`0x548..0x558`), MBInputCtrl (VP+`0xFC68/70`). New per-frame SliceHeader surfaces (`0x40000` each). SrcNbr sizes grow (Pixel `768·ceil(W/32)·(ceil(H/64)−1)` = 720 KiB at 1080p, versus the driver's 128 KiB slot). | [C] |
| 12 | A trap specific to HEVC | Wire **`0xFEB4`** (VP+`0xFE54`), which the driver leaves 0 for AVC, carries the **input chroma format** in bits [4:2] (fw `0x83658`). HEVC asserts `chroma_format <= input_chroma_format` (`CHEVCController_H13C.cpp:5118`, `0x83680`). **0 fails this for 4:2:0.** macOS sends **16** (bits [4:2] = 4 = "same as SPS"). | [C] check and macOS value; [I] meaning of 4 |

**The first hardware question is one run with a clean "no"**: Config → Open →
`HEVC_INIT` with the fill in §6, and nothing else. Success is `INIT_DONE`
with `0xEE0000` and three parseable NAL units (VPS/SPS/PPS, types 32/33/34)
in the param-sets buffer. §8 gives each later step and its failure signatures.

---

## 1. The wire commands and codec selection

### 1.1 Ids and sizes (13.5)

| host op | id | size | fw check | kext builder / memset | reply |
|---|---:|---:|---|---|---|
| Start_HEVC (`HEVC_INIT`) | 5 | `0x32DC8` | `0xd970`–`0xd97c` (`0x32DB0 + 0x18`) | `…ea9ec4`; memset `…ea9f64`–`…ea9f94` (`0x32D68 + 0x60`) | `INIT_DONE 0xE04`; failure `0xEE0002` (fw `0xf458`–`0xf460`) |
| Process_HEVC (`HEVC_ENCODE`) | 8 | `0x6838` | `0xdb30` | `…eacc18`; memset `…eaccec` | `ENCODE_DONE 0xE06`, `0x48` message |
| (`LRME_STANDALONE`) | 9 | `0x6838` | `0xdbc4` | same builder when `client[0xE0C95]` ≠ 0 | `LRME_DONE` |

All **[C]**. [46](46-abi-13.5-commands-session.md) §1 already lists these ids
and sizes, and `driver/ave_abi.h` already has `AVE_OP_START_HEVC` /
`AVE_OP_PROCESS_HEVC` descriptors with the right ids, sizes, slots and reply
ids (`ave_abi.h:1502`, `:1511`).

Host dispatch: `AVE_HwC::SendFwCmd_Process` calls `Process_AVC` when
`client+0xE0D84` == 0 (`…f0169c`) and `Process_HEVC` when == 1 (`…f01744`). It
passes the same PICMGMT and slot = 21 + index, and allocates `0x1940` or
`0x6838` (`…f01344`–`…f01358`). **[C]** *(d)*. So the coded-slot rule
(index *i* ↔ slot 21 + *i*, docs/68 §1.4) carries over.

### 1.2 Codec in the header, and in Config/Open

- Header `+0x18` u32 = codec, **1 = HEVC**. Kext `ldr w8,[x27,#220]; str
  w8,[x23,#24]` (`…ea9fac`/`…ea9fb0`) in Start; the same in Process. **[C]**
- Firmware: `ProcessInitStage2` (`0x13dd8`) takes the HEVC tail
  (`cmd+0x32D98`) when `w22` = codec ≠ 0 (`cbz w22` `0x13e60`). It reads VP
  fields through `x23 = cmd + 0xFCE9` for both codecs (`0x13e48`–`0x13e50`),
  which is direct evidence that VP sits at the same wire offset in both
  INIT commands. **[C]**
- Config (id 1) and Open (id 2) carry **no codec field** that the firmware
  reads (docs/46 §3, §4; `ProcessStart` reads only `+0x10`). The dispatcher
  logs `+0x18` for every command (`0xd6b4`). What it does with `+0x18` on
  Open is **[U]**. The kext's Open builder copies `client[220]` there too
  (docs/46 §2), so the driver should put 1 in the Open, Stop and Close headers
  of an HEVC session. **[I]**
- There is no `VIDEO_PARAMS` codec-type field on the wire that the firmware
  branches on. The codec is the header word plus the command id. **[I]**,
  from the absence of any codec compare in `ProcessInitStage2` other than
  `+0x18`.

---

## 2. `HEVC_INIT` (id 5, `0x32DC8`)

### 2.1 Block map

Kext `AVE_CHM_MakeFwCmd_Start_HEVC` `…ea9ec4`. Firmware copy in
`CHEVCController::InitEncodingParameters` (IEP, `0x82b10`), with `x20` = VP
(`ldr x20,[x21,#8]` `0x82b58`). The descriptor is built by
`CHEVCController::Init` (`0x639e4`: `add x10,x8,#0x60`). All **[C]**.

| wire | len | contents | kext store | fw copy → ctrl | source (user-kernel blob `pInfo`) |
|---:|---:|---|---|---|---|
| `0x00` | `0x40` | header: id 5, codec 1, slot/prio `{6,200}` (literal `0x722f220`) | `…ea9f98`–`…ea9fcc` | — | — |
| `0x40`/`0x48` | u64/u32 | FwClient IOVA / size | `…ea9fec`/`…ea9ff8` | CreateClient (as AVC) | — |
| `0x50`/`0x58` | u64/u32 | FwClientMem IOVA / size (only if the surface exists) | `…eaa010`/`…eaa01c` | desc+0x28/0x30 (`0x639ec`); used only when `iNum ≥ 2` (`0x82cc0`) | — |
| `0x60` | `0xFED0` | `AVE_VIDEO_PARAMS`, staged at `chm+0x338` and patched by the codec-free `AVE_CHM_SetFwBuf` | `…eaa038`, `…eaa050`, `…eaa1d8` | many fields, §2.2 | `+0x7B0` |
| `0xFF30` | `0x680` | `AVEFWRCSettings` | `…eaa1f0` | §2.2 | `+0` |
| `0x105B0` | `0x140DC` | **VPS params** (`HEVC_VIDEO_HEADER_PARAMS`) | `…eaa210` (from `*(client+0x109FD0)`) | `ctrl+0x2C770` (`0x8303c`); its PTL (`+0x18`, `0x220` B) also to `ctrl+0x2C550` (`0x83020`) | `+0x11BD4` |
| `0x2468C` | `0x1EF4` | **SPS[0]** (`HEVC_SEQUENCE_HEADER_PARAMS`) | loop `…eaa238`–`…eaa284`, each only if its pointer ≠ 0 | `ctrl+0x4084C` (`0x83058`) | `+0x25CB0` |
| `0x26580` | `0x1EF4` | SPS[1] (second layer/view) | same | `ctrl+0x42740` (`0x83090`) | `+0x25CB0 + 0x1EF4` |
| `0x28474` | `0x25A4` | **PPS[0]** (`HEVC_PICTURE_HEADER_PARAMS`) | same | `ctrl+0x44640` (`0x83074`) | `+0x29A98` |
| `0x2AA18` | `0x25A4` | PPS[1] | same | `ctrl+0x46BE4` (`0x830ac`) | `+0x29A98 + 0x25A4` |
| `0x2CFBC` | `0x5DD8` | **HEVC_RPS params**: SPS short- and long-term sets, and a per-frame slice-RPS area | `…eaa2a0` (from `*(client+0x10A000)`) | `ctrl+0x49190` (`0x830d8`) | `+0x33B50` |
| `0x32D94` | u32 | `client[0xD0E30]` (AVC `0x10DE0`); meaning **[U]** | `…eaa2b8` | — | — |
| `0x32D98` | u8 | `bTranscodeOverlap` → client+6 | `…eaa2c8` | `0x13e6c`–`0x13e7c` | — |
| `0x32D9C` | `0x24` | `sSVEMap` | `…eaa2dc`–`…eaa2e8` | `ctrl+0x1240` (`0x82c0c`); `ProcessInitStage2` `0x13eb4` | — |
| `0x32DC0` | u32 | SVE map index (the CHM index) | `…eaa2f0` | `0x82bd8`, `0x13eb8` | — |
| `0x32DC4` | 4 | padding to `0x32DC8` | — | — | — |

The per-frame slice block (`0x5570`, `pInfo+0x2E5E0`) is **not** in
`HEVC_INIT`. It travels in every `HEVC_ENCODE` (§3). **[C]** (kext
`AVE_Client_Config` HEVC arm `…ecc2e4`–`…ecc35c`).

User space fills every one of these from one zeroed storage. Its HEVC
`AppleAVEVA_DriverInit` descriptor (`0x6e0dc`–`0x6e168`) points slot 8 at
S+`0x10AD0` (VPS), slots 9/10 at SPS[0/1], 11/12 at PPS[0/1], 13 at the slice
block and 14 at the RPS. The null-check strings name them
`pInitSettings->VPSHevcParams`, `psaHEVC_SPS`, `psaHEVC_PPS`, `SHHevcParams`
and `RPSHevcParams` (`0xa2d3c`, `0xa2e84`, `0xa2f0c`, `0xa328c`, `0xa3424`).
**[C]** *(d)*

Every field user space never writes is 0: `bzero(S+0x860, 0x2CC7C)` and
`bzero(S+0x2D4E0, 0xB348)` in `AVE_CreateInstance` (`0x4463c`, `0x44648`).
**[C]** *(d)*

### 2.2 VP and RC: which AVC offsets HEVC shares

Firmware reads in HEVC IEP `0x82b10`, compared with `driver/ave_abi.h`
`.start_avc`. *(d)*. The rows marked † were re-read here (§12).

| field (`ave_abi.h` name) | wire | HEVC | evidence (HEVC fw) |
|---|---:|---|---|
| width / height | `0x60`/`0x64` | same | → ctrl+`0xA8C`/`0xA90` `0x8310c` |
| frame_rate | `0xFF4C` | same | `0x830ec` † |
| bitrate | `0xFF30` | same | `0x84720` |
| rc_mode (`ui32RCFlag`; 2 = FIXQP) | `0xFF50` | same. **0 skips DPB creation** (`0x852e0`) | `0x830e4` † |
| qp_i / qp_p / qp_b | `0xFFB4`/`B8`/`BC` | same, → ctrl+`0x1368..0x1370` | `0x830f4`–`0x83108` † |
| qp_min / qp_max | `0xFF88`/`0xFF8C` | same | `0x84694`/`0x846ac` |
| key_interval (`ui32IdrPeriod`) | `0xFF34` | same | `0x834ac` |
| dbg_bits | `0xFCD8` | same | `0x83118` |
| skip_mode (u16) | `0xFCF0` | same field, **different consumer**: `SetMdFwParams` `cmp #1`/`#2` (`0x8c2ec`); no ReconL/ReconC SKIPMODE copy. macOS HEVC sends **0** (§5) | `0x83130` |
| ipcm_islice | `0xFCE4` | same | `0x83188` |
| mode_8x8 | `0xFCEC` | read (→ ctrl+`0xAB0`); HEVC use [U] | `0x83180` |
| lambda_scales | `0xFF98` (16 B), `0xFFA8` (8 B) | same | `0x83aa0`–`0x83ac4` |
| λ tables | `0xFFC0` (`0x5B4` B) | same memcpy → ctrl+`0x15E4`; **no HEVC reader** of the per-QP table | `0x83a84` |
| src_mode | `0xFEC0` | same | `0x83318` |
| src_cfg_byte (`pix_pck`) | `0xFCE8` | same, but forced 0 when wire `0xFD7C` ≠ 0 | `0x8324c`–`0x83268` |
| src_go_bit3 | `0xFCE9` | same | `0x83330` |
| need_lsb_planes | `0xFD7D` | same (also reads `0xFD7E`, `0xFD7F`) | `0x832f0` |
| num_views[0] (`iNumViews`/`iLayerNum`) | `0xFF24` | same. It bounds the recon, SPS/PPS and RC loops: **0 silently does nothing** | `0x83200` |
| num_views[1] | `0xFF28` | not read by IEP | — |
| sve_num | `0x10DE8` (AVC) | **`0x32D9C`** | `0x82bc0` |
| recon set | `0x88`, stride `0x10` | same, but **both u64 of each entry are copied** (32 u64 → ctrl+`0xCB8`) | `0x84228`–`0x84348` |
| colocated | `0xF6B0` | same (shared `ProvideReferenceFrames` `0x2b530`, called from `0x85f98`/`0x8601c`) | `0x2b654` |
| low_res_ref | `0x2A8` | same (same function) | `0x2b6c0` |
| low_res_result | `0x3B8` | read by the shared `ProvideReferenceFrames` (`0x2b994`); HEVC use [U] | — |
| coded addr / size / hdr | `0x4B8`/`0x558`/`0x5C0` | same, 20 entries | `0x84028`–`0x84224` |
| entropy addr / size | `0xF830` / `0xFA30` | same `[16][4]` layout; j=0 replicated when desc+0x22 = 0. **Rows used: 2** | loop `0x8434c`–`0x844f4`; rows `0x835bc`–`0x83600` |
| SrcNbr info / pixels / data | `0xF7D0`/`0xF7F0`/`0xF810` | same; **data read at a fixed `0xF810` ([0] only)** | `0x844f8`–`0x84534` |
| SrcNbr FwData | `0xFED0` | same | `0x84518` |
| param_sets addr / size | `0xFB30`/`0xFB38` | same | `0x85228`/`0x8522c` † |
| slice_num (`sSliceMap`) | `0xFDAC` | same block; entries used `>>5` (CTB rows) | `0x85d5c`–`0x85d78` |
| `0x74` (MaxSubMbRectSize), `0xFF76`, `0xFCF2`, `0xF7C0/C8` | — | not read by HEVC IEP | negative within IEP |

HEVC-only reads *(d)*:

| wire | VP | name (fw dumper) | → ctrl | VA | macOS HEVC value |
|---:|---:|---|---|---|---|
| `0xFCF8` | `0xFC98` | `enable_tmvp` | `+0x58441` | `0x83224` | 0 |
| `0xFCFC` | `0xFC9C` | `numFPCPUCand` | `+0x2C504` | `0x83238` | 0 |
| `0xFD00..0xFD0C` | `0xFCA0..AC` | `visible_offx/offy/width/height` (read by `SetMdFwParams`) | `+0x15C4..0x15D0` | `0x8326c` | 0 |
| `0xFD10` | `0xFCB0` | `sao_enb_config` (→ MMIO `0x1390264`, `&0x3FFF`) | `+0x2C4EC` | `0x83294`; `0x8ce9c` | **`0xFFFF`** |
| `0xFD14`/`0xFD18` | | `sao_rdd_off_offset_luma/chroma` | `+0x2C4F0/F4` | same | 0 |
| `0xFD1C` | `0xFCBC` | `sao_eo_bo_offset_config`; `0xFFFFFFFF` = firmware default | `+0x2C4E8` | `0x8cf78` | **`0xFFFFFFFF`** |
| `0xFD20` | `0xFCC0` | `input_bitdepth` | `+0x122A` | `0x832b8` | **8** |
| `0xFD24`/`0xFD28` | | `ltr_refidx` / `long_term_ref` | `+0x1210`/`+0x120C` | `0x832bc` | 0 |
| `0xFD34` | | `split_mode` | `+0x2C48C` | `0x832d8` | 0 |
| `0xFD80..0xFDA0` | | 9 PPS ids (`0xFD80` overwrites PPS[0] `pps_pic_parameter_set_id`, `0x850dc`) | `+0x24110..` | `0x833dc` | 0 |
| `0xFDA4` | `0xFD44` | PPS count (`i32PPSsCount`); ≥ 2 makes extra firmware PPSs with generated lists | `+0x24134` | `0x83434` | **1** |
| `0xFDA8` | `0xFD48` | `bSliceEncodingMode` | `+0x5844C` | `0x83248` | 0 |
| **`0xFEB4`** | **`0xFE54`** | **input format word; bits [4:2] = input chroma format** | `+0x24140` | `0x83324`, `0x83658` | **16** |
| `0xFECF` | | scaling-table set select (mode 8 only) | `+0x24517` | `0x83424` | — |
| `0x5A8/0x5B0/0x5B8` | `0x548/0x550/0x558` | TranscodedData table (HEVC-only read) | — | `0x84548`–`0x84558` | published by the kext |
| `0xFCC8/0xFCD0` | `0xFC68/0xFC70` | MBInputCtrl (HEVC-only read) | — | `0x84538`/`0x84540` | published (count 2) |

### 2.3 The parameter-set structs

Wire = block base + struct offset. The bases are VPS `0x105B0`, SPS[0]
`0x2468C`, PPS[0] `0x28474` and RPS `0x2CFBC`. Offsets come from the
firmware's own bitstream writers *(d)*. User space's dumper and
`AVE_HEVC_SetScalingList` use the same offsets (§5). Only the fields a
single-layer Main encode touches are listed. The full syntax-order tables,
with every VA, are in the delegated analysis
(`scratchpad/a2/a_fw_*.txt`), summarised in §12.

**NAL header** `WriteBits::hevc_nal_header(type, layer, tid+1)` `0x15888`:
start code `00 00 00 01`, then `forbidden 0, type u(6), layer u(6), tid+1 u(3)`.
VPS = (32, 0, 1) `0x1d2b4`; SPS = (33, SPS+4, 1) `0x1e3a8`; PPS = (34, PPS+4, 1)
`0x1f52c`. **[C]** *(d)*

**PTL** (`HEVC_PTL::ptl_parameters` `0x1a870`), at struct+`0x18`:

| P+ | field | coding | wire (SPS[0]) | wire (VPS) |
|---:|---|---|---:|---:|
| `0x0` u32 | general_profile_space | u(2) | `0x246A4` | `0x105C8` |
| `0x4` u8 | general_tier_flag | u(1) | `0x246A8` | `0x105CC` |
| `0x8` u32 | general_profile_idc (Main = 1) | u(5) | `0x246AC` | `0x105D0` |
| `0xC+j` u8 | profile_compatibility_flag[j] | 32×u(1) | `0x246B0+j` | `0x105D4+j` |
| `0x2C..0x2F` u8 | progressive / interlaced / non_packed / frame_only | u(1) | `0x246D0..D3` | `0x105F4..F7` |
| `0x3C` u32 | general_level_idc (30 × level) | u(8) | `0x246E0` | `0x10604` |

Keep `max_sub_layers_minus1` = 0. The sub-layer PTL body is not
spec-conformant (`0x1adb8`). **[C]** *(d)*

**VPS** (`video_header_parameter_set_rbsp` `0x1d294`): `vps_id` +4;
`base_layer_internal`/`available` +8/+9 (u8); `max_layers_minus1` +0xC;
`max_sub_layers_minus1` +0x10; `temporal_id_nesting` +0x14 (u8); PTL +0x18;
`sub_layer_ordering_info_present` +0x678; `max_dec_pic_buffering_minus1[i]` /
`num_reorder[i]` / `latency_increase_plus1[i]` at +0x67C / +0x69C / +0x6BC
(+4i); `max_layer_id` +0x6DC; `num_layer_sets_minus1` +0x6E0;
`timing_info_present` +0x106E4; `num_hrd_parameters` +0x106F8 (**keep 0**: it
writes no `hrd_parameters()` and warns "Possible wrong encode of VPS",
`0x1d50c`); `vps_extension_flag` +0x11AFC. Output bytes at +0x13FD8 (`0x100`),
header_len in bits at +0x13FD4. **[C]** *(d)*

**SPS** (`seq_parameter_set_rbsp` `0x1e380`):

| S+ | field | coding | wire (SPS[0]) | note |
|---:|---|---|---:|---|
| `0x4` u8 | nuh_layer_id | — | `0x24690` | 0 |
| `0x7` u8 | (gate) | — | `0x24693` | **keep 0**: non-zero writes u(6) instead of the scaling data-present flag (`0x1e6d4`) |
| `0xC` | sps_video_parameter_set_id | u(4) | `0x24698` | |
| `0x10` | sps_max_sub_layers_minus1 | u(3) | `0x2469C` | 0 |
| `0x14` u8 | sps_temporal_id_nesting_flag | u(1) | `0x246A0` | **must be 1 when max_sub_layers = 0**, else assert + spin `Hevc_headers.cpp:756` (`0x1e49c`) |
| `0x18` | PTL | | `0x246A4` | above |
| `0x238` | sps_seq_parameter_set_id | ue | `0x248C4` | |
| `0x23C` | chroma_format_idc | ue | `0x248C8` | 1; also → ctrl+`0xA84` and the transcoder register |
| `0x244`/`0x248` | pic_width/height_in_luma_samples | ue | `0x248D0`/`0x248D4` | IEP → ctrl+`0x2C498/9C` |
| `0x24C`/`0x250` | width/height in 32-px CTBs (not syntax; used for `slice_segment_address` bits) | — | `0x248D8`/`0x248DC` | macOS `(w+31)>>5`, `(h+31)>>5` |
| `0x254` u8 | conformance_window_flag | u(1) | `0x248E0` | |
| `0x258..0x264` | conf_win left/right/top/bottom (in chroma units) | ue | `0x248E4..F0` | |
| `0x268`/`0x26C` | bit_depth luma/chroma −8 | ue | `0x248F4`/`F8` | 0 |
| `0x270` | log2_max_pic_order_cnt_lsb_minus4 | ue | `0x248FC` | → ctrl+`0x2C424` = 1<<(v+4) |
| `0x274` u8 | sps_sub_layer_ordering_info_present | u(1) | `0x24900` | |
| `0x278`/`0x294`/`0x2B0` | max_dec_pic_buffering_minus1 / num_reorder / latency (+4i) | ue | `0x24904`/`0x24920`/`0x2493C` | |
| `0x2CC..0x2E0` | log2_min_cb−3, diff_cb, log2_min_tb−2, diff_tb, depth_inter, depth_intra | ue | `0x24958..0x2496C` | **0, 2, 0, 3, 1, 0**; also → transcoder register `0x75ae0` |
| `0x2E4` u8 | scaling_list_enabled_flag | u(1) | `0x24970` | 0 (§2.5) |
| `0x2E5` u8 | sps_scaling_list_data_present_flag | u(1) | `0x24971` | 0 |
| `0x2E8..0xF90` | scaling_list_data storage | | `0x24974..` | §2.5 |
| `0x1B90` u8 | amp_enabled_flag | u(1) | `0x2621C` | 0 |
| `0x1B91` u8 | sample_adaptive_offset_enabled_flag | u(1) | `0x2621D` | 1 (macOS) |
| `0x1B92` u8 | pcm_enabled_flag | u(1) | `0x2621E` | 0; also transcoder bit 17 |
| RPS+`0` | num_short_term_ref_pic_sets | ue | `0x2CFBC` | §2.4 |
| RPS+`0x5A68` | long_term_ref_pics_present | u(1) | `0x32A24` | 0 |
| `0x1C2C` u8 | sps_temporal_mvp_enabled_flag | u(1) | `0x262B8` | see §6 |
| `0x1C2D` u8 | strong_intra_smoothing_enabled_flag | u(1) | `0x262B9` | 0; also transcoder bit 28 |
| `0x1C30` u8 | vui_parameters_present_flag | u(1) | `0x262BC` | 0 (the VUI writer never writes `overscan_appropriate_flag`; HRD is non-conformant) |
| `0x1CD8` u8 | sps_extension_present_flag | u(1) | `0x26364` | 0 |
| **`0x1CE9` u8** | **bFWCreatesHeader** | — | **`0x26375`** | 1 |
| `0x1CEC` | header_len (bits; firmware overwrites) | — | `0x26378` | |
| `0x1CF0` | header bytes (`0x200`) | — | `0x2637C` | |

**PPS** (`pic_parameter_set_rbsp` `0x1f50c`):

| P+ | field | coding | wire (PPS[0]) | note |
|---:|---|---|---:|---|
| `0x4` | nuh_layer_id | — | `0x28478` | 0 |
| `0xC` / `0x10` | pps_pic_parameter_set_id / pps_seq_parameter_set_id | ue | `0x28480`/`0x28484` | **the firmware overwrites both** from wire `0xFD80` and SPS+`0x238` (`0x850c8`–`0x850e8`) |
| `0x14`/`0x15` u8 | dependent_slice_segments / output_flag_present | u(1) | `0x28488`/`89` | 0 |
| `0x18` | num_extra_slice_header_bits | u(3) | `0x2848C` | **must be 0**, else assert + spin `Hevc_headers.cpp:945` (`0x1f5a0`) |
| `0x1C`/`0x1D` u8 | sign_data_hiding / cabac_init_present | u(1) | `0x28490`/`91` | 0 (neither is in the transcoder register) |
| `0x20`/`0x24` | num_ref_idx_l0/l1_default_active_minus1 | ue | `0x28494`/`98` | 0 |
| `0x28` s32 | init_qp_minus26 | se | `0x2849C` | 0 |
| `0x2C`/`0x2D` u8 | constrained_intra_pred / transform_skip_enabled | u(1) | `0x284A0`/`A1` | 0; also transcoder bits 16/17 |
| `0x2E` u8 / `0x30` | cu_qp_delta_enabled / diff_cu_qp_delta_depth | u(1)/ue | `0x284A2`/`0x284A4` | 0 / 0 at fixed QP (macOS FIXQP clears it); transcoder bits 18, 20–21 |
| `0x34` / `0x54` | pps_cb_qp_offset / pps_cr_qp_offset | se | `0x284A8`/`0x284C8` | 0 |
| `0x74..0x79` u8 | slice_chroma_qp_offsets_present, weighted_pred, weighted_bipred, transquant_bypass, tiles_enabled, **entropy_coding_sync_enabled** | u(1) | `0x284E8..0x284ED` | 0,0,0,0,0, **1 (macOS)**; transquant bit 24, WPP bit 31 |
| `0x889` u8 | pps_loop_filter_across_slices | u(1) | `0x28CFD` | 0 |
| `0x88A..0x88C` u8 | deblocking_control_present / override_enabled / pps_deblocking_disabled | u(1) | `0x28CFE..0x28D00` | 1 / 0 / 0 |
| `0x890`/`0x894` | beta_offset_div2 / tc_offset_div2 | se | `0x28D04`/`08` | 0 |
| `0x898` u8 | pps_scaling_list_data_present | u(1) | `0x28D0C` | **forced 0** for PPS[0] (`0x850e0`) |
| `0x2144..0x2147` u8 | lists_modification_present, log2_parallel_merge_level−2, slice_header_ext_present, pps_extension_present | | `0x2A5B8..BB` | all 0 |
| **`0x2198` u8** | **bFWCreatesHeader** | — | **`0x2A60C`** | 1 |
| `0x219C` | header_len, **accumulated** by the writer (new = old + bits, `0x1fa50`) | — | `0x2A610` | **must be 0 on input** |
| `0x21A0` | header bytes (`0x400`) | — | `0x2A614` | |

### 2.4 RPS block (wire `0x2CFBC`, `0x5DD8`)

The SPS writer is called with the RPS object whose data is ctrl+`0x49190`
(`0x85014`, `0x8507c`). **[C]** *(d)*

- RPS+0 (s32): `num_short_term_ref_pic_sets`.
- Entry *i* at RPS+4+`0x164`·*i* (wire `0x2CFC0` + `0x164`·*i*):
  `inter_ref_pic_set_prediction_flag` +0 (u8; **keep 0**: the inter-prediction
  loop reads NumDeltaPocs from entry 0 and runs one short, `0x1ec50`),
  `num_negative_pics` +0x30, `num_positive_pics` +0x34,
  `delta_poc_s0_minus1[j]` +0x38+2j (**u16**), `used_by_curr_pic_s0_flag[j]`
  +0x58+j, `delta_poc_s1_minus1[j]` +0x68+4j, `used_s1[j]` +0xA8+j,
  NumDeltaPocs +0x160.
- `long_term_ref_pics_present` RPS+`0x5A68`.
- RPS+`0x5AC0` (`0x16C`) and +`0x5C2C` (`0x1A8`) are the per-slice RPS areas.
  They are overwritten per frame from `HEVC_ENCODE` `0x6524`/`0x6690`
  (firmware `WriteSliceHeadersHevc` `0x7eea0`–`0x7eebc`).

For IPPP with one reference: `num_short_term_ref_pic_sets` = 1; entry 0 =
{num_negative 1, num_positive 0, delta_poc_s0_minus1[0] 0, used_s0[0] 1,
NumDeltaPocs 1}. **[I]** (the syntax is [C]; the choice is ours). macOS
sends this block as zeros and lets the kext build it (`HEVC_RPS::
program_sps_rps_IPPP` `…f50c54`). **[I]** *(d)*

### 2.5 Scaling lists: what the host must fill

**Nothing, for non-zero quantiser scales.** This is the answer to the
question the AVC blank frame raises.

- `CHEVCController::SetFwParams` (called every frame from setPipe, `0x72b8c`)
  calls `SetMdScalingFwParams` (`0x8d040`) and `SetRcrScalingFwParams`
  (`0x8df74`). Both switch on a per-frame mode:
  ```
  8dfac  ldr w8,[x0,#4732]            ; context index
  8dfb4  ldr x8,[x8,#9144]            ; ctrl[9144+8*ctx] = PICMGMT + 8   (store 0x651e8, x23 = PICMGMT)
  8dfb8  ldr w8,[x8,#1772]            ; -> PICMGMT + 0x6F4
  8dfbc  cmp w8,#0x9 ; b.hi 0x8e688   ; assert(0), CHEVCController_H13C.cpp:8965
  8dfe4  mov w8,#0x10 ; movk w8,#0x1000,lsl#16   ; mode 0: 0x10000010 everywhere
  ```
  **[C]**, re-read here. `x23` in `PipePrepareParam` is `[info+16]`, which
  reads PICMGMT +`0xC10`/`0xCB0`/`0xCA8`/`0xCAC` (`0x64cbc`–`0x64db4`), so
  `x23+8+0x6EC` = PICMGMT+`0x6F4`. **[C]**
- PICMGMT+`0x6F4` is the field `ave_abi.h` already calls
  `scaling_matrix_mode` (13.5 `process_avc.scaling_matrix_mode = 0x6f4`). The
  kext writes it for both codecs (`…eabbd4`). On `HEVC_ENCODE` it is wire
  **`0x55B0 + 0x6F4 = 0x5CA4`**. **[C]**
- Modes *(d)*: 0 flat 16; **1 the HEVC default matrices** (4×4 flat, 8×8
  intra/inter Table 7-6 at fw `0xd9480`/`0xd94c0`); 2 hard-coded weights;
  8 tables chosen by wire `0xFECF`; 9 constants plus s16 tables; anything
  else asserts `:8032` (Md, `0x8d788`) / `:8965` (Rcr, `0x8e688`). Every table
  byte is ≥ 6, so **no divisor can be zero**. **[C]** *(d)*
- **No firmware code reads the SPS or PPS list bytes** except the header
  writers and `DebugInit`. The same search finds the writer
  (`ldrb #740/#741`). **[C]** *(d)*
- **What the SPS must say, for a conformant stream** [I]:
  - mode 0 → `scaling_list_enabled_flag` = 0 (flat);
  - mode 1 → enabled = 1, `sps_scaling_list_data_present_flag` = 0 (the
    decoder then uses the same defaults).
  - Never set enabled = 1 and data_present = 1 with zero lists: the writer
    would code the zero deltas while the quantiser uses its own tables.
- macOS user space uses scaling mode `DRV+0xB8` = **1** by default
  (`0x6ceec`). `AVE_PrepareVideoAndSequenceHeader` then stores enabled = 1,
  present = 0 (`strh #1` `0x73764`, re-read here) and writes no list data
  (`b 0x73f34`). **[C]** Which PICMGMT+`0x6F4` value macOS sends per frame
  for HEVC is **[I]** = 1: the kext derives it through a lookup keyed by
  `HEVC_Slice+0x10` when `client+0xD0708` = 1 (`…eabba8`–`…eabcd4`) *(d)*.
- **Driver consequence.** Nothing to add in the command; leave wire
  `0x5CA4` = 0. `session_scaling` has no HEVC counterpart. The *register* half
  of the AVC lesson ("the firmware derives every scale from host data")
  **does not repeat**. The *bitstream-consistency* half does, and it is a
  single flag.

`scaling_list_data` storage (header writer only), for reference: pred_mode
u8 at SPS+`0x378 + 6·sizeId + matrixId`; pred_matrix_id_delta u32 at
+`0x318 + 24·sizeId + 4·m`; dc_coef_minus8 s32 at +`0x2E8 + 24·(sizeId−2) + 4m`;
**delta** coefficients (DPCM, not absolute) s16 at
+`0x390 + 0x300·sizeId + 0x80·m + 2k`; PPS the same at +`0x5B4`.
`0x1dba8`, `0x1edbc` **[C]** *(d)*.

### 2.6 CTU, AMP, SAO, TMVP, WPP, λ, skip

| item | firmware | what to send | label |
|---|---|---|---|
| CTU | 32×32 in the pipe (§0 #6); SPS CB/TB fields → transcoder config `0x75ae0` | SPS 0/2/0/3/1/0 (macOS: `0x6d234`/`0x6d238` *(d)*) | [C]/[I] |
| AMP | SPS+`0x1B90` read only by the writer and the dumper; hardware enable [U] | 0 (macOS 0) | [C] |
| SAO | hardware from `sao_enb_config` (wire `0xFD10` → MMIO `0x1390264`); `eo_bo` `0xFFFFFFFF` = firmware default; SPS SAO flag only in the SPS and slice-header writers | macOS: `0xFFFF`, `0xFFFFFFFF`, SPS SAO 1, slice SAO luma/chroma 1/1. Keep all four consistent (all on, or all off) | [C]; consistency [I] |
| TMVP | hardware from wire `0xFCF8` only; SPS+`0x1C2C` read only by the writers | macOS: wire `0xFCF8` 0, SPS flag 1, slice flag (S+`0x11C`) 0 — i.e. no TMVP in any slice | [C] *(d)* |
| WPP | PPS+`0x79` → transcoder bit 31 (`0x75b84`, re-read here); `num_entry_point_offsets` S+`1088` from S+`13676` (firmware-side, beyond the `0xD6C` host copy) | macOS 1 | [C] reg; [I] that the firmware fills the entry points |
| λ | ME λ ctrl+`0xB10..0xB18` ← RC+`0x68..0x70` (`SetMeFwParams` `0x8c674`); MD λ / per-QP table unread by HEVC; QP→λ from fw table `0xda420` when wire `0xFF72` = 0 | send the AVC λ block unchanged (macOS HEVC tables are byte-identical, §5) | [C] *(d)* |
| skip_mode | `SetMdFwParams` compares 1/2 (`0x8c2ec`) | **0** (macOS HEVC ends at 0: `strh #3` `0x6cec4`, then `strh wzr` `0x6d090`, both re-read here) | [C] |
| `bFlatAreaLowQpEn` (`0xFF7B`), `eStaticAreasLowQpSel` (`0xFF80`) | non-zero without the matching RC/QPMod settings → **`0xEE0005`** (`0x83bb4`–`0x83d18`) | 0 at fixed QP | [C] *(d)* |

### 2.7 Buffer tables at Start

The kext's `AVE_CHM_SetFwBuf` never loads the codec (`…eaedf0` is its only
client load); it writes the same VP tables for both codecs. **[C]** *(d)*
The firmware side is in §2.2. HEVC differs from AVC in:

- **Recon entries: both u64 must be valid.** HEVC copies both
  (`0x84228`–`0x84348`), `setRefPointers` (shared, `0x2c328`–`0x2c33c`) derives
  all four planes from them, and setPipe asserts all four planes ≠ 0 and
  **128-aligned** (§7). The driver's `session_lsb` (default on) already
  publishes the second u64 (LSB pair). **[C]**/[I]
- **Entropy**: 2 rows (ctrl+`3768`, `0x835bc`–`0x83600`), sizes read only at
  Start. setPipe asserts `entropy_size[0]`/`[1]` ≠ 0 (`:14200`), and
  `SetTranscodeCommon` asserts each address ≠ 0 and `& 127 == 0`
  (`:7447/7448`). **[C]** *(d)*
- **Coded buffer**: `curr_bitstream_addr_dst` ≠ 0 and 128-aligned
  (`SetTranscode:7597/7598`, `0x75c94`/`0x75c98`, re-read here). The
  alternative arm (`tmp_bitstream_addr_dst`, `:7605/7606`) is selected by a
  byte `[x22,#426]` of unknown origin. It is probably where the HEVC-only
  TranscodedData table (VP+`0x548`) comes in. **[I]**
- **SrcNbr**: 128-aligned (`:14062/14063`, `:14069/14070`); HEVC uses
  `data[0]` only.

---

## 3. `HEVC_ENCODE` (id 8, `0x6838`)

### 3.1 Block map

Kext `AVE_CHM_MakeFwCmd_Process_HEVC` `…eacc18` (re-read here at
`…eace08`–`…eace7c`). Firmware `ProcessHevcEncode` `0xfb90` (re-read here at
`0xfcc0`–`0xfd0c`).

| wire | len | contents | kext | fw copies |
|---:|---:|---|---|---|
| `0x00` | `0x40` | header; slot ≤ `0x28` (`…eaccc4`); `+0x18` codec 1 | `…eaccf4`–`…eace18` | `0x40` |
| `0x40` | `0x5570` | **S = `HEVC_SLICE_HEADER_PARAMS`**, a copy of the client's `HEVC_Slice` object (`*(client+0x109FF8)`) | `…eace38`–`…eace44` | **only `0xD6C` bytes** (`0xfcc4`–`0xfcd0`) |
| `0x55B0` | `0xF68` | **PICMGMT**, size word `0xF68` at +0 | `…eace1c`–`…eace34` | `0xF68` (`0xfcd4`–`0xfce4`) |
| `0x6518` | `0xC` | not written by the kext; +0 u32 (0 → fallback, clamped ≥ 8), +4 u8, +8 u32 → ctrl+`5140` (`PipePrepareParam` `0x665b4`–`0x665f8`) | — | `0xC` (`0xfce8`) |
| `0x6524` | `0x16C` | slice short-term RPS: `HEVC_RPS+0x5AC0` | `…eace48`–`…eace60` | `0x314` from `0x6524` (`0xfcfc`–`0xfd0c`) |
| `0x6690` | `0x1A8` | slice long-term RPS: `HEVC_RPS+0x5C2C` | `…eace64`–`…eace7c` | (same copy) |

The firmware reads the frame number at `cmd+0x6258` = PICMGMT+`0xCA8`
(`0xfc24`) and `SendCommandToQueue` locates PICMGMT at `+0x55B0` for codec 1
(`0x145a8`). Both **[C]**, re-read.

### 3.2 The slice block S (wire `0x40` + offset; must lie in `0..0xD6B`)

The firmware's `AVE_HEVC_PrepareSliceHeader` (`0x21278`) fills per frame:
`nal_unit_type` (S+8; IDR = 20 `IDR_N_LP`, P = 1/2/3), `slice_type` (S+`0x1C`;
0 = P, 1 = B, 2 = I), `num_ref_idx_l0/l1_active_minus1` (S+`0x120`/`0x124`),
the override flag, the temporal id and `slice_qp_delta` (S+`0x424`) from the
FIXQP QPs. **[C]** *(d)* It does **not** set SAO, deblocking,
loop-filter-across-slices, merge candidates or TMVP (no stores in
`0x21278`–`0x218cc`) *(d)*. Those come from the host:

| S+ | field | macOS default (user space, `0x6d360`–`0x6d3b8` *(d)*) | send |
|---:|---|---|---|
| `0x0` | size word (kext ctor `0x5570`) | `0x5570` | `0x5570` |
| `0xC` u8 | first_slice_segment_in_pic_flag | 1 | 1 |
| `0x10` | slice_pic_parameter_set_id (must match one of the 9 ids at wire `0xFD80..`, else `:13902`; all-zero passes, `0x72aa8`–`0x72b54`) | 0 | 0 |
| `0x28` | slice_pic_order_cnt_lsb | kext `Setup_P_Frame` writes it (`…f4e7dc`) | frame count since IDR mod MaxPocLsb [I] |
| `0x11C` u8 | slice_temporal_mvp_enabled_flag | 0 | 0 |
| `0x11D`/`0x11E` u8 | slice_sao_luma / chroma | 1 / 1 | = SPS SAO |
| `0x1AA` u8 | collocated_from_l0 | 1 | 1 |
| `0x3D4` | five_minus_max_num_merge_cand | 3 | 3 |
| `0x43C` u8 | slice_loop_filter_across_slices | 0 | 0 |
| `0x44C` | slice map, `0x100` bytes, per 32-px CTB row (kext `GenerateMap` memsets it and, for one slice, leaves it zero, `…f4fb88`–`…f4fd4c`) | zero | zero [I] |
| **`0x568`** | **256 × u64 SliceHeader slot IOVAs**, `base + i·0x400` (kext `HEVC_Slice::UpdateBuffer` `…f4e318`–`…f4e350`, HEVC only) | — | **all 256** |

`HEVC_ENCODE` wire `0x6524` (slice short-term RPS): +0 u8
`short_term_ref_pic_set_sps_flag`, +4 u32 `short_term_ref_pic_set_idx`, +8 an
explicit set (`0x164`) used when the flag is 0 (`0x7eec0`–`0x7eef8`).
**[C]** *(d)* For the §2.4 IPPP set: flag 1, idx 0 (not coded when
`num_short_term_ref_pic_sets` < 2).

### 3.3 PICMGMT is shared

Same C++ type and size in the kext (one `AVE_PICMGMT_PARAMS` passed to both
builders, `…f01698`/`…f01740`); same base-class consumers in the firmware
(`CAVECommonDPB::setRefPointers` `0x2c314`, `GetFrameType` `0x23c7c`).
**[C]** *(d)*

Every offset the driver writes in `.process_avc` is read by HEVC at the same
place: input planes `0x8C0`/`0x8D0` (setPipe `0x71fa4`/`0x72110`), strides
`0x8C8`/`0x8D8`, `input_compressed` `0x6F3`, out mode/index/coded/size/hdr
`0xC00`/`0xC04`/`0xC08`/`0xC18`/`0xC10` (`ProcessTranscodeStart`
`0x74f0c`–`0x74f58`, `PipePrepareParam` `0x64d44`), frame_num/type/ctx
`0xCA8`/`0xCAC`/`0xCB0`, force_key `0x38`, src_nbr `0x980..0x9E0`, entropy
`0xA00`, low_res_src `0xC20`, low_res_results `0xC28..0xC40`, recon
`0x898`/`0x8A0`/`0x8A8`/`0x8B0`/`0x8B8`, scratch `0x8E0..0x900`. **[C]** *(d)*,
with `0xCA8`/`0xCAC`/`0xCB0`/`0xC10` re-read here.

Differences: HEVC setPipe reads the reference arrays at `0x6F8..0x778`
directly (AVC reads the `0x798..` copies); PICMGMT+`0xF64` is written by the
kext only for HEVC (regopt flags, `…eaaf18`–`…eaaf54`; firmware use [U]).
HEVC-specific per-frame state lives in S and the RPS blocks, not in PICMGMT.
*(d)*

---

## 4. Output: coded header, slice headers, byte count

- **`CODED_DATA_HDR` is shared** (same `0x220` slice-record stride, same
  `+0x180` bytes written, `+0x38C` bytes removed, `+0x98` param-set bits,
  `+0x10C`/`+0x110` frame num/type, the latter from the same source as AVC,
  `0x7d648`/`0x7d650`). **[C]** *(d)*
- **HEVC-only per-slice fields**: record+`0x210` u64 = the slice header's
  IOVA; record+`0x218` u32 = its **byte** length. Header-relative:
  `0x390 + 0x220·s` and `0x398 + 0x220·s`. They are written only by
  `CHEVCController::WriteSliceHeadersHevc` (`0x7f1ac` `str w2,[x20,#536]` with
  `w2 = bits>>3`; `0x7f1cc` `str x26,[x20,#528]`), which generates the header
  (`generateSliceHeaderInKF` `0x7f194`) and copies it to the slot address
  S+`0x568+8s` (`ldr x26,[x9,#1384]` `0x7f1bc`, memcpy `0x7f1e8`). **[C]**,
  re-read.
- **Byte count.** `AVE_RetrieveRCStats` (`…ec4e38`) adds `ldr w9,[x25,#920]` =
  record+`0x398` only when `w21` == 1 (`cmp w21,#1` `…ec4ed8`, re-read), so
  frame bytes = Σ(`written` + `hdrlen` − `removed`). **[C]**
- **Assembly** (per slice, in order): SliceHeader slot bytes (`hdrlen`,
  starting `00 00 00 01` + the 2-byte NAL header), then coded-buffer bytes at
  the running offset (`written − removed`). On an IDR, prepend the param-sets
  buffer (`+0x98` bits / 8 bytes: VPS, SPS, PPS). **[I]**: the order follows
  from where each part lives, and the kext never CPU-maps the SliceHeader
  surfaces, so macOS's user space must splice them in.
- **Param sets**: `+0x9C..0xEF` is an HEVC-only `_S_AVE_PSInfo` {count, 5 ×
  {type 1/2/3, layer, bit offset, bits}} (fw `0x7d620`, `0x773b8`). It gives
  exact per-NAL boundaries. **[C]** *(d)*
- **`cabac_zero_words` (`+0xF0`) is never written for HEVC** (the only writers
  are AVC `0x5c200` and LRME `0x268e0`). It *is* read into an RC sum
  (`0x7e7d0`), so it must stay zero. The driver already zeroes the whole
  header per frame. **[C]** *(d)*
- `+0xF4` (filler NAL info) and whether the I/P/skip counts at `+0x00..` are
  in MBs or CTUs are **[U]**. The per-MB count check in `ave_session_process`
  must be relaxed for HEVC.

---

## 5. What macOS's user space sends for HEVC

From `AppleVideoEncoder`'s HEVC path *(d)*:

- start `sub_64050` → `AVE_SetEncoderDefault` (HEVC) `sub_6cb20` at `0x642a4`;
- first frame: `AVE_ManageSessionSettings` `sub_7ff98` →
  `AVE_ValidateEncoderParameters` (HEVC) `sub_70700` →
  `AVE_PrepareVideoAndSequenceHeader` `sub_734b8` →
  `AVE_PreparePictureHeader` `sub_75988` → `AppleAVEVA_DriverInit` at
  `0x6e1ac`.

The H.264 functions of docs/72 are **not** on this path.

Differences from the H.264 table in docs/72 §5 (all **[C]** at the cited
store *(d)*, except where a row is marked re-read):

| field | HEVC | H.264 | VA |
|---|---|---|---|
| VP+8 `EnableSelStatsFlags` | **0** (no bit 27; docs/72 §2.1's "bit 27 for HEVC" is the H.264 Validate, which HEVC never runs) | 0 | `0x6ce14` |
| `skip_mode` | 3 then **0** | 3 | `0x6cec4`, `0x6d090` (re-read) |
| `mode_8x8_transform` | 2 (not cleared) | 0 for Main | `0x6ce58` |
| `search_range` | 4 for two device classes, else 0 | 0 | `0x6ce30` |
| `sao_enb_config` / `eo_bo` | `0xFFFF` / `0xFFFFFFFF` | same | `0x6cee0`, `0x6ced8` |
| VP+`0xFD44` `i32PPSsCount` | 1 | 1 | `0x6ce6c` |
| VP+`0xFE54` | 16 | 16 | `0x6ce80` |
| RC `ui32Bitrate` default | w·h·1.5·**0.075**·30 | ·0.15· | `0x6cef4` |
| RC `ExpectedFrameRate` | `0xCDCDCDCD` (unset) | 30 | `0x6cf38` |
| RC `bEnableQPModRefresh` | **0** | 1 | `0x6cf50` |
| RC+`0x43..0x45` QP-mod variants | 1, 1, 1 | 1 at `0x43` only | `0x6cf58` |
| RC `bFlatAreaLowQpEn`, +`0x4C` | 1, 1 (kept) | 0 | `0x6d030` |
| RC `SoftMaxQP` | **48** | 51 | `0x6cf98` |
| λ scales RC+`0x68..0x7C` | `0x400` ×5, 0 | same | `0x6cf64`–`0x6cf74` |
| per-QP tables RC+`0x90..0x643` | constants `0x117848`/`0x117918`/`0x1179e8`, **byte-identical** to H.264's `0x116188`/`0x116258`/`0x116328` | — | `0x6cfa4`–`0x6d054` |
| RCFlag | 1; `FORCEFIXQP` (3) → **2** in NewDefaults `0x869e0`, which also clears PPS `cu_qp_delta_enabled` | same shape | `0x6cf38`, `0x869e0`–`0x86a48` |
| scaling mode `DRV+0xB8` | **1** (SPS enabled = 1, data_present = 0, lists zero) | fills 16 | `0x6ceec`; `0x73760`–`0x73768` (re-read) |

Parameter-set defaults (`sub_6cb20` *(d)*):

- **VPS/SPS PTL**: profile_space 0, tier 0, **profile 1 (Main)**, progressive
  1, interlaced 0, non_packed 1, frame_only 1. The level starts at 0 and is
  raised to max(size/rate level, bitrate level) in `ManageSessionSettings`
  (`0x820ec`). The Main case sets two compatibility bytes (`0x7479c`).
- **SPS**:
  - CB/TB 0/2/0/3, depth 1/0 (`0x6d234`/`0x6d238`);
  - AMP 0, SAO 1 (`0x6d244`), PCM 0;
  - TMVP 1, strong intra 0 (`0x6d25c`);
  - `log2_max_poc_lsb_minus4` 7;
  - `max_dec_pic_buffering_minus1[0]` 4;
  - width/height = VP width/height padded to 16 (`AVE_PrepareCropParams`,
    minimum 160×64), with a conformance window for the crop;
  - SPS+`0x24C/0x250` = `(w+31)>>5`, `(h+31)>>5` (`0x73f34`);
  - VUI on for colour info only.
- **PPS**:
  - sign_data_hiding 0, cabac_init_present 0;
  - init_qp_minus26 = QpI − 26 (0 by default);
  - `cu_qp_delta_enabled` 1 / depth 2 (cleared under FIXQP);
  - **WPP 1** (`0x6d32c`);
  - tiles 0, loop_filter_across_slices 0;
  - deblocking_control_present 1, deblocking not disabled;
  - lists_modification 0, parallel merge level 0.
- **Headers**: user space does not write VPS/SPS/PPS bytes. It reads them from
  the firmware's param-sets surface, uses `ui32_SPSPPSHeaderBits` (`0x92080`),
  splits them on start codes (`0x3d454`) and builds `hvcC` (`0x8d128`).
- **Per frame**: `IO_Process` (selector 7, `0x20` bytes, `0xa6e3c`) is shared
  with H.264. The slice block is built in the kext (`AVE_HEVC_GenerateSlice`
  `…ec1554`).

---

## 6. The fill for a first `HEVC_INIT` (single layer, Main, fixed QP)

This is the concrete list the builder must write. Everything not listed
stays zero (the memset).

| group | wire | value | why |
|---|---|---|---|
| header | `0x00` / `0x18` / `0x1C` / `0x20` | id 5 / **1** / 6 / 200 | §1 |
| client | `0x40`,`0x48`,`0x50`,`0x58` | as AVC | §2.1 |
| VP geometry | `0x60` / `0x64` | width / height padded to 16 (macOS) | §5 |
| RC | `0xFF4C` fps, `0xFF48` div, `0xFF50` **2**, `0xFFB4/B8/BC` QP, `0xFF88` 0, `0xFF8C` 51, `0xFF34` IDR period | as AVC fixed QP | §2.2 |
| λ | `0xFF98..0xFFAC`, `0xFFC0..0x10573` | the AVC `lambda_block` unchanged | §2.6, §5 |
| VP scalars | `0xFD7D` 1 (LSB planes); `0xFF24` / `0xFF28` 1 / 1; **`0xFEB4` 16**; `0xFDA4` 1; `0xFDAC` 1, `0xFDB0` 0, `0xFDB4` height; `0xFD10` `0xFFFF`; `0xFD1C` `0xFFFFFFFF`; `0xFD20` 8; `0xFCF0` **0** | macOS values; `0xFEB4` avoids `:5118`; `0xFF24` ≥ 1 or nothing is built | §2.2, §5 |
| buffer tables | recon `0x88` (**both u64**), colocated `0xF6B0`, low-res ref `0x2A8`, low-res result `0x3B8`, coded `0x4B8`/`0x558`/`0x5C0`, entropy `0xF830` + sizes `0xFA30` (≥ 2 rows, all 4 columns), SrcNbr `0xF7D0`/`0xF7F0`/`0xF810`/`0xFED0`, param sets `0xFB30`/`0xFB38` | same code as AVC, HEVC sizes (§9) and 128-byte alignment | §2.7 |
| tail | `0x32D9C` 1 (`sSVEMap.iNum`), `0x32DC0` 0 | single core | §2.1 |
| VPS | `0x105B4` 0; `0x105B8`/`B9` 1/1; `0x105C4` 1; PTL at `0x105C8` (below); `0x10C28` 1; `0x10C2C` 4; `0x10C90` 0 | Main, 1 layer | §2.3 |
| PTL (VPS `0x105C8`, SPS `0x246A4`) | +8 = 1; +0xD, +0xE = 1 (compat[1], [2]); +0x2C 1, +0x2E 1, +0x2F 1; +0x3C = 30·level | Main | §2.3 |
| SPS[0] `0x2468C` + | +0x14 **1**; +0x23C 1; +0x244/+0x248 width/height; +0x24C/+0x250 CTB cols/rows; +0x254 and +0x258.. crop (chroma units); +0x270 4..7; +0x274 1; +0x278 `max_dec_pic_buffering_minus1` (≥ refs); +0x2CC..+0x2E0 **0,2,0,3,1,0**; +0x2E4/+0x2E5 **0/0**; +0x1B91 1 (SAO); +0x1C2C 0 or 1 (see §2.6); +0x1CE9 **1** | | §2.3 |
| RPS `0x2CFBC` + | +0 1; entry 0: +0x34 (= 4+0x30) 1, +0x3C (4+0x38) u16 0, +0x5C (4+0x58) 1, +0x164 (4+0x160) 1 | IPPP, one reference (inert for an IDR-only run) | §2.4 |
| PPS[0] `0x28474` + | +0x18 **0**; +0x28 0; +0x2E 0; +0x79 **1** (WPP; macOS) ; +0x88A 1; +0x2198 **1**; +0x219C **0** | | §2.3 |

Per frame (`HEVC_ENCODE`): the AVC PICMGMT fill at `0x55B0` (with
PICMGMT+`0x6F4` = 0); S fields from §3.2 at `0x40`; `0x6524` = {1, 0} on P
frames; `0x6518` zero.

---

## 7. HEVC asserts that test host data

File `CHEVCController_H13C.cpp` unless named. *(d)* throughout; the rows
marked † were re-read here. Process wire = `0x55B0` + PICMGMT offset.

**At `HEVC_INIT`**

| line | test | wire | check VA |
|---|---|---|---|
| 241 | client buffer ≥ `0x143F8` (ctor); 5385 RC ≥ `0x49A0`; 5575 / 5590 DPB / LRME-DPB ≥ `0x24A0` each | FwClient (`0x40`/`0x48`); the driver's `0xB4000` is ample | `0x635dc`, `0x84610`, `0x852f8`, `0x85fb4` |
| 4838 | `bEnableFwOverride==0 || bEnableMBInputCtrl==0` | `0x6C`/`0x6D` | `0x82b60` |
| 4886 | `iFwClientMemAddr != 0` (only if `iNum ≥ 2`) | `0x50` | `0x82cd4` |
| 5080 / 5084 | still-image size limits (only if IdrPeriod = 1 or `0xFF77` ≠ 0) | `0x60`/`0x64` | `0x83548`, `0x834e0` |
| **5118** | `chroma_format <= input_chroma_format` | SPS `0x248C8` vs **`0xFEB4`** bits [4:2] | `0x83680` |
| 5033 | `low_res_pipe_sync_mode` when `search_range` = 7 | `0xFCE0`, `0xFCE9` | `0x83384` |
| 5820 | `sSliceMap.iNum >= 1` (if `bSliceEncodingMode`) | `0xFDAC` | `0x85d5c` |
| 9881 | `sSVEMap.iNum == 1` (gate [U]) | `0x32D9C` | `0x77e78` |
| `CFlowControllerBase.cpp:358` | wire `0xFF18` < 2 | `0xFF18` | `0x1401c` |
| `Hevc_headers.cpp:756` | temporal_id_nesting with 0 sub-layers | `0x246A0` | `0x1e49c` (assert **and spin**) |
| `Hevc_headers.cpp:945` | num_extra_slice_header_bits == 0 | `0x2848C` | `0x1f5a0` (spin) |
| `Hevc_headers.cpp:861` | VUI re-check | `0x262BC` | `0x1e910` |
| status `0xEE0005` | bFWCreatesHeader mismatch; `0xFF80`/`0xFF7B` rules | `0x26375`/`0x2A60C` | `0x84f8c`–`0x84f90` †, `0x83bb4`–`0x83d18` |
| status `0xEE0003` | param-sets buffer smaller than the headers (checks a stale total on a first Start, so it does not protect) | `0xFB38` | `0x85340`–`0x85368` |
| `ConfigureMCPUs:6930` | input chroma vs SPS chroma | `0xFEB4` | `0x88e98` |

**First frame** (setPipe `0x70778`, `SetTranscode*`, `ProcessTranscodeStart`)

| line | field | Process wire | check VA |
|---|---|---|---|
| 13665 / 13666 | `sInput.Y` ≠ 0, 64-aligned | `0x5E70` | `0x71fa8` |
| 13687 / 13688 | `sInput.UV` | `0x5E80` | `0x72114` |
| **13921 / 13922** | recon Y_LSB ≠ 0, **128**-aligned | `0x5E50` (from Start recon u64 #2) | `0x72cb8` † |
| 13934 / 13935 | recon Y_MSB | `0x5E48` | `0x72ee8` |
| 13955 / 13956 | recon UV_LSB | `0x5E60` | `0x73318` |
| 13968 / 13969 | recon UV_MSB | `0x5E58` | `0x737d4` |
| 14062 / 14063 | src_nbr_info ≠ 0, 128-aligned | `0x5F30` | `0x730cc` |
| 14069 / 14070 | src_nbr_pixels | `0x5F50` | `0x731ec` |
| 14199 / 14200 | entropy[0..1][pbid] ≠ 0 / entropy_size ≠ 0 (**Start** `0xFA30`/`0xFA40`) | `0x5FB0`/`0x5FD0` | `0x73600`, `0x7360c` |
| 14001 / 14002 | LowResSrcLumaScaled (gate [U]) | `0x61D0` | `0x72d54` |
| 13902 | `PPSIdIdx < 9` (slice PPS id not among `0xFD80..`) | S+`0x10` | `0x72b54` † |
| setLRME 11926/11927, 11984/11985 | `sInput.Y`; LowResSrcLumaScaled | | `0x6c058`, `0x6c2dc` |
| 3669 | coded (`0x61B8`) == Start table entry (index `0x61B4`) | | `0x74f54` |
| 7447 / 7448 | entropy addr ≠ 0, `&127 == 0` | Start/PICMGMT | `0x756bc` |
| 7597 / 7598 | coded ≠ 0, `&127 == 0` | Start `0x4B8` | `0x75c94` † |
| 7605 / 7606 | `tmp_bitstream_addr_dst` (other arm; selector [U]) | TranscodedData? | `0x75cec` |
| 8032 / 8965 | scaling mode > 9 or in 3..7 | `0x5CA4` | `0x8d788`, `0x8e688` |
| 2885 | slices per frame ≤ 256 (32-px CTBs) | `0xFDAC..` | `0x654f4` |

Already safe when zero (`cbz`-skipped): LowResResults 13159, `dst_colo`
14040/14041, `Colocated_L1` 14322/14323.

---

## 8. Implementation plan

The order is chosen so that each hardware run adds one mechanism and can
fail in a way that names it.

### 8.1 Code changes (host only, no hardware)

1. **ABI table** (`driver/ave_abi.h`).
   - Add `struct ave_start_hevc_layout`, `ave_hevc_ps_layout`,
     `ave_process_hevc_layout` and a `coded_hdr` extension (§10), and a
     `start_hevc` / `hps` / `process_hevc` member in `struct ave_cmd_abi`.
     Fill them for 13.5 only; 26.6.2 gets `AVE_OFF_NONE` everywhere (its
     `0x13F28`/`0xB1C0` layouts are unread).
   - The `AVE_OP_START_HEVC` / `AVE_OP_PROCESS_HEVC` descriptors already exist
     with the right ids and sizes.
   - Refactor so the ~60 VP/RC offsets in `ave_start_avc_layout` that §2.2
     shows are shared are **not duplicated**: split `ave_start_avc_layout`
     into a shared `ave_vp_layout` (wire offsets within the common
     `0x60..0x105B0` prefix) and the AVC-only SPS/PPS/tail. Both INIT builders
     then use one table.
   - Correct the two comments that §11 flags.
2. **Builders** (`driver/ave_cmd.c`).
   - Factor the VP/RC/buffer-table part of `ave_cmd_build_start_avc` into
     `ave_cmd_fill_vp()`. It is shared, except for: the recon second u64
     (always, for HEVC), the entropy row count, and the 128-byte alignment
     checks (enforce 128 for HEVC in the validator).
   - Add `ave_cmd_build_start_hevc(abi, buf, len, ctx, const struct
     ave_hevc_session *)`, which writes §6. `struct ave_hevc_session` =
     the shared fields plus level_idc, tier, crop, SAO/WPP/TMVP booleans and
     the IPPP RPS.
   - Add `ave_cmd_build_process_hevc(…, const struct ave_hevc_frame *)`: the
     PICMGMT fill from `ave_cmd_build_process_avc`, factored to take a PICMGMT
     base (`0x9C8` vs `0x55B0`); the S fields of §3.2, including 256 slot
     IOVAs; and the slice RPS at `0x6524`.
   - Extend `ave_cmd_coded_length()` to add record+`0x218` when the session is
     HEVC. Return per-slice `{hdr_iova, hdr_len}` so the session can find the
     header bytes. Ignore `cabac_zero_words` for HEVC.
   - `abi_selftest`: pin every §6 offset, the command sizes, and that
     `ave_cmd_build_start_hevc` refuses 0 at `0xFEB4`, `0xFF24`, nesting 0,
     `num_extra_slice_header_bits` ≠ 0, a PPS `header_len` ≠ 0, a flag
     mismatch, a zero recon LSB, or a non-128-aligned entropy or coded
     address.
3. **Session** (`driver/ave_session.c`).
   - `bufs->codec`, and a `session_codec` parameter for the self-test.
   - `ave_session_start_hevc()`, a sibling of `ave_session_start_avc()`, and
     codec 1 in the Open, Stop and Close headers.
   - HEVC buffer sizes (§9):
     - SrcNbr slot = max(Info, Pixel) from the HEVC formulas;
     - entropy per buffer = `2304·ceil(W/32)·max(4, ceil(H/64))`;
     - colocated `128·ceil(W/32)·ceil(H/64)`;
     - recon unchanged.
   - One SliceHeader surface of `0x40000` per coded slot; TranscodedData and
     MBInputCtrl surfaces allocated but not published at first (§8.2 H2
     fallback).
   - `ave_session_process()` builds `HEVC_ENCODE`.
   - `ave_sess_stream_append()` for HEVC: on an IDR, the param sets (from
     `+0x98`, cross-checked by `+0x9C` PSInfo); per slice, `hdr_len` bytes
     from the SliceHeader surface at `hdr_iova`, then the coded bytes; no
     `cabac_zero_words`.
   - Replace the MB-count check by a CTU-count print until the unit is known.
   - POC lsb per frame = frames since IDR mod `1 << (log2_max_poc_lsb_minus4 + 4)`.
   - HEVC level = the smallest `general_level_idc` whose MaxLumaPs (table at
     kext `0xfffffe000723e738`: 30→36864, 60→122880, 63→245760, 90→552960,
     93→983040, 120/123→2228224, 150/153/156→8912896, 180/183/186→35651584)
     holds W×H; floor 4.0 (120), as the AVC driver floors at 4.0. The
     firmware sizes its HEVC DPB itself (`H265VideoEncoderDPB`).
4. **V4L2** (`driver/ave_v4l2.c`).
   - CAPTURE formats `V4L2_PIX_FMT_H264` and **`V4L2_PIX_FMT_HEVC`**. The
     CAPTURE format picks the codec at `STREAMON` (`ave_enc_cfg.codec`).
   - Controls: `V4L2_CID_MPEG_VIDEO_HEVC_PROFILE` (MAIN only; MAIN_STILL_PICTURE
     optional), `HEVC_TIER` (MAIN only), `HEVC_LEVEL`, `HEVC_I_FRAME_QP` /
     `HEVC_P_FRAME_QP` / `HEVC_MIN_QP` / `HEVC_MAX_QP`,
     `HEVC_LOOP_FILTER_MODE` (enabled only),
     `HEVC_REFRESH_TYPE`/`REFRESH_PERIOD` (IDR, mapped onto the existing GOP
     path), `HEVC_SIZE_OF_LENGTH_FIELD` 0 (Annex B).
   - Reuse `GOP_SIZE`, `FORCE_KEY_FRAME`, `BITRATE`, `FRAME_RC_ENABLE`,
     `BITRATE_MODE` and `HEADER_MODE`.
   - Sizes: width multiple of 64 (stride rule, unchanged), height multiple of
     16 via the OUTPUT crop as for H.264; minimum 192×96 (kext HEVC minimum
     160×64).
   - `v4l2-compliance` must stay 54/54 on H.264 and pass on HEVC.

### 8.2 Hardware steps (for the lead; each one boot, one variable)

**H1: `HEVC_INIT` accepted.** `session_selftest=1 session_codec=1`,
1280×720, fixed QP 30, `session_dbg=0x20`, no frame.

- **Expect:**
  - `INIT_DONE` status `0xEE0000`, slot 6;
  - the param-sets buffer starts `00 00 00 01 40 01` (VPS), then
    `… 42 01` (SPS) and `… 44 01` (PPS);
  - coded-header-independent: parse it with a new `tools/hevc_parse.py` (the
    `h264_parse.py` counterpart) and check profile 1, level, 1280×720, CTB
    32, WPP 1, SAO 1, scaling 0.
- **"No" shapes:**
  - `0xEE0005`: the two `bFWCreatesHeader` bytes, or `0xFF7B`/`0xFF80`;
  - `0xEE0002` with no assert: the INIT path, before `ProcessInitStage2`
    (client buffer or priority registration, `0xf3dc`);
  - an assert naming `Hevc_headers.cpp:756/945/861` or
    `CHEVCController_H13C.cpp:5118/241/5385/5575/4838`: the row in §7;
  - no reply at all: a wrong size (impossible if `abi_selftest` passes; the
    `insize` assert spins) or a spinning header assert. The netconsole tail
    and `session_dbg` logs name it.
  - The NALs parse but a field differs: the table offset is wrong. This is
    cheap to fix and needs no reboot to diagnose.

**H2: one IDR frame decodes.** H1 + `session_frame=1`, ramp source, a
source allocation of `ALIGN(h,64)` rows. It is unknown whether the HEVC
fetcher reads whole 32-row CTUs past a 720-line picture; over-allocating
removes the question.

- **Expect:**
  - `ENCODE_DONE`;
  - record 0 has `written` > 0 and `+0x398` > 0; the header slot begins
    `00 00 00 01 28 01` (`IDR_N_LP`);
  - the assembled stream decodes in ffmpeg as HEVC Main 1280×720;
  - `ramp_psnr.py` ≥ ~45 dB (AVC at QP 30 gives 48.6);
  - the slice QP (from the header) is 30.
- **"No" shapes:**
  - an assert: §7's first-frame table names the field.
  - **A blank but decodable frame**: `0x5CA4` is not reaching the quantiser.
    Before reading the Rcr/Md scale registers (MMIO `0x1370764` /
    `0x1330678`), prove each read lies inside a real block (AGENTS.md; f38).
  - **An undecodable stream** with a sane header: a syntax/hardware mismatch.
    Toggle one at a time: WPP (PPS+`0x79` 1 → 0), then SAO (all four
    places), then TMVP.
  - `7605/7606`: publish TranscodedData (VP+`0x548..0x558`, one surface of
    CodedData/2).
  - An MBInputCtrl-related hang: publish the two MBInputCtrl surfaces
    (VP+`0xFC68/70`).
  - `FrameTypeReturned` 4: dropped, as for AVC.

**H3: P frames.** `session_frames=4`, RPS from §2.4, `0x6524` = {1, 0}.

- **Expect:**
  - frames 1–3 are `TRAIL_R` (`02 01`) P slices;
  - PSNR close to the IDR's;
  - sizes far below the IDR's.
- **"No" shapes:**
  - a decode error at frame 1: the RPS or POC lsb. Check them with the parser
    against `slice_pic_order_cnt_lsb`;
  - LowResResult/colocated asserts (§7);
  - drift over frames: SAO/deblocking recon mismatch. Toggle SAO.

**H4: V4L2.** `v4l2-ctl` and `ffmpeg -c:v hevc_v4l2m2m`, 60 frames at 720p,
1080p (crop) and 4K; `v4l2-compliance -s`.

---

## 9. Buffer sizing (kext `AVE_CalcBufSizeOf*`, codec 1)

Every function tests the codec against 0 and 1 *(d)*. Notation:
cW32 = ⌈W/32⌉, cH32 = ⌈H/32⌉, cH64 = ⌈H/64⌉, AU4K = align up to 4 KiB.

| buffer | HEVC | AVC (driver today) | 1080p HEVC | VA |
|---|---|---|---|---|
| ParameterSet | 1024 per layer | 512 | 1024 (driver 4 KiB: fine) | `…ea4bb8` |
| CodedData | same formula | same | 3 112 960 | `…ea4da0` |
| CodedHeader | `0x23000` | same | | `…ea4fb8` |
| **SliceHeader** | **`0x40000`** per coded slot (256 × `0x400`); none for AVC | — | | `…ea4ffc`, count `…ea4fc8` |
| Recon (compressed) | identical to AVC (32×32 tiles; no 64-CTU alignment) | same | 3 297 280 | `…ea528c` |
| Colocated | 128·cW32·cH64 | 128·MBs | 130 560 | `…ea5564` |
| LowResRef | AU512(ALIGN(4W,256)·cH64) | uses `(H+63)>>4` | 130 560 (driver's AVC value is larger, fine) | `…ea5668` |
| LowResResult | same | same | 131 584 | |
| **SrcNbrInfo** | max(192·cW32·(cH64−1), 16K) | 256·MBcols | 184 320 | `…ea5980` |
| **SrcNbrPixel** | 768·cW32·(cH64−1) | 1024·MBcols | **737 280** (driver slot 131 072: **too small**) | `…ea59f8` |
| SrcNbrData / FwData | max(4·cW32, 16K) / 16K | | 16 384 | |
| **EntropyCoding** | 2304·(bd≠8 ? 2 : 1)·cW32·(flag ? cH64 : 4) | ALIGN_DOWN(64W+960,1024)·K | 552 960 or **2 350 080**; use the larger | `…ea5c0c` |
| TranscodedData | CodedData/2, AU4K | same | 1 556 480 | `…ea5b6c` |
| MBInputCtrl | AU4K(ALIGN(W,32)·cH32), count 2 for HEVC when wire `0x6D` = 0 | 0 for AVC | 65 536 | `…ea4bfc` |
| MBStats | AU4K(624·cW32·cH32) | 432·MBs | 1 273 856 (unused by the driver) | `…ea4d3c` |

Recon count: `min(refNum + 1, 17)`, the same as AVC (`…ea5034`).
`HEVC_CalcMaxDpbSize` (`…f49dd4`) gives 16/12/8/6 by picture size relative to
MaxLumaPs: 1080p at L4 = 6, at L5 = 16. The kext uses it only as a limit.
*(d)* The driver's `session_dpb` path stands.

Constraints for macOS HEVC sessions (they bind the kext, not us; *(d)*):
`AVE_HEVC_CheckFrameDimension` minimum 160×64, maximum 16384×8192 or
8192×16384 on DevType 12, no alignment rule (`…efa400`); `AVE_HEVC_CheckInfo`
`iNumViews`, `iLayerNum` ∈ 1..2, `separate_colour_plane` 0, ≤ 7 temporal
layers, and **no profile/level/tier/bit-depth check** (`…ec94d4`).

---

## 10. Proposed header fragment (not applied; `driver/` is off limits for this document)

```c
/*
 * 13.5 HEVC_INIT (id 5, 0x32DC8) and HEVC_ENCODE (id 8, 0x6838). docs/77.
 * VP/RC fields are shared with AVC_INIT at the same wire offsets (docs/77
 * §2.2); only what differs is here.
 */
struct ave_start_hevc_layout {
	u32 vps_block, vps_block_size;		/* 0x105b0, 0x140dc  fw memcpy 0x8303c */
	u32 sps_block[2], sps_block_size;	/* 0x2468c, 0x26580, 0x1ef4  0x83058/0x83090 */
	u32 pps_block[2], pps_block_size;	/* 0x28474, 0x2aa18, 0x25a4  0x83074/0x830ac */
	u32 rps_block, rps_block_size;		/* 0x2cfbc, 0x5dd8   0x830d8 */
	u32 transcode_overlap;			/* 0x32d98 u8 */
	u32 sve_num;				/* 0x32d9c           fw 0x82bc0 */
	u32 sve_index;				/* 0x32dc0 */
	u32 input_format_word;			/* 0xfeb4: 16 = input chroma "same as SPS"; :5118 */
	u32 pps_count;				/* 0xfda4 (1) */
	u32 pps_ids;				/* 0xfd80, 9 x u32 */
	u32 enable_tmvp;			/* 0xfcf8 */
	u32 sao_enb_config, sao_eo_bo;		/* 0xfd10 (0xffff), 0xfd1c (0xffffffff) */
	u32 input_bitdepth;			/* 0xfd20 (8) */
	u32 entropy_rows;			/* 2 (ctrl+3768, fw 0x835bc) */
	u32 buf_align;				/* 128 (setPipe/SetTranscode asserts) */
	u32 transcoded_set, transcoded_size;	/* 0x5a8 / 0x5b8 (read 0x84548..58), layout [I] */
	u32 mb_input_ctrl_set;			/* 0xfcc8, 2 x u64 (read 0x84538/40) */
};

/* Offsets inside each struct; add the block base (and layer base). */
struct ave_hevc_ps_layout {
	/* PTL at +0x18 of VPS and SPS */
	u32 ptl, ptl_profile_idc, ptl_tier, ptl_compat, ptl_progressive,
	    ptl_non_packed, ptl_frame_only, ptl_level_idc;	/* 0x18: +8,+4,+0xc,+0x2c,+0x2e,+0x2f,+0x3c */
	/* VPS */
	u32 vps_id, vps_base_internal, vps_base_available, vps_temporal_nesting,
	    vps_sublayer_info, vps_max_dec_pic_buf_m1, vps_num_layer_sets_m1,
	    vps_timing_present, vps_num_hrd, vps_ext_flag;
		/* 0x4, 0x8, 0x9, 0x14, 0x678, 0x67c, 0x6e0, 0x106e4, 0x106f8, 0x11afc */
	/* SPS */
	u32 sps_layer_id, sps_gate7, sps_vps_id, sps_max_sub_layers_m1,
	    sps_temporal_nesting, sps_id, chroma_format_idc, pic_width, pic_height,
	    ctb_cols, ctb_rows, conf_win_flag, conf_win_left, conf_win_right,
	    conf_win_top, conf_win_bottom, bit_depth_luma_m8, bit_depth_chroma_m8,
	    log2_max_poc_lsb_m4, sublayer_info, max_dec_pic_buf_m1, num_reorder,
	    log2_min_cb_m3, log2_diff_cb, log2_min_tb_m2, log2_diff_tb,
	    tb_depth_inter, tb_depth_intra, scaling_enabled, scaling_present,
	    amp, sao, pcm, tmvp, strong_intra, vui_present, sps_ext,
	    sps_fw_creates_header, sps_header_len;
		/* 0x4, 0x7, 0xc, 0x10, 0x14, 0x238, 0x23c, 0x244, 0x248, 0x24c,
		 * 0x250, 0x254, 0x258, 0x25c, 0x260, 0x264, 0x268, 0x26c, 0x270,
		 * 0x274, 0x278, 0x294, 0x2cc, 0x2d0, 0x2d4, 0x2d8, 0x2dc, 0x2e0,
		 * 0x2e4, 0x2e5, 0x1b90, 0x1b91, 0x1b92, 0x1c2c, 0x1c2d, 0x1c30,
		 * 0x1cd8, 0x1ce9, 0x1cec */
	/* PPS */
	u32 pps_layer_id, pps_id, pps_sps_id, extra_sh_bits, sign_hiding,
	    cabac_init_present, num_ref_l0_m1, num_ref_l1_m1, init_qp_m26,
	    constrained_intra, transform_skip, cu_qp_delta, cu_qp_delta_depth,
	    cb_qp_offset, cr_qp_offset, tiles, wpp, lf_across_slices,
	    deblock_ctrl_present, deblock_override, deblock_disable,
	    pps_scaling_present, lists_mod, par_merge_m2, pps_fw_creates_header,
	    pps_header_len;
		/* 0x4, 0xc, 0x10, 0x18, 0x1c, 0x1d, 0x20, 0x24, 0x28, 0x2c, 0x2d,
		 * 0x2e, 0x30, 0x34, 0x54, 0x78, 0x79, 0x889, 0x88a, 0x88b, 0x88c,
		 * 0x898, 0x2144, 0x2145, 0x2198, 0x219c */
	/* RPS */
	u32 rps_num_st, rps_entry0, rps_entry_stride, rps_num_neg, rps_num_pos,
	    rps_dpoc_s0_m1 /* u16 */, rps_used_s0, rps_num_delta_pocs, rps_lt_present;
		/* 0x0, 0x4, 0x164, +0x30, +0x34, +0x38, +0x58, +0x160, 0x5a68 */
};

struct ave_process_hevc_layout {
	u32 slice, slice_size, slice_fw_copy;	/* 0x40, 0x5570, 0xd6c */
	u32 picmgmt;				/* 0x55b0 (PICMGMT offsets as process_avc) */
	u32 pic_extra;				/* 0x6518, 12 bytes */
	u32 st_rps, lt_rps;			/* 0x6524 (0x16c), 0x6690 (0x1a8) */
	/* inside the slice block */
	u32 sh_size_word, sh_first_slice, sh_pps_id, sh_poc_lsb, sh_tmvp,
	    sh_sao_luma, sh_sao_chroma, sh_col_from_l0, sh_five_minus_merge,
	    sh_lf_across, sh_map, sh_hdr_slots;
		/* 0x0, 0xc, 0x10, 0x28, 0x11c, 0x11d, 0x11e, 0x1aa, 0x3d4, 0x43c,
		 * 0x44c, 0x568 (256 x u64, stride 0x400 in the surface) */
	u32 hdr_slots, hdr_slot_bytes;		/* 256, 0x400 */
};

/* CODED_DATA_HDR, HEVC additions (fw 0x7f1ac/0x7f1cc; kext 0xec4ee0). */
#define AVE135_CODED_SLICE_HDR_IOVA	0x210	/* per record, u64 */
#define AVE135_CODED_SLICE_HDR_LEN	0x218	/* per record, u32, bytes */
#define AVE135_CODED_PSINFO		0x9c	/* count + 5 x {type, layer, bitoff, bits} */
```

---

## 11. Corrections and annotations to earlier documents (not edited here)

1. **docs/72 §2.1 and §0 row 3**: "bit 27 is set for HEVC" comes from the
   H.264 `AVE_ValidateEncoderParameters` (`0x2c85c`), which the HEVC encoder
   never calls. macOS HEVC sends VP+8 = 0 (`0x6ce14`). *(d)*
2. **docs/72 §5.3 / §1.3**: the kext's HEVC check names VP+`0xFEC8`
   `iNumViews` and VP+`0xFEC4` `iLayerNum` (`…ec950c`/`…ec951c`). docs/72 has
   them the other way round. *(d)*
3. **`driver/ave_abi.h` `.src_nbr_set` comment**: "The fourth (wire `0xF830`)
   is a 4x16 table with a size array at `0xFA30`; not read on any path found".
   Both controllers' IEPs read the `0xFA30` sizes (AVC `0x5d744`, HEVC
   `0x84368`). docs/62 §0 already says so; the comment is stale.
4. **`driver/ave_abi.h` `recon_y` "fw dumper `0x3c0c4`"**: that VA is inside
   `CHEVCController::DebugEncode`. The offset is right because PICMGMT is
   shared.
5. **docs/00 Trap 4 / the canary table**: a delegated reading reports that on
   13.5 the `gs_saAVE_DevID_Conversion` row for t6001 (`0xfffffe0007bc2a38`)
   gives ChipType 9, DevType 12 and DevID 15, and that "DevID 12 → ChipType 7"
   is the 26.6.2 numbering. The variant is still `_Nyx`. **Not re-read here**:
   a quick dump did not reproduce the row stride. Check before correcting.
6. **The brief's "vtable+488 = InitEncodingParameters"**: vtable+488 is
   `ProcessInit` (AVC `0x4dde8`, HEVC `0x68154`), which then calls vtable+536
   = IEP. This refines docs/62 §0.3's description of `0x46410`. *(d)*

---

## 12. Open items, and what was re-read

**Open**

- The `0x6518` 12-byte block's meaning. Zero is what the kext sends, and the
  firmware tolerates it.
- The gate on `tmp_bitstream_addr_dst` (`[x22,#426]`), and whether
  TranscodedData / MBInputCtrl are needed for a single-core encode.
- Whether the HEVC source fetcher reads past the display height to a whole
  CTU row. Over-allocate in H2.
- The units of the I/P/skip counts in the coded header.
- POC lsb: whether the firmware recomputes S+`0x28` (it sets 0 for an IDR,
  `0x215bc`) or trusts the host.
- The macOS per-frame value at PICMGMT+`0x6F4` for HEVC (inferred 1).
- `skip_mode` 1/2 semantics in HEVC, AMP's hardware enable, and the meaning of
  `sao_eo_bo` = 0.

**Delegated and re-read here with `disas.py`**

- the kext Start_HEVC builder `…ea9ec4`–`…eaa320` (whole function);
- `AVE_Client_Config`'s HEVC arm `…ecc2e4`–`…ecc35c`;
- fw `ProcessHevcInit` `0xf2c8`;
- `ProcessInitStage2` `0x13dd8`–`0x14404`;
- `ProcessHevcEncode` `0xfb90`–`0xfd8c`;
- `SendCommandToQueue` `0x14580`–`0x145ec`;
- IEP block copies `0x82ff4`–`0x83120`;
- the flag test `0x84f4c`–`0x84f90`;
- the scaling-mode load `0x8dfac`–`0x8dfec` and its PICMGMT chain
  `0x651dc`–`0x651e8`, `0x64cbc`–`0x64db4`;
- the recon-LSB check `0x72cb4`;
- the coded 128-alignment check `0x75c88`–`0x75c98`;
- the PPS-id loop `0x72aa0`–`0x72b54`;
- the transcoder PPS/SPS packing `0x75ad4`–`0x75b98`;
- `WriteSliceHeadersHevc` `0x7f194`–`0x7f1ec`;
- `RetrieveRCStats` `…ec4ec0`–`…ec4efc`;
- the Process_HEVC copies `…eace08`–`…eace7c`;
- kext `GenerateMap` `…f4fa84`–`…f4fdc8`;
- user-space `0x6cec0`–`0x6cecc`, `0x6d080`–`0x6d09c`, `0x6d230`–`0x6d24c`,
  `0x6d320`–`0x6d34c` and `0x73750`–`0x73768`.

The remaining *(d)* rows come from four delegated passes whose working files
(annotated dumps, a CFG store lister, an assert inventory of all 774 sites)
are in the session scratchpad, not the repository.

---

## 13. Reproduce

```sh
export AVE_MACOS=13.5
D="python3 tools/disas.py"

# commands and codec
$D --fw   --addr 0xd954  -n 0x30        # HEVC_INIT size check 0x32DB0+0x18
$D --kext --addr 0xfffffe0008ea9ec4 -n 0x480   # MakeFwCmd_Start_HEVC (block map)
$D --kext --addr 0xfffffe0008ecc2e4 -n 0x80    # AVE_Client_Config HEVC arm (pInfo offsets)
$D --fw   --addr 0x13e48 -n 0x40        # ProcessInitStage2: x23 = cmd+0xFCE9, HEVC tail
$D --fw   --addr 0xfcc0  -n 0x50        # ProcessHevcEncode copies (0xD6C, 0xF68, 0xC, 0x314)
$D --kext --addr 0xfffffe0008eace08 -n 0x78    # Process_HEVC: id 8/9, PICMGMT, slice, RPS

# HEVC_INIT inside the firmware
$D --fw --addr 0x82ff4 -n 0x130        # block copies, RC reads (x20 = VP, x26 = VP+0xF770)
$D --fw --addr 0x84f4c -n 0x48         # bFWCreatesHeader pair
$D --fw --addr 0x75ad4 -n 0xc8         # SPS/PPS fields packed into transcoder registers

# scaling lists
$D --fw --addr 0x8dfac -n 0x44         # mode = PICMGMT+0x6F4; mode 0 = 0x10000010
$D --fw --addr 0x651d8 -n 0x14         # ctrl[9144] = PICMGMT + 8
$D --fw --addr 0x64cb8 -n 0x100        # x23 = PICMGMT in PipePrepareParam

# output
$D --fw   --addr 0x7f194 -n 0x5c       # slice header -> slot, record +0x210/+0x218
$D --kext --addr 0xfffffe0008ec4ec0 -n 0x40    # HEVC adds record+0x398 to the byte count

# asserts (w2 = line, x0 = file)
$D --fw --addr 0x72a70 -n 0xe8         # :13902 and the PPS-id loop
$D --fw --addr 0x72cb4 -n 0x10         # recon Y_LSB non-zero / 128-aligned
$D --fw --addr 0x75c88 -n 0x14         # coded buffer 128-aligned (:7597/7598)

# user space (docs/72 §9 builds the listing)
grep -nE '^0x0006cec[0-9a-f]|^0x0006d09[0-9a-f]|^0x0007376[0-9a-f]' /tmp/ave_ua.s
```

---

## 14. After h2: the transcoder outputs (TranscodedData) and PICMGMT+0xF65

h2 (docs/53) got through HEVC_ENCODE's setup to `SetTranscode` and asserted
`CHEVCController_H13C.cpp:7605: tmp_bitstream_addr_dst[xc_index] != 0`, with
`tmp_bitstream_addr_dst[0]` logged as 0. §2.7 and §7 had the right fields
but the wrong reading of the selector: `[x22,#426]` is not "of unknown
origin", and 7605/7606 is not a fallback arm. Every VA below was read for
this section (13.5 firmware / kext). **[C]** unless marked.

**Where `tmp_bitstream_addr_dst[xc]` comes from.**

| step | what | VA |
|---|---|---|
| HEVC IEP, x20 = VP | VP+`0x548` → ctrl+`0x2090`, VP+`0x550` → ctrl+`0x2098`, u32 VP+`0x558` → ctrl+`0x20A0` | fw `0x84548`–`0x8455c` |
| wire | VP + `0x60`: **`0x5A8`, `0x5B0` (u64), `0x5B8` (u32 size, one for both)** | — |
| ProcessTranscodeStart | `ldr q0,[x19,#8336]` → `tmp_bitstream_addr_dst[0..1]` = ctrl+`0x2C508`/`0x2C510` | fw `0x74d88`–`0x74dac` |
| ProcessTranscodeStart | `SetTranscode(this, 0)`, then `SetTranscode(this, 1)` unless ctrl+`0x2418E` ≠ 0 | fw `0x74dc8`–`0x74de4` |
| SetTranscode(xc) | `ldrb w9,[x22,#426]` (x22 = ctrl+`0x23FE4`, so ctrl+`0x2418E`): ≠ 0 → `curr_bitstream_addr_dst` (ctrl+`0x2088`), asserts :7597/:7598; = 0 → `tmp_bitstream_addr_dst[xc]` (ctrl+`0x2C508`+8·xc), asserts **:7605** (≠ 0) / **:7606** (`& 127 == 0`) | fw `0x75c64`–`0x75d1c` |
| ctrl+`0x2418E` | written per frame from **PICMGMT+`0xF65`** (`ldrb w9,[x23,#3941]`, x23 = PICMGMT; `strb w9,[x22,#308]`, x22 = ctrl+`0x2405A`) | fw `0x74f64`, `0x74f70` |
| after the frame | with the byte 0, the firmware maps both TranscodedData buffers (size ctrl+`0x20A0`) and the coded buffer and memcpy's the slice data into the coded buffer | fw `0x7e4f8`–`0x7e61c` |

So `xc_index` is 0 **and** 1: with PICMGMT+`0xF65` = 0 there are two
transcoders, each with its own buffer, and both entries must be non-zero
and 128-aligned. With `0xF65` ≠ 0 there is one transcoder, writing straight
into the coded buffer, and TranscodedData is never read.

**What the kext does.**

- `AVE_CHM_SetDataInfo_Frame` writes `PICMGMT+0xF65 = (client+0xE0E2C > 0)`
  for every frame, whatever the codec (`strb w8,[x20,#3941]`
  `0xfffffe0008eaaf14`, x23 = client+`0xE0D84`, `[x23,#168]`).
- `AVE_CalcBufNumOfTranscodedData` (`0xfffffe0008ea5b18`): 0 below DevType 5,
  2 below DevType 9, and from DevType 9 up (M1 Max is 12, docs/47) **2 when
  client+`0xE0E2C` == 0**, else 0. The same field selects both, so macOS
  never has 0xF65 = 0 without the pair.
- Size, `0xfffffe0008ea5b44`: `align4K(CalcBufSizeOfCodedData(...) >> 1)`,
  one size for both.
- `AVE_CHM_SetFwBuf` publishes them **once, in the INIT command**: for
  k = 0, 1, VP+`0x548`+8k = the surface's IOVA and VP+`0x558` = its size
  (`str x0,[x23,#1352]` `0xfffffe0008eaf280`, `str w0,[x22,#1368]`
  `0xeaf28c`, loop `0xeaf250`–`0xeaf2a8`). So they are **per session**, not
  per coded slot and not per frame, and not distinct per slot.
- What sets client+`0xE0E2C` was not traced. It is loaded as `[x23,#304]`
  (x23 = client+`0xE0CFC`) in `AVE_Client_CalcSurfaceInfo` `0xfffffe0008ec69e8`.
  Which value macOS uses for an HEVC session is **[U]**.

**MBInputCtrl is not needed.** VP+`0xFC68`/`0xFC70` → ctrl+`0x13E8`/`0x13F0`
(fw `0x84538`/`0x84540`) is read only by `0x2a6b0`, which asserts
`fwDataAddr != 0 && & 127 == 0` (CAVECommonController.cpp:4099, `0x2b458`).
Its only caller, PipePrepareParam `0x651bc`, runs it only when PICMGMT+`0xF50`
> 0 (`ldr w8,[x23,#3920]` `0x64ee0`; `b.lt` `0x64ef0`). That field is the
count of 64-byte regions copied from PICMGMT+`0xCD0` (`0x64ec4`–`0x64edc`), and
we send 0.

**What the driver does now.**

- Default, `session_hevc_xc=2`: two TranscodedData surfaces of
  `align4K(coded / 2)` (676 KiB at 720p), page-aligned, allocated at
  HEVC_INIT, published at wire `0x5A8`/`0x5B0` with the size at `0x5B8`.
  PICMGMT+`0xF65` stays 0. This is what the kext sets up when it allocates
  the pair.
- `session_hevc_xc=1`: nothing published, and PICMGMT+`0xF65` = 1 (wire
  `0x6515`) in every HEVC_ENCODE, so one transcoder writes into the coded
  buffer.
- The builder refuses a TranscodedData table that is partial, has a zero
  entry, is not 128-aligned, or has no size. `ave_abi.h` has the offsets as
  `start_hevc.transcoded_*` and `process_hevc.pic_single_xc`.

**Reproduce**

```sh
$D --fw   --addr 0x84538 -n 0x2c         # IEP: MBInputCtrl, TranscodedData pair + size
$D --fw   --addr 0x74d88 -n 0x60         # ProcessTranscodeStart: tmp[0..1], SetTranscode(0)/(1)
$D --fw   --addr 0x74f60 -n 0x14         # PICMGMT+0xF65 -> ctrl+0x2418E
$D --fw   --addr 0x75c64 -n 0xbc         # SetTranscode: the two arms, :7597/:7598, :7605/:7606
$D --fw   --addr 0x7e4f8 -n 0x130        # merge of the two outputs into the coded buffer
$D --kext --addr 0xfffffe0008eaaf08 -n 0x10    # kext: PICMGMT+0xF65 = client+0xE0E2C > 0
$D --kext --addr 0xfffffe0008ea5b14 -n 0x74    # count and size of TranscodedData
$D --kext --addr 0xfffffe0008eaf250 -n 0x5c    # SetFwBuf: VP+0x548/0x550, VP+0x558
$D --fw   --addr 0x64ee0 -n 0x14         # MBInputCtrl only with PICMGMT+0xF50 > 0
```
