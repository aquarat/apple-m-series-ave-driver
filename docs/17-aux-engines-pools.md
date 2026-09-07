> ## Verification note — RESOLVED; this document was wrong
>
> The slot-mapping disagreement recorded below has been settled in
> [19-infoset-slot-map.md](19-infoset-slot-map.md). **This document's claim was
> wrong**: index 0 (`InputData`) *does* have a slot, and the six skipped indices
> are 3 (`UCInfo`) and 27..31 (`IOPIPC`, `FwImage`, `FwLog`, `FwHeap`, `FwIPC`).
> [16-encode-surface-set.md](16-encode-surface-set.md) had it right.
>
> Also corrected: this document left `_E_AVE_WorkType`'s `Enc` value as "0
> and/or 1, not read directly". It is **1**, read from the
> `AVE_MD_SVE::CalcSurfaceInfo` dispatch at `0xfffffe0008c8c83c`, where 0 falls
> to the error arm.
>
> The original note follows.
>
> ## (superseded) InfoSet slot mapping is not fully settled
>
> This document and [16-encode-surface-set.md](16-encode-surface-set.md) were
> written independently and **agree** on the load-bearing facts: the InfoSet is
> 35 entries of 48 bytes (`0x690`), and `+0x18` is a count with `+0x1c` a size
> (proved by `AVE_CreateInternalSurfaces` creating `[+0x18]` surfaces and
> rejecting any smaller than `[+0x1c]`). Two independent derivations reaching
> the same layout is strong.
>
> They **disagree on which surface indices have no slot.** This document says
> indices 0 and 27–31 are skipped; doc 16 says `UCInfo`, `IOPIPC`, `FwImage`,
> `FwLog`, `FwHeap` and `FwIPC`. Both are six, and both agree 29/30/31
> (`FwLog`/`FwHeap`/`FwIPC`) are skipped, but they differ at the low end.
>
> What was measured directly: `AVE_PrintSurfaceInfoSet` calls
> `AVE_GetSurfaceCfg` with these indices, in this order —
>
> ```
> 5..26, then 32, 33, 34, 35
> ```
>
> — 26 calls, which is fewer than 35, so `Print` covers only a subset and does
> not by itself decide the question. This document's formula
> (`48*(idx-1)` for 1..26, `48*(idx-6)` for 32..40) does total 35 slots and is
> consistent with that measured range, but indices 1–4 are unverified either
> way. **Treat the skip list as unresolved** until someone reads the writers for
> the low indices directly.

# Auxiliary work engines and the pool allocators

Read out of `AppleAVE2.kext` in `data/blobs/kc.macho` with `tools/disas.py`.
Every constant carries the VA of the instruction it came from. Anything not
read directly out of the image is marked **inferred** or **unknown**.

Scope: the five non-encode `AVE_Work_*_CalcSurfaceInfo` functions and the three
pool allocators. `AVE_Work_Enc_CalcSurfaceInfo` and the `CalcFrameSize`
primitives are covered elsewhere; they are referenced here only where they bear
on a question below.

---

## 0. `_S_AVE_SurfaceInfoSet` — the structure all five fill

All six `AVE_Work_*_CalcSurfaceInfo(_S_AVE_Client*, _S_AVE_DLB_Unit*,
_S_AVE_SurfaceInfoSet*)` begin with `memset(arg3, 0, 0x690)` (LRME:
`mov w1, #0x690` at `0xfffffe0008cb10d0`; Enc: `0xfffffe0008ca9a64`).

**`0x690` = 1680 = 35 × 48.** The set is an array of 35 fixed 48-byte entries,
indexed by `_E_AVE_SurfaceIdx` (the enum tabulated in `12-dart-surfaces-mmio.md`
§2.1) with **indices 0 and 27–31 absent**:

```
byte offset(idx) = 48 * (idx - 1)        for idx =  1 .. 26
                 = 48 * (idx - 6)        for idx = 32 .. 40
```

Derivation is not a guess. `AVE_PrintSurfaceInfoSet` (`0xfffffe0008c7454c`)
walks the set surface by surface, and for every surface it calls
`AVE_GetSurfaceCfg(N)` (`0xfffffe0008ca8cfc`) immediately before loading that
surface's fields. Extracting the (N, offset) pairs from the whole function gives
exactly the mapping above, with a contiguous run 5→26 at 192→1200 and then a
jump straight to idx 32 at 1248 — i.e. 27..31 (`IOPIPC`, `FwImage`, `FwLog`,
`FwHeap`, `FwIPC`) are skipped, and 35 entries × 48 = 1680 closes the struct
exactly. The mapping is then independently confirmed by every one of the ~200
stores in the six `CalcSurfaceInfo` functions landing on a field whose callee is
named `AVE_CalcBuf*Of<that same surface>`.

### The 48-byte entry

