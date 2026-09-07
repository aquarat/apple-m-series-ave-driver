# Coded-output (bitstream) buffer sizing

*How big the `CodedData` surface is, what determines it, and what a Linux V4L2
driver should allocate for its capture queue.*

This closes the item that [16-encode-surface-set.md](16-encode-surface-set.md)
§7 recorded as the single largest gap: "`AVE_CalcBufSizeOfCodedData` — the
rate-control-dependent bitstream buffer size. This is the one number a V4L2
driver most needs and it is not a closed form."

**It is a closed form.** The earlier reading was wrong on the important point.
The function's inputs do *not* include bitrate, frame rate, level, profile or
entropy mode. It reads exactly thirteen scalars, five doubles from a constant
table, and one 4-entry chroma-divisor table, and nothing else. Under the
parameters a straightforward encoder uses it degenerates to

```
size = align_up( align_up(W, mb) * H * 3/2 , 4096 )
```

i.e. **one uncompressed 8-bit 4:2:0 frame**, page-rounded — regardless of
codec, bitrate, QP or rate-control mode.

Everything below was read out of `AppleAVE2.kext` in `data/blobs/kc.macho`
(macOS 26.6.2, `kernelcache.release.mac13j`) with `tools/disas.py`, on
DevType 10 = `t6001` = M1 Max ([17-aux-engines-pools.md](17-aux-engines-pools.md) §5).
Every constant carries the VA of the instruction it came from. Anything not
read directly out of an instruction is marked **inferred** or **unknown**.

---

## 0. Summary

| Question | Answer | Confidence |
|---|---|---|
| Is `CalcBufSizeOfCodedData` rate-control dependent? | Only via `RCMode`/`initialQPI`, and only when `RCMode == 3` | Confirmed |
| Does it read bitrate, framerate, level, profile, entropy mode? | **No** — not in the parameter list, no table read | Confirmed |
| Base quantity | `AVE_Linear_CalcFrameSize(align_up(W,mb), H, 8, 420)` = `1.5 * align_up(W,mb) * H` | Confirmed |
| Macroblock alignment | AVC (`EncType==1`) -> 16; HEVC/AV1 -> 32. Width only, **height is not aligned** | Confirmed |
| Float table `0x723e9f0`..`0x723ea10` | `1.3, 1.6, 1.2, 2.8, 51.0` — chroma-format and lossless *inflation factors*, plus a QP divisor | Confirmed |
| Hard ceiling | `2 x base` (non-lossless) / `2.8 x base` (lossless), always | Confirmed |
| Hard floor | `min(2 x base, 460800)` | Confirmed |
| Rounding | `align_up(x, 4096)`; then the surface allocator rounds to 16 KiB | Confirmed |
| Level-derived (MaxCPB/MaxBR) limit | **None** — no level table is consulted | Confirmed |
| `CalcBufSizeOfCodedHeader` | `0xC000` = 49152, constant, no arguments | Confirmed |
| Buffer count | `<= 30`, from `AVE_CalcBufNumOfCodedData` | Confirmed |
| Recommended driver bound | see §7 | Derived |

---

## 1. Function inventory

Everything matching `Coded` in `data/derived/kext-symbols.txt` that is a sizing
or counting routine:

| VA | Symbol | Shape |
|---|---|---|
| `0xfffffe0008b5f58c` | `AVE_CalcBufSizeOfCodedData(_E_AVE_DevType, _E_AVE_EncType, int, int, _E_ChromaFmt, int, int, bool, int, bool, _E_AVE_EncMode, _E_AVE_RCMode, int)` | the primary target, `0x5a0` bytes |
| `0xfffffe0008b5f4d0` | `AVE_CalcBufNumOfCodedData(int,int,int,int,int,int, EncMode, bool, MCTF_Mode, int, RCMode, int)` | branch-free, `0xbc` bytes |
| `0xfffffe0008b5fb88` | `AVE_CalcBufSizeOfCodedHeader(void)` | `mov w0,#0xc000; ret` |
| `0xfffffe0008b5fb2c` | `AVE_CalcBufNumOfCodedHeader(_E_AVE_WorkType, …)` | 3 arms, tail-calls the above |
| `0xfffffe0008b5fc30` | `AVE_CalcBufSizeOfProtectedData(same signature)` | **tail-call to `CalcBufSizeOfCodedData`** (`b 0xfffffe0008b5f58c` at `0xb5fc3c`) — identical size |
| `0xfffffe0008b62f6c` | `AVE_CalcBufSizeOfTranscodedData(same signature)` | `align_up(CalcBufSizeOfCodedData(...) / 2, 4096)` (`0xb62f9c`–`0xb62fac`) |
| `0xfffffe0008b62f54` | `AVE_CalcBufNumOfTranscodedData(DevType)` | `sub w8,w0,#7; cmn w8,#3; csel w0,wzr,#2,cc` -> 0 on DevType 10 |

