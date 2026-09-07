> ## Verification note
>
> **The stride rule is confirmed verbatim.** Apple's own assertion text at
> `0xfffffe00072832bd` reads:
>
> ```
> pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iAddr   != 0 &&
> pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iSize   != 0 &&
> pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iStride != 0 &&
> pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iStride % 64 == 0 &&
> pLinear->saCIBuf[AVE_BufIdx_Chroma].saIBuf[AVE_BufIdx_Data].iStride % 64 == 0
> ```
>
> and the code matches it instruction for instruction:
>
> ```
> fffffe0008b72500:  cbz  x8, fail                  ; luma iAddr != 0
> fffffe0008b72504:  ldr  w8, [x27, #1400]          ; luma iSize
> fffffe0008b72508:  cbz  w8, fail
> fffffe0008b7250c:  ldr  w8, [x27, #1404]          ; luma iStride
> fffffe0008b72510:  cbz  w8, fail
> fffffe0008b72514:  and  w8, w8, #0x3f             ; luma iStride % 64
> fffffe0008b72518:  cbnz w8, fail
> fffffe0008b7251c:  ldrb w8, [x27, #1436]          ; chroma iStride (low byte)
> fffffe0008b72520:  tst  w8, #0x3f                 ; chroma iStride % 64
> fffffe0008b72524:  b.eq ok
> ```
>
> This is the one hard, enforced constraint a V4L2 driver must impose on
> userspace buffers: **`bytesPerRow` must be a non-zero multiple of 64 on both
> planes.** Note it does not conflict with
> [14-frame-size-formulas.md](14-frame-size-formulas.md)'s finding that the
> size primitives apply no alignment — the primitives compute, this path
> validates, and they are different code.

# Surface layout, strides and buffer requirements

What a client-supplied frame buffer must actually look like, read out of
`AppleAVE2.kext` in `data/blobs/kc.macho` (macOS 26.6.2,
`kernelcache.release.mac13j`). Every constant below carries the VA of the
instruction it came from. Anything not read directly out of the image is marked
**inferred** or **unknown**.

This document covers the `AVE_Surface` object and its accessors. The
`AVE_*_CalcFrameSize` arithmetic (`AVE_Linear/Packed/Interchange/HTPC`) is a
separate piece of work and is only referenced by name here.

Companion note on method: kernel symbols outside `AppleAVE2` (IOSurface,
libkern) were recovered by walking the `LC_SYMTAB` of every `LC_FILESET_ENTRY`
in the kernelcache, which resolves the `IOSurface::*` calls the kext makes. That
is what makes most of the section below readable at all.

---

## 0. The short answer

| Requirement | Value | Confidence |
|---|---|---|
| Number of planes AVE addresses | **2** (luma + chroma). A third out-pointer exists but is fed from plane **1**, never plane 2 | Confirmed |
| Per-plane stride | `IOSurface::getPlaneBytesPerRow(n)` | Confirmed |
| Per-plane offset | `IOSurface::getPlaneOffset(n)` — the kext never computes plane offsets itself for linear formats | Confirmed |
| **Stride alignment** | **luma and chroma `bytesPerRow` must be a non-zero multiple of 64 bytes**; violation makes `AVE_CHM_SetDataInfo_FwBuf` return `-1015` | Confirmed |
| Height alignment | none found in any executed path (see §7) | See §7 |
| Base-address alignment | none enforced by the kext beyond "non-zero"; the IOVA comes from the IOMMU mapping | Confirmed |
| Allocation granularity (kext-allocated surfaces) | `align_up(size, max(PAGE_SIZE, 0x4000))` — i.e. **16 KB minimum** | Confirmed |
| Frame width/height | taken from the **session config**, not from the surface — the kext never calls `IOSurface::getWidth()`/`getHeight()` | Confirmed |
| Max pixel count | `width * height <= 65520 * 8192` | Confirmed |

---

## 1. The `AVE_Surface` object

Constructor `AVE_Surface::AVE_Surface(AVE_DevInfo*, u64 id)`
(`0xfffffe0008c69ac4`) and the accessors give the following layout. Object size
is at least `0xf8` (the last constructor store is `stur q0,[x21,#232]` at
`0xfffffe0008c69b10`, covering `0xe8..0xf8`).