| off | width | filled by | evidence |
|---|---|---|---|
| `+0x00` | u64 | *nobody* — flags, see below | read at `0xfffffe0008c79904` |
| `+0x08` | int | `AVE_CalcBufModeNumOf<S>` | `0xfffffe0008cb1294` (LFSResult) |
| `+0x0c` | int | `AVE_CalcBufTypeNumOf<S>` | `0xfffffe0008cb1218` (LFSRef) |
| `+0x10` | int | `AVE_CalcBufSetNumOf<S>` | `0xfffffe0008bfa80c` (Recon) |
| `+0x14` | int | `AVE_CalcBufLayerNumOf<S>` | `0xfffffe0008cb1224` (LFSRef) |
| `+0x18` | int | `AVE_CalcBufNumOf<S>` — **buffer count** | `0xfffffe0008cb1250` |
| `+0x1c` | int | `AVE_CalcBufSizeOf<S>` — **bytes per buffer** | `0xfffffe0008cb1288` |
| `+0x20`..`+0x2c` | 4×int | sub-region sizes, written through the `int*` ("`Pi`") out-parameter that several `CalcBufSizeOf` take | `0xfffffe0008b61878`–`0xb6187c` writes `[x8+0..12]`; the caller passes `x19 + 0x320` at `0xfffffe0008cb1254` |

`+0x00` is a **flags word, not a size**. `AVE_CreateInternalSurfaces`
(`0xfffffe0008c79894`) reads it and ORs it into the static
`gs_saAVE_SurfaceCfg[idx].flags`, then clears bit 16:

```
0xfffffe0008c79904  ldr  x8, [x26, #192]        ; InfoSet[MBInputCtrl] +0x00
0xfffffe0008c79908  ldp  x10, x9, [x0]          ; AVE_GetSurfaceCfg(5)
0xfffffe0008c7990c  orr  x8, x8, x9
0xfffffe0008c79910  and  x27, x8, #0xfffffffffffeffff
0xfffffe0008c79914  ldp  w20, w28, [x26, #216]  ; +0x18 count, +0x1c size
```

and then creates `w20` surfaces, rejecting any whose `AVE_Surface::GetSize()` is
below `w28` (`0xfffffe0008c79968`). So `+0x18`/`+0x1c` are unambiguously
*count* and *minimum bytes*, which satisfies the "is this constant actually used
as a size?" test for everything below.

---

## 1. `_E_AVE_WorkType` and `_E_AVE_ClientType` are different enums

Each `CalcSurfaceInfo` passes a literal `_E_AVE_WorkType` as the first argument
of `AVE_CalcBufNumOfCodedHeader`:

| work type | value | `mov w0, #N` at |
|---|---|---|
| Enc | 0 and/or 1 (not read directly; the aux values leave 0/1 free) | — |
| LRME | **2** | `0xfffffe0008cb11ec` |
| MCTF | **3** | `0xfffffe0008bfa770` |
| MSC | **4** | `0xfffffe0008ba1244` |
| GGM | **5** | `0xfffffe0008b3f614` |
| DMV | **6** | `0xfffffe0008c37750` |

This is the same numbering as `_S_AVE_CHM + 0x34` in
`11-interrupts-bringup.md` §5, which routes `ProcessIntr_OutputData`. That field
is therefore a `_E_AVE_WorkType`.

`_E_AVE_ClientType` is a *different* enum. `AVE_DevCap_FindSEntry`
(`0xfffffe0008baa070`), `AVE_DevCap_FindResolution` (`0xfffffe0008baa12c`) and
`AVE_DevCap_FindPixelFmt` (`0xfffffe0008baa1f0`) all share one compare chain
(`0xfffffe0008baa088`–`0xbaa0ac` in `FindSEntry`) mapping client type to a slot
in the per-DevID capability row:

| `_E_AVE_ClientType` | slot | row offset |
|---|---|---|
| 1 + EncType 1 | 4 | `+0x38` (AVC) |
| 1 + EncType 2 | 5 | `+0x40` (HEVC) |
| 2 | 3 | `+0x30` (LRME) |
| 3 | 2 | `+0x28` (MCTF) |
| 4 | 1 | `+0x20` (GGM) |
| 5 | 0 | `+0x18` (DMV) |
| 0, >5 | — | returns `NULL` |

**There is no client type for MSC.** No `SEntry_MSC_*` symbol exists for any
SoC, and no slot in the row is left over.

---

## 2. Is GGM/DMV really dead on M1? — a test that discriminates

`11-interrupts-bringup.md` §6 asserted this from symbol names alone and got part
of it wrong ("no `SEntry` at all for MSC, LRME or MCTF" — there are 105
`SEntry_LRME_*` and 16 `SEntry_MCTF_*` symbols). The claim is retested here by
reading the table itself.

