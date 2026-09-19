# Rate control, GOP, level limits, and the complete buffer-size table (macOS 13.5)

*What the 13.5 firmware accepts for `ui32RCFlag`, what each mode needs, what the
level machinery enforces, and — the part that matters most — Apple's own size
formula for every surface a session allocates, as a function of width and
height.*

Everything here is **macOS 13.5** (`data/blobs/macos-13.5/`), the build this
machine runs ([43](43-macos-13.5-firmware.md)). Read any line back with:

```sh
export AVE_MACOS=13.5
python3 tools/disas.py --kext --addr 0xfffffe0008ea509c -n 0x2a0   # CalcBufSizeOfRecon
python3 tools/disas.py --fw   --addr 0x2d018 -n 0x1a0              # H264VideoEncoderDPB ctor
```

Per [00-methodology.md](00-methodology.md), every row is **C** (confirmed:
read out of an instruction, VA cited), **I** (inferred: the chain is stated) or
**U** (unknown). 13.5 firmware VAs are image VAs (`__TEXT` vm 0, file `+0x4000`);
13.5 kext VAs are kernelcache `__TEXT_EXEC` VAs.

**Nothing here touched hardware.** Suggested driver changes are proposals.

---

## 0. The five results that change what the driver does

1. **There are exactly three wire values of `ui32RCFlag`: 0, 1, 2.** 3 is
   reachable but is *not* fixed QP, and anything above 3 takes the error
   branch. Bitrate-targeted encoding on 13.5 is `ui32RCFlag = 1`, and it needs
   more than the bitrate field: bitrate (`0xFF30`), frame rate (`0xFF4C`, with
   its divisor at `0xFF48`), the QP clamp (`0xFF88`/`0xFF8C`) — and two "RC is
   really on" gates inside the firmware that we have **not** traced to a host
   field, and that must both be non-zero before a controller is built at all.
   **C** (§1).
2. **There is no HRD/VBV parameter block on 13.5.** The 26.6.2 kext has
   `AVE_RC_DecideVBVMaxBitRate`, `AVE_RC_DecideVBVBufferSize`,
   `AVE_Alg_DecideLevel` and `AVE_Enc_CheckResolution`; the 13.5 kext has
   **none of them** (the same grep finds 16 `VBV`, 6 `DecideLevel`, 6
   `MaxBitRate`, 2 `CheckResolution` symbols in 26.6.2 and 0 in 13.5, so the
   test discriminates). The firmware's rate controller is a bits-per-pixel
   controller with a min/max QP clamp and an optional *data-rate-limit* (DRL)
   leaky bucket, not an HRD model. **C** (§1.5).
3. **The recon surface is four planes, not two, and our driver's LSB window is
   too small above 720p.** `AVE_CalcBufSizeOfRecon` returns four sub-sizes —
   Y_MSB, Y_LSB, UV_MSB, UV_LSB — and the *firmware* computes the same numbers
   itself in the DPB constructor. `UV_MSB = Y_MSB + luma_size` and
   `UV_LSB = Y_LSB + luma_meta_size`, both read out of `setRefPointers`. Our
   `AVE_SESS_LSB_SPAN` of `0x20000` is exactly the 1080p luma-metadata size,
   with nothing left for UV_LSB, and it is 8× too small at 4K. **C** (§5.4,
   §7).
4. **Our recon slot is also too small at 640×480.** `ALIGN(cw*ch*2, 4K)` is
   614,400 there; Apple needs 491,520 for the MSB pair *plus* 20,480 for the
   LSB pair, and our layout puts the MSB pair at `slot + 0x20000`, leaving
   483,328 — an 8,192-byte overrun per slot. **C** (§7).
5. **DPB size is level-derived, in the firmware, by H.264 Annex A.**
   `H264VideoEncoderDPB` computes `min(16, MaxDpbMbs(level) / (mbW*mbH))` from
   a 43-entry jump table, and an out-of-range level silently gives a DPB of
   **2**. **C** (§4.2).

---

## 1. `ui32RCFlag` — the rate-control modes

### 1.1 Where the field lives

`AVEFWRCSettings` is the first `0x680` bytes of
`AVE_SessionSettings_UserKernel_Data`, copied verbatim twice:

```
pInfo[0 .. 0x680)  --memcpy-->  client+0xD0688     AVE_Client_Config   0xfffffe0008ecc1dc
client+0xD0688     --memcpy-->  cmd+0xFF30         MakeFwCmd_Start_AVC 0xfffffe0008ea9b40
```

so **wire offset = `0xFF30` + the `AVEFWRCSettings` member offset**, and the
firmware reads it as `x20 + 0xFED0 + off` with `x20 = cmd - 0x60`
([47](47-abi-13.5-frame-rc-surfaces.md) §2.3). **C** for both memcpys, **I**
for the `0x60` delta (three independent copies agree).

`driver/ave_abi.h` already carries `bitrate = 0xff30`, `rc_mode = 0xff50`,
`qp_i/p/b = 0xffb4/b8/bc`, `qp_min = 0xff88`, `qp_max = 0xff8c` — all five are
re-confirmed below.

### 1.2 The modes

| value | name | what the firmware does | evidence | conf |
|---:|---|---|---|---|
| 0 | `AVE_RC_OFF` | `InitEncodingParameters` builds a `CRateControl` **only** if the controller byte `[x23,#1806]` is set or `AVEFWRCSettings+0x24 == 2`. The slice-header QP is the session per-type QP. | `sub w10,w10,#1; cmp w10,#2; b.cc` `0x5d958`–`0x5d960` (i.e. flag ∈ {1,2} always builds); alternatives `0x5d964`–`0x5d96c` | C |
| 1 | `AVE_RC_ON` | controller built; `ProcessInit` takes the bitrate arm; per-frame QP comes from the RC and is used by the slice-header builder (`[x0,#2768]` `0x20dd4`) | `0x40188`–`0x401d0`, `0x20dd4` | C |
| **2** | **`AVE_RC_FIXQP`** | `ProcessInit` `cmp w11,#2` → copies the three fixed QPs from `sCRCInitParams+92/96/100` to `this+880/884/888`; `ProcessRateControl` returns them by slice type with no rate computation | `0x403a8`–`0x403e8`, `0x41158`, `0x411c0`–`0x411d8` | C |
| 3 | — | **not** fixed QP. `cmp w11,#3; b.hi` rejects >3, then `cmp w11,#2; b.ne 0x40188` sends 3 down the **normal bitrate arm**. `CRateControl` also sets `this+680 = 3` internally under a low-bitrate condition (§1.4), which is a different thing. | `0x403a0`–`0x403ac`, `0x40208` | C |
| >3 | — | `ProcessInit` error branch `0x40d48` | `cmp w11,#3; b.hi` `0x403a0` | C |

The names come from the firmware's own string, which prints the very field the
host wrote with no remap in between:
`"rate control flag: %d(0: AVE_RC_OFF, 1: AVE_RC_ON, 2: AVE_RC_FIXQP)"`
(`CAVCController::DebugInit` `0x4e69c`, arg `[x19,#2828]`, written from
`[x10,#32]` at `0x5ceb4`). **C**.

There is a *second* gate in front of all of this, in `ProcessInit` itself:

```
40104:  ldrb w12,[x20,#177]        ; sCRCInitParams+177
4017c:  cbz  w12, 0x4039c          ; -> the per-flag arm (FIXQP / OFF)
40180:  ldr  w12,[x20,#180]
40184:  cbz  w12, 0x4039c
```

`sCRCInitParams+177` ← a controller byte (`[x22,#3464]`, `0x5db68`) and
`+180` ← the first word of `[sp,#80]` (`0x5db70`–`0x5db80`). **Both must be
non-zero for the bitrate arm to run at all**, even with `ui32RCFlag = 1`.
Neither is a host field we have located. **C** (the branches), **U** (what
sets them). This is the single biggest risk in turning CBR on blind.

### 1.3 `sCRCInitParams` — what `InitEncodingParameters` fills, and from where

Built on the stack at `sp+0xCA0`, memset to 0 for its first `0xE8` bytes
(`0x5d990`), then filled at `0x5d994`–`0x5db94` and passed to
`CRateControl::ProcessInit` (`bl 0x3f6c8` at `0x5dbbc`). `x27` is the
`AVEFWRCSettings` base. All **C** unless noted.

