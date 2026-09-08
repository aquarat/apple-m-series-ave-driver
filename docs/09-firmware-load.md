> ## Correction — the H13C conclusion below is wrong
>
> This document concluded that `H13C` is not the M1 Max firmware, on the basis
> that `ave_h13c.bin` contains only the codename `Erebus` and no `Nyx`/`Castor`.
> **That test does not discriminate.** All nineteen `AppleAVE2FW_*.im4p`
> variants in the IPSW contain `Erebus` and nothing else, so the string is a
> constant of the firmware codebase, not a per-SoC marker.
>
> The question is settled authoritatively by the IPSW's own
> `BuildManifest.plist`, which is what iBoot uses and which this document
> correctly notes is the only place the selection happens:
>
> ```
> j314cap (M1 Max 14")  ->  Firmware/ave/AppleAVE2FW_H13C.im4p
> ```
>
> **`H13C` is correct for M1 Max.** The full board→payload table is in
> `data/derived/board-to-ave-firmware.txt` and confirms the whole scheme:
> G = base, S = Pro, C = Max, D = Ultra.
>
> The rest of this document — the loading mechanism, `segment-ranges`, the
> IOVA-0 requirement, the CTRR snapshot, the ASC start and the heartbeat — is
> unaffected and was verified independently.

# Firmware load and coprocessor start

How `AppleAVE2.kext` gets the AVE firmware into memory and starts the RTKit
core. This is the prerequisite for any bring-up: a Linux driver has to
reproduce it.

Everything below is read directly out of the disassembly and cited with the VA
of the instruction it came from. Anything not read out of an instruction is
marked **inferred** or **unknown**.

Reproduce any line with:

```sh
python3 tools/disas.py --kext --addr 0xfffffe0008beb374 -n 0x714   # RetrieveInfo
python3 tools/disas.py --kext 'AVE_FwImg' --list
```

## Summary

| Question | Answer | Confidence |
|---|---|---|
| Who loads the firmware image? | **iBoot**, before the kernel runs | Confirmed |
| How does the kext find it? | `pre-loaded` + `segment-ranges` properties on the AVE device node | Confirmed |
| Where does it live? | 2 physical carve-outs (TEXT, DATA), mapped through the DART | Confirmed |
| Where must it be mapped? | **DART IOVA 0** | Confirmed |
| Does the kext ever load a file? | **No** — the in-kext fallback path is dead code | Confirmed |
| Which firmware variant for t6001? | **H13C IS correct.** `H13C` is `Erebus`/`t8150` | Confirmed |
| What starts the core? | ASC `CPU_CONTROL` +0x44 = 0x10, standard Apple ASC | Confirmed |
| Where is the ASC block? | `reg[1]` + **0x400000** (M1 family) | Confirmed |
| Firmware base handed to the core | 64-bit write at `reg[1]` + **0x50000** (skipped when iBoot-loaded) | Confirmed |
| Is the heartbeat mandatory? | **No** — host-side watchdog, writes nothing to the device | Confirmed |
| Heartbeat period | 3 s (first tick 6 s), `IOTimerEventSource` | Confirmed |

---

## 1. `AVE_FwImg` — where the image comes from

### 1.1 It comes from iBoot, not from the kext

`AVE_FwImg::RetrieveInfo(IOService *pProvider)` at `0xfffffe0008beb374` reads
two properties off the AVE device node:

| VA | property | use |
|---|---|---|
| `0xfffffe0008beb460` | `pre-loaded` | presence → `m_bIBootLoaded` (`strb w8,[x19]` at `0xfffffe0008beb47c`) |
| `0xfffffe0008beb564` | `segment-ranges` | `OSData`, the carve-out list |

Both are fetched through the provider's vtable slot `+0x2f0`
(`0xfffffe0008beb44c`, `0xfffffe0008beb558`) — **inferred** to be
`IORegistryEntry::getProperty(const char*, const IORegistryPlane*,
IOOptionBits)` from the 4-argument call with `w3 = 1`.

The log line for the first is `"... iBootLoad %p %p %d"`
(`0xfffffe00072981c6`). That name, plus the fact that nothing in `AVE_FwImg`
opens a file or references a firmware filename, is the whole story: **the
image is placed in DRAM by iBoot and the kext only adopts it.**

These are **runtime** properties. Neither appears on `ave0`/`ave1` in the
*restore* device tree (`data/blobs/adt.bin`, from
`DeviceTree.j314cap.im4p`): a byte search finds no `segment-ranges` at all,
and the single `pre-loaded` at file offset `0x2f564` belongs to
`iop-smc-nub`. iBoot injects them into the live ADT after it loads the image,
which is also where m1n1 reads them from. Do not conclude from the restore
ADT that AVE is not pre-loaded.

Confirming the negative: grepping the AppleAVE2 `__TEXT`
(file `0x230b00 + 0xd7184`) and `__TEXT_EXEC` (file `0x1b30bb0 + 0x1a059c`)
for `AVE2FW`, `im4p`, `H13`, `H14`, `H15` yields **no hits**. The only
`H1[3-7]` string anywhere in the 118 MB kernelcache is `H13P` at file offset
`0x5cb2ac`, which is outside AppleAVE2 and belongs to another kext.

### 1.2 `segment-ranges` layout

`OSData` cast at `0xfffffe0008beb584`
(`OSMetaClassBase::safeMetaCast`), bytes via vtable `+0x198`
(`0xfffffe0008beb5a8`, **inferred** `OSData::getBytesNoCopy`), length via
vtable `+0x160` (`0xfffffe0008beb5d0`, **inferred** `OSData::getLength`).

```
26d8:  lsr  w26, w21, #5          ; 0xfffffe0008beb6d8  num = len / 32
26dc:  sub  w8, w21, #0x20        ; 0xfffffe0008beb6dc
26e0:  cmp  w8, #0x3f             ; 0xfffffe0008beb6e0  require 0x20 <= len <= 0x5f
```

Assert string: `0 < num && num <= 2` (`0xfffffe00072982fe`).

The copy loop at `0xfffffe0008beb6f4`–`0xfffffe0008beb70c` reads **only two
fields out of each 32-byte entry** — `ldr x11, [x10]` and `ldr w12, [x10, #24]`,
stride `0x20`:

| entry offset | width | read by the kext? | meaning |
|---:|---|---|---|
| `+0x00` | u64 | **yes** | segment base (physical) |
| `+0x08` | u64 | no | iova |
| `+0x10` | u64 | no | remap |
| `+0x18` | u32 | **yes** | segment size |
| `+0x1c` | u32 | no | unknown |

The two fields the kext reads are confirmed from the disassembly. The names of
the other three come from m1n1, which already parses this property for DCP and
ISP — `struct adt_segment_ranges` in `m1n1-src/src/adt.h:78`:

```c
struct adt_segment_ranges {
    u64 phys;  u64 iova;  u64 remap;  u32 size;  u32 unk;
} PACKED;   /* 32 bytes */
```

So `segment-ranges` on an AVE node is the **same property, same layout** that
DCP, ISP and SIO already use. AppleAVE2 simply ignores the `iova`/`remap`
hints and programs the DART itself (§1.4).

Stored into the object as:

| `AVE_FwImg` offset | content | VA |
|---:|---|---|
| `+0x30` | `seg[0].base` | `0xfffffe0008beb6fc` |
| `+0x38` | `seg[0].size` | `0xfffffe0008beb6fc` |
| `+0x40` | `seg[1].base` | `0xfffffe0008beb6fc` (2nd iteration) |
| `+0x48` | `seg[1].size` | `0xfffffe0008beb6fc` (2nd iteration) |
| `+0x50` | `m_iSegmentNum` | `0xfffffe0008beb710` |

The log at `0xfffffe0008beb7b0` prints
`"CTRR Segments %p %p %p %d | %d - 0x%llx %lld 0x%llx %lld"` — two
(address, size) pairs, matching the table above.

The size sanity check at `0xfffffe0008beb72c`–`0xfffffe0008beb738`
(`seg0.size + seg1.size <= 2 * m_iMaxImageSize`) only selects the log level
(5 vs 6, `0xfffffe0008beb73c` / `0xfffffe0008beba80`). It is **not** an error
path; `RetrieveInfo` returns 0 either way (`0xfffffe0008beb7bc`).

### 1.3 The two segments are TEXT and DATA, and CTRR is the point

`AVE_FwImg::InitCTRRImage(AVE_DevInfo*, AVE_SurfaceMgr*, uint id, int size,
AVE_DART*)` at `0xfffffe0008beba88`:

```
bb8c:  ldr  w8, [x19, #80]        ; 0xfffffe0008bebb8c  m_iSegmentNum
bb90:  cmp  w8, #0x2              ; assert "m_iSegmentNum == 2"  (0xfffffe00072983e6)
bb9c:  bl   AVE_DART::GetMapper   ; 0xfffffe0008bebb9c  -> x25 = IOMapper*
```

Note `RetrieveInfo` accepts 1 or 2 segments but `InitCTRRImage` **requires
exactly 2**.