`CODED_DATA_HDR` also appears as a *runtime* structure in
`AVE_Work_Enc_UpdateFrameInfoFromOutputData` (`0xfffffe0008cab2c4`),
`AVE_MD_SVE::RetrieveRCStats` (`0xfffffe0008c89e10`) and the `*_PostUpdate`
family — that is the per-frame result header the firmware writes back, not a
sizing input.

### Trap 1 check

`AVE_CalcBufSizeOfCodedData` contains four instances of the logging idiom that
[00-methodology.md](00-methodology.md) warns about:

```
fffffe0008b5f698:  mov  w0, #0x74      ; subsystem 116
fffffe0008b5f69c:  mov  w1, #0x4       ; level 4
fffffe0008b5f6a0:  bl   0xfffffe0008c46348   ; AVE_Log_CheckLevel(subsys, level)
```

and the same with `#0x4f` / `#0x8` at `0xb5f888` and `0xb5f9b8`. **None of these
is an allocation.** `0x74` and `0x4f` are log subsystem ids. The size arithmetic
in this function never calls an allocator at all — it returns an integer.

---

## 2. What the parameters actually are

The function logs its own arguments, and the format strings name every one of
them. This is authoritative, not inferred:

```
0x27c001 (VA 0xfffffe0007280001):
  "%lld %d AVE %s: %s:%d encType: %d, width: %d, height: %d, chromaFmt: %d,
   iBitDepth: %d, bufSize: %d, bLossless: %d, bufSizeFactor: %d, bMaxBufSize: %d"
0x27c09a (VA 0xfffffe000728009a):
  "%lld %d AVE %s: %s:%d encMode: %d, RCMode: %d, initialQPI: %d,
   normalSize: %d, size: %d"
```

Matching those to the os_log argument block built at `0xb5f8b8`–`0xb5f900` and
`0xb5f9e0`–`0xb5fa10`, and to the Apple AArch64 *packed* stack-argument layout
(`x29+16, +20, +24, +28, +32`; note `ldrb w8,[x29,#20]` at `0xb5f600` proving
the `bool` occupies one byte at `+20`):

| # | reg / slot | name | fed from `_S_AVE_Client` by `AVE_Work_Enc_CalcSurfaceInfo` |
|---|---|---|---|
| 1 | `x0` | `devType` | `AVE_DevInfo::GetDevType()` -> **10** on M1 Max (`0xca9c80`, `0xca9cdc`) |
| 2 | `x1` | `encType` | `client[15532]` (`0xca9a84`, `0xca9d24`) — 1 = AVC, 2 = HEVC |
| 3 | `x2` | `width` | `client[6200]` (`0xca9a6c`, `0xca9c88`) |
| 4 | `x3` | `height` | `client[6204]` (`0xca9a70`, `0xca9a74`) |
| 5 | `x4` | `chromaFmt` | AVC: `client[19188]`; HEVC: `client[36280]` (`0xca9ab4` / `0xca9ae4`) |
| 6 | `x5` | `iBitDepth` | `8 + max(bit_depth_luma_minus8, bit_depth_chroma_minus8)`: AVC `client[19196]`/`client[19200]`, HEVC `client[36324]`/`client[36328]`, each `+8`, then `max` at `0xca9dd8` |
| 7 | `x6` | `bufSize` | `client[16556]` (`0xca9b18`) — explicit override |
| 8 | `x7` | `bLossless` | AVC `client[19204]`, HEVC `client[51551]` (byte, `0xca9ab8` / `0xca9ae8`), `& 1` at `0xca9e04` |
| 9 | `[x29+16]` | `bufSizeFactor` | `client[16564]` (`0xca9b5c`) — a **percentage** |
| 10 | `[x29+20]` | `bMaxBufSize` | `client[16560]` (byte, `0xca9b64`), `& 1` at `0xca9df4` |
| 11 | `[x29+24]` | `encMode` | `client[15512]` (`0xca9b6c`) |
| 12 | `[x29+28]` | `RCMode` | `client[5892]` (`0xca9b74`) |
| 13 | `[x29+32]` | `initialQPI` | `client[5904]` (`0xca9b78`) |

The `RCMode`/`initialQPI` offsets are independently confirmed by
`AVE_Client_Enc_Print` (`0xfffffe0008b7c154`), which feeds the format string
`"RCFeature: 0x%llx LookAhead: %d RCMode: %d Bitrate: %d QP: %d %d %d
CRFScale: %d.%06d RCQPRange: [%d, %d]"` (file `0x281bcd`) from
`client[5880] / [5888] / [5892] / [5896] / [5904] / [5908] / [5912] /
[5960] / [5992] / [5996]` at `0xb7c420`–`0xb7c468`. So `+5892` is `RCMode`
and `+5904` is the first of three QP fields.