| offset | type | meaning | evidence |
|---|---|---|---|
| `+0x00` | — | `AVE_DLList` node | `_AVE_DLList_Init_Node` at `0xfffffe0008c69ae4` |
| `+0x18` | `AVE_DevInfo*` | device info | `stp x20,x19,[x21,#24]` `0xfffffe0008c69ae8` |
| `+0x20` | `u64` | surface id (ctor arg 2) | same instruction; printed as `%lld` in every log line |
| `+0x28` | `char[0x40]` | name string | `AVE_SNPrintf(this+0x28, 64, "%s", name)` `0xfffffe0008c69c44`–`0xc69c4c` |
| `+0x68` | `IOSurface*` | the backing surface | `str x22,[x19,#104]` `0xfffffe0008c69c5c` |
| `+0x78` | `IOMemoryDescriptor*` | from `IOSurface::getMemoryDescriptor()` | `str x0,[x19,#120]` `0xfffffe0008c6c630`; `GetIOMD` `0xfffffe0008c6da04` |
| `+0x80` | `IOMemoryMap*` | kernel mapping object | `str x0,[x21,#128]` `0xfffffe0008c6cb1c` |
| `+0x88` | `u32` | `IOSurface::getSurfaceID()` | `str w8,[x19,#136]` `0xfffffe0008c69c74` |
| `+0x90` | `u64` | physical address | `str x0,[x19,#144]` `0xfffffe0008c6c89c`; read by `GetPhyAddr` `0xfffffe0008c6da28` |
| `+0x98` | `u64` | kernel virtual address | `str x0,[x21,#152]` `0xfffffe0008c6cb48`; read by `GetKernelAddr` `0xfffffe0008c6da48` |
| `+0xC0` | `_S_AVE_DART_Entry*[4]` | **one DART mapping per EU id**, *not* per plane | `add x8,x19,#0xc0` + `lsl x9,euID,#3` `0xfffffe0008c6e24c`–`0xc6e268` |
| `+0xE0` | `u32` | `IOSurface::getAllocSize()` | `str w0,[x19,#224]` `0xfffffe0008c69c58`; returned by `GetSize` `0xfffffe0008c6dbc8` |
| `+0xE8` | `u64` | attribute word (Create arg 10) | `str x20,[x19,#232]` `0xfffffe0008c69c60`; `GetAttribute` `0xfffffe0008c6d9d4` |
| `+0xF0` | `u64` | `m_iOpFlag` | `GetOpStatus` `0xfffffe0008c6d9b8` |

### 1.1 `m_iOpFlag` bits

The names come from the kext's own assertion strings, so these are exact, not
guessed:

| bit | assertion string (VA) | set by | meaning |
|---|---|---|---|
| 10 | `(m_iOpFlag & (1ULL<<10)) != 0` @ `0xfffffe00072a9c4e` | `Prepare` — `orr x8,x8,#0x400` @ `0xfffffe0008c6c8a4` | IOMD obtained + prepared, physical address valid |
| 11 | `(m_iOpFlag & (1ULL<<11)) != 0` @ `0xfffffe00072aa1d1` | `KernelMap` — `orr x8,x8,#0x800` @ `0xfffffe0008c6cb50` | kernel-mapped, `+0x98` valid |
| 16+`euID` | `(m_iOpFlag & ((1ULL<<16)<<(euID))) != 0` @ `0xfffffe00072aa2da` | `DARTMap` — `orr x9,x9,x26` @ `0xfffffe0008c6d440` (`x26 = 0x10000 << euID`) | DART-mapped for engine unit `euID` |

`euID` is bounded to `< 4` (`cmp w1,#3; b.hi` at `0xfffffe0008c6e198`,
`0xfffffe0008c6da7c`, `0xfffffe0008c6ea0c`, `0xfffffe0008c6ed0c`; assertion
string `euID < 4 && ...` at `0xfffffe00072aa246`). So a single surface can carry
up to four independent IOMMU mappings, one per engine unit.

`AVE_Surface::DARTMap(u32 euID, AVE_DART*)` (`0xfffffe0008c6cdfc`):

```
require m_iOpFlag bit 10                        ; 0xfffffe0008c6ce44
if (m_iOpFlag & (0x10000 << euID)) return       ; already mapped, 0xfffffe0008c6d014
AVE_DART::SetMapCacheAttr(dart, this->iomd, IOSurface::getMapCacheAttribute())
                                                ; 0xfffffe0008c6d0b0-0xc6d0c0
AVE_DART::Map(dart, this->iomd, &this->dartEntry[euID])
                                                ; 0xfffffe0008c6d0ec
m_iOpFlag |= 0x10000 << euID                    ; 0xfffffe0008c6d440
```

`_S_AVE_DART_Entry + 0x20` holds the device address; it is written in
`AVE_DART::Map` at `0xfffffe0008ba7110` (`str x8,[x9,#32]`) from the stack
out-buffer that the `IODMACommand` segment call filled (see `12-dart-surfaces-mmio.md`
§3.1). *Inferred:* it is the IOVA of the first IOVM segment, i.e. AVE requires
the surface to map to one contiguous IOVA range.

---

## 2. Plane addressing — the `Get*YUV*` family

All five have the same shape. Each takes **three** out-pointers, at least one of
which must be non-NULL, and each begins by re-validating the pixel format:

```
planeCount = IOSurface::getPlaneCount(this->m_pSurface)
if (!CheckSupportedPixelFormat(IOSurface::getPixelFormat(...))) return -1002
*p0 = f(0)
*p1 = (planeCount >= 2) ? f(1) : 0
*p2 = (planeCount >= 2) ? f(1) : 0      <-- plane 1 again, never plane 2
```

| function | VA | gate | base | per-plane term |
|---|---|---|---|---|
| `GetYUVStride(int*,int*,int*)` | `0xfffffe0008c6e72c` | bit 10 | — | `IOSurface::getPlaneBytesPerRow(n)` `0xfffffe0008c6e89c`, `0xc6e8b8`, `0xc6e9bc` |
| `GetYUVSize(int*,int*,int*)` | `0xfffffe0008c6e474` | bit 10 | — | `IOSurface::getPlaneSize(n)` `0xfffffe0008c6e5e4`, `0xc6e600`, `0xc6e704` |
| `GetYUVPhyAddr(u64*,u64*,u64*)` | `0xfffffe0008c6dbd8` | bit 10 | `this+0x90` | `+ IOSurface::getPlaneOffset(n)` `0xfffffe0008c6dd4c`, `0xc6dd70`, `0xc6de7c` |
| `GetYUVKernelAddr(u64*,u64*,u64*)` | `0xfffffe0008c6dea8` | bit 11 | `this+0x98` | `+ getPlaneOffset(n)` `0xfffffe0008c6e01c`, `0xc6e040`, `0xc6e134` |
| `GetYUVDARTAddr(u32 euID,u64*,u64*,u64*)` | `0xfffffe0008c6e164` | bit 16+euID | `dartEntry[euID]->[+0x20]` | `+ getPlaneOffset(n)` `0xfffffe0008c6e398`, `0xc6e3b8`, `0xc6e3e8` |

**Consequences.**

1. There is **no per-plane array in the `AVE_Surface` object.** The `+0xC0`
   array that the `Get*YUV*` functions index is indexed by *engine unit id*, not
   by plane. Plane geometry lives entirely in the `IOSurface` and is queried
   from it on every call. This is a confirmed negative: the same disassembly
   shows exactly where the per-plane data *does* come from.
2. The layout of a linear input frame is **whatever `IOSurface` says it is**:
   plane 0 at `getPlaneOffset(0)` with `getPlaneBytesPerRow(0)`, plane 1 at
   `getPlaneOffset(1)` with `getPlaneBytesPerRow(1)`. Nothing is derived from
   width/height by the kext.
3. **Only two planes are ever read.** A 3-plane (fully planar I420) surface
   would have its V plane silently aliased onto the U plane. So the input must
   be semi-planar (NV12-style), which matches the `420v`/`420f` fourccs in
   `12-dart-surfaces-mmio.md` §1.2.
4. **`IOSurface::getWidth()` / `getHeight()` are never called by this kext.**
   (Scan of every `bl`/`b` in `__TEXT_EXEC` finds callers of both in
   `AppleJPEG`, `AppleM2ScalerCSC` and `AppleProResHW`, but none in the
   `0xfffffe0008b34bb0..0xfffffe0008ce0000` AVE range — so the test
   discriminates.) The only surface-level dimensions AVE reads are
   `getPlaneWidth`/`getPlaneHeight`, and only inside `GetCompressedBuf`
   (`0xfffffe0008c6ef48`, `0xc6f154`, `0xc6f320`). Frame dimensions come from
   `pClient->VideoParams.ui32Width/Height`.

---

## 3. `GetLinearBuf` — `_S_AVE_LinearBuf`

`AVE_Surface::GetLinearBuf(u32 euID, _S_AVE_LinearBuf* pBuf)`
(`0xfffffe0008c6e9e4`). Preconditions: `euID < 4 && pBuf != nullptr`
(`0xfffffe00072aa3a5`), `m_iOpFlag` bit 16+euID, and
`CheckSupportedPixelFormat`. It zeroes **64 bytes** (`stp q0,q0,[x20]` and
`[x20,#32]`, `0xfffffe0008c6ea28`–`0xc6ea30`) and then fills:

| offset | field | source | VA |
|---|---|---|---|
| `+0x00` | `saCIBuf[Luma].saIBuf[Data].iAddr` | `dartBase + getPlaneOffset(0)` | `0xfffffe0008c6ebf4` |
| `+0x08` | `.iSize` | `getPlaneSize(0)` | `0xfffffe0008c6ec04` |
| `+0x0c` | `.iStride` | `getPlaneBytesPerRow(0)` | `0xfffffe0008c6ec1c` |
| `+0x10` | `saCIBuf[Luma].saIBuf[1]` | left zero | — |
| `+0x20` | `saCIBuf[Chroma].saIBuf[Data].iAddr` | `dartBase + getPlaneOffset(1)` | `0xfffffe0008c6ec38` |
| `+0x28` | `.iSize` | `getPlaneSize(1)` | `0xfffffe0008c6ec48` |
| `+0x2c` | `.iStride` | `getPlaneBytesPerRow(1)` | `0xfffffe0008c6ec60` |
| `+0x30` | `saCIBuf[Chroma].saIBuf[1]` | left zero | — |

