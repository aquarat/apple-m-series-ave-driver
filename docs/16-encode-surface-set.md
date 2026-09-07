> ## Verification note
>
> Re-checked against the binaries before committing:
>
> - **`_S_AVE_SurfaceInfoSet` = 35 x 48 bytes confirmed.** `mov w1, #0x690` then
>   `bl bzero` at `0xfffffe0008ca9a64`; 1680/48 = 35 exactly.
> - **`gs_saAVE_SurfaceCfg` has no size field — confirmed, and the cross-check
>   this analysis was given was wrong.** The table spans `0xfffffe0007ee1050`..
>   `0xfffffe0007ee12e0` = 656 bytes over 41 entries = **16 bytes each**:
>   `{name_ptr, u32 flags, u32 pad}`. `FwIPC` is index 31 with flags `0x00010d18`
>   and no size. The "index 31, size `0x1400000`" cross-check came from
>   conflating this table with the 20 MiB figure that [08-ipc-transport.md](08-ipc-transport.md)
>   read from *code* at `0xfffffe0008c426ec`. The analysis was right to reject
>   the supplied value rather than accommodate it.
> - **No `_E_ChromaFmt` conflict with [12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md).**
>   This document reports one; there isn't one. `docs/12` says `+12` is "chroma
>   present (0 = mono)", which agrees. The divisor table at `0xfffffe0007276400`
>   reads `(1,1) (2,2) (2,1) (1,1)` for indices 0..3, matching
>   400/420/422/444 as named in the string array
>   ([14-frame-size-formulas.md](14-frame-size-formulas.md)). This document's own
>   observation — index 0 gives zero chroma bytes, index 1 gives NV12 at
>   128 B/MB — is a third independent confirmation of the same ordering.

# The encode surface set

*Which buffers must exist to encode one H.264 frame at W x H, how many of each,
how big, and who allocates them.*

Everything below was read out of `AppleAVE2.kext` in `data/blobs/kc.macho`
(macOS 26.6.2, `kernelcache.release.mac13j`) with `tools/disas.py`. Every
constant carries the VA of the instruction it came from. Anything not read
directly out of an instruction is marked **inferred** or **unknown**.

This document covers the *surface set*: the identity, count and allocator of
each buffer. The per-pixel-format frame-size arithmetic
(`AVE_PixelFmt_CalcFrameSize` and friends) and the `AVE_Surface` plane/stride
accessors are covered elsewhere.

Reproduce any line with:

```sh
python3 tools/disas.py --kext --addr 0xfffffe0008ca9a28 -n 0xa30   # CalcSurfaceInfo
python3 tools/disas.py --kext --addr 0xfffffe0008c79894 -n 0x900   # CreateInternalSurfaces
```

---

## 1. `_S_AVE_SurfaceInfoSet` — the layout

`AVE_Work_Enc_CalcSurfaceInfo(_S_AVE_Client*, _S_AVE_DLB_Unit*,
_S_AVE_SurfaceInfoSet*)` at `0xfffffe0008ca9a28` opens with

```
fffffe0008ca9a60:  mov  x0, x2          ; the SurfaceInfoSet
fffffe0008ca9a64:  mov  w1, #0x690      ; 1680
fffffe0008ca9a68:  bl   0xfffffe000b6e8d80
```

so **`sizeof(_S_AVE_SurfaceInfoSet) = 0x690 = 1680`**. (`0xb6e8d80` is a
two-argument `(ptr, len)` kernel routine, i.e. `bzero`; *inferred*, but the
struct is written field-by-field immediately afterwards, which is consistent.)

Every access to the set in every function examined is at an offset that is a
multiple of 48 plus a small field offset, so the set is **35 entries of 48
bytes** (35 x 48 = 1680 exactly).

### 1.1 Entry fields

The field meanings come from matching the writer (`AVE_Work_Enc_CalcSurfaceInfo`,
whose callees are all named `AVE_CalcBuf<Field>Of<Surface>`) against the reader
(`AVE_CreateInternalSurfaces`, `0xfffffe0008c79894`).

| offset | field | evidence |
|---|---|---|
| `+0x00` | `u64 flags` | `ldr x8,[x26,#192]` `0xfffffe0008c79904`, then `orr` with the `gs_saAVE_SurfaceCfg` flags word and `and #~0x10000` at `0xfffffe0008c79910` |
| `+0x08` | `BufModeNum` | `AVE_CalcBufModeNumOfLFSResult` -> `+872` `0xfffffe0008caa188` |
| `+0x0c` | `BufTypeNum` | `AVE_CalcBufTypeNumOfLFSRef` -> `+780` `0xfffffe0008caa0e0` |
| `+0x10` | `BufSetNum` | `AVE_CalcBufSetNumOfRecon` -> `+256` `0xfffffe0008ca9f78` |
| `+0x14` | `BufLayerNum` | `AVE_CalcBufLayerNumOfRecon` -> `+260` `0xfffffe0008ca9f84` |
| `+0x18` | `BufNum` (count) | `AVE_CalcBufNumOfRecon` -> `+264` `0xfffffe0008ca9fdc`; used as the loop bound in `AVE_CreateInternalSurfaces` (`cmp x19,x20` `0xfffffe0008c79950`) |
| `+0x1c` | `BufSize` (bytes) | `AVE_CalcBufSizeOfRecon` -> `+268` `0xfffffe0008caa014`; passed as the `size` argument of `AVE_SurfaceMgr::CreateSurface` and compared against `AVE_Surface::GetSize` at `0xfffffe0008c79968` |
| `+0x20`..`+0x2c` | four extra `u32` | out-parameter of the `...Pi` size functions; for `Recon`: `{lumaData, lumaMeta, chromaData, chromaMeta}` (`stp w26,w19,[x8]` / `stp w28,w27,[x8,#8]` at `0xfffffe0008b60d34`) |