The parameters 5, 6 and 8 come out of what is plainly a stored SPS: at
`client+19188` (AVC) the layout `{+0: chroma_format_idc, +8:
bit_depth_luma_minus8, +12: bit_depth_chroma_minus8, +16: <bool> }` matches
H.264 SPS syntax order exactly, and the H.264 flag in that position is
`qpprime_y_zero_transform_bypass_flag` — the lossless flag. That the kext's
own log calls the argument `bLossless` confirms it.

### Enum values, read from the kext's own name tables

`_E_AVE_EncType` at `0xfffffe000c69a8f8`: `0` = (none), **`1` = `AVC`**,
`2` = `HEVC`, `3` = `AV1` (string pool at file `0x289745`). Cross-checked
against the assertion strings `"AVE_EncType_None < pIn->eEncType &&
pIn->eEncType < AVE_EncType_Max"` (file `0x2b097a`) and
`"m_eEncType == AVE_EncType_AVC"` (file `0x2acafa`).

`_E_ChromaFmt`: `0`=400, `1`=420, `2`=422, `3`=444 (string pool `"400\0420\0
422\0444"` at file `0x2896c7`; agrees with [16](16-encode-surface-set.md) and
the divisor table in §4).

`_E_AVE_RCMode`: **names not recoverable.** No enumerator-name string array
exists in either binary. Values observed in comparisons: `1`, `2`, `4` and
`20` in the kext (scan of `ldr wN,[xM,#5892]` followed by `cmp wN,#imm`), `3`
in this function, and the firmware's `CRateControl::Check_RCMode` (firmware VA
`0xf830`) accepts only `20` and `100`. So the enum is sparse and `3` is a
distinct mode whose name is **unknown**.

`_E_AVE_EncMode`: values `1` and `2` observed; names **unknown**.

---

## 3. `AVE_CalcBufSizeOfCodedData` — the computation

Helper, `AVE_Linear_CalcFrameSize(int w, int h, int bitDepth, _E_ChromaFmt fmt)`
at `0xfffffe0008b8cfec` (24 instructions, no branches out):

```c
y = w * h * ((bitDepth + 7) >> 3);            // 0xb8cff0-0xb8d004
if (fmt == 0) return y;                        // 0xb8d008 -> 0xb8d040
(dw, dh) = ((int2*)0xfffffe0007276400)[fmt];   // 0xb8d010-0xb8d02c
return y + ((y / dw) / dh) * 2;                // 0xb8d030-0xb8d044
```

Divisor table dumped from `__PRELINK_TEXT` at `0xfffffe0007276400`:
`(1,1) (2,2) (2,1) (1,1)` for indices 0..3 — so 4:2:0 gives `1.5*y`, 4:2:2 gives
`2*y`, 4:4:4 gives `3*y`. (Third independent confirmation of the ChromaFmt
ordering; see [16](16-encode-surface-set.md).)

Now the function itself. Register aliases: `mb` = the alignment granule,
`base` = `w22`, `size` = `w27`.

