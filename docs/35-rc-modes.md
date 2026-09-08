# `_E_AVE_RCMode` and `_E_AVE_EncMode` — which value is constant QP

*Roadmap open question #2. The answer is **`RCMode = 3`**, and it is now
**confirmed**, not inferred: the firmware's rate-controller factory compares
the field against `3` and installs the `ConstantQpRateControl` vtable, and that
vtable's `processRateControl` slot is the function that reads the three fixed
QPs. Every step from `sCAveCmdAvcStart + 0x234` to that comparison is a read
instruction with a cited VA.*

Every row is marked **confirmed** (read out of an instruction, VA cited),
**inferred** (a chain of reasoning over confirmed facts) or **unknown**, per
[00-methodology.md](00-methodology.md).

Firmware VAs are **image virtual addresses** (`__TEXT` vmaddr 0, fileoff
`0x4000`). String addresses from `strings -t x data/blobs/ave_h13c.bin` are
**file offsets** and equal `VA + 0x4000`; both are given where a string is
cited. Kext VAs are kernelcache `__TEXT_EXEC` addresses.

This supersedes [20-command-structs.md](20-command-structs.md) §3.3's closing
paragraph ("**inferred**, on the strength of the shared `+12` offset") and
corrects [18-coded-data-sizing.md](18-coded-data-sizing.md) §2, which states
`Check_RCMode` "accepts only `20` and `100`" — the polarity is the other way
round (doc 20 already got this right; doc 18 was never updated).

---

## 0. The answer

| Question | Answer | Status |
|---|---|---|
| Which `_E_AVE_RCMode` selects constant/fixed QP? | **`3`** | **confirmed** |
| Which selects CBR? | **`2`** | **confirmed** |
| Which selects constant rate factor (CRF)? | **`6` or `8`** | **confirmed** |
| Is `0` valid? | No — `AVE_RCMode_None`, rejected by the factory | confirmed (rejection) / inferred (the name) |
| Full enumerator *names* | not recoverable | see §6 |

For a first fixed-QP encode, write `3` to `sCAveCmdAvcStart + 0x234` and put
the QP in `+0x240` (I), `+0x244` (P), `+0x248` (B).

**One extra requirement, not previously recorded:** the constant-QP controller
described here only gets built when **`sRC.Feature` bit 31 is set**
(`sCAveCmdAvcStart + 0x228`, bit 31 of the u64 — i.e. byte `+0x22B` bit 7).
With that bit clear the firmware runs the *legacy* `CRateControl` instead. See
§4. Mode `3` means constant QP in both controllers, so the encode works either
way, but only the bit-31 path reaches `ConstantQpRateControl`.

---

## 1. The dispatch, read end to end

### 1.1 `RateControl::CreateInstance` — the factory  (fw `0x60c4`)

Signature: `RateControl::CreateInstance(RateControlParameters const&,
FrameStats*, unsigned, void*)`. `x21` is the `RateControlParameters&`.

```
 6100:  ldr   w8, [x21, #12]        ; params.RCMode
 6104:  cmp   w8, #0x8
 6108:  b.hi  0x6280                ; > 8  -> "unsupported RC mode", return NULL
 610c:  mov   w9, #0x1
 6110:  lsl   w9, w9, w8            ; w9 = 1 << RCMode
 6114:  mov   w10, #0xa6            ; 0b1010_0110 -> modes {1,2,5,7}
 6118:  tst   w9, w10
 611c:  b.eq  0x61ac
 6120:  ...                         ; -> plain RateControl (base class)
 61ac:  mov   w10, #0x140           ; 0b1_0100_0000 -> modes {6,8}
 61b0:  tst   w9, w10
 61b4:  b.eq  0x6200
 61b8:  ...                         ; -> ConstantRateFactorRateControl
 61d8:  adrp  x16, 0x135000
 61dc:  add   x16, x16, #0x988      ; __ZTV29ConstantRateFactorRateControl
 61e0:  add   x16, x16, #0x10       ;   + 0x10 = first virtual slot
 6200:  cmp   w8, #0x3              ; <<-- CONSTANT QP
 6204:  b.ne  0x6280                ;      anything else -> "unsupported RC mode"
 6208:  ...
 6224:  adrp  x16, 0x135000
 6228:  add   x16, x16, #0x938      ; __ZTV21ConstantQpRateControl
 622c:  add   x16, x16, #0x10
 623c:  str   x16, [x19]            ;      installed as the vptr
```