| `sCRCInitParams` | source | wire offset | meaning | evidence |
|---|---|---|---|---|
| `+0` | controller `[x19,#2672]` | — | codec/encode kind; `==1` gates the 10-bit and low-bitrate arms | `0x5d998`, used `0x40150`, `0x401d4` |
| `+4`, `+8` | `[x26]`, `[x26+4]` | — | coded **width**, **height** (`mbW=ceil(w/16)`, `mbH=ceil(h/16)` at `0x40124`–`0x40134`) | `0x5d994` |
| `+12` | `AVEFWRCSettings+0x20` | **`0xFF50`** | `ui32RCFlag` | `0x5d9b0` |
| `+16` | `AVEFWRCSettings+0x00` | **`0xFF30`** | **target bitrate, bits per second** | `0x5d9c0` |
| `+20` | `AVEFWRCSettings+0x1C` | **`0xFF4C`** | **frame rate, Hz** (defaults to 30 if 0; rejected >90 for the low-bitrate arm) | `0x5d9c4`–`0x5d9e0` (`rev64` swap), used `0x40188`–`0x40198` |
| `+24` | `AVEFWRCSettings+0x18` | **`0xFF48`** | frame-rate divisor; `[x19+1340] = p20/p24 − 1` selects a GOP/temporal pattern | `0x5d9e0`, `0x40280`, `0x402d0` |
| `+28` | `AVEFWRCSettings+0x04` | **`0xFF34`** | `ui32IdrPeriod` | `0x5d9e8` |
| `+36` | `AVEFWRCSettings+0x58`, kept only if `< 51`, else **0** | **`0xFF88`** | **min QP** | `0x5d9f0`–`0x5d9fc` |
| `+40` | `AVEFWRCSettings+0x5C`, kept only if `(v−1) < 51`, else **51** | **`0xFF8C`** | **max QP** → `this+864` | `0x5da00`–`0x5da14`, `0x40148` |
| `+44`, `+48` | `AVEFWRCSettings+0x60`, `+0x64` | `0xFF90`, `0xFF94` | not decoded | `0x5da18`–`0x5da30` **U** |
| `+58` | `AVEFWRCSettings+0x4A` (byte) | `0xFF7A` | not decoded | `0x5da48` **U** |
| `+76`,`+80`,`+84` | `AVEFWRCSettings+0x644/8/C` | `0x10574/8/C` | a triple; `+61` is set when their sum is non-zero | `0x5da74`–`0x5da8c` **U** |
| `+92` | `AVEFWRCSettings+0x84` | **`0xFFB4`** | **QP I** | `0x5da90` |
| `+96`, `+100` | `AVEFWRCSettings+0x88`, `+0x8C` | **`0xFFB8`, `0xFFBC`** | **QP P**, **QP B** | `0x5daa0` (8-byte load) |
| `+112` | low half of the same 8-byte load, as `float` | — | reused as a float | `0x5dadc` **I** |
| `+117` | `(AVEFWRCSettings+0x50 != 0)` | **`0xFF80`** | **DRL (data-rate-limit) enable** | `0x5dae4`–`0x5daf0` |
| `+120..159` | 40 bytes from `cmd+0x10528` | **`0x10528`** | `_S_AVE_DRL_Cfg`, then `AVE_DRL_Convert(cfg, 8.0)` | `0x5db30`–`0x5db54` |
| `+177` | controller `[x22,#3464]` | — | **RC master enable** (§1.2) | `0x5db68` **U** (source) |
| `+180` | `[[sp,#80]]` | — | **second RC gate** (§1.2) | `0x5db70`–`0x5db80` **U** |
| `+212` | `(AVEFWRCSettings+0x24 == 2)` | **`0xFF54`** | selects the **alternate bitrate** field | `0x5db04`–`0x5db10` |
| `+216` | literal `0x3F000000` (= 0.5f) | — | constant | `0x5db14` |
| `+220` | `AVEFWRCSettings+0x30` (float) | `0xFF60` | not decoded | `0x5db1c` **U** |
| `+224` | `AVEFWRCSettings+0x28` | **`0xFF58`** | **alternate bitrate**, used when `+0x24 == 2` | `0x5db24` |

The bitrate select, read out of `ProcessInit`:

```
4018c:  ldrb w12,[x20,#212]
401a4:  mov  w12,#0xe0             ; 224
401a8:  mov  w13,#0x10             ; 16
401ac:  csel x12,x13,x12,eq        ; p212==0 ? p+16 : p+224
401b0:  ldr  w12,[x20,x12]         ; <- the bitrate
```

**C.** So the host can put the bitrate at `0xFF30` (the normal case) or at
`0xFF58` (when `0xFF54 == 2`).

### 1.4 What mode 1 actually computes

```
40188:  w11 = p[20]                 ; frame rate
40198:  if (w11 == 0) w11 = 30
401b0:  w12 = bitrate               ; bits/s, per the select above
401b8:  s0  = (float)bitrate / (float)framerate      ; bits per frame
401c0:  s1  = (float)(width * height)
401c8:  this+680 = 1                                  ; mode := 1
401cc:  s0 /= s1                                      ; bits per pixel
401d0:  this+848 = s0
401d4:  if (p[0] == 1 && bitrate <= 5000000 && framerate <= 90
              && (double)bpp <= 0.12)                 ; 0xd1b98
40208:      this+680 = 3                              ; low-bitrate sub-mode
```

`5000000` is `mov w8,#0x4b40; movk w8,#0x4c,lsl#16` at `0x401dc`–`0x401e0`; the
`0.12` is the IEEE-754 double at `0xd1b98`. **C.** That the divisor is the
frame rate and the product a *bits-per-pixel* figure is what fixes the unit of
`0xFF30` as **bits per second** — not bits per frame, not Q16. **C.**

Note the firmware also has `"cdai_debug: bpp=%d.%02d, bitrate=%d, framerate=%d,"`
(file `0xc94af`), the same three quantities. **C** (string), **I** (same site).

### 1.5 What is *not* there: HRD/VBV, peak bitrate, initial fullness

Discriminating check (the same grep over both symbol tables, per trap 2):

| symbol pattern | 26.6.2 kext | 13.5 kext | 26.6.2 fw | 13.5 fw |
|---|---:|---:|---:|---:|
| `VBV` | 16 | **0** | 1 | **0** |
| `DecideLevel` | 6 | **0** | 0 | 0 |
| `MaxBitRate` | 6 | **0** | 0 | 0 |
| `CheckResolution` | 2 | **0** | 0 | 0 |
| `HRD` | 0 | 0 | 3 | **1** |
| `Filler` | 0 | 0 | 3 | **1** |

The test returns a different answer for a case that differs, so the negative is
evidence. **C.**

What survives on 13.5:

- `CRateControl::AVE_CBR_InsertFiller(u32, const HEVC_SLICE_HEADER_PARAMS*, _E_AVE_RCP, _S_AVE_FillerInfo*)`
  (`0x43ad0`) — CBR filler exists, but its mode argument is `_E_AVE_RCP`, a
  *different* enum from `ui32RCFlag`, and it was not decoded. **U**.
- `CAVECommonController::CalcAccumCPB0CntNMbs` and the debug line
  `"IBDebug F %d: IBMODE %d accumCpb0Cnts %d temporalID %d"` — a CPB-0 counter
  exists internally. There is **no host field feeding it**. **C** (symbols),
  **U** (whether the host can reach it).
- `HEVC_SPS::hrd_parameters` — HEVC SPS emission only; the AVC SPS writer has
  `nal_hrd_parameters_present_flag` / `vcl_hrd_parameters_present_flag` as
  *SPS syntax bytes the host supplies verbatim* (`[x21,#1128]`, `[x21,#1129]`
  in `DebugInit`), not as a rate-control input. **C**.
- `_S_AVE_DRL_Cfg` (40 bytes at `cmd+0x10528`, enabled by `0xFF80`) is the only
  leaky-bucket-shaped thing with a host-reachable wire slot. Its field layout
  was **not** decoded. **U**.

**So: there is no VBV buffer size and no initial-fullness field to set on 13.5.**
A driver asking for CBR gets an average-bitrate controller with a QP clamp.

### 1.6 Kext-side validation of the RC block

From `AVE_Client_CheckCommonInfo` (`0xfffffe0008ec9c14`) assert strings
(kernelcache file offsets given; VA = file + `0xfffffe0007004000`):