Two memory descriptors are built over the physical ranges — confirmed callee,
resolved from `com.apple.kernel`'s symtab:

| VA | call |
|---|---|
| `0xfffffe0008bebbbc` | `IOMemoryDescriptor::withOptions(&seg[0] /*this+0x30*/, count=1, offset=0, task=NULL, options=**0x22**, mapper)` → `this+0x58` |
| `0xfffffe0008bebbe0` | `IOMemoryDescriptor::withOptions(&seg[1] /*this+0x40*/, count=1, offset=0, task=NULL, options=**0x23**, mapper)` → `this+0x60` |

Asserts name them `m_pcCTRRTextDesc` (`0xfffffe000729844e`) and
`m_pcCTRRDataDesc` (`0xfffffe00072984ba`), and the error strings say
`"fail to create IOMemoryDescriptor of CTRR TEXT"` / `"... of CTRR"`.
So **segment 0 is the firmware's text, segment 1 its data.**

`0x22` / `0x23` decompose as `kIOMemoryTypePhysical (0x20) | direction` with
direction 2 (`kIODirectionOut`) for TEXT and 3 (`kIODirectionOutIn`) for DATA
— **inferred** from the standard IOKit option encoding, not read from a
constant in this kext. The raw values are what is confirmed.

Both are then `prepare()`d (vtable `+0x00`, `0xfffffe0008bebc08` and
`0xfffffe0008bebe6c`; error strings `"fail to prepare IOMemoryDescriptor
Text"` / `"... Data"`).

### 1.4 The IOVA layout — the firmware must be at DART address 0

```
c19c:  ldr   w8, [x19, #56]           ; seg0.size
c1a0:  ldr   w9, [x19, #72]           ; seg1.size
c1a4:  add   w27, w8, w9              ; 0xfffffe0008bec1a4  total
c1f4:  blraa  <mapper vtable +0x890>  ; 0xfffffe0008bec1f4  iovmMapMemory
```
`iovmMapMemory(md=NULL, off=0, length=total, mapOptions=3, ..., &mapAddress)`
— named by its failure string `"fail to iovmMapMemory"`
(`0xfffffe000729857c`). It reserves an IOVA window of `seg0.size +
seg1.size`; the result lands at `[x29,#-88]`.

Then two inserts through mapper vtable `+0x8a0` (`iovmInsert`, named by
`"fail to iovmInsert .text section"` `0xfffffe00072985bd` and
`".data section"` `0xfffffe0007298664`):

| VA | segment | options | IOVA | physical | length |
|---|---|---|---|---|---|
| `0xfffffe0008bec30c` | TEXT | `devType == 3 ? 3 : 1` (`0xfffffe0008bec2e0`) | `mapAddress` | `seg[0].base` | `seg[0].size` |
| `0xfffffe0008bec4c0` | DATA | `3` (`0xfffffe0008bec4b4`) | `mapAddress + seg0.size` (`0xfffffe0008bec490`) | `seg[1].base` | `seg[1].size` |

So **TEXT and DATA are contiguous in IOVA space, in that order.**

Then the constraint that matters most:

```
c3e8:  bl   AVE_DevInfo::GetDevType   ; 0xfffffe0008bec3e8
c3ec:  cmp  w0, #0x2
c3f0:  b.le 0xfffffe0008bec488        ; devType <= 2: skip the check
c3f8:  bl   AVE_DevInfo::GetDevType
c400:  cmp  w0, #0xb
c404:  b.gt 0xfffffe0008bec48c        ; devType > 11: skip the check
c408:  cbz  x8, 0xfffffe0008bec48c    ; 0xfffffe0008bec408  mapAddress must be 0
```
Assert string `addr == 0` (`0xfffffe000729865a`), error string
`"firmware is not mapped to address 0"` (`0xfffffe0007298609`).

**For `devType` in 3..11 — which includes t6001 (`devType = 10`, see §3) —
the firmware image must be mapped at DART IOVA 0.** This matches the firmware
image being an `MH_PRELOAD` Mach-O with `vmaddr 0` (docs/02-firmware.md).

A Linux driver must therefore reserve IOVA 0 in the AVE DART for the firmware
and place TEXT then DATA contiguously from there.

### 1.5 The DATA segment is snapshotted and restored on every start

CTRR (Configurable Text Read-only Region) locks the *text*. The data segment
is writable and gets dirtied by a running firmware, so the driver keeps a
pristine copy.

`AVE_FwImg::AcquireCTRRData()` at `0xfffffe0008bec9c0`:

| VA | operation |
|---|---|
| `0xfffffe0008beca74` | require `m_iSegmentNum >= 2` and `seg1.size != 0` |
| `0xfffffe0008becab0` | `m_pcCTRRDataDesc->map(1)` (vtable `+0x228`) → `this+0x68` (`m_pcCTRRDataMap`, assert `0xfffffe0007298796`) |
| `0xfffffe0008becadc` | mapping vtable `+0x138` → kernel VA → `this+0x78` (**inferred** `IOMemoryMap::getVirtualAddress`) |
| `0xfffffe0008becb84` | `IOMallocTypeVarImpl(seg1.size)` → `this+0x70` (`m_piCTRRData_Org`, assert `0xfffffe0007298807`) |
| `0xfffffe0008becb98` | `memcpy(this+0x70, this+0x78, seg1.size)` |

`AVE_FwImg::RestoreCTRRData()` at `0xfffffe0008becdf4` is the inverse, and is
the *entire* function body:

```
cea8:  ldr  x0, [x19, #120]      ; dst = kernel VA of the CTRR DATA mapping
ceb0:  ldr  x1, [x19, #112]      ; src = pristine copy
ceb8:  ldr  x2, [x19, #72]       ; len = seg1.size
cebc:  bl   _memcpy              ; 0xfffffe0008becebc
```

`AVE_FwImg::UpdateImage()` at `0xfffffe0008bee1f8` is the per-power-up entry
point and picks between the two worlds:

```
e2c0:  ldrb w8, [x19]
e2c4:  tbz  w8, #0, 0xfffffe0008bee2d8
e2cc:  bl   AVE_FwImg::RestoreCTRRData   ; iBoot-loaded  -> always returns 0
e2dc:  bl   AVE_FwImg::UpdateBufImage    ; otherwise
```

**A Linux driver must re-copy the firmware's data segment before every start**,
from a snapshot taken before the coprocessor first ran.

`ReleaseCTRRData` (`0xfffffe0008becf70`) frees the copy and releases the
mapping; `UninitCTRRImage` (`0xfffffe0008bec78c`) calls `complete()`
(vtable `+0x220`, `0xfffffe0008bec880` / `0xfffffe0008bec8d4`) and `release()`
on both descriptors. Neither appears to undo `iovmMapMemory`; where that
happens is **unknown**.

### 1.6 The non-preloaded path is dead code in this build

`AVE_FwImg::InitBufImage` (`0xfffffe0008bed168`) does allocate a real buffer:
`AVE_GetSurfaceCfg(0x1c)` (`0xfffffe0008bed25c`) — surface index 28, name
**`FwImage`** (from the table at `0xfffffe0007ee1050`, stride 0x10) — then
`AVE_SurfaceMgr::CreateSurface(...)` at `0xfffffe0008bed28c` storing the
result at `this+0x80`.

But `AVE_FwImg::UpdateBufImage` (`0xfffffe0008bed614`), which is what would
actually place bytes, **cannot succeed**:

```
d6e8:  ldr  x0, [x19, #16]  ; AVE_DevInfo*
d6ec:  bl   AVE_DevInfo::GetChipType
d6f0:  ldr  x0, [x19, #16]
d6f4:  bl   AVE_DevInfo::GetDevType
d6f8:  mov  w0, #0x6e       ; result discarded -- straight into the log check
d704:  cbz  w0, 0xfffffe0008bed848  ; -> w20 = -1002
```
Every path from `0xfffffe0008bed6e8` reaches `0xfffffe0008bed848`
(`mov w20, #0xfffffc16` = **-1002**), with assert `pMap != nullptr`
(`0xfffffe00072989df`) and error `"can not find matched firmware image"`
(`0xfffffe0007298992`).

The reason is visible one function away. `AVE_FwImg_FindMap(_E_AVE_ChipType,
_E_AVE_DevType, uint)` at `0xfffffe0008beb0d8` is:

```
b0d8:  bti  c
b0dc:  mov  x0, #0x0
b0e0:  ret
```

It is a stub returning NULL. The compiler inlined it, which is why only the
two side-effecting getters survive. **The kext has no way to load a firmware
image itself; the iBoot-preloaded path is the only functional one.**

### 1.7 `GetBaseAddr` and its consumers

`AVE_FwImg::GetBaseAddr()` at `0xfffffe0008bee3ac`:

```
e474:  ldrb w8, [x19]
e478:  tbnz w8, #0, 0xfffffe0008bee498   ; iBoot-loaded -> return 0
e47c:  ldr  x0, [x19, #128]              ; m_pcBufImage
e480:  cbz  x0, 0xfffffe0008bee498       ; none -> return 0
e484:  ldr  w1, [x19, #32]               ; instance id
e48c:  bl   AVE_Surface::GetDARTAddr(id, 0)
```