The field names are the kext's own — they appear verbatim in the assertion
string at `0xfffffe00072832bd` (quoted in §4). So the type is
`_S_AVE_LinearBuf { _S_AVE_CIBuf saCIBuf[2 /*Luma,Chroma*/]; }` with
`_S_AVE_CIBuf { struct { u64 iAddr; u32 iSize; u32 iStride; } saIBuf[2 /*Data,…*/]; }`.

If `planeCount < 2` the chroma half is left zero (`cmp w21,#2; b.lt` at
`0xfffffe0008c6ec20`) — that is the monochrome (`L008`) case.

---

## 4. The stride rule — where it is actually enforced

`AVE_CHM_SetDataInfo_FwBuf` (`0xfffffe0008b71fd8`, the function that packs
surface descriptors into the firmware command) calls `GetLinearBuf` and then
validates the result. For the **input frame** (`InputData`, written at command
buffer offset `0x4570`), at `0xfffffe0008b724f8`–`0xfffffe0008b72524`:

```
GetLinearBuf(euID, cmd + 0x4570)
if (luma.iAddr   == 0)          goto fail    ; 0xfffffe0008b72500
if (luma.iSize   == 0)          goto fail    ; 0xfffffe0008b72508
if (luma.iStride == 0)          goto fail    ; 0xfffffe0008b72510
if (luma.iStride & 0x3f)        goto fail    ; 0xfffffe0008b72514-0xb72518
if (chroma.iStride & 0x3f)      goto fail    ; 0xfffffe0008b7251c-0xb72524
```

and `fail` is `mov w22, #0xfffffc09` — **return `-1015`**
(`0xfffffe0008b7270c`). This is a real error return, not just a log. The
matching assertion text at `0xfffffe00072832bd` is:

```
pLinear->saCIBuf[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iAddr != 0 &&
... .iSize != 0 && ... .iStride != 0 && ... .iStride % 64 == 0 &&
pLinear->saCIBuf[AVE_BufIdx_Chroma].saIBuf[AVE_BufIdx_Data].iStride % 64 == 0
```

The identical check is repeated for the **scaled source**
(`InputScaledData`, command offset `0x4630`) at
`0xfffffe0008b72654`–`0xfffffe0008b7267c`, assertion string
`0xfffffe000728349e`.

> **This is the single most important constraint for a V4L2 driver:
> `bytesPerRow` of both the luma and the chroma plane must be a non-zero
> multiple of 64 bytes.**

For an 8-bit NV12 frame that means `V4L2 bytesperline` must be a multiple of 64
for both planes; since NV12 chroma stride equals luma stride, aligning the luma
stride to 64 satisfies both.

Notably absent from this validation: any check on `iSize` beyond non-zero, any
check on `iAddr` alignment, and any height or plane-height check.

---

## 5. `GetCompressedBuf` — `_S_AVE_CompressedBuf`

`AVE_Surface::GetCompressedBuf(u32 euID, _S_AVE_CompressedBuf* pBuf)`
(`0xfffffe0008c6ece4`). Same `euID < 4` and bit-16+euID preconditions. It zeroes
**128 bytes** (four `stp q0,q0` at `0xfffffe0008c6ed2c`–`0xc6ed38`), i.e. two
64-byte per-plane records.

Dispatch (`fmt = AVE_PixelFmt_FindByType(pixelFormat)`, `0xfffffe0008c6eedc`):

| condition | path |
|---|---|
| `fmt != NULL && (fmt->kind & ~1) != 2` | return `-1002` (`0xfffffe0008c6eef0`, `0xc6f224`) — i.e. only the tiled families (`kind` 2 = HTPC, 3 = Interchange) are accepted |
| `fmt->kind == 2` | HTPC arithmetic (`0xfffffe0008c6ef6c`–`0xc6f074`) |
| `fmt->kind == 3` | Interchange arithmetic (`0xfffffe0008c6f22c`–`0xc6f2b8`) |
| `fmt == NULL` | `CheckInternalPixelFormat` → the `]` family (`0xfffffe0008c6f078`) |

(`fmt->kind` is field `+24` of the master pixel-format table, per
`12-dart-surfaces-mmio.md` §1.1.)

Per-plane record — luma at `+0x00`, chroma at `+0x40`:

| off (luma / chroma) | field | source | VA (luma / chroma) |
|---|---|---|---|
| `+0x00` / `+0x40` | `sCIBuf.saIBuf[Data].iAddr` | `dartBase + getTileDataRegionOffsetOfPlane(n)` | `0xfffffe0008c6ef08` / `0xc6f2e0` |
| `+0x08` / `+0x48` | `.iSize` | `Calc*PlaneDataSizeByTile(...)` | `0xfffffe0008c6f034` / `0xc6f418` |
| `+0x0c` / `+0x4c` | `.iStride` | copy of `sCPInfo.iHeaderStride` | `0xfffffe0008c6f2c0` / `0xc6f560` |
| `+0x10` / `+0x50` | `sCIBuf.saIBuf[Meta].iAddr` | `dartBase + getTileHeaderRegionOffsetOfPlane(n)` | `0xfffffe0008c6ef1c` / `0xc6f2f4` |
| `+0x18` / `+0x58` | `.iSize` | `Calc*PlaneMetaSizeByTile(...)` | `0xfffffe0008c6f2b8` / `0xc6f558` |
| `+0x20` / `+0x60` | `sCPInfo` — uncompressed plane size | `tilesX*tileW * tilesY*tileH * ceil(bits/8)` (chroma `<<1`) | `0xfffffe0008c6eff4` / `0xc6f3d8` (`lsl w8,w8,#1` @ `0xc6f3d4`) |
| `+0x24` / `+0x64` | plane width | `getPlaneWidth(n)` | `0xfffffe0008c6ef4c` / `0xc6f324` |
| `+0x28` / `+0x68` | plane height | `getPlaneHeight(n)` | `0xfffffe0008c6ef5c` / `0xc6f334` |
| `+0x2c` / `+0x6c` | h. pixel offset in tile array | `getHorizontalPixelOffsetWithinTileArrayOfPlane(n)` — **but `n = 0` in the chroma record too** | `0xfffffe0008c6ef2c` / `0xc6f304` (`mov w1,#0` @ `0xc6f2fc`) |
| `+0x30` / `+0x70` | v. pixel offset in tile array | `getVerticalPixelOffsetWithinTileArrayOfPlane(n)` — same, `n = 0` | `0xfffffe0008c6ef3c` / `0xc6f314` (`mov w1,#0` @ `0xc6f30c`) |
| `+0x34` / `+0x74` | `sCPInfo.iHeaderStride` | kind 2 & `]`: `getBytesPerRowOfTileHeaderGroupsOfPlane(n)`; kind 3: `getBytesPerRowOfTileDataOfPlane(n)` | `0xfffffe0008c6ef78` / `0xc6f350`; `0xfffffe0008c6f238` / `0xc6f4d0` |
| `+0x38` / `+0x78` | HTPC vertical header grouping mode | `getHTPCVerticalHeaderGroupingModeOfPlane(n)` | `0xfffffe0008c6ef98` / `0xc6f360` |
| `+0x3c` / `+0x7c` | HTPC prediction selector | `getHTPCPredictionSelectorOfPlane(n)` | `0xfffffe0008c6ef88` / `0xc6f370` |

The `sCPInfo.iHeaderStride` naming is not a guess: the assertion at
`0xfffffe00072831c7` reads
`pCompressed->saCPBuf[AVE_BufIdx_Luma].sCIBuf.saIBuf[AVE_BufIdx_Data].iAddr != 0 &&
pCompressed->saCPBuf[AVE_BufIdx_Luma].sCPInfo.iHeaderStride != 0`, and the
matching code in `AVE_CHM_SetDataInfo_FwBuf` tests exactly `cmd+0x4570+0x00`
and `cmd+0x4570+0x34` (`0xfffffe0008b72430`, `0xfffffe0008b72438`), failing with
`-1015`. So for compressed input the only two hard requirements are **luma data
address non-zero and luma header stride non-zero** — no `% 64` rule applies to
the compressed path.

### 5.1 Data plane vs metadata plane

**They are two separate regions inside the same `IOSurface`**, located by two
different IOSurface accessors on the same plane index:

* data region: `getTileDataRegionOffsetOfPlane(n)`
* metadata (tile header) region: `getTileHeaderRegionOffsetOfPlane(n)`

Both are byte offsets added to the same DART base. Their relative order and any
padding between them is entirely IOSurface's business; the kext does not
compute or check it.

Sizes (these are the `*ByTile` variants, distinct from the `CalcLuma/Chroma…`
ones documented in `12-dart-surfaces-mmio.md` §1.4):

