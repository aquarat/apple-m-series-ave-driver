> ## Verification note
>
> Independently re-checked against the binaries before committing:
>
> - **`_E_ChromaFmt` names confirmed.** The pointer array at
>   `0xfffffe000c69a850` (file `0x5696850`) decodes exactly as reported:
>   `400, 420, 422, 444` then `Linear, Packed, HTPC, Interchange` then
>   `VideoRange, FullRange` then `Lossless, Lossy75, Lossy62, Lossy50`.
> - **420v arithmetic confirmed.** 1920x1080: `1920*1080 + 2*((1920*1080/2)/2)`
>   = `2073600 + 1036800` = **3110400** (`0x2F7600`), CbCr plane offset
>   `0x1FA400` = `2073600` = the luma size. Self-consistent.
> - **`GetLinearBuf` partially confirmed.** Read directly: the plane index is
>   bounded at `<= 3` (`cmp w1, #0x3` at `0xfffffe0008c6ea0c`), a per-plane bit
>   is tested in a mask at object `+240` (`0xfffffe0008c6ea18`), a 64-byte output
>   struct is zeroed, and an object is loaded from `+104` and called
>   (`0xfffffe0008c6ea34`-`38`). That is consistent with geometry being fetched
>   from an external surface object rather than computed.
>
>   **Not confirmed:** the specific `getPlaneOffset` / `getPlaneSize` /
>   `getPlaneBytesPerRow` accessor names. `GetLinearBuf` makes exactly *one*
>   non-logging call, not three. The named accessors should be treated as
>   inferred until someone resolves that call target. See
>   [15-surface-layout.md](15-surface-layout.md), which covers this object
>   directly.

# Frame size, stride and alignment

Everything below was read out of `AppleAVE2.kext` in `data/blobs/kc.macho`
(macOS 26.6.2, `kernelcache.release.mac13j`) with `tools/disas.py`, plus direct
reads of `__PRELINK_TEXT` / `__DATA` at the addresses cited. Every constant
carries the VA of the instruction or the data address it came from. Anything
not read directly out of the image is marked **inferred** or **unknown**.

This closes the "surface size formulas" item in `12-dart-surfaces-mmio.md` §6
for the four `Calc*FrameSize` primitives, and identifies the previously unknown
`_S_AVE_PixelFmt` field `+12`. It does **not** close `AVE_Work_Enc_CalcSurfaceInfo`
(the internal `Recon`/`Colocated`/`MCTFRef` sizes), which is a separate family
(`AVE_CalcBufSizeOf*`) and is still undone.

Headline result, stated up front because it is the surprising part:

> **None of the four frame-size primitives applies any stride, plane or
> total-size alignment. They compute `width * height * bytesPerSample` with a
> completely unpadded stride.** For imported (client) frames the kext does not
> compute geometry at all — it reads plane offset, plane size and bytes-per-row
> straight out of the `IOSurface` (§6).

---

## 1. `_E_ChromaFmt` — confirmed by name

`AVE_Analytics_Print` (`0xfffffe0008b57270`) loads `_S_AVE_PixelFmt` field
`+12` and uses it as an index into an array of `const char *` at
**`0xfffffe000c69a850`** (`ldrsw x8,[x23,#12]` at `0xfffffe0008b572c0`,
`adrp`/`add` to `0xfffffe000c69a850` at `0xfffffe0008b572c8`–`0xb572cc`,
`ldr x8,[x10]` at `0xfffffe0008b572e4`). `x23` is the pointer returned by
`AVE_PixelFmt_FindByType` at `0xfffffe0008b57274`.

Reading that array (chained-fixup low 32 bits = file offset, per
`00-methodology.md` trap 5):

| value | string | meaning |
|---|---|---|
| 0 | `400` | monochrome / no chroma |
| 1 | `420` | 4:2:0 |
| 2 | `422` | 4:2:2 |
| 3 | `444` | 4:4:4 |

`_E_ChromaFmt` carries **no** bit-depth information — depth is a separate
argument everywhere (`bitsPerComponent`, 8/10/16).

The same `__DATA` block continues with three more enum name arrays. All were
read directly; the binding of each array to a struct field is noted per row.

