> ## Version note (2026-09-13) — macOS 13.5
>
> On macOS 13.5 (the firmware this machine runs, [43](43-macos-13.5-firmware.md))
> there is no index-to-slot map; the 26.6.2 map below stands for 26.6.2.
> Read from the 13.5 kext (`AVE_MACOS=13.5`):
>
> - `gs_saAVE_SurfaceCfg` (`0xfffffe0007bc3db8`) has **30** entries, bound
>   `cmp w0,#0x1e` at `0xfffffe0008f37408`: 0 `FrameInfo`, 1 `ParameterSet`,
>   2 `MBInputCtrl`, 3 `MBStats`, 4 `LRMEStats`, 5 `MultiPassStats`,
>   6 `CodedData`, 7 `CodedHeader`, 8 `SliceHeader`, 9 `Recon`, 10 `Colocated`,
>   11 `LowResRef`, 12 `LowResResult`, 13 `LowResRCResult`, 14–17
>   `SrcNeighbor{Info,Pixel,Data,FwData}`, 18 `TranscodedData`,
>   19 `EntropyCoding`, 20 `CrcQPMod`, 21 `IOPIPC`, 22 `FwImage`, 23 `FwHeap`,
>   24 `FwIPC` (flags `0x10d08`), 25 `FwClient`, 26 `FwClientMem`,
>   27 `InitParamsCopy`, 28 `MCTFOutput`, 29 `InputData`. `UCInfo`, `FwLog`,
>   `Link`, `ProtectedData`, `DirectRecon`, `InputScaledData`, the
>   `StaticArea*`/`HSCOutput`/`LRSNeighborMV` kinds and `MCTFRef`/`GGM*`/
>   `DMVOutput` do not exist (their name strings are absent from the 13.5 kext
>   and present in 26.6.2's; the four new names are the reverse). **Confirmed.**
> - `AVE_GetSurfaceCfg` has only 8 call sites kext-wide, none in a function that
>   takes a `_S_AVE_SurfaceInfoSet*`. `_S_AVE_SurfaceInfoSet` is `0x1ec` bytes
>   (`mov w2,#0x1ec` memset at `0xfffffe0008ec685c`) of **named** members
>   (`pClient->sSurfaceInfoSet.ia<Name>[AVE_SIIdx_{Set,Layer,Num,Size}]`,
>   assert strings) — 20-byte `u32` arrays, 36 bytes for `Recon` and
>   `MCTFOutput`, no flags word. Count is at member `+0x0c`, size at `+0x10`.
>   Full offset table in [47](47-abi-13.5-frame-rc-surfaces.md) (surfaces part).
> - Much of 26.6.2's extra surface/engine surface (MCTF/GGM/DMV kinds, 41 vs 30
>   indices) is probably support for newer SoCs; **inferred**.

