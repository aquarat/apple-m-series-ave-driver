# Input pixel format — there is no selector, and that is the finding

*How the host tells the AVE firmware that the source surface is plain 8-bit
NV12. The short answer: **it does not, because there is nothing to tell it.**
The AVC path derives everything it needs about the input from two fields it
already has for a different reason — the SPS's `chroma_format_idc` and
`bit_depth_luma_minus8` — plus one per-frame boolean that chooses linear vs
AGX-compressed. There is no fourcc, no pixel-format descriptor, no index into
`gs_sAVE_PixelFormatConversion`, and no `input_format` word anywhere in the
wire protocol. NV12 is what you get when those fields say 4:2:0, 8-bit, not
compressed.*

Every row is marked **confirmed** (read out of an instruction, VA cited),
**inferred** (a chain over confirmed facts, stated as such) or **unknown**, per
[00-methodology.md](00-methodology.md).

Firmware VAs are **image virtual addresses** (`__TEXT` vmaddr 0, fileoff
`0x4000`). String addresses quoted from `strings -t x data/blobs/ave_h13c.bin`
are **file offsets** and are `VA + 0x4000`; both forms are given.

Reproduce any line with:

```sh
python3 tools/disas.py --fw   --addr 0x6b8b0 -n 0x2b90    # CAVCController::InitEncodingParameters
python3 tools/disas.py --fw   --addr 0xb52a8 -n 0x7034    # CAVCController::setPipe  (0x7034 bytes — use all of it)
python3 tools/disas.py --fw   --addr 0x87b98 -n 0xc90     # COFController::InitEncodingParameters (the contrast case)
python3 tools/disas.py --kext --addr 0xfffffe0008b724c0 -n 0x180   # the host-side input asserts
python3 tools/disas.py --kext --addr 0xfffffe0008cbe704 -n 0x70    # AVC_SPS::SetDefaultParams
```

This resolves the two open items flagged in
[37-start-avc-session.md](37-start-avc-session.md) §11 and three in
[32-picmgmt-params.md](32-picmgmt-params.md) §8. It **retracts** one inference
in docs/37 — see §7.

---

## 0. The result

Three axes describe the input surface, and none of them is a pixel-format
selector in the usual sense:

| axis | where it lives | who reads it | for 8-bit NV12 |
|---|---|---|---|
| **chroma layout** | `sCAveCmdAvcStart + 0x2940` — the SPS's own `chroma_format_idc` | `InitEncodingParameters` copies it into two controller fields; `setPipe` branches on it four ways and writes it into two hardware register fields | `1` |
| **bit depth** | `sCAveCmdAvcStart + 0x2948` — the SPS's own `bit_depth_luma_minus8` | `InitEncodingParameters` turns it into a bits-per-sample byte (8 or 10) that scales every plane-size computation | `0` |
| **linear vs compressed** | `AVE_PICMGMT_PARAMS + 0x174E` — `sRCUpdateData.bInputCompressed`, per frame | `setPipe` `csel`s the whole input descriptor between a `sLinear` and a `sCompressed` union member | `0` |

Everything else about the input — the two plane addresses, their sizes and
their **strides** — is per-frame data in `AVE_PICMGMT_PARAMS`, not a format
declaration. The plane *count* is not configurable: the AVC path fetches
exactly one luma plane and, unless `chroma_format_idc == 0`, exactly one
interleaved chroma plane. That is NV12's shape and nothing else is reachable.

The one thing that does look like a pixel-format enumerator is **computed by
the firmware, not supplied by the host** — see §3.

The host *does* have a fourcc-based pixel-format system
([14-frame-size-formulas.md](14-frame-size-formulas.md) §5), and NV12 is
`420v`/`420f` in it. It is used entirely on the kernel side to validate and
size the `IOSurface`. `AVE_Surface::CheckSupportedPixelFormat`
(`0xfffffe0008c69a8c`) is `AVE_PixelFmt_FindByType(fourcc) != nullptr ||
AVE_Surface::CheckInternalPixelFormat(fourcc)` — **confirmed**, the two `bl`s
at `0xfffffe0008c69aa0` (→ `0xfffffe0008c4c660`) and `0xfffffe0008c69aac`
(→ `0xfffffe0008c699dc`). `CheckInternalPixelFormat` accepts only a
sixteen-member family the 86-entry table does not contain — the four bases
`]8f0` `]8v0` `]xf0` `]xv0` (`0x5D386630`, `0x5D387630`, `0x5D786630`,
`0x5D787630`, materialised as negated addends at `0xfffffe0008c699f8`,
`0xfffffe0008c69a0c`, `0xfffffe0008c69a18`, `0xfffffe0008c69a2c`) each plus
`{0, 2, 4}` (`mov w9,#0x15` + `tst`, `0xfffffe0008c69a48`–`4c`), i.e. the
`0`/`2`/`4` suffix that is 4:2:0 / 4:2:2 / 4:4:4 in Apple's fourcc scheme.
**Confirmed.** Nothing derived from any of this is ever placed in a command.
The fourcc does not cross the wire.

---

## 1. What a driver must set for a linear 8-bit NV12 frame

Nothing here is new geometry; it is the complete, field-by-field answer.

### 1.1 Once, in `sCAveCmdAvcStart`