| function | VA | formula |
|---|---|---|
| `AVE_Interchange_CalcPlaneDataSizeByTile(tilesX, tilesY, bytesPerTileData)` | `0xfffffe0008c2a544` | `tilesX * tilesY * bytesPerTileData` (`mul`,`mul` @ `0xc2a548`,`0xc2a54c`) |
| `AVE_HTPC_CalcPlaneDataSizeByTile(tilesX, tilesY, bytesPerTileData)` | `0xfffffe0008b3fb60` | identical |
| `AVE_Interchange_CalcPlaneMetaSizeByTile(tilesX, tilesY, bytesPerTileHeader)` | `0xfffffe0008c2a5b8` | `bytesPerTileHeader << (ceil(log2 tilesX) + ceil(log2 tilesY))` — via `clz` at `0xc2a5c0`/`0xc2a5d8`, `lsl` at `0xc2a5ec`. **No 128-byte round-up here** |
| `AVE_HTPC_CalcPlaneMetaSizeByTile(tilesY, vGroupingMode, bytesPerRowOfHeaderGroups)` | `0xfffffe0008b3fbb4` | `ceil(tilesY / (1<<vGroupingMode)) * bytesPerRowOfHeaderGroups` (`lsl`/`add`/`sub`/`lsr`/`mul` @ `0xb3fbb8`–`0xb3fbcc`) |

The tile counts, tile dimensions and bytes-per-tile all come from the
`IOSurface` itself (`getWidthInTilesOfPlane`, `getHeightInTilesOfPlane`,
`getTileWidthOfPlane`, `getTileHeightOfPlane`, `getBytesPerTileDataOfPlane`,
`getBytesPerTileHeaderOfPlane`), not from the AVE tables. So for compressed
input AVE trusts the producer's IOSurface metadata completely.

---

## 6. `CheckSupportedPixelFormat` vs `CheckInternalPixelFormat`

`AVE_Surface::CheckSupportedPixelFormat(u32)` (`0xfffffe0008c69a8c`) is exactly:

```
return AVE_PixelFmt_FindByType(fmt) != NULL || CheckInternalPixelFormat(fmt);
```

(`0xfffffe0008c69aa0`, `0xc69aac`) — confirming `12-dart-surfaces-mmio.md` §1.3.

`AVE_Surface::CheckInternalPixelFormat(u32)` (`0xfffffe0008c699dc`) accepts only
the four `]`-prefixed bases with sub-offsets 0/2/4 (mask `0x15` at
`0xfffffe0008c69a4c`, `0xc69a64`, `0xc69a7c`); the constants are already tabulated
in `12-dart-surfaces-mmio.md` §1.3 and re-verified here.

**What distinguishes them, from call sites** (complete `bl` scan of
`__TEXT_EXEC`):

| function | callers |
|---|---|
| `CheckSupportedPixelFormat` | `GetYUVPhyAddr`, `GetYUVKernelAddr`, `GetYUVDARTAddr`, `GetYUVSize`, `GetYUVStride`, `GetLinearBuf` — i.e. every **linear** accessor |
| `CheckInternalPixelFormat` | `CheckSupportedPixelFormat` (`0xfffffe0008c69aac`) and `GetCompressedBuf` (`0xfffffe0008c6f07c`) |

So the `]` family is not a *client* format at all — it is a format that has no
entry in the master conversion table and therefore no `CalcFrameSize` path, and
whose buffer geometry is only ever obtained through `GetCompressedBuf`. In that
path it is treated as **Interchange-style compressed**: it uses
`AVE_Interchange_CalcPlaneDataSizeByTile` / `…MetaSizeByTile`
(`0xfffffe0008c6f0e4`, `0xc6f124`) with `getBytesPerTileData` /
`getBytesPerTileHeader`, but it takes its chroma plane from the *linear*
accessors `getPlaneOffset(1)` / `getPlaneSize(1)`
(`0xfffffe0008c6f1f0`, `0xc6f204`) rather than from the tile-region accessors.

*Inferred:* `]` is an AVE-internal recompressed-reference format (only the
encoder writes and reads it), which would explain the mixed luma-compressed /
chroma-linear treatment. **Unknown**: which surface kind actually carries it.

One oddity, stated as read: if `AVE_PixelFmt_FindByType` returns NULL **and**
`CheckInternalPixelFormat` returns 0, `GetCompressedBuf` returns `0` (success)
with an all-zero descriptor (`cbz w0, 0xfffffe0008c6f564` at
`0xfffffe0008c6f080`, and `0xc6f564` is the epilogue). The caller then trips its
`iAddr != 0` check and fails with `-1015`, so nothing unsafe follows, but the
return value is misleading.

---

## 7. Dimension alignment

### 7.1 `AVE_Enc_AlignDimension` — present but unreferenced

`AVE_Enc_AlignDimension(_E_AVE_DevID, _E_AVE_ClientType, _E_AVE_EncType, int* pW, int* pH)`
(`0xfffffe0008ba2660`) implements:

```
res = AVE_DevCap_FindResolution(devid, clientType, encType)   ; 0xfffffe0008ba2690
if (!res || res[0] <= 0)              return -1000
if (clientType != 1 && clientType != 2) return -1000          ; 0xfffffe0008ba26a8-0xba26b4
*pW = max(align_up(*pW, 16), res[+8])                         ; 0xfffffe0008ba26c0-0xba26d0
*pH = max(align_up(*pH, 16), res[+12])                        ; 0xfffffe0008ba26d8-0xba26e8
```