`AVE_PrintSurfaceInfoSet` (`0xfffffe0008c7454c`) corroborates this: its format
strings are positional, not named, but the *shape* matches exactly — e.g. for
`MBInputCtrl` it prints `"%s 0x%llx %d | %d"` (`0xfffffe00072aa9d7`) from
`+0x00`, `+0x18`, `+0x1c` (`0xfffffe0008c745d4`, `0xfffffe0008c745dc`), and for
`Recon` it prints `"%s 0x%llx %d %d %d | %d | %d %d %d %d"`
(`0xfffffe00072aaa0d`) from `+0x00`, `+0x10/+0x14/+0x18`, `+0x1c`,
`+0x20..+0x2c` (`0xfffffe0008c74798`–`0xc747bc`).

```c
struct _S_AVE_SurfaceInfo {      /* 48 bytes */
    uint64_t flags;              /* +0x00 */
    uint32_t modeNum;            /* +0x08 */
    uint32_t typeNum;            /* +0x0c */
    uint32_t setNum;             /* +0x10 */
    uint32_t layerNum;           /* +0x14 */
    uint32_t num;                /* +0x18 */
    uint32_t size;               /* +0x1c */
    uint32_t extra[4];           /* +0x20 */
};
struct _S_AVE_SurfaceInfoSet { struct _S_AVE_SurfaceInfo e[35]; }; /* 0x690 */
```

Total buffers of one kind = `setNum * layerNum * num` (the three nested loops in
`AVE_CreateInternalSurfaces`, `0xfffffe0008c79a5c`–`0xc79b20`); `modeNum` and
`typeNum` add further nesting for `LFSResult`/`LRSResult`/`LFSRef`.

### 1.2 Slot index is *not* `_E_AVE_SurfaceIdx`

The InfoSet slot number and the `_E_AVE_SurfaceIdx` used by
`AVE_GetSurfaceCfg` are two different enumerations. The mapping was recovered
by pairing each `mov w0,#idx; bl AVE_GetSurfaceCfg` with the InfoSet offset
used immediately afterwards, across all four `AVE_Create*Surfaces` functions:

| slot | byte off | `_E_AVE_SurfaceIdx` | name |
|---:|---:|---:|---|
| 0 | 0 | 0 | `InputData` |
| 1 | 48 | 1 | `InputScaledData` |
| 2 | 96 | 2 | `DirectRecon` |
| 3 | 144 | 4 | `MultiPassStats` |
| 4 | 192 | 5 | `MBInputCtrl` |
| 5 | 240 | 6 | `Recon` |
| 6 | 288 | 7 | `Link` |
| 7 | 336 | 8 | `CodedData` |
| 8 | 384 | 9 | `CodedHeader` |
| 9 | 432 | 10 | `SliceHeader` |
| 10 | 480 | 11 | `ProtectedData` |
| 11 | 528 | 12 | `MBStats` |
| 12 | 576 | 13 | `StaticAreaQPModInfo` |
| 13 | 624 | 14 | `StaticAreaCBP0Cntr` |
| 14 | 672 | 15 | `Colocated` |
| 15 | 720 | 16 | `HSCOutput` |
| 16 | 768 | 17 | `LFSRef` |
| 17 | 816 | 18 | `LRSNeighborMV` |
| 18 | 864 | 19 | `LFSResult` |
| 19 | 912 | 20 | `LRSResult` |
| 20 | 960 | 21 | `SrcNeighborInfo` |
| 21 | 1008 | 22 | `SrcNeighborPixel` |
| 22 | 1056 | 23 | `SrcNeighborData` |
| 23 | 1104 | 24 | `SrcNeighborFwData` |
| 24 | 1152 | 25 | `TranscodedData` |
| 25 | 1200 | 26 | `EntropyCoding` |
| 26 | 1248 | 32 | `FwClient` |
| 27 | 1296 | 33 | `FwClientMem` |
| 28 | 1344 | 34 | `InitParamsCopy` |
| 29 | 1392 | 35 | `MCTFOutput` |
| 30 | 1440 | 36 | `MCTFRef` |
| 31 | 1488 | 37 | `GGMRef` |
| 32 | 1536 | 38 | `GGMStats` |
| 33 | 1584 | 39 | `GGMOutput` |
| 34 | 1632 | 40 | `DMVOutput` |

Sample citations: idx 5 -> `+192` (`0xfffffe0008c79900`/`0xc79904`);
idx 6 -> `+240` (`0xfffffe0008c799e4`/`0xc799e8`); idx 32 -> `+1248`
(`0xfffffe0008c7b1dc`); idx 40 -> `+1632` (`0xfffffe0008c7917c`).

**Six of the 41 surface kinds have no InfoSet slot**: idx 3 `UCInfo`,
27 `IOPIPC`, 28 `FwImage`, 29 `FwLog`, 30 `FwHeap`, 31 `FwIPC`. 41 - 6 = 35,
which is exactly the entry count. These are the device-global / firmware-side
surfaces; they are not sized per client and are allocated elsewhere (the 20 MiB
`FwIPC` ring is documented in `08-ipc-transport.md`).

---

## 2. `gs_saAVE_SurfaceCfg` at `0xfffffe0007ee1050`

Re-read raw, as a cross-check on `docs/12`. Stride is **16 bytes**,
`{const char *name; u32 flags; u32 pad}`, 41 entries (`0..0x28`, bound check at
`0xfffffe0008ca8d20`). Raw bytes at `0xfffffe0007ee1050` confirm the shape: a
chained-fixup pointer, then `10 05 01 00` (= `0x00010510`), then four zero
bytes, repeated. The name pointers and flags match `docs/12` §2.1 exactly.

**The table contains no size field.** There is no `0x1400000` anywhere in it;
the last entry ends at `0xfffffe0007ee12e0`. All sizes come from the
`AVE_CalcBufSizeOf*` family via the InfoSet. The 20 MiB (`0x1400000`) `FwIPC`
figure recorded elsewhere in this repo does not originate here.