It returns the **device-side (IOVA) address of the firmware image buffer**,
and **returns 0 whenever the image was pre-loaded by iBoot** — which is
consistent with §1.4: on a pre-loaded system the firmware genuinely is at
IOVA 0.

Callers (found by scanning `__TEXT_EXEC` for `bl` to `0xfffffe0008bee3ac`):

| caller | VA of the `bl` |
|---|---|
| `AVE_HwC::StartUpIOP` | `0xfffffe0008c1f4ac` |
| `AVE_IOP::Config` | `0xfffffe0008c4086c`, `0xfffffe0008c408bc` |
| `AVE_IPC::Init` | `0xfffffe0008c42b64` |

### 1.8 `AVE_FwImg::Init` — the whole decision tree

`Init(IOService *pProvider, const _S_AVE_Cfg *pCfg, AVE_DevInfo *pDevInfo,
AVE_SurfaceMgr *pSurfaceMgr, uint id, AVE_DART *pDART, int size)` at
`0xfffffe0008bed974`. Parameter names come from the assert string at
`0xfffffe0007298a6e`:
`pProvider != nullptr && pCfg != nullptr && pDevInfo != nullptr &&
pSurfaceMgr != nullptr && id < 4 && pDART != nullptr`.

```
daa0:  bl   AVE_DevInfo::GetChipType
daa4:  mov  w8, #0x300000
daa8:  mov  w9, #0x200000
daac:  cmp  w0, #0x3
dab0:  csel w8, w9, w8, eq
dab4:  str  w8, [x19, #136]      ; m_iMaxImageSize
```
→ **`m_iMaxImageSize` = 2 MiB if `chipType == 3`, otherwise 3 MiB**
(`0xfffffe0008bedaa4`–`0xfffffe0008bedab4`). Assert name
`size <= m_iMaxImageSize` (`0xfffffe0007298b6d`).

Then:

| step | VA | effect |
|---|---|---|
| `RetrieveInfo(pProvider)` | `0xfffffe0008bedac0` | sets `m_bIBootLoaded`, segments |
| `if (GetDevID() == 5) m_bIBootLoaded = false` | `0xfffffe0008bedbd4`–`0xfffffe0008bedbe0` | DevID 5 forces the buffer path |
| `if (GetChipType() == 3 && size > m_iMaxImageSize) → -1018` | `0xfffffe0008bedbe8`–`0xfffffe0008bedbfc` | `"firmware is too big"` |
| `size = max(m_iMaxImageSize, pCfg->[8])` | `0xfffffe0008bede8c`/`0xfffffe0008bedf3c` | see below |
| iBoot-loaded → `InitCTRRImage(...)` then `AcquireCTRRData()` | `0xfffffe0008bedeb8`, `0xfffffe0008bedfe8` | |
| otherwise → `InitBufImage(...)` | `0xfffffe0008bedf60` | |
| success stores | `0xfffffe0008bee074`–`0xfffffe0008bee07c` | `+0x18`=SurfaceMgr, `+0x20`=id, `+0x28`=DART |

`pCfg->[8]` is the `ave-fwsize` boot-arg — see §3.4. In the iBoot-loaded case
`AVE_HwC::Init` passes `size = 0` (`mov w7, #0x0` at `0xfffffe0008c1b5c4`),
so no extra surface is created (`subs w26, w24, w27` / `b.le` at
`0xfffffe0008bec598`).

### 1.9 `AVE_FwImg` object layout

| offset | field | evidence VA |
|---:|---|---|
| `+0x00` | `m_bIBootLoaded` (u8) | `0xfffffe0008beb47c` |
| `+0x10` | `AVE_DevInfo*` | `0xfffffe0008bed6e8` |
| `+0x18` | `AVE_SurfaceMgr*` | `0xfffffe0008bee074` |
| `+0x20` | instance id (u32) | `0xfffffe0008bee078` |
| `+0x28` | `AVE_DART*` | `0xfffffe0008bee07c` |
| `+0x30`/`+0x38` | `seg[0]` base / size | `0xfffffe0008beb6fc` |
| `+0x40`/`+0x48` | `seg[1]` base / size | `0xfffffe0008beb6fc` |
| `+0x50` | `m_iSegmentNum` | `0xfffffe0008beb710` |
| `+0x58` | `m_pcCTRRTextDesc` | `0xfffffe0008bebbc0` |
| `+0x60` | `m_pcCTRRDataDesc` | `0xfffffe0008bebbe4` |
| `+0x68` | `m_pcCTRRDataMap` | `0xfffffe0008becab4` |
| `+0x70` | `m_piCTRRData_Org` | `0xfffffe0008becb88` |
| `+0x78` | kernel VA of CTRR DATA | `0xfffffe0008becae0` |
| `+0x80` | `m_pcBufImage` (`AVE_Surface*`) | `0xfffffe0008bed270` |
| `+0x88` | `m_iMaxImageSize` (int) | `0xfffffe0008bedab4` |

### 1.10 Error codes

| value | encoding | meaning / string |
|---:|---|---|
| -1000 | `0xfffffc18` | descriptor create/prepare failure (`0xfffffe0008bebdd0`) |
| -1001 | `0xfffffc17` | wrong parameters (`0xfffffe0008bedcec`) |
| -1002 | `0xfffffc16` | no matching map / bad device (`0xfffffe0008bed848`) |
| -1003 | `0xfffffc15` | allocation failure (`0xfffffe0008bebffc`, `0xfffffe0008becd84`) |
| -1004 | `0xfffffc14` | kernel mapping failure (`0xfffffe0008becd28`) |
| -1008 | `0xfffffc10` | (`0xfffffe0008beb8e8`) |
| -1009 | `0xfffffc0f` | firmware image not ready (`0xfffffe0008bed8a4`) |
| -1018 | `0xfffffc06` | firmware too big (`0xfffffe0008bee0e8`) |
| -1019 | `0xfffffc05` | CTRR segment count out of range (`0xfffffe0008beba78`) |

---

## 2. `AVE_IOP` — starting and stopping the core

### 2.1 Dispatch is a static per-generation table

`AVE_IOP::Init` (`0xfffffe0008c40274`) stores no function pointer; it only
saves its arguments (`0xfffffe0008c403a0`–`0xfffffe0008c403ac`):

| `AVE_IOP` offset | field |
|---:|---|
| `+0x00` | `_S_AVE_Cfg*` |
| `+0x08` | `AVE_DevInfo*` |
| `+0x10` | instance id (asserted `< 4` at `0xfffffe0008c40388`) |
| `+0x18` | `AVE_Reg*` |
| `+0x20` | `AVE_SVECtrl*` |
| `+0x28` | `AVE_AXI2AF*` |
| `+0x30` | `AVE_FwImg*` |

Every entry point re-dispatches through `AVE_DevInfo::GetChipType()` into
`gsc_saAVE_IOP_If` at **`0xfffffe0007ee07f0`**, stride 40, index
`chipType - 1`. The assert string names it:
`type <= sizeof(gsc_saAVE_IOP_If)/sizeof(*gsc_saAVE_IOP_If) &&
gsc_saAVE_IOP_If[type - 1].Start != nullptr` (`0xfffffe00072a3760`).

| struct field | offset | used by |
|---|---:|---|
| `Config` | `+0x00` | `AVE_IOP::Config` (`mov x9,#-40`, `0xfffffe0008c4083c`) |
| `Start` | `+0x08` | `AVE_IOP::Start` (`mov x9,#-32`, `0xfffffe0008c40bd0`) |
| *(unused, NULL in all 21)* | `+0x10` | — |
| `CheckIdle` | `+0x18` | `AVE_IOP::Stop` (`0xfffffe0008c40e94`) and `CheckIdle` (`0xfffffe0008c41638`) |
| `GetCurrTime` | `+0x20` | `AVE_IOP::GetCurrTime` (`0xfffffe0008c41384`) |

Bounds check `cmp w0, #0x16 / b.cs` (`0xfffffe0008c40bc4`) → chip types 1..21,
else -1002.

Decoding the table (each entry's `Start` pointer, rebased against kernelcache
base `0xfffffe0007004000`):

| type | codename | type | codename | type | codename |
|---:|---|---:|---|---:|---|
| 1 | Hypnos | 8 | Atlas | 15 | Gaia |
| 2 | Leto | 9 | Hera | 16 | Uranus |
| 3 | Rhea | 10 | Tethys | 17 | Upis |
| 4 | Panda | 11 | Nemesis | 18 | Ersa |
| 5 | Acis | 12 | Janus | 19 | Erebus |
| 6 | **Castor** | 13 | Themis | 20 | Aion |
| 7 | **Nyx** | 14 | Pan | 21 | Ares |

(Verified independently by two routes: decoding the table's raw pointers, and
the `AVE_IOP_Start_*` symbol addresses.)

### 2.2 Register access