```c
0x291C u32  eProfile                = 6   /* High. Required so the SPS writer
                                             actually emits chroma_format_idc and
                                             bit_depth_*; with Main they are omitted
                                             and the decoder infers 4:2:0 8-bit —
                                             also correct, but then the two fields
                                             below are firmware-only.  docs/37 §3 */
0x2940 u32  chroma_format_idc       = 1   /* 4:2:0.  THE chroma-layout selector */
0x2944 u32  separate_colour_plane_flag = 0
0x2948 u32  bit_depth_luma_minus8   = 0   /* THE bit-depth selector: 0 -> 8 bits */
0x294C u32  bit_depth_chroma_minus8 = 0   /* written to the SPS; not read for layout */

0x0094 u32  feature word            = 0   /* bits 6..9 clear, so the firmware uses the
                                             per-frame strides instead of deriving
                                             them from the width.  §4.3 */
0x25A5 u8   (unnamed)               = 0   /* MUST be 0 or the firmware demands the
                                             MSB/LSB split recon planes.  §5 */
0x26E9 u8   (unnamed)               = 0   /* unknown; 0 is the safe value.  §8 */
0x26DC u32  (don't care)                  /* dead on the AVC path.  §7 */
```

### 1.2 Every frame, in `AVE_PICMGMT_PARAMS`

Offsets are into `AVE_PICMGMT_PARAMS`; add `0x12C0` for the offset inside
`sCAveCmdAvcProcess`. All of this falls inside the `0x4228 .. 0x5117` slice
that `ProcessCmd_Process_AVC` actually copies (docs/32 §0).

```c
0x174E u8   sRCUpdateData.bInputCompressed          = 0

/* sInput.uInput.sLinear.saCIBuf[AVE_BufIdx_Luma] */
0x4570 u64  .saIBuf[AVE_BufIdx_Data].iAddr     Y plane IOVA, != 0, % 64 == 0
0x4578 u32  .saIBuf[AVE_BufIdx_Data].iSize     != 0        (e.g. stride * height)
0x457C u32  .saIBuf[AVE_BufIdx_Data].iStride   != 0, % 64 == 0   (e.g. 1920)
0x4580 u64  .saIBuf[AVE_BufIdx_Header].iAddr   = 0   /* compressed-only */
0x4588 u32  .saIBuf[AVE_BufIdx_Header].iSize   = 0
0x458C u32  .saIBuf[AVE_BufIdx_Header].iStride = 0

/* sInput.uInput.sLinear.saCIBuf[AVE_BufIdx_Chroma] */
0x4590 u64  .saIBuf[AVE_BufIdx_Data].iAddr     UV plane IOVA, != 0, % 64 == 0
0x4598 u32  .saIBuf[AVE_BufIdx_Data].iSize
0x459C u32  .saIBuf[AVE_BufIdx_Data].iStride   % 64 == 0   (= luma stride for 4:2:0)
0x45A0 u64  .saIBuf[AVE_BufIdx_Header].iAddr   = 0
0x45A8 u32  ...                                = 0
0x45AC u32  ...                                = 0

0x45B0 .. 0x45EF   sInput.uInput.sCompressed overlay — leave zero
```

For 1920x1088 NV12: luma `iStride = 1920`, `iSize = 1920*1088`; chroma
`iStride = 1920`, `iSize = 1920*544`. (`AVE_Linear_CalcChromaPlaneSize`,
docs/14 §4.1, gives `chromaStride = (w/hdiv)*2*bps = w*bps` for both 4:2:0 and
4:2:2 — the two interleaved components cancel the horizontal decimation.)

The MSB/LSB pointers in `sRecon` (`0x4550`, `0x4560`) and `sRef`
(`0x4248`, `0x4288`, `0x43A8`, `0x43E8`) do **not** need to be non-zero when
`sCAveCmdAvcStart + 0x25A5 == 0`. §5.

---

## 2. The input descriptor is a union, and `bInputCompressed` picks the arm

The names are read out of two kext assert strings — this is the strongest
evidence in this document, because it names the struct path character for
character.

`AVE_CHM_SetDataInfo_FwBuf` (`0xfffffe0008b71fd8`; `x20 = AVE_PICMGMT_PARAMS*`
at `0xfffffe0008b71ffc`, `x27 = x20 + 0x4000` at `0xfffffe0008b72020`) has two
mutually exclusive validation paths.

**Linear** — string file `0x27f2bd` (VA `0xfffffe00072832bd`), referenced at
`0xfffffe0008b72584`–`88`:

```
pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iAddr   != 0 &&
pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iSize   != 0 &&
pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iStride != 0 &&
pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iStride % 64 == 0 &&
pLinear->saCIBuf[AVE_BufIdx_Chroma].saIBuf[AVE_BufIdx_Data].iStride % 64 == 0
```

The check that guards it binds every offset (`0xfffffe0008b724fc`–`0xfffffe0008b72524`):

```
b724fc:  ldr  x8, [x25]              ; surface pointer
b72504:  ldr  w8, [x27, #1400]       ; 0x4578  Luma  .iSize    -> cbz fail
b7250c:  ldr  w8, [x27, #1404]       ; 0x457C  Luma  .iStride  -> cbz fail
b72514:  and  w8, w8, #0x3f          ;                            cbnz fail
b7251c:  ldrb w8, [x27, #1436]       ; 0x459C  Chroma .iStride
b72520:  tst  w8, #0x3f              ;                            b.eq pass
```

