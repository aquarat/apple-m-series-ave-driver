# `VideoParams.ui32Width` / `ui32Height` — display or MB-aligned?

*Settling, statically, what the host must put at `sCAveCmdAvcStart + 0x368`
and `+ 0x36C` for a 1920x1080 AVC encode. The short answer: **the macroblock
grid the AVC firmware uses does not come from these fields at all — it comes
from the SPS — and every derivation the firmware does make from `ui32Height`
rounds up, so 1080 and 1088 produce byte-identical hardware programming at
1080p.** Recommendation and the one place the two choices differ are in §7.*

Every row is marked **confirmed** (read out of an instruction, VA cited),
**inferred** (a chain of reasoning over confirmed facts) or **unknown**, per
[00-methodology.md](00-methodology.md).

Firmware VAs are **image virtual addresses** (`__TEXT` vmaddr 0, fileoff
`0x4000`). Addresses from `strings -t x data/blobs/ave_h13c.bin` are **file
offsets** and are `VA + 0x4000`; both forms are given where a string is cited.

Reproduce any line with:

```sh
python3 tools/disas.py --fw   --addr 0x6b8b0 -n 0x2b90   # CAVCController::InitEncodingParameters
python3 tools/disas.py --fw   --addr 0x6e90c -n 0x140    # CAVCController::GetPicCropInfoFlag
python3 tools/disas.py --fw   --addr 0x9c924 -n 0x1b0    # CHEVCController::GetPicCropInfoFlag
python3 tools/disas.py --fw   --addr 0x41c8c -n 0x680    # AVC_SPS::seq_parameter_set_rbsp
python3 tools/disas.py --fw   --addr 0x74d70 -n 0x40     # CAVECommonController::ProcessPixHistogram
python3 tools/disas.py --fw   --addr 0xb44f0 -n 0x70     # CAVCController::setLRME, picture-size reg
python3 tools/disas.py --fw   --addr 0xb54dc -n 0x60     # CAVCController::setPipe, picture-size regs
python3 tools/disas.py --kext --addr 0xfffffe0008b91228 -n 0x20   # settings -> client memcpy
python3 tools/disas.py --kext --addr 0xfffffe0008b672f4 -n 0x20   # client  -> command memcpy
python3 tools/disas.py --kext --addr 0xfffffe0008ba2660 -n 0x9c   # AVE_Enc_AlignDimension
```

This closes the open item in [37-start-avc-session.md](37-start-avc-session.md)
§11 ("whether `width`/`height` at `0x368`/`0x36C` are display or MB-aligned")
and makes the §12 experiment 1 optional rather than necessary — see §8.

---

## 0. The result that dissolves the question

Three facts, each read out of an instruction:

1. **The AVC macroblock grid is taken from the SPS, not from `ui32Height`.**
   `CAVCController::InitEncodingParameters` copies `pic_width_in_mbs_minus1`
   and `pic_height_in_map_units_minus1` out of the host-supplied
   `H264_SEQUENCE_HEADER_PARAMS`, adds one, and stores them as the controller's
   MB dimensions.
2. **No firmware or kext code divides `ui32Height` by 16 exactly.** Every
   MB-count or row-count derived from it uses `(h + 15) >> 4`, `(h + 31) >> 4`,
   `(h + 63) >> 6` or similar. The single exception in either binary is an
   `os_log` format argument.
3. **The SPS the firmware emits is written verbatim from the host's block.**
   `AVC_SPS::seq_parameter_set_rbsp` neither derives nor checks
   `pic_height_in_map_units_minus1` or `frame_crop_bottom_offset` against
   `ui32Height`.

Consequence: at 1920x1080, `ui32Height = 1080` and `ui32Height = 1088` produce
**identical** MB grids (120x68), identical MMIO picture-size registers, and
identical kext buffer sizes. The ambiguity is real but, for the geometry, inert.

---

## 1. The field identification, re-confirmed from a string

[20-command-structs.md](20-command-structs.md) §3.3 and
[37-start-avc-session.md](37-start-avc-session.md) §0 place `ui32Width` at
`Start + 0x368` and `ui32Height` at `+0x36C` from a block map. The firmware
names them itself. `CHEVCController::DebugInit` prints the same payload
offsets with those names:

```
8233c:    ldr  w8, [x20, #768]     ; payload + 768  = cmd + 0x368
82340:    adrp x2, 0x128000
82344:    add  x2, x2, #0x9bb      ; VA 0x1289bb = file 0x12c9bb = "ui32Width: %d"
...
82368:    ldr  w8, [x20, #772]     ; payload + 772  = cmd + 0x36C
8236c:    adrp x2, 0x128000
82370:    add  x2, x2, #0x9c9      ; VA 0x1289c9 = file 0x12c9c9 = "ui32Height: %d"
```