**Confirmed.** `0x135938` is `__ZTV21ConstantQpRateControl` and `0x135988` is
`__ZTV29ConstantRateFactorRateControl` in the firmware's own symbol table.

### 1.2 The vtable is the right one — checked against its contents

Decoding `__DATA` at `0x135938` (the low 32 bits of each slot are the target
VA; the high bits are the PAC discriminator, so **Trap 5 does not apply here** —
this is `MH_PRELOAD`, not a chained-fixup image):

| slot | `ConstantQp` | `ConstantRateFactor` | PAC discriminator | symbol |
|---|---|---|---|---|
| `+0x10` | `0xa3d0` | `0xa4cc` | `0x11b4bd` | `~...` (D1) |
| `+0x18` | `0xa444` | `0xa540` | `0x11fbef` | `~...` (D0) |
| **`+0x20`** | **`0x8bd4`** | `0x8010` | `0x116520` | **`processRateControl`** |
| `+0x28` | `0xa4b8` | `0xa39c` | `0x114977` | |
| `+0x30` | `0xa4bc` | `0x9038` | `0x119bd9` | |
| `+0x38` | `0xa4c0` | `0x8c9c` | `0x1188e7` | |
| `+0x40` | `0x6b1c` | `0xa260` | `0x11956d` | `initRQModel` |

**Confirmed** — the vtable selected by `cmp w8,#3` is exactly the one whose
`processRateControl` is the QP-reading function at `0x8bd4`.

Two things make this stronger than a single slot read. First, **the PAC
discriminators agree slot-for-slot across the two vtables** while the targets
differ: these are parallel vtables of sibling classes, so `+0x20` is
demonstrably the *same virtual method* in both, and mode 3 gets the
constant-QP override where the CRF class gets the generic
`RateControl::processRateControl` (`0x8010`). Second, the destructor pair is
self-checking: `0xa3d0` runs to `0xa440` and the next slot is `0xa444`.

**Slot values are plain VAs — do not subtract `0x4000`.** This is worth
stating because it is a genuine near-miss with
[Trap 5](00-methodology.md): this image is `MH_PRELOAD` with `__DATA`
at vm `0x134000` / file `0x138000`, so the VA→file delta is `0x4000`
*uniformly*, and a low-32 value therefore looks equally plausible read either
way. The discriminator is that `0xa3d0` is a function entry (`adrp x16,
0x135000` installing the base vptr) whereas `0x63d0` lands mid-function on an
`add` with no preceding `adrp`. An earlier draft of this table had `0x63d0`
and `0x6444` in the first two rows — corrected here against the bytes.

`ConstantQpRateControl::processRateControl` (`0x8bd4`) does no rate
computation at all — it picks one of three stored QPs by slice type and adds
an offset:

```
 8c18:  ldr   w9, [x19, #36]        ; frame slice type
 8c1c:  mov   w10, #0x50            ; default        -> RateControl+0x50 (QP B)
 8c20:  mov   w11, #0x4c            ; type == 0      -> RateControl+0x4C (QP P)
 8c28:  csel  x10, x11, x10, eq
 8c30:  mov   w9,  #0x48            ; type == 2      -> RateControl+0x48 (QP I)
 8c34:  csel  x9,  x9,  x10, eq
 8c38:  ldr   w9, [x0, x9]
 8c48:  str   w8, [x19, #96]        ; = QP + delta, returned unchanged
```

**Confirmed.**

### 1.3 `params + 0xC` is called `RCMode` by the firmware itself

`printRateControlParams(RateControlParameters const&)` (fw `0x5cd4`) is called
on entry to `CreateInstance` (`bl 0x5cd4` at `0x60ec`). Its first field print:

```
 5dbc:  ldr   w8, [x19, #12]
 5dc0:  adrp  x2, 0x11e000
 5dc4:  add   x2, x2, #0x86c        ; "RCMode %d"   (VA 0x11e86c, file 0x12286c)
 5dd0:  str   x8, [sp]
 5dd4:  bl    0xa948                ; AVE_Log
```

**Confirmed.** The error path at `0x6280` prints
`"%s::%s:%d unsupported RC mode %p %p %d %p %d"` (VA `0x11ea06`, file
`0x122a06`) with `[x21,#12]` reloaded at `0x6290` as the last argument.

The full field map of `RateControlParameters` recovered from
`printRateControlParams` (all **confirmed**):