| VA | entries | binds to |
|---|---|---|
| `0xfffffe000c69a850` | `400` `420` `422` `444` | `_S_AVE_PixelFmt+12`, **confirmed** (indexed at `0xfffffe0008b572c0`) |
| `0xfffffe000c69a870` | `Linear` `Packed` `HTPC` `Interchange` | `_S_AVE_PixelFmt+24` "layout kind", *inferred* (see §2 — the dispatcher's four cases tail-call functions with exactly these four names, in this order) |
| `0xfffffe000c69a890` | `VideoRange` `FullRange` | `_S_AVE_PixelFmt+28`, *inferred* (matches the `v`/`f` fourcc split) |
| `0xfffffe000c69a8a8` | `Lossless` `Lossy75` `Lossy62` `Lossy50` | `_E_AVE_LossyLevel` = `_S_AVE_PixelFmt+32`, *inferred but cross-checked*: the luma bytes-per-tile table at `0xfffffe00072670f0` reads `0x400 0x300 0x280 0x200` = 100% / 75% / 62.5% / 50% of the uncompressed 32x32 8-bit tile |

(Further arrays in the same block, not used below: `ENCODE LRME MCTF GGM DMV`
at `0xfffffe000c69a8d0`; `AVC HEVC AV1` at `0xfffffe000c69a900`; `264 265 AV1`
at `0xfffffe000c69a928`; `FRAME TILE` at `0xfffffe000c69a950`.)

### The chroma divisor table

`0xfffffe0007276400` (file offset `0x272400`), stride 8, `{u32 hdiv, u32 vdiv}`,
indexed by `_E_ChromaFmt`. Read directly:

| `_E_ChromaFmt` | hdiv | vdiv |
|---|---|---|
| 0 (`400`) | 1 | 1 |
| 1 (`420`) | 2 | 2 |
| 2 (`422`) | 2 | 1 |
| 3 (`444`) | 1 | 1 |

Every user of this table, from an `adrp`+`add` scan of all `__TEXT*` segments
for the address `0xfffffe0007276400`: `AVE_HTPC_CalcChromaSize+0xc`
(`0xfffffe0008b3fd50`), `AVE_HTPC_CalcFrameSize+0x68` (`0xfffffe0008b3fe44`),
`AVE_Linear_CalcChromaPlaneSize+0x20` (`0xfffffe0008b8cf58`),
`AVE_Linear_CalcChromaSize+0x24` (`0xfffffe0008b8cfb4`),
`AVE_Linear_CalcFrameSize+0x28` (`0xfffffe0008b8d014`),
`AVE_Interchange_CalcChromaSize+0xc` (`0xfffffe0008c2a7d0`),
`AVE_Packed_CalcChromaSize+0x24` (`0xfffffe0008c54204`),
`AVE_Packed_CalcFrameSize+0x3c` (`0xfffffe0008c5428c`).

Note the row-0 quirk: `400` divisors are `(1,1)`, not "no chroma". `Linear` and
`Packed` guard on `chromaFmt == 0` explicitly and skip chroma
(`cbz w3` at `0xfffffe0008b8d008` and `0xfffffe0008c54280`). **`HTPC` and
`Interchange` do not guard**, so calling them with `_E_ChromaFmt = 0` produces a
frame size including a full 4:4:4-sized chroma region. That is reachable in
practice: `&L08` (row 80 of the master table) is layout kind 3 with chroma
format 0. Read directly from the code; not a transcription error.

---

## 2. `_S_AVE_PixelFmt` layout

Recovered from `AVE_PixelFmt_CalcFrameSize` (`0xfffffe0008c4c6a0`),
`AVE_PixelFmt_FindType` (`0xfffffe0008c4c7b8`) and `AVE_PixelFmt_FindType_Ext`
(`0xfffffe0008c4c85c`). Total size 44 bytes (`add x8,x8,#0x2c` at
`0xfffffe0008c4c840`).

| off | type | meaning | evidence |
|---|---|---|---|
| `+0` | u32 | fourcc | returned by `FindType` at `0xfffffe0008c4c854` (`ldur w0,[x8,#-16]`); compared by `FindByType` at `0xfffffe0008c4c678` |
| `+4` | u32 | bits per component (8, 10, 16) | `ldr w9,[x0,#4]` `0xfffffe0008c4c764`, then `(bits+7)/8`; compared at `0xfffffe0008c4c800` |
| `+8` | u32 | plane count (1 or 2) | compared at `0xfffffe0008c4c810`; never used arithmetically |
| `+12` | `_E_ChromaFmt` | 0/1/2/3 = 400/420/422/444 | §1; passed as arg 5 to the tiled primitives (`ldr w4,[x0,#12]` at `0xfffffe0008c4c6ac`) |
| `+16` | u32 | chroma horizontal divisor | `ldp w11,w12,[x0,#16]` `0xfffffe0008c4c6f8`; `sdiv` `0xfffffe0008c4c6fc` |
| `+20` | u32 | chroma vertical divisor | same `ldp`; `sdiv` `0xfffffe0008c4c724` |
| `+24` | u32 | **layout kind** 0..3 = Linear/Packed/HTPC/Interchange | switch at `0xfffffe0008c4c6b4`–`0xc4c744` |
| `+28` | u32 | 0 = video range, 1 = full range | compared only by `FindType_Ext` at `0xfffffe0008c4c8e4` |
| `+32` | `_E_AVE_LossyLevel` | 0..3, `-1` if n/a | `ldr w8,[x0,#32]` `0xfffffe0008c4c748`, passed as arg 3 to Interchange; `cmn w9,#1` at `0xfffffe0008c4c7cc` |
| `+36` | u32 | **unknown** | see below |
| `+40` | u32 | **unknown** | see below |

### `+36` / `+40` remain unidentified

**Nothing in the entire kernelcache reads them.** Method: an `adrp`+`add` scan
of every `__TEXT*` segment for `0xfffffe0007268620` / `+0x630` finds exactly
three references — `AVE_PixelFmt_FindByType+0xc` (`0xfffffe0008c4c66c`),
`AVE_PixelFmt_FindType+0xc` (`0xfffffe0008c4c7c4`) and
`AVE_PixelFmt_FindType_Ext+0xc` (`0xfffffe0008c4c868`). (`GetNumOfTypes`,
`0xfffffe0008c4c694`, just returns the literal 86.) Only `_FindByType` hands the
entry pointer out to a caller, and it
has exactly four call sites (branch scan of every `__TEXT*` segment):
`AVE_Analytics_Print+0x64`, `AVE_Analytics_SendEvent+0x63c`,
`AVE_Surface::CheckSupportedPixelFormat+0x14`,
`AVE_Surface::GetCompressedBuf+0x1f8`. Disassembling all four shows reads of
`+4`, `+12`, `+24` and `+32` only; the sole `#36]`/`#40]` operands in those
ranges are *stores* into the caller's output struct
(`str w0,[x20,#36]` at `0xfffffe0008c6ef4c`), not reads of the format entry.

Discrimination check (per `00-methodology.md` trap 2): the same scan **does**
find the reads of `+12` and `+24` in those functions, and **does** find the
call sites of `AVE_Surface::GetYUVStride`, so it is capable of returning a
positive. The negative is therefore meaningful *for this binary*.

Observed values, in case they are recognised later — they are a function of
(chroma, depth) only:

| format class | `+36` | `+40` |
|---|---|---|
| 4:2:0 8-bit | 2 | 1 |
| 4:2:0 10-bit | 7 | 2 |
| 4:2:2 8-bit and 10-bit | 8 | 8 |
| 4:4:4 8-bit | 9 | 9 |
| 4:4:4 10-bit | 9 | 10 |
| mono 8-bit and 16-bit | 6 | 6 |
| mono 10-bit | 7 | 7 |

---

## 3. The dispatcher

`AVE_PixelFmt_CalcFrameSize(const _S_AVE_PixelFmt *fmt, int width, int height)`
at **`0xfffffe0008c4c6a0`**. Argument identification: in the kind-1 path
`width` is the operand divided by 3 (`add w8,w1,#2` at `0xfffffe0008c4c6d4`,
signed divide-by-3 magic `0x55555556` at `0xfffffe0008c4c6d8`) and `height` is
the multiplier (`mul w8,w2,w8` at `0xfffffe0008c4c6f0`) — which is the byte
layout of a 10-bit packed row, so arg2 = width, arg3 = height.

```c
if (!fmt) return 0;                                  // 0xfffffe0008c4c6a4
cf   = fmt->chromaFmt;   // +12                      // 0xfffffe0008c4c6ac
kind = fmt->layoutKind;  // +24                      // 0xfffffe0008c4c6b0

switch (kind) {
case 0: /* Linear    — inlined, 0xfffffe0008c4c760 */
    bps  = (fmt->bits + 7) / 8;                      // 0xfffffe0008c4c768-c4c774
    luma = width * height * bps;                     // 0xfffffe0008c4c760, c4c778
    chroma = (cf == 0) ? 0                           // csel 0xfffffe0008c4c784
           : (2 * luma) / (fmt->hdiv * fmt->vdiv);   // 0xfffffe0008c4c78c-c4c790
    return luma + chroma;                            // 0xfffffe0008c4c794

case 1: /* Packed    — inlined, 0xfffffe0008c4c6c8 */
    if (fmt->bits != 10) return 0;                   // 0xfffffe0008c4c6cc-c4c6d0
    luma = 4 * height * ((width + 2) / 3);           // 0xfffffe0008c4c6d4-c4c728
    chroma = (cf == 0) ? 0                           // csel 0xfffffe0008c4c71c
           : (4 * (((2*width)/fmt->hdiv + 2)/3) * height) / fmt->vdiv;
    return luma + chroma;

case 2: /* HTPC */
    return AVE_HTPC_CalcFrameSize(width, height, fmt->bits, 0, cf);
                                                     // tail-call 0xfffffe0008c4c7b4
case 3: /* Interchange */
    return AVE_Interchange_CalcFrameSize(width, height,
                                         fmt->lossyLevel, fmt->bits, cf);
                                                     // tail-call 0xfffffe0008c4c75c
}
```

Note the kind-4 argument `0` handed to HTPC (`mov w3,#0` at
`0xfffffe0008c4c7b0`) — the HTPC "header grouping shift" is always 0 through
this path.

The two inlined cases are arithmetically the same as the standalone `Linear`
and `Packed` functions except for divisor order: the dispatcher computes
`(2*luma)/(hdiv*vdiv)` while `AVE_Linear_CalcFrameSize` computes
`2*((luma/hdiv)/vdiv)`. Identical when the division is exact; they can differ
by a byte or two on odd dimensions.

**`AVE_PixelFmt_CalcFrameSize` has no call site anywhere in the kernelcache**
(branch and `adrp`+`add` scan of every `__TEXT*` segment). Same for
`AVE_Packed_CalcFrameSize`, `AVE_Enc_AlignDimension`,
`AVE_HTPC_CalcLumaHdrStride` and `AVE_HTPC_CalcChromaHdrStride`. The only one of
the five primitives with a live caller is `AVE_Linear_CalcFrameSize`, called
four times from `AVE_CalcBufSizeOfCodedData` (§7). Treat these as *the
authoritative statement of the layouts*, not as the code that runs per frame.

---

## 4. The four primitives

### 4.1 `AVE_Linear_CalcFrameSize(int w, int h, int bits, _E_ChromaFmt cf)`

`0xfffffe0008b8cfec`. Argument order confirmed by the call site at
`0xfffffe0008b5f620`, which passes the width-aligned value in `w0` and the
height in `w1` (§7).

```c
bps  = (bits + 7) / 8;                 // 0xfffffe0008b8cff4-b8d000
luma = w * h * bps;                    // 0xfffffe0008b8cff0, b8d004
if (cf == 0) return luma;              // 0xfffffe0008b8d008
(hdiv, vdiv) = chromaDiv[cf];          // 0xfffffe0008b8d010, b8d02c
chroma = 2 * ((luma / hdiv) / vdiv);   // 0xfffffe0008b8d030-b8d038
return luma + chroma;                  // 0xfffffe0008b8d044
```

`AVE_Linear_CalcChromaPlaneSize(w, h, bits, cf)` (`0xfffffe0008b8cf38`) is
`(w*h*bps)/hdiv/vdiv` (`sdiv` pair at `0xfffffe0008b8cf7c`/`b8cf80`) — i.e. **one**
chroma component. The `* 2` in `CalcFrameSize` is Cb + Cr, so for a biplanar
NV12-style format the single interleaved CbCr plane is that doubled value.

Implied geometry (**inferred**, since the function never forms a stride):

```
lumaStride   = w * bps           (no padding)
lumaSize     = lumaStride * h
chromaStride = (w / hdiv) * 2 * bps = w * bps  for 4:2:0 and 4:2:2
chromaSize   = 2 * lumaSize / (hdiv * vdiv)
```

### 4.2 `AVE_Packed_CalcFrameSize(int w, int h, int bits, _E_ChromaFmt cf)`

`0xfffffe0008c54250`. The `p420`/`pf20`/`p422`/… family: 3 ten-bit samples per
32-bit word.

```c
if (bits != 10) return 0;                       // 0xfffffe0008c54254
lumaRowBytes = 4 * ((w + 2) / 3);               // 0xfffffe0008c5425c-c54278
luma         = h * lumaRowBytes;                // 0xfffffe0008c5427c
if (cf == 0) return luma;                       // 0xfffffe0008c54280
(hdiv, vdiv) = chromaDiv[cf];                   // 0xfffffe0008c54288, c542a8
chromaRowBytes = 4 * ((((2*w) / hdiv) + 2) / 3);// 0xfffffe0008c542a4-c542c4
chroma = (h * chromaRowBytes) / vdiv;           // 0xfffffe0008c542c4-c542c8
return luma + chroma;                           // 0xfffffe0008c542d8
```

`(x + 2) / 3` is the compiler's signed divide-by-3 (`smull` with `0x55555556`,
`0xfffffe0008c54260`–`c54274`), i.e. `ceil(w/3)` for positive `w`.
`2*w/hdiv` is the count of interleaved chroma samples per row.