**Confirmed.** The AVC `DebugInit` prints the controller copies with the
plainer `"width: %d\n"` / `"height: %d\n"` (VA `0x1266c7` / `0x1266d2`, file
`0x12a6c7` / `0x12a6d2`, loaded at `0x64640` / `0x64670`).

`InitEncodingParameters` moves them into the controller at `0x6c078`–`0x6c090`:

```
6c078:  ldr w8, [x24, #768]        ; x24 = cmd + 0x68
6c084:  str w8, [x20, #3960]       ; this + 3960 = ui32Width
6c088:  ldr w8, [x24, #772]
6c090:  str w8, [x20, #3964]       ; this + 3964 = ui32Height
```

**Confirmed.** Everything below refers to controller fields `+3960` / `+3964`.

---

## 2. The MB grid comes from the SPS block

`x28 = this + 0x247C0` (`add x9, x20, #0x24, lsl #12` at `0x6c010`,
`add x28, x9, #0x7c0` at `0x6c014`), and the SPS block is memcpy'd to
`x28 + 4 = this + 0x247C4` (`add x0, x28, #0x4` at `0x6c020`, `mov w2,#0x6b4`).
So `[x28, #N]` is `SPSparams + (N - 4)`.

```
6c384:  ldr w10, [x28, #1076]      ; SPSparams + 1072 = pic_width_in_mbs_minus1
6c394:  add w10, w10, #0x1
6c3d0:  str w10, [x20, #3980]      ; this + 3980 = MB width

6c3dc:  ldr w8,  [x28, #1080]      ; SPSparams + 1076 = pic_height_in_map_units_minus1
6c3e8:  add w8,  w8, #0x1
6c3ec:  str w8,  [x20, #3976]      ; this + 3976 = MB height
```

**Confirmed.** The struct offsets `1072` / `1076` are the ones
[37](37-start-avc-session.md) §3 recovered from `DebugInit`'s format strings
`"SPSparams.pic_width_in_mbs_minus1 %d"` (file `0x12ac71`) and
`"SPSparams.pic_height_in_map_units_minus1 %d"` (file `0x12ac97`).

The names of `+3976` / `+3980` are then confirmed independently by the HEVC
controller's crop log, `"clipCropDebug: spsWidthHeight %d,%d spsCropRightBottom
%d,%d validMBWidthHeight %d,%d originalMBWidthHeight %d,%d"` (file `0x130060`,
VA `0x12c060`, referenced at `0x9caa4`). Matching the stack slots the varargs
are written into (`0x9ca60`–`0x9cab8`):

| controller field | log name |
|---|---|
| `+3976` | `originalMBHeight` |
| `+3980` | `originalMBWidth` |
| `+3984` | `validMBWidth` |
| `+3988` | `validMBHeight` |
| `+9644` | crop-derivation-applied flag |
| `+9648` | crop-info flag (see §4) |

**Confirmed** for the HEVC controller; **inferred** (identical class layout,
identical code shape) that the AVC controller's fields carry the same meaning.

**The `ui32Width` / `ui32Height` fields never touch `+3976` / `+3980` on the AVC
path.** Negative-result discipline (docs/00 trap 2): the same disassembly scan
that fails to find such a write *does* find the two writes above and *does*
find the HEVC controller computing `+3976`/`+3980` **from** `ui32Width`/
`ui32Height` at `0x9de5c`–`0x9de94` — so the test discriminates.

---

## 3. Every consumer of `ui32Height` in the firmware

All reads of `[reg, #3964]` in the whole `__text`, filtered to the AVC and
common controllers by symbol attribution. (Reads at `0xbb038`/`0xbb03c` are a
false positive: there `x12` is an `{offset, value}` register-descriptor table,
not `this`.)

| VA | function | what it does with the height |
|---|---|---|
| `0x608d8` | `ProcessDataFromCpusMultiCore` | passes `w,h` to `AVE_RC_CalculateDistortion`; that divides an accumulated distortion by `w*h` (`mul w24, w7, w6` at `0xf870`) |
| `0x64670` | `DebugInit` | prints it |
| `0x6610c` | `ProcessLRMEDone` | `ProcessPixHistogram` — see §5 |
| `0x683cc` | `ProcessLRMEStatsMcore` | `ProcessPixHistogram` — see §5 |
| `0x688e8` | `ProcessTranscodeStart` | `CodedBufSize > (3*w*h)/4` check (`mul`+`add w10,w10,w10,lsl #1`+`cmp w9, w10, lsr #2`) |
| `0x6c79c` | `InitEncodingParameters` | `client[1] = (w > 4096 \|\| h > 4096)` — docs/37 §5.11 |
| `0x6d908` | `InitEncodingParameters` | `(h + 63) >> 6` rows of a `align_up(4*w,128)`-stride buffer |
| `0x6e11c` | `InitEncodingParameters` | `H264VideoEncoderDPB` ctor: `(h + 35) >> 5` 32-px tile rows (`0x1af7c`–`0x1af80`) |
| `0x7019c` | `GatherSharedFrameStats` | `AVE_RC_CalculateDistortion` again |
| `0x71724` | `SetLRMERCPerPass` | `h + 255`, then `>> 8` |
| `0x767e0` | `computeWeights` | `w*h` compared with `0xE1000` (921600 = 1280x720) |
| `0x7c8bc` | `PerformDeltaQPMod` | `h + 31`, then `>> 5` |
| `0xb450c` | `setLRME` | **`(h + 15) >> 4`** into an MMIO picture-size register |
| `0xb54fc` | `setPipe` | **`(h + 15) >> 4`** into an MMIO picture-size register |
| `0xb5564` | `setPipe` | **`(h + 15) >> 4`** into a second MMIO picture-size register |