| params off | printed as | fed from `_S_AVE_RC_Cfg` off |
|---:|---|---:|
| `0x0C` | `RCMode %d` | `0x0C` |
| `0x18` | `bitrate %d` | `0x10` |
| `0x24` | `fps %d.%03d` | `sComm.FrameRate` (`AlgCfg+0x18`) |
| `0x28` | `totalFrames %d` | (the `int` arg to `CreateRateControl`) |
| `0x40`/`0x44`/`0x48` | `InitialQp / InitialQpp / InitialQpb` | `0x18`/`0x1C`/`0x20` |
| `0x50` | `iRCFeature 0x%llx` | `0x00` |
| `0x60` | `fCRFScale %d.%03d` | `0x50` |
| `0x68` | `lookahead_frames %d` | `0x08` |

---

## 2. The chain from the wire command

Every hop below is a read instruction.

```
host: _S_AVE_Client + 5892                       ("RCMode: %d" in AVE_Client_Enc_Print)
        = client AlgCfg(+0x16D8) + sRC(0x20) + 0x0C
  |
  v  (host builds sCAveCmdAvcStart; AlgCfg lands at cmd + 0x208)
wire: sCAveCmdAvcStart + 0x234  = AlgCfg+0x20 (sRC) + 0x0C  = RCMode
  |
  |  fw  CFlowControllerBase::ProcessCmd_Start
  |      32684:  ldr  w0, [x20, #564]        ; 564 = 0x234
  |      32688:  bl   0xf830                 ; CRateControl::Check_RCMode
  v
  |  fw  CAVCController::InitEncodingParameters  (x24 = [arg+8] = the command)
  |      6b938:  add  x9, x20, #0x1b, lsl #12
  |      6b944:  add  x0, x9, #0xf60         ; dst = controller + 0x1BF60
  |      6b948:  add  x1, x24, #0x1a0        ; src = cmd's _S_AVE_Alg_Cfg
  |      6b94c:  mov  w2, #0x110             ; 272 bytes = sizeof _S_AVE_Alg_Cfg
  |      6b954:  bl   0x581c                 ; memcpy
  v
controller + 0x1BF60 = _S_AVE_Alg_Cfg copy
controller + 0x1BF80 = .sRC          (+0x20)
controller + 0x1BF8C = .sRC.RCMode   (+0x2C)
  |
  |  fw  CAVECommonController::CreateRateControl   (x21 = this + 0x1BF78)
  |      7ab54:  ldp  w22, w8, [x21, #20]    ; w22 = sRC+0x0C = RCMode
  |      7ac08:  str  w22, [sp, #76]         ; params + 0x0C   (sp+0x40 is params)
  |      7af58:  add  x0, sp, #0x40
  |      7af5c:  bl   0x60c4                 ; RateControl::CreateInstance
  v
CreateInstance: cmp w8, #3  ->  ConstantQpRateControl
```

**Every line above is confirmed.** The one identification that is a *structural*
argument rather than a single instruction — that the 272-byte block copied at
`0x6b934`–`0x6b954` is `_S_AVE_Alg_Cfg` — is pinned by **seven** independent
field agreements between the firmware's `printRateControlParams` names and the
kext's `AVE_RC_PrintCfg` / `AVE_Alg_PrintCfg` names ([20](20-command-structs.md)
§3.3):

| offset in the copied block | firmware use | host name at the matching wire offset |
|---:|---|---|
| `+0x18` | `ldr s0,[x21]` at `0x7ab70`, `scvtf` -> `fps` | `sComm.FrameRate` (`cmd+0x220`) |
| `+0x20` | `ldr x10,[x21,#8]` -> `iRCFeature` | `sRC.Feature` (`cmd+0x228`) |
| `+0x28` | `ldr w8,[x21,#16]` -> `lookahead_frames`, clamped `<= 20` at `0x7ad0c` and used to size the lookahead queue | `sRC.LookAheadFrameCount` (`cmd+0x230`) |
| `+0x2C` | `ldp w22,..,[x21,#20]` -> `RCMode` | `sRC.RCMode` (`cmd+0x234`) |
| `+0x30` | `ldp ..,w8,[x21,#20]` -> `bitrate` | `sRC.Bitrate` (`cmd+0x238`) |
| `+0x38`/`+0x3C` | `ldp w9,w8,[x21,#32]` -> `InitialQp`/`InitialQpp` | `sRC.QP[0]`/`QP[1]` (`cmd+0x240`/`0x244`) |
| `+0x70` | `ldr d8,[x21,#88]` -> `fCRFScale` | `sRC.CRFScale` |