```c
int AVE_CalcBufSizeOfCodedData(devType, encType, W, H, chromaFmt, iBitDepth,
                               bufSize, bLossless, bufSizeFactor, bMaxBufSize,
                               encMode, RCMode, initialQPI)
{
    /* ---- 1. macroblock alignment of the WIDTH ONLY ------------------- */
    mb   = (encType == 1) ? 16 : 32;              // 0xb5f5c8-0xb5f5d8
    mask = (encType == 1) ? ~15 : ~31;            // 0xb5f5c0-0xb5f5cc

    /* ---- 2. overflow guard ------------------------------------------ */
    if ((W|H) < 0 || (int64)W*H >= 0x80000000)    // 0xb5f5e0-0xb5f5f0
        return 0;                                 // 0xb5f6f4 / 0xb5f740

    Wa = (W + mb - 1) & mask;                     // 0xb5f608-0xb5f610

    /* ---- 3. base = one uncompressed 8-bit 4:2:0 frame ---------------- */
    R = AVE_Linear_CalcFrameSize(Wa, H, 8, /*420*/1);  // 0xb5f614-0xb5f620
      = 3 * Wa * H / 2

    base = (iBitDepth < 9) ? R : (R * 10) / 8;    // 0xb5f624-0xb5f644
                                                  //   10-bit costs +25%

    /* ---- 4. chroma-format inflation --------------------------------- */
    pix = W * H;                                  // 0xb5f650
    K   = 921601;                  // = 1280*720 + 1   0xb5f648-0xb5f64c
    if (!(devType == 11 && pix >= 0x1000001))     // 0xb5f654-0xb5f664
    {                                             //   (never true on M1: DevType 10)
        if (chromaFmt == 3)                       // 0xb5f66c-0xb5f670
            base *= (pix < K) ? d[0x9f8] /*1.6*/ : d[0x9f0] /*1.3*/;
                                                  // 0xb5f748-0xb5f764
        else if (chromaFmt == 2)                  // 0xb5f674-0xb5f678
            base *= (pix < K) ? 1.5             : d[0xa00] /*1.2*/;
                                                  // 0xb5f67c-0xb5f694
        /* chromaFmt 0 and 1: no scaling */
        base = (int)base;                         // fcvtzs, 0xb5f76c
    }

    /* ---- 5. "maximum buffer size" mode ------------------------------ */
    if (bMaxBufSize) {                            // 0xb5f790
        /* recompute from a 4:4:4 frame at the REAL bit depth; the
           chroma-inflated value above is discarded */
        base = AVE_Linear_CalcFrameSize(Wa, H, iBitDepth, /*444*/3);
                                                  // 0xb5f79c-0xb5f7b0
              = 3 * Wa * H * ((iBitDepth+7)>>3)
    }

    /* ---- 6. pick the raw size --------------------------------------- */
    if (bLossless)                                // 0xb5f7b4 / 0xb5f7e4
        size = base * ((pix < K) ? d[0xa08] /*2.8*/ : 2.5);
                                                  // 0xb5f7b8-0xb5f7d4,
                                                  // 0xb5f7e8-0xb5f804
    else {
        if (!bMaxBufSize && bufSizeFactor != 0)   // 0xb5f930
            size = (base / 100) * bufSizeFactor;  // 0xb5f934-0xb5f948
        else
            size = base;                          // 0xb5f90c / 0xb5fa8c

        if (RCMode == 3) {                        // 0xb5fa94-0xb5fa9c
            f = (initialQPI < 12) ? 2.0           // 0xb5faa0-0xb5faa8
                                  : (102 - initialQPI) / d[0xa10] /*51.0*/;
                                                  // 0xb5faac-0xb5fac0
            size = (int)(f * size);               // 0xb5fac4-0xb5facc
        }
        if (encMode == 2) {                       // 0xb5fad4-0xb5fadc
            size = max(size, base * d[0xa00] /*1.2*/);
                                                  // 0xb5fae0-0xb5fafc
            if (initialQPI >= 13) {               // 0xb5fb00-0xb5fb04
                cap  = (size > 8388609) ? size/2 : 4194304;
                                                  // 0xb5fb08-0xb5fb1c
                size = min(size, cap);            // 0xb5fb20-0xb5fb24
            }
        }
    }

    /* ---- 7. floor: never below one 640x480 4:2:0 frame --------------- */
    m = AVE_Linear_CalcFrameSize(640, 480, 8, 1); // 0xb5f80c-0xb5f840
      = 460800
    if (size <= m) size = m;

    /* ---- 8. ceiling -------------------------------------------------- */
    if (bLossless) size = min(size, (int)(base * 2.8));  // 0xb5f848-0xb5f864
    else           size = min(size, 2 * base);           // 0xb5f86c-0xb5f874

    /* ---- 9. explicit override, then page rounding -------------------- */
    if (bufSize != 0) size = bufSize;             // 0xb5f878-0xb5f87c
    return (size + 0xfff) & ~0xfff;               // 0xb5f880-0xb5f884
}
```

Two consequences worth stating plainly, because they are what makes this
tractable:

* **Step 8 is unconditional.** Whatever rate control, QP, percentage or
  encode mode did in steps 5–6, the result is clamped to `2 x base`
  (`2.8 x base` when lossless) before it is returned. There is no path around
  it.
* **`(102 - QP)/51` maxes out at 2.0**, and step 6's `bufSizeFactor` is
  likewise capped by step 8. So the rate-control machinery can at most double
  the base figure, never more.

The `640x480` floor is itself subject to the step-8 ceiling, so for very small
frames the effective floor is `min(2 x base, 460800)` — e.g. QCIF 176x144 AVC
returns 77,824, not 462,848.

---

## 4. The float table at `0xfffffe000723e9f0`

`__PRELINK_TEXT`, file offset `0x23a9f0`. All five are IEEE-754 `double`:

| VA | offset used in code | value | role |
|---|---|---|---|
| `0xfffffe000723e9f0` | `[x9 + 0]`, `x9 = 0x723e9f0` (`0xb5f758`) | **1.3** | 4:4:4 inflation, `pix >= 921601` |
| `0xfffffe000723e9f8` | `[x9 + 8]` (`0xb5f75c`) | **1.6** | 4:4:4 inflation, `pix < 921601` |
| `0xfffffe000723ea00` | `[x8 + 2560]` (`0xb5f688`, `0xb5faec`) | **1.2** | 4:2:2 inflation (`pix >= 921601`); also the `encMode==2` floor multiplier |
| `0xfffffe000723ea08` | `[x8 + 2568]` (`0xb5f7c0`, `0xb5f7f0`, `0xb5f854`) | **2.8** | lossless multiplier (`pix < 921601`) and the lossless ceiling |
| `0xfffffe000723ea10` | `[x8 + 2576]` (`0xb5fabc`) | **51.0** | QP divisor in `(102 - QP)/51` |

Two more constants are immediates, not table entries: `fmov d2, #1.5`
(`0xb5f68c`, 4:2:2 for `pix < 921601`) and `fmov d1, #2.5` (`0xb5f7c4`,
`0xb5f7f4`, lossless for `pix >= 921601`).

**What the values encode.** They are *not* bits-per-pixel and *not* a
compression ratio ladder. They are **inflation factors relative to the 8-bit
4:2:0 frame size**, and this is established from how they are used, not
assumed: the multiplicand is always `AVE_Linear_CalcFrameSize(Wa, H, 8, 420)`,
which is a byte count for a raw 4:2:0 frame (`0xb5f614`–`0xb5f620`), and the
product is returned directly as a byte count. So the whole function says
"the coded frame will not exceed *N* raw 4:2:0 frames", with

* `N = 1.0` for 4:2:0 and monochrome,
* `N = 1.2` (>720p) or `1.5` (<=720p) for 4:2:2,
* `N = 1.3` (>720p) or `1.6` (<=720p) for 4:4:4,
* `N = 2.5` (>720p) or `2.8` (<=720p) for lossless,
* times `1.25` for 10-bit and up.

Note the ordering is self-consistent: 4:4:4 > 4:2:2 in every pair, and small
frames get a larger margin than large ones (relatively more per-frame header
and slice overhead). The threshold is exactly `1280*720 + 1 = 921601`
(`mov w11,#0x1001; movk w11,#0xe,lsl #16` at `0xb5f648`/`0xb5f64c`), so 720p
itself falls on the *small* side.

`51.0` is the QP normaliser: `(102 - QP)/51` runs from 2.0 at QP 0 to 1.0 at
QP 51, i.e. "halve the budget as QP goes from 0 to 51", with a flat 2.0 below
QP 12.

The neighbouring words are not part of this table — `0x723e9e8` is zero and
`0x723ea18` onward is `_os_log_fmt` string data (`0x723ea18` is
`__ZZ27AVE_CalcBufSizeOfMCTFOutput…E8iaShiftY` in the symbol map).

---

## 5. Clamping, alignment, and the absence of a level table

| kind | value | VA |
|---|---|---|
| overflow guard | `W<0 \|\| H<0 \|\| (int64)W*H >= 2^31` -> return 0 | `0xb5f5e0`–`0xb5f5f0` |
| width alignment | 16 (AVC) / 32 (HEVC, AV1) | `0xb5f5c0`–`0xb5f610` |
| height alignment | **none** — `H` is used raw | `0xb5f614` |
| floor | `460800` (a 640x480 4:2:0 frame), itself subject to the ceiling | `0xb5f80c`–`0xb5f840` |
| ceiling (normal) | `2 x base` | `0xb5f86c`–`0xb5f874` |
| ceiling (lossless) | `2.8 x base` | `0xb5f848`–`0xb5f864` |
| `encMode==2` cap | `min(size, size>8388609 ? size/2 : 4194304)` when `initialQPI >= 13` | `0xb5fb00`–`0xb5fb24` |
| final rounding | `align_up(x, 4096)` | `0xb5f880`–`0xb5f884` |
| allocation rounding | `align_up(size, 16384)` by the surface allocator | [16](16-encode-surface-set.md) §3 |

**No level table exists in this path.** The whole function is 360 instructions
and its only data references are: the five doubles above, the chroma divisor
table (via `AVE_Linear_CalcFrameSize`), and log format strings. `MaxCPB` /
`MaxBR` are nowhere in it. The kext *does* have level machinery —
`AVE_Alg_DecideLevel` (`0xfffffe0008cd37f0`), `AVE_RC_DecideVBVMaxBitRate`
(`0xfffffe0008bfe13c`), `AVE_RC_DecideVBVBufferSize` (`0xfffffe0008bfe54c`),
`_E_AVC_Level` / `_E_HEVC_Level` in `AVE_DPB`'s constructors — but none of it
feeds the `CodedData` size. This is a discriminating check, not an absence
argument: the complete parameter list is fixed by the mangled name and the
complete set of loads is fixed by reading all `0x5a0` bytes.