The `pad` word at `+0x0c` is zero in every entry, so the flags word is 32 bits
even though it is loaded as a 64-bit quantity (`ldp x10,x9,[x0]` at
`0xfffffe0008c79908` takes `name` in `x10` and `flags|pad` in `x9`).

### What the flags word is used for

In every `AVE_Create*Surfaces` function the pattern is identical:

```
ldp  x10, x9, [x0]                 ; x0 = &gs_saAVE_SurfaceCfg[idx]
ldr  x8, [infoset, #slot]          ; entry.flags
orr  x8, x8, x9
and  x27, x8, #0xfffffffffffeffff  ; clear bit 16
...
bl   AVE_SurfaceMgr::CreateSurface(..., size, x27 /* options */, ...)
```

(`0xfffffe0008c79904`–`0xc79910`, `0xfffffe0008c766 00`–`0xc76608` +
`0xfffffe0008c766c0`, `0xfffffe0008c773f0`–`0xc773fc`.) So the flags word is
the `options` argument of `AVE_SurfaceMgr::CreateSurface`, formed as
`(infoEntry.flags | cfg.flags) & ~0x10000`. **Bit 16 is deliberately stripped
before allocation**; its meaning is *unknown*. `x10` (the name) is fed to
`AVE_SNPrintf` (`0xfffffe0008c68ec4`) with a 64-byte buffer to build the
surface's debug name.

The only entry flags `AVE_Work_Enc_CalcSurfaceInfo` writes are for `CodedData`:

```
fffffe0008ca9d6c:  ands x8, x8, #0x800        ; x8 = client[+6208]
fffffe0008ca9d7c:  mov  w8, #0x10000
fffffe0008ca9d80:  mov  w9, #0x800
fffffe0008ca9d84:  csel x8, x9, x8, ne
fffffe0008ca9d88:  str  x8, [x19, #336]       ; CodedData.flags = 0x800 or 0x10000
```

The individual bit meanings of the flags word remain **unknown**.

---

## 3. Who allocates what

Four constructors consume the InfoSet. Two allocate memory; two import
IOSurfaces handed in by userspace. The discriminator is which
`AVE_SurfaceMgr::CreateSurface` overload is called:

| overload | VA | 5th argument | meaning |
|---|---|---|---|
| `CreateSurface(task*, u64, const char*, **int**, u64, u64, u32, AVE_Surface**)` | `0xfffffe0008c80e30` | **size in bytes** | kernel allocation |
| `CreateSurface(task*, u64, const char*, **u32**, u64, u32, AVE_Surface**)` | `0xfffffe0008c81670` | **IOSurface ID** | import |

Call counts per constructor (exact `bl` counts over each function body):

| constructor | VA | allocating calls | importing calls |
|---|---|---:|---:|
| `AVE_CreateInternalSurfaces` | `0xfffffe0008c79894` | **19** | 0 |
| `AVE_CreateExternalOutSurfaces` | `0xfffffe0008c771a4` | **7** | 0 |
| `AVE_CreateExternalInSurfaces` | `0xfffffe0008c76588` | 0 | **1** |
| `AVE_CreateDataSurfaces` | `0xfffffe0008c78600` | 0 | **11** |

Note the naming trap: **"external out" surfaces are kernel allocations**, not
client buffers. They are allocated by the kext and their IOSurface IDs are
handed back to userspace by `AVE_RetrieveExternalOutSurfaces`
(`0xfffffe0008c7702c`). Only "external in" and "data" surfaces come *from* the
client.

`AVE_CreateExternalSurfaces` (`0xfffffe0008c78504`) is just
`CreateExternalInSurfaces()` + `CreateExternalOutSurfaces()`
(`0xfffffe0008c78538`, `0xfffffe0008c78554`).

### 3.1 The four sets

**Data surfaces — client-supplied IOSurface IDs** (`AVE_CreateDataSurfaces`,
per frame; called from `AVE_Client_CreateDataSurfaces` at
`0xfffffe0008b927e0`):

`InputData` (`0xfffffe0008c78670`), `InputScaledData` (`0xc787a0`),
`DirectRecon` (`0xc78860`), `MultiPassStats` (`0xc789c0`),
`MBInputCtrl` (`0xc78a80`), `MCTFOutput` (`0xc78b98`), `MCTFRef` (`0xc78cac`),
`GGMRef` (`0xc78de8`), `GGMStats` (`0xc78e80`), `GGMOutput` (`0xc7906c`),
`DMVOutput` (`0xc7917c`).

**External-in — client-supplied IOSurface IDs**
(`AVE_CreateExternalInSurfaces`): **`Recon` only**
(`mov w0,#6; bl AVE_GetSurfaceCfg` at `0xfffffe0008c765f0`; the IDs are read as
a `u32` array of stride 4, 17 per group, at `0xfffffe0008c7665c`/`0xc76660`).

**External-out — kext-allocated, exported to userspace**
(`AVE_CreateExternalOutSurfaces`), 7 kinds:
`Link` (`0xfffffe0008c77208`), `CodedData` (`0xc773ec`), `CodedHeader`
(`0xc774c4`), `SliceHeader` (`0xc7759c`), `MBStats` (`0xc77674`), `LFSResult`
(`0xc7774c`), `LRSResult` (`0xc77898`).