Seven fields, in order, at the exact host-side offsets. Also **confirmed** on
the host side: `_S_AVE_Client + 5892 = 0x1704 = 0x16D8 (AlgCfg) + 0x20 (sRC)
+ 0x0C`, the same arithmetic ([18](18-coded-data-sizing.md) §2).

### `sRC.RCMode` is the *same* field the flow controller validates

`CAVCController::InitEncodingParameters` reads `[x25/x24, #1212]` and passes it
to the identical `Check_RCMode` leaf that `ProcessCmd_Start` calls with the raw
wire value:

```
 6b950:  add  x25, x8, #0xad0        ; x25 = this + 0x1BAD0
 6c838:  str  x25, [sp, #104]        ; spilled ...
 6ced0:  ldr  x24, [sp, #104]        ; ... and reloaded into x24
 6d354:  ldr  w0, [x24, #1212]       ; 0x1BAD0 + 0x4BC = 0x1BF8C  = sRC.RCMode
 6d360:  bl   0xf830                 ; CRateControl::Check_RCMode
```

**Confirmed** (`0x1BAD0 + 1212 = 0x1BF8C`, and `0x1BF8C - 0x1BF60 = 0x2C`,
which is `AlgCfg + sRC(0x20) + RCMode(0x0C)`).

---

## 3. Independent corroboration for `3` and for `2`

Three separate places, none of which depends on the factory:

1. **CBR is `2`, by name.** `RateControl::insertCBRFillerAVC(_S_AVE_FillerInfo*,
   _E_AVE_RCMode, unsigned)` (fw `0x95a8`) does nothing unless the mode is 2:

   ```
    95bc:  mov  w21, w1               ; w1 = _E_AVE_RCMode
    95d4:  cmp  w21, #0x2
    95d8:  b.ne 0x9604                ; -> return, no filler emitted
   ```

   Same gate in `insertCBRFillerHEVC` (`0x964c`) and in
   `CRateControl::AVE_CBR_InsertFiller(..., _E_AVE_RCMode, ...)` (`0xf4b8`,
   `cmp w22, #0x2`). Bitrate-conformance filler data is a CBR-only artefact, and
   the function names say so. **Confirmed: `_E_AVE_RCMode == 2` is CBR.**
   This also proves the enum is a small dense enum, not a sparse one.

2. **The legacy controller treats `3` as fixed QP too.**
   `CRateControl::ProcessRateControl` (`0xc5cc`) short-circuits on mode 3 and
   returns a stored QP with no rate computation:

   ```
    ce48:  ldr  w8, [x19, #288]       ; CRateControl's copy of RCMode
    ce4c:  cmp  w8, #0x3
    ce50:  b.ne 0xc8f4
    ce5c:  ldr  w27, [x19, #448]      ; the stored QP
    ce60:  str  w27, [x19, #480]
    ce68:  mov  w0, w27               ; returned unchanged
   ```

   `CRateControl::ProcessAccumulate` likewise skips the model update for modes
   3 and 100 (`0xe604`/`0xe60c`). **Confirmed.**

3. **The host's coded-buffer size depends on the initial QP only when
   `RCMode == 3`.** `AVE_CalcBufSizeOfCodedData` (kext
   `0xfffffe0008b5f58c`):

   ```
   fffffe0008b5fa94:  ldr  x8, [sp, #136]      ; RCMode (stack arg 12)
   fffffe0008b5fa98:  cmp  w8, #0x3
   fffffe0008b5fa9c:  b.ne 0xfffffe0008b5fad0
   fffffe0008b5faac:  mov  w8, #0x66           ; 102 - initialQPI ...
   fffffe0008b5fab0:  sub  w8, w8, w9          ; ... drives the size estimate
   ```

   A bitstream-size bound computable from the QP alone is only meaningful when
   the QP is fixed. **Confirmed instruction**; the reading of *why* is
   **inferred**, but it agrees.

---

## 4. Two rate controllers — `sRC.Feature` bit 31 picks which

`CAVCController::InitEncodingParameters` (fw `0x6b8b0`), at `0x6d350`:

```
 6d354:  ldr  w0, [x24, #1212]        ; sRC.RCMode
 6d360:  bl   0xf830                  ; Check_RCMode
 6d364:  cbnz w0, 0x6d6ac             ; mode 20 or 100 -> build no RC at all
 6d368:  ldrb w8, [x24, #1203]        ; 0x1BAD0+0x4B3 = 0x1BF83 = sRC.Feature byte 3
 6d36c:  tbnz w8, #7, 0x6d3c0         ; Feature bit 31 set -> new framework
 6d370:  ...
 6d394:  bl   0xb95c                  ; CRateControl::CRateControl  (legacy)
 6d3c0:  ldr  x8, [x20, #432]
 6d3c8:  ldr  w1, [x22, #48]
 6d3d0:  bl   0x7aab8                 ; CAVECommonController::CreateRateControl
```