### 4.3 `AVE_Interchange_CalcFrameSize(int w, int h, _E_AVE_LossyLevel L, int bits, _E_ChromaFmt cf)`

`0xfffffe0008c2a894` = `CalcLumaSize(w,h,L,bits)` (`bl 0xfffffe0008c2a8c0` →
`0xfffffe0008c2a658`) `+ CalcChromaSize(w,h,L,bits,cf)`
(`bl 0xfffffe0008c2a8dc` → `0xfffffe0008c2a7c4`), summed at
`0xfffffe0008c2a8e0`.

```c
/* AVE_Interchange_CalcLumaSize, 0xfffffe0008c2a658 */
bpt = (bits == 8 ? lumaTile8[L] : lumaTileX[L]);  // csel 0xfffffe0008c2a670
tX  = (w + 31) / 32;                              // 0xfffffe0008c2a690-c2a69c
tY  = (h + 31) / 32;                              // 0xfffffe0008c2a6a0-c2a6ac
eX  = (w < 33) ? 0 : ceil_log2(tX);               // clz 0xfffffe0008c2a6b8, cmp c2a6c4
eY  = (h < 33) ? 0 : ceil_log2(tY);               // clz 0xfffffe0008c2a6d0, cmp c2a6d8
meta = align_up(32 << (eX + eY), 128);            // 0xfffffe0008c2a6e4-c2a6ec
return tX * tY * bpt + meta;                      // 0xfffffe0008c2a6f0

/* AVE_Interchange_CalcChromaSize, 0xfffffe0008c2a7c4 */
(hdiv, vdiv) = chromaDiv[cf];                     // 0xfffffe0008c2a7cc, c2a800
cw = w / hdiv;  ch = h / vdiv;                    // 0xfffffe0008c2a81c/c2a820
cbpt = (bits == 8 ? chrTile8[L] : chrTileX[L]);   // csel 0xfffffe0008c2a7fc
ctX = (cw + 15) / 16;                             // 0xfffffe0008c2a828-c2a834
ctY = (ch + 15) / 16;                             // 0xfffffe0008c2a838-c2a844
eX  = (cw < 17) ? 0 : ceil_log2(ctX);             // clz 0xfffffe0008c2a850, cmp c2a85c
eY  = (ch < 17) ? 0 : ceil_log2(ctY);             // clz 0xfffffe0008c2a868, cmp c2a870
meta = align_up(8 << (eX + eY), 128);             // 0xfffffe0008c2a880-c2a888
return ctX * ctY * cbpt + meta;                   // 0xfffffe0008c2a88c
```

