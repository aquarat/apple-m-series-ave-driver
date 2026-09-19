# What a P-frame needs: reference surfaces, motion estimation, colocated MVs, low-res

**Status: in progress.** Static analysis only — nothing here was run on hardware.

Scope split, deliberately: [64](64-.md) owns *which DPB slot and which field value
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
| `0x898` | `refinfo+72` | **current recon** luma tile data | `0x2c338` |
| `0x8A0` | `refinfo+216` | current recon luma metadata | `0x2c328` |
| `0x8A8` | `refinfo+72 + [dpb+20]` | current recon chroma tile data | `0x2c32c` |
| `0x8B0` | `refinfo+216 + [dpb+24]` | current recon chroma metadata | `0x2c33c` |
| `0x8B8` | DPB colocated `ctx+0x200+8·slot` for the matched slot | current colocated MV **write** target | `0x2c4b0`, `0x2c5f8`, `0x2c784` |
| `0xC20` | `refinfo+224` | current LowResRef | `0x2c320` |
| **`0x6F8 + 8i`** | `refinfo+8+8i` | **ref *i*** luma tile data | `0x2c78c` (i=0) … `0x2c94c` (i=7) |
| **`0x718 + 8i`** | `refinfo+152+8i` | ref *i* luma metadata | `0x2c7a4` … |
| **`0x738 + 8i`** | `refinfo+8+8i + [dpb+20]` | ref *i* chroma tile data | `0x2c79c` … |
| **`0x758 + 8i`** | `refinfo+152+8i + [dpb+24]` | ref *i* chroma metadata | `0x2c7bc` … |
| **`0x778 + 8i`** | `refinfo+232+8i` | ref *i* LowResRef | `0x2c7c4` … |
| `0xC28 + 8i` | `ctx + 0x11A0 + 64·[ctx+4640] + 8i` | ref *i* colocated MV **read** source | `0x2cd98` … |
| `0xC68 + 8i` | `ctx + 0x600 + 64·[ctx+4640] + 8i` | ref *i*, second array **[U]** | `0x2cd9c` … |

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
2d170  str w8, [dpb+20]                    ; LUMA TILE-PLANE BYTES
2d174  tw = ((w+31)>>5) - 1 ; th = w11 - 1
2d180  w8 = 32 << (64 - clz(tw) - clz(th)) ; = 32 * npot(tw+1) * npot(th+1)
2d1a0  w8 = (w8 + 0x7F) & ~0x7F
2d1a4  str w8, [dpb+24]                    ; LUMA METADATA BYTES
```

**[C].** For 1280×720, bpp byte = 8:
`[dpb+20]` = `((1311<<5) & ~0x3FF) * 8 >> 3 * 23` = `40960 * 23` = **942 080**,
and the 40 960 tile-row figure is exactly the `+0x10 = 40960` already observed
in the recon-luma write channel ([60](60-md-recon-stall.md) §3.2) — that is the
cross-check. `[dpb+24]` = `32 << (6+5)` = **65 536**. **[C]** for the
arithmetic, **[I]** that `[dpb+24]` is a metadata/header plane (from its
`32 B × npot(tiles)` shape and from its use as the chroma-metadata bias).

So a DPB slot's surface must be laid out as

```
+0                       luma tile data      942080 B   <- table word0 points here
+942080                  chroma tile data    (half-width tiles)
   (separate allocation)  luma metadata      65536 B    <- table word1 points here
+65536                    chroma metadata
```

**[I]**, from the two derivations above plus the read-channel programming in
§1.4. Note the driver's `ave_abi.h` naming (`recon_y` / `recon_uv` /
`recon_y_lsb` / `recon_uv_lsb` at PICMGMT `0x898`/`0x8a8`/`0x8a0`/`0x8b0`)
matches this exactly if "LSB" is read as "metadata plane": `0x898` and `0x8a8`
are the two *data* planes, `0x8a0` and `0x8b0` the two *metadata* planes.
Those four fields are inert (§1.2) but the names are right.

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

