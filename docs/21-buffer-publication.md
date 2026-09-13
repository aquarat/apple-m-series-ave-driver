> ## Version note (2026-09-13): macOS 13.5
>
> This document is **macOS 26.6.2** and stands for that build. On **macOS
> 13.5** (the firmware this machine runs, [43](43-macos-13.5-firmware.md)) the
> buffer tables **differ by version**: `AVE_CHM_SetFwBuf` writes into a
> `0xFED0` block that the Start builder copies to `sCAveCmdAvcInit + 0x60`
> (`0xfffffe0008ea99bc`, `0xfffffe0008ea9b3c`); inside that block DPB is
> 2 × 17 × `0x10` at `+0x28`, CodedData 20 × u64 at `+0x458` with 20 × u32
> sizes at `+0x4F8`, CodedHeader at `+0x560`/`+0x600`. Coded buffers are
> clamped to **20** (not 30) and the coded-header size is **`0x23000`** (not
> `0xC000`). The per-frame input and output addresses live in a `0xF68`-byte
> `AVE_PICMGMT_PARAMS` embedded at `sCAveCmdAvcEncode + 0x9C8` (luma
> `+0x8C0`, coded `+0xC08`). The Reset-replays-Start arithmetic has a 13.5
> counterpart: Reset `0x32DB0 = 0x48 + 0x32D68` and HevcInit
> `0x32DC8 = 0x60 + 0x32D68`, both built from the same constant in the kext
> (`0xfffffe0008ead99c`–`b4`, `0xfffffe0008ea9f64`–`84`). Details:
> [46](46-abi-13.5-commands-session.md) §10.

> ## Corrections from [32-picmgmt-params.md](32-picmgmt-params.md)
>
> Three items in this document's section 5.2 are wrong:
>
> - The Recon quad order is `{Y_MSB, Y_LSB, UV_MSB, UV_LSB}`, not
>   `{Y_LSB, Y_MSB, ...}`. That closes the open question recorded here.
> - `0x45C8 <- MBStats` is wrong. The cited instruction
>   `0xfffffe0008b72b3c` writes `0x4608`, and nothing in
>   `AVE_CHM_SetDataInfo_FwBuf` writes `0x45C8` at all - a scan returns the
>   other 26 offsets, so it discriminates.
> - `0x4618` holds `GetDARTAddr(CmdInfo[48])`, not the `SliceHeader` pool entry.

> ## Verification note
>
> - **The Reset-is-a-replay-of-Start finding is self-checking and holds.** The
>   three sizes close exactly on the same two constants:
>   `0x68 + 0x3118 = 0x3180`, `0x68 + 0x13ec0 = 0x13f28`,
>   `0x48 + 0x13ec0 = 0x13f08`, with the `0x20` difference accounted for by the
>   `FwClient`/`FwClientMem` pair. Three independent size facts reconciling on
>   one hypothesis is strong.
>
> - **One claim could not be reproduced and is downgraded.** This document
>   states that `iAddr` must be 64-byte aligned for AVC and **128-byte for
>   HEVC**, asserted by the firmware. There is **no `% 128` assertion string
>   anywhere in either binary** — searched both images for `% 128 ==` and
>   `% 64 ==`. The search does return positives (the four `% 64` assertions
>   below), so it discriminates. Treat the 64/128 `iAddr` rule as **unverified**
>   until someone cites the instruction that enforces it; it may be a numeric
>   check rather than an assert, but it has not been located.
>
> - **A different, real alignment constraint was found while checking**, from a
>   kext assertion:
>
>   ```
>   pFrameInfo->PerFrameData.StillOffsetW % 64 == 0 &&
>   pFrameInfo->PerFrameData.StillOffsetH % 16 == 0 &&
>   offset >= 0 && offset <= size && offset % 64 == 0
>   ```
>
>   So **plane offsets must be 64-byte aligned**, which joins the 64-byte stride
>   rule from [15-surface-layout.md](15-surface-layout.md) as a hard constraint
>   on client buffers. Added to `driver/ave_abi.h` as `AVE_PLANE_OFFSET_ALIGN`.

# Buffer publication — how the firmware learns where the buffers are

*The `_S_AVE_Buf_Set` slot map, the DPB publication mechanism, what the 81 KB
`Reset` payload actually contains, and how a per-frame `Process` command names
its input and output buffers.*

Everything here was read out of `AppleAVE2.kext` in `data/blobs/kc.macho` and
out of `data/blobs/ave_h13c.bin`, with `tools/disas.py`. Every constant carries
the VA of the instruction it came from. Anything not read directly out of an
instruction is marked **inferred** or **unknown**, per
[00-methodology.md](00-methodology.md).

This document corrects two claims in
[16-encode-surface-set.md](16-encode-surface-set.md) §5 and closes four of its
open questions — see §7.

---

## 0. Summary

There are **three** publication events, not one:

| when | vehicle | contents |
|---|---|---|
| `Start` (once per session) | `_S_AVE_Buf_Set` at `chm+0x4E8`, copied into `sCAveCmd{Avc,Hevc}Start+0x390` | the whole buffer **pool** — every internal and external-out surface, 22 regions, `{u64 iAddr; u32 iSize; u32 pad}` entries |
| `Start` (same command) | `cmd+0x48`/`+0x58` | `FwClient` and `FwClientMem`, as bare `{addr,size}` pairs outside the Buf_Set |
| `Process` (every frame) | `AVE_PICMGMT_PARAMS`, copied into `sCAveCmdAvcProcess+0x12C0` | the DART addresses of *this frame's* input, output, recon, colocated and reference buffers, chosen by index out of the pools above |
| `Reset` | `sCAveCmdReset+0x48` | a verbatim replay of the `Start` parameter block, saved in the `InitParamsCopy` surface |

So the model is **not** "publish once, then index by slot". The pool is
published at Start so the firmware can validate and pre-map it; the per-frame
command then re-states the actual addresses it wants used. A driver must do
both — and the firmware checks that they agree: it asserts
`pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]`
(`__cstring 0x129a12`).

The names in this document are Apple's own wherever a string in one of the two
binaries supplies them. The buffer table is `sBufSet`, its leaf descriptor has
`.iAddr`/`.iSize`, the per-frame block is `pPicParams`, and the Start payload is
`pInitParams` / `sCAveInitCmdInternalParams` — see §2.2, §5.2.1 and §6.1.

---

## 1. The `_S_AVE_SurfaceSet` offset map (confirmed)

Everything downstream depends on this. It was recovered from
`AVE_PrintInternalSurfaces` (`0xfffffe0008c7c200`) and
`AVE_PrintExternalOutSurfaces` (`0xfffffe0008c781f0`), which walk the whole
structure region by region and call
`AVE_Surface::Print(surf, subsys, level, file, __LINE__)`
(`0xfffffe0008c6b568`). The `__LINE__` immediate is monotonic across the
function, which fixes the region order; the region order matches the allocation
order in `AVE_CreateInternalSurfaces` / `AVE_CreateExternalOutSurfaces`
recorded in [16-encode-surface-set.md](16-encode-surface-set.md) §3.1, which is
what assigns the names.

`_S_AVE_SurfaceSet` is a flat array of `AVE_Surface*` (8-byte slots),
**`0x2338` bytes** total:

| off | count / shape | surface | print `__LINE__` | evidence |
|---|---|---|---:|---|
| `0x0000` | 2 | `MBInputCtrl` | 2541 | `0xfffffe0008c7c258`, `0xc7c270` |
| `0x0010` | 2 sets x 2 layers x 17, strides `0x110`/`0x88`/`8` | **`Recon`** (the DPB) | 2555 | `0xfffffe0008c7c28c`–`0xc7c2e0` |
| `0x0230` | 1 | `Link` | 996 | `ldr x0,[x21,#560]` `0xfffffe0008c78234` |
| `0x0238` | 30 | `CodedData` | 1006 | `0xfffffe0008c78258`, bound `#0xf0` `0xc78288` |
| `0x0328` | 30 | `CodedHeader` | 1016 | `0xfffffe0008c78294` |
| `0x0418` | 30 | `SliceHeader` | 1026 | `0xfffffe0008c782d0` |
| `0x0508` | 30 | `ProtectedData` | 2567 | `0xfffffe0008c7c308` |
| `0x05F8` | 30 | `MBStats` | 1036 | `0xfffffe0008c7830c` |
| `0x06E8` | 2 x 2 | `StaticAreaQPModInfo` | 2579 | `0xfffffe0008c7c344` |
| `0x0708` | 2 x 2 | `StaticAreaCBP0Cntr` | 2592 | `0xfffffe0008c7c3a4` |
| `0x0728` | 2 x 2 x 17 | `Colocated` | 2607 | `0xfffffe0008c7c404` |
| `0x0948` | 2 x 7, stride `0x38` | `HSCOutput` | 2621 | `0xfffffe0008c7c484` |
| `0x09B8` | 2 x 2 x 17 | `LFSRef` | 2636 | `0xfffffe0008c7c4e0` |
| `0x0BD8` | 1 | `LRSNeighborMV` | 2645 | `ldr x0,[x25,#3032]` `0xfffffe0008c7c55c` |
| `0x0BE0` | 16 x 2 x 10, strides `0xa0`/`0x50`/`8` | `LFSResult` | 1050 | `0xfffffe0008c78348` |
| `0x15E0` | 16 x 2 x 10 | `LRSResult` | 1066 | `0xfffffe0008c783b8` |
| `0x1FE0` | 4 | `SrcNeighborInfo` | 2655 | `0xfffffe0008c7c580` |
| `0x2000` | 4 | `SrcNeighborPixel` | 2665 | `0xfffffe0008c7c5c0` |
| `0x2020` | 4 | `SrcNeighborData` | 2675 | `0xfffffe0008c7c5fc` |
| `0x2040` | 4 | `SrcNeighborFwData` | 2685 | `0xfffffe0008c7c63c` |
| `0x2060` | 2 | `TranscodedData` | 2695 | `0xfffffe0008c7c67c` |
| `0x2070` | 4 x 16, strides `0x80`/`8` | `EntropyCoding` | 2707 | `0xfffffe0008c7c6c4` |
| `0x2270` | 4 | `FwClient` | 2718 | `0xfffffe0008c7c718` |
| `0x2290` | 1 | `FwClientMem` | 2724 | `ldr x0,[x25,#8848]` `0xfffffe0008c7c754` |
| `0x2298` | 4 | `InitParamsCopy` | 2734 | `0xfffffe0008c7c778` |
| `0x22B8` | 2 x 8, strides `0x40`/`8` | `MCTFOutput` | 2746 | `0xfffffe0008c7c7b8` |

The 26 regions are exactly contiguous with no gaps and account for every
surface kind that `AVE_CreateInternalSurfaces` or
`AVE_CreateExternalOutSurfaces` allocates. `0x2338` is therefore the extent of
everything the print functions walk; that it is also the declared `sizeof` is
**inferred** — no `bzero` of the whole `SurfaceSet` was found.
(`AVE_PrintExternalInSurfaces`, `0xfffffe0008c76f38`, prints nothing at all —
its whole body is the null check and the log-level test — which is consistent
with external-in being only `Recon`, already covered at `+0x10`.)

The other nine InfoSet slots — `InputData`,
`InputScaledData`, `DirectRecon`, `MultiPassStats`, `MCTFRef`, `GGMRef`,
`GGMStats`, `GGMOutput`, `DMVOutput` — are *data* surfaces and live in a
separate 96-byte `_S_AVE_SurfaceDataSet`; see §5.3.

### 1.1 Where the three structures live

All three are members of one enclosing object, reached as `chm->[0x20]`
(register `x25` in `MakeFwCmd_Start_AVC`, `x28` in `SetDataInfo_FwBuf`):

```
+0xEED28   _S_AVE_SurfaceInfoSet   0x690 bytes   (0xfffffe0008b671ac, 0xc8cc50)
+0xEF3B8   _S_AVE_SurfaceSet       0x2338 bytes  (0xfffffe0008b671a4, 0xc8cc5c)
```

`0xEED28 + 0x690 == 0xEF3B8` exactly — an independent confirmation that
`sizeof(_S_AVE_SurfaceInfoSet) == 0x690`.

The `_S_AVE_Buf_Set` is **not** in that object. It lives inside the
`_S_AVE_CHM` at `+0x4E8` (`add x3, x19, #0x4e8` at `0xfffffe0008b671b0`, where
`x19` is the CHM — it is also `x0` of the same call).

---

## 2. `_S_AVE_Buf_Set` — the slot map

### 2.1 Size and placement — confirmed twice

`AVE_CHM_MakeFwCmd_Start_AVC` (`0xfffffe0008b66ff8`) does, in order:

```
b6718c:  mov  w8, #0x1838              ; 6200
b67190:  add  x0, x19, #0x4c0          ; chm + 0x4C0
b67194:  add  x1, x27, x8              ; client + 6200
b67198:  mov  w2, #0x2460              ; 9312
b6719c:  bl   memcpy                   ; pre-fill the whole engine parameter block
b671a4:  add  x1, x25, #0xEF3B8        ; SurfaceSet
b671ac:  add  x2, x25, #0xEED28        ; InfoSet
b671b0:  add  x3, x19, #0x4e8          ; Buf_Set = chm + 0x4E8
b671b8:  bl   AVE_CHM_SetFwBuf
...
b672fc:  add  x0, x23, #0x368          ; cmd + 0x368
b67300:  add  x1, x19, #0x4c0          ; chm + 0x4C0
b67304:  mov  w2, #0x2460
b67308:  bl   memcpy                   ; block -> command
```

So the CHM holds a `0x2460`-byte **engine parameter block** at `+0x4C0`, of
which the buffer table is the sub-range `+0x4E8 .. ?`, and the whole block is
copied into the Start command at `+0x368`. The Buf_Set therefore sits at
**command offset `0x368 + 0x28 = 0x390`**.

The highest byte `AVE_CHM_SetFwBuf` writes is `Buf_Set + 0x21EF`
(`str w0,[x22,#8632]` at `0xfffffe0008b701ec`, plus the four-entry loop
`0xfffffe0008b70f08`–`0xb70f4c` reaching `0x21F0`). And the next field the Start
builder writes after the block is at **`cmd + 0x2580`**
(`mov w8,#0x2580; add x26,x23,x8` at `0xfffffe0008b672f4`/`0xb672f8`), and
`0x390 + 0x21F0 == 0x2580` exactly.

**`sizeof(_S_AVE_Buf_Set) = 0x21F0` (8688 bytes)** — confirmed from both ends.

### 2.2 Entry format — and Apple's own name for it

The plain entry is 16 bytes, built by the recurring idiom (e.g.
`0xfffffe0008b6fd5c`–`0xb6fd74`):

```c
struct _S_AVE_Buf { uint64_t iAddr; uint32_t iSize; uint32_t pad; };
```

`iAddr` = `AVE_Surface::GetDARTAddr(surf, chm->[0x28], 0)`
(`0xfffffe0008c6da54`), `iSize` = `AVE_Surface::GetSize(surf)`
(`0xfffffe0008c6dbbc`) — the surface's *actual* size, not the InfoSet's
requested size. Four surface kinds use wider entries: see the table.