`ceil_log2(n)` is literally `32 - clz(n - 1)`, i.e. the metadata region covers
both tile counts rounded up to powers of two. Bytes-per-tile tables (unchanged
from `12-dart-surfaces-mmio.md` §1.4, re-read here):

| table | VA | `Lossless` | `Lossy75` | `Lossy62` | `Lossy50` |
|---|---|---|---|---|---|
| luma, 8-bit | `0xfffffe00072670f0` | `0x400` | `0x300` | `0x280` | `0x200` |
| luma, other | `0xfffffe0007267100` | `0x500` | `0x400` | `0x300` | `0x280` |
| chroma, 8-bit | `0xfffffe0007267110` | `0x200` | `0x180` | `0x100` | `0x100` |
| chroma, other | `0xfffffe0007267120` | `0x280` | `0x200` | `0x180` | `0x180` |

Cross-check: luma tile 32x32 = 1024 samples, and `Lossless` 8-bit = `0x400` =
1024 bytes. Chroma tile 16x16 with two interleaved components = 512 samples, and
`Lossless` 8-bit = `0x200` = 512 bytes. Both exact, which confirms the tile
dimensions and that the chroma tile is interleaved CbCr.

### 4.4 `AVE_HTPC_CalcFrameSize(int w, int h, int bits, int k, _E_ChromaFmt cf)`