| assert | file | meaning |
|---|---|---|
| `frameRate == 0xCDCDCDCD \|\| (0 < frameRate && frameRate < 100000)` | `0x1ed1c3` | frame rate 1..99999, or the "unset" sentinel |
| `pInfo->AVEFWRCSettings.ui32RCFlag == AVE_RC_OFF` | `0x1ed24f` | required when the **driver-side** RC is in use |
| `pInfo->AVEFWRCSettings.ui32RCFlag != AVE_RC_OFF` | `0x1ed305` | required when the **firmware** RC is in use |
| `usageMode == AVE_Usage_iChat` | `0x1ed2d1` | driver RC only supports iChat |
| `usageMode == AVE_Usage_{Default,AirPlay,Nero,CarPlay,DefaultDRL}` | `0x1ed389` | firmware RC's permitted usages |
| `2 <= RateControllerSettings.userDPBnumFrames <= 17` | `0x1ed0f5` | host DPB request |
| `pInfo->VideoParams.BFrames <= 3` | `0x1ecf3d` | |
| `0 < sSliceMap.iNum && iNum <= 32` | `0x1ecfaa` | **slice count 1..32** |
| `VideoParamsDriver.RefSpacing < 15` | `0x1ed063` | |
| `numTemporalLayers <= 7` | `0x1ecc86` | |
| `0 < iNumViews <= 2`, `0 < iLayerNum <= 2` | `0x1ec90b`, `0x1ec9b9` | |
| `SPSParams.separate_colour_plane_flag == 0` | `0x1ecaef` | 4:4:4 separate planes rejected |

**C** (strings). These are `AssertMacros`-style checks in the kext, i.e. they
constrain **macOS's** client, not our driver — we build the command ourselves
and the firmware never sees them. They are still the best statement of the
contract. **I** for "the firmware would also reject".

---

## 2. GOP, key frames, intra refresh, temporal layers

### 2.1 The two ways a frame's type is decided

| host writes `PICMGMT+0xCAC` | what happens | evidence | conf |
|---|---|---|---|
| **0 / 1 / 2 / 3 / 7** | `CFlowControllerBase::SendCommandToQueue` enqueues **without** calling `GetFrameType`: the host value is used as given. Our driver's `session_frame_type` path. | `cmp w28,#5` `0x145d4`, `b` to enqueue `0x145dc`–`0x14600` | C |
| **5** (`IMG_UNDECIDED`) | the firmware runs `CAVECommonController::GetFrameType` → `CFrameType::FrameType` and **writes the result back** into `PICMGMT+0xCAC` | `0x145d4` → `0x23c7c`; `str w10,[x23,#3244]` `0x23eb8` | C |
| 4 / 6 | the slice-header builder logs `"prepareSliceHeaderForFW frametype not recognized"` | jump table `0xcef80` entries 4,5,6 → `0x21130` | C |

So **a Linux driver never has to use the firmware GOP**: emit `3` for the IDR
and `1` for each P, exactly what `ave_session.c` does today. `ui32IdrPeriod`
then has no effect on the frame type. **I** — no instruction after
`SendCommandToQueue` was found that overwrites a host-supplied type.

### 2.2 What `ui32IdrPeriod` (wire `0xFF34`) actually reaches

`AVEFWRCSettings+0x04` → `sCRCInitParams+28` (`0x5d9e8`) → `CRateControl+712`
and `+716` (`0x40298`, `0x4029c`). **C.** It is a *rate-control* input (the
I-frame bit budget period), not the GOP driver; the GOP driver is
`CFrameType`, whose init params are a separate 56-byte block memcpy'd at
`0x3d088`. Whether `IdrPeriod` also reaches `CFrameType` was **not** traced:
**U**.

`CFrameType::FrameType` (`0x3d23c`) picks IDR (`3`) when the `RCFrameInfo`
byte at `+53` is zero and a non-IDR type otherwise (`0x3d2bc`–`0x3d2e4`); that
byte is filled from `PICMGMT+0xCB0`, the context index (`strb w27,[sp,#93]`
`0x23d3c`, `RCFrameInfo` base `sp+0x28`). **C** (the two stores), **I** (the
meaning). The rest of the GOP decision (B-pyramid, scene change, look-ahead)
is `CFrameType::UpdateNextScene` / `LookAheadBFrames` and was not decoded:
**U**.

### 2.3 `force_key_frame` — `PICMGMT+0x38`

Confirmed both sides: the kext maps the client's value `3` to `0` and stores
it at `PICMGMT+0x38` (`ldr w8,[x20,#1808]; cmp #3; csel; str [x19,#56]`
`0xfffffe0008eab8d8`–`8e4`), named by the log
`"… forceNonRefFrame %d forceKeyFrame %d bInputCompressed %d"`; the firmware
reads it in `GetFrameType` (`ldr w10,[x23,#56]` `0x23cf0`) and it becomes
`RCFrameInfo+16`. **C.**

**It is only consulted on the `FrameType == 5` path.** With an explicit frame
type the field is dead. **C** (from §2.1).

`forceNonRefFrame` is the neighbouring `PICMGMT+0x3C` (u8, fw `0x23d00`). **C.**

### 2.4 Intra refresh

It exists and is per-frame, not per-session:

| field | offset | type | evidence |
|---|---|---|---|
| `iIntraRefreshHeightDiv64` | `PICMGMT+0xF60` | u16 | kext `strh` `0xfffffe0008eaac68` |
| intra-refresh position | `PICMGMT+0xF62` | u16 | kext `strh` `0xfffffe0008eaac6c` |

with the kext assert `iIntraRefreshHeightDiv64 <= iFrameHeightDiv64`
(`0xfffffe0008eaac38`). **C** (offsets and assert). What the firmware does
with them was not traced: **U**.

### 2.5 `numTemporalLayers` (wire `0xFEB8`)

Kext-validated `<= 7` (assert string `0x1ecc86`,
`pInfo->VideoParams.numTemporalLayers <= 7`). **C** (string). Companion
fields from the same log line: `bIsHierarchical`, `numBTemporalLayers`,
`bMaximizePowerEfficiency` (`0x1e962c`). The firmware side is
`CRateControl::updateTempID(u16,u16)` (`0x439dc`) and
`CRateControl::getHierarchicalMode()` (`0x439d4`), called from `GetFrameType`
at `0x23d64` — the hierarchical mode gates
`CFrameType::SetTransitionToPOnly` (`0x23d90`). **C** (call sites), **U**
(semantics). `BFrames <= 3` is asserted separately (`0x1ecf3d`).

### 2.6 What happens at a GOP boundary

Read out of the slice-header builder's jump table (`0xcef80`):

| type | `[hdr+16]` (slice_type) | side effect | VA |
|---|---|---|---|
| 0 (I, non-IDR) | 2 | `frame_num` **incremented** | `0x21004`–`0x2101c` |
| 1 (P) | 0 | — | `0x21050` |
| 2, 7 (B) | 1 | — | `0x20e38`–`0x20e48` |
| 3 (IDR) | 2 | `[hdr+34] = 1` (`idr_pic_id`/`no_output_of_prior_pics`), **`frame_num` reset to 0** | `0x210e8`–`0x210f8` |

**C** (the stores), **I** (the syntax-element names). The firmware owns
`frame_num` and `idr_pic_id`; there is no host field for either
([47](47-abi-13.5-frame-rc-surfaces.md) §1.2). **C.**

---

## 3. The per-frame feedback loop

### 3.1 `CODED_DATA_HDR` — the structure the firmware writes back

`AVE_CalcBufSizeOfCodedHeader()` returns a bare `0x23000`
(`mov w0,#0x3000; movk w0,#0x2,lsl#16` `0xfffffe0008ea4fb8`). That number is
not arbitrary:

```
0x180                    header block
+ 256 * 0x220            per-slice records      = 0x22000 + 0x180 = 0x22180
+ 0xE80                  trailer
= 0x23000
```

and `AVE_RetrieveRCStats` walks exactly 256 records at stride `0x220`
(`add x25,x25,#0x220`, `cmp x24,#0x100` `0xfffffe0008ec4efc`–`0xec4f04`) and
takes `x22 = hdr + 0x22180` as the end pointer (`0xec4e6c`–`0xec4e70`).
**C.** 256 also matches the kext's slice-count assert bound
(`iNum <= min(32,256)`, `0x1ecfaa`).