The `align_up(x,16)` is `add w,#0xf; and w,#0xfffffff0`. The floors, read from
the capability tables:

| table | VA | `res[+8]` | `res[+12]` |
|---|---|---|---|
| `gc_sAVE_DevCap_Resolution_AVC_Nyx_C` | `0xfffffe0007248018` | `0xC0` = 192 | `0x60` = 96 |
| `gc_sAVE_DevCap_Resolution_HEVC_Nyx_C` | `0xfffffe0007248220` | `0xA0` = 160 | `0x40` = 64 |
| `gc_sAVE_DevCap_Resolution_LRME_Acis` | `0xfffffe0007247e78` | `0xA0` = 160 | `0x40` = 64 |

(`Nyx_C` / `Acis` are the tables t6000/t6001 select — see
`12-dart-surfaces-mmio.md` §0. The rest of the 104-byte table layout is
**unknown**; only `[+0]`, `[+8]` and `[+12]` are read by this function.)

**However: nothing in the kernelcache calls this function.** Three independent
scans found no reference — a `bl`/`b` scan of the whole `__TEXT_EXEC`, an
`adrp`+`add` address-materialisation scan, and a scan of every 8-byte chained
fixup pointer whose low 32 bits equal its file offset (`0x1b9e660`). All three
scanners return hits for other symbols (e.g. the `bl` scan finds the single
caller of `CreateIOSurface`, the `adrp`+`add` scan finds both references to each
assertion string above), so the negative is from a discriminating test. Treat
the ×16 rule as **documented-but-not-enforced by this build**; whether the
firmware or a userspace framework applies it is **unknown**.

### 7.2 What *is* enforced on dimensions

`AVE_Client_CheckInfo` (`0xfffffe0008b90958`), assertion string
`width >= 0 && height >= 0 && iPixelProduct <= ((int64_t)65520 * (int64_t)8192)`
at `0xfffffe000728a53a`:

```
smull x25, w24, w23                 ; 0xfffffe0008b909b4  width * height (64-bit)
if (w23 < 0 || w24 < 0)  fail       ; 0xfffffe0008b909b8, 0xb909bc (tbnz #31)
if (x25 >= 0x1ffe0001)   fail       ; 0xfffffe0008b909c0-0xb909cc
```

`0x1ffe0001 = 65520*8192 + 1`, so the limit is `width*height <= 536 739 840`.
No per-dimension maximum and **no `% 16` test** appears here.

I did not find any height or plane-height validation anywhere on the input path.
That is a *not-found*, not a proven *absent*: I only walked `AVE_Surface`,
`AVE_CHM_SetDataInfo_FwBuf` and `AVE_Client_CheckInfo`. The
`AVE_Work_*_CalcSurfaceInfo` family is still undisassembled.

---

## 8. Surfaces the kext allocates itself

This is the allocation contract, read straight out of the IOSurface property
dictionary the kext builds.

`AVE_Surface::CreateDict(int cacheMode, int mapCacheAttr, u64 protectionOptions,
u32 pixelFormat, int size, OSDictionary** out)` (`0xfffffe0008c68f58`):

```
dict = OSDictionary::withCapacity(5)                       ; 0xfffffe0008c68f94
dict[kIOSurfaceCacheMode]          = OSNumber(cacheMode, 32)   ; key 0xfffffe0007ed8fa0, 0xc68fac
dict[kIOSurfaceMapCacheAttribute]  = OSNumber(mapCacheAttr,32) ; key 0xfffffe0007ed8fa8, 0xc69018
dict[kIOSurfaceProtectionOptions]  = OSNumber(protOpts, 64)    ; key 0xfffffe0007ed8fc0, 0xc69084
dict[kIOSurfacePixelFormat]        = OSNumber(pixelFormat,32)  ; key 0xfffffe0007ed8fb8, 0xc690e8
dict[kIOSurfaceAllocSize]          = OSNumber(alignedSize,32)  ; key 0xfffffe0007ed8f98, 0xc69180
```

with

```
P = 1 << PAGE_SHIFT_CONST                       ; 0xfffffe0008c69144-0xc69154
A = (P > 0x4000) ? P : 0x4000                   ; 0xfffffe0008c6915c-0xc69160
alignedSize = (size + A - 1) & ~(A - 1)         ; 0xfffffe0008c69164-0xc69174
```

Key names were resolved through the kext's authenticated-pointer GOT into the
IOSurface kext's `__DATA_CONST` (`_kIOSurfaceCacheMode` @ `0xfffffe000873fa00`,
`_kIOSurfaceMapCacheAttribute` @ `0x…873fa68`, `_kIOSurfaceProtectionOptions` @
`0x…873fb18`, `_kIOSurfacePixelFormat` @ `0x…873f968`, `_kIOSurfaceAllocSize` @
`0x…873f970`).