The row lookup at `0xfffffe0008ba9ea8` (`kext-symbols.txt` gives its name only
as `_AVE_DevCap_Find`, truncated; `AVE_DevCap_FindCEntry` is the next function,
at `0xfffffe0008ba9edc`) indexes **`0xfffffe0007edba00`**, stride **`0x48`**
(`mov w8, #0x48` at `0xfffffe0008ba9eac`), bound `devid < 35` (`cmp w0, #0x23`,
`0xfffffe0008ba9ed0`). Resolving the chained-fixup pointers at
`row + 0x18 + slot*8` for all 35 rows:

| DevID | SoC | slot0 DMV | slot1 GGM | slot2 MCTF | slot3 LRME | slot4 AVC | slot5 HEVC |
|---|---|---|---|---|---|---|---|
| 11 | 6000 | — | — | — | **yes** | yes | yes |
| 12 | 6001 | — | — | — | **yes** | yes | yes |
| 19 | 8120 | — | — | **yes** | yes | yes | yes |
| 21 | 6030 | — | — | **yes** | yes | yes | yes |
| 32 | 8150 | **yes** | **yes** | yes | yes | yes | yes |
| 34 | 8160 | **yes** | **yes** | yes | yes | yes | yes |

**The test discriminates.** The identical read of the identical field yields
non-NULL DMV/GGM pointers at rows 32–34 and non-NULL MCTF from row 19 onward,
and NULL at rows 11/12. So the NULLs at 11/12 are a property of the data, not of
the method.

The NULL is also load-bearing, not merely informational.
`AVE_DMV_CheckResolution(_E_AVE_DevID, int, int)` (`0xfffffe0008baa5f8`) calls
`AVE_DevCap_FindResolution(devid, ClientType=5, 0)` (`mov w1, #5` at
`0xfffffe0008baa610`) and on a NULL return (`cbz x0`, `0xfffffe0008baa61c`)
returns **`-1002`** (`0xfffffe0008baa698`). The same `cbz x0` → error shape is
in `AVE_Enc_CheckResolution` (`0xfffffe0008ba2148`/`0xba214c`).

**Conclusions, per engine, for `t6000`/`t6001`:**

| engine | advertised on M1 Pro/Max? |
|---|---|
| LRME | **yes** — `SEntry_LRME_6000` / `_6001`, resolved at `0xfffffe0007edbd48` / `0xfffffe0007edbd90` |
| MCTF | **no** — slot 2 is NULL; first appearance is DevID 19 (`8120`) / 21 (`6030`) |
| GGM | **no** — slot 1 NULL below DevID 32 (`8150`) |
| DMV | **no** — slot 0 NULL below DevID 32 (`8150`) |
| MSC | no capability entry exists on **any** SoC |

So `11-interrupts-bringup.md`'s bottom line for GGM and DMV survives; its
reasoning about LRME and MCTF does not, and its statement that MCTF is
unavailable was accidentally right for the wrong reason.

`AVE_Work_MCTF_CalcSurfaceInfo` / `_GGM_` / `_DMV_` / `_MSC_` themselves have no
DevID guard — they are reachable code that would compute sizes if called. What
is unreachable on M1 is the *client type* that would cause them to be called.
Where MSC is dispatched from was **not** determined; it has no client type and
no capability entry, so how a `WorkType == 4` unit is ever queued is **unknown**.

---

## 3. What each auxiliary engine needs

Surfaces below are the ones whose `_S_AVE_SurfaceInfoSet` entry the function
writes. Cross-checked by extracting every `str w*, [x19, #N]` in each function
and decoding `N` with the §0 mapping.

| surface (idx) | LRME | MCTF | MSC | GGM | DMV |
|---|:-:|:-:|:-:|:-:|:-:|
| MBInputCtrl (5) | | ● | ● | | |
| Recon (6) | | ● | | | |
| CodedData (8) | | ● | ● | | |
| **CodedHeader (9)** | ● | ● | ● | ● | ● |
| SliceHeader (10) | | ● | | | |
| MBStats (12) | | ● | | | |
| Colocated (15) | | ● | | | |
| HSCOutput (16) | | ● | | | |
| **LFSRef (17)** | ● | ● | ● | ● | |
| LRSNeighborMV (18) | | ● | ● | | |
| **LFSResult (19)** | ● | ● | ● | ● | |
| **LRSResult (20)** | ● | ● | ● | ● | |
| SrcNeighbor{Info,Pixel,Data,FwData} (21–24) | | ● | | | |
| TranscodedData (25) | | ● | | | |
| EntropyCoding (26) | | ● | | | |
| **FwClient (32)** | ● | ● | ● | ● | ● |
| InitParamsCopy (34) | | ● | | | |
| MCTFOutput (35) | | ● | | | |

Notes read off the disassembly:

- **DMV asks for almost nothing** — only `CodedHeader` and `FwClient`
  (`0xfffffe0008c376e0`–`0xc37784`, the whole function is `0xc0` bytes). Its
  actual output surface `DMVOutput` (idx 40) is **not** sized here.