**Internal — kext-allocated, never visible to userspace**
(`AVE_CreateInternalSurfaces`), 19 kinds:
`MBInputCtrl` (`0xfffffe0008c79900`), `Recon` (`0xc799e4`), `ProtectedData`
(`0xc79b60`), `StaticAreaQPModInfo` (`0xc79f30`), `StaticAreaCBP0Cntr`
(`0xc7a040`), `Colocated` (`0xc7a158`), `HSCOutput` (`0xc7a2d4`), `LFSRef`
(`0xc7a404`), `LRSNeighborMV` (`0xc7a580`), `SrcNeighborInfo` (`0xc7ab4c`),
`SrcNeighborPixel` (`0xc7ac34`), `SrcNeighborData` (`0xc7ad30`),
`SrcNeighborFwData` (`0xc7ae18`), `TranscodedData` (`0xc7af00`),
`EntropyCoding` (`0xc7b0b8`), `FwClient` (`0xc7b1dc`), `FwClientMem`
(`0xc7b2c4`), `InitParamsCopy` (`0xc7b864`), `MCTFOutput` (`0xc7b944`).

### 3.2 `Recon` is external-*optional*, internal-by-default

`docs/12` states that reference and reconstructed frames are internal. That is
**correct in effect but the mechanism is subtler**, and worth stating precisely:

`Recon` appears in *both* `AVE_CreateExternalInSurfaces` and
`AVE_CreateInternalSurfaces`, writing the same `AVE_Surface*` slots of the same
`_S_AVE_SurfaceSet`. `AVE_MD_SVE::CreateExternalSurfaces`
(`0xfffffe0008c8cc70`) runs first with the client's `_S_AVE_SurfaceIDInSet`;
for every slot whose ID word is zero it does nothing
(`ldr w8,[x24]; cbz w8, skip` at `0xfffffe0008c76660`).
`AVE_MD_SVE::CreateInternalSurfaces` (`0xfffffe0008c8d3b4`) then walks the same
array and, for each slot:

```
fffffe0008c79a8c:  ldr  x1, [x22]                ; existing AVE_Surface*
fffffe0008c79a94:  cbz  x1, <allocate>
fffffe0008c79a9c:  bl   AVE_Surface::GetSize
fffffe0008c79aa0:  cmp  w0, w28                  ; w28 = infoEntry.size
fffffe0008c79aa4:  b.lt <destroy + allocate>
fffffe0008c79aac:  bl   AVE_Surface::GetProtectionOption
fffffe0008c79ab4:  cmp  x0, x8
fffffe0008c79ab8:  b.cs <keep>
                   <destroy + allocate>
```

So: a client-provided Recon surface is **kept if it is at least
`infoEntry.size` bytes and its protection option is adequate**; otherwise it is
destroyed and reallocated by the kext. A Linux driver that supplies no Recon
IOSurfaces gets the fully kernel-allocated behaviour, which is what we want.

No other surface behaves this way — `Recon` is the only kind reachable through
both paths.

---

## 4. Sizes

All of the following are for **`_E_AVE_EncType == 1` (AVC)**, which is
established by `cmp w1,#1` / `cmp w1,#2` dispatch in each size function and by
`docs/12` §0 (slot 4 = AVC, slot 5 = HEVC in the DevCap table).

### 4.1 `_E_AVE_DevType` on M1 Pro / Max

Several size and count functions branch on `cmp wX, #0x1e` (30).
`AVE_DevInfo::GetDevType` (`0xfffffe0008bab78c`) is `ldr w0,[x0,#12]`;
`AVE_DevInfo::Init` fills that from `AVE_DevCap_Find(devID)+4`
(`ldr d0,[x22]; str d0,[x21,#8]` at `0xfffffe0008baa774`/`0xbaa778`), and
`AVE_DevCap_Find` (`0xfffffe0008ba9ea8`) is
`0xfffffe0007edba00 + devID*0x48`. Reading those rows:

| `soc-id` | DevID | **DevType** | ChipType |
|---|---:|---:|---:|
| `t6000` | 11 | **9** | 6 |
| `t6001` | 12 | **10** | 7 |

(raw bytes at `0xfffffe0007edbd18` and `0xfffffe0007edbd60`.)

**Both are `< 30`**, so every `devType > 29` branch is dead on this generation.
That has direct consequences below.

### 4.2 Surfaces that are *not* allocated on M1

These are counts that evaluate to literal zero for DevType 9/10. In
`AVE_CreateInternalSurfaces` a zero `BufNum` means the allocation loop body
never runs (`cmp x19,x20` / `b.ge` at `0xfffffe0008c79950`), so nothing is
allocated.

| surface | count function | why zero |
|---|---|---|
| `StaticAreaQPModInfo` | `AVE_CalcBufNumOfStaticAreaQPModInfo` `0xfffffe0008b60184` | `cmp w0,#0x1d; cset w8,gt; tst w8,w1` -> 0 for devType<=29 |
| `StaticAreaCBP0Cntr` | `...OfStaticAreaCBP0Cntr` `0xfffffe0008b601b4` | same idiom |
| `LRSNeighborMV` | `...OfLRSNeighborMV` `0xfffffe0008b61c60` | `cmp w0,#0x1d; cset w0,gt` -> 0 |
| `HSCOutput` | `...OfHSCOutput` `0xfffffe0008b61a04` | `csel w8,wzr,w8,le` on `cmp w0,#0x1d` forces the `tst` to 0 |
| `TranscodedData` | `...OfTranscodedData` `0xfffffe0008b62f54` | `sub w8,w0,#7; cmn w8,#3; csel w0,wzr,#2,cc` -> 0 for devType>=5 |

Their size functions are correspondingly irrelevant on M1 and were not reduced.
The `AVE_CalcBufSizeOfLRSNeighborMV` (`0xfffffe0008b61c70`) return is literally
`csel w0, w8, wzr, gt` on the same `#0x1d` compare, i.e. **0** — an independent
confirmation of the same conclusion.

*Caveat on the negative:* these are constant-folded returns with the DevType
value pinned from the DevCap table, so the test does discriminate — the same
functions return non-zero for DevType 30+. They are not "unused" in general,
only on this generation.

### 4.3 Confirmed size formulas (AVC, DevType 9/10)