`0xfffffe0008b3fddc`. `k` is a header-grouping shift; the dispatcher always
passes 0. Uncompressed 16x8 (luma) / 8x8-pair (chroma) tiling plus a 4-byte
per-tile-column header.

```c
bps = (bits + 7) / 8;                          // 0xfffffe0008b3fde0-b3fdec
tX  = (w + 15) / 16;                           // 0xfffffe0008b3fdf0-b3fdfc
tY  = (h +  7) /  8;                           // 0xfffffe0008b3fe00-b3fe0c
lumaTiles = tX * tY;                           // 0xfffffe0008b3fe10
lumaHdrStride = align_up((tX << k) * 4, 128);  // 0xfffffe0008b3fe14-b3fe20
lumaHdrRows   = ((1 << k) + tY - 1) >> k;      // 0xfffffe0008b3fe24-b3fe34
lumaHdr       = lumaHdrStride * lumaHdrRows;   // 0xfffffe0008b3fe38

(hdiv, vdiv) = chromaDiv[cf];                  // 0xfffffe0008b3fe40, b3fe5c
cw = w / hdiv;  ch = h / vdiv;                 // 0xfffffe0008b3fe60/b3fe64
ctX = (cw + 7) / 8;                            // 0xfffffe0008b3fe68-b3fe74
ctY = (ch + 7) / 8;                            // 0xfffffe0008b3fe78-b3fe84
chrHdrStride = align_up((ctX << k) * 4, 128);  // 0xfffffe0008b3fe88-b3fe94
chrHdrRows   = (((1 << k) - 1) + ctY) >> k;    // 0xfffffe0008b3fe98-b3fea8
totalTiles = ctX * ctY + lumaTiles;            // 0xfffffe0008b3feac
return (chrHdrRows * chrHdrStride + lumaHdr)   // 0xfffffe0008b3feb4
     + totalTiles * bps * 128;                 // 0xfffffe0008b3feb8
```

`align_up(..., 128)` is `add #0x7f` / `and #0xffffff80`. The luma-header
expression is independently confirmed by the standalone
`AVE_HTPC_CalcLumaHdrStride(int w, int k)` at `0xfffffe0008b3fb38`, which is
byte-for-byte the same sequence.

Both the luma tile (16x8 = 128 samples) and the chroma tile (8x8 pixels x 2
interleaved components = 128 samples) are `128 * bps` bytes, which is the
`lsl #7` at `0xfffffe0008b3feb8`.

---

## 5. The master table `gs_sAVE_PixelFormatConversion`

`0xfffffe0007268620` (file offset `0x264620`), 86 entries
(`mov w10,#0x56` at `0xfffffe0008c4c7c8`), stride `0x2c`. Full dump; `kind` is
field `+24`, `cf` is `+12` (chroma format), `rng` is `+28`, `L` is `+32`
(`-1` = n/a). Rows in **bold** are the ones the `_Acis` DevCap tables accept on
t6000/t6001 (per `12-dart-surfaces-mmio.md` §1.2).

