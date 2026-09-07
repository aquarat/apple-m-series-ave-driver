# DART, surfaces and MMIO ranges

Everything below was read out of `AppleAVE2.kext` in `data/blobs/kc.macho`
(macOS 26.6.2, `kernelcache.release.mac13j`) with `tools/disas.py`. Every
constant carries the VA of the instruction or the data address it came from.
Anything not read directly out of the image is marked **inferred** or
**unknown**.

Terminology note: several tables below are indexed by an Apple *AVE IP
codename* (Rhea, Panda, Acis, Atlas, Erebus, …). The codename is not the SoC
name; the mapping used for M1 Max is established in
[§0](#0-device-identification) and is a *confirmed* lookup, not a guess.

---

## 0. Device identification

`AVE_DevInfo::RetrieveDevID` (`0xfffffe0008baad50`) reads the ADT property
**`soc-id`** (string at `0xfffffe000728f039`, referenced at
`0xfffffe0008baada4`) from the provider's `IORegistryEntry` and looks it up in a
34-entry table at **`0xfffffe0007edf0f0`** (loop at `0xfffffe0008baba48`).
Each entry is 16 bytes: `{const char *prefix; u32 number; u32 devid}`; the
lookup formats `"%s%d"` (`0xfffffe000728f395`) with prefix `"t"`
(`0xfffffe000728f39a`) and string-compares against `soc-id`.

Relevant rows (read from `0xfffffe0007edf0f0`):

| `soc-id` | `_E_AVE_DevID` |
|---|---|
| `t8103` | 10 |
| `t6000` | 11 |
| `t6001` | 12 |
| `t6002` | 13 |
| `t8112` | 15 |

`/arm-io/ave0` on j314c carries `soc-id = t6000`, so **`AVE_DevID = 11` on both
M1 Pro and M1 Max as far as the ADT is concerned** — the ADT reports `t6000`
even on a `t6001` part (already noted in `01-hardware.md`). Rows 11 and 12 point
at the same capability tables for everything examined here, so the distinction
does not matter below.

### Per-DevID capability dispatch

`AVE_DevCap_FindPixelFmt(_E_AVE_DevID, _E_AVE_ClientType, _E_AVE_EncType)`
(`0xfffffe0008baa1f0`) computes, at `0xfffffe0008baa274`–`0xfffffe0008baa2a8`:

```
row   = devid * 0x48                        ; 72 bytes per DevID
entry = *(void**)(0xfffffe0007edba00 + row + 0x18 + slot*8)
return *(void**)(entry + 32)
```

`slot` comes from the client/encoder type (`0xfffffe0008baa208`–`0xbaa274`):

| slot | selected by | offset in row |
|---|---|---|
| 3 | clientType 2 | `+0x30` |
| 4 | clientType 1, encType 1 | `+0x38` |
| 5 | clientType 1, encType 2 | `+0x40` |
| 0/1/2 | clientType 5 / 4 / 3 | `+0x18` / `+0x20` / `+0x28` |

Resolving the DevID-11/12 rows (`0xfffffe0007edbd18`, `0xfffffe0007edbd60`)
gives named symbols, which confirms slot 4 = AVC and slot 5 = HEVC:

| row slot | DevID 11 (t6000) | DevID 12 (t6001) |
|---|---|---|
| `+0x30` | `gsc_sAVE_DevCap_SEntry_LRME_6000` | `gsc_sAVE_DevCap_SEntry_LRME_6001` |
| `+0x38` | `gsc_sAVE_DevCap_SEntry_AVC_6000` | `gsc_sAVE_DevCap_SEntry_AVC_6001` |
| `+0x40` | `gsc_sAVE_DevCap_SEntry_HEVC_6000` | `gsc_sAVE_DevCap_SEntry_HEVC_6001` |

Each `SEntry` is 72 bytes of pointers (verified against the accessors at
`0xfffffe0008baa2a8`, `0xbaa36c`, `0xbaa430`):

| offset | contents | `SEntry_AVC_6001` (`0xfffffe0007edceb8`) | `SEntry_HEVC_6001` (`0xfffffe0007edcf00`) |
|---|---|---|---|
| `+0x18` | resolution limits | `Resolution_AVC_Nyx_C` | `Resolution_HEVC_Nyx_C` |
| `+0x20` | **pixel formats** | **`PixelFmt_AVC_Acis`** | **`PixelFmt_HEVC_Acis`** |
| `+0x28` | search range | `SearchRange_AVC_Castor` | `SearchRange_HEVC_Castor` |
| `+0x30` | perf table | `Perf_AVC_Nyx_C` | `Perf_HEVC_Nyx_C` |
| `+0x38` | throughput | `Throughput_AVC_Rhea` | `Throughput_HEVC_Uranus` |
| `+0x40` | DPM map | `DPMMap_AVC_Panda` | `DPMMap_HEVC_Panda` |

So **the pixel-format capability tables that apply to M1 Pro/Max are the
`_Acis` ones.** (The codenames are per-table, not per-SoC: t6001 mixes `Acis`
formats with `Nyx_C` resolutions and `Castor` search ranges.)

---

## 1. Pixel formats

This closes open question 8 in `04-roadmap.md`.

### 1.1 The master format table

`AVE_PixelFmt_GetNumOfTypes()` (`0xfffffe0008c4c694`) returns **86**.
`AVE_PixelFmt_FindByType(u32)` (`0xfffffe0008c4c660`) linearly scans a table at
**`0xfffffe0007268620`** (`__ZL29gs_sAVE_PixelFormatConversion`) with stride
**`0x2c` = 44 bytes**, comparing the first `u32` of each entry.

Field meanings recovered from `AVE_PixelFmt_CalcFrameSize`
(`0xfffffe0008c4c6a0`) and `AVE_PixelFmt_FindType` (`0xfffffe0008c4c7b8`):

| offset | meaning | evidence |
|---|---|---|
| `+0` | fourcc type | compared at `0xfffffe0008c4c678` |
| `+4` | bits per component (8, 10, 16) | `ldr w8,[x0,#4]` `0xfffffe0008c4c764`, `(bits+7)>>3` |
| `+8` | plane count (1 or 2) | value only; not used in `CalcFrameSize` |
| `+12` | chroma present (0 = mono) | `csel w9, wzr, w9, eq` `0xfffffe0008c4c71c` |
| `+16` | chroma horizontal divisor | `sdiv` at `0xfffffe0008c4c790` |
| `+20` | chroma vertical divisor | same |
| `+24` | **layout kind** (0..3) | switch at `0xfffffe0008c4c6b4` |
| `+28` | 0 = video range, 1 = full range | correlates with `v`/`f` in the fourcc |
| `+32` | `_E_AVE_LossyLevel` (0..3), `-1` if n/a | passed as arg 3 at `0xfffffe0008c4c748` |
| `+36`,`+40` | **unknown** (values 1,2,6,7,8,9,10) | not read by any function examined |

The `+24` layout kind selects the frame-size function:

| kind | handling | function |
|---|---|---|
| 0 | plain planar/biplanar | inline, `0xfffffe0008c4c760` |
| 1 | packed 10-bit (`p420` family), requires `+4 == 10` | inline, `0xfffffe0008c4c6c8` |
| 2 | tiled, `[` prefix | tail-call `AVE_HTPC_CalcFrameSize` (`0xfffffe0008b3fddc`) at `0xfffffe0008c4c7b4` |
| 3 | tiled + compressed, `&`/`-`/`/`/`\|` prefixes | tail-call **`AVE_Interchange_CalcFrameSize`** (`0xfffffe0008c2a894`) at `0xfffffe0008c4c75c` |

**AVE accepts the Interchange format.** The kext contains a full family of
`AVE_Interchange_*` size helpers (`0xfffffe0008c2a544`–`0xfffffe0008c2a894`),
and the `&`/`-`/`/`/`|` fourcc families in the master table dispatch to them
with `_E_AVE_LossyLevel` taken from field `+32`:

| fourcc prefix | `+24` kind | `+32` LossyLevel |
|---|---|---|
| `&` (0x26) | 3 | 0 |
| `-` (0x2D) | 3 | 1 |
| `/` (0x2F) | 3 | 2 |
| `\|` (0x7C) | 3 | 3 |
| `[` (0x5B) | 2 (HTPC, a different tiled scheme) | `-1` |

*Inferred:* `&` and `-` correspond to CoreVideo's `kCVPixelFormatType_Lossless_*`
and `_Lossy_*` private pixel formats; `/` and `|` are two further lossy levels.
The kext also imports `IOSurface::getTileWidthOfPlane`,
`getBytesPerTileHeaderOfPlane`, `getTileDataRegionOffsetOfPlane` etc. (undefined
symbols in `kext-symbols.txt`), which is consistent with the tiled-plus-metadata
layout these size functions compute.

### 1.2 What M1 Pro/Max actually accepts

`gc_sAVE_DevCap_PixelFmt_*` tables have layout `{u32 count; {u32 fourcc, u32
mask}[count]}` (established from `AVE_DevCap_PixelFmt_Find`,
`0xfffffe0008b3f19c`, and `_FindList`, `0xfffffe0008b3f1cc`; the match in
`_FindList` is `(query & ~mask) == 0` at `0xfffffe0008b3f1f8`). The mask
semantics are **unknown**.

**AVC — `gc_sAVE_DevCap_PixelFmt_AVC_Acis` @ `0xfffffe000723784c`, 14 entries:**

```
420v  420f  422v  422f  444v  444f  L008
&8v0  &8f0  &8v2  &8f2  &8v4  &8f4  &L08
```
(all mask `0x3e`)

**HEVC — `gc_sAVE_DevCap_PixelFmt_HEVC_Acis` @ `0xfffffe00072384e0`, 34 entries:**

```
420v 420f x420 xf20  422v 422f x422 xf22  444v 444f x444 xf44
L008 L010  p420 pf20 p422 pf22 p444 pf44
&8v0 &8f0 &xv0 &xf0  &8v2 &8f2 &xv2 &xf2  &8v4 &8f4 &xv4 &xf4
&L08 &L10
```
(all mask `0x3e`)

**LRME — `gc_sAVE_DevCap_PixelFmt_LRME_Acis` @ `0xfffffe0007236bb8`:** same 34
entries as HEVC, mask `0x38`.

Reading of the practical consequences:

- The **plain NV12-style `420v`/`420f` path exists on both codecs** — an
  ordinary V4L2 `NV12` capture buffer is directly acceptable. This is the path
  to use for first light.
- **Interchange is accepted, but only the lossless (`&`) variant** on this
  generation. The lossy `-`, `/`, `|` variants appear in the `Atlas`/`Erebus`
  tables (later IP revisions) but **not** in the `Acis` tables that t6000/t6001
  select. So zero-copy from a producer that emits lossless-compressed
  Interchange is possible in principle; lossy Interchange is not.
- The `[` (HTPC) family is **not** in any `Acis` table.
- 10-bit (`x`/`p` prefixes, `L010`) is HEVC-only on this generation; AVC is
  8-bit only.
- Monochrome (`L008`) is supported by both codecs.

### 1.3 "Internal" pixel formats

`AVE_Surface::CheckSupportedPixelFormat` (`0xfffffe0008c69a8c`) is:

```
return AVE_PixelFmt_FindByType(fmt) != NULL || CheckInternalPixelFormat(fmt);
```

`AVE_Surface::CheckInternalPixelFormat` (`0xfffffe0008c699dc`) accepts a
fourth prefix family, `]` (0x5D), which does **not** appear in the master
table. The accepted set is four bases plus offsets 0, 2 and 4 (mask `0x15`
tested at `0xfffffe0008c69a4c`, `0xc69a64`, `0xc69a7c`):

| base constant | VA of the `movz`/`movk` pair | ASCII | accepted values |
|---|---|---|---|
| `0x5D386630` | `0xfffffe0008c699f8` (`+0xa2c799d0`) | `]8f0` | `]8f0 ]8f2 ]8f4` |
| `0x5D786630` | `0xfffffe0008c6a018` (`+0xa28799d0`) | `]xf0` | `]xf0 ]xf2 ]xf4` |
| `0x5D387630` | `0xfffffe0008c69a0c` (`+0xa2c789d0`) | `]8v0` | `]8v0 ]8v2 ]8v4` |
| `0x5D787630` | `0xfffffe0008c69a2c` (`+0xa28789d0`) | `]xv0` | `]xv0 ]xv2 ]xv4` |

(The split between the `8` and `x` groups is the signed compare against
`0x5D78662F` at `0xfffffe0008c699e8`.) The trailing `0`/`2`/`4` is 4:2:0 /
4:2:2 / 4:4:4, matching the master table's naming. What the `]` family *is* is
**unknown** — it is only reachable through `CheckSupportedPixelFormat`, and no
size function for it was found.

### 1.4 Interchange geometry

Read out of the size helpers. These are the concrete numbers a driver would
need if it ever wants to import or produce Interchange buffers.

`AVE_Interchange_CalcLumaPlaneDataSize(w, h, lossyLevel, bitDepth)`
(`0xfffffe0008c2a554`):

```
tilesX = ceil(w / 32)          ; asr #5, 0xfffffe0008c2a598
tilesY = ceil(h / 32)          ; asr #5, 0xfffffe0008c2a5a8
size   = tilesX * tilesY * bytesPerTile[lossyLevel]
```
→ **luma tile is 32x32 pixels.**

`AVE_Interchange_CalcChromaPlaneDataSize(...)` (`0xfffffe0008c2a6f8`) uses
`asr #4` at `0xfffffe0008c2a73c`/`0xc2a74c` → **chroma tile is 16x16 pixels.**

Bytes per tile, selected by bit depth (`cmp w3,#8` at `0xfffffe0008c2a56c`) and
indexed by lossy level:

| table | VA | level 0 | 1 | 2 | 3 |
|---|---|---|---|---|---|
| luma, 8-bit | `0xfffffe00072670f0` | `0x400` | `0x300` | `0x280` | `0x200` |
| luma, non-8-bit | `0xfffffe0007267100` | `0x500` | `0x400` | `0x300` | `0x280` |
| chroma, 8-bit | `0xfffffe0007267110` | `0x200` | `0x180` | `0x100` | `0x100` |
| chroma, non-8-bit | `0xfffffe0007267120` | `0x280` | `0x200` | `0x180` | `0x180` |

Metadata regions (`CalcLumaPlaneMetaSize` `0xfffffe0008c2a5f4`,
`CalcChromaPlaneMetaSize` `0xfffffe0008c2a75c`):

```
e    = ceil(log2(tilesX)) + ceil(log2(tilesY))   ; via clz, 0xc2a61c / 0xc2a634
size = align_up(K << e, 128)                     ; and #0xffffff80
```
with `K = 32` for luma (`0xfffffe0008c2a620`) and `K = 8` for chroma
(`0xfffffe0008c2a7b0`). So the metadata region is 32 (luma) / 8 (chroma) bytes
per tile with both tile counts rounded up to powers of two, then padded to 128
bytes.

`AVE_Interchange_CalcChromaSize` (`0xfffffe0008c2a7c4`) divides w/h by a
`_E_ChromaFmt`-indexed pair of `u32` divisors at **`0xfffffe0007276400`**
(stride 8); the first three pairs read `(1,1)`, `(2,2)`, `(2,1)` — 4:4:4, 4:2:0,
4:2:2 respectively (**inferred** from the values).

For comparison, `AVE_HTPC_CalcFrameSize` (`0xfffffe0008b3fddc`) uses
`ceil(w/16) x ceil(h/8)` tiles of 128 bytes per byte-of-sample
(`lsl #7` at `0xfffffe0008b3feb8`) — a different, uncompressed tiling.

---

## 2. Surfaces and buffers

### 2.1 Surface kinds

`AVE_GetSurfaceCfg(_E_AVE_SurfaceIdx)` (`0xfffffe0008ca8cfc`) indexes
`gs_saAVE_SurfaceCfg` at **`0xfffffe0007ee1050`**, stride **16 bytes**, valid
indices `0 .. 0x28` (bound check at `0xfffffe0008ca8d20`). Each entry is
`{const char *name; u32 flags; u32 pad}`. The full enum, read from the table:

| # | name | flags | # | name | flags |
|---|---|---|---|---|---|
| 0 | `InputData` | `0x10510` | 21 | `SrcNeighborInfo` | `0x10518` |
| 1 | `InputScaledData` | `0x10510` | 22 | `SrcNeighborPixel` | `0x10518` |
| 2 | `DirectRecon` | `0x10510` | 23 | `SrcNeighborData` | `0x10518` |
| 3 | `UCInfo` | `0xd18` | 24 | `SrcNeighborFwData` | `0x10518` |
| 4 | `MultiPassStats` | `0x10518` | 25 | `TranscodedData` | `0x10518` |
| 5 | `MBInputCtrl` | `0x10d18` | 26 | `EntropyCoding` | `0x10518` |
| 6 | `Recon` | `0x10518` | 27 | `IOPIPC` | `0x10518` |
| 7 | `Link` | `0x10d08` | 28 | `FwImage` | `0x10d18` |
| 8 | `CodedData` | `0x518` | 29 | `FwLog` | `0x10d18` |
| 9 | `CodedHeader` | `0x10d18` | 30 | `FwHeap` | `0x10518` |
| 10 | `SliceHeader` | `0x10d18` | 31 | `FwIPC` | `0x10d18` |
| 11 | `ProtectedData` | `0x10d18` | 32 | `FwClient` | `0x10518` |
| 12 | `MBStats` | `0x10d18` | 33 | `FwClientMem` | `0x10518` |
| 13 | `StaticAreaQPModInfo` | `0x10518` | 34 | `InitParamsCopy` | `0xd18` |
| 14 | `StaticAreaCBP0Cntr` | `0x10518` | 35 | `MCTFOutput` | `0x10518` |
| 15 | `Colocated` | `0x10518` | 36 | `MCTFRef` | `0x10518` |
| 16 | `HSCOutput` | `0x10518` | 37 | `GGMRef` | `0x10518` |
| 17 | `LFSRef` | `0x10518` | 38 | `GGMStats` | `0x10518` |
| 18 | `LRSNeighborMV` | `0x10518` | 39 | `GGMOutput` | `0x10518` |
| 19 | `LFSResult` | `0x10518` | 40 | `DMVOutput` | `0x10518` |
| 20 | `LRSResult` | `0x10518` | | | |

The meaning of the flags word is **unknown**; it is presumably an
`IOMemoryDescriptor`/allocation option set. Note that `InputData`,
`InputScaledData` and `DirectRecon` are the only three that lack bit 3
(`0x10510` vs `0x10518`), and that `FwImage`/`FwLog`/`FwIPC`/`CodedHeader`/
`SliceHeader`/`MBStats`/`MBInputCtrl`/`ProtectedData` share `0x800`.

### 2.2 Where reference and reconstructed frames come from

Surfaces are split three ways by the helper functions (names from
`kext-symbols.txt`, all in the `AVE_SurfaceMgr` orbit):

- **External** — `AVE_CreateExternalInSurfaces` / `...OutSurfaces`
  (`_S_AVE_SurfaceIDInSet`): built from IOSurface IDs handed in by userspace.
  These are the client's input frames and output bitstream buffers.
- **Internal** — `AVE_CreateInternalSurfaces(AVE_SurfaceMgr*, task*,
  _S_AVE_SurfaceInfoSet*, u64, u64, _S_AVE_SurfaceSet*)`: allocated by the
  kext. This is where `Recon`, `Colocated`, `LFSRef`, `MCTFRef`, `GGMRef`,
  `MultiPassStats` etc. live — **reconstructed and reference frames are kernel
  allocations, not client buffers.**
- **Data** — `AVE_CreateDataSurfaces` / `AVE_Client_CreateDataSurfaces`:
  per-frame side data.

Sizes are computed per engine before allocation, by
`AVE_Work_Enc_CalcSurfaceInfo`, `AVE_Work_LRME_CalcSurfaceInfo`,
`AVE_Work_MCTF_CalcSurfaceInfo`, `AVE_Work_DMV_CalcSurfaceInfo`,
`AVE_Work_GGM_CalcSurfaceInfo`, `AVE_Work_MSC_CalcSurfaceInfo`, each of which
fills a `_S_AVE_SurfaceInfoSet`. **These were not disassembled** — that is where
the actual per-surface size formulas live, and it is the obvious next piece of
static work.

`AVE_DPB` (27 methods) is pure bookkeeping: it manipulates `_S_AVE_DPB_Set`
entries and `AVE_Surface*` handles (`AVE_SVEDPB::Init` takes
`AVE_Surface*` arrays). It does not allocate. `AVE_BufPool` and `AVE_BlkPool`
are generic sub-allocators over an already-allocated region —
`AVE_BlkPool::CreateWithMem(u32, size_t, int, int, int, int)` and
`CreateInMem(...)` with `Alloc(size_t*, int*)` / `Free(size_t)` /
`Addr2Idx` / `Idx2Addr` — i.e. index/offset arithmetic inside a buffer, not
IOMemory allocation.

`AVE_Surface` accessors come in linear and per-plane pairs: `GetSize` /
`GetYUVSize`, `GetKernelAddr` / `GetYUVKernelAddr`, `GetPhyAddr` /
`GetYUVPhyAddr`, `GetDARTAddr` / `GetYUVDARTAddr`, plus `GetYUVStride`
(`0xfffffe0008c6e72c`). The `YUV` variants take **three** out-pointers
(`x1`, `x2`, `x3`; the "all three null" guard is at
`0xfffffe0008c6e758`–`0xc6e760`), so up to three planes are addressed
independently even though every YUV entry in the master format table declares
`plane count = 2`.

`AVE_Surface::GetLinearBuf` (`0xfffffe0008c6e9e4`) and `GetCompressedBuf`
(`0xfffffe0008c6ece4`) exist as separate accessors, consistent with the
compressed formats carrying a second (metadata) region.

**Not determined:** stride/height alignment requirements for the plain
`420v`/`420f` path. Those are computed inside
`AVE_Work_Enc_CalcSurfaceInfo` and `AVE_Surface::Create`, neither of which was
disassembled here.

---

## 3. DART / IOMMU

### 3.1 The kext does not touch DART registers

`AVE_DART` never maps or writes DART MMIO. It holds an IOKit mapper object at
`this+0x18` (assertion string `"m_pcMapper != nullptr"` at
`0xfffffe000728e42c`, referenced from the failure path of
`AVE_DART::GetMapper`, `0xfffffe0008ba65c4`, which tests `ldr x8,[x0,#24]` at
`0xfffffe0008ba65e0`).

`AVE_DART::Init` (`0xfffffe0008ba5744`) obtains that object at
`0xfffffe0008ba58bc`–`0xc58c4` from its first argument and stores it at
`this+0x18`, then at `0xfffffe0008ba5b60`–`0xba5ba0` calls virtual slot
`+0x6f0` on it with a string argument at `0xfffffe000728e489`, which is
**`"setActive"`**. `AVE_DART::SetMapCacheAttr` (`0xfffffe0008ba8acc`) calls the
same slot (`0xfffffe0008ba8d28`–`0xba8d68`) with the string at
`0xfffffe000728ea51`, **`"setIomdCacheAttribute"`**.

*Inferred* (high confidence, from the six-argument shape `(name, 0, p1, p2, p3,
p4)`): slot `+0x6f0` is `IOService::callPlatformFunction`. The mapper is the
`mapper-ave0` node's service.

The actual mapping is done with what is unmistakably `IODMACommand`.
`AVE_DART::Map` (`0xfffffe0008ba6bb8`):

- `0xfffffe0008ba6cb8` — `AVE_DART::GetEntry(&entry)` takes a free pool entry;
- `0xfffffe0008ba6cd8`–`0xba6d18` — virtual slot `+0x148` on `entry[+0x18]`,
  called with `(memoryDescriptor, 1)`;
- `0xfffffe0008ba6d48`–`0xba6d64` — virtual slot `+0x178` on the same object,
  called with three stack out-pointers (`&offset`, `&segments`, `&numSegments`).

*Inferred:* `IODMACommand::setMemoryDescriptor(md, autoPrepare=true)` followed
by `gen64IOVMSegments(&offset, segments, &numSegments)`.

**Consequence for Linux: nothing here needs replicating.** Everything AVE does
to the IOMMU is the standard "map this memory descriptor, give me device
addresses" operation, which the existing `apple-dart` driver plus the ordinary
DMA API already provide. `01-hardware.md`'s expectation is confirmed.

### 3.2 Cache attributes

`AVE_DART::SetMapCacheAttr(this, iomd, attr)` matches `attr` against a 2-entry,
16-byte-stride table at **`0xfffffe000c69a960`** (`__DATA`; loop at
`0xfffffe0008ba8b74`–`0xba8b98`). Contents:

| `attr` | name string | string VA |
|---|---|---|
| 1 | `iomdEarlyReclaim` | `0xfffffe000728ec13` |
| 2 | `iomdEarlyPurge` | `0xfffffe000728ec24` |

Anything else returns `-1002` (`0xfffffe0008ba8b9c`). The matched name is
passed to `callPlatformFunction("setIomdCacheAttribute", …)`. These are
**IOMemoryDescriptor lifetime hints, not DART page-table cache attributes** —
there is no equivalent to configure on the Linux side, and no page attribute
(cacheable/device/etc.) is being selected here.

### 3.3 Pools

There is **one address space**. "Pools" are pools of *mapping descriptors*, not
address spaces:

`AVE_DART::AddPool(this, count)` (`0xfffffe0008ba6124`) calls a constructor at
`0xfffffe0008ba32c8` with `(count, this->mapper, &pool)`
(`0xfffffe0008ba61fc`–`0xba6208`), links the result onto a list at `this+0x668`
via `AVE_MList_Add` (`0xfffffe0008ba6294`–`0xba629c`), and adds `count` to a
running total at `this+0x660` (`0xfffffe0008ba62a4`–`0xba62ac`). The allocation
site tag is the string `"site._S_AVE_DART_Pool"` (`0xfffffe000728ec33`). The
constructor allocates `count * 40` bytes of entries (`mov w8, #0x28` at
`0xfffffe0008ba33bc`, `umulh` overflow check at `0xba33c0`).

`GetEntry` / `PutEntry` (`0xfffffe0008ba738c` / `0xfffffe0008ba7794`) take and
return entries from that free list. `DeletePool` / `DeletePools` tear them down.

So: **no multiple address spaces, no per-pool DART configuration.** Pools exist
purely so that a mapping can be set up without allocating an `IODMACommand` on
the hot path.

`AVE_DART::Flush` (`0xfffffe0008ba86e0`), `SetActive` (`0xfffffe0008ba82b4`),
`CheckErrorType` / `ErrorHandler` / `InstallErrorHandler` were not disassembled
in detail; `SetActive`'s platform-function name (`"setActive"`) is confirmed
above from `Init`.

`AVE_DART::RetrieveDARTInfo(u32, u64*, s64*, u64*)` and
`RetrieveMapperInfo(u32, int*)` (`0xfffffe0008ba4c94`) query the mapper for
address-space geometry; the values were not extracted.

---

## 4. MMIO ranges

This corrects `01-hardware.md`, which guessed the roles from sizes.

### 4.1 How the ranges are mapped

`AVE_Reg::Init(AVE_Reg*, IOService *provider, AVE_DevInfo*, u32, void*, u32
nRanges)` at **`0xfffffe0008c532d0`** is the only place any of the five ranges
is mapped.

- `nRanges` is validated to be in `1..6` (`sub w8,w24,#1` / `cmp w8,#6` /
  `b.cs` at `0xfffffe0008c5331c`–`0xc53324`);
- then, at `0xfffffe0008c53328`–`0xc5333c`:
  `n = (AVE_DevInfo::GetDevType() == 0x1e) ? nRanges : 5` — **unless the device
  type is 30, exactly five ranges are mapped**, which is exactly the number
  `/arm-io/ave0` declares;
- the loop at `0xfffffe0008c53348`–`0xc533ac`, for `i` in `0 .. n-1`:
  - calls provider virtual slot `+0x710` with `(i, 0)` and stores the result at
    `this + 0x10 + 8*i` (`0xfffffe0008c53370`–`0xc53374`) — *inferred*
    `IOService::mapDeviceMemoryWithIndex(index, options)`;
  - calls virtual slot `+0x138` on that result and stores it at
    `this + 0x40 + 8*i` (`0xfffffe0008c53398`–`0xc533a0`) — *inferred*
    `IOMemoryMap::getVirtualAddress()`.

The accessors confirm the layout. `AVE_Reg::Read32(this, bank, byteOffset)`
(`0xfffffe0008c53df0`):

```
cmp   w1, #5 ; b.hi -> return 0        ; 0xfffffe0008c53df4
x8 = this + 0x40
x8 = [x8 + bank*8]                     ; 0xfffffe0008c53e04
w0 = *(u32 *)(x8 + offset)             ; 0xfffffe0008c53e08
```

`Read64` (`0xfffffe0008c53e24`), `Write32` (`0xfffffe0008c53e58`) and `Write64`
(`0xfffffe0008c53e88`) are identical in form.

**The `bank` argument is the ADT `reg` index.** `AVE_HwC::Init` passes
`nRanges = 6` (`mov w5, #6` at `0xfffffe0008c1ac0c`, immediately before the
call to `AVE_Reg::Init` at `0xfffffe0008c1ac10`).

### 4.2 Which bank is what

Established by cross-referencing every `BL` to the four accessors across the
kext's `__TEXT_EXEC` and reading the immediate `bank` argument at each site:

| bank | ADT `reg` (ave0) | Used by | Confidence |
|---|---|---|---|
| 0 | `0x20D100000` + `0x45C000` | `AVE_DPE::Enable` / `Disable` / `ApplyTunables` | **Confirmed** |
| 1 | `0x20D800000` + `0x800000` | `AVE_IOP_Config_*`, `AVE_IOP_Start_*`, `AVE_IOP_CheckIdle_*` — the **RTKit/ASC coprocessor control block** | **Confirmed** |
| 2 | `0x20D050000` + `0x8000` | no call site with an immediate bank index found | **Unknown** |
| 3 | `0x8E588000` + `0x24` | no call site with an immediate bank index found | **Unknown** |
| 4 | `0x20C000000` + `0x1000000` | `AVE_AXI2AF::ReadReg` / `WriteReg` | **Confirmed** |
| 5 | *(does not exist on ave0)* | `AVE_HwC::Init` revision read | see below |

Evidence per row:

- **bank 0** — `AVE_DPE::Enable` `mov w1,#0` at `0xfffffe0008bca794` before
  `bl 0xfffffe0008c53df0`, and `0xfffffe0008bca7ac` before
  `bl 0xfffffe0008c53e58`; `AVE_DPE::ApplyTunables` likewise at
  `0xfffffe0008bc9f50` / `0xfffffe0008bc9f70`. The offset comes from the
  `_S_AVE_DPE_RegCfg` entry plus a base argument
  (`add w2, w8, w20` at `0xfffffe0008bc9f4c`).
- **bank 1** — see §4.3.
- **bank 4** — `AVE_AXI2AF::ReadReg(this, const _S_AVE_AXI2AF_Field*)`
  (`0xfffffe0008b5c9e8`): `ldr x0,[x0,#16]` (the `AVE_Reg*`),
  `ldr w2,[x1]` (the field's register offset), `mov w1,#4`
  (`0xfffffe0008b5ca04`), then `bl Read32`. The result is masked and shifted
  with `[x1+4]` and `[x1+8]` (`0xfffffe0008b5ca0c`–`0xb5ca14`), so
  `_S_AVE_AXI2AF_Field = {u32 offset; u32 mask; u32 shift}`. `WriteReg`
  (`0xfffffe0008b5ca24`) does read-modify-write on the same bank
  (`0xfffffe0008b5ca70`).
- **bank 5** — `AVE_HwC::Init` at `0xfffffe0008c1bf00`–`0xc1bf0c`:
  `Read32(reg, 5, 0x9c000)`, result passed straight to
  `AVE_DevInfo::SetDevRevision` (`0xfffffe0008c1bf18`). Bank 5 only exists when
  six ranges were mapped, i.e. when `GetDevType() == 0x1e`. **On `ave0`, which
  declares five ranges, this path must be guarded** — the guard was not
  located, so treat "the revision register is at bank 5 + `0x9c000`" as
  applying to six-range parts only.

`AVE_MCC` does **not** use `AVE_Reg` at all. `AVE_MCC::Init`
(`0xfffffe0008c47250`) looks up the string **`"function-mcc_dataset"`**
(`0xfffffe00072a49e0`, referenced at `0xfffffe0008c472fc`) on its provider and
stores the resulting object at `this+0x20`. That is the ADT property present on
`/arm-io/ave0` (`function-mcc_dataset`, phandle 129). So the memory-controller
tuning path goes through a *different node's* platform function, **not** through
one of AVE's own `reg` ranges — which removes the `01-hardware.md` guess that
`reg[3]` is the `mcc_dataset` window. What `reg[3]` (36 bytes at `0x8E588000`)
actually is remains **unknown**.

### 4.3 The coprocessor control block (bank 1)

`AVE_IOP_{Config,Start,CheckIdle}_<codename>` exist for Rhea, Panda, Pan, Nyx,
Castor, Acis, Atlas, Hypnos, Tethys, Nemesis, Hera, Janus, Themis, Gaia,
Uranus, Upis, Ersa, Erebus, Aion, Ares and Leto. **Acis and Atlas use identical
offsets** (verified below), which removes the need to know exactly which variant
t6001 dispatches to.

`AVE_IOP_Start_Acis` (`0xfffffe0008c2abd0`) / `AVE_IOP_Start_Atlas`
(`0xfffffe0008c2b228`), in order:

| # | op | bank | offset | value | VA (Acis / Atlas) |
|---|---|---|---|---|---|
| 1 | `Write32` | 1 | `0x400808` | `1` | `0xfffffe0008c2abe8` / `0xfffffe0008c2b2dc` |
| 2 | `Write32` | 1 | `0x400044` | `0` | `0xfffffe0008c2ac04` / `0xfffffe0008c2b2f8` |
| 3 | `Write32` | 1 | `0x400400` | `0x10000` | `0xfffffe0008c2ac14` / `0xfffffe0008c2b308` |
| 4 | `Write32` | 1 | `0x400044` | `0x10` | `0xfffffe0008c2ac30` / `0xfffffe0008c2b324` |

(`0x400044` is assembled as `mov w20,#0x44; movk w20,#0x40,lsl #16` at
`0xfffffe0008c2abe0`/`0xfffffe0008c2b2d4`; `0x400808` and `0x400400` are that
base plus `0x7c4` and `0x3bc`.)

`AVE_IOP_CheckIdle_Acis` (`0xfffffe0008c2ada0`) / `_Atlas`
(`0xfffffe0008c2b3dc`):

```
v = Read32(bank 1, 0x400048)           ; 0xfffffe0008c2adac / 0xfffffe0008c2b494
idle = ((v & 3) == 0)                  ; 0xfffffe0008c2adbc / 0xfffffe0008c2b4a4
```

`AVE_IOP_Config_Acis(AVE_Reg*, u64 fwBase)` (`0xfffffe0008c2a9d0` area) /
`_Atlas`:

```
Read64 (bank 1, 0x50000)                                   ; 0xfffffe0008c2a9e4 / 0xc2b0dc
Write64(bank 1, 0x50000,
        (fwBase & 0x3fffffff800) | 0x0102000000000000)     ; 0xfffffe0008c2aa60-0xc2aa7c
                                                           ; / 0xc2b158-0xc2b170
```

Absolute addresses for `ave0` (`reg[1]` base `0x20D800000`):

| register | absolute address | role |
|---|---|---|
| `+0x050000` | `0x20D850000` | 64-bit; firmware base address, bits 11..41, with `0x0102` in bits 48..63 |
| `+0x400044` | `0x20DC00044` | CPU control — write 0, then `0x10` to run |
| `+0x400048` | `0x20DC00048` | CPU status — low 2 bits are the busy indication |
| `+0x400400` | `0x20DC00400` | written `0x10000` between the two control writes |
| `+0x400808` | `0x20DC00808` | written `1` first |

*Inferred, but strongly:* `+0x400044` / `+0x400048` with the `0x10` "run" bit
are the standard Apple ASC `CPU_CONTROL` / `CPU_STATUS` pair. If that holds,
the ASC block on `ave0` sits at **`0x20DC00000`**, i.e. `reg[1] + 0x400000`,
and the mailbox is in the same 4 MB half.

This **contradicts** `01-hardware.md`, which nominated `reg[2]`
(`0x20D050000` + `0x8000`) as the ASC on size grounds. `reg[2]` has no
identified user in the kext at all. Open question 4 in `04-roadmap.md` should be
re-pointed at `0x20DC00000`.

### 4.4 AXI2AF tunables

The kext ships `gsc_saAVE_AXI2AF_RegCfg_Default_<codename>_<number>` tables for
Hypnos_8310, Leto_8320, Hera_6020/6021/6022, Tethys_8120, Nemesis_8122,
Janus_6030/6031/6032/6034, Themis_8130/8132, Gaia_6040/6041,
Uranus_8140, Upis_8142, Ersa_6050, Erebus_8150, Aion_8152 and Ares_8160 —
plus matching `RegParity` tables for the newer ones. Note that **no table exists
for 6000/6001/6002 or 8103**, so it is likely (but **not confirmed**) that
`AVE_AXI2AF::ApplyTunables` is a no-op on M1-family parts and that `reg[4]`
(`0x20C000000`, 16 MB) may not need programming at all for bring-up.

---

## 5. Summary of corrections to existing docs

- `01-hardware.md` "Likely role" table: **`reg[1]` is the coprocessor control
  block, not `reg[2]`**; `reg[0]` carries DPE; `reg[4]` carries AXI2AF; `reg[3]`
  is *not* the mcc_dataset window (that goes through a platform function on
  another node); `reg[2]` and `reg[3]` remain unidentified.
- `04-roadmap.md` open question 4 ("Which `reg` range is the ASC?"): answered —
  `reg[1]`, at `+0x400000`.
- `04-roadmap.md` open question 8 ("Does AVE accept Interchange?"): **yes**, via
  `AVE_Interchange_*`, but on M1 Pro/Max only the lossless (`&`) variant.
  Plain `420v`/`420f` is also accepted, so an NV12 path exists as expected.
- `01-hardware.md` DART section: confirmed. The kext drives the DART only
  through an IOKit mapper service and `IODMACommand`; there is nothing for a
  Linux driver to do beyond ordinary DMA mapping.

## 6. What is still open

- **Surface size/alignment formulas.** `AVE_Work_Enc_CalcSurfaceInfo` and its
  siblings were not disassembled. Stride and height alignment for the plain
  `420v` path, and the sizes of `Recon` / `Colocated` / `MCTFRef` / `GGMRef`,
  live there.
- **`reg[2]` (`0x20D050000`, 32 KB) and `reg[3]` (`0x8E588000`, 36 bytes).**
  No call site passes an immediate bank 2 or 3 to `AVE_Reg`. They may be reached
  via `AVE_RegCfg_Apply` (`0xfffffe0008c54618`), which takes the bank out of a
  `_S_AVE_RegCfg` structure rather than an immediate — that structure was not
  chased.
- **`_S_AVE_PixelFmt` fields `+36` and `+40`.** Present in all 86 entries, read
  by nothing that was examined. Possibly the code the firmware expects on the
  wire.
- **The `]` internal pixel-format family.** Accepted by
  `CheckInternalPixelFormat` but absent from the master table and from every
  `DevCap` table.
- **The DevCap format mask values** (`0x3e`, `0x3fe`, `0x38`, `0x3c0`, `0x3c00`,
  `0x4000`). Matched with `(query & ~mask) == 0`; the bit meanings are unknown.
- **The `gs_saAVE_SurfaceCfg` flags word.**
- **Whether `AVE_AXI2AF` does anything on M1.** No `RegCfg_Default` table exists
  for 6000/6001/6002.
- **The bank-5 revision register guard.** `AVE_HwC::Init` reads bank 5 offset
  `0x9c000`, which cannot exist on a five-range part; the conditional that
  protects it was not located.