---

## 6. Buffer counts, and `CodedHeader`

### `AVE_CalcBufSizeOfCodedHeader` — reconciling docs 16 and 17

There is nothing to reconcile. The function takes **no arguments**:

```
fffffe0008b5fb88:  bti  c
fffffe0008b5fb8c:  mov  w0, #0xc000        ; 49152
fffffe0008b5fb90:  ret
```

`0xC000` *is* 49152. [16](16-encode-surface-set.md)'s "flat 49152 at 1080p" and
[17](17-aux-engines-pools.md)'s "flat `0xC000` for LRME" are the same constant
written two ways; it is resolution-, codec- and work-type-independent.

### `AVE_CalcBufNumOfCodedHeader` (`0xfffffe0008b5fb2c`)

```
if ((unsigned)(workType - 5) < 2) return 10;   // 0xb5fb30-0xb5fb3c
if (workType == 2)                return 4;    // 0xb5fb44-0xb5fb4c
tail-call AVE_CalcBufNumOfCodedData(args shifted left by one)  // 0xb5fb84
```

The literal work-type values are visible at the call sites: GGM passes 5
(`mov w0,#0x5` at `0xb3f614`), DMV passes 6 (`mov w0,#0x6` at `0xc37750`) —
confirming [17](17-aux-engines-pools.md)'s reading of the `{5,6}` arm — and
`AVE_Work_Enc_CalcSurfaceInfo` passes 1 (`mov w0,#0x1` at `0xca9e44`), so for
the encode path the header count equals the data count.

### `AVE_CalcBufNumOfCodedData` (`0xfffffe0008b5f4d0`)

Branch-free. With `a0..a5` the six leading ints, `a6 = encMode`, `a7` a bool,
`a8 = MCTF_Mode`, `a9`, `a10 = RCMode`, `a11`:

```c
c = (a2 == 0) ? 1 : a2;                       // 0xb5f4e0-0xb5f4e4
p = (a7 == 0) ? c + (a3 ? 2 : 4)              // 0xb5f4e8-0xb5f4f4, 0xb5f514
              : ((a8 == 1) ? 2 : c + 7);      // 0xb5f4fc-0xb5f50c
q = (a9 == 1) ? 12 : c + 4;                   // 0xb5f518-0xb5f52c
n = (a1 < 2) ? p : q;                         // 0xb5f530-0xb5f534
if (a6 == 2) n = 10;                          // 0xb5f538-0xb5f53c
n += (a5 - 1) * (a2 + 2);                     // 0xb5f540-0xb5f548
if (a11 > 0) n = min(n, a11 * a5);            // 0xb5f54c-0xb5f55c
n = min(n, 30 / (3 - a5));                    // 0xb5f560-0xb5f574
if (a0 != 0) n = a0;                          // 0xb5f578-0xb5f57c
return min(n, 30);                            // 0xb5f580-0xb5f584
```

From `AVE_Work_Enc_CalcSurfaceInfo` (`0xca9d8c`–`0xca9dc8`): `a0 =
client[16552]` (an explicit override), `a2 = client[6016]`, `a5 =
client[15316]`, `a6 = client[15512]`, `a8 = client[15312]`, `a10 =
client[5892]`, `a11 = client[5480]`, `a1`/`a9` from the `_S_AVE_DLB_Unit`
(`[+24]`, `[+20]`). The meaning of those client fields is **unknown**; what is
certain is the hard bound: `1 <= count <= 30`. The result is stored to the
InfoSet at `[infoset+360]` (`0xca9dcc`) and the size to `[infoset+364]`
(`0xca9e24`) — entry 8 (`CodedData`) `+0x18` and `+0x1c`, matching
[16](16-encode-surface-set.md) §1.

---

## 7. The practical answer for a driver

### 7.1 The exact value Apple's kext computes, for the parameters a normal encode uses

With `bLossless = 0`, `bMaxBufSize = 0`, `bufSize = 0`, `bufSizeFactor = 0`,
`iBitDepth = 8`, `chromaFmt = 420`, and any `RCMode != 3` / `encMode != 2`,
the function reduces exactly to:

```c
/* mb = 16 for H.264, 32 for HEVC/AV1 */
static u32 ave_coded_data_size(u32 w, u32 h, bool hevc)
{
        u32 mb   = hevc ? 32 : 16;
        u32 wa   = ALIGN(w, mb);
        u32 base = wa * h * 3 / 2;          /* one raw 8-bit 4:2:0 frame */
        u32 size = (base >= 460800) ? base : min(2 * base, 460800u);
        return ALIGN(size, 4096);
}
```