**Confirmed.** `CHEVCController::InitEncodingParameters` has the identical
sequence (the same 272-byte AlgCfg copy at `0x9cc6c`–`0x9cc78`).

So there are three domains, and they are consistent rather than contradictory:

| path | reached when | modes it understands |
|---|---|---|
| `RateControl::CreateInstance` (new) | `Feature` bit 31 set, and `Check_RCMode` passes | `1,2,3,5,6,7,8`; `0` and `4` -> "unsupported RC mode" |
| `CRateControl` (legacy) | `Feature` bit 31 clear | `3,4,100` branched on explicitly; `2` at `0xc54c` |
| neither | `RCMode` is 20 or 100 | `Check_RCMode` gates both out at `0x6d364` |

### `Check_RCMode` is a *rejection* list

```
 f830:  cmp   w0, #0x14              ; 20
 f834:  mov   w8, #0x64             ; 100
 f838:  ccmp  w0, w8, #0x4, ne
 f83c:  mov   w8, #0xfffffc16       ; -1002
 f840:  csel  w0, wzr, w8, ne       ; NE (neither 20 nor 100) -> 0 = OK
 f844:  ret
```

`csel Wd,Wn,Wm,cond` yields `Wn` when the condition holds, so `ne` -> `wzr` ->
0 -> success. **Returns `-1002` for 20 and 100, `0` for everything else.**
**Confirmed.** ([18](18-coded-data-sizing.md) §2 has this backwards.)

Note the symbol is called from ~50 sites, many in slice-header code, with a
plain `w0` argument — it is a leaf predicate, not a member function, whatever
the mangled name implies. The call at `0x32684`–`0x32688` passes
`[x20, #564]` = `cmd + 0x234` directly, which is what anchors it to `RCMode`.

---

## 5. Enumerator table

| value | what the firmware does with it | status |
|---:|---|---|
| `0` | `RateControl::CreateInstance` -> "unsupported RC mode". The kext has the assertion string `"pInfo->sSessionCfg.sEnc.sAlgCfg.sRC.eRCMode != AVE_RCMode_None"` (`AppleAVE2.macho` file `0x54efd`, `kc.macho` file `0x286067`), so `0` = `AVE_RCMode_None` follows Apple's own `_None = 0` convention (as `_E_AVE_EncType` does). | rejection **confirmed**; name **inferred** |
| `1` | base `RateControl` (bitrate/VBV/ABR machinery) | **confirmed** grouping; name **unknown** |
| **`2`** | **CBR** — the only mode for which `insertCBRFiller*` emits filler | **confirmed** |
| **`3`** | **Constant QP** — `ConstantQpRateControl` vtable; legacy `CRateControl` returns a fixed QP | **confirmed** |
| `4` | not accepted by `CreateInstance`; branched on by `CAVERefManager::Init` (`0x20ee4`) and compared in the kext | **confirmed** it exists; meaning **unknown** |
| `5` | base `RateControl` | **confirmed** grouping; name **unknown** |
| `6` | `ConstantRateFactorRateControl` (CRF) | **confirmed** |
| `7` | base `RateControl` | **confirmed** grouping; name **unknown** |
| `8` | `ConstantRateFactorRateControl` (CRF) | **confirmed** |
| `20` | rejected by `Check_RCMode`, so no RC object is built; heavily special-cased in the kext (`AVE_PSGen::Init`, `AVE_MD_SVE::*`, `AVE_CHM_SetDataInfo_FwBuf`, ...) | **confirmed** it exists; meaning **unknown** |
| `100` | rejected by `Check_RCMode`; in legacy `CRateControl` it skips the model update alongside mode 3 (`0xe60c`) and is branched on in `ProcessInit` (`0xc160`), `ProcessRateControl` (`0xc8f4`) and `CAVERefManager::Init` (`0x20edc`) | **confirmed** it exists; meaning **unknown** |

Values seen compared against the field, by binary: firmware `{0..8, 20, 100}`;
kext `{1, 2, 3, 4, 7, 20}` (scan of `ldr wN,[xM,#5892]` followed by a `cmp`).
`5`, `6` and `8` appear only in the firmware factory's bitmasks; `100` only in
the firmware.

