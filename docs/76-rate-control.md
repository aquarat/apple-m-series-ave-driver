# Rate control: the "gates", what macOS sends, and why low targets overshoot

f77 showed the firmware controller runs with what we send (`ui32RCFlag = 1`).
The slice QP moved 39..46 at 300 kbit/s on the self-test ramp, but the rate
settled at about **2× target**. f78/f79 then tracked within a few percent from
~1 Mbit/s up (300 frames of `testsrc2`) and overshot below that
([53](53-first-frame.md), f77–f79). This document covers four things:

- It traces the two "hidden gates" of [66](66-ratecontrol-sizing.md) §1.2.
- It lists every rate-control input where our driver differs from macOS.
- It reads what the controller does with the inputs that differ.
- It says how to prove convergence on hardware.

Static analysis only; **nothing here was run**. Conventions as
[72](72-userspace-video-params.md) and [74](74-residual-path.md):

- Firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`).
- Kext VAs are kernelcache `__TEXT_EXEC` VAs.
- Userspace VAs are `AVE.bin` VAs.
- **VP** is an offset in `AVE_VIDEO_PARAMS`, and wire = VP + `0x60`.
- **RC** is an offset in `AVEFWRCSettings`, and wire = `0xFF30` + RC.
- `this` is the `CRateControl` object and `ctrl` is `CAVCController`.
- `p+N` is `sCRCInitParams + N` (built at `sp+0xCA0` in `InitEncodingParameters`).
- Labels follow [00](00-methodology.md): **[C]** read from an instruction (VA cited), **[I]** inferred with the chain stated, **[U]** unknown.

Two delegated sweeps, one of userspace and one of the kext, fed §3 and §4. Their
rows were spot-checked here; the spot-checks are listed in §8.

---

## 0. Verdicts

| # | question | answer | label |
|---:|---|---|---|
| 1 | What are `sCRCInitParams+177` and `+180`? | **The multipass block, not a rate-control prerequisite.** `+177` ← VP+0xFE9C u8 (wire **0xFEFC**), `+180` ← VP+0xFEA0 u32 (wire **0xFF00**), `+184..+196` ← VP+0xFEA4..0xFEB0 (wire 0xFF04..0xFF10). When both `+177` and `+180` are non-zero, ProcessInit skips the per-flag dispatch and calls `CMultiPassControl::Initialize`. When either is zero, as macOS sends and as we send, **`ui32RCFlag = 1` still takes the bitrate arm** (`0x4039c`→`0x403ac`→`0x40188`). [66](66-ratecontrol-sizing.md) §1.2's "both must be non-zero" is wrong; f77 agrees. §1. | [C] |
| 2 | Does anything else gate `ui32RCFlag = 1`? | No. A `CRateControl` is built for flag 1 or 2 (`0x5d958`–`0x5d960`), and ProcessInit's bitrate arm needs only bitrate, frame rate and size. It computes bits per pixel and an **initial QP from a static reference-bitrate table** (8.1 Mbit/s at 720p30). The model predicts **39** for f77's 300 kbit/s, and f77's IDR was coded at **39**. §2.2, §2.3. | [C], checked against f77 |
| 3 | What does macOS send that we don't, with RC on (path A, 720p30)? | 14 RC fields and 4 VP fields differ (§3). Two of them feed the **per-frame QP path**: **RC+0x18 (wire 0xFF48)** `ui32AverageNonDroppableFrameRate`, where macOS sends `0xCDCDCDCD` and we send **1**, and **RC+0x04 (0xFF34)** `ui32IdrPeriod`, where macOS sends 30 and we send **1**. `0xFF48` is not a frame-rate divisor. Apple's own validator resets it to "unset" when it equals the frame rate, logging "honor the ui32AverageNonDroppableFrameRate request". With 1 there, the controller runs its **MiniGOP / droppable-frame path** on 29 of every 30 frames. The rest are QP modulation (per-MB), multipass, DRL and reference-spacing fields. At 300 kbit/s and at 2 Mbit/s macOS sends **identical fields except RC+0x00**. | [C] |
| 4 | Why ~2× at 300 kbit/s on the ramp? | **[U], with a ranked list.** The per-frame budget is `bitrate / fps` = 10 000 bits (`0x4158c`–`0x415b8`). The ±3/frame step clamp (`0x41efc`–`0x41f48`) and the min/max clamp (10/51, `0x424b0`) did not bind in f77: QP rose at most 1 per frame and sat at 45 for 14 frames at 2× budget. So the controller's own model believed QP 45 was on target. Candidates, most testable first: (H1) the RC+0x18 path; (H2) IdrPeriod = 1 switches the complexity model to all-intra; (H3) the bits the controller is fed (`rcStatsParam.bitsFromCAVLC`) differ from the coded size; (H4) a QP ceiling or quality floor below 51 that this analysis did not find, which macOS would share. §2.4, §5. | [C] mechanisms, [I]/[U] cause |
| 5 | Per-frame fields macOS sends | No per-frame bitrate or frame-rate field exists. Every frame the kext writes `bUpdateLambdaTable = 0` (PICMGMT+0x80), PICMGMT+0x6D8 = 1, `throughputRateMode` (+0x6DC), +0x6E4, +0x6D0 = 100, +0x6C4 = 0xFFFF and +0x638 = −1. It writes timestamps (+0xCB8/+0xCC0) only when a driver flag is set. With timescale 0, which is what we send, the controller uses the configured frame rate (`0x3f2d4`–`0x3f328`). None of these is needed for frame-level rate control. §4. | [C] kext stores; [I] "not needed" |
| 6 | What shows the controller working, per frame, from data we already capture? | Three sources. (a) Slice QP in the bitstream. (b) `CODED_DATA_HDR` +0x90 / +0x94 = **Σ QP over inter / intra MBs** (`QpyInter`/`QpyIntra`). On all four f77 headers `(QpyInter+QpyIntra)/3600` equals the slice QP exactly, and it also exposes per-MB QP steering that the slice header hides (f78/f79). (c) Coded bytes. `tools/rc_track.py` grades a run with these controls (§5). | [C] (names from the kext; values from f77) |
| 7 | A V4L2 hazard found on the way | `ave_v4l2.c` sends `fps_num = timeperframe.denominator` to wire 0xFF4C and `fps_den` to 0xFF48. The firmware reads 0xFF4C as **integer Hz** (`ucvtf` `0x401a0`, class compares `0x4060c`–`0x40688`). So 30000/1001 would arrive as 30 000 fps: `bpp` would be 1000× too small, and the fps class becomes −1, which indexes before the reference table (`csinv` `0x40688`, loads `0x406bc`/`0x406c4`). Send `round(den/num)` at 0xFF4C and `0xCDCDCDCD` at 0xFF48. | [C] mechanism, [I] consequence |

**The next runs are §5 R0–R2.** R0 repeats f77 at 300 frames. R1 and R2 are
the two per-frame-path differences, one at a time. Both are reachable with
existing module parameters and no rebuild (`session_fps_div=30`,
`session_idr_period=30`), and "no" is a byte-identical stream.

---

## 1. The two "gates" are the multipass block

### 1.1 Where `+177` and `+180` come from

`InitEncodingParameters` (fw `0x5c9c8`, `x20` = VP base, `x22 = ctrl+0x23FC4`):

```
5cdd0  mov  w8, #0xf760
5cdd4  add  x23, x20, x8             ; x23 = VP + 0xF760
5cddc  ldrb w13, [x23, #1852]        ; VP+0xFE9C  (wire 0xFEFC)
5cde8  strb w13, [x22, #3464]        ;   -> ctrl+0x24D4C
5cdec  ldr  w12, [x23, #1856]        ; VP+0xFEA0  (wire 0xFF00)
5cdf8  str  w12, [x22, #3468]        ;   -> ctrl+0x24D50
5cdfc  ldr  w11, [x23, #1860]        ; VP+0xFEA4 -> [x22,#3472]
5ce0c  ldr  w10, [x23, #1864]        ; VP+0xFEA8 -> [x22,#3476]
5ce1c  ldr  w14, [x23, #1868]        ; VP+0xFEAC, <0 -> 6      -> [x22,#3480]
5ce2c  ldr  w14, [x23, #1872]        ; VP+0xFEB0, <0 -> 0x1305 -> [x22,#3484]
5ce3c  add  x16, x22, #0xd8c         ; &[x22,#3468]
5ce40..5ce58                          ; if FE9C && FEA0==1 && FEA4>=0 && FEA8>=0: [x16] = 9
5ce88  stp  x0, x16, [sp, #72]       ; [sp,#80] = &[x22,#3468]
...
5db58  ldr  x9, [sp, #80]
5db5c  ldrb w8, [x22, #3464]
5db64  ldr  q0, [x9]                  ; the four words FEA0..FEAC
5db68  strb w8, [sp, #3409]          ; p+177 = VP+0xFE9C
5db78  ldr  x9, [sp, #1040]
5db80..5dbb0                          ; p+188,+192 = FEA8,FEAC; p+196 = [x22,#3484]
5dbb0  str  w9, [sp, #3412]          ; p+180 = VP+0xFEA0 (or 9)
5db9c  str  w10, [sp, #3416]         ; p+184 = VP+0xFEA4
```

**[C].** [72](72-userspace-video-params.md) §5.3 already called VP+0xFE9C..0xFEB0 the
multipass block (ctrl `0x24D4C..60`), and the `6` / `0x1305` substitutions are
the ones it recorded.

### 1.2 What they do in `CRateControl::ProcessInit` (`0x400dc`)

```
40104  ldrb w12, [x20, #177]
4010c  ldrb w11, [x20, #12]          ; ui32RCFlag (byte)
4017c  cbz  w12, 0x4039c             ; +177 == 0 -> per-flag dispatch
40180  ldr  w12, [x20, #180]
40184  cbz  w12, 0x4039c             ; +180 == 0 -> per-flag dispatch
40188  ...                            ; bitrate arm (both non-zero: forced, any flag)
4039c  cbz  w11, 0x4048c             ; flag 0 -> OFF arm
403a0  cmp  w11, #0x3 ; b.hi 0x40d48 ; >3 -> error
403a8  cmp  w11, #0x2 ; b.ne 0x40188 ; flag 1 or 3 -> the SAME bitrate arm
...
40d08  ldrb w9, [x20, #177] ; cbz -> skip
40d14  ldr  w3, [x20, #180] ; cbz -> skip
40d30  bl   0x32a38                  ; CMultiPassControl::Initialize(codec, p160, p180, p184..p196)
```

**[C].** The flags do not gate the controller. They *force* the bitrate arm
whatever the flag, and start the multipass controller. macOS path A sends
VP+0xFE9C = 0 (`0x29ae0`) and VP+0xFEA0 = 0 (`0x29ae4`); the latter is 1 only in
`AVE_H264BeginPass` (`0x23eec`). Our driver writes neither, so both are 0 too.
That is why f77 worked. **[C]**

VP+0xFEA4..0xFEB0 differ: macOS sends −1 ×4 (`0x29af4`) and we send 0. They
reach only `p+184..196`, and those only reach `CMultiPassControl::Initialize`,
which is skipped. They are recorded in §3 but are irrelevant to single-pass rate
control. **[C]**

---

## 2. What the controller reads, and what it does with it

### 2.1 `sCRCInitParams` for AVC, corrected

This updates [66](66-ratecontrol-sizing.md) §1.3. Only the rows that change or
that §2.2–§2.4 use are listed. Stores are at `0x5d994`–`0x5dbb4` unless noted.

| p+ | source | wire | read by | label |
|---:|---|---|---|---|
| 0 | `ctrl+2672`, **always 0 in AVC** (`0x5ce5c`; the only other writers are the ctor `0x460f8` and `ResetBetweenPasses` `0x466d0`, both 0). So every `p[0]==1` arm in ProcessInit is **HEVC-only**, including the low-bitrate sub-mode `this+680 = 3` (`0x401d4`) and `this+796` (a QP offset, 0 for AVC) | — | ProcessInit | [C] |
| 12 | `ui32RCFlag` (byte) | 0xFF50 | `0x4010c` | [C] |
| 16 | RC+0x00 bitrate | 0xFF30 | `0x401b0` | [C] |
| 20 | RC+0x1C `ui32ExpectedFrameRate` | 0xFF4C | budget, bpp, table column | [C] |
| 24 | RC+0x18 **`ui32AverageNonDroppableFrameRate`** (named by userspace's dumper, [72](72-userspace-video-params.md) §5.2), **not a divisor** | 0xFF48 | `this+728`; `this+1340 = p20/p24 − 1` (`0x40270`–`0x402dc`) | [C] |
| 28 | RC+0x04 `ui32IdrPeriod` | 0xFF34 | `this+712`, `this+716` (`0x40298`/`0x4029c`) | [C] |
| 36 / 40 | RC+0x58 / RC+0x5C (min / max QP) | 0xFF88 / 0xFF8C | `this+1884/1900`, `1888/1904` | [C] |
| 44 / 48 | RC+0x60 / RC+0x64 | 0xFF90 / 0xFF94 | **QP-range modes.** `p44 == 1` clamps min QP into [14, 51] (`0x40ab8`–`0x40adc`). `p48 == 1` does the same for max QP (`0x40b7c`–`0x40ba0`) and enables a per-frame **dynamic** QP range (`0x41948`–`0x41a50`, `0x424c8`–`0x424f0`). macOS sends 0/0 | [C] |
| 76/80/84, 61 | RC+0x644/0x648/0x64C (userspace names them RefSpacingP/B0/B1), and p+61 = their sum ≠ 0 | 0x10574..7C | `this+812/816/820` → `getFrameIndexReferenceFrame*` (`0x436b0`–`0x43978`) | [C] path, [U] effect on P-only |
| 116 | `(RC+0x50 != 0)` | 0xFF80 | **not read by ProcessInit** (no `[x20,#116]` in `0x3f6c8`–`0x40e40`). [66](66-ratecontrol-sizing.md) had this at +117 | [C] |
| 117 | **VP+0xFE6E** (`0x5d938`–`0x5d940`, `0x5dafc`) | 0xFECE | DRL enable (`0x40210`) **and** the gate on the DRL copy (`0x5db2c`) | [C] |
| 120..159 | the 40-byte DRL config at `x20+0x10528`, VP-relative | **0x10588** (= RC+0x658). [66](66-ratecontrol-sizing.md)'s "0x10528" is 0x60 short | ProcessInit `0x4022c` | [C] |
| 160 | `ctrl+0x24188` ← `[ctrl+0x2C1F8,#44]` = **SPS `chroma_format_idc`** (`0x5d094`, `0x5d130`; SPS copy base `0x5ce7c`) | 0x105D8 | reference-bitrate scale: 3 → ×1.2, 0 → ×0.9, else ×1 (`0x406e4`–`0x40730`) | [C] |
| 177, 180..196 | multipass (§1) | 0xFEFC.. | §1 | [C] |
| 200 | VP+0xFEC4 `iNumViews` | 0xFF24 | `this+648`, the divisor of every frame index (`0x40ed4`, `0x432cc`) | [C] |
| 212 / 224 | RC+0x24 == 2 / RC+0x28 | 0xFF54 / 0xFF58 | CBR selector and bitrate | [C] |

`CRateControl::Init` (`0x3f6c8`) copies the whole 0xE8-byte block to
`this+0x1C0` (`0x3f71c`–`0x3f730`). Only `this+592`, `628`, `648`, `652` and
`656` are read back from that copy in `0x3f168`–`0x44200` (p+144, +180, +200,
+204, +208). **[C]**

### 2.2 ProcessInit's bitrate arm (flag 1)

- **Bits per pixel.** `this+848 = bitrate / fps / (W·H)`, with fps 30 if 0
  (`0x40188`–`0x401d0`). **[C]**
- **Frame rate.** `this+708 = p20`, and 0 or `0xCDCDCDCD` become 30
  (`0x409bc`–`0x409e4`). `this+1336` (float fps) = `this+708` (`0x40a3c`/`0x40a5c`).
  **[C]**
- **QP range.** Min = `p36`, max = `p40` (the p44/p48 modes are §2.1). The
  initial QP is clamped into that range only in the DRL/CBR arms (`0x40bd4`–`0x40be8`).
  **[C]**
- **Initial QP**, `this+880`: §2.3.
- **The MiniGOP selector.** `this+1340 = p20/p24 − 1`. It picks one of three
  static tables (`this+1360`/`1368`: `0xd1800` if 7, `0xd181c` if 3, else
  `0xd1828` = {0}, `0x40374`–`0x404cc`). **[C]**

  | 0xFF48 | 1340 | table |
  |---|---:|---|
  | macOS `0xCDCDCDCD` | 30/0xCDCDCDCD − 1 = −1 | default |
  | ours, 1 | **29** | default |
  | 30 | 0 | default |

### 2.3 The initial QP, and a free check against f77

Read out of `0x404d0`–`0x409c8`, with the non-DRL, non-CBR arm that AVC takes
(`p176 = 0`, because InitEncodingParameters never writes it and the 0xE8-byte
memset zeroes it, `0x5d990`):

1. **Reference bitrate R.**
   - The row is picked by macroblock count, against `{3600, 6075, 8100, 10800, 32400}` at `0xd1a28` (`0x40574`–`0x40604`).
   - The column is picked by frame rate, against `{24, 30, 60, 120, 240}` at `0xd182c` (`0x40608`–`0x40688`; >240 gives −1).
   - The value is the u64 at `0xd1848 + codec·240 + row·48 + col·8`.
   - It is interpolated linearly in MBs and in fps when they are off the grid (`0x40d68`–`0x40e3c`).
   - It is scaled by `chroma_format_idc` (§2.1).
   - AVC 720p30 gives **R = 8 100 000**; AVC 1920×1088 at 30 fps gives 15 304 444.
2. **B < R** (`0x408a8`–`0x40910`):
   - `n` = number of k ∈ 1..5 with `B ≤ R>>k`;
   - `pct = (B << (n+1))·100 / R`;
   - `d = 3n + [pct < 126] + [pct < 159]`, then `d −= (d ≠ 0)`.
3. **B > R** (`0x40804`–`0x40890`):
   - `n` = number of k ∈ 1..5 with `B ≥ R<<k`;
   - `pct = (B >> n)·100/R`;
   - `d = −6n − #{pct > 111, 124, 140, 157, 177}`.
4. **Result:** `QP₀ = clamp(26 + d, 0, 48)` (`0x40960`–`0x409c8`). ProcessRateControl uses it for frame 0 (`0x415bc`–`0x415c8`).

| 720p30 target | 150k | **300k** | 600k | 1M | **2M** | 4M | 8.1M | 16M |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| QP₀ (model) | 42 | **39** | 36 | 34 | **31** | 28 | 26 | 21 |

**f77's IDR slice QP is 39, and the CODED_DATA_HDR QP sums agree** (§4.2).
**[C]** for the code, and the one data point matches. `tools/rc_track.py`
implements the model and reports it as control C0 on every run. If a run's frame
0 disagrees, this reading is wrong, not the controller.

The table stops at 240 fps (`csinv` at `0x40688` gives −1). An integer frame
rate is therefore required (§0 row 7). **[C]**

### 2.4 Per frame: `CRateControl::ProcessRateControl` (`0x40e40`)

What was read, in the order the function applies it:

| step | what | VA | label |
|---|---|---|---|
| budget | `this+1920 = (bitrate + round(fps)/2) / round(fps)`, fps = `this+1336`. That is **10 000 bits/frame** at 300k/30 | `0x4158c`–`0x415b8` | [C] (the "budget" name is [I]) |
| budget, accumulate side | `ProcessAccumulate` divides the bitrate by `this+1336` if the timescale (`this+1840` = RCFrameInfo+16) is non-zero, else by `this+708` | `0x4289c`–`0x428e4` | [C] |
| measured fps | `calcFPSinstantaneous` updates `this+1336` from PTS deltas only when the timescale is non-zero. Otherwise it leaves it alone (`0x3f2d4`–`0x3f328`). The timescale comes from **PICMGMT+0xCC0** and the PTS from **PICMGMT+0xCB8** (setLRME `0x516d4`–`0x516e8`); **we send 0**, so the configured fps stands | `0x3f2bc` | [C] |
| low-bitrate floor | if `(bitrate + MBs/2)/MBs < 6000` (12 000 if `p176`), a **QP floor** in [0, 26] is computed and kept in `this+1048` (`0x42198`–`0x421a4`). At 720p this holds for any bitrate below 21.6 Mbit/s, so both macOS and we are in it | `0x41f4c`–`0x421a4` | [C] exists; [U] its value |
| step clamp | the new QP is clamped to **prev ± 3**. It is ±5 when a scene flag (w16 bit 0) is set, and +1/−3 in the first four frames with DRL or CBR | `0x41efc`–`0x41f48` | [C] |
| upper bound | `min(QP, this+792)`, where 792 = 51 for AVC | `0x42208`–`0x42210` | [C] |
| **MiniGOP / droppable path** | runs if `this+1940 == 0` (not hierarchical) and **`this+728 < this+708`**, i.e. RC+0x18 < fps. A counter `this+1372` runs 1..`this+1340` (29 for us) and resets. On counter == 1, an offset `this+1352` ∈ {0..4} is chosen from a running figure `this+1348` (thresholds 200/300/500/1000, `0x426d8`–`0x42724`). Frames 1..29 of each 30 get **QP + 1352 + table[0]**, and table[0] = 0 for `0xd1828` (`0x42788`–`0x427c8`). The controller's own QP memory (`this+1028`) is written **before** the offset (`0x4227c`) | `0x42328`–`0x42374`, `0x4257c`–`0x427c8` | [C] |
| same path, macOS | `0xCDCDCDCD ≥ 30` → `b.cs 0x42378`: never taken | `0x42358`–`0x42364` | [C] |
| min/max clamp | `[this+1900, this+1904]` = [10, 51] for us, [0, 51] for macOS | `0x424b0`–`0x424c4` | [C] |
| IdrPeriod | `this+716` is read **only** as `== 1`: at `0x40f08` (I frames), `0x3f1bc` (`calculateComplexity`, I-type arm) and `0x4315c` (`ProcessAccumulate` skips the I/P/B complexity blend when it is 1). `this+712` has no reader in `0x3f168`–`0x44200`. So **IdrPeriod = 1 tells the model "all intra"** while we code P frames | as listed | [C] reads, [I] meaning |
| non-reference marking | `getIsThisFrameMarkedAsNonReference` (`0x432ac`, called per frame from `H264VideoEncoderDPB::ManageDPBBuffer` `0x2d440`) takes its frame-dropping arm only when `this+728 ≠ 0xCDCDCDCD` **and** the timescale is non-zero (`0x43348`–`0x433cc`). We send timescale 0, so it is off for us. **f77 agrees: all 30 frames have `nal_ref_idc` 1** | as listed | [C], and the data agrees |

**What the controller measures.** `CAVCController::ProcessDataFromCpusMultiCore`
sums a per-core CAVLC bit counter (`[core_dmem,#1972]`, `0x4c364`–`0x4c390`).
It adds the header bits on IDR (`0x4c42c`–`0x4c43c`), stores the total in
`sCRCStatPerFrame+4` (`0x4c3d8`), and calls `CRateControl::UpdateBits`
(`0x4c528`). With the print gate on, it logs the number as
`"fid:%d fnum:%d dfnum:%d rcStatsParam.bitsFromCAVLC = %d"` (`0x4c474`) and
`"CAVCController:: Pipe frame_num = %d rcStatsParam.bitsFromCAVLC = %d"`
(`0x4c54c`). **[C].** Whether that equals 8 × the coded slice bytes is **[U]**,
and it is the cleanest single test of H3.

**What f77 says about the loop.**

- Frame 0 is at QP₀ = 39 (§2.3).
- Frames 1–5 hold QP 40–42 at 0.9–2.8× budget, and frames 6–7 *drop* to 39 while over budget.
- Frames 10–16 rise one step at a time to 45, then hold 45 (46 once) for 14 frames at 2.0–2.2× budget.

Neither clamp binds. A controller that measured 2× overshoot would keep
stepping up, so either its bit measurement or its budget differs from ours by
about 2×, or something caps it at 45. The MiniGOP offset (H1) is the one
mechanism found that separates the QP the model remembers from the QP that is
coded. **[I].** Note also that 45 = QP₀ + 6. No such cap was found in
`0x40e40`–`0x427cc`, but the function was not decoded end to end. **[U]**

---

## 3. The complete RC-on diff (question 2)

"macOS" is path A of [72](72-userspace-video-params.md) §5.1: a plain VideoToolbox
H.264 session with `AverageBitRate`, 1280×720, `ExpectedFrameRate` 30. **The
300 kbit/s and 2 Mbit/s configurations differ only in RC+0x00.** The BPP gate
(`0x3ab24`–`0x3abd8`) would keep `bFlatAreaLowQpEn` only above 7.19 Mbit/s, and
Validate clears it anyway (`0x2c3f4`–`0x2c4c0`). **[C].** "Ours" is
`ave_cmd_build_start_avc` with `rc_enable` set (`driver/ave_cmd.c:435`–`470`).

The kext changes exactly one of these on the way to the wire: RC+0x1C,
`0xCDCDCDCD` → 30 (`AVE_Client_Config` `0xfffffe0008ecc204`–`0xecc21c`). It
never touches RC+0x18, RC+0x54, RC+0x58 or the DRL block. Start_AVC copies RC
verbatim (`0xfffffe0008ea9b30`–`0xea9bc4`) and writes nothing after that into
0xFF30..0x105AF. **[C]**

| RC | wire | width | macOS (writer VA) | ours, RC on | fw consumer | rate-relevant? |
|---:|---:|---|---|---|---|---|
| 0x00 | FF30 | u32 | target (AverageBitRate handler `0xff64`) | target | budget | same |
| **0x04** | **FF34** | u32 | **30** (`0x29b60`) | **1** (`session_idr_period` default) | `this+716 == 1` switches (§2.4) | **yes (H2)** |
| 0x10 | FF40 | u8 | 1 `bAllowFrameReordering` (`0x29bb4`) | 0 | not in `sCRCInitParams`; consumer not traced | [U], B-frame related |
| **0x18** | **FF48** | u32 | **0xCDCDCDCD** (`0x29c50`–`0x29c58`; Validate resets it to CDCD whenever it equals RC+0x1C, `0x2bd40`–`0x2bd50`, log at `0x2bd24`) | **1** (`frame_rate_div`, `ave_cmd.c:445`) | MiniGOP path (§2.4), `this+1340` | **yes (H1)** |
| 0x1C | FF4C | u32 | 30 (`0x11150`) | `fps_num` | budget, bpp, table | same for 30/1. **Wrong for fractional V4L2 rates** (§0 row 7) |
| 0x20 | FF50 | u32 | 1 (`0x3aa74`) | 1 | — | same |
| 0x24 / 0x28 | FF54 / FF58 | u32 | 0 / 6 220 800 (stale default, unused while 0x24 ≠ 2) | 0 / 0 | CBR only | no |
| 0x30 | FF60 | f32 | 1.0f (`0x29b74`) | 0 | `this+876`, read only in the DRL/CBR arm of ProcessAccumulate (`0x429b4`) | no (DRL off) |
| 0x40 | FF70 | u8 | 1 `bEnableQPMod` (`0x29b80`) | 0 | per-MB QP modulation ([72](72-userspace-video-params.md) §5.2) | per-MB, not frame budget |
| 0x42 / 0x43 | FF72 / 73 | u8 | 1 / 1 `bEnableLamdaMod`, `bEnableVarianceQPMod` (`0x29b8c`) | 0 / 0 | mode-word bit 0; DMem 0x764 bit 9 | per-MB |
| 0x48 | FF78 | u8 | 1 `bEnableQPModRefresh` (`0x29b84`) | 0 | `ctrl+0x23FC7` | per-MB |
| 0x50 | FF80 | u32 | 1 `eStaticAreasLowQpSel` (`0x3aa84`) | 0 | p+116 (not read by ProcessInit), static-area low QP | per-MB |
| 0x54 | FF84 | u32 | 0xCDCDCDCD `RealTimeClient` (`0x29bd0`/`0x29bd4`, constant `0x115f90`) | 0 | `ctrl+0xA80 = (v≠0)` (`0x5cee4`–`0x5cef0`); no direct reader found | [U] |
| 0x58 | FF88 | u32 | 0xCDCDCDCD → fw min QP **0** (`0x5d9f0`–`0x5d9f8`) | `session_qp_min` (10) | min clamp | not binding in f77 |
| 0x5C | FF8C | u32 | 51 | 51 | max clamp | same |
| 0x60 / 0x64 | FF90 / 94 | u32 | 0 / 0 | 0 / 0 | QP-range modes (§2.1) | same |
| 0x68..0x7C, 0x90..0x643 | FF98..10573 | | λ block ([72](72-userspace-video-params.md)) | sent (`session_lambda`) | ME/MD λ | same |
| 0x84..0x8C | FFB4..BC | u32 | 26/26/26 | `session_qp` | **FIXQP arm only** (`0x403c8`–`0x403e8`; no other reader of p+92..100 in ProcessInit) | no |
| 0x644/648/64C | 10574..7C | u32 | 1/1/1 (`0x29ca0`–`0x29ca8`) | 0/0/0 | reference-frame index helpers (§2.1) | [U] for P-only |
| 0x658..0x67F | 10588..105AF | 40 B | DRL config, all zero (`0x29c98`/`0x29c9c`) | 0 | only when VP+0xFE6E ≠ 0 | same |

Fields of `AVE_VIDEO_PARAMS` in the same neighbourhood:

| VP | wire | macOS | ours | note |
|---:|---:|---|---|---|
| 0xFE6E | FECE | 0 (set only by a multipass pass-state, `0x12598`–`0x125b0`, and **not** by `DataRateLimits`) | 0 | DRL enable, p+117 |
| 0xFE9C / 0xFEA0 | FEFC / FF00 | 0 / 0 | 0 / 0 | §1 |
| 0xFEA4..0xFEB0 | FF04..FF10 | −1 ×4 (`0x29af4`) | 0 | multipass only |
| 0xFEC4 / 0xFEC8 | FF24 / FF28 | 1 / 1 | 1 / 1 | `this+648` divisor |

**What the driver must add to send macOS's RC-on configuration** (a proposal;
not done here):

1. `0xFF48 = 0xCDCDCDCD` instead of `frame_rate_div`.
2. `0xFF4C = round(fps_num / fps_den)`, an integer.
3. `0xFF34 = ` the real IDR period: the V4L2 `gop`, else 30.
4. `0xFF88 = 0xCDCDCDCD`, or leave `session_qp_min` but default it to 0.
5. `0x10574..0x1057C = 1, 1, 1`.
6. `0xFF84 = 0xCDCDCDCD`, `0xFF40 = 1`, `0xFF60 = 1.0f`.
7. Separately, and as its own experiment: the QP-modulation set (`0xFF70`,
   `0xFF72`, `0xFF73`, `0xFF78` = 1, `0xFF80` = 1). These change per-MB QP and
   the λ path, not the frame budget. f78/f79 already show per-MB steering
   without them, so what they add is unknown.

Items 1–3 are the ones the controller's frame-level path reads (§2.4). Items
4–6 have no traced effect on it.

---

## 4. Per-frame and post-frame information (question 3)

### 4.1 What macOS sends per frame (kext `AVE_CHM_SetDataInfo_RC`, `0xfffffe0008eab85c`, every frame)

| PICMGMT | macOS | VA | we send |
|---|---|---|---|
| +0x08..+0x7F | per-frame user data, force key / non-ref | `0xeab8a4`–`0xeab940` | force flags only |
| +0x80 `bUpdateLambdaTable` | **0 always** (`strb wzr`) | `0xeab94c` | 0 |
| +0x638 | −1 | `0xeab944`–`0xeab948` | 0 |
| +0x6C4 / +0x6D0 | 0xFFFF / 100 | `0xeabafc` | 0 |
| +0x6D8 | 1 [I] | `0xeab8f8`–`0xeab908` | 0 |
| +0x6DC `throughputRateMode` | 1 or 3 | `0xeabb28` | 0 |
| +0x6E4 | 3 | `0xeabb30`–`0xeabb50` | 0 |
| +0xCB8 / +0xCC0 | PTS / timescale, only if DRV+0xE2 ≠ 0 | `0xeaaa5c`–`0xeaaa6c` | 0 |

- **There is no per-frame bitrate or frame-rate field. [C]**
- `bUpdateLambdaTable` is always 0, so the per-frame λ copy at +0x84 is not
  applied. That closes [72](72-userspace-video-params.md) §8's open item.
- For frame-level rate control with timescale 0, only the timestamp pair could
  matter, and it is off both in our driver and (unless DRV+0xE2 is set, [U]) in
  macOS. **[I]**
- PICMGMT+0x6D8 reaches `RCFrameInfo+32` (ManageDPBBuffer `0x2d300`–`0x2d32c`).
  It is used in `calcFPSinstantaneous` and `getIsThisFrameMarkedAsNonReference`
  only on the timescale ≠ 0 arms. **[C]**
- `throughputRateMode`'s firmware consumer was not traced. **[U]**

### 4.2 What the encoder hands back, and what f77 already shows

`CODED_DATA_HDR` names come from the kext's `AVE_PrintRCStats`
(`0xfffffe0008ec1e4c`), mapped through `AVE_RetrieveRCStats`'s copies
(`0xec4e94`–`0xec5008`):

| hdr+ | name | f77 frame 28 |
|---:|---|---|
| 0x00 / 0x10 / 0x20 | I / P / Skip MB count [4] | 1411 / 2189 / 2188 |
| 0x30 / 0x40 | Inter / Intra SATD sum | 962 147 / 302 548 |
| 0x50 | Residual_Bits [4] | 1738 |
| 0x60 | SliceDataLen [4] | 2082 |
| **0x90 / 0x94** | **QpyInter / QpyIntra** | **100 694 / 64 906** |
| 0x98 | SPSPPSHeaderBits | 168 |

`QpyInter = 2189 × 46` and `QpyIntra = 1411 × 46`, where 46 is frame 28's slice
QP. On all four f77 headers (frames 26–29), `(QpyInter+QpyIntra)/(I+P) =`
slice QP exactly (46, 45, 45, 45). **So hdr+0x90/+0x94 are Σ per-MB QP.** They
report the mean coded QP including per-MB modulation, which f78/f79 say is
where the V4L2 runs steered. **[C]** for the data; the names are Apple's.

### 4.3 The yes/no a run can give

`tools/rc_track.py <results dir> --bitrate B [--fps F] [--qp-max Q]`:

- **C0** frame 0 QP = the §2.3 model. f77: 39 = 39.
- **C1** for each `coded_hdr*.bin`: Qpy mean = slice QP of the frame it names.
- **C2** for each `coded_hdr*.bin`: `ui32BytesWritten` = that frame's slice NAL
  size. f77: 4/4 pass on both. If C1 or C2 fails, the tool refuses to grade.
- It then prints per-second rate and QP range, and a verdict over the frames
  after `--settle` (3 s default):

  | verdict | meaning |
  |---|---|
  | `TRACKS` | mean within ±10%, and every 1-s window within ±25% |
  | `CEILING` | over target at `--qp-max`: the content cannot go lower. Not a fault |
  | `STALLS` | over target, QP below max and not rising: the controller believes it is on target. f77's shape |
  | `UNDERSHOOT` | under target |
  | `SHORT` | too few frames to grade |

On f77 it reports 2.04× in second 0 and `SHORT`, as it should for 30 frames.

**Convergence claim for a report:** `TRACKS` on two content types (the self-test
ramp and V4L2 `testsrc2`), each at a target above and below 1 Mbit/s, 300
frames each, with the identical run repeated once (AGENTS.md "repeat before
believing"). With the `-v` flag, per-frame QP that moves while the rate is
off-target is the evidence the controller acts. Under FIXQP the slice QP
provably cannot vary ([66](66-ratecontrol-sizing.md) §9), which is the negative
control.

Optional, and not relied on: `session_dbg=0x20` should make the firmware print
`bitsFromCAVLC` and `RateControl ... qp:%d` each frame (§2.4;
PipePrepareParam `0x48fb0`–`0x490d8` runs them when `ui32RCFlag ≠ 0`). f41
(FIXQP, one frame) printed none of the per-frame lines, only Start_AVC ones
([53](53-first-frame.md) f41). So whether they appear is **[U]**.

---

## 5. Ranked single-variable proposals (question 4)

Operator or lead agent only ([AGENTS.md](../AGENTS.md)). Every run: one load,
self-test path, 1280×720, `session_bitrate=300000`, `session_frames=300`, all
else f77's configuration. Grade with `tools/rc_track.py <dir> --bitrate 300000 -v`.

| rank | run | change from R0 | why | yes | no |
|---:|---|---|---|---|---|
| **R0** | f77 at 300 frames | `session_frames=300` | Repeat-before-believing for f77 (n=1), and the first look at 10 s of convergence on the ramp | `TRACKS` after settling: f77 was only slow | `STALLS` ≈2× through 10 s: f77's shape is steady state. C0 must pass (QP₀ 39) |
| **R1** | R0 + `session_fps_div=30` | wire 0xFF48 = 30 instead of 1 | H1. For AVC with timescale 0, 30 there is equivalent to macOS's CDCD at every reader: `0x42358` (30 ≮ 30), `0x433b4` (timescale arm off), `0x40c2c` (HEVC-only after `0x40c64`), and `this+1340` = 0 picks the same default table as −1 (`0x40374`–`0x404cc`). No rebuild. `session_fps_div` reaches only `bufs->fps_den` → `frame_rate_div` (`ave_session.c:3146`, `1605`; `ave_cmd.c:445`) | ratio moves toward 1.0; QP climbs past 45 | **byte-identical `frame.h264` to R0**: the MiniGOP offset was 0 and H1 is dead |
| **R2** | R0 + `session_idr_period=30` | wire 0xFF34 = 30 instead of 1 | H2 (§2.4 IdrPeriod). Frame types stay explicit (IDR then P, `ave_session.c:2621`–`2625`), so only the RC sees it ([66](66-ratecontrol-sizing.md) §2.1). No rebuild | ratio changes; QP trajectory differs from frame 1 on | byte-identical to R0 |
| **R3** | R0 + `session_dbg=0x20` | wire 0xFCD8 bit 5 | H3: does `rcStatsParam.bitsFromCAVLC` equal 8 × slice bytes (`coded_hdr` C2 gives the bytes for four frames)? f41 survived this bit | per-frame `bitsFromCAVLC` lines. ≈ 8×bytes kills H3; ≈ 4×bytes is H3 | no per-frame RC lines (§4.3 [U]): uninformative, not a "no" |
| **R4** | fixed-QP ladder on the same ramp | `session_bitrate=0 session_qp=45`, then `=51` (two runs) | H4 and a content control. Bytes/frame at QP 45 must match R0's steady state (~2600 B); if not, the ramp or the pairing differs. Bytes at 51 say whether 300 kbit/s is reachable at all | QP 51 < 1250 B/frame: reachable, and the controller should go there | QP 51 ≥ 1250 B/frame: the target is below this content's floor. The overshoot is physics, and CEILING is the right verdict |
| R5 | after the §3 driver change (items 1–3) | V4L2, `v4l2-ctl` VBR 500 kbit/s, `testsrc2` 300 frames (f78's 689 kbit/s case) | varied content, and the configuration macOS sends | `TRACKS` | same 689 kbit/s: the frame-level inputs are not the cause, so look at H3/H4 |
| R6 | R5 + QP-modulation set (§3 item 7) | wire 0xFF70/72/73/78 = 1, 0xFF80 = 1 | the rest of macOS path A | rate unchanged and quality up, or rate closer | anything worse is a reason to keep them off |

Order R1 before R2, because R1 is the larger and better-evidenced divergence.
If either gives a byte-identical stream, that is a clean "no" in one run, and it
needs no repeat. A changed stream needs its repeat before it is believed.

None of R0–R4 adds a register read, a surface, or a new command field. They
change values in fields the firmware already parses. **Risk: as a normal
session.**

---

## 6. Corrections and annotations to earlier documents (not edited here)

1. **[66](66-ratecontrol-sizing.md) §0 item 1 and §1.2.**
   - "Two 'RC is really on' gates … must both be non-zero before a controller is
     built" is wrong. `+177`/`+180` are the multipass block (VP+0xFE9C/0xFEA0).
     They *force* the bitrate arm when both are set, and flag 1 reaches the same
     arm when they are not (§1.2).
   - §9's hardware proposal is answered by f77.
2. **[66](66-ratecontrol-sizing.md) §1.3.**
   - `+116` is `(RC+0x50 ≠ 0)`, not `+117`.
   - `+117` is VP+0xFE6E and gates the DRL copy.
   - The DRL block is at wire **0x10588** (RC+0x658), not 0x10528.
   - `+24` (RC+0x18) is `ui32AverageNonDroppableFrameRate`, not a frame-rate
     divisor.
   - `+160` is SPS `chroma_format_idc`.
   - `+200` is `iNumViews`.
   - `+0` is always 0 for AVC, so the low-bitrate `this+680 = 3` sub-mode is
     HEVC-only.
3. **`driver/ave_abi.h`** (`frame_rate_div = 0xff48`, "frame rate is
   frame_rate/this") and **`driver/ave_cmd.h:85`** carry the divisor reading.
   `driver/ave_session.c:441`–`444` still describes the gates as unresolved.
4. **[72](72-userspace-video-params.md) §8** "whether the kext changes RC λ
   fields per frame": no. `bUpdateLambdaTable` is written 0 every frame
   (§4.1).
5. **[53](53-first-frame.md) f77** says "undershoots badly … (~2x the per-frame
   budget)". The rate was **over** the target. f78/f79's "the slice-header QP
   stays at its start value; the rate is steered per macroblock" can now be
   checked directly from hdr+0x90/+0x94 (§4.2).

---

## 7. Open items

- The rest of `ProcessRateControl` (`0x40e40`–`0x427cc`, about 1 600
  instructions), and in particular the value of the low-bitrate QP floor and
  where the model's complexity comes from, is not decoded. That is where H4 lives.
- The MiniGOP offset's input `this+1348` derives from `this+944` (a float written
  in ProcessRateControl), whose meaning is **[U]**. So it is not known whether
  H1's offset is 0 or 4 on the ramp. R1 settles it empirically.
- Whether macOS itself reaches 300 kbit/s at 720p on this content is unknown.
  R4 measures the content's floor instead.
- Consumers of RC+0x10 (`bAllowFrameReordering`), RC+0x54's `ctrl+0xA80`, and
  PICMGMT+0x6DC were not traced.

---

## 8. Reproduce

```sh
export AVE_MACOS=13.5

# --- §1: the multipass block, and ProcessInit's dispatch ------------------
python3 tools/disas.py --fw --addr 0x5cdd0 -n 0xc0     # VP+0xFE9C.. -> ctrl
python3 tools/disas.py --fw --addr 0x5db58 -n 0x68     # -> p+177..+196
python3 tools/disas.py --fw --addr 0x400dc -n 0xb0     # +177/+180 test
python3 tools/disas.py --fw --addr 0x4039c -n 0x14     # flag dispatch
python3 tools/disas.py --fw --addr 0x40d04 -n 0x30     # CMultiPassControl::Initialize
python3 tools/disas.py --fw --addr 0x5d938 -n 0x38     # controller built for flag 1/2

# --- §2.2/2.3: ProcessInit's bitrate arm, initial QP ----------------------
python3 tools/disas.py --fw --addr 0x40188 -n 0x160
python3 tools/disas.py --fw --addr 0x404d0 -n 0x500    # reference table, QP0
python3 tools/disas.py --fw --addr 0x40d68 -n 0xd8     # interpolation
python3 tools/disas.py --fw --addr 0x5d090 -n 0xa4     # p+160 <- SPS chroma_format_idc
python3 - <<'EOF'
import struct; d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read(); F=0x4000
q=lambda va,n,f: struct.unpack('<%d%s'%(n,f), d[va+F:va+F+n*struct.calcsize(f)])
print(q(0xd1a28,5,'I'), q(0xd182c,6,'I'))
for r in range(5): print(q(0xd1848+r*48,6,'Q'))
EOF

# --- §2.4: per frame -------------------------------------------------------
python3 tools/disas.py --fw --addr 0x4158c -n 0x30     # budget
python3 tools/disas.py --fw --addr 0x41efc -n 0x50     # +-3 step clamp
python3 tools/disas.py --fw --addr 0x42328 -n 0x60     # MiniGOP gate (728 < 708)
python3 tools/disas.py --fw --addr 0x4257c -n 0x250    # MiniGOP offset
python3 tools/disas.py --fw --addr 0x424b0 -n 0x40     # min/max clamp
python3 tools/disas.py --fw --addr 0x43348 -n 0x90     # non-reference marking gate
python3 tools/disas.py --fw --addr 0x3f2bc -n 0x70     # calcFPSinstantaneous
python3 tools/disas.py --fw --addr 0x516c4 -n 0x30     # PICMGMT+0xCB8/+0xCC0 -> frame info
python3 tools/disas.py --fw --addr 0x4c364 -n 0x1f0    # bitsFromCAVLC -> UpdateBits
for a in 0x3f1bc 0x40f08 0x4315c; do python3 tools/disas.py --fw --addr $a -n 0xc; done

# --- §3: userspace (listing per docs/72 §9) ---------------------------------
B=data/blobs/macos-13.5/userspace/AVE.bin
data/blobs/tools/ipsw macho disass $B -x __TEXT.__text --no-color > /tmp/ave_ua.s
for a in 0000ff64 00029b60 00029c50 00029c58 0002bd40 0002bd50 0003aa74 0003aa84 \
         00029bd0 00029bd4 00029ca0 00029ca8; do grep -m1 "^0x$a:" /tmp/ave_ua.s; done
python3 -c "import struct;d=open('$B','rb').read();print([hex(x) for x in struct.unpack('<4I',d[0x115f90:0x115fa0])])"

# --- §3/§4: kext ------------------------------------------------------------
python3 tools/disas.py --kext --addr 0xfffffe0008ecc1f8 -n 0x30   # RC+0x1C CDCD -> 30
python3 tools/disas.py --kext --addr 0xfffffe0008eab85c -n 0x340  # SetDataInfo_RC, per frame
python3 tools/disas.py --kext --addr 0xfffffe0008ec1e4c -n 0x400  # PrintRCStats (names)

# --- §4.2/§4.3: the f77 data -------------------------------------------------
python3 tools/rc_track.py results/f77-rc-baseline-1790259856-load1 --bitrate 300000 -v
python3 tools/h264_parse.py results/f77-rc-baseline-1790259856-load1/frame.h264
```

**Spot-checks behind the delegated rows.**

- Userspace: `0xff64` (RC+0 store), `0x29b60` (RC+4 = 30), `0x29c50`–`0x29c58`
  (RC+0x18 = CDCD), `0x2bd40`–`0x2bd50` (the reset to CDCD), `0x3aa74`/`80`/`84`,
  the constant at `0x115f90` (`CDCD, CDCD, 0x33, 0`), `0x29ca0`–`0x29ca8`, and
  `0x29ae0`/`0x29ae4`/`0x29af4` (VP+0xFE9C = 0, VP+0xFEA0 = 0, VP+0xFEA4.. = −1).
- Userspace, compare only: at `0x12598`–`0x125b0` the 0x7766 pass-state compare
  and the `strb 1` were re-read, but not the claim that its base register
  `x28` makes the target VP+0xFE6E.
- Kext: `0xecc204`–`0xecc21c`, `0xeab944`–`0xeab94c`, `0xeab8f8`–`0xeab908`.
- Not re-read here: the kext sweep's claim that no other store reaches the RC
  range between Config and Start_AVC, which was a scan of every
  `client+0xD0xxx` store. The `AVE_PrintRCStats` names are also not re-read,
  but f77's data confirms the two that matter (§4.2).