`mbW = ceil(W/16)`, `mbH = ceil(H/16)` throughout.

| surface | size formula | evidence |
|---|---|---|
| `MBInputCtrl` | `16 * mbW * mbH` | `AVE_CalcBufSizeOfMBInputCtrl` `0xfffffe0008b5ef10`; `asr #4` `0xb5ef50` + `0xb5f038`, `mul` `0xb5f03c`, `sbfiz x25,x24,#4` `0xb5f040`, return `0xb5f0d0` |
| `Recon` (uncompressed) | luma `align_up(256*mbW*mbH, 512)`, chroma `C*mbW*mbH`; **total = luma + chroma** | `AVE_CalcBufSizeOfRecon` `0xfffffe0008b606c0`, path `0xb6087c`: `mul w12` `0xb608c0`, `lsl #8` `0xb608c4`, `add #0x1ff; and #~0x1ff` `0xb608c8`/`0xb608cc`; chroma constant select `0xb608a4`–`0xb608bc` and `0xb608d4`; sum `0xb60d3c`–`0xb60d48` |

The `Recon` chroma constant `C` is selected purely by the `_E_ChromaFmt`
argument, read straight out of the `csel` chain:

| `_E_ChromaFmt` | `C` (bytes/MB) | VA |
|---:|---:|---|
| 0 | 0 | `0xfffffe0008b608d4`/`0xb608d8` (`csel w9,w6,w9,eq` with `w6 == 0`) |
| 1 | 128 | `0xfffffe0008b608b4`–`0xb608bc` |
| 2 | 256 | `0xfffffe0008b608a8`–`0xb608b0` |
| other (3) | 512 | default `mov w11,#0x200` `0xfffffe0008b608a4` |

256 bytes/MB luma + 128 bytes/MB chroma is exactly NV12, so **`_E_ChromaFmt == 1`
is 4:2:0** and the enum reads `{0 = mono, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4}`
(*inferred* from the constants). Note this ordering is **not** the one
`docs/12` §1.4 inferred for the divisor table at `0xfffffe0007276400`; one of
the two inferences is wrong, or the two tables use different indices. Treat
`docs/12`'s "index 0 = 4:4:4" as unconfirmed.
| `Colocated` | `128 * mbW * mbH` | `AVE_CalcBufSizeOfColocated` `0xfffffe0008b60ed8`, path `0xb6105c`: `mul w22` `0xb61064`, `sbfiz x24,x22,#7` `0xb61068`, return `0xb612a8` |
| `MBStats` | `align_up(432 * mbW * mbH, 4096)` | `AVE_CalcBufSizeOfMBStats` `0xfffffe0008b5fc80`, path `0xb5fd00`: `mov w8,#0x1b0` `0xb5fd14`, `smull` `0xb5fd18`, `add #0xfff; and #~0xfff` `0xb5fe80`/`0xb5fe84` |
| `CodedHeader` | **`0xc000` (49152)**, constant | `AVE_CalcBufSizeOfCodedHeader` `0xfffffe0008b5fb88`: `mov w0,#0xc000; ret` |
| `SliceHeader` | `align_up(n * 1024, 4096)`, `n` = client`[+15012]` | `AVE_CalcBufSizeOfSliceHeader` `0xfffffe0008b5fbdc`: `lsl #10`, `add #0xfff`, `and #~0xfff`; argument loaded at `0xfffffe0008ca9ecc` from `[sp,#40]` <- `0xfffffe0008ca9bb8` |
| `SrcNeighborInfo` | `max(256 * mbW, 16384)` | `AVE_CalcBufSizeOfSrcNeighborInfo` `0xfffffe0008b6236c`, path `0xb6246c`: `(W+15)>>2 & ~3` = `4*mbW`, `ubfiz #6` `0xb62478`, clamp `0xb62640`–`0xb62648` |
| `SrcNeighborPixel` | `max(1024 * mbW, 16384)` | `AVE_CalcBufSizeOfSrcNeighborPixel` `0xfffffe0008b627b0`, path `0xb627ec`: same `4*mbW`, `ubfiz #8` `0xb627f8`, clamp `0xb628d8`–`0xb628e0` |
| `SrcNeighborData` | `max(56 * mbW, 16384)` | `AVE_CalcBufSizeOfSrcNeighborData` `0xfffffe0008b62a3c`, path `0xb62b94`: `x9 = mbW<<6; x23 = x9 - (mbW<<3)` `0xb62b94`/`0xb62b98`, clamp `0xb62bac`–`0xb62bb0` |
| `SrcNeighborFwData` | `max(align_up(64 * mbW, 128), 16384)` | `AVE_CalcBufSizeOfSrcNeighborFwData` `0xfffffe0008b62ce0`, path `0xb62d14`: `ubfiz #6` `0xb62d1c`, `align 128` `0xb62d30`/`0xb62d34`, clamp `0xb62d38`–`0xb62d40` |
| `EntropyCoding` | `align_down(64*W + 960, 1024) * K`, `K = ceil(mbH/4)` if flag else `8` | `AVE_CalcBufSizeOfEntropyCoding` `0xfffffe0008b6300c`, path `0xb63064`: `(H+15)>>4`, `(x+3)>>2` `0xb6306c`/`0xb63070`, `csel w8,w8,#8` `0xb6307c`, `lsl w9,w19,#6; add #0x3c0; and #~0x3ff` `0xb63080`–`0xb63088`, `mul` `0xb6308c` |
| `EntropyCoding` header | `align_up(n, 128)`, stored at entry `+0x24` | `AVE_CalcBufSizeOfEntropyCodingHeader` `0xfffffe0008b631ec`; stored `0xfffffe0008caa370`. `BufSize` (`+0x1c`) is then copied from `+0x20` at `0xfffffe0008caa374`/`0xcaa37c`, i.e. the *data* size is the allocation size |
| `InitParamsCopy` | **`0x13ec0` (81600)**, constant | `AVE_CalcBufSizeOfInitParamsCopy` `0xfffffe0008b63204` |
| `FwClient` | `arg ? arg : 0x13c000` (1294336) | `AVE_CalcBufSizeOfFwClient` `0xfffffe0008b6321c`: `mov w8,#0x13c000; cmp w0,#0; csel w0,w8,w0,eq` |
| `FwClientMem` | `n << 16` (n x 64 KiB) | `AVE_CalcBufSizeOfFwClientMem` `0xfffffe0008b63244`: `lsl w0,w0,#16` |