**Confirmed.** Not one of them divides by 16 exactly, and not one of them is a
source-fetch row count.

The register writes, quoted because they are the ones that reach hardware:

```
; CAVCController::setLRME
b4508:  ldr  w8, [x20, #3960]      ; ui32Width
b450c:  ldr  w9, [x20, #3964]      ; ui32Height
b4514:  add  w8, w8, #0x1f
b4518:  lsl  w9, w9, #12
b451c:  lsr  w8, w8, #4            ;  (w + 31) >> 4
b4520:  add  w9, w9, #0xf, lsl #12 ;  (h + 15) << 12
b4524:  and  w8, w8, #0x7fe        ;  ... rounded down to an even MB count
b4528:  and  w9, w9, #0x7ff0000    ;  ((h + 15) >> 4) in bits 16..26
b452c:  orr  w8, w9, w8
b4534:  stur w8, [x9, #-8]         ; MMIO

; the very next register, from the SPS grid instead
b4538:  ldr  w8, [x20, #3980]      ; MB width  = pic_width_in_mbs_minus1 + 1
b453c:  ldr  w9, [x20, #3976]      ; MB height = pic_height_in_map_units_minus1 + 1
b4544:  and  w8, w8, #0x7ff
b4548:  bfi  w8, w9, #16, #11
b4554:  str  w8, [x9]              ; MMIO
```

```
; CAVCController::setPipe
b54dc:  ldr  w9,  [x20, #3980]     ; MB width  (SPS)
b54e0:  ldr  w10, [x20, #3976]     ; MB height (SPS)
b54e8:  and  w9, w9, #0x1fff
b54ec:  bfi  w9, w10, #16, #13
b54f4:  stur w9, [x10, #-8]        ; MMIO

b54f8:  ldr  w9,  [x20, #3960]     ; ui32Width
b54fc:  ldr  w10, [x20, #3964]     ; ui32Height
b5500:  add  w11, w9, #0xf
b5504:  lsl  w9, w10, #12
b5508:  add  w9, w9, #0xf, lsl #12
b5510:  and  w9, w9, #0x1fff0000   ; ((h + 15) >> 4) in bits 16..28
b5514:  bfxil w9, w11, #4, #13     ; ((w + 15) >> 4) in bits 0..12
b5528:  stur w9, [x10, #-8]        ; MMIO
```

At 1920x1080: `(1920+31)>>4 & ~1 = 120`, `(1920+15)>>4 = 120`,
`(1080+15)>>4 = 68`, `(1088+15)>>4 = 68`. **Both candidate heights give 68**,
and both match the SPS-derived `68` written to the neighbouring register.
**Confirmed.**

### There is no source-fetch row count derived from `ui32Height`

`ProcessCmd_Start` performs no range check on any payload field
(docs/37 §5.11, re-checked), `InitEncodingParameters` contains no comparison of
`+3964` against `+3976`, and none of the fourteen reads above feeds a DMA row
count. The number of luma rows the encoder touches is `16 * (
pic_height_in_map_units_minus1 + 1)` = **1088 regardless of what is in
`ui32Height`** — see §6 for the consequence, which matters more than the
question this document set out to answer.

---

## 4. The SPS writer copies the crop verbatim; it derives nothing

`AVC_SPS::seq_parameter_set_rbsp` (`0x41c8c`). `x19` is the `AVC_SPS` object,
`x8 = [x19]` is the `H264_SEQUENCE_HEADER_PARAMS` the host supplied, and
`[x19, #1048]` is the bit writer. Primitives, fixed by their argument shapes
and by the field order they emit: `0xe8784` = `u(1)`, `0xe8934(v, n)` = `u(n)`,
`0xe8a18` = `se(v)`, `0xe8b24` = `ue(v)`.

