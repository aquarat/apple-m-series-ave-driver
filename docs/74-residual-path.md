# The residual path: where the coefficients go to zero

Static analysis of the macOS 13.5 firmware (`ave_h13c.bin`), its seven MCPU
images, the 13.5 userspace encoder (`userspace/AVE.bin`, AppleVideoEncoder),
and the driver, against f42–f44 in [53](53-first-frame.md) (end). **Nothing
here was run on hardware.**

Conventions as [70](70-intraest.md) and [73](73-modedec-costs.md): firmware
VAs are 13.5 image VAs (file offset = VA + `0x4000`); **wire** = byte offset
in the command on IPC channel 1; VP + `0x60` = wire; firmware MMIO offset `X`
is AP `0x40C000000 + X`; the MCPUs see it at `0x40000000 + X`; the driver's
DPE bank offset is AP − `0x40D100000`. `ctrl` = the `CAVCController` object.
Userspace VAs are AVE.bin VAs (`__TEXT` file offset = VA). Labels per
[00](00-methodology.md): **[C]** read from an instruction (VA cited), **[I]**
inferred with the chain stated, **[U]** unknown.

```sh
python3 tools/scaling_regs.py            # the model below, 28 instruction anchors
python3 tools/mmio_map.py --check        # whole-image MMIO map, with its controls
```

---

## 0. Verdicts