All accesses go through `AVE_Reg::Write32(_E_AVE_RegType, int off, uint32)`
(`0xfffffe0008c53e58`) / `Read32` (`0xfffffe0008c53df0`) / `Write64`
(`0xfffffe0008c53e88`) / `Read64` (`0xfffffe0008c53e24`), which are trivial:
`base = ((void**)(this+0x40))[type]; *(u32*)(base+off) = val`
(`0xfffffe0008c53e70`–`0xfffffe0008c53e78`).

`AVE_Reg::Init` (`0xfffffe0008c532d0`) fills that array by calling the
provider's `mapDeviceMemoryWithIndex(i, 0)` (vtable `+0x710`,
`0xfffffe0008c5336c`), storing at `this+0x40+8*i`. So **regtype N == ADT `reg`
entry N**. Every `AVE_IOP` access uses **regtype 1**, which for
`/arm-io/ave0` is `0x20D800000` size `0x800000`
(`data/derived/adt-ave-nodes.txt`).

### 2.3 Start

`AVE_IOP_Start_<X>(AVE_Reg*)` — example `AVE_IOP_Start_Nyx`
(`0xfffffe0008c35628`), which is the t6001 path:

| order | op | offset (regtype 1) | value | VA |
|---:|---|---|---|---|
| 1 | Write32 | `0x400808` | `0x1` | `0xfffffe0008c356ec` |
| 2 | Write32 | **`0x400044`** | `0x0` | `0xfffffe0008c35704` |
| 3 | Write32 | `0x400400` | `0x10000` | `0xfffffe0008c35718` |
| 4 | Write32 | **`0x400044`** | **`0x10`** | `0xfffffe0008c35730` |

It then returns 0. **There is no ready poll and no readback in `Start`.**

All 21 variants are identical modulo a block base of `0x400000` or `0x600000`:

- base **`0x400000`**: Acis, Atlas, **Castor**, Hera, Hypnos, Janus, Nemesis,
  **Nyx**, Panda, Tethys, Themis
- base **`0x600000`**: Ares, Aion (`0xfffffe0008ca825c`…), Erebus, Ersa
  (`0xfffffe0008b386a4`…), Gaia, Leto, Pan, Upis, Uranus
- **Panda** writes `0x400` (not `0x10000`) to base+`0x400`
  (`0xfffffe0008c364e0`)
- **Rhea** (type 3) is the legacy outlier, no block base
  (`0xfffffe0008c36bf4` on): `W32(0x808,1)`, `W32(0x44,0)`, `W32(0x44,0x10)`,
  then `v = R32(0x80c)` (`0xfffffe0008c36c3c`), `W32(0x80c, v)`, `W32(0x80c,
  v|2)` (`0xfffffe0008c36c68`)

### 2.4 CheckIdle

`AVE_IOP_CheckIdle_Nyx` (`0xfffffe0008c357dc`):

```
589c:  <Read32(regtype 1, 0x400048)>
58a4:  tst   w0, #0x3
58a8:  csel  w20, w0, wzr, eq        ; return (v & 3) == 0 ? v : 0
```
Same in all 21 (offset = block base + **`0x48`**). Only **Aion** uses
`tst w0, #0x2` (`0xfffffe0008ca8414`). Rhea reads plain `0x48`
(`0xfffffe0008c36dd0`).

`AVE_IOP::CheckIdle` maps this to `csel w20, wzr, #-1016, eq`
(`0xfffffe0008c41674`): **nonzero from the per-generation function = failure
(-1016)**. The semantics of `CPU_STATUS` bits 0 and 1 as AVE uses them are
**unknown** — the mask and polarity are read, the meaning is not.

### 2.5 Stop

`AVE_IOP::Stop` (`0xfffffe0008c40da4`) **performs no register write at all.**
It loads `gsc_saAVE_IOP_If[type-1].CheckIdle` (`0xfffffe0008c40eb8`) and
polls:

```
w22 = 10000                                ; 0xfffffe0008c40ebc
loop: ret = CheckIdle_<X>(AVE_Reg*)        ; 0xfffffe0008c40ec8
      if (ret != 0) consec = 0             ; 0xfffffe0008c40ed4
      else if (consec > 1) return 0        ; 0xfffffe0008c40edc -> 0xfffffe0008c41178
      limit = cfg->[0x18] * 10000          ; 0xfffffe0008c40ee8
      if (iter > limit) return -1017       ; 0xfffffe0008c40ef4 -> 0xfffffe0008c411d0
      IODelay(50); iter++                  ; 0xfffffe0008c40efc
```
**50 µs per iteration, three consecutive idle reads required**, iteration cap
`cfg->[0x18] * 10000`. No `mach_absolute_time` deadline. Timeout string
`"stopping IOP time out %p %d %d %d 0x%x"` (`0xfffffe00072a380a`).

A register-agnostic scan of AppleAVE2's whole `__TEXT_EXEC` — decoding every
32-bit `MOVZ Wd, #0x44|#0x48` followed within 64 bytes by
`MOVK Wd, #0x40|#0x60, lsl #16` on the same `Wd` — finds exactly **40** sites:
the 20 `AVE_IOP_Start_*` functions (`+0x44`) and the 20
`AVE_IOP_CheckIdle_*` functions (`+0x48`), and nothing else. (Rhea is absent
from the 40 because it uses bare `0x44`/`0x48` with no `MOVK`.)

**Nothing in the kext ever clears `CPU_CONTROL`.** The core is stopped by
sending the firmware a `Halt` command (§4.3) and waiting for it to quiesce,
not by taking the run bit away.

### 2.6 Is this the standard Apple ASC pattern?

**Yes.** `CPU_CONTROL` at **`+0x44`** written **`0x10`** to run
(`0xfffffe0008c35730`), preceded by a `0x0` write (`0xfffffe0008c35704`).
That matches `m1n1-src/src/asc.c` exactly — `#define ASC_CPU_CONTROL 0x44`
(line 8), `#define ASC_CPU_CONTROL_START 0x10` (line 9), and
`set32(cpu_base + ASC_CPU_CONTROL, ASC_CPU_CONTROL_START)` at line 71.

The status read at **`+0x48`** (`0xfffffe0008c3589c`) is the offset that is
confirmed; naming it `CPU_STATUS` is **inferred** from its position — m1n1's
`asc.c` does not define a register there (it probes liveness by reading
`CPU_CONTROL` back, line 81).

The one wrinkle a Linux driver has to know: **the ASC block is not at offset 0
of the AVE register window.** It sits at `reg[1] + 0x400000` on the M1 family
(and `+0x600000` on newer parts).

The two extra writes (block+`0x808` = 1, block+`0x400` = `0x10000`) are not in
m1n1's ASC register map and their meaning is **unknown**.

### 2.7 Config — handing the firmware base to the core

`AVE_IOP::Config` (`0xfffffe0008c4073c`) gets its `uint64` from
`AVE_FwImg::GetBaseAddr()` on `this+0x30` (`0xfffffe0008c4086c`). Guard: if
`((u8*)fwimg)[0] & 1` (i.e. iBoot-loaded) **and** chip type != 3, `Config` is
skipped entirely and returns 0 (`0xfffffe0008c40818`–`0xfffffe0008c40828`).

> For t6001 (chip type 7, iBoot-loaded) that guard means **`Config` is a
> no-op**: the base register is not written at all, because the firmware is
> already at IOVA 0 where the core expects it. Read from the branch at
> `0xfffffe0008c40818`; the consequence is **inferred**.

When it does run, `AVE_IOP_Config_<X>` (Nyx, `0xfffffe0008c353f0`) is
identical in all 20 non-Rhea variants:

```
54e0:  Read64 (regtype 1, 0x50000)             ; logging only
5558:  x8 = addr & 0x3FFFFFFF800               ; 0xfffffe0008c35558  bits [42:11], no shift
555c:  x9 = 0x0102000000000000                 ; 0xfffffe0008c3555c
5570:  Write64(regtype 1, 0x50000, x8 | x9)    ; 0xfffffe0008c35570
```

**Offset `0x50000`, 64-bit.** Note it is *not* block-relative: `0x50000` for
both the `0x400000` and `0x600000` families (verified for all 20, e.g. Ersa
`0xfffffe0008b38528`, Aion `0xfffffe0008ca80e0`). The address is masked in
place to bits 42:11 (2 KiB alignment) and OR'd with `0x0102` in the top 16
bits; the meaning of those bits is **unknown**.

Rhea instead (`0xfffffe0008c36954`–`0xfffffe0008c36a90`): `W32(0x18,0)`,
`W32(0x20,0)`, `W32(0x28,0x70800000)`, `W32(0x30,0)`, `R32(0x8)`,
`W32(0x8, (u32)addr)` (`0xfffffe0008c36a68`), `W32(0x10,0)`, `W32(0x38,1)`.

### 2.8 GetCurrTime

`AVE_IOP_GetCurrTime64(AVE_Reg*, uint counterOff, uint freqOff, int mult)`
(`0xfffffe0008c400e8`): reads a 64-bit counter (`0xfffffe0008c40118`) and a
32-bit frequency (`0xfffffe0008c4012c`), returns `counter / ((freq*mult)/1e6)`
(`0xfffffe0008c40154`) — i.e. **microseconds** — with a `/24` fallback when
the frequency register reads 0 (`0xfffffe0008c401bc`).