- **GGM does not size `GGMRef`/`GGMStats`/`GGMOutput`** (idx 37/38/39) either.
  Where those are sized was not found; **unknown**.
- GGM's set is byte-for-byte the same surface list as LRME's, with different
  literal arguments.
- **MCTF is the heaviest**, and is the only one of the five that sizes
  `MCTFOutput`. `AVE_CalcBufNumOfMCTFOutput` is not called; the count is the
  literal **2** (`mov w8, #2; str w8, [x19, #1416]`, `0xfffffe0008bfac84`).
- MSC's surface list is LRME's plus `MBInputCtrl`, `CodedData` and
  `LRSNeighborMV`. That is the only structural information about MSC recovered
  here. **No acronym expansion for MSC or GGM appears in either binary**; none
  is proposed.

---

## 4. Can an encode proceed without LRME? — no, and it does not have to

`AVE_Work_Enc_CalcSurfaceInfo` (`0xfffffe0008ca9a28`) writes, among its 67
stores, offsets **780, 788, 792, 796** (LFSRef), **872, 876, 888, 892**
(LFSResult) and **920, 924, 936, 940** (LRSResult) — i.e. *every* surface that
`AVE_Work_LRME_CalcSurfaceInfo` sizes is also sized by the encode path, using
the same `AVE_CalcBuf*Of{LFSRef,LFSResult,LRSResult}` primitives.

So the answer to "can an encode proceed without LRME's buffers" is that the
question does not arise: **the LRME scratch surfaces are part of the ordinary
encode surface set**, allocated by `AVE_CreateInternalSurfaces` for every
encoding client. The standalone `AVE_Work_LRME_*` path
(`_E_AVE_ClientType == 2`) exists for a client that wants motion estimation
*without* encoding.

Supporting cross-reference (naming, so **inferred** but strongly): `03-protocol.md`
records the firmware classes `CLRMEFSController` and `CLRMERSController` and the
pipeline states `LRME_FS_START` / `LRME_RC_START`. The surface names
**LFS**Ref / **LFS**Result / **LRS**Result / **LRS**NeighborMV are the buffers of
those two controllers (LRME Full Search, LRME R… Search). That accounts for why
they appear in the encode set and why the pipeline runs `LRME_FS_START` before
`PIPE_START`.

A Linux driver therefore has to allocate LFSRef + LFSResult + LRSResult (and,
per the encode path, LRSNeighborMV/HSCOutput etc.) as internal buffers before an
encode can be started. It does not need to submit a separate LRME work item.

---

## 5. Size formulas for LRME's surfaces

`_E_AVE_DevType` is **not** `_E_AVE_DevID`. `AVE_DevInfo::Init`
(`0xfffffe0008baa724`) copies the first 8 bytes of the capability row into
`AVE_DevInfo+8` (`ldr d0,[x22]` / `str d0,[x21,#8]`, `0xfffffe0008baa774`–
`0xbaa778`), and `AVE_DevInfo::GetDevType` is `ldr w0, [x0, #12]`
(`0xfffffe0008bab790`) — i.e. the `u32` at **row + 4**. From the table dump:

| SoC | DevID | **DevType** | ChipType (row+8) |
|---|---|---|---|
| t6000 | 11 | **9** | 6 |
| t6001 | 12 | **10** | 7 |

Every threshold below is evaluated at DevType 9/10.

### CodedHeader (idx 9)

`AVE_CalcBufNumOfCodedHeader(_E_AVE_WorkType wt, …)` (`0xfffffe0008b5fb2c`):

```
if ((unsigned)(wt - 5) < 2) return 10;      ; wt in {GGM, DMV}   0xb5fb30-0xb5fb3c
if (wt == 2)                return 4;       ; LRME               0xb5fb44-0xb5fb4c
tail-call AVE_CalcBufNumOfCodedData(...)    ;                    0xb5fb84
```

`AVE_CalcBufSizeOfCodedHeader()` (`0xfffffe0008b5fb88`) is a constant:

```
mov w0, #0xc000    ; 0xfffffe0008b5fb8c
```

**49152 bytes, always.** (Used as the `+0x1c` size field, so it is a size.)

### FwClient (idx 32)

`AVE_CalcBufNumOfFwClient(int n)` (`0xfffffe0008b63214`) is the identity — the
body is `bti c; ret`. LRME/GGM/DMV/MSC all pass 1.

`AVE_CalcBufSizeOfFwClient(int n)` (`0xfffffe0008b6321c`):

```
w8 = 0x0013C000                     ; movz/movk 0xb63220-0xb63224
return (n == 0) ? w8 : n;           ; 0xb63228-0xb6322c
```

