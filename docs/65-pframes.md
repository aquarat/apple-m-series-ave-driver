# What a P-frame needs: reference surfaces, motion estimation, colocated MVs, low-res

**Status: in progress.** Static analysis only — nothing here was run on hardware.

Scope split, deliberately: [64](64-multiframe.md) owns *which DPB slot and which field value
per frame* (slot rotation, Complete/Flush). This document owns *what inter
prediction needs to exist at all* — the reference-read datapath, the motion
estimation configuration, and the slice/MB-level coding parameters that only
start to matter once a frame is not an IDR.

Conventions as [62](62-kext-field-map.md):

- Kext VAs are 13.5 kernelcache `__TEXT_EXEC` VAs; a bare `0xeaXXXX` means
  `0xfffffe0008eaXXXX`.
- Firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`).
- **wire** = byte offset in the command as it goes out on IPC channel 1.
- **VP** = offset inside `AVE_VIDEO_PARAMS` = `AVC_INIT` wire − `0x60`.
- **ctrl** = the `CAVCController` object (`x19`/`x25` in most firmware listings).
- **AP** = the encoder register aperture; firmware address `0x11xxxxx` is host
  MMIO `0x40Dxxxxxx` (`0x1130380` ⇒ `0x40D130380`), per [60](60-md-recon-stall.md) §3.2.
- Labels per [00](00-methodology.md): **[C]** read from an instruction (VA
  cited), **[I]** inferred (chain stated), **[U]** unknown.

Reproduce with `AVE_MACOS=13.5 python3 tools/disas.py --kext|--fw --addr VA -n LEN`.

---

## 0. The short version

1. **There is no separate reference table.** A P-frame reads its references out of
   *the same* `Start_AVC` DPB table the recon writer writes into — wire
   `0x88 + 0x110·set + 0x10·slot`, 17 × 16 B per set, 2 sets. A "reference
   entry" and a "recon entry" are the same 16 bytes; what changes is which slot
   the firmware picks that frame. The only *new* host obligation for P is that
   **more than one slot must be populated and each slot must be a distinct,
   full-size recon surface**. §1.
2. The 16-byte entry is `{u64 Y_MSB IOVA, u64 Y_LSB IOVA}` — the MSB/LSB pair
   [53](53-first-frame.md)/[57](57-pipe-hang.md) named. Chroma is **not** a
   separate entry: the firmware derives `UV_MSB = Y_MSB + lumaMsbBytes` and
   `UV_LSB = Y_LSB + lumaLsbBytes`, both computed in the
   `H264VideoEncoderDPB` constructor from width and height. §1.3.
3. Reference **pixels** are read by a distinct DMA block, `0x40D128000 + 0x40·i`
   (luma) and `0x40D128200 + 0x40·i` (chroma), one channel pair per reference
   index `i`, programmed by `setPipe` straight out of
   `PICMGMT + 0x6F8/0x718/0x738/0x758 + 8·i` = Apple's
   `sRef.{Y_L0_MSB, Y_L0_LSB, UV_L0_MSB, UV_L0_LSB}[me_ref_index]`. Those
   PICMGMT words are **not** host data — they are overwritten every frame by
   `CAVECommonDPB::setRefPointers` from the DPB table. §1.2, §1.4.
4. Motion estimation is a real register block at **`0x40D190000`**, plus
   `0x40D1E0000` / `0x40D1F0000` / `0x40D200000` per-reference banks. Almost
   none of it is host-configurable: the firmware programs it from
   `CAVCController` state seeded by `SetDefaultParameters`. §2.
5. The colocated MV buffer has a **writer** (`0x40D130380`, the current slot,
   `encoder_addr_dst_colo`) and a **reader** fed from `sRef.Colocated_L1[0]`.
   Both are asserted non-zero on the inter path; it is **per DPB slot** and a
   plain P-frame needs it. §3.
6. `LowResRef` is **per DPB slot** and travels with the reference through
   exactly the same `AVECommonDPBEntry` the recon address does (Start table word
   → entry `+64` → `PICMGMT + 0x778 + 8·i` = `sRef.Low_Res_Y_L0[i]`). §4.
7. **The newly load-bearing table nobody has filled**: the per-reference
   channel at `0x40D120F80 + 0x40·i` is enabled from
   `PICMGMT + 0xC28 + 8·i` = `sLowResOutput.LowResResults[me_ref_index]`, which
   `setPipe` asserts non-zero and 64-byte aligned
   (`CAVCController_H13C.cpp:6184/6185`, fw `0x53fac`/`0x542d4`). That array
   comes from **Start_AVC wire `0x3B8`** (`LowResResult`), which
   [62](62-kext-field-map.md) §3 item 8 reasonably told us to leave at zero —
   correct for an I-frame, **fatal for a P-frame**. §4.

---

## 1. Reference surfaces

### 1.1 One table, and where it is

`AVE_CHM_SetFwBuf` writes a single DPB/recon table into `AVE_VIDEO_PARAMS`
([62](62-kext-field-map.md) §1.3 row 1):

| | |
|---|---|
| wire | **`0x88 + 0x110·set + 0x10·slot`** (VP `0x28`) |
| shape | 2 sets × 17 slots × 16 B |
| kext writer | `AVE_CHM_SetFwBuf` `0xeaef38` (`str q0,[x20,x21,lsl #4]`, `x20 = VP+0x28 + 0x110·set`) **[C]** |
| per-entry producer | `AVE_CHM_GetFwDPBBuf` (`0xeae4d8`), a 32-byte out buffer of which **only the first 16 bytes** are stored (`ldr q0,[sp,#128]` `0xeaef34`) **[C]** |
| set count | 2 written, **1 read** — `H264VideoEncoderDPB` ctor does `strb #1,[dpb,#32]` (fw `0x2d1c4`) and every consumer loops to `[dpb+32]` **[C]** |

**Both** firmware consumers read this one table, and neither has a second:

| consumer | what it takes | VA |
|---|---|---|
| `CAVECommonDPB::ProvideReferenceFrames(n, VP)` | **both** u64s, `VP + 0x28 + 0x110·set + 0x10·slot` → `ctx + 0x10·(16·set+slot)`; also `VP+0x248` (LowResRef) → `ctx + 0x10A0 + 0x80·set + 8·slot` and `VP+0xF650` (Colocated) → `ctx + 0x200 + 0x80·set + 8·slot` | fw `0x2b748`–`0x2b790` (tail arm, clearest) **[C]** |
| `CAVCController::InitEncodingParameters` | **word 0 only**, `VP + 0x28 + 0x10·slot` → `ctrl + 0xCB8 + 0x10·slot`, 16 slots (`ldr x8,[x20,#40]` … `[x20,#280]`) | fw `0x5d6ac`–`0x5d728` **[C]** |

`ctx` is `[dpb + 4704]`, and the constructor sets it to `dpb + 0x38`
(`add x8,x0,#0x38 ; str x8,[x0,#4704]`, fw `0x2d1a8`–`0x2d1ac`). **[C]**
That retires the ambiguity in [60](60-md-recon-stall.md) §3.3 about what
`[dpb+4704]` is.

`ProvideReferenceFrames`' entry count is the *host's* `max_num_ref_frames`:
`ldr w1,[x24,#1072]` at fw `0x5dd14`, `bl 0x2b530` at `0x5dd38`, and the
function rejects `n > 16` (`cmp w1,#0x11 ; b.cc`, fw `0x2b5b8`) and
`n < [dpb+8]` (the level-derived MaxDpbFrames, fw `0x2b550`–`0x2b558`) with
error `0xe00002bc`. **[C]**

### 1.2 A reference entry is not different from a recon entry

The whole point: the same 16 bytes are used both ways, and the firmware decides
per frame which slot is "recon" and which are "references" by *address
matching*.

```
Start_AVC DPB table slot k   (wire 0x88 + 0x10k)
  ->  AVECommonDPBContext  ctx + 0x10k                 (ProvideReferenceFrames)
  ->  AVECommonDPBEntry[k] = ctx + 0x680 + 0x50k       (InitPointerAndVariables)
         entry+48 = table word0        (fw 0x2bdd8: str x6,[x5,#1792], x5 = ctx + 0x50k)
         entry+56 = table word1        (fw 0x2bdc8: str x6,[x5,#1800])
         entry+64 = LowResRef[k]       (fw 0x2bde8: str x6,[x5,#1808], src ctx+0x10A8+8k)
  ->  ReferenceFrameInfoData           (H264VideoEncoderDPB::ManageDPBBuffer)
         current : +72  <- entry+48 ; +216 <- entry+56 ; +224 <- entry+64   (fw 0x2d53c-0x2d55c)
         ref i   : +8+8i<- entry+48 ; +152+8i<- entry+56 ; +232+8i<- entry+64 (fw 0x2d510-0x2d524)
  ->  AVE_PICMGMT_PARAMS               (CAVECommonDPB::setRefPointers, fw 0x2c314)
```

All **[C]** at the VAs shown.

`setRefPointers(PICMGMT, refinfo)` then writes, with
`w10 = [dpb+20]` (luma tile-plane bytes) and `w9 = [dpb+24]` (luma metadata
bytes):

| PICMGMT | source | meaning | VA |
|---:|---|---|---|
| `0x898` | `refinfo+72` | `sRecon.Y_MSB` | `0x2c338` |
| `0x8A0` | `refinfo+216` | `sRecon.Y_LSB` | `0x2c328` |
| `0x8A8` | `refinfo+72 + [dpb+20]` | `sRecon.UV_MSB` | `0x2c32c` |
| `0x8B0` | `refinfo+216 + [dpb+24]` | `sRecon.UV_LSB` | `0x2c33c` |
| `0x8B8` | DPB colocated `ctx+0x200+8·slot` for the matched slot | current colocated MV **write** target | `0x2c4b0`, `0x2c5f8`, `0x2c784` |
| `0xC20` | `refinfo+224` | current LowResRef | `0x2c320` |
| **`0x6F8 + 8i`** | `refinfo+8+8i` | **`sRef.Y_L0_MSB[i]`** | `0x2c78c` (i=0) … `0x2c94c` (i=7) |
| **`0x718 + 8i`** | `refinfo+152+8i` | **`sRef.Y_L0_LSB[i]`** | `0x2c7a4` … |
| **`0x738 + 8i`** | `refinfo+8+8i + [dpb+20]` | **`sRef.UV_L0_MSB[i]`** | `0x2c79c` … |
| **`0x758 + 8i`** | `refinfo+152+8i + [dpb+24]` | **`sRef.UV_L0_LSB[i]`** | `0x2c7bc` … |
| **`0x778 + 8i`** | `refinfo+232+8i` | **`sRef.Low_Res_Y_L0[i]`** | `0x2c7c4` … |
| `0xC28 + 8i` | `ctx + 4512 + 64·[ctx+4640] + 8i` | **`sLowResOutput.LowResResults[i]`** | `0x2cd98` … |
| `0xC68 + 8i` | `ctx + 1536 + 64·[ctx+4640] + 8i` | `LowResRCResult[i]` **[I]** | `0x2cd9c` … |

The names in the "meaning" column are **not guesses** — they are the field
names out of `setPipe`'s own assert strings, which name the exact array and
index (`pPicParams->sRef.Y_L0_MSB[me_ref_index] != 0` etc.). The mapping from
each assert to its PICMGMT offset is the `cbz` that reaches it: `0x53e8c` ←
`[x23+0]`, `0x5439c` ← `[x23+32]`, `0x53f1c` ← `[x23+64]`, `0x5454c` ←
`[x23+96]`, `0x53fac` ← `[x23+1328]`, with `x23 = PICMGMT + 0x6F8 + 8i`.
**[C]** See §6.1 for the full assert list.

**[C]** for every row. The `0x6F8`/`0x718`/`0x738`/`0x758`/`0x778` arrays are
**four entries at stride 8 for i = 0..3, then the same five arrays again for
i = 4..7** starting at `0x798`/`0x7B8`/`0x7D8`/`0x7F8`/`0x818`
(fw `0x2c888`–`0x2c988`). So the hardware reference list is **8 deep**. **[C]**

> **Consequence for the driver, and it is the important one.** Everything in
> `PICMGMT + 0x6F8 .. 0x8BF` is host-writable on the wire and *thrown away*:
> `ManageDPBBuffer` calls `setRefPointers` before `ProcessPipeStart`
> ([60](60-md-recon-stall.md) §3.3 item 1). The **only** host input to the
> reference datapath is the Start_AVC DPB table at wire `0x88`, plus
> `max_num_ref_frames`. This is the same shape as the entropy-table finding in
> [61](61-mb3083-stall.md) §10. **[C]**

### 1.3 Chroma and metadata are derived, not published

`H264VideoEncoderDPB::H264VideoEncoderDPB(w, h, profile, level, …, bitDepthByte)`
fw `0x2d018`; `w1 = ctrl[2700]` (width in pixels), `w2 = ctrl[2704]` (height),
`w8 = [sp,#4]` (a byte from `[x26,#1951]`, the bytes-per-sample multiplier):

```
2d14c  w10 = w + 31
2d150  w11 = w10 << 5
2d154  w11 &= ~0x3FF                       ; luma tile-row bytes
2d158  w8  = w11 * bpp
2d164  w8 >>= 3
2d15c  w11 = (h + 35) >> 5                 ; tile rows
2d168  w8  = w8 * w11
2d170  str w8, [dpb+20]                    ; LUMA **MSB** PLANE BYTES
2d174  tw = ((w+31)>>5) - 1 ; th = w11 - 1
2d180  w8 = 32 << (64 - clz(tw) - clz(th)) ; = 32 * npot(tw+1) * npot(th+1)
2d1a0  w8 = (w8 + 0x7F) & ~0x7F
2d1a4  str w8, [dpb+24]                    ; LUMA **LSB** PLANE BYTES
```

**[C].** For 1280×720, bpp byte = 8:
`[dpb+20]` = `((1311<<5) & ~0x3FF) * 8 >> 3 * 23` = `40960 * 23` = **942 080**,
and the 40 960 tile-row figure is exactly the `+0x10 = 40960` already observed
in the recon-luma write channel ([60](60-md-recon-stall.md) §3.2) — that is the
cross-check. `[dpb+24]` = `32 << (6+5)` = **65 536**. **[C]** for the
arithmetic; the *name* "LSB plane" is Apple's (from the asserts), and the
`32 B × npot(tile_w) × npot(tile_h)` shape says it is a per-tile side-band
plane rather than a literal 2-bits-per-sample low-bit plane — an 8-bit
1280×720 picture would need 230 400 B for that, not 65 536. **[I]**

So a DPB slot must present four addresses derived from exactly two:

```
word0 (wire 0x88 + 0x10k)  ->  Y_MSB
word0 + [dpb+20]           ->  UV_MSB       (942080 at 1280x720)
word1 (wire 0x90 + 0x10k)  ->  Y_LSB
word1 + [dpb+24]           ->  UV_LSB       (65536 at 1280x720)
```

**[C]** for the arithmetic (`add x8,x12,x10` `0x2c324`, `add x11,x11,x9`
`0x2c334`, and the same pair per reference at `0x2c798`/`0x2c7ac`).
The driver's `ave_abi.h` names (`recon_y` `0x898`, `recon_uv` `0x8a8`,
`recon_y_lsb` `0x8a0`, `recon_uv_lsb` `0x8b0`) are **correct as written** —
the assert at `0x54f94` (`sRecon.Y_LSB & 127`) sits directly after
`ldr x9,[x27,#2208]`, which pins `0x8A0 = Y_LSB`. **[C]** Those four PICMGMT
words are inert (§1.2) but the layout they describe is not: the *slot* must
hold `Y_MSB` and `UV_MSB` contiguously and `Y_LSB` and `UV_LSB` contiguously,
and all four must be **128-byte** aligned (asserts 6777/6791/6805/6818 —
note that is 128, not the 64 the *references* are checked at).