| generation family | counter (64-bit) | freq (32-bit) | mult |
|---|---|---|---|
| `0x400000` family (incl. Castor, Nyx) | **`0x178000`** | **`0x160020`** | 1 |
| `0x600000` family | **`0x378000`** | **`0x360020`** | 1 |
| Acis (`0xfffffe0008c2af40`), Panda (`0xfffffe0008c367f0`) | `0x178000` | `0x160020` | `0xF4240` |
| Rhea (`0xfffffe0008c36f4c`) | `GetCurrTime32(hi=0xc038, lo=0xc030)` | — | — |

---

## 3. Device identification — and the H13C question

### 3.1 The selector is the ADT `soc-id` string

`AVE_DevInfo::RetrieveDevID(IORegistryEntry*, _E_AVE_DevID*)` at
`0xfffffe0008baad50`:

```
ada8:  add  x1, x1, #0x3a       ; 0xfffffe000728f03a  "soc-id"
adb8:  blraa <provider vtable +0x2f0>       ; getProperty
adc8:  bl   OSMetaClassBase::safeMetaCast   ; -> OSData
adf0:  <getBytesNoCopy>
adfc:  bl   AVE_DevStr2ID       ; 0xfffffe0008babaec
ae08:  cmp  w8, #0x22           ; require 1 <= devID <= 34
```
Assert `AVE_DevID_None < devID && devID < AVE_DevID_Max`
(`0xfffffe000728f112`); error `"%s is not supported"` (`0xfffffe000728f0d8`).

`AVE_FindByDevStr(const char*)` (`0xfffffe0008baba00`) walks a **34-entry,
16-byte** table at **`0xfffffe0007edf0f0`** (`0xfffffe0008baba34`,
`mov w22, #0x22` at `0xfffffe0008baba3c`), formatting each entry with
`"%s%d"` (`0xfffffe000728f395`) into a 32-byte buffer and `strcmp`-ing it
(`0xfffffe0008baba6c`). The prefix string in every entry is `"t"`.

Dumping that table (entry 0 is an empty placeholder — null string, number 0,
DevID 0 — which is why the valid DevID range starts at 1):

| entry | string | DevID | | entry | string | DevID |
|---:|---|---:|---|---:|---|---:|
| 1 | `t8310` | 1 | | 18 | `t6022` | 18 |
| 2 | `t8320` | 2 | | 19 | `t8120` | 19 |
| 3 | `t8010` | 3 | | 20 | `t8122` | 20 |
| 4 | `t8011` | 4 | | 21 | `t6030` | 21 |
| 5 | `t8012` | 5 | | 22 | `t6031` | 22 |
| 6 | `t8020` | 6 | | 23 | `t6032` | 23 |
| 7 | `t8027` | 7 | | 24 | `t6034` | 24 |
| 8 | `t8030` | 8 | | 25 | `t8130` | 25 |
| 9 | `t8101` | 9 | | 26 | `t8132` | 26 |
| 10 | `t8103` | 10 | | 27 | `t6040` | 27 |
| 11 | **`t6000`** | **11** | | 28 | `t6041` | 28 |
| 12 | **`t6001`** | **12** | | 29 | `t8140` | 29 |
| 13 | **`t6002`** | **13** | | 30 | `t8142` | 30 |
| 14 | `t8110` | 14 | | 31 | `t6050` | 31 |
| 15 | `t8112` | 15 | | 32 | `t8150` | 32 |
| 16 | `t6020` | 16 | | 33 | `t8152` | 33 |
| 17 | `t6021` | 17 | | | | |

The ADT confirms the property exists and its values
(`data/blobs/adt.bin`, raw bytes at file offsets `0x44b14` and `0x453a4`):

```
/arm-io/ave0   soc-id = "t6000"
/arm-io/ave1   soc-id = "t6001"   (also sve-id = 1)
```

Note the two instances on one j314c declare **different** `soc-id`s. That is
read straight out of the ADT; why Apple does it is **unknown**.

### 3.2 DevID → DevType / ChipType

`AVE_DevCap_Find(devID)` (`0xfffffe0008ba9ea8`) indexes
`gsc_saAVE_DevCap` at **`0xfffffe0007edba00`**, stride **`0x48`**
(`mov w8, #0x48` at `0xfffffe0008ba9eac`), bound `< 0x23`
(`0xfffffe0008ba9ed0`). Entry layout, from `AVE_DevInfo::Init`
(`0xfffffe0008baa774`–`0xfffffe0008baa788`) and the accessors at
`0xfffffe0008bab780`+:

| entry offset | field |
|---:|---|
| `+0x00` | DevID |
| `+0x04` | DevType |
| `+0x08` | ChipType |
| `+0x10` | `_S_AVE_DevCap_CEntry*` |

`AVE_DevInfo` accessors: `GetDevID` `+0x08` (`0xfffffe0008bab784`),
`GetDevType` `+0x0c` (`0xfffffe0008bab790`), `GetChipType` `+0x10`
(`0xfffffe0008bab79c`), `GetDevRevision` `+0x14`, `GetDevArch` `+0x18`,
`GetDevNum` `+0x1c`, `GetDevSubIDFlag` `+0x28`.

For the M1 family:

| soc-id | DevID | DevType | ChipType | IOP codename |
|---|---:|---:|---:|---|
| `t8103` | 10 | 8 | 5 | Acis |
| **`t6000`** | **11** | **9** | **6** | **Castor** |
| **`t6001`** | **12** | **10** | **7** | **Nyx** |
| `t6002` | 13 | 11 | 7 | Nyx |

Consequences for t6001: `ChipType == 7`, so `m_iMaxImageSize` = **3 MiB**
(§1.8, the `!= 3` branch), and `DevType == 10`, which is inside 3..11, so the
**IOVA-0 requirement applies** (§1.4).

Two other DevInfo properties are read off the node:
`active-mcc-bit-vector` for the revision (`0xfffffe0008bab1a4`, an `OSData` of
length 4) and `sve-id` (`0xfffffe0008bab54c`).

### 3.3 Firmware variant selection is **not** in the kext

There is no firmware-file selection logic in `AppleAVE2.kext` at all — see
§1.1 and §1.6. The variant is chosen by **iBoot**, which loads
`Firmware/ave/AppleAVE2FW_<VARIANT>.im4p` (FourCC `avef`) into the carve-outs
that then appear as `segment-ranges`.

**But the question can still be settled, from the image itself.** Each AVE
firmware build contains exactly one set of device-capability tables, named
after the AVE hardware codename. In `data/blobs/ave_h13c.bin` (byte-identical
to the payload of `AppleAVE2FW_H13C.im4p`, verified with `pyimg4 im4p
extract` + `cmp`):

```
_gc_sAVE_DevCap_PDMap_Erebus
_gc_sAVE_DevCap_DPMMap_AVC_Erebus
_gc_sAVE_DevCap_DPMMap_HEVC_Erebus
_gc_sAVE_DevCap_DPMMap_LRME_Erebus
_gc_sAVE_DevCap_DPMMap_MCTF_Erebus
_gc_sAVE_DevCap_DPMMap_DMV_Erebus
_gc_sAVE_DevCap_DPMMap_GGM_Erebus
```

That is the complete list. Searching the image for all 21 codenames from the
table in §2.1 finds **only `Erebus`**, and in particular **no `Castor` and no
`Nyx`**.

`Erebus` is ChipType 19, and the kext pairs it with SoC number 8150
(`gsc_saAVE_AXI2AF_Cfg_Erebus_8150`, `0xfffffe0007edb398`), i.e. `soc-id`
`t8150`, DevID 32.

> ### `H13C` is **not** the M1 Max firmware. Settled.
>
> `AppleAVE2FW_H13C.im4p` in the current MacBookPro18,3 IPSW builds
> device-capability tables for **`Erebus` / `t8150`**. M1 Max is
> `t6001` → ChipType 7 → **`Nyx`** (and M1 Pro is ChipType 6 → `Castor`).
>
> The inference recorded in [02-firmware.md](02-firmware.md) was wrong, and
> so was the evidence offered for it. That evidence was the source paths
> `CAVCController_H13C.cpp` / `CHEVCController_H13C.cpp` — but their full
> paths (image offsets `0x1314dd` and `0x13253a`) are
> `./AppleAVE2FW/Legacy/CAVCController_H13C.cpp` and
> `./AppleAVE2FW/Legacy/CHEVCController_H13C.cpp`. They sit under
> **`Legacy/`**: a controller class named after an old part, present in an
> image whose actual device tables say `Erebus`. A source filename under
> `Legacy/` is not a statement about which SoC the image targets.
>
> **Loading `H13C` on an M1 Max would have been exactly the hard-to-diagnose
> bring-up failure 02-firmware.md warned about.**

Which variant *is* M1 Max is not answerable from the blobs currently in
`data/blobs/` — only `H13C` was fetched. The test is mechanical:

```sh
# for each Firmware/ave/AppleAVE2FW_*.im4p in the IPSW
./.venv/bin/pyimg4 im4p extract -i AppleAVE2FW_<V>.im4p -o /tmp/<V>.bin
strings -a /tmp/<V>.bin | grep -o 'DevCap_PDMap_[A-Za-z]*' | sort -u
```

The variant whose payload reports `DevCap_PDMap_Nyx` is t6001 (M1 Max); the
one reporting `DevCap_PDMap_Castor` is t6000 (M1 Pro). Note this may need
**two different images** for a single M1 Max, since `ave0` declares `t6000`
and `ave1` declares `t6001` (§3.1) — that is **inferred** from the ADT plus
the one-codename-per-image observation, not confirmed.

### 3.4 Boot-arg overrides

`AVE_Cfg_RetrieveBootArgs(_S_AVE_Cfg*)` at `0xfffffe0008b63864` calls
`PE_parse_boot_argn` for:

| boot-arg | `_S_AVE_Cfg` offset | store VA | log string |
|---|---:|---|---|
| `ave-platform` | `+0x00` | `0xfffffe0008b63934` | `"device id 0x%x"` |
| `ave-devid` | `+0x04` | `0xfffffe0008b639ec` | |
| **`ave-fwsize`** | **`+0x08`** | `0xfffffe0008b63aa4` | `"firmware image size 0x%x"` |
| `ave-fwlogsize` | `+0x0c` | `0xfffffe0008b63b5c` | `"firmware log size 0x%x"` |
| `ave-log` | (64-bit) | `0xfffffe0008b63ba4` | `"log 0x%llx"` |
| `axi2af_parity_enable` | | `0xfffffe0008b63c2c` | |
| `ave-flag` | | `0xfffffe0008b63ce0` | |

`+0x08` is the `pCfg->[8]` used in `AVE_FwImg::Init` (§1.8), so `ave-fwsize`
can only *enlarge* the image allocation: `size = max(m_iMaxImageSize,
ave-fwsize)`. `_S_AVE_Cfg + 0x18` is the `AVE_IOP::Stop` timeout multiplier
(§2.5).

Also confirmed: `AVE_AXI2AF::Init` (`0xfffffe0008b5c308`) selects its tunables
via `AVE_AXI2AF_GetTunables(DevID, instanceId, revision, &cfg)`
(`0xfffffe0008b5c3e0`) — keyed on **DevID**, not ChipType. That is why
`gsc_saAVE_AXI2AF_Cfg_Castor_6001` exists even though t6001's ChipType is
`Nyx`.

---

## 4. `AVE_HwC` — bring-up order and the heartbeat

| symbol | VA |
|---|---|
| `AVE_HwC::StartUp` | `0xfffffe0008c1cd8c` |
| `AVE_HwC::StartUpIOP` | `0xfffffe0008c1d354` |
| `AVE_HwC::ShutDownIOP` | `0xfffffe0008c1ffac` |
| `AVE_HwC::MakeFwCfg` | `0xfffffe0008c1cbf0` |
| `AVE_HwC::CreateFwHeap` | `0xfffffe0008c1c7b4` |
| `AVE_HwC::CreateFwHeartBeatTimer` | `0xfffffe0008c1c05c` |
| `AVE_HwC::StartFwHeartBeatTimer` | `0xfffffe0008c1cabc` |
| `AVE_HwC::FwHeartBeatTimerHandler` | `0xfffffe0008c1c83c` |

### 4.1 The SVE scratch registers

Most of the boot handshake goes through eight scratch registers, not the
mailbox. `AVE_SVECtrl::ReadScratch(int idx, u32*)` (`0xfffffe0008c913c4`) /
`WriteScratch(int idx, u32)` (`0xfffffe0008c9127c`): take the per-SoC map at
`this+0x20`, reject `idx >= map[2]` with -1002, and do
`AVE_Reg::Read32/Write32(**regtype 2**, map[3 + idx])`.

`AVE_SVECtrl_GetReg(chipType)` indexes a 21-entry pointer table at
`0xfffffe0007ee0f80` by `chipType - 1`:

| chipType | map |
|---|---|
| 1–2 | `gsc_sAVE_SVECtrl_Reg_Hypnos` (`0xfffffe00072747a4`), 8 regs at `0x1C + 4i` |
| **3–12** | **`gsc_sAVE_SVECtrl_Reg_Rhea`** (`0xfffffe00072748b4`), **8 regs at `0x18 + 4i`** |
| 13–21 | `gsc_sAVE_SVECtrl_Reg_Themis` (`0xfffffe0007274694`), 0x40 regs at `0x18 + 4i` |

t6001 is chipType 7 → the **Rhea** map. Dumping that struct gives
`{12, 16, nScratch = 8, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34}`.

`regtype 2` is ADT `reg[2]` (§2.2), i.e. **`0x20D050000` size `0x8000`** for
`ave0` (`0x307050000` for `ave1`).

> This **identifies `reg[2]`**, which
> [12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md) and
> [01-hardware.md](01-hardware.md) both leave unresolved: it is the **SVE
> control block**, and its first 8 words from `+0x18` are the coprocessor
> scratch/handshake registers.

So on M1 Pro/Max:

| scratch | offset | `ave0` absolute | use |
|---:|---|---|---|
| 0 | `reg[2] + 0x18` | `0x20D050018` | halt acknowledge, magic `0x08042006` |
| 1 | `reg[2] + 0x1C` | `0x20D05001C` | `_S_AVE_Fw_Cfg` DART address, low 32 |
| 2 | `reg[2] + 0x20` | `0x20D050020` | `_S_AVE_Fw_Cfg` DART address, high 32 |
| 4 | `reg[2] + 0x28` | `0x20D050028` | `m_iFwTimeShift` low 32 |
| 5 | `reg[2] + 0x2C` | `0x20D05002C` | `m_iFwTimeShift` high 32 |
| **7** | **`reg[2] + 0x34`** | **`0x20D050034`** | **heartbeat counter** |

Registers 3 and 6 are unused by these paths.

### 4.2 `StartUpIOP` — the ordered sequence

Called only from `AVE_HwC::StartUp` (`0xfffffe0008c1cea0`), gated on state
`[this+0xC0] == 2` (`0xfffffe0008c1ce80`); state 3 = already up; otherwise
-1011 (`0xfffffe0008c1cfa0`).

| # | VA of the `bl` | step |
|---:|---|---|
| 1 | `0xfffffe0008c1d458` | `AVE_FwLog::Print(4, 6, 0, 0)` |
| 2 | `0xfffffe0008c1d468` | **`AVE_DPM::SetIOP(PL=4, 0)`** — power |
| 3 | `0xfffffe0008c1d474` | **`AVE_DPM::SetHw(PL=4)`** — power (→ `AVE_PMGR::SetPS`, `0xfffffe0008c720d4`) |
| 4 | `0xfffffe0008c1d47c` | **`AVE_FwImg::UpdateImage()`** — restores the CTRR data segment (§1.5) |
| 5 | `0xfffffe0008c1d50c` | construct `AVE_IPC` → `this+0xE0` (null ⇒ -1003) |
| 6 | `0xfffffe0008c1d528` | `AVE_IPC::Init(devInfo, surfaceMgr, id, sveCtrl, fwImg)` |
| 7 | `0xfffffe0008c1d690` | `AVE_IPC::Alloc(0x38, &pFwCfg)` (`mov w1,#0x38` at `0xfffffe0008c1d68c`) |
| 8 | `0xfffffe0008c1d978` | **`AVE_HwC::MakeFwCfg(pFwCfg)`** (§4.5) |
| 9 | `0xfffffe0008c1da64` | `AVE_SVECtrl::SetIOPFlag(0)` |
| 10 | `0xfffffe0008c1db50` | `AVE_IPC::Kernel2DARTAddr(pFwCfg)` |
| 11 | `0xfffffe0008c1dc7c` | `WriteScratch(1, lo32(dartAddr))` |
| 12 | `0xfffffe0008c1dd10` | `WriteScratch(2, hi32(dartAddr))` (`lsr x2,x21,#32` at `0xfffffe0008c1dd08`) |
| 13 | `0xfffffe0008c1ddfc` | **`AVE_IOP::Config()`** (§2.7 — a no-op on iBoot-loaded parts) |
| 14 | `0xfffffe0008c1dee8` | **`AVE_IOP::Start()`** — core released |
| 15 | `0xfffffe0008c1dfe4` | `AVE_SVECtrl::RecvIOPMsg(&m0,&m1,&m2,&m3)` — **wait 1**, boot message |
| 16 | `0xfffffe0008c1e208` | `AVE_IOP::GetCurrTime()`; `m_iFwTimeShift = host − fw` (`0xfffffe0008c1e21c`) |
| 17–18 | `0xfffffe0008c1e534`, `0xfffffe0008c1e5cc` | `WriteScratch(4/5, lo/hi(m_iFwTimeShift))` |
| 19 | `0xfffffe0008c1e6b8` | `AVE_DevInfo::GetDevArch()`; `== 0x40` selects the 64-bit layout (`0xfffffe0008c1e6bc`) |
| 20 | `0xfffffe0008c1e6e4` | `AVE_IPC::GetInfo(&addr, &size)` |
| 21 | `0xfffffe0008c1e784` | **`AVE_HwC::CreateFwHeap(m3)`** — skipped when `m3 == 0` (`0xfffffe0008c1e778`) |
| 22–25 | `0xfffffe0008c1ee34`…`0xfffffe0008c1f1f0` | `UpdateFwBaseAddr`, `AllocChannelMem(m1)`, `Alloc(0x50)` handshake, `Alloc(0x10000)` shared |
| 26–27 | `0xfffffe0008c1f76c` | fill the handshake block, `SendIOPMsg(lo32(fwAddr), hi32, 0, 0)` |
| 28 | `0xfffffe0008c1f818` | `RecvIOPMsg(&a,&b,&c,0)` — **wait 2**, channel info |
| 29–31 | `0xfffffe0008c1fa30`, `0xfffffe0008c1fb58` | `CreateChannel`, `Alloc(0xF0)` command buffer |
| 33 | `0xfffffe0008c1fcbc` | `AVE_SVECtrl::SetIOPFlag(3)` |
| 34 | `0xfffffe0008c1fcc8` | poll `CheckIOPFlag(3)` — **wait 3** |