| # | question | verdict | label |
|---:|---|---|---|
| 1 | Why are there no coefficients at any QP? | **The quantiser's per-coefficient scale is zero.** The firmware builds every quantiser scaling-list register from the SPS scaling lists in Start_AVC as `((0x10000 / w) << 16 \| w) & 0x3FFF00FF` (fw `0x5fb9c`–`0x5fbbc`) and writes them every frame (setPipe `0x576b0`–`0x57814`). **Our driver sends every list as 0** (`ave_cmd_begin` memsets the command, `driver/ave_cmd.c:125`, and `ave_cmd_build_start_avc` writes no list among its SPS fields, `:583`–`:605`). A64 `udiv` by zero returns 0, so all 560 registers are **0**: IntraEst `0x40D24A088..1C4`, ReconLuma `0x40D28A0A0..31C`, ReconChroma `0x40D2AA0A4..5A0`. **Apple's userspace always fills the lists with 16** when no scaling matrix is in use (AVE.bin `0x2d038`–`0x2d098`). | [C] firmware and driver; [C] userspace code; [I] that the hardware multiplies by the high half |
| 2 | Does that explain the whole symptom? | **Yes, every part of it**: zero levels at any QP (f44), I_PCM exact (f43; PCM is not quantised), recon = prediction = 128 everywhere, and the same 2709 bytes for a ramp and a flat plane (every MB's syntax is identical). §1.6. | [I] |
| 3 | Where do the residual stages get the original pixels? | **Nowhere the firmware can configure.** The only pixel source any code programs is the 2D source reader `0x40D120000`/`+0x80`. No register in the ReconLuma, ReconChroma, `0x127A`/`0x129A`, or Deblock block is an address. Every MCPU touches only its own stage block. I_PCM and the residual path draw on the same hardware MB pipeline. f43 shows that pipeline carries the true pixels to the entropy coder. | [C] negative over every direct and table-driven write; [I] "same pipeline" |
| 4 | What does IntraEst contribute? | Per MB, the MCPU programs QP/λ and intra sub-mode enables. With mode word 0 it programs nothing and only kicks go. The IntraEst *hardware* block also holds its own copy of the intra-luma scaling list (`0x40D24A088..1C4`), which is **also 0**, and it uses that list for its cost estimate. **An idle IntraEst cannot by itself zero the coefficients.** ReconLuma's quantiser gets its QP, λ and lists from setPipe, whatever IntraEst does. | [C] for the writes; [I] for the use |
| 5 | Other residual-path settings | Rounding offsets are sane defaults (intra `0x2AA`, inter `0x156`). Coefficient cancellation is disabled (enable bit 0), the same as Apple's. `mode_8x8_transform` is 0 (Baseline). **RECONL/RECONC SKIPMODE is 0; Apple sends 3 (both on).** MCPU mode words are 0 for every stage (one shared pair of variables), so each MCPU leaves setPipe's values in place. §4. | [C] |
| 6 | Side results | `0x40D2EA00C` is `AVC_TRANSCODECONFIGGLOBALS_CONTEXTHEIGHT`, which closes docs/70 §7's `[U]`. `0x40D2BA000` is the deblocking block. `0x127A`, `0x129A` and `0x12BA` are stages with no MCPU, started with go = 3 at setPipe `0x57e24`–`0x57e38`. `0x40D120004/08/84/88` are written by `SetTunable`'s table (`0xcf630`). They are not read-only descriptors, which corrects docs/69 §1.2. §2.1, §2.2, §6. | [C] |

**The next run to make is §5 R1+R2**: add eleven read-only register reads in
two pages the driver already reads every run, and send flat-16 lists. The
reads can come back "no" on their own. The lists can come back "no" as a
bitstream.

---

## 1. The scaling-list chain

### 1.1 The lists live in the SPS block the host sends

`InitEncodingParameters` copies the host SPS into the controller:

```
5ce74  add x1, x9, #0x550        ; x9 = VP + 0x10000  -> VP+0x10550 = wire 0x105B0
5ce7c  add x0, x9, #0x1fc        ; x9 = ctrl + 0x2C000 -> ctrl+0x2C1FC
5ce80  mov w2, #0x6ac
5ce90  bl  memcpy
```

**[C].** This is the block the driver already fills (`sps_block = 0x105b0`,
`driver/ave_abi.h:1655`). Inside it, `AVC_SPS::seq_parameter_set_rbsp` uses the
standard H.264 scaling fields at these offsets (x8 = SPS):

| SPS + | field | fw VA |
|---:|---|---|
| `0x39` u8 | `qpprime_y_zero_transform_bypass_flag` | `0x195f8` |
| `0x40` u8 | `seq_scaling_matrix_present_flag` | `0x19608` |
| `0x41+i` u8 | `seq_scaling_list_present_flag[i]` | `0x19674` |
| `0x4D+i` / `0x53+i−6` u8 | use-default flags, 4×4 / 8×8 | `0x196a0` / `0x19638` |
| **`0x5A + 32·i`** | **4×4 list `i` (0..5), u16[16], zig-zag order** | **`0x196b0` `add x26,x8,#0x5a`** |
| **`0x11A + 128·(i−6)`** | **8×8 list `i` (6..11), u16[64]** | **`0x1973c` `add x26,x8,#0x11a`** |

**[C].** The field that follows is `log2_max_frame_num_minus4` at SPS + `0x41C`,
which the driver writes at wire `0x109CC` = `0x105B0 + 0x41C`
(`driver/ave_abi.h:1668`). That confirms the region ends where the H.264
layout says it does. **[C]**

So the lists sit at **wire `0x1060A + 32·i`** (4×4, `i` = 0..5:
Intra Y, Intra Cb, Intra Cr, Inter Y, Inter Cb, Inter Cr) and **wire
`0x106CA + 128·j`** (8×8, `j` = 0..5: Intra Y, Inter Y, Intra Cb, Inter Cb,
Intra Cr, Inter Cr).

The SPS writer emits these fields only for the High-class profiles
(`0x195a0` `tst x10,x11`, `0x197b0`/`0x197b8` `cmp #0xf4`/`#0x2c`). For our
Baseline stream it skips straight to `0x197c0`, so **the lists never reach the
bitstream**. They exist only to program the quantiser. **[C]**

### 1.2 `InitScalingListRegs` (fw `0x5fb64`)

It is called once, unconditionally, from `InitEncodingParameters`
(`0x5e154`, straight-line after `0x5e13c`/`0x5e14c`). It is the only caller
(`bl 0x5fb64` scan). **[C]**

```
5fb64  w8  = 0x12AA0A4 (0x5fb84)   ReconChroma
5fb68  w9  = 0x128A0A0 (0x5fb88)   ReconLuma
5fb6c  w10 = 0x124A088 (0x5fb8c)   IntraEst
5fb70  x12 = ctrl+0x2F010          (offset,value) pair table
5fb94  x13 = ctrl+0x2C2B6          = SPS+0xBA; lists read at x13-96..x13+64
5fb9c  w15 = 0x10000 ; 5fba0 w16 = 0x3FFF00FF
loop k = 0..15 (zig-zag index; raster idx from the table at 0xd84b4):
5fbb4  w3 = 0x10000 / w            ; udiv
5fbb8  bfi w2, w3, #16, #16        ; w | recip << 16
5fbbc  and w2, w2, w16             ; & 0x3FFF00FF
       pairs: (0x124A088+idx, L0) (0x128A0A0+idx, L0) (0x128A0E0+idx, L3)
              (0x12AA0A4+idx, L1) (0x12AA0E4+idx, L2) (0x12AA124+idx, L4)
              (0x12AA164+idx, L5)
second loop 0x5fcc4-0x5fdc8: the six 8x8 lists into
       0x124A0C8.., 0x128A120.., 0x128A220.., 0x12AA1A4/2A4/3A4/4A4..
```

**[C]** for every constant and the arithmetic. The table at `0xd84b4` is a
permutation of 0..15 in `(row, col)` form, which makes it the zig-zag scan
(checked by `tools/scaling_regs.py`). The value layout is `weight` in bits
0..7 and `0x10000/weight` in bits 16..29. For the flat list H.264 uses when no
matrix is signalled (16), that is **`0x10000010`**. For a zero list, AArch64
`UDIV` by zero returns 0 (architectural; there is no trap option), and the
register is **`0x00000000`**.

The 8×8 loop pairs one list's weight with another list's reciprocal for three
of the chroma tables (`0x5fd30`, `0x5fd58`, `0x5fd5c`). `tools/scaling_regs.py`
models this instruction by instruction. It is invisible with uniform lists and
irrelevant to 4:2:0 without 8×8 transforms. **[C]** code, **[U]** intent.

### 1.3 setPipe writes the pairs every frame

```
57698  x9 = 0x2F000 ; x10 = x9|0x80 ; x11 = ctrl + x10 ...   (4x4 table)
576b8  ldp w12, w13, [x11, #8] ; 576bc str w13, [x8, x12]   x8 = MMIO base
...    7 sub-tables x 16 pairs, loop to 0x57754
57758  x10 = ctrl + 0x2F580 ; 8x8 table, 7 x 64 pairs, loop to 0x57814
```

**[C].** There are no branches into or out of `0x576ac`–`0x57818` except the
two back-edges. The block is straight-line from the ModeDec record writes that
f42 read back exactly as [73](73-modedec-costs.md) predicted. **[C]** for the
control flow, **[I]** strong that it ran on our frames.

This is table-driven MMIO, the Trap-3 case. No immediate scan finds these
registers, which is why [70](70-intraest.md) §1.3's map listed six IntraEst
accesses and not the other 80.

### 1.4 What Apple sends

AppleVideoEncoder's `AVE_PrepareSequenceHeader` (log string `0xd5a69`,
`0x2cd34`) fills the same structure. `x23 = S + 0x10AA8`, so the SPS base is
`S + 0x10AD0`. It switches on the "scaling_matrix" mode at SPS + `0x3C`
(`0x2cd6c` `ldr w8,[x23,#0x64]`; the same field is logged as
`SPSparams.scaling_matrix` at `0x2e4b4`):