### One more precondition on the new framework

`RateControl::validate` (fw `0x6380`) and the entry check of `CreateInstance`
(`0x60f0`–`0x60fc`) both require `(params[0x58] & ~2) == 8`, i.e. that field
must be 8 or 10, otherwise `CreateInstance` logs `"invalid parameter"`
(VA `0x11e9cf`) and returns `NULL`. `params+0x58` is loaded in
`CreateRateControl` at `0x7abe0` from `controller + 0x25DB` (a byte), **not**
from the wire command, so it is not something the host sets directly.
**Confirmed as a guard; its meaning is unknown.** Flagged here only because a
`NULL` return from `CreateRateControl` yields `-1001` and would look like a
rate-control configuration failure.

---

## 6. `_E_AVE_EncMode`

The type exists **only in the kext** (`AppleAVE2`); the firmware's symbol table
has no `_E_AVE_EncMode`. What is recoverable:

* **It has exactly three enumerators: `0`, `1`, `2`.** `AVE_EncMode_Max == 3`,
  read from the bounds assertion in
  `AVE_Analytics_CollectEncMode(_S_AVE_Analytics*, _E_AVE_EncMode)`
  (`0xfffffe0008b56790`):

  ```
  fffffe0008b567b0:  cmp   w1, #0x3
  fffffe0008b567b4:  b.ge  0xfffffe0008b567d4     ; -> "eEncMode < AVE_EncMode_Max"
  ```

  The assertion string is at `kc.macho` file `0x27a604`, referenced from
  `0xfffffe0008b56814` (`adrp x8, 0xfffffe000727e000; add x8, x8, #0x604`).
  **Confirmed.**

* **Names: not recoverable.** There is no enumerator-name array (see §7).

* **What selects AVC vs HEVC is `_E_AVE_EncType`, not `EncMode`**: `0` = none,
  `1` = AVC, `2` = HEVC, `3` = AV1 — already **confirmed** in
  [18](18-coded-data-sizing.md) §2 from the kext's own name table
  (`0xfffffe000c69a8f8`, pool at `kc.macho` file `0x289745`). Independently
  re-checked here: `_S_AVE_Client + 15532` is the field compared against `1`,
  `2` and `3` throughout (`AVE_CHM_SetDataInfo_FwBuf` `0xb733cc` compares 2 and
  3; `AVE_Client_Verify` `0xb9127c` compares 2 and 1).

* **Open discrepancy, flagged rather than resolved.** [18](18-coded-data-sizing.md)
  §2 identifies argument 11 of `AVE_CalcBufSizeOfCodedData` (`_E_AVE_EncMode`)
  as coming from `_S_AVE_Client + 15512` (`0xfffffe0008ca9b6c`). But
  `+15512` is also used as a codec-ish selector — `AVE_PSGen::GetVPSID`
  (`0xfffffe0008c3a204`) gates on `== 2`, and VPS is HEVC-only — and it is
  compared against `4` at `0xfffffe0008c18f5c`, which cannot be a value of a
  3-enumerator enum. Either `+15512` is not `EncMode`, or one of those two
  reads is of a different register. **Not resolved. Do not build on
  `client+15512` without re-deriving it.**

`_E_AVE_EncMode` is **not on the critical path to a first encode** — it is not
a field of `sCAveCmdAvcStart`; it appears only as a parameter to the host's
buffer-sizing functions, where [18](18-coded-data-sizing.md) §5 already gives a
size that is valid for `encMode != 2`.

---

## 7. What I checked and ruled out

Negative results, so nobody repeats them. Per Trap 2, each test below was one
that *would* have returned something for a case that differs — the
`_E_AVE_EncType` name table was found by exactly these methods, which is the
control.

* **Enumerator-name string arrays for `RCMode`/`EncMode`: do not exist.**
  `strings -t x` over both `AppleAVE2.macho` and `ave_h13c.bin` for `CBR`,
  `VBR`, `ABR`, `CQP`, `CRF`, `ConstantQ`, `RCMode_`, `EncMode`,
  `RateControlMode`. The only enumerator name anywhere is `AVE_RCMode_None`,
  inside an assertion string. The string pool that holds the `_E_AVE_EncType`
  names (`kc.macho` `0x289745`: `"AVC\0HEVC\0AV1"`) was dumped in full for
  `0x2896a0`–`0x2897e0` and contains name pools for `_E_ChromaFmt`, surface
  layout, work type and enc type — and **no** RC-mode pool. The nearby
  `"EncMode"` string (`kc.macho` `0x27af38`) is an *analytics key name*, one
  entry in an alphabetised list of ~25 keys (`ChromaFormat`, `ClientType`,
  `CodecType`, `EncMode`, `GOPMode`, ...), not an enumerator table.