### 1.4 Where reference pixels are actually read: `0x40D128000`

`CAVCController::setPipe` walks the reference list once, `x23 = PICMGMT+0x6F8`
advancing by 8 each iteration (fw `0x534b8`, `0x5388c`), trip count
`[sp,#160]+68` (a signed word), per-iteration channel stride `w24 += 0x40`
(fw `0x5389c`). For reference `i`:

| AP address | written | value | VA |
|---|---|---|---|
| `0x40D128004 + 0x40i` | luma data addr | `PICMGMT + 0x6F8 + 8i` | `0x536e0` |
| `0x40D128008 + 0x40i` | luma row bytes | `((ctrl[0xA8C]·32 + 0x3E0) & ~0x3FF) · ctrl[0x122B] >> 3` = **40960** at 1280 | `0x536d8` |
| `0x40D12800C + 0x40i` | format/control | constant **`0x44072015`** | `0x536ec` |
| `0x40D128010 + 0x40i` | luma metadata addr | `PICMGMT + 0x718 + 8i` | `0x536b0` |
| `0x40D128200 + 0x40i` | control | `0x80037F74 \| (ctrl[0xA84] != 0)` | `0x537bc` |
| `0x40D128204 + 0x40i` | chroma data addr | `PICMGMT + 0x738 + 8i` | `0x53758` |
| `0x40D128208 + 0x40i` | chroma row bytes | `((ctrl[0xA8C] >> s)·32 + 0x1E0) & ~0x1FF) · ctrl[0x122B] >> 3`, `s = (ctrl[0xA84] ∈ {1,2})` = **20480** at 1280 4:2:0 | `0x537a4` |
| `0x40D12820C + 0x40i` | control | `0x04072084 \| (ctrl[0xA84] != 0)` | `0x53730` |
| `0x40D128210 + 0x40i` | chroma metadata addr | `PICMGMT + 0x758 + 8i` | `0x53710` |
| `0x40D120F80 + 0x40i` | control | `0x80030001` iff `PICMGMT + 0xC28 + 8i` non-zero | `0x537d4` |