```
2cd70  sub w16, w8, #1 ; cmp w16, #6 ; b.hi 0x2d038     ; mode 0 and >7 -> flat
jump table 0x2da84: modes 1..7 -> 0x2cd9c 0x2cfac 0x2d038 0x2d038 0x2d0a0 0x2d15c 0x2d218
2d038  strb wzr, [x23, #0x68]           ; SPS+0x40 seq_scaling_matrix_present_flag = 0
2d05c  x0 = SPS+0x5A + 32*i ; w2 = 0x20  ; bl memset_pattern16(pattern 0x115ff0)
2d07c  x0 = SPS+0x11A+128*i ; w2 = 0x80  ; bl memset_pattern16
       and clears SPS+0x41+i, +0x47+i, +0x4D+i, +0x53+i         (i = 0..5)
```

**[C]** for every instruction. The pattern at `0x115ff0` is eight u16 `16`s.
**So macOS sends every 4×4 and 8×8 list as flat 16, with the present-flag 0.**
The stub at `0xbf110` is named `memset_pattern16` only by the symbolised
disassembly **[I]**. The arguments make it a 16-byte pattern fill either way.

This is the block that reaches the firmware. [72](72-userspace-video-params.md)
§1.2–1.3 shows that `S+0x10AD0` is descriptor slot [5], the `0x6AC`-byte SPS
block that `AppleAVEVA_DriverInit` copies verbatim to the kext (`pInfo+0x10A20`).
**[C]** there. Six offsets also agree between this writer, the firmware's SPS
writer (§1.1) and `InitScalingListRegs`. **[C]** for each offset. The kext's
hop to wire `0x105B0` was not re-traced here. It is the block the firmware
copies at `0x5ce68`, and the driver's SPS fields at those offsets work. **[I]**

### 1.5 What we send

`ave_cmd_begin` memsets the whole command (`driver/ave_cmd.c:125`), and
`ave_cmd_build_start_avc` then writes the SPS fields at `:583`–`:605` (line numbers as of this writing). None of them is a list, a list
flag or the mode. **Every list is zero.** **[C]**

`tools/scaling_regs.py` prints the result:

```
SPS lists = zero
  IntraEst 4x4 intra Y       AP 0x40d24a088..0x40d24a0c4  all 0x00000000
  IntraEst 8x8 intra Y       AP 0x40d24a0c8..0x40d24a1c4  all 0x00000000
  ReconLuma 4x4 intra Y      AP 0x40d28a0a0..0x40d28a0dc  all 0x00000000
  ReconLuma 4x4 inter Y      AP 0x40d28a0e0..0x40d28a11c  all 0x00000000
  ReconLuma 8x8 intra/inter  AP 0x40d28a120..0x40d28a31c  all 0x00000000
  ReconChroma (all 8 lists)  AP 0x40d2aa0a4..0x40d2aa5a0  all 0x00000000
--lists flat: every one of these 0x10000010
```

### 1.6 Why this is the whole symptom

| observation | with a zero quantiser scale |
|---|---|
| no coefficients at QP 30 **or QP 10** (f44) | level = coefficient × 0: zero at every QP, including every DC level |
| every MB `I_16x16`, cbp 0 | IntraEst's own list is zero too, so its cost estimate sees free zero residual for every mode. `I_16x16` with cbp 0 has the shortest header. |
| decodes to 128 everywhere | the first MB has no neighbours, so it predicts 128 and reconstructs 128. Every later prediction is built from 128s. |
| 2709 bytes for a ramp and for flat 200 (F17/F18) | every prediction is flat 128, so every mode has identical distortion. The tie-break, and so the syntax, does not depend on the source. |
| I_PCM codes the source exactly (f43) | PCM samples bypass transform and quantiser |
| neighbour Pixel[0] fully written | the recon/neighbour writers run and write 128s |