```
421f0:  ldr  w1, [x8, #1072]  ; pic_width_in_mbs_minus1          -> ue()
42204:  ldr  w1, [x8, #1076]  ; pic_height_in_map_units_minus1   -> ue()
42218:  ldr  w1, [x8, #1080]  ; frame_mbs_only_flag              -> u(1)
4222c:  cbz  w9, 0x423f8      ; if !frame_mbs_only_flag: mb_adaptive_frame_field_flag
                              ;   from [x19, #20] (the object, not the params) -> u(1)
42230:  ldr  w9, [x8, #1084]  ; direct_8x8_inference_flag
42234:  ldrb w1, [x8, #1176]  ; frame_cropping_flag
42240:  bfi  w1, w9, #1, #1
42244:  bl   0xe8934 (w2 = 2) ; u(2): direct_8x8_inference_flag, frame_cropping_flag
42250:  ldrb w9, [x8, #1176]
42258:  b.ne 0x422ac          ; if frame_cropping_flag == 1:
42260:      ldr w1, [x8, #1180]   ; frame_crop_left_offset       -> ue()
42274:      ldr w1, [x8, #1184]   ; frame_crop_right_offset      -> ue()
42288:      ldr w1, [x8, #1188]   ; frame_crop_top_offset        -> ue()
4229c:      ldr w1, [x8, #1192]   ; frame_crop_bottom_offset     -> ue()
422b0:  ldrb w1, [x8, #1088]  ; vui_parameters_present_flag      -> u(1)
```

**Confirmed. Every one of those is a straight load from the host's block.**
Nothing in the function reads `ui32Width`, `ui32Height`, or any controller
field — the writer only has the params pointer. So `bFWCreatesHeader = 1` does
*not* mean the firmware picks the crop: it means the firmware serialises the
crop the host chose.

New offset for [37](37-start-avc-session.md) §3's table: **`SPSparams + 1084`
(`Start + 0x2D58`) is `direct_8x8_inference_flag`**, emitted as the high bit of
the `u(2)` at `0x42244`. It was previously an unlisted gap between
`frame_mbs_only_flag` (`+1080`) and `VUI.vui_parameters_present_flag`
(`+1088`). **Confirmed.**

The writer also stores the finished bit length back into `SPSparams + 1200`
(`0x422f0`–`0x422fc`: `ldp w8,w9,[x8,#8]` then `w8 + (w9 << 3)`), which is why
`header_len` must be 0 on input (docs/37 §5.2).

### What the firmware *does* do with the crop

`CAVCController::GetPicCropInfoFlag` (`0x6e90c`), called from
`InitEncodingParameters` at `0x6d950`. `x11 = this + 0x247E8 = SPSparams + 36`.

(listed in execution order; `0x6e924` is interleaved with the stores by the
scheduler)

```
6e91c:  ldr  w10, [x0, #3980]      ; originalMBWidth
6e920:  ldr  w9,  [x0, #3976]      ; originalMBHeight
6e924:  ldrb w12, [x11, #1140]     ; SPSparams + 1176 = frame_cropping_flag
6e928:  str  w10, [x0, #3984]      ; validMBWidth  = originalMBWidth   (default)
6e92c:  str  w9,  [x0, #3988]      ; validMBHeight = originalMBHeight  (default)
6e930:  str  w8,  [x0, #9648]      ; crop-info flag = 0 (w8 = 0, from movi v0.2d,#0)
6e934:  str  w8,  [x0, #9644]      ; crop-derivation-applied = 0
6e938:  cmp  w12, #0x1
6e93c:  b.ne 0x6ea40               ; not cropping -> done
6e940:  ldr  w12, [x11]            ; SPSparams + 36  = chroma_format_idc
6e94c:  ldr  w13, [x11, #4]        ; SPSparams + 40  = separate_colour_plane_flag
                                   ; ChromaArrayType == 0 -> no crop maths
/* chroma_format_idc == 1 (4:2:0), 0x6e97c: */
6e97c:  ldr  w12, [x11, #1156]     ; SPSparams + 1192 = frame_crop_bottom_offset
6e980:  ldr  w11, [x11, #1148]     ; SPSparams + 1184 = frame_crop_right_offset
6e988:  orr  w10, w14, w10, lsl #4 ; 16*originalMBWidth + 14
6e994:  sub  w10, w10, w11, lsl #1 ;   - 2 * frame_crop_right_offset
6e9a8:  lsr  w10, w10, #4          ; -> validMBWidth  = ceil(visibleW / 16)
6e9a0:  orr  w9,  w14, w9,  lsl #4 ; 16*originalMBHeight + 14
6e9b0:  sub  w9,  w9,  w12, lsl #1 ;   - 2 * frame_crop_bottom_offset
6ea30:  lsr  w9,  w9,  #4          ; -> validMBHeight = ceil(visibleH / 16)
6e98c:  tst  w12, #0x7             ; bottom crop not a whole MB?  -> flag bit 1
6e99c:  tst  w11, #0x7             ; right  crop not a whole MB?  -> flag bit 0
```

**Confirmed.** Three things follow:

* The firmware's idea of the **visible** picture is
  `16 * originalMB - CropUnit * crop`, i.e. it comes from the SPS crop, not
  from `ui32Height`. The two quantities are independent by construction.
* The crop unit is `SubWidthC` / `SubHeightC` **only** — `2` and `2` for
  4:2:0 (`sub ..., lsl #1` in both directions at `0x6e994` / `0x6e9b0`), `2`
  and `1` for 4:2:2 (`0x6e9d8` / `0x6ea28`), `1` and `1` for 4:4:4
  (`0x6ea0c` / `0x6ea28`). **`frame_mbs_only_flag` is never read here.** The
  standard's `CropUnitY = SubHeightC * (2 - frame_mbs_only_flag)` therefore
  only agrees with the firmware when `frame_mbs_only_flag = 1`. Set it to 1.
* The crop-info flag at `+9648` records "the visible edge is not on a
  macroblock boundary" per axis. For 1080p 4:2:0 with
  `frame_crop_bottom_offset = 4` it is **2**, i.e. the firmware knows the
  bottom 8 luma rows are padding. **Confirmed** by arithmetic, and by the HEVC
  twin at `0x9c924` whose log names all these fields.

---

## 5. The one computation where 1080 and 1088 genuinely differ

`CAVECommonController::ProcessPixHistogram` (`0x74d70`), called twice on the
AVC path — `ProcessLRMEDone+0x18c` (`0x66108`) and
`ProcessLRMEStatsMcore+0x1e10` (`0x683c8`) — with **both** dimension pairs:

```
66100:  ldr  w1, [x19, x25]
66104:  ldrb w2, [x19, x26]
66108:  ldr  w5, [x19, #3960]      ; a5 = ui32Width
6610c:  ldr  w6, [x19, #3964]      ; a6 = ui32Height
66114:  lsl  w3, w8, #4            ; a3 = 16 * originalMBWidth
66118:  lsl  w4, w9, #4            ; a4 = 16 * originalMBHeight
6611c:  bl   0x74d70
```

and inside:

```
74d90:  mul  w8, w4, w3            ; codedArea = 16*mbW * 16*mbH
74d94:  msub w8, w6, w5, w8        ; diff = codedArea - ui32Width*ui32Height
74d9c:  add  w9, w8, #0xf
74da4:  csel w9, w9, w8, lt
74da8:  asr  w9, w9, #4            ; diff >= 0 : ceil(diff / 16)
74dac:  cmn  w8, #0xf
74db0:  csel w9, wzr, w9, lt       ; diff < -15 : 0
74dc8:  dup  v0.4s, w9             ; subtracted from each histogram bin,
74e88:  sub  v30.4s, v27.4s, v0.4s ;   clamped at 0 by the smax below
74e8c:  smax v29.4s, v30.4s, v1.4s
```

**Confirmed.** This is a padding correction: the histogram is accumulated over
the whole macroblock grid, and `diff` is the number of padded luma samples.
It is **only meaningful if `ui32Width * ui32Height` is the true (display)
picture area**; with MB-aligned dimensions `diff` is identically zero and the
correction is a no-op.

This is the single piece of evidence in either binary that points at the
display convention, and it is a statistics adjustment, not a correctness
constraint. Weighed against it:

| evidence | points at | strength |
|---|---|---|
| `AVE_Enc_AlignDimension` (`0xfffffe0008ba2660`) rewrites `*pW`/`*pH` in place to `max(align16(d), devcapMin)` for `clientType` 1 and 2 | MB-aligned | the only *statement of intent* about this field pair in either binary; **no caller** in the kernelcache (branch scan; the same scan finds three callers of `AVE_Enc_CheckResolution`, so it discriminates), so it is documentation of the contract, not enforcement |
| `AVE_Client_Enc_PrintAVC+0x7d4` (`0xfffffe0008b7d464`) computes `nmb = (w >> 4) * (h >> 4)`, truncating | MB-aligned | **discredited.** `AVE_Client_Enc_PrintHEVC+0x818` (`0xfffffe0008b7ea38`) uses the identical idiom, and on the HEVC path the firmware provably rounds up for itself (`((d + 31) >> 4) & ~1` at `0x9de5c`–`0x9de94`), so the value it is fed need not be aligned. It is a sloppy `os_log` argument, and it is the *only* truncating `>>4` on these fields in the whole kernelcache |
| `AVE_CalcBufSizeOfRecon` (`0xfffffe0008b606c0`) and the DPB ctor (`0x1af6c`) size buffers as `ceil((d + 4) / 32)` 32-px tiles | MB-aligned | the `+4` is a margin over the *coded* dimension. With `h = 1088` you get 35 tile rows (1120 px, margin intact); with `1080` you get 34 (1088 px, exactly the coded height, no margin). Whether the margin is required is **unknown** — but our driver allocates recon itself, so this is advice about sizing, not about `+0x36C` |
| `AVE_Work_Enc_CalcEUNumInParallel+0x40` (`0xfffffe0008caa620`) computes MBs as `((w+15)>>4) * ((h+15)>>4)` | neither | functional code, rounds up, works with either |
| `AVE_CalcBufSizeOfMBStats` / `MBInputCtrl` use `(w+15)>>4` and `((h+63)>>4) & ~3` | neither | round up, identical at 1080p |