**[C]** for every address, value and VA. The `ctrl[0xA84]` gate is
unambiguously the **chroma format** (`0` disables the whole `+0x200` bank; the
width is halved for values 1 and 2 and not for 3, which is exactly
4:2:0 / 4:2:2 / 4:4:4), which is what fixes the `+0x000` bank as luma and the
`+0x200` bank as chroma. **[I]**, strong.

Both the luma address and the chroma address are asserted **non-zero and
64-byte aligned** before use (`cbz` `0x53690`, `tst x10,#0x3f` `0x53694`, and
the same pair at `0x53738`/`0x5373c`, `0x537c4`/`0x537c8`). A misaligned or
null reference surface therefore produces a *firmware assert*, not a DMA fault.
**[C]**

### 1.5 How the firmware picks the slot, and the one host field that gates it

`PipePrepareParam` matches the eight `PICMGMT + 0x6F8/0x798` luma addresses
against the 16 `ctrl + 0xCB8 + 0x10k` table entries and records, for each, the
table index it matched (fw `0x4840c`–`0x48638`). It then checks

```
48638  ldr w13,[x26,#3244]        ; PICMGMT + 0xCAC
4863c  cbz  w13, 0x48668          ; type 0  -> skip the check
48644  cmp  w13, #3 ; b.eq        ; type 3  -> skip the check
48648..48660  OR together the eight "matched" flags
48664  tbz  w8, #0, 0x488d4       ; none matched -> error path
```

**[C].** So **`PICMGMT + 0xCAC` is the frame/slice-type selector on the pipe
path**, and for every value other than `0` and `3` the firmware *requires* at
least one reference address to be found in the Start-time DPB table. This is
the single sharpest "did the host set the DPB up correctly" trip-wire we have
for a P-frame, and it fires before any DMA. (`PICMGMT+0xCAC` is also the word
the pooled-copy reader takes as `cmd+0x1674`, [62](62-kext-field-map.md) §2.)
Which enum value is P is **[U]** here — see §5, owned by the parallel pass.


---

## 2. Motion estimation configuration

### 2.1 Where the ME block is, and who programs it

| AP base | role | evidence |
|---|---|---|
| **`0x40D190000`** | motion estimation / ME cost generation | `CAVCController::setupMeSetupConfig` writes `+0x004..+0x018` and `+0x104` (fw `0x61c60`–`0x61cb4`); `setupMeCGenConfig` writes the `+0x910..` window (fw `0x61cbc`+) **[C]** |
| `0x40D110000` | top-level encoder config word, **carries `search_range`** | fw `0x531c0` **[C]** |
| `0x40D128000 + 0x40i` / `+0x200` | reference luma / chroma read DMA | §1.4 **[C]** |
| `0x40D1E0000`, `0x40D1F0000` | per-reference luma / chroma side blocks, one u32 slot per reference at `+0x00C + 4i` and `+0x4118 + 4i` | fw `0x53548`, `0x53560`, `0x53584`, `0x5359c` **[C]**; role **[U]** |
| `0x40D20A020 + 4i` | per-reference, `(i<<16 & 0xF0000) \| 0xC0000000` | fw `0x53530` **[C]**; role **[U]** |
| `0x40D120F80 + 0x40i` | `RDDMAMESFSRSLTS[i]` — the low-res ME result **read** channels | log string `RDDMAMESFSRSLTS baseaddr[%d] 0x%llx` at fw `0x53404`; §4 **[C]** |
| `0x40D26A08C` | `AVC_MODEDECISIONCONFIG_MESATDSCALING` | log at fw `0x5654c`, write at `0x5657c` **[C]** |

Nothing in our driver touches any of this, and nothing needs to: **every ME
register is programmed by the firmware.** The host's influence is five scalar
fields in `AVE_VIDEO_PARAMS` plus the reference/low-res surfaces.

### 2.2 The host-settable ME fields

These all live in a `AVE_VIDEO_PARAMS` sub-block at **VP `0xF760`**
(= Start_AVC wire `0xF7C0`), which `InitEncodingParameters` fans out into
`ctrl` and into `EncCommParams` at `ctrl + 0x23FC4`. The anchor is
`mov w8,#0xf760 ; add x23,x20,x8` at fw `0x5cdd0`–`0x5cdd4`, with `x20 = VP`
proved by `ldr x20,[x27,#8]` at `0x5ca20` ([62](62-kext-field-map.md) §0.3).
**[C]**

Names come from `CAVCController::DebugInit`'s own format strings paired with
the `ctrl` offset it prints (fw `0x4e6b0`–`0x4e6e8`). **[C]**