The field names are not guesses. The firmware's own trace and assertion strings
name the structure and its members verbatim (`__cstring`, file offsets from
`strings -a -t x data/blobs/ave_h13c.bin`):

| VA | string |
|---|---|
| `0x12a7f7` | `sBufSet.saCodedData[%d].iAddr:%016llx` |
| `0x12a81e` | `sBufSet.saCodedData[%d].iSize:0x%x` |
| `0x12a865` | `sBufSet.saRecon[%d][0].iAddr:%016llx` |
| `0x12a88b` | `sBufSet.saEntropyCoding[%d][%d].saIBuf[AVE_BufIdx_Data].iAddr:%016llx` |

So `_S_AVE_Buf_Set` is **`sBufSet`**, its members are `sa<SurfaceName>[]`
arrays, the leaf descriptor has `.iAddr` and `.iSize`, and the wider entries
(`0x20`, `0x40`) are arrays `saIBuf[]` of that leaf, indexed by an enum
`_E_AVE_BufIdx` whose members `AVE_BufIdx_Data`, `AVE_BufIdx_Luma` and
`AVE_BufIdx_Chroma` all appear as literal text in the image. `saRecon[%d][0]`
is two-dimensional exactly as §3.2 predicts. This is an independent
confirmation of the whole §2.3 shape from the other side of the interface.

### 2.2.1 Address alignment — a hard firmware requirement

The firmware asserts alignment on the addresses it is handed. From the same
`__cstring` region:

```
0x1297ad  (EncCommParams.encoder_addr_entropy[i][...].saIBuf[AVE_BufIdx_Data].iAddr & 63)  == 0
0x130a69  (EncCommParams.encoder_addr_entropy[i][...].saIBuf[AVE_BufIdx_Data].iAddr & 127) == 0
0x12ed62  (cPicMgmtParams[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iAddr & 63)   == 0
0x12edf1  (cPicMgmtParams[AVE_BufIdx_Chroma].saIBuf[AVE_BufIdx_Data].iAddr & 63) == 0
0x131c75  (pPicParams->sRecon.Y_LSB  & 127) == 0
0x131cb9  (pPicParams->sRecon.Y_MSB  & 127) == 0
0x131cfe  (pPicParams->sRecon.UV_LSB & 127) == 0
0x131d44  (pPicParams->sRecon.UV_MSB & 127) == 0
0x131517  (pPicParams->sRef.Low_Res_Y_L0[me_ref_index] & 63) == 0
0x131933  (pPicParams->sRef.Y_L0_MSB[me_ref_index]     & 63) == 0
0x131e46  ((pPicParams->sRef.Colocated_L1[0] + ...)    & 63) == 0
```

**64-byte alignment for AVC, 128-byte for HEVC and for every recon plane.**
`0x1297ad` is the AVC entropy path and `0x130a69` the HEVC one, which is what
fixes the two constants to the two codecs. Kext-allocated surfaces are
16 KB-aligned ([16-encode-surface-set.md](16-encode-surface-set.md) §3), so this
only bites on client-supplied buffers and on the *derived* plane addresses
(`base + extra[1] + extra[3]`, §3.1) — those must stay 128-aligned too.
These are `AVE_ASSERT`-style failures, i.e. the firmware halts, so a driver
must honour them.

### 2.3 The map

`AVE_CHM_SetFwBuf(_S_AVE_CHM* x19, _S_AVE_SurfaceSet* x26,
_S_AVE_SurfaceInfoSet* x21, _S_AVE_Buf_Set* x28)` at `0xfffffe0008b6fc6c`.
Every region below was read from its loop.

| Buf_Set off | entry | count | source (SurfaceSet) | surface | loop VA |
|---|---|---:|---|---|---|
| `0x0000` | **`0x20`** | 2 x 17 | `+0x10`, layer stride `0x88` | **`Recon` (DPB)** | `0xfffffe0008b6fef8` |
| `0x0440` | **`0x20`** | 2 x 17 | `+0x9B8`, set 0 | `LFSRef` set 0 | `0xfffffe0008b70aec` |
| `0x0880` | `0x10` | 10 | `+0xBE0` | `LFSResult` set 0 | `0xfffffe0008b70c30` |
| `0x0920` | `0x10` | 10 | `+0x15E0` | `LRSResult` set 0 | `0xfffffe0008b70ce0` |
| `0x09C0` | `0x10` | 30 | `+0x238` **or** `+0x508` | **`CodedData`** / `ProtectedData` | `0xfffffe0008b6fd4c` / `0xb6fe4c` |
| `0x0BA0` | `0x10` | 2 | `+0x2060` | `TranscodedData` | `0xfffffe0008b702c4` |
| `0x0BC0` | `0x10` | 30 | `+0x328` | `CodedHeader` | `0xfffffe0008b6fe9c` |
| `0x0DA0` | `0x10` | 2 x 17 | `+0x728`, stride `0x88` | `Colocated` | `0xfffffe0008b6ff94` |
| `0x0FC0` | `0x10` | 4 | `+0x1FE0` | `SrcNeighborInfo` | `0xfffffe0008b70e08` |
| `0x1000` | `0x10` | 4 | `+0x2000` | `SrcNeighborPixel` | `0xfffffe0008b70e54` |
| `0x1040` | `0x10` | 4 | `+0x2020` | `SrcNeighborData` | `0xfffffe0008b70ea0` |
| `0x1080` | **`0x20`** | 16 x 4, row `0x80` | `+0x2070` | `EntropyCoding` | `0xfffffe0008b70f50` |
| `0x1880` | `0x10` | 2 x 2 | `+0x6E8` | `StaticAreaQPModInfo` | `0xfffffe0008b7038c` |
| `0x18C0` | `0x10` | 2 x 2 | `+0x708` | `StaticAreaCBP0Cntr` | `0xfffffe0008b70418` |
| `0x1900` | **`0x40`** | 7 | `+0x948` | `HSCOutput` | `0xfffffe0008b70a24` |
| `0x1AC0` | `0x20` | (set 1) | `+0x9B8`, set 1 | `LFSRef` set 1 | `0xfffffe0008b70b9c` |
| `0x1C40` | `0x10` | 10 | `+0xBE0` | `LFSResult` set 1 | `0xfffffe0008b70c98` |
| `0x1CE0` | `0x10` | 10 | `+0x15E0` | `LRSResult` set 1 | `0xfffffe0008b70d4c` |
| `0x1D80` | `0x10` | 1 | `+0xBD8` | `LRSNeighborMV` | `0xfffffe0008b70bfc` |
| `0x1D90` | **`0x40`** | 2 x 8 | `+0x22B8` | `MCTFOutput` | `0xfffffe0008b704a4` |
| `0x2190` | `0x10` | 2 | `+0x0000` | `MBInputCtrl` | `0xfffffe0008b7032c` |
| `0x21B0` | `0x10` | 4 | `+0x2040` | `SrcNeighborFwData` | `0xfffffe0008b70efc` |
| — | | | | **end `0x21F0`** | |

The regions are contiguous and account for the whole `0x21F0`.

**Not published through the Buf_Set at all:** `Link` (`+0x230`),
`SliceHeader` (`+0x418`), `MBStats` (`+0x5F8`), `FwClient` (`+0x2270`),
`FwClientMem` (`+0x2290`), `InitParamsCopy` (`+0x2298`). `FwClient` and
`FwClientMem` go into the Start command header area instead (§4.1);
`SliceHeader` and `MBStats` are published per frame (§5.2); `Link` and
`InitParamsCopy` are never given to the firmware as a DART address at all
(`InitParamsCopy` is a host-side save area, §6).