Other quantities that shift slightly but bind nothing: the rate-control
distortion divisor `w*h` (`0xf870`), the `CodedBufSize > 3*w*h/4` alternate
check at `0x688e8` (1 566 720 vs 1 555 200), and the
`AVE_CalcBufSizeOfCodedData` bitstream estimate.

---

## 6. Neither kernel binary aligns anything — the value is userspace's

The path from the user's settings struct to the wire command is two verbatim
`memcpy`s of the same 9312 bytes:

```
; AVE_Client_Verify
fffffe0008b9122c:  mov w8, #0x1838        ; 6200
fffffe0008b91230:  add x0, x20, x8        ; _S_AVE_Client + 0x1838
fffffe0008b91234:  add x1, x21, #0xa58    ; AVE_SessionSettings_UserKernel_Data + 0xA58
fffffe0008b91238:  mov w2, #0x2460        ; 9312
fffffe0008b9123c:  bl  memcpy

; AVE_CHM_MakeFwCmd_Start_AVC   (via an intermediate copy at CHM + 0x4C0, 0xfffffe0008b6718c)
fffffe0008b672fc:  add x0, x23, #0x368    ; sCAveCmdAvcStart + 0x368
fffffe0008b67300:  add x1, x19, #0x4c0
fffffe0008b67304:  mov w2, #0x2460        ; 9312
fffffe0008b67308:  bl  memcpy
```

**Confirmed**, and cross-checked: the next copy in `MakeFwCmd_Start_AVC` starts
at `cmd + 0x27C8` = `0x368 + 0x2460`, so the block boundaries are consistent.

`AVE_AVC_CheckInfo` reads exactly `[settings + 2648]` / `[settings + 2652]`
(`0xfffffe0008b8ebec`/`0xb8ebf0`) and hands them to `AVE_Enc_CheckResolution`
(`0xfffffe0008b8edb4`), which only enforces the DevCap min/max (192x96 /
352x208 min, 4096x4096 max for `Resolution_AVC_Nyx_C`, docs/14 §7).
`AVE_Client_CheckCommonInfo+0x40` compares the client copy against the settings
copy for equality and nothing else. **No rounding, no MB check, anywhere in the
kext.** Therefore the convention is fixed by the userspace VideoToolbox plugin,
which is not in either blob — which is exactly why the kext still carries
`AVE_Enc_AlignDimension` with no caller: its caller is on the other side of the
user-client boundary. **Inferred**, stated as such.

### The finding that matters more than the question

Because the coded grid is `120 x 68` either way, **the encoder reads 1088 luma
rows from the source surface no matter what is in `ui32Height`.** A 1920x1080
NV12 buffer with an unpadded 1080-row luma plane is 8 rows short of what the
hardware will fetch. The driver must either allocate/DART-map the source with
at least `16 * ceil(H/16)` luma rows (and `8 * ceil(H/16)` chroma rows), or
accept reads past the plane. Nothing in either binary was found that clamps or
replicates the fetch at the picture height — **and nothing was found that
proves it does not**, so treat this as a required allocation rule, not an
observed fault.

---

## 7. The answer

**Put the MB-aligned (coded) dimensions in `ui32Width` / `ui32Height`: 1920 and
1088 for 1080p.** Then express the display size with `frame_cropping_flag = 1`
and `frame_crop_bottom_offset = 4`.

Status of that recommendation, stated precisely:

* **Confirmed:** it cannot be wrong in the sense the question feared. The MB
  grid, the picture-size registers, the slice map and the emitted SPS are all
  identical for 1080 and 1088, because the grid comes from the SPS and every
  derivation from `ui32Height` rounds up.
* **Inferred:** 1088 is the better of the two. It is what
  `AVE_Enc_AlignDimension` documents, it keeps the recon/DPB tile margin, and
  it makes the `nmb`, distortion-divisor and coded-buffer estimates exact
  rather than 0.7% low.
* **Known cost of choosing 1088:** `ProcessPixHistogram`'s padding correction
  becomes a no-op, so the LRME pixel histogram includes the 8 padded rows.
  That biases a rate-control statistic by roughly 0.7% of the frame. At fixed
  QP (`RCMode = 3`) it is irrelevant. If you later move to a bitrate-targeted
  mode and the rate control looks slightly off on non-MB-aligned heights,
  revisit this — passing 1080 is the other legal reading and is a one-word
  change.

### Worked example — 1920x1080, AVC, 4:2:0 8-bit, progressive