| wire | width | field | firmware destination | how used | evidence | our value |
|---:|---|---|---|---|---|---|
| **`0xFCE0`** | u16 | **`search_range`**, enum `0 = ±192H×±96V, 1 = ±128H×±64V, 2 = ±64H×±32V` (the enum text is the firmware's own) | `ctrl+4644` **and** `ctrl+4646` (`strh` twice, fw `0x5cf10`/`0x5cf14`) | `ubfiz w14,w13,#12,#3` → **`0x40D110000` bits 12..14** (fw `0x53194`, `0x531c0`); also feeds `MESATDSCALING` (fw `0x566ac`) | read fw `0x5cf0c`; name fw `0x4e6d8`+`0xc74ca` | **0** (memset) = widest range, which is the safe default |
| **`0xFCF0`** | u16 | **`skip mode`**, `0 = normal, 1 = natural skip, 2 = adaptive` | `ctrl+4638` | fw `0x5cef4`; DebugInit `0x4e6b0` + string `0xc7440` | | 0 |
| **`0xFCF4`** | u16 | **`DCT coefficient cancellation mode`**, same enum | `ctrl+4636` | fw `0x5cf08`; DebugInit `0x4e6c4` + `0xc7479` | | 0 |
| `0xFCF2` | u16 | third mode word of the same triple | `ctrl+4640` | fw `0x5cf00` | name **[U]** | 0 |
| `0xFD30` | u32→u16 | | `ctrl+4642` | fw `0x5cf18` | **[U]** | 0 |
| **`0xFCE9`** | u8 | **the ME-path selector** (§2.4) | `EncCommParams+40` = `ctrl+0x23FEC` | fw `0x5cfcc` | see §2.4 | **0** |
| **`0xFD7D`** | u8 | `NEED_LSB_PLANES` | `EncCommParams+148` = `ctrl+0x24058` | fw `0x5d08c`; read by `setPipe` at `0x5367c`/`0x53c98` to gate the **reference** LSB planes as well as recon | already in `ave_abi.h` | see §6 |
| `0xFCEA` | u16 | gates the `0x40D1F0000` (chroma-side) per-reference writes | `EncCommParams+86` | fw `0x5d024`; read `0x53128`, `0x53568` | **[U]**, but it is read as a **byte** by `setPipe` | 0 |
| `0xFCD8` | u32 | | `ctrl+2684` | fw `0x5cedc` | **[U]** | 0 |
| `0xFCEC` | u32 | | `ctrl+2736` | fw `0x5cf50` | **[U]** | 0 |

The three `ctrl+4636/4638/4644` words are the *only* ME-shaped knobs Apple's own
verbose printer names, and all three are host fields. There is **no** host field
for sub-pel refinement, partition-size masks, early-exit thresholds or lambda in
the AVC path — see §2.3.

### 2.3 Lambdas, cost tables, partition masks: firmware-side only

The HEVC verbose printer (`CHEVCController::DebugInit`, fw `0x39e10`–`0x3a220`)
names `MaxMvsPer2Mb`, `bDisableIntra8x8`, `bDisableIntra16x16`,
`bRestrictInter4x4`, `search_range`, `bSkipThrdEn`, `mode_8x8_transform`,
`ME_FullPelLambda`, `ME_SubPelLambda`, `ME_LowResLambda`, `MD_InterLambda`,
`MD_IntraLambda` — all off the **HEVC** init command, not `AVE_VIDEO_PARAMS`.
**[C]** The AVC printer has no counterpart for any of them: its ME section is
exactly the three lines in §2.2. **[C]**

On the AVC side the cost tables are generated in firmware:

- `CAVCController::setupMeCGenConfig(a, b)` (fw `0x61cbc`), called from
  `setPipe` `0x57c40` with `a = ctrl[0x1230]`, `b = ctrl[0x1231]` (two bytes
  read from `[sp,#176] = ctrl+0x122B` at fw `0x57c38`/`0x57c3c`). It fills a
  register window from `0x40D190910` upward with the constants `0x03F61FF4`,
  `0x03F60000` and `0x03F6000C` plus a per-entry `udiv` and an
  `orr w5, w5, w4, lsl #11` — i.e. a generated cost/lambda ladder, `(a + b)`
  rows of 0x100. **[C]** for the instructions; the exact cost semantics are
  **[U]**.
- `CAVCController::InitCoeffCancelCostRegs()` (in the symbol table) is the
  matching coefficient-cancellation cost programmer. **[C]** (name only).
- The kext does own `LambdaQPTable`, `qp2Lambda`, `qp2LambdaArray`,
  `lambdaArray` and `GetLambdaQPFromQP`/`GetQPFromLambdaQP`
  (`0xfffffe000722c958`, `0xfffffe000722f250/f320/f3f0`,
  `0xfffffe0008e9821c`), but they are **rate-control** helpers — they convert
  between QP and lambda for the RC decision, and none of them writes into the
  AVC `Start_AVC` image. **[I]**, from their being in the RC symbol
  neighbourhood and absent from `AVE_CHM_SetFwBuf` /
  `MakeFwCmd_Start_AVC`.

**Conclusion for Q2: there is nothing to program.** ME configuration is not a
gap in our driver; leaving the five fields above at zero gives the widest search
range, normal skip mode and normal coefficient cancellation, which is what a
first P-frame wants.

### 2.4 `EncCommParams+40` (wire `0xFCE9`) selects hardware ME vs LRME

`setPipe` tests this byte twice:

```
57c08  ldr  x9,[sp,#200]          ; x9 = ctrl + 0x23B38
57c0c  ldrb w9,[x9,#1204]         ; ctrl + 0x23FEC = EncCommParams+40
57c10  cbz  w9, 0x57c54           ; ZERO  -> skip setupMe*, take the 0x113xxxx arm
57c2c  bl   0x61c34               ; NON-ZERO -> setupMeSetupConfig()
57c40  bl   0x61cbc               ;          -> setupMeCGenConfig(ctrl[0x1230], ctrl[0x1231])
...
57c90  ldrb w9,[x9,#1204]
57c94  cbnz w9, 0x57d40           ; NON-ZERO -> skip setLRME
57d30  bl   0x5144c               ; ZERO  -> setLRME(...)
```

**[C]** for every line. It is a clean either/or, and the right name for it is
**synchronous vs asynchronous LRME**, not "ME on/off":

- **0 (our value)** — `setPipe` runs `setLRME` itself, in the pipe, and does
  **not** program the ME setup/cost registers.
- **non-zero** — `setPipe` skips `setLRME` and programs `setupMeSetupConfig` +
  `setupMeCGenConfig` instead; LRME then runs out-of-band through the flow
  controller's `LRME_FS_START` → `CAVCController::ProcessLRMEStart` → the *same*
  `setLRME` (fw `0x513e0`), gated on the same byte (`ldrb w8,[x24]; cbz`, fw
  `0x51398`, `x24 = ctrl+0x23FEC`). It also sets bit 3 of `SRCDMAGO`
  (`0x40D110128`), which the firmware's own log calls "async"
  (`ProcessLRMEStart read: SRCDMAGO 0x%x async %d-%d`, fw `0x512ec`).

**[C].** So LRME happens either way; the byte only chooses where. Keep it **0**
for the first P-frame — it is the arm the rest of our machinery (LowResRef,
`LowResSrcLumaScaled`, the 5782/5783 asserts the driver already codes against)
is wired for, and the async arm needs an extra command round-trip.

This also upgrades a label: [53](53-first-frame.md) §11.3 recorded
`ctrl+0x23FEC` as firmware-internal with no host writer, and
[59](59-row1-stall.md) §4 guessed wire `0xFCE9` as **[I]**. The writer exists
(fw `0x5cfcc`–`0x5cfd0`) and is invisible to an immediate scan because the base
is pre-biased to `ctrl+0x23FC4` and the offset is `+40` — Trap 3 again. **[C]**

### 2.5 `MERefIdxMapping` — the one ME table with a per-reference host effect

`ctrl + 0x23B38 + 4·i` (`i = 0..3`) is `EncCommParams.MERefIdxMapping[i]` — the
name is from the firmware's own asserts
`EncCommParams.MERefIdxMapping[2].f.MeRamEn == 0` / `[3]` (strings at file
`0xbee9b` / `0xbeecb`, assert sites fw `0x24908` / `0x24950`). **[C]**

`setPipe` maintains it **in place**, once per reference index, in the same loop
as the read channels:

```
534c8  ldr w9,[x21,x26]           ; x21 = ctrl+0x23B38, x26 = 4*i
534d4  and w9, w9, #0x3ff0fffe    ; keep everything but bit 0 and bits 16..19
534dc  and w10, w25, #0xf0000     ; w25 = 0x10000*i  -> the reference index
534e0  orr w9, w10, w9
534e8  orr w9, w9, #0xc0000000
534f0  str w9, [x21,x26]
```

**[C].** `setupMeSetupConfig` then copies it to the ME block with a narrower
mask:

```
61c7c  ldr w8,[ctrl+0x23B38 + 4k]
61c88  and w8, w8, #0xc00f0001     ; enable bits 30/31, ref index 16..19, bit 0
61c8c  str w8, [0x40D19000C + 4k]  ; k = 0..3
```

**[C].** So bit 0 is the field the asserts call `MeRamEn`, bits 16..19 are the
ME reference index, and bits 30..31 are an enable pair. **[I]** for the bit
names, from the mask overlap and the assert text. The host cannot set any of
them — `setPipe` rewrites the word every frame — but it is worth knowing this
exists, because a P-frame is the first time `i > 0` is ever reached.

`setupMeSetupConfig` also programs the picture geometry into the ME block:

| AP | value | VA |
|---|---|---|
| `0x40D190004` | `(ctrl[0xA98] & 0x1FFF) \| (ctrl[0xA94] << 16)`, 13 bits each | `0x61c60` |
| `0x40D190008` | same pair, 11 bits each | `0x61c78` |
| `0x40D19000C + 4k` | `MERefIdxMapping[k] & 0xC00F0001`, `k = 0..3` | `0x61c8c`–`0x61cb0` |
| `0x40D190104` | `0` | `0x61cb4` |

**[C].** `ctrl[0xA94]`/`ctrl[0xA98]` are the MB width/height pair
([59](59-row1-stall.md) §2.3).


---

## 3. The colocated MV buffer: what writes it, what reads it, how big

### 3.1 It has two independent halves, and only one of them is a P-frame thing

| | **writer** | **reader** |
|---|---|---|
| AP channel | `0x40D130380` | `0x40D120BC0` |
| Apple's name | `EncCommParams.encoder_addr_dst_colo` | `pPicParams->sRef.Colocated_L1[0]` |
| address register | `0x40D13038C = dst_colo + coloDataContextOffset` | `0x40D120BCC = Colocated_L1[0] + coloDataContextOffset` |
| size register | `0x40D130390 = bfrSizeColoData` | `0x40D120BD0 = bfrSizeColoData − coloDataContextOffset` |
| control | `0x40D130380 = 0x80030001` (or `0` + size `0x40` if the address is 0) | `0x40D120BC0` |
| host source | Start_AVC Colocated table wire `0xF6B0 + 8·slot` → DPB ctx → `PICMGMT + 0x8B8` | Start_AVC *same* table, for the **matched reference's** slot → `PICMGMT + 0x838` |
| **gate** | `ctrl+5080 != 0` — no frame-type test at all | **`slice_type == 1` only**, i.e. **B slices** |
| asserts | 6889/6890 (non-zero, 64-aligned) on the enabled arm | 6860/6861 and 6873/6874 |
| VAs | `0x554e0`–`0x555bc` ([60](60-md-recon-stall.md) §3.3) | `0x54f0c`–`0x54f30`, `0x551e4`–`0x55210` |

The reader's gate is explicit:

```
54f0c  ldr  x8,[sp,#160]        ; the slice object
54f10  ldr  w8,[x8,#16]         ; slice type
54f14  cmp  w8, #1
54f18  b.ne 0x551fc             ; not a B slice -> the reader is never touched
54f1c  ldr  x9,[x27,#2104]      ; PICMGMT + 0x838 = sRef.Colocated_L1[0]
54f20  cbz  x9, 0x551fc
54f24  adds x9, x9, w20, uxtw   ; + coloDataContextOffset
54f28  b.eq 0x5509c             ; -> ASSERT line 6860
54f2c  tst  x9, #0x3f ; b.eq ok ; else -> ASSERT line 6861
```

**[C].** `slice_type == 1` is B (the same encoding the slice-header writer uses;
see the parallel pass on slice-type fields). So:

> **Answer to "is the colocated buffer only for temporal direct / B, or does a
> plain P need it too?"** — the **read** side is B-only. The **write** side runs
> on *every* frame including I, which is exactly why enabling it fixed the F12
> row-0 stall on an I-frame ([31](31-bringup-state.md), 2026-09-19 18:44). A
> P-frame therefore needs **no new colocated behaviour at all**: the same
> per-slot write buffer we already publish is enough, and it must be published
> for **every** DPB slot because slot selection rotates. **[C]/[I]**

### 3.2 Layout and required size

| quantity | formula | 1280×720 | VA |
|---|---|---|---|
| record size | `sizeof(aveCommon_AVECoLoData_t)` = **64 B per MB** | — | from `bfrSizeColoData = mbH·mbW·64`, assert text 6873 |
| `bfrSizeColoData` | `pic_height_in_mbs · pic_width_in_mbs · 64` | 45·80·64 = **230 400** | fw `0x53038`–`0x53044`, `lsl #6` `0x54efc` ([60](60-md-recon-stall.md) §3.3 item 6) |
| `coloDataContextOffset` | `ctrl[5176] · ctrl[0xA98] · 64` = `startRow · mbW · 64`, **0 on a whole-frame chunk** | 0 | fw `0x54ee0`–`0x54eec` |
| kext allocation | `AVE_CalcBufSizeOfColocated(codec=0, W, H)` = `align_down(8·W + 0x78, 128) · ceil(H/16)` = **128 B/MB** | 80·128·45 = **460 800** | kext `0xea5560`–`0xea55ac` |
| buffers per session | `AVE_CalcBufNumOfColocated(n, …)` = `flag ? n+1 : 2`, `flag = ((w1==1 && w2>=0) \| w3)` | `max_num_ref_frames + 1` | kext `0xea553c` |
| sets / layers | `AVE_CalcBufSetNumOfColocated(DevType, n) = min(n,4)` when `bit(DevType) & 0x63000`; `AVE_CalcBufLayerNumOfColocated(n) = min(n,2)` | 1 / 1 | kext `0xea54ec`, `0xea5528` |

**[C]** for all of the above. Two things to notice:

1. **The kext allocates exactly twice what the firmware's own size register
   says** (128 B/MB vs 64 B/MB). Our driver already sizes it at 128 B/MB
   (`ave_session.c:253`), so this is safe; do not shrink it.
2. Assert 6873/6874 checks
   `Colocated_L1[0] + coloDataContextOffset − 20·sizeof(aveCommon_AVECoLoData_t)`
   — the reader touches **1280 bytes below** the nominal base. On a whole-frame
   chunk `coloDataContextOffset` is 0, so that is a read below the buffer. It
   only matters for B slices, but if we ever get there, **pad the colocated
   allocation by 20 records (1280 B) at the front**, or make the whole
   allocation 128 B/MB *and* offset the published pointer by 1280.
   **[C]** for the assert, **[I]** for the padding conclusion.

**It is per DPB slot.** `AVE_CHM_SetFwBuf` writes one IOVA per non-null
Colocated surface into `VP + 0xF650 + 0x88·set + 8·slot`
(kext `0xeaef74`–`0xeaefb8`), `ProvideReferenceFrames` copies it to
`ctx + 0x200 + 0x80·set + 8·slot` (fw `0x2b78c`–`0x2b790`), and
`setRefPointers` picks the entry whose recon luma address matches the frame
being coded. **[C]** With `session_dpb = 2` and a rotating slot, **both slots
need one**.

---

## 4. Low-res / downscaled surfaces

**Confirmed, not refuted.** There are three low-res surface kinds and they are
all real for AVC on this part.

### 4.1 What they are

| kind | Start_AVC wire (set 0) | entries | size formula | 1280×720 | kext VA |
|---|---|---:|---|---:|---|
| **LowResRef** — the ½×½ scaled luma of each DPB picture | **`0x2A8 + 8·slot`** (set 1 at `0x330`) | 17 | `ALIGN(4·W, 256) · ((H+63)>>4)`, `ALIGN(…, 512)` | `5120 · 48` = **245 760** | `0xea560c` |
| **LowResResult** — the LRME search results per reference | **`0x3B8 + 8·i`** (set 1 at `0x3F8`) | 8 (count used = **4**, `DevType > 10`) | `ALIGN(4·W, 128) · ceil(H/64) + 1024` | `5120 · 12 + 1024` = **62 464** | `0xea5720`, count `0xea5708` |
| **LowResRCResult** | `0x438 + 8·i` (set 1 at `0x478`) | 8 | `ALIGN(4·W,256) · ceil(H/256)`, **count 0 unless the RC-lowres flag is set** | 0 | `0xea58dc`, count `0xea58c0` |

Per-entry layout is a **bare u64 IOVA at stride 8** — no `{addr,size}` pairs and
no size array anywhere, unlike the entropy table. `AVE_Client_InitFwBuf`
memsets exactly `VP+0x248` for `0x110`, `VP+0x358` for `0x80`, `VP+0x3D8` for
`0x80`, which is the independent confirmation of the extents. **[C]**

**The scale is ½ × ½ linear (¼ area), 8 bits per sample.** The clincher is that
the kext's `lr_stride = ALIGN(4·W, 256)` is byte-for-byte the value the
*firmware* writes into the low-res DMA channels' size register
(`0x40D130010` at fw `0x52498`, `0x40D1205D0 + 0x40i` at fw `0x51a24`), and
`4·W` per MB row is `8 low-res lines × W/2 bytes`, i.e. 64 B per 16×16 MB =
an 8×8 sample block. **[C]** for the arithmetic, **[I]** for the conclusion.

### 4.2 Which engine writes it and which reads it

Write (the downscaler), programmed by `CAVCController::setLRME` fw `0x5144c`:

| AP | value | VA |
|---|---|---|
| `0x40D130000` | control `0x80030001` | `0x524a4` |
| `0x40D13000C` | `PICMGMT + 0xC20` (`sLowResOutput.LowResSrcLumaScaled`) + `(ctrl[5176]>>2)·lr_stride` | `0x523bc`, `0x52488` |
| `0x40D130010` | `lr_stride = ALIGN(4·ctrl[0xA8C], 256)` | `0x52498` |
| `0x40D150134/138/144/148` | scaler ratio words | `0x52504`–`0x52510` |

Read (one channel per reference), same function:

| AP | value | VA |
|---|---|---|
| `0x40D1205CC + 0x40i` | `PICMGMT + 0x778 + 8i` = `sRef.Low_Res_Y_L0[i]` | `0x519e8`, `0x51a20` |
| `0x40D1205D0 + 0x40i` | `lr_stride` | `0x51a24`–`0x51a3c` |
| `0x40D1308C0 + 0x40i` | control `0x80030001` | `0x51a50` |
| `0x40D120F80 + 0x40i` | `RDDMAMESFSRSLTS[i]`, address from `PICMGMT + 0xC28 + 8i` = `sLowResOutput.LowResResults[i]`; enabled per active reference by `setPipe` | `0x53390`–`0x53438` (address/stride/bfrsize/bfrCredit), `0x537d4` (enable) |

**[C]** throughout. The statistics product is a **1 KiB block** read out of
`0x40D15017C` by `CAVCController::processLowResMeStats` (fw `0x4f534`–`0x4f5bc`)
into `ctrl+0x23B7C`. **[C]**

### 4.3 Is it mandatory, and when

| | I-frame | P-frame |
|---|---|---|
| `setLRME` runs | **yes** — nothing on the path reads `PICMGMT+0xCAC` | yes |
| `LowResSrcLumaScaled` (`PICMGMT+0xC20`) write target | **required**, asserted non-zero + 64-aligned (`CAVCController_H13C.cpp:5782/5783`) | required |
| per-reference read loop | **skipped**: bound is `num_ref_idx_l0_active_minus1`, which is `−1` (`tbnz w13,#31,0x51ad4` at fw `0x519e4`) | **runs** |
| `sRef.Low_Res_Y_L0[i]` (`PICMGMT+0x778+8i`) | not read | **asserted** non-zero + 64-aligned, line 5575 |
| `sLowResOutput.LowResResults[i]` (`PICMGMT+0xC28+8i`) | not read | **asserted** non-zero + 64-aligned, lines 6184/6185 (`setPipe`) and again in `setLRME` fw `0x51a44` |

**[C].** So the working I-frame already exercises the low-res *writer* — which
is why `session_lowres` exists in our driver and defaults on, and why the
`LowResRef` table at wire `0x2A8` is already filled. What a P-frame adds is the
**read** side, and that needs the one table nobody has filled:
**`LowResResult` at Start_AVC wire `0x3B8`**.

`ProvideReferenceFrames` copies it: `VP + 0x358 + 0x40·set + 8·j` →
`ctx + 4512 + 0x80·set + 8·j` (fw `0x2b870`–`0x2b95c`, `ldur x16,[x14,#-248]`
with `x14 = VP + 0x450 + 0x80·set`, so the source is `VP + 0x358`), and
`ManageDPBBuffer` then hands it to `refinfo + 296 + 8i` →
`PICMGMT + 0xC28 + 8i` (fw `0x2d5b0`–`0x2d5b4`, `0x2cd94`–`0x2cd98`). **[C]**
The 0x38-byte discrepancy between `ctx+4512` and the `ctx+4568` the
`ProvideReferenceFrames` store lands on for set 0 is **[U]** and does not change
the host-visible conclusion: the table must be on the wire at `0x3B8`.

### 4.4 The other low-res facts worth recording

- `AVE_CalcBufNumOfLowResRef` = `max_num_ref_frames + 1` when references are
  enabled (kext `0xea55d8`) — the same count as the recon slots, which is what
  `ave_cmd.c` already enforces.
- `AVE_CalcBufNumOfLowResResult(DevType) = (DevType > 10) ? 4 : 8`; for
  t6001 / DevType 12 that is **4**. **[C]** (kext `0xea5708`.)
- `LowResRCResult`'s count is `(DevType > 11 && flag) ? 8 : 0` with `flag`
  coming from two client booleans; for a plain fixed-QP session it is **0**, so
  wire `0x438` stays empty. **[C]** arithmetic, **[I]** the flag value.
- `CAVECommonController::load_parameters` (fw `0x24a60`) and
  `get_global_mv_params` (`0x24bd8`) unpack a global-motion estimate out of the
  LRME statistics (26-/25-/18-bit packed accumulators plus eight candidate
  `{s7 dx, s6 dy, u18 weight}` vectors). **Their only callers are
  `CHEVCController::ProcessLRMEDone` / `ProcessLRMEStatsMcore` (fw `0x68544`,
  `0x6859c`, `0x6a3ec`, `0x6a448`) and `CHEVCController::setPipe` for
  `program_async_gmv_params`.** They are **not on the AVC path** and consume no
  host field. **[C]**


---

## 5. Slice-type-dependent wire fields

This section is the boundary with the parallel slice-type pass; the numbers
below were produced by that pass and the ones marked ✓ I re-derived here
independently.

### 5.1 The one field that makes a frame a P

| command | wire | width | field | value | evidence |
|---|---:|---|---|---|---|
| `AVC_ENCODE` (id 7) | **`0xD74`** = PICMGMT `+0xCAC` | s32 | `IMG_FRAME_TYPE` | **1 = P** | ✓ fw `0x48638` (`PipePrepareParam`), `0x145cc`; jump tables at fw `0xcef78` / `0xcef80` |

`IMG_FRAME_TYPE` → slice type, via the two byte jump tables
`AVE_H264_PrepareSliceHeader` (fw `0x20d44`) dispatches through:

| value | slice_type | `nal_ref_idc` / `nal_unit_type` | `frame_num` | QP source |
|---:|---|---|---|---|
| 0 (I, non-IDR) | 2 | 1 / 1 | `++` | Start wire `0xFFB4` (QP_I) |
| **1 (P)** | **0** | `!forceNonRef` / 1 | `++` if the previous picture was a reference | **Start wire `0xFFB8` (QP_P)** |
| 2, 7 (B) | 1 | `!forceNonRef` / 1 | `++` | Start wire `0xFFBC` (QP_B) |
| 3 (IDR) | 2 | 1 / 5 | reset to 0 | QP_I |
| 4, 5, 6 | — | — | — | `"prepareSliceHeaderForFW frametype not recognized"` |

✓ I re-derived the `PipePrepareParam` gate: for `IMG_FRAME_TYPE ∉ {0, 3}` the
firmware **requires** at least one of the eight `PICMGMT + 0x6F8/0x798` luma
addresses to match an entry in the Start-time DPB table at `ctrl+0xCB8`, and
otherwise takes the error path at fw `0x488d4`
(`"Reference buffers are not in recon buffers!"`). **[C]**

### 5.2 Everything else is firmware-generated

`slice_type`, `nal_ref_idc`, `nal_unit_type`, `frame_num`, `idr_pic_id`, POC,
`num_ref_idx_l0/l1_active_minus1`, `ref_pic_list_modification`, the
`pred_weight_table`, the MMCO list, `cabac_init_idc`, `slice_qp_delta` and the
deblocking offsets are **all** produced by `AVE_H264_PrepareSliceHeader` +
`Slice::slice_header` from the frame type, the DPB, the SPS/PPS copies and the
Start-time QP triple. There is no host field for any of them. **[C]**

In particular `num_ref_idx_l0_active_minus1` (`SH+68`) is **derived** as
(count of non-null L0 reference pointers) − 1 (fw `0x21070`–`0x210b8`), and it
is the loop bound for *every* per-reference thing in §1.4, §3 and §4 — which is
exactly why an I-frame (`−1`) never touches any of those asserts and a P-frame
turns them all on at once.

### 5.3 The PPS/SPS gate fields that become live for P

| command | wire | width | field | value for Baseline P | in `ave_abi.h`? |
|---|---:|---|---|---|---|
| Start_AVC | `0x10C78` | u32 | `num_ref_idx_l0_default_active_minus1` | **0** | **no — add** |
| Start_AVC | `0x10C7C` | u32 | `num_ref_idx_l1_default_active_minus1` | 0 | **no — add** |
| Start_AVC | `0x10C80` | u8 | `weighted_pred_flag` | **0** | **no — add** |
| Start_AVC | `0x10C84` | u32 | `weighted_bipred_idc` | 0 | **no — add** |
| Start_AVC | `0x10CB1` | u8 | `constrained_intra_pred_flag` | 0 | yes |
| Start_AVC | `0x10CB2` | u8 | `redundant_pic_cnt_present_flag` | **0** (1 would emit `ue()` from an unwritten SH field) | **no — add** |
| Start_AVC | `0x10CB3` | u8 | `transform_8x8_mode_flag` | 0 (Baseline) | yes |
| Start_AVC | `0x10CB0` | u8 | `deblocking_filter_control_present_flag` | 1 (unchanged) | yes |
| Start_AVC | `0x10C68` | u32 | `entropy_coding_mode_flag` | **0** — CABAC exists in the firmware (a CAVLC-only register program at fw `0x59324` proves both engines) but needs `profile_idc >= 77` and leaves `cabac_init_idc` (`SH+76`) with no located writer | yes |
| Start_AVC | `0x109F0` | u32 | `direct_8x8_inference_flag` | 1 | yes |
| Start_AVC | `0x109DC` | u32 | `max_num_ref_frames` | 1 (try 2 if the DPB refuses — the kext logs `m_ctx.m_num_ref_frame (%d) < 2`) | yes |
| Start_AVC | `0xFFB8` | u32 | **QP_P** | the P QP — a *different* controller slot from QP_I (`ctrl+4972` vs `ctrl+4968`, fw `0x5cec4`) | yes (`ave_cmd.c:411`) |
| Process | `0xA00` = PICMGMT `+0x38` | s32 | `force_key_frame` | **0** on P frames | yes |
| Process | `0xA04` = PICMGMT `+0x3C` | u8 | `force_non_ref` | **0** — 1 gives `nal_ref_idc = 0` *and* stalls `frame_num` | yes |

Deblocking **across slices** has no PPS element in H.264 — it is
`disable_deblocking_filter_idc == 2`, which lives only in `SH+92` and is
firmware-owned with no located writer. **[U]**, and immaterial with one slice.

The `0x984` block at Process wire `+0x40` remains **[U]** with a stronger
negative: the slice geometry the firmware uses comes from the **Start-time**
`sSliceMap` at wire `0xFDAC` (`memcpy(dst+0xCC, AvcInitCmd + 0xFDAC, 0x104)`,
fw `0x14414`), not from the per-frame block. Leave it zero.


---

## 6. What a P-frame must allocate that an I-frame does not

### 6.1 The complete `setPipe` / `setLRME` precondition list

Extracted mechanically: every `bl 0xa56bc` in `setPipe` (`0x52f38`–`0x58fa0`)
with its assert string and `CAVCController_H13C.cpp` line number. **[C]**
"P-only" means the assert is inside a loop bounded by
`num_ref_idx_l0_active_minus1`, which is `−1` on an I-frame.

| line | assert | when | PICMGMT source |
|---:|---|---|---|
| 6133/6134 | `sRef.Y_L0_MSB[me_ref_index] != 0`, `& 63 == 0` | **P-only** | `+0x6F8 + 8i` |
| 6140/6141 | `sRef.Y_L0_LSB[me_ref_index]` | **P-only**, gated on `NEED_LSB_PLANES` | `+0x718 + 8i` |
| 6170/6171 | `sRef.UV_L0_MSB[me_ref_index]` | **P-only** | `+0x738 + 8i` |
| 6158/6159 | `sRef.UV_L0_LSB[me_ref_index]` | **P-only**, LSB-gated | `+0x758 + 8i` |
| 5575 | `sRef.Low_Res_Y_L0[me_ref_index]` (in `setLRME`) | **P-only** | `+0x778 + 8i` |
| **6184/6185** | **`sLowResOutput.LowResResults[me_ref_index] != 0`, `& 63 == 0`** | **P-only** | `+0xC28 + 8i` |
| 6023 | `sLowResOutput.LowResResults[i] & 63 == 0` (the 4-channel setup loop) | always | `+0xC28 + 8i` |
| 6266/6267, 6296/…/6333 | the `_L1` mirrors of all of the above | **B-only** | `+0x798..` , `+0x818 + 8i` |
| 6440/6441, 6454/6455 | `sInput.Y` / `sInput.UV` non-zero, 64-aligned | always | — |
| 6777/6778, 6791/6792, 6805/6806, 6818/6819 | `sRecon.Y_LSB`, `Y_MSB`, `UV_LSB`, `UV_MSB` non-zero and **`& 127 == 0`** | always | `+0x8A0/0x898/0x8B0/0x8A8` |
| 6889/6890 | `encoder_addr_dst_colo + coloDataContextOffset` | always (when enabled) | `+0x8B8` |
| 6860/6861, 6873/6874 | `sRef.Colocated_L1[0] + coloDataContextOffset` (and `− 20·sizeof(AVECoLoData)`) | **B-only** | `+0x838` |
| 6632/6633, 6659/6660, 6697/6698, 6727/6728 | `encoder_addr_fw_data`, `mbAddressCPUFWData`, `stats_DMA_addr` | always | — |
| 6990–6994, 7928/7929 | `encoder_addr_src_nbr_{info,pixels,data}` | always | — |
| 7402 | `EncCommParams.slicePPSIdIdx < AVE_MAX_NUMBER_OF_PPS` | always | — |
| 2649/2655/2656 | `sOutput.Coded` matches the Start table, `CodedBufSize > minBufSize/2` | always | — |

Reproduce the table with the script in §7.

### 6.2 The allocation checklist

`W`, `H` = MB-aligned pixel dimensions; `mbW = W/16`, `mbH = H/16`;
`bpp = ctrl[0x122B]` (8 for 8-bit). Everything is **64-byte** aligned except
the recon planes, which are **128-byte**.

| buffer | per | size | 1280×720 | already in the driver? |
|---|---|---|---:|---|
| Recon `Y_MSB` + `UV_MSB` (contiguous, `UV_MSB = Y_MSB + lumaMsb`) | DPB slot | `lumaMsb = (((W+31)<<5) & ~0x3FF) · bpp/8 · ((H+35)>>5)`; chroma tile-row `= (((W>>s)<<5) + 0x1E0) & ~0x1FF) · bpp/8` | `942 080` + chroma | **yes** (`recon_slot = ALIGN(W·H·2, 4K)` — an over-estimate, keep it) |
| Recon `Y_LSB` + `UV_LSB` (contiguous, `UV_LSB = Y_LSB + lumaLsb`) | DPB slot | `lumaLsb = ALIGN(32 << (64 − clz(tw−1) − clz(th−1)), 128)`, `tw = (W+31)>>5`, `th = (H+35)>>5` | `65 536` + chroma | **yes** (`session_lsb`) |
| **Colocated MV store** | **DPB slot** | `align_down(8·W + 0x78, 128) · ceil(H/16)` = **128 B/MB** (firmware only uses `64 B/MB`) | `460 800` | **yes** (`session_coloc`) — but check it is allocated for **every** slot, not just slot 0 |
| **LowResRef** (½×½ luma) | **DPB slot** | `ALIGN(ALIGN(4·W, 256) · ((H+63)>>4), 512)` | `245 760` | **yes** (`session_lowres`) |
| **`LowResResult`** | **4 buffers, session-wide** | `ALIGN(4·W, 128) · ceil(H/64) + 1024` | **`62 464` each, 249 856 total** | **NO — this is the gap** |
| `LowResRCResult` | 0 for a fixed-QP session | `ALIGN(4·W,256) · ceil(H/256)` | 0 | no, and not needed |
| Entropy / SEB, SrcNeighbor{Info,Pixel,Data,FwData}, CodedData, CodedHeader | session | unchanged | — | yes |

**DPB slot count.** `ProvideReferenceFrames` is called with
`max_num_ref_frames` (Start wire `0x109DC`) and rejects it if it exceeds the
level-derived `MaxDpbFrames` (fw `0x2b550`) or 16 (fw `0x2b5b8`). The recon,
colocated and LowResRef tables must all be filled for
`max_num_ref_frames + 1` slots — that is `AVE_CalcBufNumOfColocated` and
`AVE_CalcBufNumOfLowResRef` agreeing. With `max_num_ref_frames = 1` that is
**2 slots**, which is what `session_dpb` already defaults to. **[C]/[I]**

**Total new allocation for the first IDR+P attempt: 249 856 bytes**
(4 × `LowResResult`), plus making sure the existing per-slot colocated and
LowResRef arenas really do cover both slots.

---

## 7. Reproduce

```sh
cd ~/Projects/apple-ave-driver
M="AVE_MACOS=13.5 python3 tools/disas.py --fw"
K="AVE_MACOS=13.5 python3 tools/disas.py --kext"

# --- the reference surface chain, end to end ---
eval $M --addr 0x2b530 -n 0x300   # ProvideReferenceFrames: wire 0x88 / 0x2A8 / 0xF6B0 -> ctx
eval $M --addr 0x2bcb0 -n 0x180   # InitPointerAndVariables: ctx -> AVECommonDPBEntry +48/+56/+64
eval $M --addr 0x2d4c8 -n 0x180   # ManageDPBBuffer: entry -> ReferenceFrameInfoData
eval $M --addr 0x2c314 -n 0x700   # setRefPointers: refinfo -> PICMGMT 0x6F8.. / 0x898.. / 0xC28..
eval $M --addr 0x2d018 -n 0x1c0   # H264VideoEncoderDPB ctor: [dpb+20] / [dpb+24] / [dpb+4704]

# --- the read datapath ---
eval $M --addr 0x534a0 -n 0x420   # setPipe's reference loop: 0x40D128000 + 0x40i
eval $M --addr 0x53390 -n 0xb0    # RDDMAMESFSRSLTS: the 4 low-res result channels
eval $M --addr 0x48400 -n 0x270   # PipePrepareParam: the ctrl+0xCB8 match and the 0xCAC gate

# --- motion estimation ---
eval $M --addr 0x61c34 -n 0x90    # setupMeSetupConfig -> 0x40D190004..018, 0x104
eval $M --addr 0x61cbc -n 0x340   # setupMeCGenConfig  -> the cost ladder
eval $M --addr 0x5cdc8 -n 0x180   # InitEncodingParameters: VP+0xF760 block -> ctrl / EncCommParams
eval $M --addr 0x4e6a0 -n 0x60    # CAVCController::DebugInit: search_range / skip mode names
eval $M --addr 0x53140 -n 0x90    # search_range -> 0x40D110000 bits 12..14
eval $M --addr 0x57c00 -n 0x60    # the wire-0xFCE9 gate: setupMe* vs setLRME

# --- colocated ---
eval $M --addr 0x54ec0 -n 0x80    # the B-only colocated READER and its two asserts
eval $M --addr 0x551e4 -n 0x40    # 0x40D120BCC / 0x40D120BD0
eval $M --addr 0x554e0 -n 0xe0    # the colocated WRITER (docs/60 3.3)
eval $K --addr 0xfffffe0008ea54ec -n 0xc4   # CalcBuf{SetNum,LayerNum,Num,Size}OfColocated

# --- low res ---
eval $K --addr 0xfffffe0008ea55b0 -n 0x140  # CalcBuf*OfLowResRef / LowResResult
eval $M --addr 0x5144c -n 0x1200  # setLRME: writer 0x40D130000, readers 0x40D1205CC+0x40i

# --- every setPipe assert, with its line number ---
python3 - <<'EOF'
import re,subprocess
d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
s=subprocess.run(['python3','tools/disas.py','--fw','--addr','0x52f38','-n','0x6070'],
                 capture_output=True,text=True,env={'AVE_MACOS':'13.5','PATH':'/usr/bin'}).stdout.splitlines()
for i,ln in enumerate(s):
    if 'bl\t0xa56bc' in ln:
        m=re.search(r'adr\tx8, (0x[0-9a-f]+)',s[i+1])
        n=next((re.search(r'w2, #(0x[0-9a-f]+)',x) for x in s[i+1:i+14]
                if re.search(r'w2, #0x',x)),None)
        if m:
            o=int(m.group(1),0)+0x4000
            print(n.group(1) if n else '?', d[o:d.index(b'\0',o)].decode())
EOF
```

---

## 8. Annotations to earlier documents

Per [00](00-methodology.md), these annotate; nothing is retracted.

1. **[53](53-first-frame.md) §11.3** — `ctrl+0x23FEC` ("async LRME") recorded as
   firmware-internal with no host writer. It has one:
   `InitEncodingParameters` fw `0x5cfcc`–`0x5cfd0`, from
   `AVE_VIDEO_PARAMS` byte VP `0xFC89` = **Start_AVC wire `0xFCE9`**. Invisible
   to an immediate scan because the base is pre-biased to `ctrl+0x23FC4` and
   the offset is `+40` — Trap 3 in its documented form. **[C]**
   [59](59-row1-stall.md) §4's **[I]** guess of `0xFCE9` was right and can be
   upgraded to **[C]**.
2. **[60](60-md-recon-stall.md) §3.3** — `[dpb+4704]` is now pinned: the
   `H264VideoEncoderDPB` constructor sets it to `dpb + 0x38`
   (`add x8,x0,#0x38 ; str x8,[x0,#4704]`, fw `0x2d1a8`–`0x2d1ac`). **[C]**
3. **[60](60-md-recon-stall.md) §3.4** — "LowResResults (ME SFS readers) …
   an I-frame does no ME search" is right for the I case and is now the
   *reason* a P-frame needs the table: the readers are bounded by
   `num_ref_idx_l0_active_minus1`. §4.3. **[C]**
4. **[62](62-kext-field-map.md) §3 item 8** — "LowResResult / LowResRCResult …
   leave 0" is correct for an I-frame and **wrong for a P-frame**:
   `LowResResult` (wire `0x3B8`) is asserted non-zero per reference at
   `CAVCController_H13C.cpp:6184`. `LowResRCResult` (wire `0x438`) stands at 0.
   **[C]**
5. **[62](62-kext-field-map.md) §1.3** — the LowRes rows' "2 × 2 × 17" /
   "2 × 2 × 8" shapes are loop trip counts; the outer index is a *type*, and
   type 1 goes to the high `0xFB40`/`0xFB68`/`0xFBE8` region. Each low table
   therefore holds **2 sets**: `0x110` / `0x80` / `0x80` bytes, matching
   `AVE_Client_InitFwBuf` exactly. **[C]**
6. **[62](62-kext-field-map.md) §1.1** — `AVC_INIT` wire `0x14`
   ("unknown ← `client[212]`") is the **`_E_AVE_ClientType`**: `AVE_Client_Init`
   stores its `ClientType` argument at `client+212` (kext `0xecd948`), and the
   same word is arg `x26[212]` to the surface-size helpers. **[C]** for the
   identity. An ordinary encode client is type 1 and 0 is not a legal value
   (`AVE_DevCap_FindSEntry` rejects it, [17](17-aux-engines-pools.md) §1), so
   this is a one-word change worth making. **[I]** for the value.
7. **[47](47-abi-13.5-frame-rc-surfaces.md) §4** — the `_S_AVE_SurfaceSet`
   ordering gains three contiguous anchors: LowResRef `+0x9A8` (`0x220`),
   LowResResult `+0xBC8` (`0x100`), LowResRCResult `+0xCC8` (`0x100`).
   **[C]** (kext `0xeaefc0`, `0xeaf048`, `0xeaf0d8`.)
8. **New** — the `PICMGMT + 0x6F8 .. 0x838` region is Apple's `sRef` struct and
   its field names are recoverable verbatim from `setPipe`'s assert strings.
   The full map is §1.2.