Field names come from `AVE_PrintCodedHeader(CODED_DATA_HDR*, …)`
(`0xfffffe0008eb6584`), which prints each one by name:

| offset | name | type | evidence |
|---|---|---|---|
| `+0x000` | `ui32_I_MbCnt[4]` | u32[4] | `ldr w8,[x21,x28,lsl #2]` `0xeb6728` |
| `+0x010` | `ui32_P_MbCnt[4]` | u32[4] | `ldr w8,[x26,#16]`, `x26 = x21 + 4·i` `0xeb681c` |
| `+0x020` | `ui32_Skip_MbCnt[4]` | u32[4] | `ldr w8,[x26,#32]` `0xeb6918` |
| `+0x070` | `ui32_B_MbCnt` | u32 | `ldr w9,[x21,#112]` `0xeb6a1c` |
| `+0x098` | `ui32_SPSPPSHeaderBits` | u32 | `ldr w9,[x21,#152]` `0xeb6b24` |
| `+0x10C` | `FrameNumberFromDriverReturned` | u32 | `ldr w9,[x21,#268]` `0xeb6c2c` |
| `+0x110` | `FrameTypeReturned` | u32 | `ldr w9,[x21,#272]` `0xeb6d34` |
| `+0x180 + 0x220·s` | `ui32BytesWritten` (slice `s`) | u32 | `ldr w9,[x21,#384]` `0xeb6e54`; loop `0xec4ec0` |
| `+0x38C + 0x220·s` | `ui32BytesToRemoveAtTheEndOfTheSliceForContextSwitch` | s8 | `ldrb w8,[x21,#908]` `0xeb6e50`; sign-checked `ldrsb`/`tbnz w9,#31` `0xec4ee8`–`0xec4eec` |
| `+0x398 + 0x220·s` | HEVC-only extra byte count | u32 | added only when `codec == 1` `0xec4ee0` |
| `+0x221A4`, `+0x221AC` | two u64s copied to `S_AVE_DRC_FrameStats+440/+448` | u64 | `0xec4e9c`–`0xec4eb8` |