### 2.4 Branches inside `SetFwBuf` — three of them matter to a driver

1. **Coded output vs protected output.** `ldrb w8, [client+6209]; tbnz w8,#3`
   at `0xfffffe0008b6fd40`/`0xb6fd44` selects `ProtectedData` (`SurfaceSet+0x508`)
   instead of `CodedData` (`+0x238`) as the source for Buf_Set `+0x9C0`. A
   non-DRM driver always takes the `CodedData` arm.
2. **Client type 4 skips a whole block.** `ldr w8,[x19,#52]; cmp w8,#4; b.ne`
   at `0xfffffe0008b7000c`–`0xb70014`. `chm+0x34` is the *client type*
   ([07-commands-abi.md](07-commands-abi.md) §4). For client type 4 the
   `HSCOutput`, `LFSRef`, `LRSNeighborMV`, `LFSResult` and `LRSResult` regions
   are **left as whatever the `0x2460` template copy put there**. The two paths
   rejoin at `0xfffffe0008b70018` (`b` at `0xfffffe0008b70d94`).
3. **Single-slot vs four-slot `SrcNeighbor*`/`EntropyCoding`.**
   `cbz w8, 0xfffffe0008b70e08` at `0xfffffe0008b70038`, where `w8` is
   `client[ pipelineObj[+20] * 0x70 + 756 ]` — `ldrsw x8,[x8,#20]`,
   `smaddl x8,w8,#0x70,x10`, `ldr w8,[x8,#756]` at
   `0xfffffe0008b7001c`–`0xb70028`, with `x8 = chm[+0x20]` and
   `x10 = chm[+0x18]` restored from `[fp-144]`/`[fp-136]`
   (stored at `0xfffffe0008b6fd1c`). Zero -> the four-entry loops
   (`0xb70e08` onward); non-zero -> a single entry per region, taken at
   SurfaceSet index `chm[+0x2C]` and written to slot 0 of the same regions
   (`0xfffffe0008b7003c` onward). Both write the same Buf_Set offsets, so §2.3
   holds either way.

*Caveat, read but not explained:* several outer loops re-write the same Buf_Set
bytes on every outer iteration — `HSCOutput` (`x24` fixed at `Buf_Set+0x1900`
across the 2-iteration outer loop, `0xfffffe0008b70a48`), `LFSResult` and
`LRSResult` (`x24` reloaded from `[sp,#152]` inside the 16-iteration outer
loop, `0xfffffe0008b70c54`). And the `LFSRef` set-1 destination advances by
`0xC0` per layer (`0xfffffe0008b70b4c`) while the inner loop writes `0x220`.
Either the outer counts are 1 in practice for an encode session, or these are
Apple bugs that are latent because the extra surfaces are `NULL`. **Do not rely
on the outer dimension of these three.**

---

## 3. The DPB — `AVE_CHM_GetFwDPBBuf`, resolved

`AVE_CHM_GetFwDPBBuf(_S_AVE_CHM*, AVE_Surface*, int* extra, uint64_t out[4])`
at `0xfffffe0008b6f2f0`. The out-parameter is **four** addresses (32 bytes);
`SetFwBuf` zeroes them with `stp q0,q0,[x29,#-128]` (`0xfffffe0008b6ff28`)
before the call.

`extra` is `&InfoSet.Recon.extra[0]` = InfoSet `+0x110`
(`add x2, x21, #0x110` at `0xfffffe0008b6ff38`), i.e. slot 5 (`240`) `+0x20`.
From `AVE_CalcBufSizeOfRecon`'s store `stp w26,w19,[x8]` / `stp w28,w27,[x8,#8]`
at `0xfffffe0008b60d34`/`0xb60d38`, and from the uncompressed AVC arm at
`0xfffffe0008b6087c` where `w19 = 0` and `w27 = 0` while
`w26 = align_up(256*mbW*mbH,512)` (`0xfffffe0008b608c4`–`0xb608cc`) and
`w28 = C*mbW*mbH` (`0xb608dc`):

```
extra[0] = luma data bytes
extra[1] = luma metadata bytes     (0 for uncompressed)
extra[2] = chroma data bytes
extra[3] = chroma metadata bytes   (0 for uncompressed)
```

### 3.1 The baseline path — plane addresses

`0xfffffe0008b6f5c0`–`0xb6f5ec`:

```
b6f5cc:  x0 = AVE_Surface::GetDARTAddr(surf, chm[0x28], 0)   ; base
b6f5d4:  x8 = extra[3]
b6f5d8:  ldpsw x10, x9, [x21]          ; x10 = extra[0], x9 = extra[1]
b6f5dc:  x9 = base + extra[1]
b6f5e0:  x8 = base + extra[1] + extra[3]
b6f5e4:  stp x8, x0,  [x22]            ; out[0], out[1]
b6f5e8:  x10 = x8 + extra[0]
b6f5ec:  stp x10, x9, [x22,#16]        ; out[2], out[3]
```

So the `Recon` surface is laid out **metadata first, then data**:

| region | address | size |
|---|---|---|
| luma metadata | `base` | `extra[1]` |
| chroma metadata | `base + extra[1]` | `extra[3]` |
| luma data | `base + extra[1] + extra[3]` | `extra[0]` |
| chroma data | `base + extra[1] + extra[3] + extra[0]` | `extra[2]` |

and `out[] = { lumaData, lumaMeta, chromaData, chromaMeta }`.

Two other arms exist, gated on compression flags at
`0xfffffe0008b6f458`–`0xb6f498`: an IOSurface YUV-info query
(`0xfffffe0008c6ece4`, out[] filled from a `0x70`-byte stack struct at
`0xfffffe0008b6f794`) and a two-plane query (`0xfffffe0008c6e164`,
`0xfffffe0008b6f5a0`, which sets `out[1] = out[3] = 0`). The baseline
uncompressed AVC encode takes the arm above; the gate is
`client[0x4afc] == 2` OR `client[0x3a75]`, AND `client[0x3a75+1611]`
(`0xfffffe0008b6f490`/`0xb6f494`/`0xb6f498`).

### 3.2 What the Start-time DPB entry carries — the chroma question, answered

`SetFwBuf` keeps only two of the four (`0xfffffe0008b6ff4c`–`0xb6ff64`):

```c
struct _S_AVE_DPBBuf {          /* 0x20 bytes */
    uint64_t dataAddr;   /* +0x00 = out[0] = luma data plane        */
    uint32_t dataSize;   /* +0x08 = InfoSet.Recon.extra[0]          */
    uint32_t pad0;
    uint64_t metaAddr;   /* +0x10 = out[1] = luma metadata plane    */
    uint32_t metaSize;   /* +0x18 = InfoSet.Recon.extra[1]          */
    uint32_t pad1;
};
```

The chroma addresses are **deliberately not transmitted at Start**, because
they are derivable from what is: chroma data is `dataAddr + dataSize`, chroma
metadata is `metaAddr + metaSize` — exactly `out[2]` and `out[3]` above.

That reading is confirmed independently by the `LFSRef` entry, which `SetFwBuf`
builds inline with the same shape but combining both planes
(`0xfffffe0008b70b7c`–`0xb70bcc`):

```
+0x00 = base + (extra[1] + extra[3])     ; start of the data area
+0x08 = extra[0] + extra[2]              ; total data bytes
+0x10 = base                             ; start of the metadata area
+0x18 = extra[1] + extra[3]              ; total metadata bytes
```

with `extra[]` read from InfoSet `+800/804/808/812` = slot 16 (`768`) `+0x20`
= `LFSRef.extra[]` (`0xfffffe0008b70bac`–`0xb70bc8`, `0xb70a28`/`0xb70a2c`).
Same "metadata block first, then data block" layout, same pairing.