**[I]**, each row. What is still inferred is the hardware half: that the stage
multiplies by the register rather than ignoring it when some other bit is
clear. No firmware writer of an "enable weighted quant" bit exists. The only
reader of `seq_scaling_matrix_present_flag` found is the SPS writer (§1.1).
The firmware programs these 560 registers unconditionally, so the hardware has
nothing else to go on. **[I]**, with the Trap-3 caveat, and §5 R1 tests it.

---

## 2. Question 1: where the residual stages get original pixels

### 2.1 The stage blocks, named

From the whole-image map (`tools/mmio_map.py`, direct and table-driven writes)
plus the firmware's own register-name strings:

| fw block | name / evidence | ASC writes on our path |
|---|---|---|
| `0x1120000`/`+0x80`/`+0x100` | source picture readers (luma, chroma, third) — [69](69-source-to-modedec.md) §1 | address, stride, config |
| `0x1242000`/`0x1243000`/`0x124A000` | IntraEst hif / ctx / stage | QPY, nQuant, per-QP words, **4×4+8×8 intra-Y list `0x124A088..1C4`**, rounding `0x124A234..23C`, cancel `0x124A240..` |
| `0x1262000`/`0x126A000` | ModeDecision | λ, 25-word candidate record ([73](73-modedec-costs.md)) |
| `0x127A000` | no MCPU, no name | enable `0x127A098 = 1` (`0x60350`), go `0x127A080 = 3` (`0x57e24`) |
| `0x1282000`/`0x128A000` | ReconLuma | `0x088` mode_8x8 & 3 (`0x5782c`), `0x08C` **`AVC_RECONLCONFIG_SKIPMODE`** (`0x5783c`, string `0xc6b2a`), `0x090` QPY, `0x094` nQuant (`0x55eb4`/`0x55ebc`), **lists `0x0A0..0x31C`**, rounding `0x320..0x330`, cancel `0x334..0x348` |
| `0x129A000` | no MCPU, no name | enable `0x129A0D8`, go `0x129A080 = 3` (`0x57e30`) |
| `0x12A2000`/`0x12AA000` | ReconChroma | `0x08C` **`AVC_RECONCCONFIG_SKIPMODE`** (`0x57874`, string `0xc6b53`), **lists `0x0A4..0x5A0`**, rounding `0x5A4..0x5BC`, cancel `0x5C0..` |
| `0x12BA000` | **Deblock** (`"AVC PIPE:: DeblockControl.all %x"`, `0xc6b7c`) | `0x090 = 1`, `0x094` = deblock idc/α/β (`0x57b1c`–`0x57b38`), go `0x080 = 3` (`0x57e38`) |
| `0x12C8000`/`0x12DA000` | CAVLC | QPY into `0x08C..0x098` (`0x55e9c`–`0x55ea8`), `0x088` (`0x4f3d4`) |
| `0x12EA000` | **Transcode** (`AVC_TRANSCODECONFIGGLOBALS_CONTEXTHEIGHT`, `0xc6c51`, `adr` at `0x584e0`, store `0x58534` → `0x12EA00C`) | entropy/bitstream globals |

**[C]** for every address, VA and string. The `0x127A`/`0x129A` roles are
**[U]**. Their position in the chain (between ModeDec and ReconLuma, between
ReconLuma and ReconChroma) suggests prediction or transform helpers, but that
is ordering only.

### 2.2 No original-pixel source is configurable

- **ASC firmware.** None of the addresses above is a DMA address. Every
  firmware-programmed address on the pipe is in a `0x112xxxx` reader or a
  `0x113xxxx` writer ([69](69-source-to-modedec.md) §2.1). The only reader fed
  a picture is the source reader. **[C]**, over the direct map (3093 accesses,
  controls pass) plus every table-driven block now enumerated: scaling lists
  (`ctrl+0x2F010`/`0x2F390`), rounding (`ctrl+0x2EF18`/`0x2EF90`, from
  `SetRoundingOffsetRegs` `0x5fa8c`–`0x5fb48`), cancellation
  (`ctrl+0x2E728..`, `InitCoeffCancelCostRegs` `0x5f218`), the ModeDec record
  ([73](73-modedec-costs.md)). The firmware's third table-driven writer,
  `CAVECommonController::SetTunable` (`0x277a8`), is called from
  `InitEncodingParameters` at `0x5e858`. It applies 232 `{offset − 0x1100000,
  width, clear, set}` entries from `0xcf630` (loop to `cmp x10,#0xe80`,
  `0x278f4`), plus a second table at `0xd04c0` based at `0x1800000`. **None
  of the 232 lands in `0x1240000..0x12FFFFF`.** They hit `0x111`, `0x112`
  (140), `0x113` (85), `0x115`, `0x117` and `0x119`. **[C]**, by decoding the
  table.
- **MCPUs.** ReconLuma's image (`0xe4480`) writes only `0x4128xxxx`: its hif,
  ctx `0x41283xxx`, stats `0x41284xxx`, and stage words `0x4128a080..a094`,
  `0x4128a320..a340`. ReconChroma's (`0xe4a90`) writes only `0x412axxxx`.
  **[C]**, movw/movt scan of both images, which finds each image's known hif
  base as its control.