**C** for every offset (each is a load feeding a named `%d`); **C** for the
names (they are Apple's own strings).

**This re-derives, independently, the `coded_hdr` layout already in
`driver/ave_abi.h:1660`** (`slice_stride 0x220`, `slice_max 0x100`,
`slice_bytes_written 0x180`, `slice_bytes_removed 0x38c`,
`frame_type 0x110`, `frame_num 0x10c`, `sps_pps_bits 0x98`) and agrees with
it field for field, including the `0x180 + 256·0x220 + 0xE80 = 0x23000`
accounting. `ave_cmd_coded_length()` already sums the slices correctly, so
**no driver change is needed for §3** — including for multi-slice.

`FrameNumberFromDriverReturned` is what the kext matches against its own
frame number — assert `pFrameInfo->frameNumber == pCodedHeader->FrameNumberFromDriverReturned`
(`0x1ec86b`). **C.**

### 3.2 `AVE_RetrieveRCStats` (`0xfffffe0008ec4e38`)

```c
void AVE_RetrieveRCStats(_E_AVE_CodecType codec, CODED_DATA_HDR *hdr,
                         S_AVE_DRC_FrameStats *out)   /* 456 bytes, memset 0 */
{
    out->frameType   = hdr->FrameTypeReturned;          /* out+8   0xec4e94 */
    out->u64_a       = *(u64*)((u8*)hdr + 0x221A4);     /* out+440 0xec4ea4 */
    out->u64_b       = *(u64*)((u8*)hdr + 0x221AC);     /* out+448 0xec4eb4 */
    total = 0; overhead = 0; out->nSlices = 0;
    for (s = 0; s < 256; s++) {
        n = slice[s].ui32BytesWritten;
        if (n == 0) break;                              /* 0xec4ec4 */
        out->nSlices++;                                 /* out+4   0xec4ed0 */
        total += n;
        if (codec == HEVC) total += slice[s].extra;     /* 0xec4ee0 */
        r = (int8_t)slice[s].bytesToRemove;
        if (r < 0) goto error;                          /* 0xec5078 */
        overhead += r;
    }
    out->frameBytes = total - overhead;                 /* out+0   0xec4f08 */
    /* … then a fixed run of u32 copies from hdr+0x20 at stride 16 … */
}
```

**C.** `S_AVE_DRC_FrameStats` is `0x1C8` = **456** bytes
(`mov w2,#0x1c8; bl bzero` `0xec4e80`–`0xec4e84`).

**So the bitstream length a driver must report to userspace is
`Σ ui32BytesWritten − Σ bytesToRemove` over the slices with a non-zero
`ui32BytesWritten`** — not a single field. For our single-slice I-frame that
is `slice[0].ui32BytesWritten − slice[0].bytesToRemove`. **C.**

The negative `bytesToRemove` is the firmware's own error signal; the kext
treats it as a hard failure. **C.**

### 3.3 Does the firmware's controller need anything fed back?

**No.** Scanning `CRateControl`'s entry points:

- `ProcessRateControl(...)` (`0x40e40`) is called by the controller *itself*
  per frame; the host does not call it.
- `ProcessAccumulate` (`0x427cc`) / `Accumulate` (`0x3f8bc`) /
  `UpdateBits(u32, u32, sCRCStatPerFrame*, Engine, bool)` (`0x3f99c`) take
  their statistics from `sCRCStatPerFrame`, which the firmware fills from the
  hardware, not from a host command.
- The only host-supplied *per-frame* rate-control surface is
  `AVE_PICMGMT_RC_UPDATE_DATA`, the third argument of
  `CRateControl::AVE_DRL_UpdateBitrate(_S_AVE_DRL_Cfg*, RCFrameInfo*, AVE_PICMGMT_RC_UPDATE_DATA*)`
  (`0x43ca4`) — i.e. the *data-rate-limit* path only, plus the two per-frame
  flags `throughputRateMode` (`PICMGMT+0x6DC`) and
  `bChangeBitrateAtNextIDR` (`PICMGMT+0x6EB`)
  ([47](47-abi-13.5-frame-rc-surfaces.md) §1.2).

**C** (symbols and signatures), **I** (the conclusion "fully autonomous"), on
the strength of the symbol list being complete for the class.

A **host-side** rate controller has everything it needs from §3.1 plus the
fixed-QP mode: measure `frameBytes`, pick the next QP, rewrite
`AvcInit+0xFFB4/B8/BC` — except that those are *Start_AVC* fields and QP is
session-scoped on 13.5 for flags 0 and 2 (no per-frame QP writer exists in
`CAVCController`; write scan over the whole image, [47](47-abi-13.5-frame-rc-surfaces.md)
§2.4). **So per-frame host QP control needs a new Start_AVC per QP change, or
mode 1.** **C** (the write scan), **I** (the consequence).

---

## 4. Level and profile limits

### 4.1 Dimensions

`0xfffffe0008ea3d9c`, called from `AVE_Client_Config` at `0xfffffe0008ec90a4`:

```
ea3da0:  cmp  w0, #0x1, lsl #12      ; W <= 4096
ea3da4:  mov  w8, #0x1000
ea3da8:  ccmp w1, w8, #0x2, ls       ; H <= 4096
ea3dac:  cset w8, ls
ea3db0:  cmp  w1, #0x60              ; H >= 96
ea3db4:  csel w8, wzr, w8, cc
ea3db8:  cmp  w0, #0xc0              ; W >= 192
ea3dbc:  csel w0, wzr, w8, cc
```

**192 ≤ W ≤ 4096, 96 ≤ H ≤ 4096**, hard-coded, no table, no level term.
**C.** (26.6.2 reaches the same numbers through `AVE_Enc_CheckResolution` and
a device-capability table; 13.5 has no such symbol.)

`AVE_CalcBufSizeOfCodedData` on 13.5 has **no** `W*H >= 2^31` overflow guard
(the 26.6.2 one does) — at 4096×4096 the products still fit, so this is
recorded, not a hazard. **C** ([47](47-abi-13.5-frame-rc-surfaces.md) §3).

### 4.2 DPB size is level-derived — in the firmware

`H264VideoEncoderDPB::H264VideoEncoderDPB(u32 W, u32 H, SH_PROFILE_TYPE,
SH_LEVEL_TYPE, u32, u32, bool, bool, u32)` at fw `0x2d018`:

```
2d028:  mov  w9, #2                  ; default DPB = 2
2d02c:  sub  w11, w4, #0xa           ; level - 10
2d030:  cmp  w11, #0x2a
2d034:  b.hi 0x2d14c                 ; level outside 10..52  -> DPB = 2
2d038:  adr  x12, 0xd0780            ; 43-entry byte jump table
2d04c:  lsr  w10, w1, #4             ; mbW = W >> 4   (truncating)
2d050:  lsr  w11, w2, #4             ; mbH = H >> 4
...     mbs = mbW * mbH
        if (mbs < MaxDpbMbs/16 + 1) w9 = 16      ; 0x2d148
        else                        w9 = MaxDpbMbs / mbs   ; udiv 0x2d140
2d16c:  [dpb+8] = w9                 ; the DPB frame count
2d194:  [dpb+36] = profile ; [dpb+40] = level
```

The jump table at `0xd0780` decodes to exactly H.264 Annex A `MaxDpbMbs`:

| level | `MaxDpbMbs` | target | level | `MaxDpbMbs` | target |
|---|---:|---|---|---:|---|
| 1.0 | 396 | `0x2d0b0` | 3.1 | 18 000 | `0x2d130` |
| 1.1 | 900 | `0x2d0c4` | 3.2 | 20 480 | `0x2d0ec` |
| 1.2, 1.3, 2.0 | 2 376 | `0x2d058` | 4.0, 4.1 | 32 768 | `0x2d080` |
| 2.1 | 4 752 | `0x2d0d8` | 4.2 | 34 816 | `0x2d100` |
| 2.2, 3.0 | 8 100 | `0x2d06c` | 5.0 | 110 400 | `0x2d114` |
| | | | 5.1, 5.2 | 184 320 | `0x2d094` |

Every other index (x.3 … x.9) jumps straight to `0x2d14c` and keeps the
default **2**. **C** — the table bytes were read out of the image and each
target's constant disassembled.

**Consequences for the driver.** `ave_cmd.c` sends profile 66 / level 40. At
level 4.0, `MaxDpbMbs = 32768`:

| resolution | `mbW·mbH` (truncating) | firmware DPB |
|---|---:|---:|
| 640×480 | 40·30 = 1 200 | 16 (1200 < 2049) |
| 1280×720 | 80·45 = 3 600 | **9** (32768/3600) |
| 1920×1088 | 120·68 = 8 160 | **4** |
| 3840×2160 | 240·135 = 32 400 | **1** — and level 4.0 is far below 4K anyway |

**C** (arithmetic). Sending an out-of-range level (e.g. 41 is fine, 43 is not)
silently yields DPB 2 rather than an error — a trap worth avoiding.

Separately, the kext caps the *surface* counts: `Recon <= 17`
(`mov w9,#0x11` `0xfffffe0008ea505c`) and `refNum <= 16`
(`0xfffffe0008ec67b0`), and the host request is asserted
`2 <= userDPBnumFrames <= 17` (`0x1ed0f5`). **C.**

### 4.3 What is *not* enforced

- **No macroblock-rate (`MaxMBPS`) check** anywhere in either 13.5 binary:
  the level table above is the only `SH_LEVEL_TYPE` consumer that reaches a
  numeric limit, and it uses `MaxDpbMbs` only. `AVE_Alg_DecideLevel` and
  `AVE_RC_DecideVBVMaxBitRate` exist in 26.6.2 and **not** in 13.5 (§1.5).
  **C**, with the positive control.
- **No bitrate-vs-level check.** `AVE_CalcBufSizeOfCodedData` reads no level,
  profile, bitrate or frame rate ([18](18-coded-data-sizing.md) §5, re-read on
  13.5 in [47](47-abi-13.5-frame-rc-surfaces.md) §3). **C.**
- **Profile** (`SH_PROFILE_TYPE`) is stored at `dpb+36` and used for
  `direct_8x8_inference` / CABAC gating in the SPS/PPS the host supplies; no
  numeric limit hangs off it. **I** (from the single store and no other
  consumer in the ctor).

---

## 5. The complete buffer-size table

### 5.1 How to read it

Apple computes every surface size in `AVE_Client_CalcSurfaceInfo`
(`0xfffffe0008ec67c4`), which calls the 23 `AVE_CalcBufSizeOf*` /
`AVE_CalcBufNumOf*` pairs in a fixed order (`bl` list at `0xec6a28` …
`0xec6fb8`) and writes each result into `_S_AVE_SurfaceInfoSet` at the offsets
[47](47-abi-13.5-frame-rc-surfaces.md) §4 lists. Every formula below was
disassembled in this pass.

Notation: `Wa16 = ALIGN(W,16)`, `mbW = ceil(W/16)`, `mbH = ceil(H/16)`,
`AD(x,a) = ALIGN_DOWN(x,a)`, `AU(x,a) = ALIGN_UP(x,a)`,
`npo2(n)` = the next power of two ≥ n. All rows are **AVC (`CodecType == 0`),
8-bit, 4:2:0 (`CHROMA_FORMAT == 1`), DevType 12 (t6001)** unless stated.

Two allocation-side rules apply to *every* row:

- `AVE_SURFACE_ALLOC_ALIGN = max(PAGE_SIZE, 0x4000)` → **every surface is
  rounded up to 16 KiB** by the allocator (kext `0xfffffe0008f33128`–`3144`,
  same on both versions). **C.**
- The DART page granule on our side is 16 KiB too, so a 16 KiB-rounded
  allocation never shares a page with another surface.

### 5.2 The table

| surface | Apple's formula | function | align / assert | 640×480 | 1280×720 | 1920×1088 | 3840×2160 | conf |
|---|---|---|---|---:|---:|---:|---:|---|
| **ParameterSet** | `512` (AVC); `1024·n` (HEVC) | `0xea4bb4` | — | 512 | 512 | 512 | 512 | C |
| **MBInputCtrl** | `AU(Wa16 · mbH, 4096)` = 16 B/MB | `0xea4bf8`–`c38` | 4 KiB in the formula | 20 480 | 61 440 | 131 072 | 520 192 | C |
| **MBStats** | `AU(432 · mbW · mbH, 4096)` | `0xea4cfc`–`d50` | 4 KiB | 520 192 | 1 556 480 | 3 526 656 | 14 000 128 | C |
| **CodedData** | `AU(max(min(2·base,460800), base), 4096)`, `base = Wa16·H·3/2` | `0xea4d58` | 4 KiB; fw asserts `CodedBufSize > 3·W·H/4` (`0x58358`–`0x58370`, strings `0xc4a4c`/`0xc4a07`) | 462 848 | 1 384 448 | 3 133 440 | 12 443 648 | C |
| **CodedHeader** | `0x23000`, constant | `0xea4fb4` | — | 143 360 | 143 360 | 143 360 | 143 360 | C |
| **SliceHeader** | `0x40000`, **but count is 0 for AVC** | `0xea4ff8`; count `0xea4fc4` (`codec != 1 → 0`) | — | — | — | — | — | C |
| **Recon Y_MSB** | `AD(32·(W+31),1024) · (bd/8) · ((H+35)>>5)` = `1024·ceil(W/32)·ceil((H+4)/32)` at 8-bit | `0xea528c`–`0xea52a8`; fw `0x2d14c`–`0x2d168` | 128 (fw asserts `& 127` `0x55310`/`0x54f94`) | 327 680 | 942 080 | 2 150 400 | 8 355 840 | C |
| **Recon Y_LSB** (tile metadata) | `AU(32 · npo2(ceil(W/32)) · npo2(ceil((H+4)/32)), 128)` | `0xea52f4`–`0xea531c`; fw `0x2d174`–`0x2d1a0` | 128 (`& 127` `0x58068`) | 16 384 | 65 536 | 131 072 | 524 288 | C |
| **Recon UV_MSB** | `AU(AD(32·(W/2)+480, 512) · (bd/8) · ((H/2+19)>>4), 128)` = `512·ceil(W/32)·ceil((H/2+4)/16)` | `0xea52ac`–`0xea52f0` | 128 (`& 127` `0x5541c`) | 163 840 | 471 040 | 1 075 200 | 4 177 920 | C |
| **Recon UV_LSB** | `AU(8 · npo2(ceil(W/32)) · npo2(ceil((H/2+4)/16)), 128)` | `0xea5324`–`0xea535c` | 128 | 4 096 | 16 384 | 32 768 | 131 072 | C |
| **Recon total** (return value) | sum of the four | `0xea54a0`–`0xea54a8` | | **512 000** | **1 495 040** | **3 389 440** | **13 189 120** | C |
| *Recon, uncompressed arm* (`bool = 0`) | Y `AU(256·mbW·mbH,512)`, UV `128·mbW·mbH`, both LSB = 0 | `0xea51c4`–`0xea5218` | 128 | 307 200 + 153 600 | 921 600 + 460 800 | 2 088 960 + 1 044 480 | 8 294 400 + 4 147 200 | C |
| **Colocated** | `128 · mbW · mbH` | `0xea5560`–`0xea55a8` | 64 (docs/60) | 153 600 | 460 800 | 1 044 480 | 4 147 200 | C |
| **LowResRef** (per DPB slot) | `AU(AD(4·W+252,256) · ((H+63)>>4), 512)` = `AU(ALIGN(4W,256)·ceil(H/16 rounded to 4), 512)` | `0xea5668`–`0xea56dc` | 64 (fw `0x523dc`, `setLRME:5783`) | 84 480 | 245 760 | 545 280 | 2 119 680 | C |
| **LowResResult** (×4) | `ALIGN(4·W,128) · ceil(H/64) + 1024` | `0xea5840`–`0xea5858`; count `0xea5708` = 4 for DevType > 10 | 64 (`setPipe:6184/6185`) | 21 504 | 62 464 | 131 584 | 523 264 | C |
| **LowResRCResult** | `AD(4·W+252,256) · ((H+255)>>8)`; **count 0** unless a client flag is set | `0xea592c`–`0xea5940`; count `0xea58a4` | — | 5 120 | 15 360 | 38 400 | 138 240 | C |
| **SrcNeighborInfo** (×1) | `max(AD(16·W+240,256), 16384)` = `max(256·mbW, 16 KiB)` | `0xea5970`–`0xea59b0` | 64; fw `setPipe:6990` asserts non-zero | 16 384 | 20 480 | 30 720 | 61 440 | C |
| **SrcNeighborPixel** (×1) | `max(AD(64·W+960,1024), 16384)` = `max(1024·mbW, 16 KiB)` | `0xea59e8`–`0xea5a28` | 64 | 40 960 | 81 920 | 122 880 | 245 760 | C |
| **SrcNeighborData** (×4) | `max(56·mbW, 16384)` | `0xea5a70`–`0xea5a98`; count table `0xfffffe000722f1e8[DevType-12]` = **4** | 64; `:7928` asserts non-zero | 16 384 | 16 384 | 16 384 | 16 384 | C |
| **SrcNeighborFwData** (×1) | `max(AD(AD(4·W+188,64)−1,128), 16384)` ≈ `max(64·mbW, 16 KiB)` | `0xea5adc`–`0xea5b04`; count `0xea5aa8` = 1 for DevType 12 | 64 | 16 384 | 16 384 | 16 384 | 16 384 | C |
| **EntropyCoding** | `AD(64·W+960,1024) · K`, `K = flag ? ceil(mbH/4) : 8` | `0xea5be0`–`0xea5c6c` | 64 (fw `0x59558`/`0x595a0`, `:8020`/`:8021`) | 327 680 (K=8) | 983 040 (K=12) | 2 088 960 (K=17) | 8 355 840 (K=34) | C |
| — count | `4 · n` for AVC (`2 · n` HEVC), 0 if the bool is 0; `n` = `sSVEMap.iNum` | `0xea5ba8`–`0xea5bcc` | | 4 | 4 | 4 | 4 | C (arithmetic), I (`n`) |
| **TranscodedData** (×2) | `AU(CodedData/2, 4096)` | `0xea5b44`–`0xea5b78` | 4 KiB | 233 472 | 692 224 | 1 568 768 | 6 221 824 | C |
| **CrcQPMod** (×2 if enabled) | `AU(Wa16 · mbH, 4096)` | `0xea5c88`–`0xea5ca4` | 4 KiB | 20 480 | 61 440 | 131 072 | 520 192 | C |
| **InitParamsCopy** | `0x32D68`, constant | `0xea5cb4` | — | 208 232 | 208 232 | 208 232 | 208 232 | C |
| **FwClient** | `arg ? arg : 0x100000` | `0xea5ccc` | — | 1 048 576 | 1 048 576 | 1 048 576 | 1 048 576 | C |
| **FwClientMem** | `arg << 16` (64 KiB units) | `0xea5cf0` | — | *host-chosen* | | | | C |

**On the 1080p column.** It is computed at **1920×1088**, the MB-aligned coded
height [38](38-dimension-convention.md) §7 recommends. At a literal
`H = 1080` the rows that change are: `CodedData` 3 112 960,
`Recon Y_MSB` 2 088 960, `Recon UV_MSB` 1 044 480, `Recon total` 3 297 280,
`TranscodedData` 1 556 480. Every other row is identical, because every other
formula rounds `H` up to a multiple of 16 or more. **C.**

Counts, for completeness (all **C**): `ParameterSet` 1 (`0xea4ba8`);
`Recon` `min(numRefs+1, 17)` (`0xea5034`–`0xea5064`); `Colocated`
`numRefs+1` or 2 (`0xea553c`); `LowResRef` `numRefs+1`, floored at 5 when a
flag is set (`0xea55d8`); `CodedData`/`CodedHeader` capped at **20**
(`0xea4cc4`–`0xea4cf4`).

### 5.3 Cross-checks against what we already have

| value | source | this table | agrees |
|---|---|---|---|
| CodedData 1 384 448 / 3 112 960 / 12 443 648 | [18](18-coded-data-sizing.md) §8, re-run on 13.5 | same (at H = 1080) | yes |
| Recon uncompressed 2 088 960 + 1 044 480 at 1080p | [47](47-abi-13.5-frame-rc-surfaces.md) §4 | same | yes |
| LowResRef 245 760 at 720p, 545 280 at 1080p | [65](65-pframes.md) §4.1, `ave_session.c` comment | same | yes |
| LowResResult 62 464 at 720p | [65](65-pframes.md) §4.1 | same | yes |
| Colocated 128·MBs, MBStats `AU(432·MBs,4K)` | [47](47-abi-13.5-frame-rc-surfaces.md) §4 | same | yes |
| SrcNeighbor 256/1024/56/64 per MB column, ≥16 KiB | [47](47-abi-13.5-frame-rc-surfaces.md) §4 | same | yes |
| Entropy `AD(64W+960,1024)·K` | `ave_session.c` comment | same, and the `K` selector is now pinned to `sCalc` arg 6 | yes |
| CodedHeader `0x23000` | [47](47-abi-13.5-frame-rc-surfaces.md) §3 | same, and §3.1 explains the number | yes |

### 5.4 The recon plane layout — where the four sub-sizes go

The kext writes the four sub-sizes to `InfoSet+0xA0/0xA4/0xA8/0xAC` in the
order `{Y_MSB, Y_LSB, UV_MSB, UV_LSB}` (`add x8,x19,#0xa0` … `stp` at
`0xfffffe0008ec6c0c`–`0xec6c20`, stores at `0xea5478`–`0xea549c`). **C.**

The *firmware* derives the two chroma addresses itself. `CAVECommonDPB::setRefPointers`:

```
2c314:  ldr  x12, [x2, #72]          ; Y_MSB base   (DPB entry +72)
2c318:  ldp  x11, x8, [x2, #216]     ; x11 = Y_LSB base (DPB entry +216)
2c31c:  ldp  w10, w9, [x0, #20]      ; w10 = luma_size, w9 = luma_meta_size
2c324:  add  x8,  x12, x10           ; UV_MSB = Y_MSB + luma_size
2c334:  add  x11, x11, x9            ; UV_LSB = Y_LSB + luma_meta_size
2c338:  str  x12, [x1, #2200]        ; PICMGMT+0x898  sRecon.Y_MSB
2c328:  str  x11, [x1, #2208]        ; PICMGMT+0x8A0  sRecon.Y_LSB
2c32c:  str  x8,  [x1, #2216]        ; PICMGMT+0x8A8  sRecon.UV_MSB
2c33c:  str  x11, [x1, #2224]        ; PICMGMT+0x8B0  sRecon.UV_LSB
```

and `[x0+20]` / `[x0+24]` are written by the DPB constructor with **exactly the
kext's two formulas**:

```
2d14c:  w10 = W + 31
2d150:  w11 = w10 << 5
2d154:  w11 &= ~0x3ff                ; 1024 * ceil(W/32)
2d158:  w8  = w11 * bitDepth
2d15c:  w11 = (H + 0x23) >> 5        ; ceil((H+4)/32)
2d164:  w8 >>= 3
2d168:  w8 *= w11                    ; luma_size
2d170:  [dpb+16] = H ; [dpb+20] = luma_size
2d174…: w8 = ALIGN(32 * npo2(ceil(W/32)) * npo2(ceil((H+4)/32)), 128)
2d1a4:  [dpb+24] = w8                ; luma_meta_size
```

**C**, and this is the strongest kind of evidence available here: the host
allocator and the coprocessor compute the same number independently
([00-methodology.md](00-methodology.md), "both sides agreeing").

**Therefore a DPB slot needs two bases, not one:**

```
MSB base (Start_AVC recon entry +0x00) : Y_MSB, then UV_MSB at +luma_size
                                         → luma_size + chroma_size bytes
LSB base (Start_AVC recon entry +0x08) : Y_LSB, then UV_LSB at +luma_meta_size
                                         → luma_meta_size + chroma_meta_size bytes
```

Both bases must be 128-aligned; the firmware asserts `& 127 == 0` on all four
derived addresses (`0x55310`, `0x54f94`, `0x58068`, `0x5541c`). **C.**

Required per-slot bytes:

| | MSB region | LSB region |
|---|---:|---:|
| 640×480 | 491 520 | 20 480 |
| 1280×720 | 1 413 120 | 81 920 |
| 1920×1088 | 3 225 600 | 163 840 |
| 3840×2160 | 12 533 760 | 655 360 |

---

## 6. What scales with something other than the resolution

| quantity | scales with | evidence |
|---|---|---|
| `Recon`, `Colocated`, `LowResRef` **counts** | `max_num_ref_frames + 1`, capped 17 / 16 | `0xea5034`, `0xea553c`, `0xea55d8`, `0xea505c`, `0xec67b0` |
| firmware DPB frame count | **level** (H.264 Annex A `MaxDpbMbs`), §4.2 | fw `0x2d018`–`0x2d16c` |
| `EntropyCoding` count | `4 · n`, `n` = `sSVEMap.iNum` (SVE cores) — **not** slices | `0xea5ba8`–`0xea5bcc` (C); `n`'s identity from the `4·iNum × 4` table shape at `ave_abi.h:1091` (I) |
| `EntropyCoding` size `K` | `ceil(mbH/4)` or a flat 8, by a client bool | `0xea5bf4` |
| `SrcNeighbor*` counts | **DevType**, via a table — Info 1, Pixel 1, Data 4, FwData 1 on DevType 12 | `0xea5948`, `0xea59c0`, `0xea5a38` + table `0x722f1e8` = `{4,4,1,1,1,4,4}`, `0xea5aa8` |
| `CodedData` / `CodedHeader` count | client fields, capped **20** | `0xea4c6c`–`0xea4cf8` |
| `LowResResult` count | DevType (`>10 → 4`, else 8) | `0xea5708` |
| `TranscodedData` count | DevType ≥ 9 and a client word → 2 | `0xea5b14` |

**Nothing in the size table takes a slice count.** Every
`AVE_CalcBufSizeOf*` signature was read; none has a slice parameter, and
`AVE_CalcBufNumOfSliceHeader` returns **0** for AVC (`codec != 1 → 0`,
`0xea4fc4`–`0xea4ff4`). **C.**

### 6.1 Multi-slice (`sSliceMap.iNum > 1`)

| fact | evidence | conf |
|---|---|---|
| `sSliceMap` is **260 (`0x104`) bytes** at wire `0xFDAC`, memcpy'd verbatim into the controller at `+0xCC` | fw `mov w2,#0x104; add x1,x21,#0xfdac; bl memcpy` `0x1440c`–`0x1441c` | C |
| the firmware asserts `pInVideoParams->sSliceMap.iNum>=1` | fw string `0xcd88e` | C |
| the kext asserts `0 < iNum <= 32` | kext string `0x1ecfaa` | C |
| the slice count is printed as `"number of slices: %d"` from controller `+5172` | fw `0x4e6xx`, `DebugInit` | C |
| `CODED_DATA_HDR` already has room for **256** slice records, so it does not grow | §3.1 | C |
| **no buffer size changes** with `iNum` | §6 | C |
| what changes is the *content* of `CODED_DATA_HDR`: `out->nSlices` counts the non-empty records and the frame length becomes a sum | §3.2 | C |
| the layout of the 256 bytes after `iNum` (per-slice first-MB, or a per-MB map) | — | **U** |

For a first multi-slice bring-up the safe reading is: `iNum` at `0xFDAC+0`
(**I** — `ave_abi.h:1464` already marks this inferred), the remaining 256
bytes zero, and the size table unchanged. The bitstream-length computation
needs no change either: `ave_cmd_coded_length()` already walks all 256 slice
records and sums them (§3.1).

---

## 7. Where `ave_session.c` is wrong today

Our driver sizes seven things itself. Measured against §5:

| driver constant / expression | today | Apple | verdict |
|---|---|---|---|
| `recon_slot = ALIGN(cw·ch·2, 4K)` with `LSB at +0`, `MSB at +0x20000` | 614 400 / 1 843 200 / 4 177 920 / 16 588 800 | needs `LSB 20 480 / 81 920 / 163 840 / 655 360` **and** `MSB 491 520 / 1 413 120 / 3 225 600 / 12 533 760` | **BROKEN at 640×480 (MSB overruns the slot by 8 192 B), at 1080p and 4K (LSB overruns into MSB by 32 768 / 524 288 B).** Correct at 1280×720 only |
| `AVE_SESS_LSB_SPAN 0x20000` | fixed | `luma_meta + chroma_meta`, resolution-dependent | **wrong above 720p** |
| `AVE_SESS_CODED_SIZE 0x200000` | fixed 2 MiB | 462 848 / 1 384 448 / 3 133 440 / 12 443 648 | **too small at ≥1080p**; 4.5× over-allocated at 640×480 |
| `AVE_SESS_PARAM_SETS_SIZE 0x1000` | 4 KiB | 512 | over-allocated, harmless (and deliberately so — the fw's length check is against a zero field, `0x5de44`) |
| `AVE_SESS_FWCLIENT_FALLBACK 0xb4000` | 737 280 | `0x100000` when the argument is 0 | under Apple's default; only used when `ave->client_buf_size` is 0 |
| colocated `ALIGN(128·(cw/16)·(ch/16), 4K)` | | `128·mbW·mbH` | **correct** (rounded up) |
| `ave_session_lowres_size` | `ALIGN(ALIGN(4W,256)·((H+63)>>4), 512)` | identical | **correct** |
| `ave_session_lowres_result_size` | `ALIGN(4W,128)·ceil(H/64)+1024` | identical | **correct** |
| `ave_session_entropy_size` | `AD(64·cw+960,1024) · max(8, ceil(mbH/4))` | identical, `K` selected by a client bool | **correct** (takes the larger `K`) |
| SrcNeighbor slot = `max_g(per_mb_col[g]·mb_cols)`, ≥16 KiB, 16 KiB-aligned | | per-group `256/1024/56/64 · mbW`, each ≥16 KiB | **correct**, uniformly over-allocated to the Pixel group's size |

The two recon bugs are the dangerous ones: the 640×480 case writes past the
end of the last DPB slot's allocation, which on our DART is an **unmapped
page** — a fault, not corruption. The prompt's concern is exactly right.

---

## 8. Reproduce

```sh
export AVE_MACOS=13.5

# --- rate control -------------------------------------------------------
python3 tools/disas.py --fw   --addr 0x400dc -n 0x340   # CRateControl::ProcessInit
python3 tools/disas.py --fw   --addr 0x5d900 -n 0x300   # sCRCInitParams construction
python3 tools/disas.py --fw   --addr 0x4e69c -n 0x10    # "rate control flag ... FIXQP"
python3 tools/disas.py --fw   --addr 0x43ad0 -n 0x20    # AVE_CBR_InsertFiller
python3 tools/disas.py --fw   --addr 0x43ca4 -n 0x20    # AVE_DRL_UpdateBitrate
for p in VBV DecideLevel MaxBitRate CheckResolution; do
  grep -c "$p" data/derived/kext-symbols.txt data/blobs/macos-13.5/derived/kext-symbols.txt
done

# --- GOP / frame type ---------------------------------------------------
python3 tools/disas.py --fw   --addr 0x23c7c -n 0x120   # GetFrameType
python3 tools/disas.py --fw   --addr 0x3d23c -n 0x160   # CFrameType::FrameType
python3 tools/disas.py --fw   --addr 0x20d44 -n 0x400   # frame type -> slice header

# --- feedback -----------------------------------------------------------
python3 tools/disas.py --kext --addr 0xfffffe0008ec4e38 -n 0x120  # RetrieveRCStats
python3 tools/disas.py --kext --addr 0xfffffe0008eb6584 -n 0x9f8  # PrintCodedHeader

# --- level / DPB --------------------------------------------------------
python3 tools/disas.py --fw   --addr 0x2d018 -n 0x1a0   # H264VideoEncoderDPB ctor
python3 - <<'EOF'
d = open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
print(list(d[0xd0780+0x4000:0xd0780+0x4000+43]))   # the 43-entry level table
EOF

# --- sizes --------------------------------------------------------------
python3 tools/disas.py --kext --addr 0xfffffe0008ea4ba8 -n 0xa4   # ParameterSet, MBInputCtrl
python3 tools/disas.py --kext --addr 0xfffffe0008ea4cfc -n 0x5c   # MBStats
python3 tools/disas.py --kext --addr 0xfffffe0008ea509c -n 0x410  # Recon (all arms)
python3 tools/disas.py --kext --addr 0xfffffe0008ea553c -n 0xd0   # Colocated
python3 tools/disas.py --kext --addr 0xfffffe0008ea560c -n 0x120  # LowResRef
python3 tools/disas.py --kext --addr 0xfffffe0008ea5720 -n 0x1a4  # LowResResult, SrcNeighbor
python3 tools/disas.py --kext --addr 0xfffffe0008ea5b14 -n 0x1fc  # Transcoded..FwClientMem
python3 tools/disas.py --kext --addr 0xfffffe0008ea3d9c -n 0x28   # 192..4096 x 96..4096
python3 tools/disas.py --kext --addr 0xfffffe0008ec67c4 -n 0x820  # the caller, in order
python3 tools/disas.py --fw   --addr 0x2c314 -n 0x30              # setRefPointers recon quad
```

---

## 9. Proposed hardware experiment (operator only — **do not run**)

Per [AGENTS.md](../AGENTS.md), this is written down, not performed.

**One load, one question: does `ui32RCFlag = 1` reach a working controller?**

Parameters (all reachable from existing module parameters plus the three new
ones proposed in the reply):

```
session_frame=1 session_frames=16
session_width=1280 session_height=720
rc_mode = 1          (AVE_RC_ON,   wire 0xFF50)
bitrate = 200000     (200 kbit/s,  wire 0xFF30)   deliberately unreachable
framerate = 30       (wire 0xFF4C; 0xFF48 = 1)
qp_i = qp_p = 30     (wire 0xFFB4/B8)
qp_min = 10          (wire 0xFF88)
qp_max = 51          (wire 0xFF8C)
frame types: IDR then 15 × P, explicit (PICMGMT+0xCAC), so the firmware GOP
             stays out of it
```

Read out of one run:

1. the firmware log line `rate control flag: 1` (`0x4e69c`) — proves the field
   arrived;
2. per frame, `Σ ui32BytesWritten − Σ bytesToRemove` from `CODED_DATA_HDR`
   (§3.1/§3.2);
3. per frame, `slice_qp_delta` parsed out of the returned slice header, added
   to `pic_init_qp_minus26 + 26`.

**Discriminator.** Under `AVE_RC_FIXQP` the slice QP provably *cannot* vary:
the only writers of the controller's fixed-QP words are
`InitEncodingParameters` and `SetDefaultParameters`, and a write scan over the
whole image finds no per-frame writer ([47](47-abi-13.5-frame-rc-surfaces.md)
§2.4). So:

- **QP rises above 30 across the 16 frames** → the firmware rate controller is
  live and both hidden gates (`sCRCInitParams+177`, `+180`, §1.2) are already
  satisfied by whatever we send. Rate control works.
- **QP stays at 30 and the byte counts are flat** → the controller was not
  built; the gates are the blocker and the next step is to find what sets
  controller `+3464` and the word behind `[sp,#80]`.

Both outcomes are informative, which is the point. The negative control is
free: the *same* build with `rc_mode = 2` must give a flat QP — if it does not,
the measurement apparatus is wrong, not the firmware
([00-methodology.md](00-methodology.md) trap 2).

**Not** a risk to the machine beyond what a Process already is: no new
surface, no new DMA, four extra u32s in a command the firmware already parses.

---

## 10. Annotations owed to earlier documents

Nothing below is a retraction; each is a 13.5-specific addition beside a
26.6.2 statement that stands for 26.6.2.

- **[38](38-dimension-convention.md) §5 and §7** cite
  `AVE_CalcBufSizeOfRecon` (26.6.2 `0xfffffe0008b606c0`) as sizing 32-px tiles
  as `ceil((d+4)/32)` in **both** axes, and give
  `61 × 35 × 1024 = 2 186 240` at 1920×1088. On **13.5** the two axes differ:
  the width term is `ceil(W/32)` (`add w15,w2,#0x1f; lsl #5; and ~0x3ff`
  `0xea5294`–`0xea529c`) and only the height term carries the `+4` margin
  (`add w10,w3,#0x23; lsr #5` `0xea528c`–`0xea5290`). At 1920×1088 that is
  `60 × 35 × 1024 = 2 150 400`, one tile column less. The *firmware* agrees
  with the kext (fw `0x2d14c`–`0x2d168`), so this is not an ambiguity.
  §7's recommendation (send MB-aligned dimensions, 1088 not 1080) is
  unaffected and gets stronger: at `H = 1080` the 13.5 formula gives 34 tile
  rows with **no** margin, at 1088 it gives 35.
- **[47](47-abi-13.5-frame-rc-surfaces.md) §2.4** notes
  `params+0x24`/`+0x28 ← RC+0x58`/`+0x5C` "likely min/max QP", **I**. Now
  **C** for the roles: `+0x28` (max) goes straight to `CRateControl+864`
  (`0x40148`) and `+0x24` (min) is only kept when `< 51` while `+0x28` is
  forced to 51 when out of `1..51` — an asymmetry that only makes sense for a
  min/max pair.
- **[47](47-abi-13.5-frame-rc-surfaces.md) §3** records
  `CalcBufSizeOfCodedHeader = 0x23000` without explaining it. §3.1 here shows
  it is `0x180 + 256 × 0x220 + 0xE80`, i.e. a 256-slice record array.
- **[18](18-coded-data-sizing.md) §9** lists "whether the firmware enforces the
  buffer size" as undetermined. Partly answered: the firmware checks
  `CodedBufSize > 3·W·H/4` up front (fw `0x58358`–`0x58370`) and reports the
  produced length per slice in `CODED_DATA_HDR.ui32BytesWritten`, with a
  negative `bytesToRemove` as the error signal (§3.2). Whether it *truncates*
  on overflow is still **U**, though the firmware string
  `"bitstream size overflow, buffer size: %d, bitstream size: %d"`
  (file `0xc8ffa`) says it at least detects it. **C** (string), **U** (the
  behaviour).
- **[65](65-pframes.md) §4.1** gives `LowResRCResult` as
  `ALIGN(4W,256)·ceil(H/256)`. Re-read here as
  `AD(4·W+252,256) · ((H+255)>>8)` (`0xea592c`–`0xea5940`) — the same value
  for 16-aligned widths. Count 0 on our configuration either way.
- **[62](62-kext-field-map.md)** / `driver/ave_abi.h` `slice_num = 0xfdac`:
  the 13.5 firmware copies `0x104` bytes from there (fw `0x1440c`–`0x1441c`),
  so the whole `sSliceMap` is 260 bytes on the wire, not just `iNum`.