and the log that follows loads the two addresses (`0xfffffe0008b72560`–`6c`):
`x10 = [x20, #17776]` = `0x4570`, `x11 = [x20, #17808]` = `0x4590`.
**Confirmed.**

**Compressed** — string file `0x27f1c7`, referenced at `0xfffffe0008b7260c`–`10`:

```
pCompressed->saCPBuf[AVE_BufIdx_Luma].sCIBuf.saIBuf[AVE_BufIdx_Data].iAddr != 0 &&
pCompressed->saCPBuf[AVE_BufIdx_Luma].sCPInfo.iHeaderStride != 0
```

with loads at `0xfffffe0008b725e8`–`f4`: `0x4570`, `[x27,#1444]` = `0x45A4`,
`[x20,#17840]` = `0x45B0`, `[x27,#1508]` = `0x45E4`. The log format is
`0x%llx %d 0x%llx %d`, i.e. `{addr, stride, addr, stride}`, so
`saCPBuf[]` has stride `0x40` with `sCIBuf` at `+0x00` and `sCPInfo` at
`+0x20`, and `iHeaderStride` at `sCPInfo + 0x14`:
`0x45A4 - 0x4590 = 0x45E4 - 0x45D0 = 0x14`. **Confirmed offsets; the
`sCPInfo+0x14` decomposition is inferred from that coincidence.**

The union member name is read directly from two further asserts
(`0x27f722`, `0x27f8c5`): `pInfo->sInput.uInput.sLinear.saCIBuf[...]`.
**Confirmed** that the field at `PICMGMT + 0x4570` is `sInput.uInput`.

### The firmware picks the same arm, from `bInputCompressed`

`CAVCController::setPipe` (`0xb52a8`) reads the flag first thing and keeps it
on the stack:

```
b52d4:  ldrb w22, [x1, #0x174e]      ; sRCUpdateData.bInputCompressed
b536c:  str  w22, [sp, #100]
```

and then `csel`s the chroma descriptor base on it, in three places:

```
b6ce8:  cmp  w16, #0x0               ; w16 = [sp,#100]
b6cec:  mov  w8,  #0x4590
b6cf0:  mov  w9,  #0x45b0
b6cf4:  csel x8,  x9, x8, ne         ; compressed -> 0x45B0, linear -> 0x4590
b6cf8:  ldr  x8,  [x27, x8]          ; x27 = AVE_PICMGMT_PARAMS*
```

same at `0xb7120`–`28` and `0xb712c`. **Confirmed.** This closes docs/32 §8's
"which of `0x4590` / `0x45B0` is used, and what `[sp+100]` in `setPipe` selects
on".

`AVE_BufIdx_Data` = 0 and `AVE_BufIdx_Header` = 1 — **inferred**, from the
adjacent entropy-coding assert pair (`Data` at file `0x27ebf7`, `Header` at
`0x27ecc2`) and from the `0x10`-byte leaf stride. This closes docs/32 §8's
"what `saIBuf[]` index 1 holds": it is the compressed-surface header plane,
which is why a linear NV12 frame leaves it zero.

### Leaf layout

`{u64 iAddr; u32 iSize; u32 iStride}` = `0x10` bytes; `saIBuf[2]` per
`saCIBuf[]` element = `0x20`; `saCIBuf[2]` = Luma at `0x4570`, Chroma at
`0x4590`. **Confirmed** by the three offsets `0x4570 / 0x4578 / 0x457C` all
being named by the assert string above, and the same triple at
`0x4590 / 0x4598 / 0x459C`.

---

## 3. The pixel-format enumerator exists — and the firmware computes it

`CAVCController::InitEncodingParameters` (`0x6b8b0`) builds a small integer
that goes straight into a hardware register field. It is derived from
`chroma_format_idc` and `bit_depth_luma_minus8` and from nothing else.

Register conventions in that function, all **confirmed**:

| reg | value | proof |
|---|---|---|
| `x20` | `this` (the `CAVCController`) | `0x6b8e4 mov x20, x0` |
| `x24` | the Start payload = `cmd + 0x68` | `0x6b8f4 ldr x24,[x1,#8]`; `0x6c078 ldr w8,[x24,#768]` is `cmd+0x368` = `VideoParams.ui32Width`, stored to `[x20,#3960]` at `0x6c084` |
| `x23` | `payload + 0x2000` | `0x6c000 add x23, x24, #0x2, lsl #12`; `0x6c224 ldrb w9,[x23,#1308]` = `cmd+0x2584` and `0x6c200 ldrb w9,[x23,#1667]` = `cmd+0x26EB`, both already named in docs/20 §3 |
| `x28` | `SPSparams - 4`, where `SPSparams` is the controller's copy at `this+0x247C4` | `0x6c014 add x28, x20+0x24000, #0x7c0`; `0x6c020 add x0, x28, #4` / `0x6c024 mov w2,#0x6b4` / `0x6c054 add x1, x24, #0x28b4` / `0x6c058 bl 0x581c` = `memcpy(this+0x247C4, cmd+0x291C, 0x6B4)` |
| `x25` | `this + 0x1BAD0` — the block the firmware's own strings call `EncCommParams` | `0x6b950 add x25, x20+0x1b000, #0xad0` |