For a baseline uncompressed AVC encode `extra[1] = extra[3] = 0`, so the
Start-time DPB entry degenerates to `{ base, lumaBytes, base, 0 }`.

### 3.2.1 The firmware's names for the four planes

The firmware calls the four `GetFwDPBBuf` outputs, in the per-frame block,
`pPicParams->sRecon.{Y_LSB, Y_MSB, UV_LSB, UV_MSB}` (assert strings at
`0x131c57`, `0x131c9b`, `0x131cdf`, `0x131d25`), and elsewhere addresses the
same pair-of-pairs as
`cPicMgmtParams[AVE_BufIdx_{Luma,Chroma}].saIBuf[AVE_BufIdx_Data]`
(`0x12ec6e`, `0x12edac`). There is a
`CAVE_PICMGMT_PARAMS::GetPlaneCompressedInfo(_E_AVE_BufIdx)` (symbol at
`0x1342b0`), so the second element of each `saIBuf[]` pair is the *compression*
side-band of that plane.

That means [16-encode-surface-set.md](16-encode-surface-set.md)'s label
"lumaMeta / chromaMeta" for `extra[1]`/`extra[3]` should be read as
**the second (MSB / compression-info) plane**, not necessarily "metadata" in
any general sense. The grouping — two planes, each a `{iAddr, iSize}` pair,
luma first — is confirmed; the `_LSB`/`_MSB` assignment to `out[0]`/`out[1]` is
**inferred** from C declaration order and has not been proved.

### 3.3 Count

**2 groups of 17** entries, `0x20` each, at Buf_Set `+0x000 .. +0x440`. The
group index is the `Recon` *layer* (SurfaceSet stride `0x88`), `x28 in {0,1}`
via the unrolled-two idiom at `0xfffffe0008b6ff08`/`0xb6ff7c`/`0xb6ff88`; the
`Recon` *set* index is fixed at 0. `LFSRef`, by contrast, publishes both sets
(§2.3). 17 is the hard DPB maximum already established in
[16-encode-surface-set.md](16-encode-surface-set.md) §4.5.

---

## 4. The `Start` command layout

`AVE_CHM_MakeFwCmd_Start_AVC(_S_AVE_CHM* x19, u64 cnt, u32 flags,
_S_AVE_TimeOut*, sCAveCmdAvcStart* x23)` at `0xfffffe0008b66ff8`:

| off | size | contents | VA |
|---|---:|---|---|
| `0x0000` | `0x40` | common header ([07](07-commands-abi.md) §4), id 6 | `0xfffffe0008b670e8` |
| `0x0048` | 8 | `FwClient[chm[0x2C]]` DART address | `0xfffffe0008b67124` |
| `0x0050` | 4 | its size | `0xfffffe0008b6714c` |
| `0x0058` | 8 | `FwClientMem[0]` DART address | `0xfffffe0008b6716c` |
| `0x0060` | 4 | its size | `0xfffffe0008b67178` |
| `0x0068` | `0x300` | copy of `client + 0x1538` | `0xfffffe0008b67188` |
| `0x0368` | `0x2460` | the engine parameter block from `chm + 0x4C0` | `0xfffffe0008b67308` |
| ↳ `0x0390` | `0x21F0` | ↳ **`_S_AVE_Buf_Set`** | §2 |
| `0x27C8` | `0x154` | copy of `client + 0x417C` | `0xfffffe0008b67320` |
| `0x291C` | `0x6B4` | copy of `client + 0x4AD0` | `0xfffffe0008b67338` |
| `0x2FD0` | `0x180` | copy of `client + 0x5184` | `0xfffffe0008b67350` |
| `0x2580` | 2 | `3` if `clientType == 4` and `DevType >= 30`, else `0` | `0xfffffe0008b67374` |
| `0x3150` | 1 | `client[1472] & 1` | `0xfffffe0008b67380` |
| | | **total `0x3180`** | `mov w1,#0x3180` `0xfffffe0008b670ac` |

The `FwClient` pointer at `+0x48` is read back by the firmware:
`ProcessCmd_Start_AVC` does `ldr x1,[x21,#72]` / `ldr w2,[x21,#80]` at
`0x297d8`/`0x297dc` and passes them to `0x31cc0`. Both sides agree.

`AVE_CHM_MakeFwCmd_Start_HEVC` (`0xfffffe0008b67738`) is the same shape with the block
sizes scaled up; total `0x13F28`.

---

## 5. Per-frame publication

### 5.1 `Process` carries no addresses of its own

`AVE_CHM_MakeFwCmd_Process_AVC(chm, cnt, flags, timeout, sCAveCmdAvcProcess*,
int slot, AVE_PICMGMT_PARAMS*, _S_AVE_FrameInfo*)` at `0xfffffe0008b69f74` is
short:

```
b6a098:  bzero(cmd, 0x63D8)
         ... 0x40-byte header, id 8 at +0x00 (b6a15c), caller's slot at +0x20 (b6a0a0) ...
b6a544:  memcpy(cmd + 0x99C,  *(pipeline+0xF1728) + 40, 0x924)
b6a558:  memcpy(cmd + 0x12C0, PICMGMT_PARAMS,           0x5118)
```

`0x12C0 + 0x5118 == 0x63D8 == sizeof(sCAveCmdAvcProcess)`. There is **no**
`GetDARTAddr` / `GetSize` / `GetFwDPBBuf` call anywhere in the function
(`grep` over the full `0x7B0`-byte disassembly returns zero hits for
`c6da54`, `c6dbbc`, `c6da34`, `c6ece4`, `c6e164`, `b6f2f0`; the same grep over
`AVE_CHM_SetFwBuf` returns dozens, which is the positive control).

### 5.2 `AVE_CHM_SetDataInfo_FwBuf` is where the per-frame addresses are written

`AVE_CHM_SetDataInfo_FwBuf(_S_AVE_CHM* x25, _S_AVE_CmdInfo* [sp+168],
_S_AVE_FrameInfo* x23, _S_AVE_DPB_Set* [sp+128], AVE_PICMGMT_PARAMS* x20)` at
`0xfffffe0008b71fd8`. It writes DART addresses straight into the PICMGMT block
that `Process` then copies. `x27 = x20 + 0x4000` is used for the size fields.

The **frame slot index** is `x21 = (int32)FrameInfo[+0xC6C]` (3180),
`x19 = x21 * 8` (`0xfffffe0008b7204c`, `0xb72098`). It indexes the 30-entry
pools published at Start:

| PICMGMT off | source | VA |
|---|---|---|
| `0x4EF8` (addr), `0x4F08` (size) | `SurfaceSet[+0x238 or +0x508][slot]` = **`CodedData`** / `ProtectedData` | `0xfffffe0008b720c8`, `0xb720d4`; base select `0xb72074`–`0xb72094` |
| `0x4F00` | `SurfaceSet[+0x328][slot]` = `CodedHeader`, after `bzero(kva, 0xBAA8)` | `0xfffffe0008b7212c`; zero-fill `0xb72118` |
| `0x4618` / `SliceHeader` | `CmdInfo[+48]`, then `SurfaceSet[+0x418][slot]` | `0xfffffe0008b73384`, `0xb7338c` |
| `0x45C8` | `SurfaceSet[+0x5F8][slot]` = **`MBStats`** | `0xfffffe0008b72b3c`, base `0xb72b0c` |
| `0x4F48`/`0x4F58` | `SurfaceSet[+0xBE0]` / `[+0x15E0]` = `LFSResult` / `LRSResult` | `0xfffffe0008b72f10`, `0xb72b94`; bases `0xb72edc`, `0xb72b5c` |
| `0x4548`, `0x4550`, `0x4558`, `0x4560` | a **four-address plane quad** — `sRecon.{Y_LSB,Y_MSB,UV_LSB,UV_MSB}` (§3.2.1). Written either from a stack YUV-info struct (`0xfffffe0008b72a7c`–`0xb72a90`) or from `GetFwDPBBuf`'s `out[0..3]` as `q0,q1` (`0xfffffe0008b74fb8`/`0xb74fc0`, call at `0xb73f5c`) — the same two-arm choice `GetFwDPBBuf` itself makes | as cited |
| `0x4568` | the `Colocated` buffer of the *same* DPB slot | `0xfffffe0008b7501c` |
| `0x4570` (+`0x4578` size) | input picture address, adjusted by `frameInfo[5264] + frameInfo[5268]*stride` — a crop origin | `0xfffffe0008b72c40`–`0xb72c50` |
| `0x4F18` / `0x4F48` / `0x4F58` | the surfaces at `_S_AVE_DPB_Set[+0x38, +0x40, +0x48]` | `0xfffffe0008b75060`, `0xb75090`, `0xb750c8` |
| `0x4670`,`0x4690`,`0x46B0`,`0x46D0` | `SrcNeighbor{Info,Pixel,Data,FwData}[chm[0x2C]]` | `0xfffffe0008b73bfc`, `0xb73c3c`, `0xb73c7c`, `0xb73cc0` |

**The DPB slot number is recovered by pointer search, not stored.** After
`GetFwDPBBuf` on the current recon surface, the code scans
`SurfaceSet + 0x3C8`… wait — `add x21, x28+0xEF000, #0x3c8` = `SurfaceSet+0x10`
= the `Recon` region — with stride `0x88` for 17 entries, comparing each
`AVE_Surface*` against the one it just used
(`0xfffffe0008b74fc4`–`0xb74ff8`, bound `cmn x25,#0x11` at `0xb74ff4`). On a
match it takes `[matchedSlot + 1816]`; `0x10 + 0x718 == 0x728` = the
`Colocated` region, i.e. **the colocated buffer with the same index as the
recon buffer** (`0xfffffe0008b75000`).

So the answer to "how does a frame say which buffer" is: **by DART address,
restated every frame inside `AVE_PICMGMT_PARAMS`**, with the *host* choosing
the pool slot (`FrameInfo[+0xC6C]` for the output pools, a
`_S_AVE_DPB_Set` of surface pointers for the references). The firmware is not
given an index it has to resolve. The Start-time Buf_Set is the *declaration*
of the pool; the Process command is the *selection*.

### 5.2.1 The firmware's names for the per-frame block

The firmware calls this block **`pPicParams`** and names its members verbatim in
assertion strings. This is the most useful single list in the image for anyone
filling the structure:

| VA | string (abridged) | what it is |
|---|---|---|
| `0x128acc` | `pPicParams->sInput.sMultiPassStats.iAddr != 0` | input sub-struct, uses the same `{iAddr,iSize}` leaf |
| `0x129a12` | `pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]` | **the output bitstream address**, and the firmware cross-checks it against its own table |
| `0x129a57` | `pPicParams->sOutput.CodedBufSize > (minBufSize/2)` | output size, with a minimum |
| `0x131c57`.. | `pPicParams->sRecon.{Y_LSB,Y_MSB,UV_LSB,UV_MSB}` | the recon quad (§3.2.1) |
| `0x131906`.. | `pPicParams->sRef.{Y_L0_MSB,Y_L0_LSB,Y_L1_MSB,Y_L1_LSB}[me_ref_index]` | **the reference lists** — arrays of addresses indexed by `me_ref_index` |
| `0x1314e6`, `0x1315e4` | `pPicParams->sRef.Low_Res_Y_L{0,1}[me_ref_index]` | low-resolution (LRME) reference lists |
| `0x131df4` | `pPicParams->sRef.Colocated_L1[0] + coloDataContextOffset + multiCoreOffset` | colocated MV store, with a context offset |
| `0x12f1f4`, `0x13154f` | `pPicParams->sLowResOutput.{LowResSrcLumaScaled,LowResResults}` | LRME outputs |
| `0x12f921`, `0x12fae5` | `pPicParams->sFrameInfo.{iLayerID,PicOrderCntVal}` | frame identity |
| `0x12f18f` | `pPicParams->sRCUpdateData.bInputCompressed` | rate-control update block |

So the reference pictures **are** named by index within the frame
(`me_ref_index` into `sRef.Y_L0_*[]`), but the array elements are addresses,
not pool slot numbers. `0x129a12` is worth noting: the firmware asserts that
the address the host puts in `sOutput.Coded` equals the entry in its own
`bitstream_addr_dst[index]` table — i.e. **it validates the per-frame address
against the pool published at Start**. That is direct evidence that the two
publication events are meant to agree, and that a driver cannot skip either.

*Caveat:* AVC transmits only the first `0x5118` bytes of `AVE_PICMGMT_PARAMS`
(`0xfffffe0008b6a55c`), so any PICMGMT offset `>= 0x5118` — e.g. the
`str x0,[x23,#29536]` at `0xfffffe0008b73adc` — is HEVC-only
(`sCAveCmdHevcProcess` is `0xB1C0`).

### 5.4 Firmware side of `Process`

`CFlowControllerBase::ProcessCmd_Process_AVC` spans `0x2adf0`–`0x2b47c`
(next symbol `ProcessCmd_Process_HEVC` at `0x2b480`). It gets the command as
`ldr x19,[x20,#1472]` (`0x2ae4c`), reads only `+0x08`, `+0x10` and `+0x24` from
the header on the normal path, acquires an internal buffer from
`CFlowControllerBase::aquireEncCmdBufFromPool` (`0x263ec`, named by the assert
tag `"pEncCmdBuf"` at `0x122203`), and copies the command into it **at the same
offsets**, in slices:

| command range | bytes | copy VA |
|---|---:|---|
| `+0x0048` .. `+0x099C` | 2388 | `0x2b270` (gated on a flag at `+0x48`, `0x2b264`) |
| `+0x099C` .. `+0x12C0` | 2340 | `0x2b32c` |
| `+0x12C0` .. `+0x1690` | 976 | `0x2b318` |
| `+0x29F8` .. `+0x2A18/0x2A20` | 32 or 40 | `0x2b33c` |
| `+0x2A20` .. | count x 8 | `0x2b35c` |
| `+0x54E8` .. `+0x63D8` | 3824 | `0x2b378` |

then `CFlowControllerBase::SendCommandToQueue(...)` at `0x2b3ac`. **Every
address `SetDataInfo_FwBuf` writes lands in the last slice**: PICMGMT `+0x4228`
is command `+0x54E8`, so the recon quad (PICMGMT `+0x4548` = cmd `+0x5808`) and
the coded-output pointer (PICMGMT `+0x4EF8` = cmd `+0x61B8`) are both inside
`+0x54E8 .. +0x63D8`. A separate pointer, cmd `+0x4E20`, is passed *uncopied*
to `0x32dc4`, which walks it as `{int32 count; ...; 0x78-byte records at +0x20}`
and logs them via `"AVE_StoreRCFrameStats"` (`0x122b88`) — rate-control frame
statistics, not buffers.

Two side results worth recording here because they close open items in
[07-commands-abi.md](07-commands-abi.md) §7:

- **Header `+0x24` is a priority**, for `Process` as well as for `Priority`.
  `ProcessCmd_Process_AVC` compares it against
  `CAVEPriorityQueue::GetClientPriority` and, on a mismatch, calls
  `SetClientPriority(pCmd->client_id, thatField)` (`0x2aebc`–`0x2aef0`), under
  the log string `"%s::%s:%d adjust piority %lld %lld | %d %d"` (`0x1223ae`,
  Apple's typo). The assert tag
  `"hClientQueue.GetClientPriority(pCmd->client_id, &tmpPriority) == true"`
  (`0x122368`) also names `+0x10` verbatim as `client_id`.
- **Header `+0x20` is read by the firmware**, once, at `0x2b034`, only on the
  failure path, where it is stored into an error record built by `0x311a8`.
  docs/07 records this as "not determined".

### 5.3 The data surfaces

Client-supplied surfaces (`InputData` and friends) live in a separate
`_S_AVE_SurfaceDataSet`, 96 bytes, mapped by `AVE_PrintOutSurfaces`
(`0xfffffe0008c79600`): slots at `+0x00`, `+0x08`, `+0x10`, `+0x18`, `+0x20`,
`+0x28`, `+0x30`, `+0x38` (2 entries), `+0x48`, `+0x50`, `+0x58` — 11 slots for
the 11 kinds `AVE_CreateDataSurfaces` imports. Their addresses reach the
firmware only through `SetDataInfo_FwBuf` -> PICMGMT, never through the
Buf_Set. Assigning names to those 11 offsets was **not done** (the print order
is not the creation order and was not cross-checked).

---

## 6. The `Reset` payload — solved

`AVE_CHM_MakeFwCmd_Reset(_S_AVE_CHM* x19, u64 cnt, u32 flags,
_S_AVE_TimeOut*, sCAveCmdReset* x23)` at `0xfffffe0008b6b9f4`:

```
b6ba98:  bzero(cmd, 0x13F08)
         ... 0x40-byte header, id 12 at +0x00 (b6bb9c) ...
b6bba0:  x8 = pipeline + 0xF1650              ; SurfaceSet + 0x2298 = InitParamsCopy[]
b6bba8:  x9 = chm[+0x2C] * 8
b6bbc4:  x0 = InitParamsCopy[chm[0x2C]]       ; AVE_Surface*
b6bbcc:  x0 = AVE_Surface::GetAddr(surf, 0)   ; kernel VA, 0xfffffe0008c6da34
b6bbd4:  w8 = client[+624]                    ; codec
b6bbe8:  memcpy(cmd + 0x48, x0, 0x3118)       ; codec == 1 (AVC)
b6bbf4:  memcpy(cmd + 0x48, x0, 0x13EC0)      ; codec == 2 (HEVC)
```

and the *last* thing `AVE_CHM_MakeFwCmd_Start_AVC` does
(`0xfffffe0008b675a4`–`0xb675e0`) is the mirror image:

```
b675a4:  x8 = pipeline + 0xF1650              ; InitParamsCopy[]
b675c8:  x0 = InitParamsCopy[chm[0x2C]]
b675d4:  x0 = AVE_Surface::GetAddr(surf, 0)
b675d8:  x1 = cmd + 0x68
b675dc:  mov w2, #0x3118
b675e0:  bl memcpy                            ; save the Start body
```

`AVE_CHM_MakeFwCmd_Start_HEVC` does the same with `0x13EC0` at
`0xfffffe0008b67dbc`–`0xb67dc8`.

`0x13EC0` is exactly `AVE_CalcBufSizeOfInitParamsCopy`'s constant return
(`mov w0,#0x3ec0; movk` at `0xfffffe0008b63208`, recorded in
[16-encode-surface-set.md](16-encode-surface-set.md) §4.3 as 81600 bytes). The
arithmetic closes on all three structures:

```
sizeof(sCAveCmdAvcStart)  = 0x68 + 0x3118  = 0x3180    (b670ac)
sizeof(sCAveCmdHevcStart) = 0x68 + 0x13EC0 = 0x13F28   (b677f8)
sizeof(sCAveCmdReset)     = 0x48 + 0x13EC0 = 0x13F08   (b6ba98)
```

**`sCAveCmdReset` is a verbatim replay of the `Start` command's parameter
block.** The `0x20`-byte difference between `sCAveCmdHevcStart` and
`sCAveCmdReset` — the thing that made them look suspiciously similar — is
exactly the `{FwClient addr,size}` + `{FwClientMem addr,size}` pair that Start
carries at `+0x48..+0x68` and Reset does not. Reset is always allocated at the
HEVC size; an AVC session fills only the first `0x3118` bytes of it.

The task's hypothesis — that Reset "carries a full buffer/surface table" — is
therefore **confirmed, and then some**: the `_S_AVE_Buf_Set` sits inside it, at
Reset offset `0x48 + (0x390 - 0x68) = 0x370`, with the same `0x21F0` layout as
§2.3. Reset replays the *entire* session setup, buffer table included.

This is what the 81,672 bytes are. [07-commands-abi.md](07-commands-abi.md) §7
lists "why `sCAveCmdReset` is 81672 bytes" as unexplained; it can be closed.

*What the firmware does with it* is covered in §6.1.

### 6.1 What the payload is called, and what the firmware does with it

The firmware's own names, from `__cstring` and from the symbol table, settle
what this block is:

| VA | name |
|---|---|
| `0x129ef2` | `pAvcInitCmd->sFwClientMem.iAddr != 0` |
| `0x130125` | `pHevcInitCmd->sFwClientMem.iAddr != 0` |
| `0x12eee6` | `%s::%s: pInitParams is NULL` |
| `0x1304f1` | `... pInitParams->sPSInfo.iNum %d header [%d] eType %d iLayerID %d iOffset %d iSize %d` |
| `0x136615` | `(psPSInfo->saBuf[iNum].sBuf.iOffset + psPSInfo->saBuf[iNum].sBuf.iSize) <= (int32_t)sizeof(psPSContext->iaPSData)` |
| `0x0580f4` | `CAVCController::Init(_E_AVE_EncType, sCAveInitCmdInternalParams, int, unsigned, unsigned*)` |
| `0x057254` | **`CAVCController::ResetBetweenPasses(sCAveInitCmdInternalParams)`** |
| `0x08b930` | `CHEVCController::ResetBetweenPasses(sCAveInitCmdInternalParams)` |
| `0x0814bc` | `CHEVCController::DebugInit(CAVEControllerHevcInitCmd*)` |

So the `Start` command is `sCAveCmd{Avc,Hevc}Start` on the wire and
`pAvcInitCmd` / `CAVEControllerHevcInitCmd` inside the firmware; its parameter
block is `pInitParams` / `sCAveInitCmdInternalParams`; and the same type is the
argument of `ResetBetweenPasses`. The kext's name for the save area,
`InitParamsCopy`, is literally that: a copy of `InitParams`. That the payload
is an init-parameter block rather than a bare address table is therefore
confirmed from both sides, by name.

`pInitParams->sPSInfo` carries the SPS/PPS/VPS bytes inline, described by
`saBuf[i].sBuf.{iOffset,iSize}` into a byte array `iaPSData`, built by
`AVE_PSInfo_Make(const unsigned char*, _E_AVE_PSType, int, int,
_S_AVE_PSContext*)` (`0x0e8f64`). That is another chunk of the 81 KB.

**The handler itself does not parse it.**
`CFlowControllerBase::ProcessCmd_Reset` (`0x28d64`) reads only `+0x08`, `+0x10`
(`0x28dcc`, `0x28df8`, `0x28e4c`) and `+0x24` (`0x28e50`) — maximum offset
`0x24` — then passes the raw command pointer as arg 5 to
`CAVEPriorityQueue::Enqueue` (`0x403fc`, `w4 = 12` at `0x28e74`), which stores
it verbatim into a ring slot (`str x4,[x19,#24]` at `0x3db84`) and sets a ready
flag (`0x3dcf0`). Nothing in that chain — `0x3f840` `IsClientRegistered`,
`0x401c8` `SetClientPriority`, `0x403fc` `Enqueue`, `0x3db84`, `0x559f0`
`AVE_CheckEncCmd` — dereferences the command at or beyond `+0x48`, and none of
them contains a 16-byte-stride loop over the payload.

The **positive control** for that negative is `Start`, which takes the same
shape of pointer and *does* read deep: `ProcessCmd_Start_AVC` (`0x29744`) reads
`+0x48`/`+0x50` (`0x297d8`/`0x297dc`) into
`CFlowControllerBase::CreateClient` (`0x31cc0`), then hands the raw pointer to
`CFlowControllerBase::ProcessCmd_Start` (`0x31e44`, call at `0x29858`), which
reads `cmd+0x3150` (`0x31efc`), `cmd+0x3154` (`0x31f34`) and memcpy's 36 bytes
from `cmd+0x3158` (`0x31f38`) for AVC, and `+0x13ef8`/`+0x13efc`/`+0x13f00` for
HEVC (`0x32088`–`0x320f8`). Those are exactly the tail fields the kext writes at
`cmd+0x2580 + 3024 = cmd+0x3150` (`strb w8,[x26,#3024]`,
`0xfffffe0008b67380`) — **both sides agree on a byte offset near the end of an
81 KB structure**, which is about as strong a cross-check as this project gets.

So Reset is queued, not decoded, on the synchronous path. Whether the
asynchronous worker that drains that queue re-runs the same `InitParams`
consumer — which the `ResetBetweenPasses(sCAveInitCmdInternalParams)` signature
strongly suggests — was **not traced**. Treat "Reset re-applies the Start
parameters, buffer table included" as the reading the evidence supports, and
the exact firmware path as unknown.

### 6.2 Size assertion, re-confirmed

`0x12569d` `insize == sizeof(struct sCAveCmdReset)`; the compare is
`mov w8,#0x3f08` (`0x28294`) / `movk w8,#1,lsl#16` (`0x28298`) /
`cmp w21,w8` (`0x2829c`), gating the call at `0x282b0`. There is **no**
equivalent `sizeof(struct sCAveCmdAvcStart)` / `...HevcStart` string anywhere
in the image (grepped over the whole blob) — Start's size gate is not
assert-backed, which is why [07-commands-abi.md](07-commands-abi.md) §2 could
only get the Start sizes from the host.

---

## 7. Corrections to [16-encode-surface-set.md](16-encode-surface-set.md) §5

1. **"`x19` is the CHM/command object" and "the Buf_Set sits at `+0x4e8` within
   [the command]" is wrong.** `x19` is the `_S_AVE_CHM` only. The Buf_Set is at
   `chm + 0x4E8`; it reaches the command at `cmd + 0x390`, via the `0x2460`-byte
   block copy at `0xfffffe0008b67308`. Likewise the second memcpy in that
   function (`0x2460` from `client+6200`) writes to `chm + 0x4C0`, not into the
   command (`0xfffffe0008b67190`).
2. **"Its full extent is at least `0x1FC8` bytes (`str xzr,[x8,#8080]` at
   `0xfffffe0008b7052c`)" is wrong.** Those stores are the null-surface
   zero-fill of one `0x40`-byte `MCTFOutput` entry, and the base register `x8`
   is `Buf_Set + group*0x200 + i*0x40 - 0x200`, not the Buf_Set
   (`0xfffffe0008b704dc`, `0xb70520`). The true size is `0x21F0` (§2.1).
