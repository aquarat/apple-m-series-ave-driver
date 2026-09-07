# Buffer publication — how the firmware learns where the buffers are

*The `_S_AVE_Buf_Set` slot map, the DPB publication mechanism, what the 81 KB
`Reset` payload actually contains, and how a per-frame `Process` command names
its input and output buffers.*

Everything here was read out of `AppleAVE2.kext` in `data/blobs/kc.macho` and
out of `data/blobs/ave_h13c.bin`, with `tools/disas.py`. Every constant carries
the VA of the instruction it came from. Anything not read directly out of an
instruction is marked **inferred** or **unknown**, per
[00-methodology.md](00-methodology.md).

This document supersedes two claims in
[16-encode-surface-set.md](16-encode-surface-set.md) §5 — see §7.

---

## 0. Summary

There are **three** publication events, not one:

| when | vehicle | contents |
|---|---|---|
| `Start` (once per session) | `_S_AVE_Buf_Set` at `chm+0x4E8`, copied into `sCAveCmd{Avc,Hevc}Start+0x390` | the whole buffer **pool** — every internal and external-out surface, ~19 kinds, `{u64 dartAddr; u32 size; u32 pad}` entries |
| `Start` (same command) | `cmd+0x48`/`+0x58` | `FwClient` and `FwClientMem`, as bare `{addr,size}` pairs outside the Buf_Set |
| `Process` (every frame) | `AVE_PICMGMT_PARAMS`, copied into `sCAveCmdAvcProcess+0x12C0` | the DART addresses of *this frame's* input, output, recon, colocated and reference buffers, chosen by index out of the pools above |
| `Reset` | `sCAveCmdReset+0x48` | a verbatim replay of the `Start` parameter block, saved in the `InitParamsCopy` surface |

So the model is **not** "publish once, then index by slot". The pool is
published at Start so the firmware can validate and pre-map it; the per-frame
command then re-states the actual addresses it wants used. A driver must do
both.

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

Regions are exactly contiguous with no gaps, and 26 regions cover the 26 surface
kinds that live in a `SurfaceSet`. (The other nine InfoSet slots — `InputData`,
`InputScaledData`, `DirectRecon`, `MultiPassStats`, `MCTFRef`, `GGMRef`,
`GGMStats`, `GGMOutput`, `DMVOutput` — are *data* surfaces and live in a
separate 96-byte `_S_AVE_SurfaceDataSet`; see §5.3.)

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

`MakeFwCmd_Start_AVC` (`0xfffffe0008b66fxx`) does, in order:

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

### 2.2 Entry format

The plain entry is 16 bytes, built by the recurring idiom (e.g.
`0xfffffe0008b6fd5c`–`0xb6fd74`):

```c
struct _S_AVE_Buf { uint64_t dartAddr; uint32_t size; uint32_t pad; };
```

`dartAddr` = `AVE_Surface::GetDARTAddr(surf, chm->[0x28], 0)`
(`0xfffffe0008c6da54`), `size` = `AVE_Surface::GetSize(surf)`
(`0xfffffe0008c6dbbc`) — the surface's *actual* size, not the InfoSet's
requested size. Four surface kinds use wider entries: see the table.

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
   `cbz w8, 0xfffffe0008b70e08` at `0xfffffe0008b70038`, where `w8` comes from a
   per-unit table (`chm[+0x20][+20]*0x70 + chm[+0x18] + 756`,
   `0xfffffe0008b7001c`–`0xb70028`). Zero -> the four-entry loops
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

### 3.3 Count

**2 groups of 17** entries, `0x20` each, at Buf_Set `+0x000 .. +0x440`. The
group index is the `Recon` *layer* (SurfaceSet stride `0x88`), `x28 in {0,1}`
via the unrolled-two idiom at `0xfffffe0008b6ff08`/`0xb6ff7c`/`0xb6ff88`; the
`Recon` *set* index is fixed at 0. `LFSRef`, by contrast, publishes both sets
(§2.3). 17 is the hard DPB maximum already established in
[16-encode-surface-set.md](16-encode-surface-set.md) §4.5.

---

## 4. The `Start` command layout

`AVE_CHM_MakeFwCmd_Start_AVC(chm, u64 cnt, u32 flags, _S_AVE_TimeOut*,
sCAveCmdAvcStart* x23)`:

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