LRME passes the mutable `__DATA` word at **`0xfffffe000c69aaf0`**
(`0xfffffe0008cb1344`), which is **0** in the shipped image → the default
**`0x13C000` = 1 294 336 bytes**. Whether anything writes that global at runtime
was not determined.

### LFSRef (idx 17)

```
AVE_CalcBufTypeNumOfLFSRef(bool b)     0xb614fc   -> b ? 2 : 1
AVE_CalcBufLayerNumOfLFSRef(int n)     0xb61510   -> min(n, 2)
```

`AVE_CalcBufSizeOfLFSRef(DevType, WorkType, EncType, w, h, ChromaFmt, bool f,
int, int, int* out4)` (`0xfffffe0008b615a0`) branches `wt==5` / `wt==2` /
default at `0xb615dc`–`0xb615e8`; the LRME arm additionally splits on
`DevType >= 30` (`cmp w19, #0x1e`, `0xb615ec`). **On M1 the `DevType < 30` arm at
`0xfffffe0008b61764` runs:**

```
s  = f ? 2 : 0                                    ; 0xb61770-0xb61778
bw = ceil( ceil((w << s) / 16) / 4 )              ; 0xb6177c-0xb61790
bh = ceil( ceil((h << s) / 16) / 4 )              ; 0xb61794-0xb617a8
sz = align_up(bw * bh * 256, 512)                 ; 0xb617ac-0xb617b8
d  = (DevType > 16 && ChromaFmt != 0) ? 1 : 0     ; 0xb617bc-0xb617c4  (0 on M1)
return sz << d
```

The three other sub-region sizes (`w27`, `w28`, `w20`) are forced to zero on
this arm (`0xb61764`–`0xb6176c`); all four are written through the `int*`
out-parameter into entry fields `+0x20..+0x2c` (`0xb61878`–`0xb6187c`) and the
return value is their sum (`0xb61880`–`0xb6188c`).

1920×1080, `f = 0`: `bw = 30`, `bh = 17` → **130 560 bytes**.

`f` is bit 0 of `_S_AVE_Client + 15288` (`ldrb w8,[x23,#356]` where
`x23 = client + 0x3a54`, `0xfffffe0008cb10c4` / `0xcb11bc`); its meaning is
**unknown**.

### LFSResult (idx 19)

```
AVE_CalcBufModeNumOfLFSResult(int n)   0xb61c88   -> (n == 1) ? 17 : 1
AVE_CalcBufTypeNumOfLFSResult(bool b)  0xb61c9c   -> b ? 2 : 1
AVE_CalcBufNumOfLFSResult(wt,a,b,c)    0xb61cb0:
    c ? 10 : (wt in {5,6}) ? 10 : (wt == 2) ? 4 : (a == 0 ? 3 : 1)
```

LRME passes `wt=2, c=0` (`mov w3, #0` at `0xfffffe0008cb12b4`) → **4 buffers**.

`AVE_CalcBufSizeOfLFSResult(wt, DevType, EncType, w, h, bool g)`
(`0xfffffe0008b61ce8`); LRME arm `0xb61d40`, and for `DevType < 30` it lands at
`0xfffffe0008b61f40`:

```
s = g ? 2 : 0                                     ; 0xb61d44-0xb61d4c
W = ceil((w << s) / 16)                           ; 0xb61f40-0xb61f44
H = ceil((h << s) / 16)                           ; 0xb61f48-0xb61f4c
H4 = ceil(H / 4)                                  ; 0xb61f68-0xb61f6c
                    DevType <  9 : align_up(((W+1)>>1) * 96, 64) * H4        (0xb61f50-0xb61f70)
size =              9 <= DT < 23 : align_up(W * 64, 128) * H4 + 1024         (0xb61f74-0xb61f84)
                    DevType >= 23: align_up(((W+1) & ~1) * 80, 64) * H4 + 1024 (0xb61f90-0xb61fa0)
return size << (DevType < 9 ? 1 : 0)              ; 0xb61fac-0xb62054
```

**M1 uses the middle line.** 1920×1080, `g = 0`: `W = 120`, `H = 68`,
`H4 = 17` → `align_up(7680,128) * 17 + 1024` = **131 584 bytes** per buffer,
4 buffers.

### LRSResult (idx 20)

```
AVE_CalcBufModeNumOfLRSResult(int n)   0xb620a4   -> (n == 1) ? 17 : 1
AVE_CalcBufTypeNumOfLRSResult(bool b)  0xb620b8   -> b ? 2 : 1
AVE_CalcBufNumOfLRSResult(DevType, wt, bool e, bool f)   0xb620cc:
    if (wt == 5) return 10;                                        ; 0xb620f8
    n = f ? 10 : ((wt == 2) ? 4 : 3);                              ; 0xb620d8-0xb620ec
    return (DevType > 8 && e) ? n : 0;                             ; 0xb620d0, 0xb620f0
```

LRME passes `wt=2, f=0` → **4 buffers if `e`, else 0.**