This is a **true closed form**, not a conservative bound: it is what
`AVE_CalcBufSizeOfCodedData` returns, instruction for instruction, on that
input domain. A driver that configures the session itself controls all of
`bLossless`, `bMaxBufSize`, `bufSize` and `bufSizeFactor`, so it can *hold*
the function on this path.

### 7.2 Safe upper bound if you do not want to pin the rate-control state

If you want a number that is valid no matter what `RCMode`, `encMode`,
`initialQPI` or `bufSizeFactor` the firmware/host ends up using, take the
step-8 ceiling — which no path can exceed:

```
size <= ALIGN( 2 * base , 4096 )  =  ALIGN( 3 * ALIGN(W, mb) * H , 4096 )
```

i.e. **3 bytes per pixel** (24 bpp) for 8-bit 4:2:0, both codecs. This is a
*derived maximum over the input domain*, read from the ceiling instruction at
`0xb5f86c`–`0xb5f874`, not a guess.

Extending the domain (still 8-bit, from `base`'s own maximum in §3 step 4):

| domain admitted | bound |
|---|---|
| 4:2:0 / mono, non-lossless | `3 * Wa * H` |
| any chroma format, non-lossless, `!bMaxBufSize` | `3.2 * Wa * H` (`2 x 1.6 x 1.5`) |
| `bMaxBufSize` allowed | `6 * Wa * H` (`2 x 3`) |
| lossless allowed | `8.4 * Wa * H` (`2.8 x 3`) |
| any bit depth `> 8` | multiply the above by `(bitDepth + 7) / 8` |

The absolute maximum the function can return, over its entire input domain and
excluding the `bufSize` override, is `2.8 * 3 * ceil(bd/8) * ALIGN(W,32) * H`.
At 4K 10-bit that is 124,416,000 bytes — which is why you should pin the
parameters rather than provision for the worst case.

### 7.3 Recommendation

Use **§7.1** for the V4L2 capture-buffer size, and reject the frame with
`V4L2_BUF_FLAG_ERROR` if the firmware reports more bytes than fit. It matches
Apple exactly for the ordinary configuration, it is generous (a real H.264/HEVC
frame at any sane bitrate is one to two orders of magnitude smaller than an
uncompressed one), and it needs nothing from Apple's rate-control state. If you
later enable 4:2:2, 4:4:4 or 10-bit, apply the §4 factors; if you enable
lossless, use `2.8 x base`.

Remember the surface allocator then rounds to 16 KiB
([16](16-encode-surface-set.md) §3), so the DMA allocation is
`ALIGN(size, 16384)`.

---

## 8. Concrete byte values

`AVE_CalcBufSizeOfCodedData` evaluated exactly (integer and IEEE-754 double
arithmetic reproduced from the disassembly), DevType 10, `bufSize = 0`:

### Default parameters — `bLossless=0, bMaxBufSize=0, bufSizeFactor=0, iBitDepth=8, chromaFmt=420, RCMode!=3, encMode!=2`

| resolution | H.264 (mb 16) | HEVC (mb 32) | = raw 4:2:0 frame |
|---|---|---|---|
| 1280 x 720 | **1,384,448** | **1,384,448** | 1,382,400 |
| 1920 x 1080 | **3,112,960** | **3,112,960** | 3,110,400 |
| 3840 x 2160 | **12,443,648** | **12,443,648** | 12,441,600 |

H.264 and HEVC agree here only because 1280, 1920 and 3840 are all multiples
of 32. They do diverge: 1360x768 gives 1,568,768 (AVC, `Wa`=1360) vs
1,585,152 (HEVC, `Wa`=1376); 176x144 gives 77,824 vs 86,016. (Stated as a
discriminating check, per [00-methodology.md](00-methodology.md) trap 2 — the
alignment difference is real, it just does not show at these three sizes.)

### Variants (identical for both codecs at these widths)

| variant | 1280x720 | 1920x1080 | 3840x2160 |
|---|---|---|---|
| default (above) | 1,384,448 | 3,112,960 | 12,443,648 |
| 10-bit 4:2:0 | 1,728,512 | 3,891,200 | 15,552,512 |
| 8-bit 4:2:2 | 2,076,672 | 3,735,552 | 14,929,920 |
| 8-bit 4:4:4 | 2,211,840 | 4,046,848 | 16,175,104 |
| monochrome (400) | 1,384,448 | 3,112,960 | 12,443,648 |
| `bLossless=1`, 8-bit 4:2:0 | 3,870,720 | 7,778,304 | 31,105,024 |
| `bMaxBufSize=1`, 8-bit | 2,764,800 | 6,221,824 | 24,883,200 |
| `RCMode=3, initialQPI=0` | 2,764,800 | 6,221,824 | 24,883,200 |
| `encMode=2, initialQPI=26` | 1,658,880 | 3,735,552 | 7,467,008 |
| **max over all 8-bit inputs** | 7,741,440 | 15,552,512 | 62,210,048 |
| **max over all inputs (10-bit)** | 15,482,880 | 31,105,024 | 124,416,000 |

Companion surfaces, for the same configurations:

| surface | size | count |
|---|---|---|
| `CodedHeader` | 49,152 (`0xC000`), always | same as `CodedData` for work type ENCODE |
| `ProtectedData` | identical to `CodedData` | gated by a bool ([16](16-encode-surface-set.md) §5) |
| `TranscodedData` | `align_up(CodedData/2, 4096)` | 0 on DevType 10 |
| `CodedData` | above | `<= 30` |

---

## 9. What could not be determined

* **The names of the `_E_AVE_RCMode` and `_E_AVE_EncMode` enumerators.** No
  name-string array exists in either binary. Only the numeric values are
  recoverable: `RCMode` is compared against `1, 2, 3, 4, 20` in the kext and
  `20, 100` in the firmware (`CRateControl::Check_RCMode`, firmware VA
  `0xf830`); `encMode` against `1, 2`. So which mode `RCMode == 3` is —
  the only one that changes the buffer size — is **unknown**. It does not
  matter for the bound, because step 8 clamps it regardless.
* **What sets `client[16552] / [16556] / [16560] / [16564]`** (`bufNum`
  override, `bufSize` override, `bMaxBufSize`, `bufSizeFactor`). The string
  `"iOutputBufSizeFactor %d"` (file `0x282c4a`) shows `bufSizeFactor` is
  user-facing, but the copy-in path from `AVE_SessionSettings_UserKernel_Data`
  was not traced; a scan for stores to those offsets returns thousands of
  false hits because the offsets are common. A driver that builds its own
  client state simply leaves all four zero.
* **The meaning of `AVE_CalcBufNumOfCodedData`'s `a1, a2, a3, a5, a9, a11`.**
  The arithmetic is fully recovered and the `<= 30` clamp is certain, but the
  client fields feeding it (`client[6016]`, `client[15316]`, `client[5480]`,
  `_S_AVE_DLB_Unit[+20]`, `[+24]`) are unnamed.
* **Whether the firmware enforces the buffer size.** `CODED_DATA_HDR` is
  written back by the firmware and read by
  `AVE_Work_Enc_UpdateFrameInfoFromOutputData` (`0xfffffe0008cab2c4`); whether
  an overflow is reported there, or the firmware simply truncates, was not
  traced. This matters for the driver's error path, not for the allocation.
* **The `devType == 11` branch** (`0xb5f654`) skips chroma inflation for
  `W*H >= 0x1000001`. DevType 11 is not any M1 part (t6000 = 9, t6001 = 10,
  per [17](17-aux-engines-pools.md) §5), so this arm is dead on the target
  hardware and was not investigated further.

---

## Reproducing

```sh
python3 tools/disas.py --kext 'AVE_CalcBufSizeOfCodedData' -n 0x5a0
python3 tools/disas.py --kext 'AVE_Linear_CalcFrameSize'   -n 0x60
python3 tools/disas.py --kext 'AVE_CalcBufNumOfCodedData'  -n 0xc0
python3 tools/disas.py --kext 'AVE_CalcBufSizeOfCodedHeader' -n 0x10
python3 tools/disas.py --kext --addr 0xfffffe0008ca9a28 -n 0x440   # the caller
python3 tools/disas.py --fw   'Check_RCMode' -n 0x20
```

The float table (`__PRELINK_TEXT` vmaddr `0xfffffe000700c000` -> file `0x8000`):

```sh
python3 - <<'EOF'
import struct
d = open('data/blobs/kc.macho','rb').read()
for va in range(0xfffffe000723e9f0, 0xfffffe000723ea18, 8):
    o = 0x8000 + (va - 0xfffffe000700c000)
    print(hex(va), struct.unpack('<d', d[o:o+8])[0])
EOF
# 0xfffffe000723e9f0 1.3
# 0xfffffe000723e9f8 1.6
# 0xfffffe000723ea00 1.2
# 0xfffffe000723ea08 2.8
# 0xfffffe000723ea10 51.0
```

The chroma divisor table (`0xfffffe0007276400`, file `0x272400`) reads
`(1,1) (2,2) (2,1) (1,1)`.