The HEVC branches are present in the same functions (`cmp w1,#2`) and use
32x32 CTB granularity — e.g. `MBInputCtrl` HEVC is
`32 * ceil(W/32) * ceil(H/32)` (`0xfffffe0008b5f0b0`–`0xb5f0c4`).

### 4.4 Sizes not reduced here

- **`CodedData`** (`AVE_CalcBufSizeOfCodedData`, `0xfffffe0008b5f58c`). Not a
  closed form: it takes `(devType, encType, W, H, chromaFmt, bitrate-ish,
  frameRate-ish, ..., encMode, rcMode, ...)`, calls a helper at
  `0xfffffe0008b8cfec`, and applies floating-point scale factors selected from
  a table at `0xfffffe000723e000+0xa00` (`ldr d1,[x8,#2560]` at
  `0xfffffe0008b5f688`, `fmov d2,#1.5` at `0xb5f68c`). **Rate-control
  dependent.** Not reduced.
- `ProtectedData`, `LFSRef`, `LFSResult`, `LRSResult`, `MCTFOutput` size
  functions were located but not reduced (`0xfffffe0008b5fc30`,
  `0xb615a0`, `0xb61ce8`, `0xb62104`, `0xb63280`).
- `Link` — appears only in `AVE_CreateExternalOutSurfaces`
  (`0xfffffe0008c77208`); `AVE_Work_Enc_CalcSurfaceInfo` writes **nothing** to
  slot 6, so its `num`/`size` are zero for the encode path as far as this
  function is concerned. Whether some other `CalcSurfaceInfo` (e.g.
  `AVE_Pipeline::CalcSurfaceInfo`, `0xfffffe0008c9c54c`) fills it was **not
  checked** — do not treat this as a confirmed "unused".

### 4.5 Counts

| surface | count | evidence |
|---|---|---|
| `Recon` | `setNum * layerNum * num`, `num = min(refNum + K, 17)` | `AVE_CalcBufSetNumOfRecon` `0xfffffe0008b6062c` -> 1 or 2; `AVE_CalcBufLayerNumOfRecon` `0xb60170` -> `min(x,2)`; `AVE_CalcBufNumOfRecon` `0xb60648`, cap `cmp w8,#0x11; csel` at `0xb606b4`/`0xb606b8` |
| `Colocated` | `setNum * layerNum * num` | `0xfffffe0008b60e84`, `0xb60ea0`, `0xb60eb4` |
| `SrcNeighbor{Info,Pixel,Data}` | `min(N, 4)`, `N` = DLB-unit `[+24]` | `0xfffffe0008b62358`, `0xb6279c`, `0xb62a28`; arg from `[x29,#-96]` set at `0xfffffe0008ca9bf0` |
| `SrcNeighborFwData` | `0` if `N < 2`; else `min(N,4)` on DevType 10, `1` on DevType 9 | `0xfffffe0008b62cb8`: `and w8,w0,#~1; cmp w8,#0xa; csinc` |
| `LFSResult` | `3` or `1` | `AVE_CalcBufNumOfLFSResult` `0xfffffe0008b61cb0` with `workType = 1` (literal at `0xfffffe0008caa1a0`) |
| `LRSResult` | `3` if the client flag is set, else `0` | `AVE_CalcBufNumOfLRSResult` `0xfffffe0008b620cc` with `workType = 1` (`0xfffffe0008caa1f4`); `tst w8,w2` at `0xb620f0` |
| `EntropyCoding` | `flag ? 4*i : 0` (AVC) | `AVE_CalcBufNumOfEntropyCoding` `0xfffffe0008b62fdc` |
| `FwClientMem` | `n > 1 ? 1 : 0` | `0xfffffe0008b63234` |
| `MCTFOutput` | `flag ? 8 : 0` | `0xfffffe0008b6326c` |
| `InitParamsCopy`, `FwClient` | pass-through of the argument | `0xfffffe0008b631fc`, `0xfffffe0008b63214` (both bare `ret`) |

### 4.6 Concrete byte sizes at 1920x1080

`mbW = 120`, `mbH = ceil(1080/16) = 68` (so the coded height is 1088),
`mbW*mbH = 8160`. Per buffer:

| surface | bytes | hex |
|---|---:|---|
| `MBInputCtrl` | 130,560 | `0x1FE00` |
| `Recon` luma | 2,088,960 | `0x1FE000` |
| `Recon` chroma (4:2:0) | 1,044,480 | `0xFF000` |
| **`Recon` total** | **3,133,440** | `0x2FD000` |
| `Colocated` | 1,044,480 | `0xFF000` |
| `MBStats` | 3,526,656 | `0x35D000` |
| `SrcNeighborInfo` | 30,720 | `0x7800` |
| `SrcNeighborPixel` | 122,880 | `0x1E000` |
| `SrcNeighborData` | 16,384 (floor) | `0x4000` |
| `SrcNeighborFwData` | 16,384 (floor) | `0x4000` |
| `CodedHeader` | 49,152 | `0xC000` |
| `EntropyCoding` (K=17) | 2,088,960 | `0x1FE000` |
| `EntropyCoding` (K=8) | 983,040 | `0xF0000` |
| `InitParamsCopy` | 81,600 | `0x13EC0` |
| `FwClient` (default) | 1,294,336 | `0x13C000` |
| `SliceHeader` | `align_up(1024*numSlices, 4096)` | — |
| `CodedData` | rate-control dependent | — |