3. The docs/16 partial table's `+0xba0` and `+0xfc0` rows are correct but
   unattributed; they are `TranscodedData` and `SrcNeighborInfo` (§2.3).
4. docs/16 §7's open item "the chroma half of the DPB Buf_Set entry" is closed
   (§3.2), as is "a complete Buf_Set offset map" (§2.3).

---

## 8. What a Linux driver has to do

1. Allocate the pools of §1 at the counts in
   [16-encode-surface-set.md](16-encode-surface-set.md) §4.5. For a baseline
   AVC encode the ones that must be non-`NULL` in the Buf_Set are: `Recon`
   (2 x 17), `CodedData` (30), `CodedHeader` (30), `Colocated` (2 x 17),
   `SrcNeighbor{Info,Pixel,Data,FwData}` (4 each), `EntropyCoding` (16 x 4),
   `MBInputCtrl` (2), `LFSRef`/`LFSResult`/`LRSResult`. `ProtectedData`,
   `TranscodedData`, `StaticArea*`, `HSCOutput`, `LRSNeighborMV` and
   `MCTFOutput` all have count 0 on DevType 9/10 or on the non-DRM path and can
   be left zero — `SetFwBuf` skips `NULL` slots (`cbz x0` at
   `0xfffffe0008b6fd58` and each equivalent).
2. Build the `0x21F0` Buf_Set at §2.3's offsets and place it at Start-command
   offset `0x390`.
3. Publish `FwClient` and `FwClientMem` at Start-command `+0x48`/`+0x58`.
4. Per frame, fill `AVE_PICMGMT_PARAMS` with the actual addresses (§5.2) and
   send it at Process-command `+0x12C0`.
5. Keep a copy of the Start body so `Reset` can replay it (§6) — or just
   rebuild it, since it is deterministic.

---

## 9. Still open

- The remaining `0x2460`-byte engine parameter block outside the Buf_Set
  (`chm+0x4C0..+0x4E8` and `chm+0x26D8..+0x2920`), and the three other
  client-derived blocks the Start command carries (`+0x27C8`, `+0x291C`,
  `+0x2FD0`). None of them were decoded. `pInitParams->sPSInfo` (§6.1) is
  somewhere in there.
- The full field map of `AVE_PICMGMT_PARAMS` / `pPicParams`. §5.2 and §5.2.1
  name about twenty offsets and a dozen member names out of `0x5118` bytes, but
  the two are not yet joined up — no `sRef.Y_L0_MSB[]` *offset* is known.
  `AVE_CHM_SetDataInfo_Frame` (`0xfffffe0008b682bc`), `_Header` (`0xb68858`) and
  `_RC` (`0xb69110`) fill the rest and were not read. The firmware never touches
  command `+0x1690..+0x29F8` (§5.4), so roughly `0x1368` bytes of the block are
  inert on this path.
- `_S_AVE_DPB_Set` — its size and the meaning of its members beyond
  `+0x00/+0x30/+0x38/+0x40/+0x48` (§5.2). `AVE_CHM_PrepareDataInfo`
  (`0xfffffe0008b69a28`) builds it and was not read.
- The `_S_AVE_SurfaceDataSet` offset -> surface name mapping (§5.3) — the
  eleven client-supplied surfaces, `InputData` included.
- Whether `sRecon.Y_LSB` is `out[0]` or `out[1]` (§3.2.1). The grouping is
  confirmed; the order within each pair is inferred.
- Why `SetFwBuf` publishes only `Recon` set 0 while publishing both `LFSRef`
  sets (§3.3), and the three re-writing outer loops flagged in §2.4.
- What `client[+6209]` bit 3 means beyond "use `ProtectedData`", and what
  distinguishes client type 4 (§2.4).
- Which firmware function re-applies the Reset payload. §6.1 traces Reset only
  to the queue; `C{AVC,HEVC}Controller::ResetBetweenPasses` (`0x057254`,
  `0x08b930`) is the obvious candidate but the link was not traced.