* **`AVE_RC_DecideMode(int,int)`** (kext `0xfffffe0008bfd9bc`) sounds like the
  host-side mapping. It is not: the whole 0x168-byte body is two `AVE_Log`
  calls printing `"RCMode: %d"` on entry and exit, and it returns `0`
  unconditionally (`0xfffffe0008bfdb0c`). Disassembled in full.
* **No write to `_S_AVE_Client + 5892` exists in `AppleAVE2`.** Scanned the
  whole kext text (`0xfffffe0008b34bb0` + `0x1a1000`) for `str`/`strb`/`stp`
  with offset `#5892`: zero hits, 34 loads (the field's address is instead
  formed as `client + 0x16F8` and handed to `AVE_RC_PrintCfg`-shaped
  callees — `add xN, xM, #0x6f8` at `0xbdf894`, `0xbf33a4`, `0xc893f4`,
  `0xcd1008`, ...). So there is **no host-side constant to read** —
  **confirmed**. That the value therefore arrives from user space in
  `AVE_SessionSettings_UserKernel_Data` (validated in
  `AVE_Client_CheckCommonInfo` `0xb8fc14` and `AVE_Client_Verify`) is
  **inferred**. Either way, the answer had to come from the firmware.
* **Command-ordering / `__text`-layout inference: not used anywhere here**
  (Trap: the wire command ids were once wrong for exactly that reason). Every
  claim above is a comparison, a store, or a vtable entry.
* **`Check_RCMode` is not identical-code-folded onto some other function.** It
  has exactly one symbol at `0xf830` in the raw `LC_SYMTAB` (checked with
  `nm`), and the call at `0x32684`–`0x32688` reaches it with
  `cmd + 0x234` in `w0`, which is independently known to be `RCMode` from the
  kext's `AVE_RC_PrintCfg`. Its many call sites in slice-header code are
  therefore genuine uses of the same predicate, not evidence of folding.
* **`RateControlParameters` is not a remapped enum.** Checked explicitly
  because the `{0..8}` domain of `CreateInstance` and the `{20,100}` domain of
  `Check_RCMode` look contradictory. They are not: `Check_RCMode` gates
  `CreateRateControl` out entirely at `0x6d364`, so 20 and 100 never reach the
  factory. `w22` is moved from `[x21,#20]` to `[sp,#76]` with no arithmetic
  (`0x7ab54` -> `0x7ac08`), so no remap happens.
* **Firmware `__DATA_CONST` jump tables**: `CreateInstance` uses a `1 << mode`
  bitmask and an if-chain, not a table, so there is nothing more to decode.
  Both mask constants (`0xa6`, `0x140`) are `mov` immediates and are quoted
  above.

---

## 8. Reproduce

```sh
# the factory: cmp w8,#3 -> ConstantQpRateControl vtable
python3 tools/disas.py --fw   --addr 0x60c4   -n 0x2c0

# the firmware names params+0xC "RCMode"
python3 tools/disas.py --fw   --addr 0x5cd4   -n 0x3f0

# wire sRC -> RateControlParameters
python3 tools/disas.py --fw   --addr 0x7aab8  -n 0x300

# the 272-byte _S_AVE_Alg_Cfg copy, and the Feature-bit-31 controller select
python3 tools/disas.py --fw   --addr 0x6b8b0  -n 0xc0
python3 tools/disas.py --fw   --addr 0x6d340  -n 0xa0

# ProcessCmd_Start validates cmd+0x234
python3 tools/disas.py --fw   --addr 0x32680  -n 0x20
python3 tools/disas.py --fw   --addr 0xf830   -n 0x18

# CBR == 2, by name
python3 tools/disas.py --fw   --addr 0x95a8   -n 0x70
python3 tools/disas.py --fw   --addr 0xf39c   -n 0x1c8

# what mode 3 actually does
python3 tools/disas.py --fw   --addr 0x8bd4   -n 0xc8      # new framework
python3 tools/disas.py --fw   --addr 0xce44   -n 0x30      # legacy CRateControl

# host side
python3 tools/disas.py --kext --addr 0xfffffe0008b5fa8c -n 0x18   # RCMode==3 sizing
python3 tools/disas.py --kext --addr 0xfffffe0008b56790 -n 0x30   # AVE_EncMode_Max == 3
python3 tools/disas.py --kext --addr 0xfffffe0008bfd9bc -n 0x168  # DecideMode is a printer
```