Power comes **before** the image; `AVE_IOP::Config`/`Start` come **after** the
config-struct DART address is in scratch 1/2. `AVE_DART` is not touched here —
`AVE_DART::Init` runs once in `AVE_HwC::Init` (`0xfffffe0008c1a93c`). The
heartbeat is **not** started inside `StartUpIOP`; `AVE_HwC::StartUp` starts it
immediately after (`0xfffffe0008c1cfb4`), then `AVE_MCC::Enable`
(`0xfffffe0008c1d0ec`) and `SendFwCmd_Config` (`0xfffffe0008c1d0f4`).

Boot-message validation: `0 < m0 <= 3` (`0xfffffe0008c1e1f0`), assert
`0 < ipcChNum && ipcChNum <= AVE_IPC_Ch_Max` → **`AVE_IPC_Ch_Max = 3`**;
`m1 > 0` (`0xfffffe0008c1e1fc`).

Final poll bound: `IODelay(100)` per iteration (`mov w0,#0x64` at
`0xfffffe0008c1fd9c`), limit `cfg->iTimeOutCntFactor * 20000`
(`0xfffffe0008c1fd8c`) ⇒ **≈2 s at the default factor of 1**. Timeout returns
**-1009** (`0xfffffe0008c1ffa4`, `"starting IOP time out"`).

Other `StartUpIOP` error codes: -1000 (`0xfffffe0008c1ff58`), -1003
(`0xfffffe0008c1d7c8`), -1015 (`0xfffffe0008c1e43c`, bad channel count/size).
On any failure, `cfg->flags` bit 12 gates a panic
`"IOPBootup" @AVE_HwC.cpp:1229` (`0xfffffe0008c1d804`,
`0xfffffe0008cd508c`) — off by default.

### 4.3 `ShutDownIOP`

| # | VA | step |
|---:|---|---|
| 1 | `0xfffffe0008c2008c` | `AVE_DPM::SetHw(PL=4)` |
| 2 | `0xfffffe0008c20094` | **`AVE_HwC::SendFwCmd_Halt()`** — the halt command goes first |
| 3 | `0xfffffe0008c20154` | **`AVE_IOP::Stop()`** |
| 4 | `0xfffffe0008c20164` | poll `ReadScratch(0)` until it reads **`0x08042006`** |
| 5 | `0xfffffe0008c203a0` | destroy the FwHeap surface |
| 6–12 | `0xfffffe0008c203b4`…`0xfffffe0008c20420` | `CleanAllFwCmds`, `FreeAllIPCMem`, free cmd/shared buffers, `DestroyChannel`, `FreeChannelMem`, destroy `AVE_IPC` |

The halt-ack magic `0x08042006` is built at `0xfffffe0008c20118` /
`0xfffffe0008c2011c` (`mov w28,#0x2006 ; movk w28,#0x804,lsl #16`) and
compared at `0xfffffe0008c20170`. Poll: `IODelay(100)` per iteration
(`0xfffffe0008c20244`), limit `cfg->iTimeOutCntFactor * 10000`
(`0xfffffe0008c20234`) ⇒ **≈1 s**; timeout returns -1009
(`0xfffffe0008c20390`). If `SendFwCmd_Halt` fails it logs
`"failed to halt IOP"` and skips to teardown (`0xfffffe0008c20098`).

So the shutdown order is: **halt command → stop the core → wait for the ack
word → tear down IPC.** Note the ack is read *after* `AVE_IOP::Stop`, which
itself only polls `CPU_STATUS` (§2.5).

### 4.4 `CreateFwHeap`

**The heap size is not a constant in the kext** — it is the 4th word of the
IOP boot message, passed in at `0xfffffe0008c1e780`, and the call is skipped
entirely when it is zero (`0xfffffe0008c1e778`).

```
c7d8-c7e4:  size = roundup(size, PAGE_SIZE)     ; page size from a global
c7e8:       size += 0x3fff                      ; 0xfffffe0008c1c7e8
c7f0:       AVE_GetSurfaceCfg(30)               ; name "FwHeap", flags 0x10518
c818:       size &= 0xffffc000                  ; 0xfffffe0008c1c818  -> 16 KiB aligned
c828:       AVE_SurfaceMgr::CreateSurface(..., &this[0xA8])
```

It is DART-mapped: `StartUpIOP` hands the firmware
`m_psFwHeap->GetDARTAddr(instanceId, 0)` (`0xfffffe0008c1f3e4`) and
`GetSize()` (`0xfffffe0008c1f3f0`). Whether the allocation is physically
contiguous is **unknown** (it depends on `AVE_SurfaceMgr::CreateSurface`,
`0xfffffe0008c80e30`, not examined).

### 4.5 `MakeFwCfg` — `_S_AVE_Fw_Cfg`, 56 bytes

Size `0x38` from the allocation at `0xfffffe0008c1d68c`. No memset in
`MakeFwCfg` itself.

| offset | size | value | store VA |
|---:|---|---|---|
| `0x00` | u32 | instance id (`AVE_HwC+0x48`) | `0xfffffe0008c1cc18` |
| `0x04` | u32 | `AVE_DevInfo::GetDevID()` | `0xfffffe0008c1cc24` |
| `0x08` | u32 | `GetDevNum()` | `0xfffffe0008c1cc30` |
| `0x0C` | u32 | `GetDevNumPerGroup()` | `0xfffffe0008c1cc3c` |
| `0x10` | u64 | `GetDevSubIDFlag()` | `0xfffffe0008c1cc48` |
| `0x18` | u32 | `GetDevRevision()` | `0xfffffe0008c1cc54` |
| `0x1C` | u32 | never written (padding) | — |
| `0x20` | u64 | FwLog surface **DART address** (out-param of `AVE_FwLog::GetInfo`) | `0xfffffe0008c1cc70` |
| `0x28` | u32 | FwLog surface size | `0xfffffe0008c1cc70` |
| `0x2C` | u32 | never written (padding) | — |
| `0x30` | u32 | global cfg `+0x30` (`AVE_Cfg_Print`: `"DPM: 0x%x"`) | `0xfffffe0008c1cc60` |
| `0x34` | u32 | never written | — |

Confirmed field-for-field by the log format at `0xfffffe00072e89d1`:
`"... %p %d | %p %d || %d %d %d 0x%llx %d || %p %d || 0x%x"` with varargs
loaded at `0xfffffe0008c1cca8`–`0xfffffe0008c1ccc0`.

The struct's **DART** address is what reaches the firmware, split across
scratch 1 and 2 before `AVE_IOP::Config`/`Start` (§4.2 steps 10–12).

A second, 0x50-byte "handshake" block (`Alloc(0x50)` at
`0xfffffe0008c1f008`, zeroed at `0xfffffe0008c1f1c8`) is sent by
`SendIOPMsg` after the core is running. 64-bit layout
(`GetDevArch() == 0x40`):

| offset | value | VA |
|---:|---|---|
| `0x08` | fw address of the IPC channel memory | `0xfffffe0008c1f3d0` |
| `0x10` | fw address of the shared/notification memory | `0xfffffe0008c1f408` |
| `0x18` | shared memory size (`0x10000`) | `0xfffffe0008c1f410` |
| `0x1C` | FwHeap DART address (unaligned `stur`) | `0xfffffe0008c1f3e8` |
| `0x24` | FwHeap size | `0xfffffe0008c1f3f4` |
| `0x28` | `AVE_DevInfo::GetDevType()` | `0xfffffe0008c1f41c` |

### 4.6 The heartbeat — it is a host watchdog, not a firmware requirement

This is the answer that matters most, and it inverts the assumption recorded
in [06-kext.md](06-kext.md).

`CreateFwHeartBeatTimer` (`0xfffffe0008c1c05c`) builds an
**`IOTimerEventSource`** — `IOTimerEventSource::timerEventSource(owner,
action)` at `0xfffffe0008c1c098`, action = `FwHeartBeatTimerHandler`
(`0xfffffe0008c1c084`) — wrapped by `AVE_Timer_Create` into `this+0xD0`
(`0xfffffe0008c1c0ac`). It is created **unconditionally** in `AVE_HwC::Init`
(`0xfffffe0008c1bd38`) and its result is not checked.