`MakeFwCmd_Start_HEVC` (`0xfffffe0008b676xx`) is the same shape with the block
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
| `0x4548`–`0x4560` | **the current recon picture**: all four `GetFwDPBBuf` outputs, stored as `q0,q1` | `0xfffffe0008b74fb8`, `0xb74fc0`; call `0xb73f5c` |
| `0x4568` | the `Colocated` buffer of the *same* DPB slot | `0xfffffe0008b7501c` |
| `0x4548`+ per reference | `_S_AVE_DPB_Set[+0x00,+0x38,+0x40,+0x48]` -> `0x4F18`, `0x4F48`, `0x4F58` | `0xfffffe0008b75060`, `0xb75090`, `0xb750c8` |
| `0x4548`.. input | input frame plane addresses from a stack YUV-info struct, plus a crop offset | `0xfffffe0008b72a7c`–`0xb72a90`, `0xb72c40`–`0xb72c50` |
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

*Caveat:* AVC transmits only the first `0x5118` bytes of `AVE_PICMGMT_PARAMS`
(`0xfffffe0008b6a55c`), so any PICMGMT offset `>= 0x5118` — e.g. the
`str x0,[x23,#29536]` at `0xfffffe0008b73adc` — is HEVC-only
(`sCAveCmdHevcProcess` is `0xB1C0`).

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

`AVE_CHM_MakeFwCmd_Reset(chm, u64 cnt, u32 flags, _S_AVE_TimeOut*,
sCAveCmdReset* x23)` at `0xfffffe0008b6b9f4`:

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

and the *last* thing `MakeFwCmd_Start_AVC` does
(`0xfffffe0008b675a4`–`0xb675e0`) is the mirror image:

```
b675a4:  x8 = pipeline + 0xF1650              ; InitParamsCopy[]
b675c8:  x0 = InitParamsCopy[chm[0x2C]]
b675d4:  x0 = AVE_Surface::GetAddr(surf, 0)
b675d8:  x1 = cmd + 0x68
b675dc:  mov w2, #0x3118
b675e0:  bl memcpy                            ; save the Start body
```

`MakeFwCmd_Start_HEVC` does the same with `0x13EC0` at
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

### 6.1 Firmware side

`CFlowControllerBase::ProcessCmd_Reset` (`0x28d64`) itself touches only the
header: `x19 = this[1472]` is the command (`0x28dbc`), then `ldp x8,x1,[x19,#8]`
= `+0x08`/`+0x10` (`0x28dcc`), `ldr x1,[x19,#16]` (`0x28df8`, `0x28e4c`),
`ldr w2,[x19,#36]` = `+0x24` (`0x28e50`). It hands the command pointer on as
`x5` to `0x403fc` with `w4 = 12` (`0x28e74`/`0x28e78`) after
`0x3f840` and `0x401c8`. It does not parse the payload inline. Compare
`ProcessCmd_Start_AVC` (`0x29744`), which reads `+0x48`/`+0x50`/`+0x18`/`+0x1c`
(`0x297d8`–`0x297e0`) and calls `0x31cc0`.

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
  `+0x2FD0`). None of them were decoded.
- The full field map of `AVE_PICMGMT_PARAMS`. §5.2 names about fifteen offsets
  out of `0x5118` bytes; `AVE_CHM_SetDataInfo_Frame` (`0xfffffe0008b682bc`),
  `_Header` (`0xb68858`) and `_RC` (`0xb69110`) fill the rest and were not read.
- `_S_AVE_DPB_Set` — its size and the meaning of its members beyond
  `+0x00/+0x30/+0x38/+0x40/+0x48` (§5.2). `AVE_CHM_PrepareDataInfo`
  (`0xfffffe0008b69a28`) builds it and was not read.
- The `_S_AVE_SurfaceDataSet` offset -> surface name mapping (§5.3).
- Why `SetFwBuf` publishes only `Recon` set 0 while publishing both `LFSRef`
  sets (§3.3), and the three re-writing outer loops flagged in §2.4.
- What `client[+6209]` bit 3 means beyond "use `ProtectedData`", and what
  distinguishes client type 4 (§2.4).
- Which firmware function consumes the Start/Reset parameter block. §6.1 traces
  Reset only as far as `0x403fc`.