Vtable decode (firmware `__DATA` vm `0x134000` / file `0x138000`; low 32 bits
of each slot are the target VA):

```sh
python3 - <<'EOF'
import struct
d = open('data/blobs/ave_h13c.bin','rb').read()
for name, va in [('ConstantQpRateControl', 0x135938),
                 ('ConstantRateFactorRateControl', 0x135988)]:
    off = 0x138000 + (va - 0x134000)
    print(name, [hex(struct.unpack('<Q', d[off+i:off+i+8])[0] & 0xFFFFFFFF)
                 for i in range(0x10, 0x50, 8)])
EOF
# ConstantQpRateControl        ['0xa3d0', '0xa444', '0x8bd4', '0xa4b8', ...]
#                                                    ^ processRateControl
# ConstantRateFactorRateControl['0xa4cc', '0xa540', '0x8010', '0xa39c', ...]
```

Strings:

```sh
strings -t x data/blobs/ave_h13c.bin  | grep -E 'RCMode %d|unsupported RC mode|ConstantQpRateControl'
strings -t x data/blobs/AppleAVE2.macho | grep -E 'AVE_RCMode_None|AVE_EncMode_Max'
```

---

## 9. What this changes for the driver

```c
/* sCAveCmdAvcStart, fixed-QP AVC */
cmd[0x228] = <Feature>            /* u64; SET BIT 31 for ConstantQpRateControl */
cmd[0x230] = 0                    /* LookAheadFrameCount; clamped to <= 20 */
cmd[0x234] = 3                    /* RCMode = constant QP   <- CONFIRMED       */
cmd[0x238] = <bitrate>            /* ignored by mode 3, but keep it sane        */
cmd[0x240] = qp_i                 /* read by ConstantQpRateControl at fw 0x8c38 */
cmd[0x244] = qp_p
cmd[0x248] = qp_b
```

Do **not** send `RCMode` 0, 4, 20 or 100: 0 and 4 make `CreateInstance` return
`NULL` (`-1001` out of `CreateRateControl`), and 20/100 leave the session with
no rate controller object at all.

Roadmap item #2 is answered for `RCMode`. `_E_AVE_EncMode` is bounded
(3 enumerators) but unnamed, and is not on the path to a first encode.

---

## Verification pass (independent re-derivation)

Every load-bearing hop was re-read from the bytes rather than taken on trust.

| Claim | Evidence | Verdict |
|---|---|---|
| Mode 3 dispatches to a distinct class | `0x6200: cmp w8,#0x3` / `0x6204: b.ne 0x6280` | confirmed |
| ...and installs `__ZTV21ConstantQpRateControl` | `0x6224: adrp x16,0x135000` / `add #0x938` / `add #0x10` / `0x6238: pacda` / `0x623c: str x16,[x19]` | confirmed |
| That vtable's `processRateControl` is `0x8bd4` | `__DATA` `0x135958` = `0x8011652000008bd4` | confirmed |
| `0x8bd4` is the QP-reading function | `0x8c18` selects `+0x48/0x4c/0x50` by slice type | confirmed (also doc 32) |
| The firmware names the field `RCMode` | VA `0x11e86c` = `"RCMode %d"` | confirmed |
| `sRC` base is Start `+0x220` | `0x7ab54: ldp w22,w8,[x21,#20]` → `sRC+0x14` = `0x234` = the RCMode offset already confirmed in doc 20 | confirmed |
| `sRC.Feature` bit 31 selects the framework | `0x6d368: ldrb w8,[x24,#1203]` / `0x6d36c: tbnz w8,#7` — byte 1203 bit 7 is bit 31 of the word at 1200 | confirmed |

**One correction made.** The vtable table originally carried `0x63d0` / `0x6444`
in its first two rows, `0x4000` below the actual bytes. The conclusion was
unaffected — the `+0x20` row was right — but the table contradicted its own
stated convention, which is exactly how a Trap 5 error would look if one were
present. It is not: see the note in §1.2.

**One claim deliberately left partial.** The `sRC.Feature` bit is read at
controller offset 1203, not from the wire command directly, so the
command→controller mapping is inferred from the `sRC` base rather than read at
the copy site. The bit position and its effect are confirmed; only the
arithmetic tying offset 1203 back to Start `+0x22B` is inferred.