So `[x28,#40]` = `SPSparams+36` = **`cmd+0x2940`, `chroma_format_idc`**, and
`[x28,#48]` = `SPSparams+44` = **`cmd+0x2948`, `bit_depth_luma_minus8`**.
Cross-checked three ways on neighbouring fields: `[x28,#1052]` feeds
`1 << (n+4)` = `MaxFrameNum` (`0x6c348`–`0x6c368`), `[x28,#1076]+1` is used as
the picture width in macroblocks (`0x6c384`–`0x6c3d0`), `[x28,#1080]+1` as the
height — exactly `log2_max_frame_num_minus4`, `pic_width_in_mbs_minus1`,
`pic_height_in_map_units_minus1` at docs/37's `0x2D34`, `0x2D4C`, `0x2D50`.

### 3.1 Bit depth → bits per sample

```
6c7a8:  ldr  w8, [x28, #48]        ; bit_depth_luma_minus8
6c7b0:  mov  w9, #0xa              ; 10
6c7b4:  cmp  w8, #0x0
6c7b8:  mov  w10, #0x8             ; 8
6c7bc:  csel w9, w10, w9, eq
6c7c0:  strb w9, [x22, #19]        ; -> this + 0x25DB   (x22 = x20 + 0x25C8, 0x6ba40/0x6baf8)
```

**Confirmed.** The field is *binary*: any non-zero `bit_depth_luma_minus8`
gives 10, not `8 + n`. That byte at `this + 0x25DB` then multiplies every
plane-size and row-bytes computation in the pipeline:

* `setPipe` `0xb5b84 ldrb w9,[x9,#7]` with `x9 = [sp,#208] = this + 0x25D4`
  (`0xb52e8`, `0xb5624`), then `0xb5b90 mul w8, w8, w9` — the luma row-bytes
  for the low-res source DMA; and again at `0xb5bec`/`0xb5bf8` for chroma.
* Argument 7 of the `H264VideoEncoderDPB` constructor: `0x6e130 ldrb w7,
  [x25,#19]` where `x25` was reloaded from `[sp,#72]` at `0x6ce48` and
  `[sp,#72]` holds `x22` (`0x6c7e4`) — the same `this + 0x25DB`. Inside the
  constructor, `0x1af78 mul w9, w22, w9` scales the DPB plane size by it.

**Confirmed.**

### 3.2 The enumerator

```
6c7dc:  ldr  w10, [x20, #3952]     ; EncCommParams.chroma_format (= chroma_format_idc)
6c7e8:  cmp  w10, #0x2
6c7ec:  b.eq 0x6c810               ;   4:2:2 -> base 18
6c7f0:  cmp  w10, #0x3
6c7f4:  b.ne 0x6c81c               ;   0 or 1 -> base 20
6c7fc:  mov  w8, #0x16             ;   4:4:4 -> base 22
6c810:  mov  w8, #0x12             ; 18
6c81c:  mov  w8, #0x14             ; 20
6c824:  cinc w8, w8, ne            ; +1 if bit_depth_luma_minus8 != 0
6c828:  str  w8, [x20, #3956]
```

**Confirmed.** So:

| chroma_format_idc | 8-bit | 10-bit |
|---:|---:|---:|
| 0 (mono) / 1 (4:2:0) | 20 | 21 |
| 2 (4:2:2) | 18 | 19 |
| 3 (4:4:4) | 22 | 23 |

`COFController::InitEncodingParameters` builds the same enumerator with the
same bases and adds a third code per group (`0x8810c`–`0x88140`: `18/19/25`
and `22/23/27`), which is why the values are almost certainly a shared
`_E_..._PixelFmt` enum rather than an ad-hoc encoding. **The extra codes are
confirmed; their meaning is unknown.**

The enumerator reaches the hardware in `setPipe`:

```
b7390:  ldr  w9, [x20, #3956]      ; the enumerator
b73a4:  bfi  w8, w9, #8, #8        ; -> register bits 8..15
b73b4:  str  w8, [ ... ]           ; two source-DMA config registers
b73c8:  str  w8, [ ... ]
```

**Confirmed.** *This is the pixel-format selector the search was looking for.
It is a firmware-internal value. The host never writes it and there is no
command field that carries it.*

---

## 4. `chroma_format_idc` is a memory-layout field, not just a bitstream field

This is the finding docs/37 asked to be flagged loudly if it turned up. It
turned up in four independent places, all in the AVC path, all **confirmed**.

### 4.1 It gates whether the chroma plane is fetched at all