**Interval.** Set in `StartFwHeartBeatTimer`, not in Create:

```
cae8:  ReadScratch(7, &v)
caf4:  ands w8, w8, #0xffffff      ; 0xfffffe0008c1caf4
caf8:  str  w8, [this+0xD8]        ; seed = v & 0x00FFFFFF
cb00:  w8 = cfg->[0x14]            ; iTimeOutTimeFactor
cb04:  w9  = 0x002dc6c0            ; 3,000,000
cb0c:  w10 = 0x005b8d80            ; 6,000,000
cb14:  csel w9, w10, w9, eq        ; eq == (seed == 0)
cb18:  mul  w21, w8, w9
cb30:  AVE_Timer_Start(timer, w21)
```

`AVE_Timer_Start` (`0xfffffe0008cabec0`) calls `IOTimerEventSource` vtable
slot `+0x1F8` (`0xfffffe0008cabf9c`), which is a thunk setting `w2 = 1000`
(`0xfffffe000bf3f374`) into slot `+0x200`, which computes `interval * scale`
**nanoseconds** (`0xfffffe000bf3f300`) — so the argument is **microseconds**
and slot `+0x1F8` is `setTimeoutUS` (**inferred** from the scale, the
neighbouring 10,000,000 / 1,000,000 thunks, and declaration order).

| when | period |
|---|---|
| first arm, scratch7 low 24 bits == 0 | **6000 ms** × `iTimeOutTimeFactor` (`0xfffffe0008c1cb0c`) |
| first arm, non-zero | **3000 ms** × factor (`0xfffffe0008c1cb04`) |
| every re-arm in the handler | **3000 ms** × factor (`0xfffffe0008c1c9cc`) |

`iTimeOutTimeFactor` is `_S_AVE_Cfg + 0x14`, named by `AVE_Cfg_Print`
(`"TimeOut Time Factor: %d"`, load at `0xfffffe0008b64d10`), and
`AVE_Cfg_Default` writes **1** into both `+0x14` and `+0x18`
(`movi v0.2s,#1 ; stur d0,[x0,#20]` at `0xfffffe0008b63844`). No boot-arg
writes `+0x14`.

**⇒ default heartbeat period is 3 seconds (first tick possibly 6 s).**

**What the tick does** (`FwHeartBeatTimerHandler`, `0xfffffe0008c1c83c`):

```
c85c:  x8 = cfg->[0x28]; tbz w8,#8 -> return   ; gated on flags bit 8
c878:  AVE_SVECtrl::ReadScratch(7, &v)          ; the ONLY device access
c8f4:  w23 = v >> 24                            ; status byte
c8fc:  w22 = v & 0x00ffffff                     ; 24-bit counter
c900:  ok = (w23 == 0) && (w22 != this[0xD8])
```

**It reads scratch register 7 and nothing else.** No register write, no
`SendFwCmd_*`, no shared-memory poke. On the OK path it re-arms
(`0xfffffe0008c1c9d8`), records history cmd 24 (`0xfffffe0008c1ca08`) and
stores the new counter (`0xfffffe0008c1ca9c`).

**On failure** (`0xfffffe0008c1ca44` on): error log
`"########## AVE_TIMER %p %d | 0x%x %d ##########"` (`0xfffffe00072ea82e`),
history cmd 25, `AVE_HwC::PrintAll` (`0xfffffe0008c1ca8c`), then
`cfg->flags` **bit 9** gates a panic `"AVE_TIMER" @AVE_HwC.cpp:2401`
(`0xfffffe0008c1ca90` → `0xfffffe0008cd5060`). **The timer is not re-armed on
this path** — `AVE_Timer_Start` appears only on the OK path.

**Flag defaults.** `cfg + 0x28` is the 64-bit "Configuration" word
(`AVE_Cfg_Print`: `"Configuration: 0x%llx"`, `0xfffffe0008b647ec`), settable
by the `ave-flag` boot-arg (`0xfffffe0008b63db0`).
`AVE_Cfg_DefaultVolatile` assembles `0x100 | 0x400 | 0x10000 | 0x20000 |
0x40000` = **`0x70500`** (`0xfffffe0008b63774`–`0xfffffe0008b637a4`). So:

| bit | mask | meaning | default |
|---:|---|---|---|
| 8 | `0x100` | run the heartbeat check | **on** |
| 9 | `0x200` | panic when it fails | off |
| 12 | `0x1000` | panic when IOP bring-up fails | off |

> **The firmware does not need the heartbeat.** The host writes nothing to the
> device on a tick; it only reads a counter the firmware increments in
> scratch 7. A Linux driver that omits it loses "firmware is wedged"
> detection and nothing else. This contradicts the guess in
> [06-kext.md](06-kext.md) that "a driver that omits it will probably see the
> firmware fault" — that is **not** what the code does.
>
> Conversely, scratch 7 is a free, cheap liveness probe worth implementing:
> read `reg[2] + 0x34`, require the top byte to be 0 and the low 24 bits to
> have changed.

---

## 5. What a Linux driver has to do

Each step is read out of the sections above.

1. **Do not load a firmware file.** Under Asahi, iBoot has already placed the
   image, exactly as for DCP/ISP/SIO. m1n1's `dt_reserve_asc_firmware()`
   (`m1n1-src/src/kboot.c:1781`) already turns a node's `segment-ranges` into
   `reserved-memory` nodes with `compatible = "apple,asc-mem"` plus
   `iommu-addresses`, and it is already called for `dcpext`
   (`kboot.c:1868`), `sio` (`kboot.c:2239`) and `isp` (`kboot.c:2876`) — but
   **not for `ave0`/`ave1`**. Adding those two calls is a small, self-contained
   m1n1 patch and is the natural first piece of upstreamable work, alongside
   teaching `asahi-fwextract` about AVE (docs/02-firmware.md).
2. The DART mapping must land at **IOVA 0**, text first, data immediately
   after (§1.4). The DART is `dart,t6000` with 16 KB pages
   (`data/derived/adt-ave-full.txt`), already supported by `apple-dart`.
   Note ISP passes `remap = false` to `dt_reserve_asc_firmware` and DCP/SIO
   pass `true`; which AVE needs is **unknown** and wants a live ADT dump —
   the kext reads neither field, so the kext cannot answer it.
3. Keep a pristine copy of the data segment and restore it before **every**
   start (§1.5).
4. Write the ASC `CPU_CONTROL` at `reg[1] + 0x400044`: `0` then `0x10`, with
   the two auxiliary writes at `+0x400808` = 1 and `+0x400400` = `0x10000`
   first (§2.3). Do **not** expect a ready poll here.
5. Order the bring-up as in §4.2: power (`AVE_PMGR`) → restore the firmware
   data segment → allocate the 56-byte `_S_AVE_Fw_Cfg` → write its DART
   address into scratch 1/2 (`reg[2] + 0x1C` / `+0x20`) → start the core →
   read the 4-word boot message → allocate the firmware heap it asks for →
   send the 0x50-byte handshake block → set IOP flag 3 and wait.
6. To stop, send the `Halt` command first, then stop the core, then wait for
   scratch 0 (`reg[2] + 0x18`) to read **`0x08042006`** (§4.3). Stopping the
   core writes no register — poll `reg[1] + 0x400048` until `(v & 3) == 0`
   three times in a row, 50 µs apart (§2.5).
7. The heartbeat is optional (§4.6). If you want liveness detection, poll
   `reg[2] + 0x34` every 3 s: top byte must be 0, low 24 bits must change.
8. Pick the variant by the rule in §3.3, not by the `H13C` guess.

## Open

- The AVE-specific meaning of `CPU_STATUS` bits 0/1, and of the two
  auxiliary Start writes (`+0x808`, `+0x400`).
- The `0x0102` high bits in the `Config` 64-bit write.
- Which `AppleAVE2FW_*.im4p` is `Nyx` (t6001) and which is `Castor` (t6000) —
  mechanical, needs the other variants fetched.
- Whether an M1 Max really needs two different firmware images (one per
  instance).
- Whether AVE wants `iova` or `remap` from `segment-ranges` (§5 step 2) —
  the kext reads neither, so only a live ADT can answer it.
- Where `iovmMapMemory` is undone on teardown.
- The blocking behaviour and timeout of `AVE_SVECtrl::RecvIOPMsg`
  (`0xfffffe0008c9161c`), which is what both boot-time waits actually block
  in (§4.2 waits 1 and 2). Not disassembled.
- Whether the FwHeap surface is physically contiguous, and the meaning of the
  surface flags (`0x10518` for `FwHeap`, `0x10d18` for `FwImage`) — needs
  `AVE_SurfaceMgr::CreateSurface` (`0xfffffe0008c80e30`).
- The contents of the 4-word IOP boot message beyond the three fields the
  kext validates (`ipcChNum`, `ipcChBufSize`, fw heap size).