| # | fourcc | bits | planes | cf | hdiv | vdiv | kind | rng | L | +36 | +40 | primitive |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | **`420v`** | 8 | 2 | 1 | 2 | 2 | 0 | 0 | -1 | 2 | 1 | Linear |
| 1 | **`420f`** | 8 | 2 | 1 | 2 | 2 | 0 | 1 | -1 | 2 | 1 | Linear |
| 2 | **`x420`** | 10 | 2 | 1 | 2 | 2 | 0 | 0 | -1 | 7 | 2 | Linear |
| 3 | **`xf20`** | 10 | 2 | 1 | 2 | 2 | 0 | 1 | -1 | 7 | 2 | Linear |
| 4 | **`p420`** | 10 | 2 | 1 | 2 | 2 | 1 | 0 | -1 | 7 | 2 | Packed |
| 5 | **`pf20`** | 10 | 2 | 1 | 2 | 2 | 1 | 1 | -1 | 7 | 2 | Packed |
| 6-9 | `[8v0` `[8f0` `[xv0` `[xf0` | 8/8/10/10 | 2 | 1 | 2 | 2 | 2 | 0/1/0/1 | -1 | | | HTPC |
| 10 | **`&8v0`** | 8 | 2 | 1 | 2 | 2 | 3 | 0 | 0 | 2 | 1 | Interchange |
| 11 | **`&8f0`** | 8 | 2 | 1 | 2 | 2 | 3 | 1 | 0 | 2 | 1 | Interchange |
| 12 | **`&xv0`** | 10 | 2 | 1 | 2 | 2 | 3 | 0 | 0 | 7 | 2 | Interchange |
| 13 | **`&xf0`** | 10 | 2 | 1 | 2 | 2 | 3 | 1 | 0 | 7 | 2 | Interchange |
| 14-17 | `-8v0` `-8f0` `-xv0` `-xf0` | 8/8/10/10 | 2 | 1 | 2 | 2 | 3 | 0/1/0/1 | 1 | | | Interchange |
| 18-21 | `/8v0` `/8f0` `/xv0` `/xf0` | 8/8/10/10 | 2 | 1 | 2 | 2 | 3 | 0/1/0/1 | 2 | | | Interchange |
| 22-25 | `\|8v0` `\|8f0` `\|xv0` `\|xf0` | 8/8/10/10 | 2 | 1 | 2 | 2 | 3 | 0/1/0/1 | 3 | | | Interchange |
| 26 | **`422v`** | 8 | 2 | 2 | 2 | 1 | 0 | 0 | -1 | 8 | 8 | Linear |
| 27 | **`422f`** | 8 | 2 | 2 | 2 | 1 | 0 | 1 | -1 | 8 | 8 | Linear |
| 28 | **`x422`** | 10 | 2 | 2 | 2 | 1 | 0 | 0 | -1 | 8 | 8 | Linear |
| 29 | **`xf22`** | 10 | 2 | 2 | 2 | 1 | 0 | 1 | -1 | 8 | 8 | Linear |
| 30 | **`p422`** | 10 | 2 | 2 | 2 | 1 | 1 | 0 | -1 | 8 | 8 | Packed |
| 31 | **`pf22`** | 10 | 2 | 2 | 2 | 1 | 1 | 1 | -1 | 8 | 8 | Packed |
| 32-35 | `[8v2` `[8f2` `[xv2` `[xf2` | 8/8/10/10 | 2 | 2 | 2 | 1 | 2 | 0/1/0/1 | -1 | | | HTPC |
| 36 | **`&8v2`** | 8 | 2 | 2 | 2 | 1 | 3 | 0 | 0 | 8 | 8 | Interchange |
| 37 | **`&8f2`** | 8 | 2 | 2 | 2 | 1 | 3 | 1 | 0 | 8 | 8 | Interchange |
| 38 | **`&xv2`** | 10 | 2 | 2 | 2 | 1 | 3 | 0 | 0 | 8 | 8 | Interchange |
| 39 | **`&xf2`** | 10 | 2 | 2 | 2 | 1 | 3 | 1 | 0 | 8 | 8 | Interchange |
| 40-51 | `-`/`/`/`\|` `8v2 8f2 xv2 xf2` | 8/10 | 2 | 2 | 2 | 1 | 3 | 0/1 | 1/2/3 | | | Interchange |
| 52 | **`444v`** | 8 | 2 | 3 | 1 | 1 | 0 | 0 | -1 | 9 | 9 | Linear |
| 53 | **`444f`** | 8 | 2 | 3 | 1 | 1 | 0 | 1 | -1 | 9 | 9 | Linear |
| 54 | **`x444`** | 10 | 2 | 3 | 1 | 1 | 0 | 0 | -1 | 9 | 10 | Linear |
| 55 | **`xf44`** | 10 | 2 | 3 | 1 | 1 | 0 | 1 | -1 | 9 | 10 | Linear |
| 56 | **`p444`** | 10 | 2 | 3 | 1 | 1 | 1 | 0 | -1 | 9 | 10 | Packed |
| 57 | **`pf44`** | 10 | 2 | 3 | 1 | 1 | 1 | 1 | -1 | 9 | 10 | Packed |
| 58-61 | `[8v4` `[8f4` `[xv4` `[xf4` | 8/8/10/10 | 2 | 3 | 1 | 1 | 2 | 0/1/0/1 | -1 | | | HTPC |
| 62 | **`&8v4`** | 8 | 2 | 3 | 1 | 1 | 3 | 0 | 0 | 9 | 9 | Interchange |
| 63 | **`&8f4`** | 8 | 2 | 3 | 1 | 1 | 3 | 1 | 0 | 9 | 9 | Interchange |
| 64 | **`&xv4`** | 10 | 2 | 3 | 1 | 1 | 3 | 0 | 0 | 9 | 10 | Interchange |
| 65 | **`&xf4`** | 10 | 2 | 3 | 1 | 1 | 3 | 1 | 0 | 9 | 10 | Interchange |
| 66-77 | `-`/`/`/`\|` `8v4 8f4 xv4 xf4` | 8/10 | 2 | 3 | 1 | 1 | 3 | 0/1 | 1/2/3 | | | Interchange |
| 78 | **`L008`** | 8 | 1 | 0 | 1 | 1 | 0 | 1 | -1 | 6 | 6 | Linear |
| 79 | **`L010`** | 10 | 1 | 0 | 1 | 1 | 0 | 1 | -1 | 7 | 7 | Linear |
| 80 | **`&L08`** | 8 | 1 | 0 | 1 | 1 | 3 | 1 | 0 | 6 | 6 | Interchange |
| 81 | **`&L10`** | 10 | 1 | 0 | 1 | 1 | 3 | 1 | 0 | 7 | 7 | Interchange |
| 82 | `-L08` | 8 | 1 | 0 | 1 | 1 | 3 | 1 | 1 | 6 | 6 | Interchange |
| 83 | `/L08` | 8 | 1 | 0 | 1 | 1 | 3 | 1 | 2 | 6 | 6 | Interchange |
| 84 | `\|L08` | 8 | 1 | 0 | 1 | 1 | 3 | 1 | 3 | 6 | 6 | Interchange |
| 85 | `L00h` | 16 | 1 | 0 | 1 | 1 | 0 | 1 | -1 | 6 | 6 | Linear |

Reproduce with:

```python
import struct
d = open("data/blobs/kc.macho","rb").read()
base = 0x8000 + (0x7268620 - 0x700c000)      # __PRELINK_TEXT vmaddr -> file
for i in range(86):
    print(struct.unpack_from("<11i", d, base + i*44))
```

Everything in `12-dart-surfaces-mmio.md` §1.1's field table is confirmed by this
dump, with `+12` upgraded from "chroma present (0 = mono)" to the full
`_E_ChromaFmt` enum. The `+24 -> primitive` mapping in §1.1 is also confirmed
verbatim.

---

## 6. What the driver actually needs: strides come from the buffer