- **So** the residual stages take the original MB from the same hardware
  pipeline that delivers PCM samples. No firmware- or host-visible buffer is
  filled only in some mode. **[I]**. The internal bus is invisible to static
  analysis. **This is no longer the question.** §1 explains the symptom
  without any pixel-path fault, and f43 shows that pipeline carries our exact
  pixels to at least the entropy stage.

---

## 3. Question 2: what IntraEst contributes

- **MCPU** ([70](70-intraest.md) §1.2, [73](73-modedec-costs.md) §5.2): per
  MB it programs QPY/λ (`0x4124A1C8/1CC`) and the three intra sub-mode words
  (`0x4124A1D0..1D8`), gated by DMem `0x10000000`/`0x10000764`. It then
  writes go `0x4124A080 = 1`. With both mode words 0 (f40–f44), it programs
  nothing and still writes go. **[C]**
- **Hardware stage**: holds QPY/λ, the sub-mode enables, **the intra-Y 4×4 and
  8×8 scaling lists `0x124A088..0x124A1C4`** (contiguous up to QPY at
  `0x1C8`), rounding `0x234..0x23C` and cancellation `0x240..`. That is the
  register set of a rate–distortion estimator that quantises its own trial
  residuals. **[C]** registers, **[I]** use.
- **If IntraEst processed nothing** (curMB 0), ModeDec would still choose among
  its enabled candidates. Only intra is enabled, `0x40D26A0A4` bit 0 (f42).
  ReconLuma would still quantise `original − prediction` with setPipe's QPY,
  λ and lists, because nothing IntraEst writes reaches `0x128A000`. With
  working lists, a +72 DC residual at QP 10 gives non-zero levels whatever
  mode was chosen. **So an idle IntraEst cannot produce "cbp 0 at every QP"
  alone. The zero lists can.** **[I]**
- **curMB.** f44 reads `0x40D243184 = 0x58de4767`. That is ≥ `0x10000000`, so
  the handler's "is there work" test (`intraest:0x20a`) passes on the last MB
  even though `0x40D243180 = 0`. The semantics of `+0x180` for IntraEst remain
  **[U]**. ModeDec's and ReconLuma's `+0x000` equal their `+0x180` (f44: both
  `93638402`, both `fffafdc1`), so the matching read, IntraEst `0x40D243000`,
  is the cheap control (§5 R5).

---

## 4. Question 3: every residual-path setting

What the firmware writes on our path, the host field feeding it, and what we
and Apple send. "Apple" values are the userspace defaults
(`AVE_SetEncoderDefault`, AVE.bin; `x8 = S + 0x104E5` at `0x29920`;
wire = S − `0x800`, [72](72-userspace-video-params.md) §1.3). Where
[72](72-userspace-video-params.md) §5 records a later Baseline override, its
value is used.