> ## Verification note — independently confirmed
>
> Re-checked against the binary before committing:
>
> - **`AVE_GetSurfaceCfg` does no remapping.** `sbfiz x8, x0, #4, #32` at
>   `0xfffffe0008ca8d00` (idx x 16, matching the table's 16-byte stride), base
>   `0xfffffe0007ee1050`, bound `cmp w0, #0x29`. Plain lookup.
> - **The decisive pairing holds.** In `AVE_CreateDataSurfaces`:
>   `mov w0,#0x2` -> `ldr x8,[x26,#96]` (`0xfffffe0008c7886c`), and the next arm
>   `mov w0,#0x4` -> `ldr x8,[x26,#144]` (`0xfffffe0008c789cc`). 144/48 = slot 3,
>   so index 4 occupies slot 3 and index 3 has no slot.
> - **The structural signature is visible in the same block:** `ldp x10,x9,[x0]`
>   then `orr x8, x8, x9` — the InfoSet flags word OR'd into the static config
>   flags, exactly as described.
> - **The mapping is a bijection onto 0..34** — 35 slots, no gaps, no
>   collisions, matching the `0x690` bzero. Checked programmatically over all
>   41 indices.
>
> **Verdict stands: [16-encode-surface-set.md](16-encode-surface-set.md) was
> right, [17-aux-engines-pools.md](17-aux-engines-pools.md) was wrong at the low
> end.** The mapping is now in `driver/ave_abi.h` as `ave_surface_slot()`.

# `_S_AVE_SurfaceInfoSet` — the definitive slot map

*Settles the disagreement between [16-encode-surface-set.md](16-encode-surface-set.md)
§1.2 and [17-aux-engines-pools.md](17-aux-engines-pools.md) §0 over which six
`_E_AVE_SurfaceIdx` values have no InfoSet entry.*

Read out of `AppleAVE2.kext` in `data/blobs/kc.macho` with `tools/disas.py`.
Every constant carries the VA of the instruction it came from. Anything not read
directly out of an instruction is marked **inferred** or **unknown**.

---

## Verdict

**[16-encode-surface-set.md](16-encode-surface-set.md) was right.
[17-aux-engines-pools.md](17-aux-engines-pools.md) was wrong at the low end.**

The six surface kinds with **no** InfoSet slot are
**3 `UCInfo`, 27 `IOPIPC`, 28 `FwImage`, 29 `FwLog`, 30 `FwHeap`, 31 `FwIPC`**.
Index 0 `InputData` **does** have a slot — it is slot 0, at byte offset 0.

`17`'s formula (`48*(idx-1)` for idx 1..26) is correct only over **4..26**. It is
wrong for 0..3: it assigns idx 1 to offset 0, idx 2 to 48, idx 3 to 96 and idx 4
to 144, whereas the code puts idx 0/1/2 at 0/48/96 and idx 4 at 144. The two
documents agree from idx 5 upward, which is why the earlier evidence
(`AVE_PrintSurfaceInfoSet`, which only touches 5..26 and 32..35) could not
discriminate.

The correct closed form is:

```
slot(idx) = idx        for idx = 0 .. 2
          = idx - 1    for idx = 4 .. 26
          = idx - 6    for idx = 32 .. 40
          = none       for idx = 3, 27, 28, 29, 30, 31

byte offset = 48 * slot(idx)
```

35 slots, `35 * 48 = 0x690`, closing the struct exactly — consistent with
`mov w1, #0x690 ; bl bzero` at `0xfffffe0008ca9a64`.

One row is **not** directly read and is settled by elimination: idx 40
`DMVOutput` → slot 34 (offset 1632). See §4.

---

## 1. `AVE_GetSurfaceCfg` does no remapping

`AVE_GetSurfaceCfg(_E_AVE_SurfaceIdx)` (`0xfffffe0008ca8cfc`) is a plain
16-byte-stride table lookup with a bound check — there is no index translation
hiding in it, so the InfoSet mapping has to come from the call sites:

```
fffffe0008ca8cfc:  bti  c
fffffe0008ca8d00:  sbfiz x8, x0, #4, #32        ; idx * 16
fffffe0008ca8d04:  adrp  x9, 0xfffffe0007ee1000
fffffe0008ca8d08:  add   x9, x9, #0x50          ; &gs_saAVE_SurfaceCfg
fffffe0008ca8d10:  add   x10, x9, w8, sxtw
fffffe0008ca8d20:  cmp   w0, #0x29              ; 41
fffffe0008ca8d24:  csel  x0, x10, xzr, lt       ; NULL for idx >= 41
fffffe0008ca8d28:  ret
```

`gs_saAVE_SurfaceCfg` at `0xfffffe0007ee1050`, 41 entries × 16 bytes
`{const char *name; u32 flags; u32 pad}`, ending `0xfffffe0007ee12e0`. Dumped in
full (chained-fixup low 32 bits = image-relative file offset, per
[00-methodology.md](00-methodology.md) trap 5) in §3's table; `pad` is zero in
every entry.

## 2. Method

Four independent lines of evidence, all agreeing:

1. **Consumer pairing (decisive).** Eight functions take a
   `_S_AVE_SurfaceInfoSet*` and, per surface, call `AVE_GetSurfaceCfg(idx)` and
   then dereference the InfoSet at that surface's byte offset in the same basic
   block. `AVE_CreateDataSurfaces` (`0xfffffe0008c78600`, InfoSet = arg 4 →
   `mov x26, x3` at `0xfffffe0008c78630`) and `AVE_DARTMapDataSurfaces`
   (`0xfffffe0008c7d678`, InfoSet = arg 3 → `mov x23, x2` at
   `0xfffffe0008c7d69c`) are the two that cover the disputed low indices.
2. **Kext-wide census of `AVE_GetSurfaceCfg` call sites.** 109 `BL` sites decoded
   straight out of `__TEXT_EXEC`, each paired with its preceding `movz w0, #imm`.
   All 41 indices appear. Exactly six of them — 3, 27, 28, 29, 30, 31 — never
   appear in any function that takes an InfoSet.
3. **Store bucketing in the six `AVE_Work_*_CalcSurfaceInfo`.** In all six the
   InfoSet is arg 3 in `x19`. Every `str`/`stp` through `x19` was bucketed by
   offset; all of them land on `48*slot + f` with `f` in
   {0, 8, 12, 16, 20, 24, 28, 32, 36} — i.e. a legal field of the 48-byte entry.
   No store lands off a slot boundary.
4. **The "no slot" allocation signature.** Where a surface *has* a slot, the
   allocation site always ORs the InfoSet entry's `+0x00` flags word into the
   static cfg flags and strips bit 16 before calling
   `AVE_SurfaceMgr::CreateSurface`. Where a surface has *no* slot, the cfg flags
   word is passed straight through as the `options` argument with no `orr` and
   no mask. All six skipped indices show the second shape (§5).

### Positive control (methodology trap 2)

Before asserting "offset 1632 is never touched", the identical scan was run for
offsets that are known to be used, and it returns positives: the same
offset-immediate decoder finds `#1584` (slot 33) at `0xfffffe0008c79078`,
`#1440` (slot 30) at `0xfffffe0008c78cb8` and `0xfffffe0008c7dad4`, and `#1392`
(slot 29) at `0xfffffe0008c78ba4`, `0xfffffe0008c7b94c` and `0xfffffe0008c7da4c`.
The scan discriminates.

Likewise the consumer-pairing extractor was validated against the rows both prior
documents already agree on (idx 5 → 192, idx 6 → 240, idx 26 → 1200, idx 32 →
1248) before being trusted on 0..4.

### Why emission order is not what was measured

In `AVE_DARTMapDataSurfaces` the calls are emitted **out of index order** — idx 5
at `0xfffffe0008c7d8b4` comes *before* idx 4 at `0xfffffe0008c7d9ac`, yet idx 5
takes offset 192 and idx 4 takes offset 144. The mapping therefore does not come
from ordering ([00-methodology.md](00-methodology.md) rank 5, the weakest kind of
evidence); it comes from the literal in `movz w0` paired with the literal in the
load. That is rank 4, a disassembled call site, twice over.

---

## 3. The 41-row table

`slot` is the index into the 35-entry array; `off` is `48 * slot`. "Evidence" is
`<VA of the mov w0,#idx>` / `<VA of the InfoSet access>` unless noted.

| idx | name | flags | slot | off | evidence |
|---:|---|---|---:|---:|---|
| 0 | `InputData` | `0x00010510` | **0** | 0 | `0xfffffe0008c7866c` / `ldr x8,[x26]` `0xfffffe0008c7867c`; also `0xfffffe0008c7d6b4` / `ldr x9,[x23]` `0xfffffe0008c7d6cc` |
| 1 | `InputScaledData` | `0x00010510` | **1** | 48 | `0xfffffe0008c7879c` / `0xfffffe0008c787ac`; `0xfffffe0008c7d7a4` / `0xfffffe0008c7d7bc` |
| 2 | `DirectRecon` | `0x00010510` | **2** | 96 | `0xfffffe0008c7885c` / `0xfffffe0008c7886c`; `0xfffffe0008c7d82c` / `0xfffffe0008c7d844` |
| 3 | `UCInfo` | `0x00000d18` | **none** | — | sole call site kext-wide is `0xfffffe0008b8d2f8` in `AVE_Client_InitUCInfoPool`, which has no InfoSet (§5) |
| 4 | `MultiPassStats` | `0x00010518` | **3** | 144 | `0xfffffe0008c789bc` / `0xfffffe0008c789cc`; `0xfffffe0008c7d9ac` / `0xfffffe0008c7d9c4` |
| 5 | `MBInputCtrl` | `0x00010d18` | **4** | 192 | `0xfffffe0008c798fc` / `0xfffffe0008c79904`, count+size `ldp w20,w28,[x26,#216]` `0xfffffe0008c79914` |
| 6 | `Recon` | `0x00010518` | **5** | 240 | `0xfffffe0008c799e0` / `0xfffffe0008c799e8`; `0xfffffe0008c765f0` / `0xfffffe0008c76604` |
| 7 | `Link` | `0x00010d08` | **6** | 288 | `0xfffffe0008c77204` / `0xfffffe0008c7720c` |
| 8 | `CodedData` | `0x00000518` | **7** | 336 | `0xfffffe0008c773e8` / `0xfffffe0008c773f0` |
| 9 | `CodedHeader` | `0x00010d18` | **8** | 384 | `0xfffffe0008c774c0` / `0xfffffe0008c774cc` |
| 10 | `SliceHeader` | `0x00010d18` | **9** | 432 | `0xfffffe0008c77598` / `0xfffffe0008c775a4` |
| 11 | `ProtectedData` | `0x00010d18` | **10** | 480 | `0xfffffe0008c79b5c` / `0xfffffe0008c79b68` |
| 12 | `MBStats` | `0x00010d18` | **11** | 528 | `0xfffffe0008c77670` / `0xfffffe0008c7767c` |
| 13 | `StaticAreaQPModInfo` | `0x00010518` | **12** | 576 | `0xfffffe0008c79f2c` / `0xfffffe0008c79f34` |
| 14 | `StaticAreaCBP0Cntr` | `0x00010518` | **13** | 624 | `0xfffffe0008c7a03c` / `0xfffffe0008c7a048` |
| 15 | `Colocated` | `0x00010518` | **14** | 672 | `0xfffffe0008c7a154` / `0xfffffe0008c7a160` |
| 16 | `HSCOutput` | `0x00010518` | **15** | 720 | `0xfffffe0008c7a2d0` / `0xfffffe0008c7a2dc` |
| 17 | `LFSRef` | `0x00010518` | **16** | 768 | `0xfffffe0008c7a400` / `0xfffffe0008c7a40c` |
| 18 | `LRSNeighborMV` | `0x00010518` | **17** | 816 | `0xfffffe0008c7a57c` / `0xfffffe0008c7a588` |
| 19 | `LFSResult` | `0x00010518` | **18** | 864 | `0xfffffe0008c77748` / `0xfffffe0008c77754` |
| 20 | `LRSResult` | `0x00010518` | **19** | 912 | `0xfffffe0008c77894` / `0xfffffe0008c778a0` |
| 21 | `SrcNeighborInfo` | `0x00010518` | **20** | 960 | `0xfffffe0008c7ab48` / `0xfffffe0008c7ab54` |
| 22 | `SrcNeighborPixel` | `0x00010518` | **21** | 1008 | `0xfffffe0008c7ac30` / `0xfffffe0008c7ac3c` |
| 23 | `SrcNeighborData` | `0x00010518` | **22** | 1056 | `0xfffffe0008c7ad2c` / `0xfffffe0008c7ad38` |
| 24 | `SrcNeighborFwData` | `0x00010518` | **23** | 1104 | `0xfffffe0008c7ae14` / `0xfffffe0008c7ae20` |
| 25 | `TranscodedData` | `0x00010518` | **24** | 1152 | `0xfffffe0008c7aefc` / `0xfffffe0008c7af08` |
| 26 | `EntropyCoding` | `0x00010518` | **25** | 1200 | `0xfffffe0008c7b0b4` / `0xfffffe0008c7b0c0` |
| 27 | `IOPIPC` | `0x00010518` | **none** | — | sole call site `0xfffffe0008bd9b0c` in `AVE_Drv::IO_init()` |
| 28 | `FwImage` | `0x00010d18` | **none** | — | `0xfffffe0008bec5a4` (`AVE_FwImg::InitCTRRImage`), `0xfffffe0008bed25c` (`AVE_FwImg::InitBufImage`) |
| 29 | `FwLog` | `0x00010d18` | **none** | — | sole call site `0xfffffe0008b406b8` in `AVE_FwLog::Init` |
| 30 | `FwHeap` | `0x00010518` | **none** | — | sole call site `0xfffffe0008c1c7f4` in `AVE_HwC::CreateFwHeap` |
| 31 | `FwIPC` | `0x00010d18` | **none** | — | sole call site `0xfffffe0008c426c8` in `AVE_IPC::Init`; size is the literal `0x1400000` at `0xfffffe0008c426ec` |
| 32 | `FwClient` | `0x00010518` | **26** | 1248 | `0xfffffe0008c7b1d8` / `0xfffffe0008c7b1e4` |
| 33 | `FwClientMem` | `0x00010518` | **27** | 1296 | `0xfffffe0008c7b2c0` / `0xfffffe0008c7b2cc` |
| 34 | `InitParamsCopy` | `0x00000d18` | **28** | 1344 | `0xfffffe0008c7b860` / count `ldrsw x19,[x8,#1368]` `0xfffffe0008c7b874`, size `ldr w27,[x8,#1372]` `0xfffffe0008c7b878` |
| 35 | `MCTFOutput` | `0x00010518` | **29** | 1392 | `0xfffffe0008c7b940` / `0xfffffe0008c7b94c`; `0xfffffe0008c78b94` / `0xfffffe0008c78ba4` |
| 36 | `MCTFRef` | `0x00010518` | **30** | 1440 | `0xfffffe0008c78ca8` / `0xfffffe0008c78cb8`; `0xfffffe0008c7dabc` / `0xfffffe0008c7dad4` |
| 37 | `GGMRef` | `0x00010518` | **31** | 1488 | `0xfffffe0008c78de4` / `0xfffffe0008c78df4` |
| 38 | `GGMStats` | `0x00010518` | **32** | 1536 | `0xfffffe0008c78e7c` / `0xfffffe0008c78e94` |
| 39 | `GGMOutput` | `0x00010518` | **33** | 1584 | `0xfffffe0008c79068` / `0xfffffe0008c79078` |
| 40 | `DMVOutput` | `0x00010518` | **34** (by elimination) | 1632 | see §4 — **not directly read** |

34 of the 35 slots are pinned by at least one directly disassembled
`(movz w0, #idx) → (InfoSet load at 48*slot)` pair.

---

## 4. The one row that is not directly read: idx 40 → slot 34

`0xfffffe0008c7917c` — the citation
[16-encode-surface-set.md](16-encode-surface-set.md) gives for "idx 40 → +1632" —
**is the `bl` to `AVE_GetSurfaceCfg` itself, and no InfoSet access follows it.**
The code at the idx-40 site in `AVE_CreateDataSurfaces` is:

```
fffffe0008c79178:  mov  w0, #0x28              ; 40, DMVOutput
fffffe0008c7917c:  bl   AVE_GetSurfaceCfg
fffffe0008c79180:  ldr  w4, [x22, #44]         ; IOSurface ID from the SurfaceIDDataSet
fffffe0008c79188:  ldp  x8, x19, [x0]          ; cfg.name, cfg.flags -- no InfoSet OR
fffffe0008c791b0:  and  x5, x19, #0xfffffffffffeffff
```

Compare the idx-39 site four instructions earlier, which does read the InfoSet
(`ldr x8, [x26, #1584]` at `0xfffffe0008c79078`, then `orr`). So `DMVOutput`'s
options come from the static cfg alone.

A kext-wide scan for any `ldr`/`str`/`ldrsw` at immediate offset 1632 finds only
two sites in the whole `__TEXT_EXEC`, and both are **client** reads, not InfoSet
reads: `ldr w8, [x28, #1632]` at `0xfffffe0008ca9b5c` in
`AVE_Work_Enc_CalcSurfaceInfo`, where `x28 = client + x8` (`add x28, x0, x8` at
`0xfffffe0008ca9a5c`), and `ldr w8, [x23, #1632]` at `0xfffffe0008bfa510` in
`AVE_Work_MCTF_CalcSurfaceInfo`, where `x23 = client + x8`
(`add x23, x0, x8` at `0xfffffe0008bfa44c`). In both functions the InfoSet is
`x19`, not `x28`/`x23`.

A second, deliberately **register-blind** sweep was then run over the whole
`__TEXT_EXEC` for every immediate in the slot-34 field range
(1632 + {0, 8, 12, 16, 20, 24, 28, 32, 36}), restricted to the seventeen
functions that take an InfoSet, to make sure the register-aware scan had not
missed an access through a reloaded base. It produced six candidates, and every
one was individually excluded by reading its base register — all six are the
same `client + x8` idiom, none is `x19`:

| site | instruction | base set at | function |
|---|---|---|---|
| `0xfffffe0008b3f594` | `ldr w8, [x26, #1648]` | `add x26, x0, x8` `0xfffffe0008b3f54c` | GGM |
| `0xfffffe0008ba1100` | `ldr w8, [x26, #1648]` | `add x26, x0, x8` `0xfffffe0008ba1078` | MSC |
| `0xfffffe0008bfa4b8` | `ldr w8, [x23, #1648]` | `add x23, x0, x8` `0xfffffe0008bfa44c` | MCTF |
| `0xfffffe0008ca9b30` | `ldr w8, [x28, #1648]` | `add x28, x0, x8` `0xfffffe0008ca9a5c` | Enc |
| `0xfffffe0008bfa510` | `ldr w8, [x23, #1632]` | as above | MCTF |
| `0xfffffe0008ca9b5c` | `ldr w8, [x28, #1632]` | as above | Enc |

All six engines share this idiom: each computes `client + x8` into a scratch
register and reads a block of per-client configuration words at offsets
1620/1624/1632/1644/1648. Those immediates collide numerically with slot 33/34
field offsets and are the only reason a naive offset scan appears to find
anything there. **Nothing in the kext reads or writes offset 1632 through a
`_S_AVE_SurfaceInfoSet*`.**

So **slot 34 (offset 1632) is dead in the shipped kext** — nothing reads or
writes it through a `_S_AVE_SurfaceInfoSet*`. The assignment idx 40 → slot 34 is
forced: 34 of 35 offsets are individually pinned to 34 distinct indices, one
offset (1632) and one index (40) remain, and the struct is exactly 35 slots.
That is arithmetic on top of read facts, but it is **not** itself read. Mark it
**inferred**.

This is consistent with [17-aux-engines-pools.md](17-aux-engines-pools.md) §3's
observation that `AVE_Work_DMV_CalcSurfaceInfo` sizes only `CodedHeader` and
`FwClient` and never sizes its own output surface — on M1 the DMV client type has
no capability entry at all, so nothing on this SoC would ever fill slot 34.

**What would settle it:** an `AppleAVE2.kext` from an SoC where DMV is
advertised (DevID 32 `8150` / 34 `8160`, per
[17-aux-engines-pools.md](17-aux-engines-pools.md) §2) is likely to contain a
`AVE_CalcBufSizeOfDMVOutput`-style writer, or a `CreateDataSurfaces` whose
idx-40 arm reads the InfoSet the way its idx-39 arm does here.

---

## 5. Why the six skipped surfaces are skipped

They are the device-global and firmware-side surfaces: one per *driver instance*
or per *client pool*, not one per encode work unit. Structurally they are
identifiable without knowing what they are for — a surface with a slot is
allocated as

```
ldr  x8, [infoset, #48*slot]        ; entry.flags   (+0x00)
ldp  x10, x9, [x0]                  ; cfg.name, cfg.flags
orr  x8, x8, x9
and  x5, x8, #0xfffffffffffeffff    ; strip bit 16
bl   AVE_SurfaceMgr::CreateSurface
```

whereas a surface without one skips the `orr` and the mask entirely. `UCInfo`
(`AVE_Client_InitUCInfoPool`, `0xfffffe0008b8d214`):

```
fffffe0008b8d2f4:  mov  w0, #0x3               ; UCInfo
fffffe0008b8d2f8:  bl   AVE_GetSurfaceCfg
fffffe0008b8d2fc:  ldp  x8, x24, [x0]          ; name -> x8, flags -> x24
fffffe0008b8d338:  mov  x5, x24                ; options = cfg.flags, verbatim
fffffe0008b8d344:  bl   0xfffffe0008c81670     ; CreateSurface (IOSurface import)
```

and `FwIPC` (`AVE_IPC::Init`):

```
fffffe0008c426c4:  mov  w0, #0x1f              ; FwIPC
fffffe0008c426c8:  bl   AVE_GetSurfaceCfg
fffffe0008c426d8:  ldp  x3, x5, [x0]           ; name -> x3, flags -> x5 (options)
fffffe0008c426ec:  mov  w4, #0x1400000         ; size, a literal -- 20 MiB
fffffe0008c426f8:  bl   0xfffffe0008c80e30     ; CreateSurface (kernel alloc)
```

Neither reads an InfoSet. The size that a slot would have supplied is instead a
literal (`FwIPC`) or comes from the caller (`UCInfo`).

---

## 6. Complete list of InfoSet consumers

Seventeen functions take a `_S_AVE_SurfaceInfoSet*`. Three of them
(`AVE_DARTMap*Surfaces`) and two more (`AVE_CheckExternalInSurfaces`,
`AVE_CHM_SetFwBuf`) were not read by either prior document; the DARTMap family is
what makes the low-index mapping unambiguous, since it is a second, independent
pass over the same surfaces.

| function | VA | InfoSet reg | indices it pairs |
|---|---|---|---|
| `AVE_Work_Enc_CalcSurfaceInfo` | `0xfffffe0008ca9a28` | `x19` | writes slots 4, 5, 7..29 (by offset, not by index) — every slot in 4..29 except 6 (`Link`) |
| `AVE_Work_LRME_CalcSurfaceInfo` | `0xfffffe0008cb1094` | `x19` | slots 8, 16, 18, 19, 26 |
| `AVE_Work_MCTF_CalcSurfaceInfo` | `0xfffffe0008bfa418` | `x19` | slots 4, 5, 7, 8, 9, 11, 14..26, 28, 29 |
| `AVE_Work_MSC_CalcSurfaceInfo` | `0xfffffe0008ba1044` | `x19` | slots 4, 7, 8, 16, 17, 18, 19, 26 |
| `AVE_Work_GGM_CalcSurfaceInfo` | `0xfffffe0008b3f51c` | `x19` | slots 8, 16, 18, 19, 26 |
| `AVE_Work_DMV_CalcSurfaceInfo` | `0xfffffe0008c376e0` | `x19` | slots 8, 26 only |
| `AVE_CreateInternalSurfaces` | `0xfffffe0008c79894` | `x26` (arg 3) | 5, 6, 11, 13..18, 21..26, 32..35 |
| `AVE_CreateExternalInSurfaces` | `0xfffffe0008c76588` | `x24` (arg 4) | 6 (`Recon`), 17-deep loop |
| `AVE_CreateExternalOutSurfaces` | `0xfffffe0008c771a4` | `x27` (arg 3) | 7, 8, 9, 10, 12, 19, 20 |
| `AVE_CreateDataSurfaces` | `0xfffffe0008c78600` | `x26` (arg 4) | **0, 1, 2, 4, 5**, 35..40 |
| `AVE_CreateExternalSurfaces` | `0xfffffe0008c78504` | — | wrapper: calls In then Out |
| `AVE_CheckExternalInSurfaces` | `0xfffffe0008c76ac4` | `x20` (arg 2) | reads only slot 5 (`Recon`): `+268` `0xfffffe0008c76b54`, `+264` `0xfffffe0008c76b74`, `+260` `0xfffffe0008c76b98`, `+256` `0xfffffe0008c76bcc` |
| `AVE_DARTMapExternalSurfaces` | `0xfffffe0008c7c8ec` | `x23` (arg 3) | 6, 7, 8, 9, 10, 12, 19, 20 |
| `AVE_DARTMapDataSurfaces` | `0xfffffe0008c7d678` | `x23` (arg 3) | **0, 1, 2, 5, 4**, 35..40 |
| `AVE_DARTMapInternalSurfaces` | `0xfffffe0008c7e328` | `x27` (arg 3) | 5, 6, 11, 13..18, 21..26, 32..35 |
| `AVE_CHM_SetFwBuf` | `0xfffffe0008b6fc6c` | `x21` (arg 3) | reads only `extra[]`: `+272/276` (slot 5), `+800/804/808/812` (slot 16), `+1232/1236` (slot 25) |
| `AVE_PrintSurfaceInfoSet` | `0xfffffe0008c7454c` | `x20` (arg 1) | 5..26, 32..35 only |

`AVE_Pipeline::CalcSurfaceInfo` (`0xfffffe0008c9c54c`) and
`AVE_MD_SVE::CalcSurfaceInfo` (`0xfffffe0008c8c768`) do **not** index the InfoSet
at all — they only forward it. Both prior documents were right to leave them out.

### Bonus: the InfoSet lives at `AVE_MD_SVE + 0xEED28`, and `_E_AVE_WorkType` is now fully read

`AVE_MD_SVE::CalcSurfaceInfo` computes the InfoSet pointer identically in every
arm (`add x8, x19, #0xee, lsl #12 ; add x2, x8, #0xd28`, e.g.
`0xfffffe0008c8c85c`–`0xc8c860`), confirming
[16-encode-surface-set.md](16-encode-surface-set.md)'s `+0xEED28`. Its dispatch
also closes the gap [17-aux-engines-pools.md](17-aux-engines-pools.md) §1 left
open ("Enc = 0 and/or 1, not read directly"):

```
fffffe0008c8c83c:  cmp w8, #0x1 ; b.eq -> 0xc8c89c -> bl 0xfffffe0008ca9a28   Enc
fffffe0008c8c844:  cmp w8, #0x2 ; b.eq -> 0xc8c950 -> bl 0xfffffe0008cb1094   LRME
fffffe0008c8c84c:  cmp w8, #0x3 ; b.ne ...          bl 0xfffffe0008bfa418     MCTF
fffffe0008c8c86c:  cmp w8, #0x4 ; b.eq -> 0xc8c8b4 -> bl 0xfffffe0008ba1044   MSC
fffffe0008c8c874:  cmp w8, #0x5 ; b.eq -> 0xc8c968 -> bl 0xfffffe0008b3f51c   GGM
fffffe0008c8c87c:  cmp w8, #0x6 ; b.ne -> error;    bl 0xfffffe0008c376e0     DMV
```

**`_E_AVE_WorkType`: Enc = 1, LRME = 2, MCTF = 3, MSC = 4, GGM = 5, DMV = 6.**
Value 0 falls through to the error arm at `0xfffffe0008c8c8cc`
(`mov w0,#0x60 ; mov w1,#5 ; bl 0xfffffe0008c46348` — the logging idiom, subsystem
`0x60`, not a size). This agrees with the literals
[17-aux-engines-pools.md](17-aux-engines-pools.md) §1 read out of the individual
`CalcSurfaceInfo` bodies and adds the one it could not get.

---

## 7. Field layout, re-confirmed

The store bucketing (method 3) independently re-derives the entry layout that
both prior documents give, and finds no store outside it:

| off | width | meaning | slots seen writing it |
|---|---|---|---|
| `+0x00` | u64 | flags, OR'd into `cfg.flags`, bit 16 stripped | 7, 16, 18, 19 |
| `+0x08` | int | `AVE_CalcBufModeNumOf<S>` | 18, 19 |
| `+0x0c` | int | `AVE_CalcBufTypeNumOf<S>` | 16, 18, 19 |
| `+0x10` | int | `AVE_CalcBufSetNumOf<S>` | 5, 14, 25, 29 |
| `+0x14` | int | `AVE_CalcBufLayerNumOf<S>` | 5, 12, 13, 14, 15, 16, 29 |
| `+0x18` | int | buffer **count** | every written slot |
| `+0x1c` | int | **bytes per buffer** | every written slot |
| `+0x20`..`+0x2c` | 4×int | sub-region sizes via the `int*` out-param | 25 only, in these six functions |

`+0x18`/`+0x1c` remain proved as count/size by `AVE_CreateInternalSurfaces`
creating `[+0x18]` surfaces and rejecting any smaller than `[+0x1c]`
(`0xfffffe0008c79914`, `0xfffffe0008c79968`) — unchanged from both prior
documents.

---

## 8. Corrections to record

- **[17-aux-engines-pools.md](17-aux-engines-pools.md) §0** — the skip list
  ("indices 0 and 27–31") and the formula (`48*(idx-1)` for 1..26) are wrong for
  idx 0..3. Its §3 engine/surface table is unaffected: every surface in it is
  idx ≥ 5, where the two formulas coincide.
- **[16-encode-surface-set.md](16-encode-surface-set.md) §1.2** — the table is
  correct in all 35 rows. Its citation for the last row, "idx 40 → +1632
  (`0xfffffe0008c7917c`)", does not support the claim: that VA is the `bl`, and
  no InfoSet access follows. Slot 34 is correct but is reached by elimination,
  not by reading (§4).
- **`AVE_PrintSurfaceInfoSet` was the wrong instrument.** It walks only 5..26 and
  32..35 (26 of 35 slots), which is exactly the region where the two candidate
  mappings agree. Any derivation resting on it alone could not have decided the
  question either way.

## 9. What is still unknown

- The meaning of the individual bits in the flags word, and specifically why bit
  16 is stripped before every `CreateSurface` call.
- Whether slot 34 is genuinely dead or is filled on a DMV-capable SoC (§4).
- Where `GGMRef`/`GGMStats`/`GGMOutput` (slots 31–33) get their sizes — they have
  slots and are read by `AVE_CreateDataSurfaces`, but none of the six
  `CalcSurfaceInfo` functions writes them, so on M1 they are read as zero.
  Unchanged from [17-aux-engines-pools.md](17-aux-engines-pools.md) §9.