`AVE_CalcBufSizeOfLRSResult(DevType, wt, bool e, w, h, bool g)`
(`0xfffffe0008b62104`):

```
if (wt == 5) return 0x40000;              ; GGM: 256 KiB flat, 0xb62154
if (DevType < 9) return 0;                ; 0xb621c4-0xb621c8
if (!e) return 0;                         ; 0xb621cc
s  = g ? 2 : 0                            ; 0xb621f0-0xb621f8
w' = w << s ;  h' = h << s
cols = align_up(w', 64)                                        ; 0xb62218-0xb6221c
      DevType > 29 : ceil(h'/128) * (((w'+127)>>1) & ~63) * 40 + cols   ; 0xb62200-0xb62228
size =
      DevType <= 29: (((h' + 255) >> 6) & ~3) * cols                    ; 0xb6222c-0xb62238
return size << ((wt == 2) ? 3 : 1)         ; 0xb62244-0xb62250
```

**M1 uses the second line, with the ×8 LRME shift.** 1920×1080, `g = 0`:
`((1080+255)>>6) & ~3 = 20`, `cols = 1920` → `38 400 << 3` = **307 200 bytes**
per buffer.

`e` and `g` come from the same unnamed client bits as `f` above
(`0xfffffe0008cb11a8`–`0xcb11c8`); their meaning is **unknown**, so the LRME
totals are conditional. Everything upstream of that conditional is confirmed.

> Trap check: `AVE_CalcBufSizeOf{LFSRef,LFSResult,LRSResult}` all contain
> `mov w0, #0x74 ; mov w1, #4 ; bl 0xfffffe0008c46348` (e.g.
> `0xfffffe0008b618ac`). `0xfffffe0008c46348` is
> `AVE_Log_CheckLevel(unsigned, char)` — `0x74` is a **log subsystem id**, not a
> size. Likewise `0x38`/`0x39` in the pool code (§7) and `0x5d` in `AVE_IPC`.

---

## 6. Blk vs Chk vs Buf

Three distinct things, confirmed from the code and from the assertion strings
compiled into the kext:

| | what it is | evidence |
|---|---|---|
| **BlkBuf / BlkPool** | fixed-size **block** array: *N* units of equal size at a fixed alignment, plus a 24-byte-per-unit descriptor table. Alloc/Free are index arithmetic. | assertion `"msize > 0 && num > 0 && size > 0 && alignment >= 0 && alignment <= 64 && (alignment == 0 \|\| (alignment & (alignment - 1)) == 0)"` at `0xfffffe000727fa71`; `AVE_BlkPool::Addr2Idx`/`Idx2Addr` |
| **ChkBuf / ChkPool** | variable-size **chunk** allocator, and specifically a **buddy allocator** — `AVE_ChkBuf_Buddy{Init,Idx,NextIdx,Split,Merge}` are all present as symbols. Backing store is carved in power-of-two-sized chunks. | assertion `"msize > 0 && size > 0 && unitSize >= 0 && (unitSize == 0 \|\| (unitSize & (unitSize - 1)) == 0)"` at `0xfffffe0007280fab` |
| **BufPool** | not a memory allocator at all — a **ring of buffer descriptors** (48 bytes each) with `WriterGet`/`WriterPut`/`ReaderGet`/`ReaderPut` and an `_E_AVE_BPDir` direction. It *delegates* storage to a BlkPool, a ChkPool, or plain `kalloc`, chosen by `_E_AVE_BPType`. | `AVE_BufPool::Create` `0xfffffe0008c64eb8`, see below |

### `_E_AVE_BPType`

`AVE_BufPool::Create(u64, u32, _E_AVE_BPType, int, int)` (`0xfffffe0008c64eb8`)
range-checks the type (`cmp w8, #3; b.cs <error>` at `0xfffffe0008c64fc0`) and
then:

| value | backing store | evidence |
|---|---|---|
| 0 | invalid | `CalcSize` returns 0, `0xfffffe0008c64ea8` |
| **1** | **`AVE_BlkPool`** | ctor `0xfffffe0008b5d300` at `0xfffffe0008c64ff8`, `AVE_BlkPool::Create` at `0xfffffe0008c65018` |
| **2** | **`AVE_ChkPool`** | ctor `0xfffffe0008b64f0c` at `0xfffffe0008c6515c`, `AVE_ChkPool::Create` at `0xfffffe0008c65178` |
| **3** | **none** — each buffer separately `kalloc`ed | `0xfffffe0008c65230`–`0xc65238` |
| >3 | invalid | same range check |

`AVE_BufPool::CalcSize(_E_AVE_BPType, int n, int d)` (`0xfffffe0008c64df0`)
accounts only for the **descriptor array**, 48 bytes per entry:

```
type 1: if (n < 1 || d < 1) 0;  else BlkPool::CalcSize(n,0,0) + align_up(n*48, 64)
                                                          ; 0xc64e34-0xc64e54
type 2: if (n < 1 || d < 1) 0;  else ChkPool::CalcSize(0,n,1) + align_up((n/d)*48, 64)
                                                          ; 0xc64e70-0xc64e94
type 3: return 48 * n                                     ; 0xc64e9c-0xc64ea0
```

Both delegated calls are passed a **zero size** (`unitSize=0` for the BlkPool
call, `totalSize=0` for the ChkPool call) and therefore contribute **0** — see
§7 for why. So `BufPool::CalcSize` is, in practice, just the descriptor array.
Whether that is intentional or a bug is **unknown**; it is what the code does.

---

## 7. `AVE_BlkPool::CalcSize` / `AVE_BlkBuf_CalcSize`

`AVE_BlkPool::CalcSize(int,int,int)` (`0xfffffe0008b5d620`) is a two-instruction
stub: `bti c ; b AVE_BlkBuf_CalcSize` (`0xfffffe0008b5d624` →
`0xfffffe0008b3c800`). Argument names come from the `CreateWithMem` assertion
above: **`(int num, int size, int alignment)`**.

```
AVE_BlkBuf_CalcSize(num, size, alignment):
    if (num  < 1)                       return 0;      ; 0xb3c808
    if (size < 1)                       return 0;      ; 0xb3c810
    if ((unsigned)alignment > 64)       return 0;      ; 0xb3c818
    if (alignment & (alignment - 1))    return 0;      ; 0xb3c820-0xb3c828
    a = (alignment == 0) ? 64 : alignment;             ; 0xb3c82c-0xb3c834
    unit = align_up(size, a);                          ; 0xb3c838-0xb3c844
    hdr  = (num*24 + a + 167) & ~(a-1);                ; 0xb3c848-0xb3c854
         =  align_up(num*24 + 168, a)
    return num*unit + hdr;                             ; 0xb3c858
```

So: **alignment must be a power of two ≤ 64, 0 means 64; the manager overhead is
a 168-byte header plus 24 bytes per unit, rounded to the alignment.**

`AVE_BlkBuf_CalcDataSize(num, size, alignment)` (`0xfffffe0008b3c7d4`) returns
just `num * align_up(size, alignment)`, or 0 on `INT_MAX` overflow
(`0xb3c7e8`–`0xb3c7f8`).

This is where `BufPool::CalcSize` type 1 loses its backing store: it calls
`BlkPool::CalcSize(n, 0, 0)`, and `size == 0` fails the `size < 1` test.

---

## 8. `AVE_ChkPool::CalcSize` / `AVE_ChkBuf_CalcSize` — the FwIPC allocator

`AVE_ChkPool::CalcSize(int,int,int)` (`0xfffffe0008b65228`) is likewise
`bti c ; b AVE_ChkBuf_CalcSize` (`0xfffffe0008b3d504`). Arguments, from the
`CreateWithMem` assertion and from the code: **`(int size, int unitSize,
int roundUp)`**.

```
AVE_ChkBuf_CalcSize(size, unitSize, roundUp):
    if (size == 0)                        return 0;    ; 0xb3d510
    if (unitSize < 0)                     return 0;    ; 0xb3d52c
    if (unitSize & (unitSize - 1))        return 0;    ; 0xb3d530-0xb3d538   power of two only
    if (unitSize == 0) {                               ; 0xb3d53c
        if (size & 63)                    return 0;    ; 0xb3d5a0
        unitSize = 64;                                 ; 0xb3d5c0
    }
    aligned = roundUp ? align_up(size, unitSize)
                      : align_down(size, unitSize);    ; 0xb3d540-0xb3d554
    AVE_ChkBuf_CalcAlignedSize(&aligned, &unitSize, 0);; 0xb3d568
    if (unitSize < original unitSize)     return 0;    ; 0xb3d574
    n = aligned / unitSize;
    if (n > 0x200000)                     return 0;    ; 0xb3d584   2 097 152-chunk cap
    return ((n*4 + 167) & ~63) + aligned;              ; 0xb3d58c-0xb3d598
```

**Granularity constants, confirmed:**

| | value | evidence |
|---|---|---|
| default chunk size | **64 bytes** | `mov w19, #0x40` `0xfffffe0008b3d5c0` (CalcSize); `mov w3, #0x40` `0xfffffe0008b3d944` (`AVE_ChkBuf_CreateWithMem`) |
| chunk size constraint | power of two, ≥ 0 | `0xb3d530`–`0xb3d538` |
| maximum chunk count | **2 097 152** (2²¹) | `cmp w9, #0x200, lsl #12` `0xfffffe0008b3d584`; same cap in `AVE_ChkBuf_CalcDataSize` `0xfffffe0008b3d4e8` |
| manager overhead | 4 bytes/chunk + header, to 64 | `lsl #2` / `+0xa7` / `& ~63` `0xb3d58c`–`0xb3d594`; identical in `AVE_ChkBuf_CalcMgrSize` `0xfffffe0008b3d480`–`0xb3d48c` |
| allocation alignment default | **64 bytes** | `AVE_ChkPool::Alloc`, `mov w8, #0x40` + `csel` on `alignment == 0`, `0xfffffe0008b66338`–`0xb66340` |