| register (AP) | written at | host field | ours | Apple | effect on our frame | label |
|---|---|---|---|---|---|---|
| **scaling lists** IntraEst `0x40D24A088..1C4`, ReconLuma `0x40D28A0A0..31C`, ReconChroma `0x40D2AA0A4..5A0` | ISLR `0x5fb64`, setPipe `0x576b0`–`0x57814` | SPS lists, wire `0x1060A` (6×16 u16), `0x106CA` (6×64 u16) | **0 → registers 0** | **16 → `0x10000010`** (`0x2d038`) | **zero quantiser scale** | [C]/[C]/[C] |
| QPY ReconLuma `0x40D28A090`, CAVLC `0x40D2DA08C..098` | `0x55eb4`, `0x55e9c`–`0x55ea8` | `qp_i` wire `0xFFB4` | 30 (10 in f44) | — | correct (read back on IntraEst, f42/f44) | [C] |
| nQuant ReconLuma `0x40D28A094` | `0x55ebc` | from QPY via table `0xd8414` | `0x80` | — | correct | [C] |
| mode_8x8 `0x40D28A088` | `0x5782c` (`ctrl+0xAB0 & 3`) | wire `0xFCEC` | 0 | 2, then 0 for Baseline ([72](72-userspace-video-params.md) §5, `0x2b1d8`) | no 8×8 transform. Same as Apple. | [C] |
| **RECONL SKIPMODE** `0x40D28A08C` | `0x5783c` ← `ctrl+0x142C` = `0xFCF0` bit 0 (`0x5e12c`/`0x5e134`) | wire `0xFCF0` u16 | **0** | **3** (`0x29b04`/`0x29b08`) | **[U]** meaning. Also feeds cancellation when `0xFCF2 == 0xFFFF` | [C] |
| **RECONC SKIPMODE** `0x40D2AA08C` | `0x57874` ← `ctrl+0x1430` = bit 1 (`0x5e130`/`0x5e138`) | same | **0** | **3** | as above | [C] |
| cancellation enables IntraEst `0x40D24A240/244`, ReconLuma `0x40D28A334..348`, ReconChroma `0x40D2AA5C0..5D4` | `InitCoeffCancelCostRegs` `0x5f218`; written at setPipe `0x57928`–`0x579d0` (base `[sp,#128] = ctrl+0x2C200`, `0x52f58`/`0x53248`) | `qcoeff_cancel` `0xFCF4` (enable bit); `0xFCF2` (0xFFFF → derive from SKIPMODE); arm select `ctrl+0x23FD0` ← wire `0xFF77` (`0x5d19c`/`0x5d1a0`, tested at `0x5f238`) | 0 / 0 / 0 | 0 (`0x29b0c`) / 0 (upper half of the u32 at `0x29b08`) / no writer in [72](72-userspace-video-params.md)'s map | enable bit 0 = off. **Identical to Apple's.** | [C]; Apple `0xFF77` [I] |
| cancellation cost tables `0x124A248..`, `0x128A34C..`, `0x12AA5D8..` (4×4 and 8×8) | `0x5f388`–`0x5f690`, setPipe `0x579d4`–`0x57afc` | none: byte tables `0xd8874`, `0xd87f4`, `0xd8894` (values 3,2,1,0) | — | — | firmware constants | [C] |
| rounding IntraEst `0x40D24A234..23C`, ReconLuma `0x40D28A320..330`, ReconChroma `0x40D2AA5A4..5BC` | `SetRoundingOffsetRegs` `0x5f69c` (`ctrl+0x2EF18` intra / `0x2EF90` inter, chosen at `0x578a8` by slice type); setPipe `0x578b0`–`0x57924` | wire `0xFD30` (`[x23,#1392]`, `0x5cf18`) | 0 → default arm | — | intra `0x2AA` (≈1/3), inter `0x156` (≈1/6), `0x5f854`–`0x5f860`. Sane. | [C] |
| ModeDec / ME λ (`0x40D26A09C/0A0`, …) | `0x5633c`, `0x56354` | RC λ scales wire `0xFF98..0xFFA8`, per-QP table RC+`0x90` | 0 | `0x400` ×5 and the table ([72](72-userspace-video-params.md) §0 row 7) | λ = 0. This changes **which mode** is chosen, never whether levels are zero. Not a cause of the blank frame. | [C] ([72](72-userspace-video-params.md)) |
| transform bypass (`qpprime_y_zero_transform_bypass`) | SPS + `0x39` | wire `0x105E9` | 0 | 0 | read only by the SPS writer (`0x195f8`), no register | [C] |
| stage enables `…A098/A0D8/A0CC/…` and go `= 3` for `0x127A`/`0x129A`/`0x12BA` | `ConfigureMCPUs` `0x60320`–`0x6038c` (via `0x57eac` on our skip = 0/create = 1 path); setPipe `0x57e24`–`0x57e38` | `bSkipMcpu` | enabled | — | on | [C] |
| **MCPU mode words**: IntraEst DMem `+0`/`+0x764`, ModeDec `+0`/`+0x9AC`, **ReconLuma `+0` (`0x61788`)/`+0x278` (`0x61860`)**, **ReconChroma `+0` (`0x618c4`)/`+0x94`**, CAVLC `+0x88`/`+0x600` | `ConfigureMCPUs`; every one is the same `w20`/`w21` (not reassigned `0x61440`–`0x619b4`) | wire `0x68` bits, `0xFCE4`, … ([70](70-intraest.md) §3 row 3) | read 0 (IntraEst, ModeDec) | wire `0x68` = `0xF` / `0x8000000` (`0x29a78`, `0x35090`) | each MCPU leaves setPipe's stage words in place. Benign **only if** setPipe's words are right, and the lists are not. | [C] stores; [I] values for ReconLuma/Chroma |

What the ReconLuma MCPU would do with non-zero words (`reconluma:0x1a8`,
`r9 = 0x4128a08c`) **[C]**:
- `w21` bit 0: QPY from ctx `+0xa8`.
- `w21` bit 2: copy 6 words ctx `0x41283078` → `0x4128a088..09c`.
- `w21` bit 4: per-MB QP. It also clears SKIPMODE per MB when MB word bit 1 is
  set, and restores it later.
- `w20` bit 0: nQuant from its own table (image `0x5d2`).
- `w20` bit 12: per-MB rounding into `0x4128a320..330` plus bit 0 of
  `0x4128a334`/`340`.
- `w20` bits 0+19: the monochrome 8×8 patch.

None of these touches the lists. ReconChroma's SKIPMODE block copy
(`reconchroma:0x70e`) is gated by `w21` bit 2, its other stage writes by
`w21 & 0x15` (`0x73e`). **[C]**

The f43 anomaly stays **[U]**: `0xFCE4 = 1` sets `w20` bit 9
(`0x612e4`, `0x61370`), yet IntraEst DMem `+0` read 0. `ConfigureMCPUs` is
straight-line from `0x61440` to `0x619b4`, and `StartUnit` (`0x91d14`) does
not touch DMem. It does not affect the conclusion above.

---

## 5. Ranked proposals

Each is one variable. The predictions come from `tools/scaling_regs.py`.