These are per-buffer figures. Total footprint = size x
`setNum * layerNum * num` per surface, and the multipliers are runtime config
(reference count, DLB unit count, multipass) — see §4.5.

---

## 5. `AVE_CHM_SetFwBuf` — how addresses reach the firmware

`AVE_CHM_SetFwBuf(_S_AVE_CHM*, _S_AVE_SurfaceSet*, _S_AVE_SurfaceInfoSet*,
_S_AVE_Buf_Set*)` at `0xfffffe0008b6fc6c` (0x236c bytes). Registers:
`x19` = CHM, `x26` = SurfaceSet, `x21` = InfoSet, `x28` = Buf_Set
(`0xfffffe0008b6fc90`–`0xb6fc9c`).

It is called from exactly two places, both command builders:

| caller | VA of the `bl` |
|---|---|
| `AVE_CHM_MakeFwCmd_Start_AVC(_S_AVE_CHM*, u64, u32, _S_AVE_TimeOut*, sCAveCmdAvcStart*)` | `0xfffffe0008b671b8` |
| `AVE_CHM_MakeFwCmd_Start_HEVC(..., sCAveCmdHevcStart*)` | `0xfffffe0008b67900` |

So **buffer addresses are published once at Start, not per frame.** In
`MakeFwCmd_Start_AVC` the arguments are built at
`0xfffffe0008b671a0`–`0xb671b4`:

```
add x1, x25, #0xef000 + 0x3b8       ; SurfaceSet
add x2, x25, #0xee000 + 0xd28       ; InfoSet
add x3, x19, #0x4e8                 ; Buf_Set  <- inside the command object
mov x0, x19
bl  AVE_CHM_SetFwBuf
```

`x19` is the CHM/command object. `x2` is the InfoSet at object `+0xEED28`
(978,216) and `x1` the SurfaceSet at `+0xEF3B8` (980,408). Both match the
offsets `AVE_MD_SVE::CreateExternalSurfaces` uses:
`mov w22,#0xed28; movk w22,#0xe,lsl#16` at `0xfffffe0008c8cc50`/`0xc8cc54` and
`mov w24,#0xf3b8; movk w24,#0xe,lsl#16` at `0xc8cc5c`/`0xc8cc60`, applied at
`0xc8cc64`/`0xc8cc68`. That is an independent cross-check that the same
InfoSet and SurfaceSet instances drive both allocation and the command.

Note the two `memcpy`s immediately before (`0xfffffe0008b67188` copying `0x300`
bytes and `0xfffffe0008b6719c` copying `0x2460` bytes from client `+6200`):
the Start command carries a large parameter block; the buffer table sits at
`+0x4e8` within it. Its full extent is at least `0x1FC8` bytes
(`str xzr,[x8,#8080]` at `0xfffffe0008b7052c`, `str d0,[x8,#8136]` at
`0xb70550`). This is consistent with `docs/07`'s `sCAveCmdAvcStart` being far
larger than the `0x48` common header.

### 5.1 Buf_Set entry format

The recurring idiom is:

```
ldr  x0, [surfaceSetSlot]
ldr  w1, [x19, #40]                 ; CHM[+40] = DART index
mov  w2, #0
bl   AVE_Surface::GetDARTAddr       ; 0xfffffe0008c6da54
str  x0, [bufSlot]                  ; +0x00  device address
ldr  x0, [surfaceSetSlot]
bl   AVE_Surface::GetSize           ; 0xfffffe0008c6dbbc
str  w0, [bufSlot, #8]              ; +0x08  size
```

(`0xfffffe0008b6fe5c`–`0xb6fe74`.) So a Buf_Set entry is
**`{u64 dartAddr; u32 size; u32 pad}` = 16 bytes**, and the size written to the
firmware is the *surface's actual* size, not the InfoSet's requested size.
Entries advance by `#0x10` (`0xfffffe0008b6fe8c`).

Loops read directly (a partial map; the function has ~30 of these):

| SurfaceSet offset | count | Buf_Set offset | stride | evidence |
|---|---:|---|---:|---|
| `+0x508` | 30 | (register-carried) | `0x10` | `0xfffffe0008b6fe4c`, `cmp x27,#0x1e` `0xb6fe90` |
| `+0x328` | 30 | `+0xbc0` | `0x10` | `0xfffffe0008b6fe9c`, `0xb6fea0`, `0xb6fee4` |
| `+0x10` (`Recon`) | 17 per set, sets stride `0x88` | `+0x10`, sets stride `0x220` | `0x20` | `0xfffffe0008b6fefc`–`0xb6ff70` |
| `+0x728` | 17 | `+0xda0` | `0x10` | `0xfffffe0008b6ff94`, `0xb6ff98`, `0xb6fff0` |
| `+0xba0` | — | — | `0x10` | `0xfffffe0008b702d8` |
| `+0xfc0` | 4 | — | `0x10` | `0xfffffe0008b70e10`, `cmp x25,#4` `0xb70e4c` |

### 5.2 The DPB entries are special

The `Recon` loop does **not** call `GetDARTAddr` directly. It calls

```
fffffe0008b6ff38:  add  x2, x21, #0x110        ; &InfoSet.Recon.extra[0]
fffffe0008b6ff3c:  sub  x3, x29, #0x80         ; 4-qword output buffer
fffffe0008b6ff44:  bl   0xfffffe0008b6f2f0     ; AVE_CHM_GetFwDPBBuf
```

`AVE_CHM_GetFwDPBBuf(_S_AVE_CHM*, AVE_Surface*, int* planeSizes, u64* outAddrs)`
(`0xfffffe0008b6f2f0`) — the symbol names this explicitly as the **DPB** path.
`0x110` = 272 = Recon slot (240) + `0x20`, i.e. the
`{lumaData, lumaMeta, chromaData, chromaMeta}` array from
`AVE_CalcBufSizeOfRecon`. Two of the returned addresses are then stored
together with two of those sizes:

```
fffffe0008b6ff4c:  ldp  x8, x9, [x29, #-128]
fffffe0008b6ff50:  stur x8, [x25, #-16]        ; entry +0x00  addr0
fffffe0008b6ff54:  ldr  w8, [x21, #272]        ; InfoSet Recon extra[0]
fffffe0008b6ff58:  stur w8, [x25, #-8]         ; entry +0x08  size0
fffffe0008b6ff5c:  str  x9, [x25]              ; entry +0x10  addr1
fffffe0008b6ff60:  ldr  w8, [x21, #276]        ; InfoSet Recon extra[1]
fffffe0008b6ff64:  str  w8, [x25, #8]          ; entry +0x18  size1
```

So each DPB entry is **32 bytes = two `{addr, size}` pairs**, and the sizes come
from the InfoSet rather than from the surface. The `Recon` region of the
Buf_Set is `17 * 0x20 = 0x220` bytes per set, starting at Buf_Set `+0x10`.

**Unresolved:** the two sizes taken are `extra[0]` and `extra[1]`, which
`AVE_CalcBufSizeOfRecon` fills as luma-data and luma-*metadata*
(`stp w26,w19,[x8]` at `0xfffffe0008b60d34`), while chroma data/meta go to
`extra[2]`/`extra[3]`. Either `AVE_CHM_GetFwDPBBuf` reinterprets the array, or
the pair really is (data, metadata) with chroma handled elsewhere. The inside
of `AVE_CHM_GetFwDPBBuf` was **not** disassembled far enough to settle this.
Do not build a driver on the assumption that the second entry is the chroma
plane without checking.

---

## 6. What a Linux driver must allocate

For a baseline AVC encode on M1 Pro/Max, the kernel-allocated set reduces to
**14 kinds** (19 internal minus the 5 that are zero on this DevType), plus the
7 "external out" kinds, which are also kernel allocations:

| surface | allocator | per-buffer size (1920x1080) | purpose (name-derived, *inferred*) |
|---|---|---|---|
| `Recon` | internal (or client IOSurface) | 3,133,440 | reconstructed / reference frames — the DPB |
| `Colocated` | internal | 1,044,480 | co-located MV store for temporal direct |
| `MBInputCtrl` | internal *and* per-frame data | 130,560 | per-MB encoder control input |
| `SrcNeighborInfo` | internal | 30,720 | source-row neighbour context |
| `SrcNeighborPixel` | internal | 122,880 | source-row neighbour pixels |
| `SrcNeighborData` | internal | 16,384 | " |
| `SrcNeighborFwData` | internal | 16,384 | " (firmware-visible half) |
| `EntropyCoding` | internal | 983,040 or 2,088,960 | CABAC/CAVLC working store |
| `LFSRef` | internal | not reduced | — |
| `ProtectedData` | internal | not reduced | DRM path; count gated by a bool |
| `FwClient` | internal | 1,294,336 | per-client firmware state |
| `FwClientMem` | internal | n x 64 KiB | firmware heap for this client |
| `InitParamsCopy` | internal | 81,600 | copy of the init parameter block |
| `MCTFOutput` | internal + data | not reduced | motion-compensated temporal filter output |
| `CodedData` | external-out | rate-control dependent | **the output bitstream** |
| `CodedHeader` | external-out | 49,152 | |
| `SliceHeader` | external-out | `align_up(1024*n, 4096)` | |
| `MBStats` | external-out | 3,526,656 | per-MB statistics returned to the host |
| `LFSResult` | external-out | not reduced | |
| `LRSResult` | external-out | not reduced (count 0 unless a flag) | |
| `Link` | external-out | nothing written by `Work_Enc` | see §4.4 |

Client-supplied (V4L2 `OUTPUT` queue and friends): `InputData` — plus
`InputScaledData`, `DirectRecon`, `MultiPassStats`, `MCTF*`, `GGM*`,
`DMVOutput` for the advanced paths, all imported as IOSurface IDs by
`AVE_CreateDataSurfaces`.

---

## 7. Open questions

- The meaning of the individual bits of the surface flags word, and why bit 16
  is stripped before `CreateSurface`.
- `AVE_CalcBufSizeOfCodedData` — the rate-control-dependent bitstream buffer
  size. This is the one number a V4L2 driver most needs and it is not a closed
  form.
- The chroma half of the DPB Buf_Set entry (§5.2).
- A complete Buf_Set offset map. Roughly 30 loops exist; six are mapped above.
  Attributing the rest needs the `_S_AVE_SurfaceSet` offset -> surface map,
  which can be extracted the same way (each `AVE_Create*Surfaces` block names
  its surface via `AVE_GetSurfaceCfg` and then indexes the SurfaceSet).
- Sizes for `LFSRef`, `LFSResult`, `LRSResult`, `ProtectedData`, `MCTFOutput`.
- Whether `AVE_Pipeline::CalcSurfaceInfo` (`0xfffffe0008c9c54c`) or
  `AVE_MD_SVE::CalcSurfaceInfo` (`0xfffffe0008c8c768`) fill any slot that
  `AVE_Work_Enc_CalcSurfaceInfo` leaves at zero (notably `Link`, slot 6).
- `AVE_DPB` / `AVE_GOPMgr` were not read. The DPB *capacity* is settled here
  (17 hard maximum, `min(refNum + K, 17)`, `refNum` itself capped at 16 by
  `AVE_Work_Enc_CalcRefNum` at `0xfffffe0008ca996c`), but the mapping from a
  requested GOP structure to `refNum` lives in
  `AVE_Work_Enc_CalcRefNum_Ext` (`0xfffffe0008ca951c`), which was not read.