Offsets into `sCAveCmdAvcStart`. Every dimension-related field:

```
/* VideoParams ------------------------------------------------------- */
0x0368  u32  VideoParams.ui32Width               = 1920
0x036C  u32  VideoParams.ui32Height              = 1088   /* = 16 * 68 */

/* H264_SEQUENCE_HEADER_PARAMS, base 0x291C -------------------------- */
0x2940  u32  SPSparams.chroma_format_idc         = 1      /* 4:2:0 */
0x2944  u32  SPSparams.separate_colour_plane_flag= 0
0x2948  u32  SPSparams.bit_depth_luma_minus8     = 0
0x294C  u32  SPSparams.bit_depth_chroma_minus8   = 0
0x2D4C  u32  pic_width_in_mbs_minus1             = 119    /* 1920/16 - 1 */
0x2D50  u32  pic_height_in_map_units_minus1      = 67     /* 1088/16 - 1 */
0x2D54  u32  frame_mbs_only_flag                 = 1      /* MUST be 1, see §4 */
0x2D58  u32  direct_8x8_inference_flag           = 1
0x2D5C  u8   VUI.vui_parameters_present_flag     = 0
0x2DB4  u8   CropParams.frame_cropping_flag      = 1
0x2DB8  u32  frame_crop_left_offset              = 0
0x2DBC  u32  frame_crop_right_offset             = 0
0x2DC0  u32  frame_crop_top_offset               = 0
0x2DC4  u32  frame_crop_bottom_offset            = 4      /* (1088-1080)/2, CropUnitY = SubHeightC = 2 */
0x2DCC  u32  SPSparams.header_len                = 0      /* output; MUST be 0 on input */
```

What the firmware then derives, all checkable against §2–§4:

| quantity | value | from |
|---|---|---|
| `originalMBWidth` (`this+3980`) | 120 | `pic_width_in_mbs_minus1 + 1` |
| `originalMBHeight` (`this+3976`) | 68 | `pic_height_in_map_units_minus1 + 1` |
| `validMBWidth` (`this+3984`) | 120 | `(120*16 + 14 - 2*0) >> 4` |
| `validMBHeight` (`this+3988`) | 68 | `(68*16 + 14 - 2*4) >> 4` |
| crop-info flag (`this+9648`) | 2 | `frame_crop_bottom_offset & 7 != 0` |
| `setLRME` picture-size reg | `120 \| (68 << 16)` | `((1920+31)>>4) & ~1`, `(1088+15)>>4` |
| `setPipe` picture-size regs | `120 \| (68 << 16)` | `(1920+15)>>4`, `(1088+15)>>4` |
| MBs per frame | 8160 | Level 4.0 `MaxFS` 8192 — docs/37 §3 |

Buffer sizing that follows (see §5 and §6, and docs/14 §8 for the formulas):

```
source luma rows the hardware fetches  = 16 * 68 = 1088   (not 1080)
source NV12 minimum size, 1920 stride  = 1920*1088 * 3/2 = 3 133 440  (0x2FD000)
recon / DPB tile rows                  = ceil((1088 + 4) / 32) = 35
recon / DPB tile cols                  = ceil((1920 + 4) / 32) = 61
recon size, 8-bit                      = 61 * 35 * 1024 = 2 186 240 per surface
CodedBufSize floor (alternate path)    > 3*1920*1088/4  = 1 566 720
```

Emitted SPS, for cross-checking with `ffprobe` on the first bitstream:
`pic_width_in_mbs_minus1 = 119`, `pic_height_in_map_units_minus1 = 67`,
`frame_mbs_only_flag = 1`, `frame_cropping_flag = 1`, crop offsets
`0, 0, 0, 4`, decoded size **1920x1080**.

---

## 8. Is the hardware experiment still needed?

**No, not for this question.** docs/37 §12 experiment 1 proposed submitting the
Start command twice, with `height = 1088` and `1080`, and comparing the emitted
SPS. That experiment cannot discriminate: the SPS is written verbatim from
`Start + 0x291C` (§4), so **both runs emit the identical SPS**, and both
program identical picture-size registers (§3). It would have produced a
confident null result and been read as "either is fine" — which happens to be
true, but for a reason the experiment could not have shown.

The experiments still worth running, in the same spirit:

* **Cheap and high-value:** docs/37 §12 experiment 3 — one Start with logging
  at level 8 on subsystem `0x8c` dumps `DebugInit`'s full `SPSparams.*` /
  `CropParams.*` and both header lengths, which validates §4 and §7's table end
  to end without encoding a frame. Subsystem `0x91` at level 8 additionally
  gets `CHEVCController`'s `clipCropDebug` line on the HEVC path, which prints
  `validMB`/`originalMB` directly.