**The dictionary sets no width, no height, no `bytesPerRow` and no
`planeInfo`.** Kext-allocated surfaces are plain linear byte buffers of a given
`allocSize`. `PAGE_SHIFT_CONST` reads 0 in the on-disk image (it is filled at
boot), but the `max(P, 0x4000)` floor is unconditional, so the **allocation
granularity is at least 16 KB** — which is also the DART page size on t600x.

`AVE_Surface::CreateIOSurface(IOSurfaceRoot*, task*, int cacheMode,
int mapCacheAttr, u64 protOpts, u32 pixelFormat, int size, IOSurface** out)`
(`0xfffffe0008c6962c`) is just `CreateDict` +
`IOSurfaceRoot::createSurface(task, dict)` (`0xfffffe0008c697c0`), with
`size > 0` required (`cmp w26,#1; b.lt` at `0xfffffe0008c69680`; assertion
`pSurfaceRoot != nullptr && psTask != nullptr && size > 0 && ppSurface != nullptr`
at `0xfffffe00072a9336`).

The values actually used come from `AVE_SurfaceMgr::CreateSurface`
(`0xfffffe0008c80e30`), which calls `AVE_Surface::Create(IOSurfaceRoot*, task*,
u32, u32, u64, u32, int, u64, const char*, u64)` (`0xfffffe0008c6a180`) with:

| parameter | value | VA |
|---|---|---|
| `cacheMode` | `(flags & 0x10) ? 0 : 0x400` | `tst x23,#0x10` / `csel` `0xfffffe0008c80eb0`–`0xc80eb8` |
| `mapCacheAttr` | **2** | `mov w4,#2` `0xfffffe0008c80ed0` |
| `pixelFormat` | **0** | `mov w6,#0` `0xfffffe0008c80ed8` |
| `size` | caller's `size` (must be ≥ 1) | `cmp w26,#1; b.lt` `0xfffffe0008c80e7c` |
| `protectionOptions` | caller's arg 6 | `mov x5,x28` `0xfffffe0008c80ed4` |

`flags` here is the `gs_saAVE_SurfaceCfg` flags word from
`12-dart-surfaces-mmio.md` §2.1. `AVE_HwC::CreateFwHeap`
(`0xfffffe0008c1c7b4`) shows the wiring explicitly: `AVE_GetSurfaceCfg(30)` then
`ldp x3,x5,[x0]` (`0xfffffe0008c1c808`) passes `cfg->name` as the name and
`cfg->flags` as `flags`. Every entry in that table has bit 4 set, so **every
kext-allocated AVE surface uses `IOSurfaceCacheMode = 0`** (default/cacheable),
never `0x400`.

That same `AVE_HwC::CreateFwHeap` also rounds its size up to `PAGE_SIZE` and
then to 16 KB before calling (`0xfffffe0008c1c7d8`–`0xc1c818`,
`and w4,w20,#0xffffc000`), i.e. 16 KB alignment is applied twice.

`AVE_Surface::Create(IOSurface*, u64 flags, const char* name, u64 attr)`
(`0xfffffe0008c69b48`), the adopt-an-existing-surface path, does only
bookkeeping: it fills `+0x28` (name), `+0xE0` (`getAllocSize`), `+0x68`
(the surface), `+0xE8` (attr) and `+0x88` (`getSurfaceID`). If `flags & 8` it
additionally sets the surface's `kIOSurfaceName` property to
`"AVE2" " " <name>` (`0xfffffe0008c69ba8`–`0xc69c0c`, format `"%s%s%s"` at
`0xfffffe00072a9492`, `"AVE2"` at `0xfffffe00072a9499`) — a debug label, nothing
functional.

---

## 9. What a V4L2 driver must do

Confirmed, load-bearing:

1. Two planes for NV12-style input; the V pointer is the U pointer.
2. `bytesperline` (luma) and `bytesperline` (chroma) each a non-zero multiple of
   **64 bytes**.
3. Plane 1 starts at an arbitrary byte offset from the buffer base — AVE reads
   it from the surface descriptor, so a driver supplying its own descriptor is
   free to choose it (subject to the stride rule).
4. Buffer must map to device addresses through the IOMMU; allocation size
   rounded to 16 KB is what Apple does for everything it allocates itself.
5. `width * height <= 65 520 * 8192`.

Not established:

* Any height / plane-height alignment. `AVE_Enc_AlignDimension`'s ×16 rule is
  dead code in this kext build.
* Minimum frame dimensions actually enforced at runtime (the 192×96 / 160×64
  floors live only in the dead function).
* Whether the total buffer size must equal the `CalcFrameSize` result — the only
  size check on the input path is `iSize != 0`. (`CalcFrameSize` is a sibling
  work item.)
* The `IOSurfaceProtectionOptions` and `IOSurfaceMapCacheAttribute` value
  spaces; only the constant `2` for the latter was observed.
* What the `]` internal pixel-format family is for.