For client-supplied (external) surfaces the kext does **not** derive geometry
from the format at all.

`AVE_Surface::GetLinearBuf(this, u32 idx, _S_AVE_LinearBuf *out)`
(`0xfffffe0008c6e9e4`) fills, for plane `p` in `{0, 1}`:

| out field | source | VA of the call |
|---|---|---|
| `+0` / `+32` (u64) | `base + IOSurface::getPlaneOffset(p)` | `0xfffffe0008c6ebec` / `0xfffffe0008c6ec30` |
| `+8` / `+40` (u32) | `IOSurface::getPlaneSize(p)` | `0xfffffe0008c6ec00` / `0xfffffe0008c6ec44` |
| `+12` / `+44` (u32) | `IOSurface::getPlaneBytesPerRow(p)` | `0xfffffe0008c6ec10` / `0xfffffe0008c6ec54` |

Plane 1 is only filled if `IOSurface::getPlaneCount() >= 2`
(`0xfffffe0008c6ea38`, compare at `0xfffffe0008c6ec20`).

`AVE_Surface::GetYUVStride(int*, int*, int*)` (`0xfffffe0008c6e72c`) likewise
returns `IOSurface::getPlaneBytesPerRow(0)` and `(1)` (`0xfffffe0008c6e89c`,
`0xfffffe0008c6e8b8`, `0xfffffe0008c6e9bc`); it never computes anything. Its one
caller, `AVE_HwC::ProcessReadyCmd_Process+0x854` (`0xfffffe0008c116e4`), passes
the results straight into `kernel_debug` (`0xfffffe000bc8cacc`, resolved from
the fileset symbol tables) with trace code `0x2ba60074` — **it is a tracepoint,
not the data path.**

`AVE_Surface::GetCompressedBuf` (`0xfffffe0008c6ece4`) is the tiled equivalent
and is equally IOSurface-driven:
`getTileDataRegionOffsetOfPlane` (`0xfffffe0008c6ef00`),
`getTileHeaderRegionOffsetOfPlane` (`0xfffffe0008c6ef14`),
`getHorizontalPixelOffsetWithinTileArrayOfPlane` (`0xfffffe0008c6ef28`),
`getVerticalPixelOffsetWithinTileArrayOfPlane` (`0xfffffe0008c6ef38`),
`getPlaneWidth`/`getPlaneHeight` (`0xfffffe0008c6ef48`/`0xfffffe0008c6ef58`).
It is gated on `layoutKind & ~1 == 2` (`0xfffffe0008c6eeec`–`c6eef0`), i.e.
kinds 2 and 3.

**Consequence for a V4L2 driver:** the hardware side of this interface takes an
arbitrary per-plane `bytes_per_row` and an arbitrary per-plane offset. The
`Calc*FrameSize` formulas are the *unpadded minimum*, not a required layout.
Nothing was found that rejects a larger stride — but nothing was found that
proves the firmware accepts one either, so **"arbitrary stride is accepted" is
unverified** and the safe first-light choice is the unpadded layout in §8.

---

## 7. Dimension alignment

Two independent facts, neither of which is an input-buffer stride requirement.

**`AVE_Enc_AlignDimension(_E_AVE_DevID, _E_AVE_ClientType, _E_AVE_EncType, int *pW, int *pH)`**
at `0xfffffe0008ba2660`:

```c
set = AVE_DevCap_FindResolution(devid, clientType, encType);   // 0xfffffe0008ba2690
if (!set || set->count <= 0) return error;                     // 0xfffffe0008ba2694, ba26a0
if (clientType == 1 || clientType == 2) {                      // 0xfffffe0008ba26a8-ba26b4
    *pW = max((*pW + 15) & ~15, set->minEntry[0].w);           // 0xfffffe0008ba26c0-ba26d0
    *pH = max((*pH + 15) & ~15, set->minEntry[0].h);           // 0xfffffe0008ba26d8-ba26e8
    return 0;
}
```

So the encoder's coded dimensions are rounded up to a multiple of **16** and
clamped to a per-device minimum. This function has **no caller** in the
kernelcache (same scan as §3), so read it as documentation of the constraint
rather than as the enforcement point.

**`AVE_CalcBufSizeOfCodedData`** (`0xfffffe0008b5f58c`) aligns width to 16 for
`encType == 1` (AVC) and 32 otherwise (HEVC) before calling
`AVE_Linear_CalcFrameSize`: `csel w28, #16, #32` at `0xfffffe0008b5f5d8`,
`csel w23, #-16, #-32` at `0xfffffe0008b5f5cc`, then
`w0 = (width + w28 - 1) & w23` at `0xfffffe0008b5f608`–`b5f610`, and
`bl 0xfffffe0008b8cfec` at `0xfffffe0008b5f620` with `w2 = 8` (bits) and
`w3 = 1` (`_E_ChromaFmt` = 420). That call site is what fixes the argument order
of `AVE_Linear_CalcFrameSize`.

**Resolution limits.** `AVE_DevCap_FindResolution` (`0xfffffe0008baa12c`)
returns `SEntry[+0x18]`, which for DevID 11/12 + AVC is
`gc_sAVE_DevCap_Resolution_AVC_Nyx_C` at `0xfffffe0007248018`
(per `12-dart-surfaces-mmio.md` §0). The set layout follows from
`AVE_Enc_CheckResolution` (`0xfffffe0008ba2128`): `{u32 nMin;
{u32 kind, u32 w, u32 h} min[nMin]; ... u32 nMax @ +52;
{u32 kind, u32 w, u32 h} max[nMax] @ +56}` — min compared with `>=`
(`0xfffffe0008ba2168`/`ba2174`), max with `<=` (`0xfffffe0008ba21a8`/`ba21b4`).
Reading the two `Nyx_C` tables directly:

| table | min entries | max entries |
|---|---|---|
| `Resolution_AVC_Nyx_C` `0xfffffe0007248018` | `{1, 192, 96}`, `{2, 352, 208}` | `{-1, 4096, 4096}` |
| `Resolution_HEVC_Nyx_C` `0xfffffe0007248220` | `{1, 160, 64}`, `{2, 192, 208}` | `{0, 65536, 8192}`, `{0, 8192, 16384}`, `{1, 16384, 8192}`, `{1, 8192, 16384}` |

The leading `kind` field is matched against the 6th argument of
`CheckResolution`, with `-1` acting as a wildcard (`tbnz w10,#31` at
`0xfffffe0008ba21c8`); what that argument *is* was **not** determined. The
HEVC `65536` is what the table contains — reported as read, not endorsed.

---

## 8. Concrete numbers for first light

Computed from §4 with C signed-truncating division, matching the instruction
sequences exactly. `420v` and `420f` are identical in size (they differ only in
field `+28`, video vs full range).

### `420v` / `420f` — Linear, 8-bit, 4:2:0 (the NV12 path)

| resolution | luma stride | luma size | chroma stride | chroma size | **total** |
|---|---|---|---|---|---|
| 1280x720 | 1280 | 921 600 | 1280 | 460 800 | **1 382 400** (`0x151800`) |
| 1920x1080 | 1920 | 2 073 600 | 1920 | 1 036 800 | **3 110 400** (`0x2F7600`) |
| 3840x2160 | 3840 | 8 294 400 | 3840 | 4 147 200 | **12 441 600** (`0xBDD000`) |

Plane offsets, **inferred** (the primitives return only a total; the offsets
follow from the plane sizes with no padding, and match what an unpadded NV12
`IOSurface` would report through `getPlaneOffset`):

```
plane 0 (Y)    offset 0                 size w*h        bytesPerRow w
plane 1 (CbCr) offset w*h               size w*h/2      bytesPerRow w
total          w*h*3/2
```

For 1920x1080 that is Y at `0`, CbCr at `0x1FA400`, total `0x2F7600`.

### Other accepted formats at 1920x1080

| fourcc | primitive | luma | chroma | total |
|---|---|---|---|---|
| `420v` `420f` | Linear 8b 420 | 2 073 600 | 1 036 800 | 3 110 400 |
| `422v` `422f` | Linear 8b 422 | 2 073 600 | 2 073 600 | 4 147 200 |
| `444v` `444f` | Linear 8b 444 | 2 073 600 | 4 147 200 | 6 220 800 |
| `L008` | Linear 8b mono | 2 073 600 | 0 | 2 073 600 |
| `x420` `xf20` | Linear 10b 420 | 4 147 200 | 2 073 600 | 6 220 800 |
| `p420` `pf20` | Packed 10b 420 | 2 764 800 | 1 382 400 | 4 147 200 |
| `&8v0` `&8f0` | Interchange lossless 8b 420 | 2 220 032 | 1 077 248 | 3 297 280 |
| `&L08` | Interchange lossless 8b mono | 2 220 032 | 4 308 992 | 6 529 024 |

`&8v0` breakdown at 1920x1080: luma `tX=60`, `tY=34`, data `60*34*1024 =
2 088 960`, meta `align128(32 << (6+6)) = 131 072`; chroma `cw=960`, `ch=540`,
`ctX=60`, `ctY=34`, data `60*34*512 = 1 044 480`, meta
`align128(8 << (6+6)) = 32 768`.

`&L08` shows the §1 row-0 quirk: chroma format `400` gives divisors `(1,1)`, so
`AVE_Interchange_CalcChromaSize` adds a 4:4:4-sized chroma region to a
monochrome format. Read from the code, not a transcription error.

The other resolutions are in the same shapes; regenerate with the pseudocode in
§4 if needed.

---

## 9. What is still open

- **Whether a padded stride is accepted.** §6 shows the kext forwards the
  IOSurface's `bytesPerRow` verbatim, but nothing was found on either side that
  states the firmware's constraint. Unverified in both directions.
- **`_S_AVE_PixelFmt` `+36` / `+40`.** No reader anywhere in the kernelcache
  (§2). Values tabulated, meaning unknown.
- **`AVE_Work_Enc_CalcSurfaceInfo`** (`0xfffffe0008ca9a28`) and the
  `AVE_CalcBufSizeOf*` family (`Recon`, `Colocated`, `LFSRef`, `MCTFOutput`,
  `MBStats`, `EntropyCoding`, …, all around `0xfffffe0008b5eee4`–`0xfffffe0008b63280`).
  These size the kernel-allocated internal surfaces and are untouched here.
  `AVE_CalcBufSizeOfCodedData` (`0xfffffe0008b5f58c`) — the output bitstream
  buffer size — was read only far enough to fix `Linear`'s argument order; it
  scales the frame size by double-precision constants loaded from
  `0xfffffe000723e9f0`/`0xfffffe000723ea00` that were not decoded.
- **The HTPC `k` argument.** Always 0 from the dispatcher; no other caller
  exists, so the non-zero behaviour is unexercised.
- **The `kind` field in the resolution min/max entries** (§7) and the 6th
  argument of `AVE_Enc_CheckResolution` it is matched against.
- **Where dimension alignment is actually enforced.** `AVE_Enc_AlignDimension`
  is dead code in this kernelcache.