| rank | change | predicted registers | what a "no" means |
|---:|---|---|---|
| **R1** | **Read-only.** New `session_costs` bit 4, one `ave_step` marker per page, reading **IntraEst `0x40D24A088`, `0x40D24A08C`, `0x40D24A0C8`** (DPE `0x14A088`, `0x14A08C`, `0x14A0C8`) and **ReconLuma `0x40D28A088`, `08C`, `090`, `094`, `0A0`, `0A4`, `0E0`, `120`** (DPE `0x18A088`..`0x18A120`). Safe by construction: the first page is the one group 1 reads (`0x40D24A1C8`, f39–f44). The second is the one `ave_session_diag_mcpu` reads (`0x40D28A080`, every run including f44, `results/f44-qp10-1790252211.kmsg`). Both are registers the firmware writes every frame. | `0x088` **0**, `0x08C` **0**, **`0x090` 30 (10 at QP 10), `0x094` `0x80`** (the controls), `0x0A0`/`0x0A4`/`0x0E0`/`0x120` **0**. IntraEst `0x088`/`0x08C`/`0x0C8` **0** | QPY/nQuant read back but a list word is non-zero → the firmware did not write 0. The model (§1.2–1.3) is wrong, and so is the hypothesis. If QPY/nQuant also read 0, the block reads as zero and R1 measured nothing. Go to R2. |
| **R2** | **Flat-16 SPS lists**, nothing else: u16 `16` at wire `0x1060A + 2k` (k < 96) and `0x106CA + 2k` (k < 384). This is exactly what Apple's userspace sends (§1.4). Present-flag stays 0 and the profile stays 66, so SPS/PPS bytes do not change (§1.1). Driver shape: two layout fields `sps_scaling4x4 = 0x1060a`, `sps_scaling8x8 = 0x106ca` in the 13.5 table (`AVE_OFF_NONE` for 26.6.2, whose SPS is laid out differently, [37](37-start-avc-session.md) §1), filled with `wr16` after the `sps->header_len` write in `ave_cmd_build_start_avc`, behind `session_flat_lists` (default 1 once it works). Keep R1's reads on. | every list register **`0x10000010`**. IntraEst `0x40D24A088` `0x10000010`, ReconLuma `0x40D28A0A0` `0x10000010`, ReconChroma `0x40D2AA0A4` `0x10000010`. **Frame ≫ 2709 bytes** with non-zero `Intra16x16DCLevel`, and a decode near Y = 200 / our chroma at QP 30 (`check_frame.py`). | registers `0x10000010` but still 2709 bytes → the lists were necessary and not sufficient. Next is R3, then the internal pixel path, which is **[U]**. Registers still 0 → the wire offsets are wrong: recheck `0x5ce68`–`0x5ce90` against what the driver writes. |
| R3 | wire `0xFCF0` u16 = **3** (Apple's `skip_mode`), after R2 | `0x40D28A08C = 1`, `0x40D2AA08C = 1`. Cancellation words unchanged, because `0xFCF2 = 0` takes the explicit arm (`0x5f260`). | registers unchanged → wrong offset. A bitstream change on an I frame is not predicted. This is parity with Apple before P frames. |
| R4 | read ReconChroma `0x40D2AA08C`, `0x40D2AA0A4` (DPE `0x1AA08C`, `0x1AA0A4`), each behind its own marker | 0 / 0 (R2: 0 / `0x10000010`) | This page has never been read by the driver. It holds registers the firmware writes every frame (`0x57874`, setPipe pair loop), so it is a real block, but it is untested, hence ranked below R1. |
| R5 | read IntraEst ctx `0x40D243000` (DPE `0x143000`, same page as the existing `0x143180` read) | bits 10..15 = the per-MB QP (30 → `0x7800` pattern) if it mirrors ModeDec/ReconLuma | Settles whether IntraEst's `+0x180 = 0` is a real "no work" or a register with a different meaning. Diagnostic only. |

**Not proposed:** any read outside a block shown in §2.1 with a firmware
writer. The f38 SError came from `0x40D348000`, which has no block. Every
address above lies in a block whose writer is cited, and R1's two pages have
already been read on hardware.

---

## 6. Corrections and annotations to earlier documents (not edited here)

- **[53](53-first-frame.md)** f44, "It is not quantisation": it is not the
  **QP**. It is the quantiser's per-coefficient **scale**, which is zero
  because the SPS scaling lists are zero. §1.
- **[70](70-intraest.md)** §1.3: the IntraEst map has 80 more writes
  (`0x124A088..1C4`, table-driven through `ctrl+0x2F010`/`0x2F390`), plus
  rounding `0x234..23C` and cancellation `0x240..`. §1.2, §4.
- **[70](70-intraest.md)** §7: the Context Height store `[x8,#-252]` at
  `0x58534` is **`0x12EA00C`**, which the firmware names
  `AVC_TRANSCODECONFIGGLOBALS_CONTEXTHEIGHT` (`0x584e0` `adr x1, 0xc6c51`).
  Its block is Transcode (`0x12EA000`), not `0x119xxxx`.
- **[58](58-pipe-start.md)** §1.4/§1.6: on our path (skip 0, create 1),
  `ConfigureMCPUs` is called at **`0x57eac`**. `0x57df4` is reached only when
  `bSkipMcpu != 0` (`0x57ddc`/`0x57de0`). Three of the ten stage enables
  belong to blocks with no MCPU (`0x127A`, `0x129A`, `0x12BA` = Deblock), and
  setPipe writes their go as `3` (`0x57e24`–`0x57e38`).
- **[60](60-md-recon-stall.md)** §3.1: ReconLuma's MCPU writes
  `0x4128a320/328/330` and bit 0 of `0x4128a334/340` (`r9 = 0x4128a08c`,
  offsets `#660..#692`), not `0x4128a294..2b4`. They are the rounding and
  cancellation registers.
- **[69](69-source-to-modedec.md)** §1.2: `0x40D120004/08/84/88` are **not**
  unwritten hardware descriptors. `SetTunable`'s table writes them
  (`0xcf630` entries 3, 4, 7, 8: mask `0x07FF07FF`, set `0xC0`, `0xC0`,
  `0x00C00140`, `0x00C000C0`), and runs from `InitEncodingParameters`
  (`0x5e858`) before any frame. Those are exactly the values F21 read
  "before the session was configured and after". The immediate scan missed
  them (Trap 3). They are firmware tunables, still with no host field.
  The same table also explains `+0x18 = 0x00072065` after Start_AVC: entry 5
  sets `0x72060` under mask `0x7FFF7FF0`, so the reset low bits `0x5`
  survive. It is not a power-on default.
- **[73](73-modedec-costs.md)** §4.1 / **[62](62-kext-field-map.md)** §6.5:
  add wire `0xFCF0` (skip_mode → RECONL/RECONC SKIPMODE, Apple 3), `0xFCF2`
  (cancellation override, 0xFFFF = derive), `0xFD30` (rounding mode), and the
  SPS lists at `0x1060A`/`0x106CA`.

---

## 7. Reproduce

```sh
# the models (anchors checked first)
python3 tools/scaling_regs.py
python3 tools/scaling_regs.py --lists flat --skip-mode 3
python3 tools/scaling_regs.py --dump                  # all 577 registers
python3 tools/mmio_map.py --check                     # controls: docs/69, 70, 73 sites
python3 tools/mmio_map.py --lo 0x1270000 --hi 0x1300000 --st

# SPS copy, and the SPS writer's list offsets
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5ce60 -n 0x50
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x195a0 -n 0x230
# InitScalingListRegs, its caller, the pair loops in setPipe
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5fb64 -n 0x270
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5e124 -n 0x38
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57630 -n 0x1e8
# SKIPMODE, mode_8x8; cancellation; rounding; the other pair loops
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57818 -n 0x70
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cedc -n 0x48
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5f218 -n 0x484
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5f69c -n 0x4c8
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57888 -n 0x278
# go = 3 for the MCPU-less stages, the ConfigureMCPUs call on our path, deblock, transcode
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57dd8 -n 0xe0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57b00 -n 0xe0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x584cc -n 0x70
# SetTunable and its table (none in 0x124..0x12F; 0x1120004/08 = 0xC0)
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x277a8 -n 0x160
python3 -c "import struct;d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
e=[struct.unpack_from('<4I',d,0xcf630+0x4000+16*i) for i in range(232)]
print(sorted({hex((o+0x1100000)>>16) for o,_,_,_ in e})); print([tuple(map(hex,x)) for x in e[2:9]])"
# MCPU mode words into ReconLuma / ReconChroma / CAVLC DMem
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x615a4 -n 0x440
# strings
python3 -c "d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
for v in (0xc6b2a,0xc6b53,0xc6b7c,0xc6c51): print(hex(v), d[v+0x4000:d.index(b'\0',v+0x4000)])"

# MCPU images (objdump refuses a pipe)
T=$(mktemp -d); python3 -c "d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
for n,va,sz in (('reconl',0xe4480,0x606),('reconc',0xe4a90,0xbb8)):
    open('$T/'+n+'.bin','wb').write(d[va+0x4000:va+0x4000+sz])"
objdump -D -b binary -m arm -M force-thumb $T/reconl.bin | sed -n '/^ 1a8:/,/^ 52c:/p'
objdump -D -b binary -m arm -M force-thumb $T/reconc.bin | sed -n '/^ 634:/,/^ 760:/p'

# Apple's userspace: AVE_PrepareSequenceHeader, the flat-16 arm and its pattern
U=data/blobs/macos-13.5/userspace/AVE.bin
objdump -D -b binary -m aarch64 --start-address=0x2cd44 --stop-address=0x2cda0 $U
objdump -D -b binary -m aarch64 --start-address=0x2d038 --stop-address=0x2d0a0 $U
objdump -D -b binary -m aarch64 --start-address=0x29900 --stop-address=0x29b24 $U  # skip_mode = 3
python3 -c "import struct;d=open('$U','rb').read();print(struct.unpack_from('<8H',d,0x115ff0), struct.unpack_from('<7i',d,0x2da84))"

# driver side
grep -n 'memset(buf, 0\|wr[0-9]*(&w, sps->' driver/ave_cmd.c
grep -n 'ReconLuma ctx' results/f44-qp10-1790252211.kmsg        # 0x40D28A080 page already read
```