* **New, and now the real risk:** §6's finding that the encoder fetches 1088
  luma rows from a 1080-row source. Allocate the source surface at
  `16 * ceil(H/16)` rows for first light. If you want to *observe* the
  over-read, DART-map exactly 1080 rows and watch for a translation fault
  rather than guessing.

---

## 9. Corrections and additions owed to other documents

* **[37](37-start-avc-session.md) §1** lists `ui32Height = 1088` with the note
  "MB-aligned; see §12 risk 1". The value stands; the risk does not — §3 above
  shows the choice is inert for the MB grid, and §8 shows the proposed
  experiment could not have decided it.
* **[37](37-start-avc-session.md) §11** lists "whether width/height at
  `0x368`/`0x36C` are display or MB-aligned" as unknown. Closed here.
* **[37](37-start-avc-session.md) §12 risk 1** says the kext's
  `nmb = (w>>4)*(h>>4)` is "only correct if height is already a multiple of
  16 — so 1088". True of that expression, but it is an `os_log` argument and
  the identical idiom on the HEVC path is provably fed unaligned values. It is
  not evidence. §5.
* **[37](37-start-avc-session.md) §3** table: `SPSparams + 1084`
  (`Start + 0x2D58`) is `direct_8x8_inference_flag`. §4.
* **[14](14-frame-size-formulas.md) §7** notes `AVE_CalcBufSizeOfCodedData`
  aligns width but not height, and reads that as weak evidence height arrives
  pre-aligned. Re-checked: its callers pass `client + 6200` / `+ 6204`
  unmodified (`0xfffffe0008ca9a6c`/`0xca9a70`), and the neighbouring sizers
  (`MBStats`, `MBInputCtrl`, `Recon`, `EUNumInParallel`) all round *both*
  dimensions up. The asymmetry is a quirk of one estimator, not a contract.
* **[14](14-frame-size-formulas.md) §9** lists "where dimension alignment is
  actually enforced" as open. Answer: **nowhere in the kernelcache**. The AVC
  firmware does not need it (the grid is in the SPS); the HEVC firmware does it
  itself (`((d + 31) >> 4) & ~1` at `0x9de5c`–`0x9de94`, i.e. round up to 32
  px); `AVE_Enc_AlignDimension` is the userspace-side contract compiled into
  the kext but not called from it.
* **[20](20-command-structs.md) §3.3** names `0x368`/`0x36C` from a block map.
  Now confirmed from the firmware's own format strings `"ui32Width: %d"` /
  `"ui32Height: %d"`. §1.


---

## Verification pass (independent re-derivation)

| Claim | Evidence | Verdict |
|---|---|---|
| The MB grid comes from the host SPS block, not `ui32Height` | `0x6c384: ldr w10,[x28,#1076]` / `0x6c394: add w10,w10,#1` / `0x6c3d0: str w10,[x20,#3980]`; and `0x6c3dc: ldr w8,[x28,#1080]` / `add #1` / `0x6c3ec: str w8,[x20,#3976]`. `x28` is the SPS copy destination, so these are `pic_width_in_mbs_minus1 + 1` and `pic_height_in_map_units_minus1 + 1` | confirmed |
| The picture-size register rounds height **up** | `0xb451c` path: `lsl w9,#12` / `add w9,#0xf,lsl#12` / `and w9,#0x7ff0000` = `((h + 15) >> 4) << 16` | confirmed |
| 1080 and 1088 produce identical registers | `(1080+15)>>4 = 68` and `(1088+15)>>4 = 68` | confirmed |

**One refinement to the agent's reading.** The two axes do not use the same
formula. Height rounds up to the next macroblock as described. Width instead
computes `((w + 31) >> 4) & 0x7fe` (`0xb4514`, `0xb451c`, `0xb4524`), i.e. it
rounds up to 32 and then clears the low bit, giving an **even** macroblock
count: `(1920 + 31) >> 4 = 121`, masked to `120`. The conclusion is unaffected
at 1080p, but a driver supporting arbitrary widths should know that the
hardware's width granularity is 2 MB (32 px), not 1 MB.

## The finding that matters more than the question

The question asked was cosmetic; the answer that came out of it is not.

Because the macroblock grid is 120x68 **regardless** of `ui32Height`, the
encoder reads **1088 luma rows** from the source surface for a 1080p encode.
A source buffer allocated for 1080 rows is eight rows short, and the read runs
off the end of the mapping.

That is a DART fault or silent garbage on the first frame, and it is a
*buffer allocation* bug rather than a parameter bug, so it would not have been
found by any amount of squinting at the parameter block. The rule for the
driver is:

```
source plane rows = 16 * ceil(H / 16)      /* luma   */
                    8 * ceil(H / 16)       /* chroma, NV12 4:2:0 */
```

Note also that this makes the original hardware experiment in
[37](37-start-avc-session.md) §12 unrunnable as designed: both arms emit the
same SPS (it is copied verbatim) and program the same registers, so it would
have returned a confident null result and we would have believed it.