`AVE_ChkPool::Alloc(int alignment, int size, u64* pAddr, int* pSize)`
(`0xfffffe0008b662e0`) — argument roles from the assertion
`"alignment >= 0 && (alignment == 0 || (alignment & (alignment - 1)) == 0) &&
pSize != nullptr && iSize != 0 && pAddr != nullptr"` at `0xfffffe00072811c8`,
matched against the checks at `0xb66310`–`0xb66330`.

### Confirming `docs/08`: ChkPool *is* the FwIPC allocator

`AVE_IPC::Init` creates it over the surface's **DART address range**
(`08-ipc-transport.md` §2 was right about that; the argument roles are now
pinned down by the assertion string):

```
0xfffffe0008c42820  bl AVE_Surface::GetDARTAddr   -> x25
0xfffffe0008c4282c  bl AVE_Surface::GetSize       -> x27
0xfffffe0008c42838  bl AVE_Surface::GetSize       -> x4
0xfffffe0008c42844  mov w1, #0                    ; flags
0xfffffe0008c42848  mov x2, x25                   ; maddr  = IOVA of FwIPC
0xfffffe0008c4284c  mov x3, x27                   ; msize  = 20 MiB
0xfffffe0008c42850  mov w5, #0                    ; unitSize = 0  -> 64
0xfffffe0008c42854  bl AVE_ChkPool::CreateWithMem
```

and `AVE_IPC::Alloc(int size, u64* pAddr)` (`0xfffffe0008c42f00`) forwards to it
with **alignment 0**:

```
0xfffffe0008c42ff4  ldr x0, [x19, #48]     ; AVE_IPC+0x30 = AVE_ChkPool*
0xfffffe0008c43010  mov w1, #0             ; alignment -> 64
0xfffffe0008c43014  mov x2, x20            ; size
0xfffffe0008c43018  bl AVE_ChkPool::Alloc
```

**Consequence for the Linux side.** The 20 MiB `FwIPC` surface is carved by a
buddy allocator with a **64-byte granule**; `0x1400000 / 64` = **327 680
chunks**, comfortably under the 2²¹ cap. Every IPC allocation is 64-byte aligned
and rounded up to a multiple of 64. A Linux implementation that hands the
firmware IOVAs must therefore keep the same 64-byte granularity and alignment
for anything the firmware indexes by chunk.

Two caveats, both read from the code:

- With `CreateWithMem` the **manager array is a separate `kalloc`**
  (`0xfffffe0008b3d988`), not carved out of the surface — so the whole 20 MiB is
  usable data. `CalcSize` (manager + data in one block) applies to the
  `CreateInMem` flavour instead. `CreateWithMem` only checks
  `aligned_size <= msize` and returns `-1019` otherwise
  (`0xfffffe0008b3d96c`–`0xb3d978`).
- The *exact* buddy layout (`AVE_ChkBuf_CalcAlignedSize` at
  `0xfffffe0008b3d13c` may raise the chunk size and lower the managed size so
  the ratio is buddy-friendly, via `AVE_ChkBuf_RoundRatio` `0xfffffe0008b3d0c0`)
  was **not** fully decoded. For a 20 MiB region with a 64-byte unit the ratio
  is `327680 = 2^16 * 5`, so an adjustment is possible; whether it occurs was
  not verified.

---

## 9. What was not determined

- The meaning of the `_S_AVE_Client` bit fields that gate the LRME sizes
  (`client + 15288` bits, read at `0xfffffe0008cb11bc`–`0xcb11c8`). Without them
  the LRME totals are conditional.
- Where `GGMRef`/`GGMStats`/`GGMOutput` (idx 37–39) and `DMVOutput` (idx 40) are
  sized. No `CalcSurfaceInfo` examined touches them and there are no
  `AVE_CalcBufSizeOfGGM*`/`*DMVOutput` symbols.
- How an MSC work unit is ever dispatched: MSC has no `_E_AVE_ClientType` slot
  and no `SEntry` on any SoC.
- The expansion of **MSC** and **GGM**. Neither binary contains one.
- `_E_AVE_BPDir` values (`AVE_BufPool::WriterGet`/`ReaderGet`).
- Whether `AVE_ChkBuf_CalcAlignedSize` actually adjusts the 20 MiB / 64 B pair.
- Whether anything writes the `FwClient`-size global at `0xfffffe000c69aaf0` at
  runtime (it is 0 in the image).