In `setPipe`, `x19 = [sp,#232] = this + 0x1BAC0` (`0xb5354`, `0xb5658`,
`0xb68d0`) — the same `EncCommParams` block as `InitEncodingParameters`'s `x25
= this + 0x1BAD0`, minus `0x10`. So `[x19,#1844]` and `[x25,#1828]` are the
same word (`this+0x1C1F4`); likewise `[x19,#1491]` = `[x25,#1475]`, which is
the byte written at `0x6c268`. `x19` is reassigned only inside the two panic
blocks at `0xb6b94` and `0xb6c28`, both of which end in `b .-4`, so on the
live path it holds `EncCommParams`. **Confirmed.**

```
b6cdc:  ldr  w8, [x19, #1844]      ; = EncCommParams+1828 = input_chroma_format
b6ce4:  cbz  w8, 0xb6f88           ; monochrome -> skip the chroma plane entirely
b6ce8..b6cf8: load the chroma iAddr (0x4590 or 0x45B0)
b6cfc:  cbz  x8, <panic "cPicMgmtParams[Chroma]... != 0">
b6d00:  tst  x8, #0x3f  -> panic unless 64-byte aligned
```

and `EncCommParams+1828` is `chroma_format_idc` — see §7. Same gate at
`0xb6fe0` (chroma crop offsets) and `0xb7184` (a chroma stride register).
**Confirmed.** The luma plane at `0x4570` has no such gate
(`0xb6b74`–`0xb6b80`). So the plane count is exactly **two**, or **one** if
`chroma_format_idc == 0`.

### 4.2 It selects the chroma subsampling arithmetic

```
b6fe8:  ldr  w9, [x20, #3952]      ; chroma_format
b6fec:  cmp  w9, #0x3  -> b.eq 0xb7070   ; 4:4:4 : (x,     y)
b6ff4:  cmp  w9, #0x2  -> b.eq 0xb703c   ; 4:2:2 : (x>>1,  y)
b6ffc:  cmp  w9, #0x1  -> b.ne 0xb70a8   ; 4:2:0 : (x>>1,  y>>1)
                                          ; 0    : (0,     0)
```

(`0xb7030`/`0xb7034` for the 4:2:0 pair, `0xb7068` for 4:2:2, `0xb709c`/`0xb70a0`
for 4:4:4, `0xb70a8`/`0xb70ac` for mono.) A textbook chroma-decimation table.
The same computation appears in the low-res source path:

```
b5bc8:  ldr  w8, [x20, #3952]
b5bcc:  cbz  w8, 0xb5c14           ; mono -> no chroma DMA
b5bd4:  cmp  w8, #0x3
b5bd8:  cset w8, cc                ; 1 if chroma_format < 3
b5bdc:  lsr  w8, w9, w8            ; chroma width = width >> that
```

**Confirmed.**

### 4.3 It goes into two hardware register fields

```
b55e0:  ldr   w11, [x20, #3952]
b55f0:  ubfiz w9, w11, #10, #2     ; register bits 10..11 = chroma_format
b55f4:  cmp   w11, #0x0
b55f8:  mov   w11, #0x8400         ; bit 15 | (1 << 10)
b55fc:  csel  w9, w11, w9, eq      ; monochrome: set bit 15, report 4:2:0
b561c:  orr   w8, w8, w9
b5628:  str   w8, [x10, x9]        ; MMIO +0x1190000
```

and, in `InitEncodingParameters`, the identical idiom into a different register:

```
6d95c:  ldr   w10, [x28, #40]      ; chroma_format_idc
6d968:  and   w9,  w10, #0x3       ; bits 0..1
6d96c:  cmp   w10, #0x0
6d974:  mov   w11, #0x41           ; bit 6 | 1
6d978:  csel  w9,  w11, w9, eq     ; monochrome: bit 6, report 4:2:0
```

**Confirmed.** Two different registers, same encoding, same monochrome
special-case. This is not a bitstream field being incidentally reused; it is
the hardware's chroma-format input.

### 4.4 And it sizes the DPB

```
6e118:  ldr w1, [x20, #3960]       ; width
6e11c:  ldr w2, [x20, #3964]       ; height
6e120:  ldr w3, [x28, #40]         ; chroma_format_idc
6e134:  bl  0x1af04                ; H264VideoEncoderDPB::H264VideoEncoderDPB(
                                   ;   unsigned, unsigned, unsigned, unsigned long long,
                                   ;   bool, bool, unsigned)
```

**Confirmed** (symbol read from `data/derived/symbols.txt`).

**Practical consequence.** `AVC_SPS::seq_parameter_set_rbsp` emits
`chroma_format_idc` only for profile enums `{1,6,7,8,9}` (docs/37 §3). With
`eProfile = 4` (Main) the firmware still *uses* the value for the input
geometry while the decoder never sees it. Setting `chroma_format_idc = 2` with
a Main-profile SPS would silently produce a 4:2:2 fetch and a stream that
claims 4:2:0. Use `eProfile = 6`.

---

## 5. Bit depth, the MSB/LSB planes, and the 10-bit dead end

The `sRecon`/`sRef` `_MSB`/`_LSB` pairs are Apple's split for >8-bit content,
as docs/32 suspected. The gate is a **single session byte**, not a per-frame
one, and it is not the SPS bit depth.

```
6c2d4:  ldrb w8, [x23, #1341]      ; payload+0x253D = cmd + 0x25A5
6c2d8:  strb w8, [x25, #1536]      ; EncCommParams+1536
```

**Confirmed** (base `x23` proven in §3). In `setPipe` the same byte is
`[sp+232 base, #1552]`, and it gates every LSB requirement:

```
b75fc:  ldr  x8, [sp, #232]
b7600:  ldrb w8, [x8, #1552]
b7604:  cmp  w8, #0x1
b7608:  b.ne 0xb7970               ; not 1 -> skip the whole block
b760c:  ldr  x8, [x27, #17744]     ; sRecon.Y_LSB  (0x4550)
b7610:  cbz  x8, <panic "pPicParams->sRecon.Y_LSB != 0">
b7614:  tst  x8, #0x7f  -> panic unless 128-byte aligned
```

Same guard before the `sRef.Y_L0_LSB` asserts (`0xb619c`–`0xb61a4`, panic
string at `0xb6514`) and before the second low-res DMA argument
(`0xb6274`, `0xb5c0c`). **Confirmed.** With `cmd+0x25A5 = 0` a driver never
has to supply an LSB plane anywhere.

Incidentally this confirms docs/32's *inferred* `sRecon.Y_LSB` at `0x4550`:
the log at `0xb7580`–`0xb759c` loads `[x27,#17736]` and `[x27,#17744]` in that
order into `"AVC COMMON:: encoder_addr_dst_luma_msb:%016llx, lsb:%016llx"`
(file `0x131bdb`, VA `0x12dbdb`). `0x4548` = MSB, `0x4550` = LSB.
**Now confirmed, not inferred.**

### 10-bit is not supported, and the firmware only *says so in a log*

```
b69ec:  ldr  x8, [sp, #160]        ; = this + 0x247C4 = the SPSparams copy (0xb5560)
b69f0:  ldr  w8, [x8, #44]         ; SPSparams+44 = bit_depth_luma_minus8
b69f4:  cbz  w8, 0xb6a34           ; 8-bit -> nothing to say
b69f8:  ldr  w8, [x20, #3952]      ; chroma_format
b69fc:  cbz  w8, 0xb6a34
b6a18:  adrp x2, 0x12d000
b6a1c:  add  x2, x2, #0xa96        ; "10bit content is not supported"  (file 0x131a96)
b6a28:  bl   0xa948                ; log emit, level 4 (error)
b6a34:  ... execution continues
```

**Confirmed.** There is no `bl 0xe08fc` and no `b .-4` in that block, unlike
every real assert in the same function — it is a log line and the encode
proceeds with the hardware configured for 10-bit anyway. This is the same
failure shape docs/32 §1 warned about: accepted, then ignored, and it looks
like the hardware disobeying you.

`sp+160` is proven to be the SPS copy by `0xb5560 add x9, x20+0x24000, #0x7c4`
followed by `0xb5568 str x9,[sp,#160]` — the same `this+0x247C4` that
`InitEncodingParameters` memcpy's `cmd+0x291C` into.

**Answer to "is 10-bit or 4:2:2 supported by this AVC path":**

* **10-bit — no.** Explicitly refused (as a log), and the refusal is keyed on
  `bit_depth_luma_minus8 != 0 && chroma_format != 0`.
* **4:2:2 / 4:4:4 — structurally present, not validated here.** The chroma
  switch has real, distinct arms for 0/1/2/3 (§4.2); the enumerator has
  distinct codes (§3.2); the profile table has High 4:2:2 (`eProfile = 8`) and
  High 4:4:4 (`eProfile = 9`) (docs/37 §3). Nothing found rejects them. But
  nothing found exercises them either, and the only default anywhere is 1
  (`AVC_SPS::SetDefaultParams` `0xfffffe0008cbe73c`, `CAVCController::
  SetDefaultParameters` `0x5758c`). Treat 4:2:2 as **unknown**, not supported.

---

## 6. Strides: per-frame, and the firmware really does read them

docs/37 §11 says "stride / plane-layout fields: not found in the Start
command … there may be none to set". Correct about the Start command; wrong
about there being none. They are per-frame, and `setPipe` programs them into
the source-DMA registers verbatim:

```
b70c4:  ldr  x13, [sp, #88]        ; = this + 0x1CDC0
b70c8:  ldrh w9,  [x13, #1800]     ; the feature word from cmd + 0x94  (0x6c008)
b70cc:  tst  w9,  #0x3c0
b70d0:  b.eq 0xb7118               ; bits 6..9 clear -> use the host's strides
        ...
b7118:  cmp  w16, #0x0             ; bInputCompressed
b711c:  mov  w10, #0x4590
b7120:  mov  w11, #0x45b0
b7124:  ldr  x9,  [sp, #184]       ; = AVE_PICMGMT_PARAMS + 0x457C
b7128:  csel x10, x11, x10, ne
b712c:  add  x10, x27, x10
b7130:  ldr  w9,  [x9]             ; PICMGMT + 0x457C   luma  iStride
b7134:  ldr  w10, [x10, #12]       ; PICMGMT + 0x459C   chroma iStride
b7180:  str  w9,  [ ... ]          ; -> DMA register
b7198:  str  w10, [ ... ]          ; -> DMA register  (skipped if mono)
```

**Confirmed.** `sp+184` is `PICMGMT + 0x457C`: set at `0xb5524`
(`add x11, x21, x19` with `w19 = 0x457c` from `0xb53b8` and `x21` the
`AVE_PICMGMT_PARAMS*` argument) and independently cross-checked — `[sp+184,
#2516]` is `0x4F50` and `[sp+184, #2532]` is `0x4F60`, the two size fields
docs/32 §6.5 already names.

When the feature word's bits 6..9 are *set*, the firmware instead derives the
row bytes arithmetically from width and bits per sample
(`0xb70e0`–`0xb7168`) — that is the scaled-source path, not the normal one.
Keep `sCAveCmdAvcStart + 0x94 = 0`.

The plane addresses go to the DMA registers unconditionally, all four leaves:

```
b72e8:  ldr x10, [x27, #17776]     ; 0x4570  Luma   Data   iAddr
b72fc:  ldr x10, [x27, #17792]     ; 0x4580  Luma   Header iAddr
b7310:  ldr x10, [x9]              ; 0x4590  Chroma Data   iAddr
b7320:  ldr w8,  [x9, #16]         ; 0x45A0  Chroma Header iAddr
```

**Confirmed.** For a linear frame the two Header leaves should be zero; the
kext never writes them (a scan of every `ldr`/`str` with an immediate offset in
`AVE_CHM_SetDataInfo_FwBuf` returns `0x4570`, `0x4578`, `0x457C`, `0x4590`,
`0x4598`, `0x459C`, `0x45A4`, `0x45B0`, `0x45E4` and nothing at `0x4580` or
`0x45A0`). That the Header DMA is harmless when zero is **inferred**, not read.

---

## 7. `cmd + 0x26DC` resolved — and docs/37's inference retracted

**docs/37 §1 and §11 name `sCAveCmdAvcStart + 0x26DC` `input_chroma_format`
and mark it "INFERRED name". That row should be removed from the minimal
Start_AVC listing.** The field is real and the name is right *for
`COFController`*. On the AVC path it is stored and then unconditionally
overwritten, so its value has no effect whatsoever.

Both stores, in `CAVCController::InitEncodingParameters`:

```
6c240:  ldr w9,  [x24, #9844]      ; payload + 0x2674 = cmd + 0x26DC
6c24c:  str w9,  [x25, #1828]      ; EncCommParams.input_chroma_format
   ...
6c360:  ldr w10, [x28, #40]        ; SPSparams + 36 = cmd + 0x2940 = chroma_format_idc
6c374:  str w10, [x20, #3952]      ; EncCommParams.chroma_format
6c378:  str w10, [x25, #1828]      ; EncCommParams.input_chroma_format  <-- overwrite
```

A branch scan of every instruction in `0x6c24c .. 0x6c378` finds exactly three
control-transfer instructions, and all three target addresses inside that
range:

```
0x6c278  b.ne  -> 0x6c2ac
0x6c2a8  b     -> 0x6c2c0
0x6c340  tbz   -> 0x6c348
```

No `ret`, no `b` out of the range, no call. **Confirmed unconditional:** any
execution that reaches `0x6c24c` reaches `0x6c378`. `cmd + 0x26DC` is dead on
the AVC path.

The contrast case is worth recording because it is where the name comes from.
`COFController::InitEncodingParameters` reads the *same* payload offset and
then actually branches on it:

```
87d08:  ldr w8,  [x26, #9844]      ; the same cmd + 0x26DC
87d10:  str w8,  [x22, #1828]      ; input_chroma_format
87d14:  str w10, [x20, #3952]      ; chroma_format
87d18:  cmp w8,  #0x4
87d1c:  b.ne 0x87dbc
87d24:  str w10, [x22, #1828]      ;   == 4 means "match the coded format"
87d28:  b    0x87e58
   ...
87dbc:  cmp w10, w8
87dc0:  b.le 0x87e58
87dc4:  <panic>  "EncCommParams.chroma_format <= EncCommParams.input_chroma_format"
                 (file 0x12ef22, VA 0x12af22, referenced 0x87dd4-d8)
```

**Confirmed.** So `input_chroma_format` is a real `_E_ChromaFmt`-valued field
with a `4` = "same as coded" sentinel — in `COFController`. `CAVCController`
has no such compare and no such assert; it simply clobbers the slot with
`chroma_format_idc`. Which is why §4.1's chroma-plane gate on
`EncCommParams+1828` is, on the AVC path, a gate on `chroma_format_idc`.

---

## 8. Corrections and closures

* **docs/37 §1** — delete the `0x26DC u32 input_chroma_format = 1` row. §7.
* **docs/37 §11** — "`cmd + 0x26DC` as `input_chroma_format`" is resolved:
  the name is right for `COFController` and the field is dead for AVC.
  The doubt it records ("the AVC path overwrites `x25 + 1828` at `0x6c378`")
  was the correct instinct; the overwrite is unconditional.
* **docs/37 §11** — "**the NV12 pixel-format selector is not located**" is
  resolved as *there is no such selector*. §0, §3. The candidates docs/37
  proposed (the `0x68`–`0x207` head, the bulk of `VideoParams`) can be
  dropped.
* **docs/37 §11** — "stride / plane-layout fields: not found in the Start
  command … there may be none to set". Correct for the Start command; the
  strides exist per-frame at `PICMGMT + 0x457C` / `+0x459C` and the firmware
  reads them. §6.
* **docs/32 §8** — "which of `0x4590` / `0x45B0` is used, and what `[sp+100]`
  in `setPipe` selects on" — `[sp+100]` is `sRCUpdateData.bInputCompressed`
  (`PICMGMT + 0x174E`); linear takes `0x4590`, compressed `0x45B0`. §2.
* **docs/32 §8** — "whether `AVE_BufIdx` has more than `{Luma, Chroma}`, and
  what `saIBuf[]` index 1 holds" — for the input descriptor there are exactly
  two `saCIBuf[]` entries (`Luma`, `Chroma`), and `saIBuf[1]` is
  `AVE_BufIdx_Header`, the compressed-surface header plane. §2.
* **docs/32 §1** — `sRecon.Y_LSB` at `0x4550` was "inferred (by elimination)".
  It is now read directly from the `msb/lsb` log at `0xb7580`–`0xb759c`. §5.
* **docs/32 §1** — the input rows should gain `0x457C` and `0x459C`
  (`iStride`, non-zero, `% 64 == 0`); the kext refuses the frame without them.

### Still unknown

* `sCAveCmdAvcStart + 0x25A5` — proven to gate the MSB/LSB planes, but no
  name was recovered. `AVE_Client_Enc_PrintAVC` prints `input_compress` and
  `UseHWTileOffsets` from client-struct offsets 36 and 687
  (`0xfffffe0008b7dcc8`/`cd0`), which are *not* adjacent, so docs/37's
  suggestion that `cmd+0x25A4/0x25A5` are that pair is unsupported. The value
  a driver needs (`0`) is certain; the name is not.
* `sCAveCmdAvcStart + 0x26E9` — a byte, copied to `EncCommParams+1839` and
  then decremented if it equals 2 (8-bit) or 3 (10-bit) into
  `EncCommParams+1840` (`0x6c7c4`–`0x6c7e0`). Sits next to the format code but
  is not part of it. Unknown.
* `sCAveCmdAvcStart + 0x26E2` (u16) → `EncCommParams+1820`, masked to two bits
  and written to a register at `0xb6cc8`–`0xb6cd8`. Unknown.
* The extra `COFController` format codes 25 and 27 (§3.2). Probably 16-bit or
  packed variants; not reachable from the AVC path.
* Whether the four-arm chroma switch (§4.2) has ever been exercised for
  `chroma_format_idc` other than 1. Nothing rejects 2 or 3; nothing confirms
  them.
* Which host function fills `sInput.uInput` in `AVE_PICMGMT_PARAMS`. It is not
  `AVE_CHM_SetDataInfo_FwBuf` (which only reads and patches it, for the
  still-image crop offset at `0xfffffe0008b72c28`–`50`). This is a limitation
  of an immediate-offset scan (docs/00 trap 3), not a finding — a driver
  writes these fields itself in any case.

---

## 9. Proposed hardware experiment (do not run — for the operator)

Nothing here needs hardware. If a first-light frame comes back green or
chroma-swapped, the discriminating check is cheap and needs no register
access:

1. Submit the same frame twice with `sCAveCmdAvcStart + 0x2940` set to `1`
   and then to `0`. Under the reading above, `0` must produce a monochrome
   result and must not touch `PICMGMT + 0x4590` at all — so zeroing the UV
   pointer in the `0` run should still succeed. If it faults, §4.1 is wrong.
2. Halve `PICMGMT + 0x457C` (with a correspondingly narrow surface). If the
   output shears, the firmware is using the host stride (§6); if it does not,
   `sCAveCmdAvcStart + 0x94` is not what §6 says it is.

Both are ordinary command submissions.


---

## Verification pass (independent re-derivation)

| Claim | Evidence | Verdict |
|---|---|---|
| The firmware *computes* the pixel-format enumerator | `0x6c7dc: ldr w10,[x20,#3952]`; `cmp #2 -> 18`, `cmp #3 -> 22`, else `20` (`0x6c814`, `0x6c7fc`, `0x6c820`); `0x6c824: cinc w8,w8,ne` adds 1 for non-zero bit depth; `0x6c828: str w8,[x20,#3956]` | confirmed |
| Its input is `SPS.chroma_format_idc` | `0x6c360: ldr w10,[x28,#40]` — `x28` is the SPS copy destination and the block sits at `x28+4`, so this is SPS `+0x24` = **`Start + 0x2940`** — then `0x6c374: str w10,[x20,#3952]`, the exact word read above | confirmed |
| `cmd+0x26DC` is overwritten and dead for AVC | `0x6c378: str w10,[x25,#1828]` writes the same `SPS.chroma_format_idc` over the earlier store | confirmed |

The two halves meet: the word the format computation reads (`+3952`) is
written from the SPS block, so the chain from wire field to DMA register is
closed without a host-supplied format selector anywhere in it.

**Retraction stands.** [37](37-start-avc-session.md) §1's
`0x26DC input_chroma_format` row is withdrawn — the field is real and
correctly named in `COFController`, but dead on the AVC path.

## The hazard worth flagging

10-bit input is **not rejected**. `0xb69ec`–`0xb6a28` logs
`"10bit content is not supported"` and then proceeds mis-configured: no panic,
no error return. Everywhere else this firmware has been loud about bad input
(wrong command size, `header_len != 0`, mismatched `bFWCreatesHeader`, a
`FrameRate` of 0 is an outright panic), so it is worth not generalising from
that: this one path fails quietly. The driver should refuse anything but
8-bit itself rather than rely on the firmware to complain.
