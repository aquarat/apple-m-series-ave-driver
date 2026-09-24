# The ModeDecision "cost ladder" and IntraEst's per-QP words: who writes them, and what they mean

Static analysis of the macOS 13.5 firmware (`ave_h13c.bin`), its IntraEst and
ModeDecision MCPU images, and the driver, against the f39/f40/f40b register
reads in [53](53-first-frame.md) (end). **Nothing here was run on hardware.**

Conventions as [70](70-intraest.md): firmware VAs are 13.5 image VAs (file
offset = VA + `0x4000`); **wire** = byte offset in the command on IPC channel
1; VP + `0x60` = wire; firmware MMIO offset `X` is AP `0x40C000000 + X`; the
MCPUs see the same registers at `0x40000000 + X`. `ctrl` = the
`CAVCController` object (`x19`/`x25` in the functions cited). Labels per
[00](00-methodology.md): **[C]** read from an instruction (VA cited), **[I]**
inferred with the chain stated, **[U]** unknown.

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5e510 -n 0x340
python3 tools/modedec_costs.py          # the model below, anchored to 24 instructions
```

---

## 0. Verdicts

| # | question | verdict | label |
|---:|---|---|---|
| 1 | Who writes `0x01000000` into the 23 ModeDec words | `CAVCController::InitEncodingParameters` clears bit 0 of words 1..24 of **every record of the I-slice struct**, unconditionally (fw `0x5e598`–`0x5e690`), after `SetDefaultParameters` seeded everything with `0x01000001`. `setPipe` copies record 17 to `0x40D26A0AC..0x104`. **No host field is involved.** | [C] |
| 2 | Is `0x01000000` a fault? | **No. It is the firmware's designed I-slice value**: bit 0 is a per-candidate enable, words 1..24 are the inter/skip candidates, and an I slice turns them all off. docs/70 §3 row 2 / §9 rank 2 ("the ladder was zeroed from a zeroed struct") is **closed**: the struct is not zeroed, and the one value it could hold for an I slice is the one we read. | [C] for the writes, [I] for "enable" |
| 3 | "client" in `ctrl+0x2CA2C + client*1800` | Not a client. It is **the slice-type index** `ctrl+0x1BA0`: I → 0, P → 1, B → 2 (`PipePrepareParam` fw `0x481c4`, `0x490e4`, `0x482c0`). Three 1800-byte structs = 18 records × 25 words each. | [C] |
| 4 | The 23 words are only part of it | The record is **25 words, `0x40D26A0A4..0x104`**. Words 0 and 1 (`0x0A4`, `0x0A8`) are written by `setPipe` from a per-QP pair with bit 0 from the record (fw `0x57638`–`0x576a8`). Predicted for our frame: `0x0A4 = 0x01000001` (candidate 0 = intra, **enabled**), `0x0A8 = 0x01000000`. **Never read.** | [C] |
| 5 | Why the 23rd word (`0x40D26A104`) is 0 | **[U].** The firmware writes `0x01000000` there on our path, exactly like the other 22. No firmware writer of 0 was found (four independent scans, §2.4), and both MCPU paths that could touch it are gated off by DMem words that are 0 in our run. Most likely the register does not store bit 24; one P frame settles it (§6, P3). | [U] |
| 6 | IntraEst `0x1D0/1D4/1D8` | Upper bits = the `0x01000001` seed of the 12-byte per-QP entry (never rewritten). **Bit 0 of each comes from a second 3-word entry `B[slice]` at `ctrl+0x2E0E4`** (fw `0x570a4`–`0x57130`). `B[0].w1` bit 0 is cleared because **wire `0xFCEC` (`mode_8x8_transform`) = 0**: word 1 is the Intra8x8 enable, and we send "no 8x8" (fw `0x5e7ec`). `0x01000001 / 0x01000000 / 0x01000001` is again the designed value. | [C] / [I] (8x8) |
| 7 | IntraEst handler patch at image `0x26a` | The mask is **`0x00080001`, not `0x08000001`** (`movs r4,#1; movt r4,#8`, image `0x270`/`0x272`). Bit 19 is set only when SPS `chroma_format_idc == 0`, so it **cannot run on a 4:2:0 session**. When it runs it sets `0x4124A1D4` bit 0 (the 8x8 enable) per MB from bits 0..12 of `0x41243180`. | [C] |
| 8 | ModeDec MCPU and bit 0 | It never *tests* bit 0; it **rewrites** it as a candidate mask: clear bit 0 of all 25 words, set it on word 0 or word 1 (image `0x38e`–`0x504`), or reload a whole record from DMem (image `0x2a8`–`0x31a`). Both paths are gated by DMem words that are 0 for us. | [C] |
| 9 | Host fields that feed these registers | `0xFCEC`, `0xFCE4`, `0xFCE2`, `0xFCE3`, `0x70`, and **three λ words at `0xFFA4`/`0xFFA8`/`0xFFAC`** (MD_InterLambda, MD_IntraLambda, MD_IntraOffset — named from the HEVC twin of this struct, §4.2). We send 0 in all of them. | [C] reads, [I] names |
| 10 | Side finding: f41's missing `QPY` line | `SetTranscode` **saves** the print gate, sets it from `0xFCD8` bit 5, prints, and **restores** it (fw `0x584d8`, `0x58560`, `0x5949c`). `setPipe` runs with the gate still at its saved value, so no `setPipe` `AVC COMMON::` line can appear from `0xFCD8` alone. docs/70 §6 overstated what bit 5 reaches. | [C] |

The short version: **every one of these registers holds exactly what the firmware
means to program for a Baseline I slice.** The ModeDec ladder is not why the frame
is blank. What is left to test is below the configuration: whether the decision the
pipe codes follows this configuration at all. §6 P2 (`0xFCE4 = 1`, I_PCM) asks
that directly.

---

## 1. The three structures

### 1.1 Seeded once, by `SetDefaultParameters`

`CAVCController::SetDefaultParameters` (`0x4674c`) returns at once if
`ctrl+0xA68` is set (`0x46778`/`0x4677c`), and sets it at the end (`0x47e38`).
**[C]** On its one run:

| region | what | fill | VA |
|---|---|---|---|
| `ctrl+0x2CA2C` .. `+0x2DF44` | three 1800-byte structs | `0x01000001` into all 5400 bytes: a 4-pass loop (`x9 = -5376` at `0x468c8`, `adds x9,x9,#0x540 / b.ne` at `0x474a0`) of 336 words each at `[x10,#5376..6716]`, `x10 = ctrl+0x2CA2C + x9`, then 6 tail words at `0x474b4`–`0x474c8` | **[C]** |
| `ctrl+0x2DF44` .. `+0x2E0E4` | 52 × 8-byte per-QP ModeDec pairs | `0x01000001` | `0x474dc`–`0x47de0` **[C]** |
| `ctrl+0x2E0E4` .. `+0x2E0FC` | `B[2][3]`, the per-slice-class intra sub-mode enables | `0x01000001` | `0x47de4`–`0x47e28` **[C]** |
| `ctrl+0x2E0FC` .. `+0x2E36C` | 52 × 12-byte IntraEst per-QP entries | `0x01000001` | via `x11 = x21+0x1B8`, `0x478a8` on **[C]** |

Every word from `0x2DF44` to `0x2E36B` is stored (instruction-level coverage
check in §8). `ProcessInit` calls `SetDefaultParameters` (`0x4de30`) and then the
virtual at `vt+536` (`0x4de44`), which is `InitEncodingParameters` **[I]** —
the f40 values below are only reachable in that order, which is the check on it.

### 1.2 The slice-type index

`PipePrepareParam` writes the pair `(ctrl+0x1BA0, ctrl+0x1BA4)` from the frame's
slice type `[[ctrl+0x2E370+8·ctx]+16]` (`0x481b0`):

| slice_type | pair | VA |
|---|---|---|
| 2 (I, incl. IDR) | `(0, 0)` — `movi v0.2d,#0` | `0x481c4`/`0x481c8` |
| 0 (P) | `(1, 1)` — `movi v0.2s,#1` | `0x490e4`/`0x490e8` |
| 1 (B) | `(2, 1)` — literal at `0xd7fc8` = `{2, 1}` | `0x482c0`, `0x490e8` |

**[C]**; that `+16` is `slice_type` with 2 = I is from [60](60-md-recon-stall.md)
§7 and [65](65-pframes.md) §5.1 **[I]**. These are the only writers of
`ctrl+0x1BA0`/`0x1BA4` in the image. **[C]** So `ctrl+0x1BA0` picks the
1800-byte struct (0 = I, 1 = P, 2 = B) and `ctrl+0x1BA4` picks `B[]` (I → 0,
P and B → 1). Our IDR frame uses **struct 0 and `B[0]`**.

### 1.3 A struct is 18 records of 25 words; record 17 is the register image

`setPipe` reads record 17 (bytes `1700..1799`):

- words 2..24 (`+0x6AC..+0x704`) → `0x40D26A0AC..0x104` (fw `0x57128`–`0x57638`,
  §2.2);
- words 0 and 1 (`+0x6A4`, `+0x6A8`) → bit 0 of `0x40D26A0A4`/`0x0A8`
  (fw `0x57640`–`0x576a8`, §2.3).

Records 0..16 are copied to the ModeDecision MCPU's DMem at `0x10000104`
(`ConfigureMCPUs` `0x61148`–`0x61168` memcpy 1800 bytes into the stack image
at `sp+0xa60+0x104`, which `0x61720`–`0x61768` copies to `0x1468008..0x14689B0`)
— only when `ctrl+0x23FD2 != 0` (`cbz w27` `0x61144`). The MCPU loads one of
them into the 25 registers per MB (§5.1). **[C]**

That the record is exactly the 25 registers `0x0A4..0x104` is confirmed from the
other side: the ModeDec MCPU's save/restore copies 100 bytes at `0x4126A0A4`
(image `0x368`–`0x37c`, `0x4d6`–`0x4ec`) and its clear loop walks
`[r8,#0]..[r8,#0x60]` (image `0x38e`–`0x4ba`). **[C]**

---

## 2. Question 1a: the 25 ModeDecision words

### 2.1 `InitEncodingParameters` writes the enables (fw `0x5e510`–`0x5e84c`)

A loop over `x21 = 0..51` (`cmp x21,#0x34` `0x5e590`); the record part runs for
`x21 <= 17` only (`cmp x21,#0x11 / b.hi` `0x5e598`). `x23` starts at
`ctrl+0x2CA30` (`0x5e524`/`0x5e534`) = struct 0 + 4 and steps 100
(`0x5e57c`). For each record `i`:

| struct | words | action | VA | gate |
|---|---|---|---|---|
| 0 (I) | 1..24 | `bic v0.4s,#1` on six q-words = bit 0 cleared | `0x5e5a0`–`0x5e690` | **none** |
| 1 (P) | 9..12, 13..16, 17..20, 21 | bit 0 cleared | `0x5e694`–`0x5e724` | none |
| 1 and 2 | 1 | bit 0 cleared | `0x5e730`–`0x5e744` | `ctrl+0x23FE3` = wire `0xFCE2` (`disable_skip_mode`) |
| 1 and 2 | 0 | bit 0 cleared | `0x5e750`–`0x5e768` | `ctrl+0x23FDF` = wire `0xFCE3` (`disable_intra_mode`) |

**[C]** for every row; the wire sources are `0x5cf28`–`0x5cf34`
([70](70-intraest.md) §8). Before this loop, and only if `ctrl+0x23FD3`
(from VP+`0x10`, wire `0x70`, set to `v < 0x11` at `0x5d3c0`–`0x5d3d0`) is
non-zero, records 0..16 of structs 1 and 2 get their 25 enable bits from
two 17-entry tables — P at `0xd8048`, B at `0xd808c` (`0x5e454`–`0x5e50c`).
**[C]** Record 17, the register image, is never touched by that preset loop.

Every path through `InitEncodingParameters` reaches `0x5e44c` (the
`ctrl+0x11F4` switch at `0x5e15c`/`0x5e174` falls through or ends at
`0x5e248 b 0x5e44c`), so the struct-0 clear is **unconditional**. **[C]**

What the tables and the gates say about the words, all consistent with
"bit 0 = this candidate may be chosen":

- struct 0 (I): only word 0 left on, in every record.
- struct 1 (P): words 9..21 always off; the P presets never enable 9..21
  either (`0xd8048[16] = 0x01c001ff` = words 0..8, 22..24).
- struct 2 (B): the fullest B preset is `0x01ffffff`, all 25.
- `disable_intra_mode` clears word 0; `disable_skip_mode` clears word 1.

So **word 0 = intra, word 1 = skip, 2..8 and 22..24 = P inter candidates,
9..21 = B-only (bi/direct) candidates**. **[I]**, strong: three independent
gates and two tables agree, and the ModeDec MCPU treats bit 0 the same way (§5.1).

### 2.2 `setPipe` writes words 2..24 (fw `0x57128`–`0x57638`)

Per word `k = 0..22`: `w11 = [ctrl + 1800·[ctrl+0x1BA0] + 0x2D0D8 + 4k]`;
`cmp w11, 0x01000001; b.eq skip`; otherwise `str w11` to
`0x126A08C + 32 + 4k` = AP `0x40D26A0AC + 4k`. **[C]** `ProcessPipeReset`
first writes `0x01000001` to all 23 (`0x4f218`–`0x4f278`), and
`ProcessPipeStart` calls it (`vt+472`, `0x52e08`) before `setPipe`
(`0x52e40`). **[C]**

For our I frame every struct-0 value is `0x01000000`, so **all 23 are
written with `0x01000000`**. That the reads show `0x01000000` and not the
reset seed is itself the proof that `ctrl+0x1BA0 = 0` and that the clear in
§2.1 ran after the seed. **[C]** by observation + code.

### 2.3 Words 0 and 1, and the two λ words before them (never read)

| AP | value | source | VA |
|---|---|---|---|
| `0x40D26A09C` | `(ctrl[0xB20]·nQuant + 0x200) >> 10` | `ctrl+0xB20` = RC settings `+120` = **wire `0xFFA8`** | `0x56330`–`0x5634c` |
| `0x40D26A0A0` | `(ctrl[0xB1C]·nQuant + 0x200) >> 10` | `ctrl+0xB1C` = **wire `0xFFA4`** | `0x56350`–`0x5635c` |
| `0x40D26A0A4` | per-QP pair word 0, bit 0 ← record-17 word 0 | pair at `ctrl+0x2DF44+8·QPY` | `0x57640`–`0x5769c` |
| `0x40D26A0A8` | per-QP pair word 1, bit 0 ← record-17 word 1 | same | `0x57674`–`0x576a8` |

**[C]**. `ctrl+0xB10..0xB27` are six u32 copied from `VP+0xFED0+104..127` =
wire `0xFF98..0xFFAF` (`0x5d3d4`–`0x5d3f4`, `[sp,#104] = VP+0xFED0` stored at
`0x5ce00`). **[C]**

The pair is built by `InitEncodingParameters` for every QP (`0x5e76c`–`0x5e7b4`):
word 0 bits 4..15 = `ctrl[0xB24]` (wire `0xFFAC`) + `u16 table[0xd80d0 + 2·QP]`;
word 1 = `(w1 & 0xF) | table[0xd8138+4·QP] << 16 | (table[0xd8208+4·QP] & 0xFFF) << 4`.
**[C]** The three tables are all-zero, all-`0x100`, all-zero for QP 0..51.
**[C]** (read from the blob by `tools/modedec_costs.py`.)

So for our frame (QPY 30, nQuant `0x80`, all three RC words 0):
**`0x09C = 0`, `0x0A0 = 0`, `0x0A4 = 0x01000001`, `0x0A8 = 0x01000000`.**
**[C]** as a prediction. `0x0A4` bit 0 is the only candidate enable left on in
an I slice. If a read shows it clear, ModeDec has no legal candidate — that
is the one outcome in this area that would be a real fault.

### 2.4 The 23rd word

Record 17 word 24 (`ctrl+0x2D130`) is seeded (`0x474b4`–`0x474c8`), cleared to
`0x01000000` by the same q-word as words 21..23 (`0x5e67c`–`0x5e690`,
`[x8,#80..92]`), and written by `0x57630` (`str w11,[x8,#120]` =
`0x126A104`). **[C]** Searches for anything else that writes it:

1. every `mov`/`movk` 32-bit constant in `0x2CA2C..0x2E36C` (only the reads in
   `setPipe` and the two bases in `InitEncodingParameters`);
2. every `add …, #0x2c/#0x2d/#0x2e, lsl #12` in the AVC controller (§8), each
   followed to its offsets;
3. every store with immediate `#1796` or `#304` (the two ways to reach
   `+0x704` from a struct or `+0x2D000` base) — all resolve elsewhere;
4. every constant in `0x1260000..0x1270000`: only `0x126A020`
   (`ProcessPipeReset`), `0x126A08C` (`setPipe`, 27 sites) and `0x126A114`
   (`ConfigureMCPUs`).

**None writes 0.** On the MCPU side the only writers of `0x4126A104` are the
clear loop (bit 0 only), the save/restore copy, the record load, and a 108-byte
copy from `0x4126300C` (image `0x33e`–`0x354`); all are gated by DMem words
that are 0 in our run (§5.1). **[C]** for the scans (Trap 3 applies to the
table-driven case; scan 4 reproduces docs/70 §1.3's list, the control).

So the zero is **[U]**, and the cheapest reading consistent with everything is
that `0x40D26A104` does not store bit 24 (and bit 0 was cleared, as designed).
That is testable for free (§6, P3).

---

## 3. Question 1b: the IntraEst per-QP words

`setPipe` `0x570a4`–`0x57130`:

```
570a4  x10 = [sp,#168]                ; QPY (stored at 0x55eb0)
570a8  x8  = ctrl + 12·[ctrl+0x1BA4] + 0x2E0E4   ; B[b]
570ac  x9  = ctrl + 12·QPY + 0x2E0FC  (0x570c0/0x570cc)
570ec  bit v0.8b, v2.8b, v1.8b        ; v1 = 0xFFFFFFFE x2: words 0,1 keep B's bit 0
570e0  bfxil w13, w10, #0, #1          ; word 2: bit 0 from B word 2
570f8  str back to the per-QP entry   ; persistent
57118  0x40D24A1D8 = word 2
57120  0x40D24A1D4 = word 1
57130  0x40D24A1D0 = word 0
```

**[C].** So the upper 31 bits are the per-QP entry — seeded `0x01000001` and,
by every scan in §2.4 plus the `0x2e, lsl #12` bases in
`InitCoeffCancelCostRegs`/`SetRoundingOffsetRegs` (both land above
`0x2E728`), **never rewritten**. **[C]** Bit 0 of each word is `B[b]`'s.

`B[]` is written only by `InitEncodingParameters` (`0x5e7b8`–`0x5e848`),
from `ctrl+0xAB0` = **wire `0xFCEC`, u32** (`0x5cf50`/`0x5cf54`), and
`ctrl+0x23FDE` = **wire `0xFCE4`, u8** (`0x5cf58`/`0x5cf5c`):

| `0xFCEC` (`mode_8x8_transform`) | cleared | VA |
|---|---|---|
| 0 | `B[0].w1`, `B[1].w1` | `0x5e7ec`–`0x5e804` (`[sp,#96]` = `0x2E0F4`, `[sp,#104]` = `0x2E0E8`) |
| 1 | `B[0].w0`, `B[0].w2`, `B[1].w0`, `B[1].w2` | `0x5e7c8`–`0x5e804` |
| ≥ 2 | nothing | `0x5e7c4` `b.ne` |

| `0xFCE4` | cleared | VA |
|---|---|---|
| ≠ 0 | `B[0].w0/w1/w2` — logs `"Disabling Intra Modes!!!!"` | `0x5e808`–`0x5e848` |

**[C]**. The name `mode_8x8_transform` is the firmware's own
(`setPipe` prints `ctrl+0xAB0` with `0xc6a27` at `0x56700`; it also writes
`ctrl+0xAB0 & 3` to ReconLuma `0x40D28A088`, `0x57818`–`0x5782c`). **[C]**
Mode 0 turns word 1 off and mode 1 turns words 0 and 2 off, so **word 1 is the
Intra8x8 enable** **[I]**, strong. Words 0 and 2 are Intra4x4 and Intra16x16;
which is which is **[U]** (the HEVC debug dump lists `bDisableIntra4x4`,
`8x8`, `16x16` in that order, which suggests w0 = 4x4, w2 = 16x16 — ordering
only).

For us: `0xFCEC = 0` → `B[0] = {0x01000001, 0x01000000, 0x01000001}` →
**`0x1D0 = 0x01000001`, `0x1D4 = 0x01000000`, `0x1D8 = 0x01000001`** — the f39,
f40 and f40b reads exactly. The 8x8 enable is off because we send Baseline
(`transform_8x8_mode_flag = 0`); that is correct, not a fault.

`0xFCE4 ≠ 0` clears only `B[0]`, which is used only by I slices (§1.2): it
disables every intra sub-mode **in I slices**, and `CAVCController::DebugInit`
describes the same byte (`ctrl+0x23FDE`, `0x4e654`/`0x4e658`/`0x4e6ec`) as
**"Disable all intra modes in intra slices: %d (i.e., code macro-blocks in I
slices as I_PCM)"** (string `0xc7520`). **[C]**

---

## 4. The host fields, and what we send

### 4.1 Everything that reaches these registers

| wire | width | → | register effect | VA | driver |
|---|---|---|---|---|---|
| Process `0xD74` (`IMG_FRAME_TYPE`) | s32 | slice type → `ctrl+0x1BA0/0x1BA4` | picks struct 0/1/2 and `B[0]`/`B[1]` | `0x481b0`–`0x490e8` | 3 (IDR) → I |
| `0xFFB4` (`qp_i`) | u32 | QPY | picks the per-QP entry and pair (all equal) | docs/70 | 30 |
| **`0xFCEC`** | u32 | `ctrl+0xAB0` | IntraEst word 1 (0), words 0/2 (1); ReconLuma `0x40D28A088` | `0x5cf50`, `0x5e7b8`, `0x57818` | **0** (never written) |
| **`0xFCE4`** | u8 | `ctrl+0x23FDE` | all three IntraEst bit 0s off (I only); IntraEst/ModeDec DMem word bit 9 | `0x5cf58`, `0x5e808`, `0x612e4` | **0** |
| `0xFCE2` | u8 | `ctrl+0x23FE3` | ModeDec word 1 (skip) off, P/B only | `0x5cf28`, `0x5e728` | 0 |
| `0xFCE3` | u8 | `ctrl+0x23FDF` | ModeDec word 0 (intra) off, P/B only | `0x5cf30`, `0x5e748` | 0 |
| `0x70` (VP+`0x10`) | u32 | `ctrl+0x23FD3` | P/B preset records 0..16 (DMem only) | `0x5d3c0`, `0x5e44c` | 0 |
| **`0xFFA4`** | u32 | `ctrl+0xB1C` | `0x40D26A0A0` | `0x5d3e4`, `0x56350` | **0** |
| **`0xFFA8`** | u32 | `ctrl+0xB20` | `0x40D26A09C` | `0x5d3f4`, `0x56330` | **0** |
| **`0xFFAC`** | u32 | `ctrl+0xB24` | `0x40D26A0A4` bits 4..15 | `0x5d3f4`, `0x5e774` | **0** |
| `0xFF98`, `0xFF9C` | u32 | `ctrl+0xB10/0xB14` | ME `0x40D190500..0x508`, `0x40D190600..0x608` and a clamped copy | `0x55ec0`–`0x55f3c` | 0 |
| `0xFFA0` | u32 | `ctrl+0xB18` | `setLRME` | `0x524d8` | 0 |

**[C]** for every read and destination. The driver's `Start_AVC` builder
memsets the command (`driver/ave_cmd.c:125`) and writes none of the bold rows
(`driver/ave_cmd.c:434`–`489`, `driver/ave_abi.h:1531`–`1622`).

### 4.2 Naming the `0xFCD8` block from its HEVC twin

`CHEVCController::DebugInit` (`0x39e0c`–`0x3a284`) prints an
`EncCommParams`-style block field by field. Aligning its offsets with the AVC
reads gives six independent matches, which is what makes the λ names usable:

| HEVC `+off` | printed as | AVC wire (`0xFCD8 + off`) | AVC evidence |
|---:|---|---|---|
| 0 | `verbose` | `0xFCD8` | the debug bitfield, docs/70 §6 |
| 4/5/6 | `bDisableIntra4x4/8x8/16x16` | `0xFCDC/DD/DE` | → `ctrl+0x23FE0..E2` (`0x5cf38`–`0x5cf4c`); no AVC consumer found **[U]** |
| 8 | `search_range` | `0xFCE0` | → `ctrl+0x1224`, printed "search range" (`0x5cf0c`, `0x4e6d8`) |
| 10 | `disable_skip_mode` | `0xFCE2` | §2.1 |
| 11 | `disable_intra_mode` | `0xFCE3` | §2.1 |
| 12 | `enable_IPCM_in_IntraSlice` | `0xFCE4` | "…code macro-blocks in I slices as I_PCM" (§3) |
| 20 | `mode_8x8_transform` (u32) | `0xFCEC` | §3 |
| 24 | `skip_mode` (u16) | `0xFCF0` | → `ctrl+0x121E`, printed "skip mode" (`0x5cef4`, `0x4e6b0`) |
| 28 | `qcoeff_cancel` (u16) | `0xFCF4` | → `ctrl+0x121C`, printed "DCT coefficient cancellation mode" (`0x5cf04`, `0x4e6c4`) |
| 704/708/712 | `ME_FullPel/SubPel/LowResLambda` | `0xFF98/9C/A0` | ME registers / `setLRME` (§4.1) |
| 716/720/724 | `MD_InterLambda / MD_IntraLambda / MD_IntraOffset` | `0xFFA4/A8/AC` | ModeDec `0x0A0` / `0x09C` / `0x0A4` bits 4..15 |

**[C]** for the HEVC prints and the AVC reads; the names on the AVC side are
**[I]** by alignment. The λ trio lands inside what docs/62 §1.1 calls
`AVEFWRCSettings` (wire `0xFF30`, `0x680` bytes) — the two blocks overlap on
the wire; that is a naming problem, not a layout one.

---

## 5. Question 2: what the bits mean, from the MCPUs

### 5.1 ModeDecision (image `0xe37a0`, handler `0x1ac`)

```
282  r0 = [0x41263184]; if < 0x10000000 -> no work
296  r1 = [0x4126A018] & 3
2a4  if r1 != 2 && (DMem[0x10000000] & 2):                 ; record load
2be    cls = ([0x41263180] >> 16) & 3 ; n = DMem[0x10000004 + 4·cls]
30c    memcpy(0x4126A0A4, 0x10000104 + 100·(16 - n), …)     ; bl 0xbc4
328  m = DMem[0x100009AC]
33e  if m bit 2: memcpy(0x4126A09C, 0x4126300C, 108)         ; from per-MB ctx
35c  if m bit 4:
       if (MB word 0x41263000 & 3) == 0:  restore 100 B from DMem 0x100009B0 (0x4d4)
       else: save 100 B to 0x100009B0 once (0x368),
             bic #1 on all 25 words 0x4126A0A4..0x104 (0x38e-0x4ba),
             orr #1 into word 0 if MB bit 0, else word 1 unless r1 == 2 (0x4be-0x504)
```

**[C]** for every line. The MCPU therefore treats bit 0 of the 25 words as a
**candidate mask** — it forces an MB to "intra only" or "skip only" by clearing
all and setting one — and treats records 0..16 in DMem as alternative masks
chosen per MB class. It never reads a bit to decide anything. **[C]**

Upper bits: word 1's bits 16..31 come from a table that is `0x100` for every
QP, and the seed's upper half is `0x0100` too; IntraEst's handler forces
`0x1D0` bits 4..31 to `0x100000` (below). A 16-bit field that is `0x100`
everywhere reads as a Q8 weight of 1.0; bits 4..15 carry `MD_IntraOffset` on
word 0. So a candidate word is **`{bit 0 enable, bits 4..15 additive offset,
bits 16..31 multiplier (0x100 = 1.0)}`**. **[I]**

Both MCPU paths are off for us: `DMem[0x10000000]` and `DMem[0x100009AC]` are
the same two variables `ConfigureMCPUs` writes into IntraEst's DMem —
`w20` (`mov w10,w20` `0x615b4`; `0x6169c` IntraEst `+0`, `0x6171c` ModeDec
`+0`) and `w21` (`0x61688` IntraEst `+0x764`, `0x61708` ModeDec `+0x9AC`).
**[C]** f40 read IntraEst `+0` = 0 and `+0x764` = 0, so ModeDec's are 0 too.
**[I]**, from the shared variable; readable directly (§6, P1).

### 5.2 IntraEst (image `0xe3350`, handler `0x1a8`) — corrected

docs/70 §1.2 is right except the mask at `0x26a`:

```
26a  r3 = DMem[0x10000000]
270  movs r4,#1 ; 272 movt r4,#8        -> r4 = 0x00080001
276  if ((r3 & 0x00080001) == 0x00080001):
27e    w = [0x4124A1D4] & ~1
282    if ([0x41243180] << 19) != 0: w |= 1      ; bits 0..12 of the per-MB word
28e    [0x4124A1D4] = w
294  if (r3 bit 12):
298    [0x4124A1D0] = ([..] & 0xF) | 0x01000000
2a8    [0x4124A1D4] = ([..] & 0xF) | u16[0x41243234] << 16   ; [0x41243180 + 0xB4]
2bc    [0x4124A1D8] = ([..] & 0xF) | u16[0x41243236] << 16
```

**[C].** `w20` bit 0 ← `ctrl+0x23FC7` (`0x612d8`, `0x6135c`); bit 19 ←
`chroma_format_idc == 0 && ctrl+0x2C8D4 != 0` (`0x612e0`–`0x61318`; `ctrl+0x2C224`
= SPS `+0x28` = wire `0x105D8`, driver-labelled `chroma_format_idc`). **[C]**
We send 1, so **the patch can never run on our session**. When it does run it
toggles the 8x8 enable per macroblock. With `w20` bit 12, the MCPU overrides
the per-QP words per MB with per-MB weights from the context block — the same
`{enable, …, weight << 16}` layout as §5.1. **[C]** for the writes, **[I]** for
the reading.

---

## 6. Question 3: what would change these words, ranked

Everything below is a proposal for the operator. Each is one change, the
predicted register values come from `tools/modedec_costs.py` with the matching
flags, and each has a "no".

| rank | change | predicted registers | what a "no" means |
|---:|---|---|---|
| **P1** | **Read-only**: extend `session_costs` group 2 to `0x40D26A09C..0x104` (27 words, DPE `0x16A09C..`) and add ModeDec DMem `0x40D468000` and `0x40D4689AC` (DPE `0x368000`, `0x3689AC`, one per marker) to group 4. Same block as the existing `0x16A0AC` and `0x368A1C` reads. Piggyback on any run. | `0x09C 0`, `0x0A0 0`, **`0x0A4 0x01000001`**, `0x0A8 0x01000000`, ModeDec DMem `0`, `0` | `0x0A4` bit 0 clear → ModeDec has **no enabled candidate** in an I slice, a real fault and the first one this area could produce; any other mismatch → §1–§2 model wrong |
| **P2** | **wire `0xFCE4 = 1`** ("code MBs in I slices as I_PCM"), plus `session_coded_kb=2048` because I_PCM at 720p is ~1.39 MB and the default buffer is 1 384 448 B | IntraEst `0x1D0/1D4/1D8` = **`0x01000000` ×3** (positive control that the byte arrived); IntraEst DMem `0x40D448000` = **`0x200`** (bit 9, `0x612e4`/`0x61370`); ModeDec unchanged | **Frame still 2709 B of I_16x16** → the macroblock type the pipe codes does not follow the firmware's intra configuration at all; stop looking at configuration and look at whether the decision stages feed the entropy coder. **I_PCM with the ramp in it** → pixels reach the pipe and the loss is in prediction/residual. **I_PCM with flat grey** → the pipe never sees our source. All three are informative. |
| **P3** | `session_frames=2` (IDR then P), nothing else. Frame 0's diag is the f40 control; frame 1's is the P prediction. | P frame: `0x0A4 0x01000001`, **`0x0A8 0x01000001`**, `0x0AC..0x0C4 0x01000001`, `0x0C8..0x0F8 0x01000000`, **`0x0FC..0x104 0x01000001`**; IntraEst unchanged | Settles §2.4 either way: `0x104` = `0x00000001` → the register has no bit 24, the I-frame zero is benign; `0x104` = `0x01000001` → it does, and the I-frame zero has a writer we have not found. Any other pattern → the slice-type index model is wrong. |
| P4 | `profile_idc=100` (so PPS `transform_8x8_mode_flag = 1`, driver `ave_cmd.c:581`) **and** wire `0xFCEC = 2` | IntraEst **`0x1D4 = 0x01000001`**; ReconLuma `0x40D28A088 = 2` | Registers unchanged → `0xFCEC` is not the wire offset. Bitstream is only weakly informative (SPS/PPS bytes change regardless) |
| P5 | wire `0xFFA8 = 64` (`MD_IntraLambda`) | `0x40D26A09C = (64·0x80 + 0x200) >> 10 = 8`; nothing else | Register stays 0 → the RC-block mapping (§4.1) is wrong. Apple's values for this word are unknown, and with one candidate enabled in an I slice λ cannot change the choice; register-level check only |

Ranking rationale: P1 costs nothing and is the only direct test of the one
remaining failure mode in this area (`0x0A4` bit 0). P2 is the most
discriminating experiment this document can offer on the actual symptom —
it makes the firmware request an MB type that cannot be confused with the
current output, and the result splits the remaining hypotheses three ways.
P3 closes §2.4 and validates the model for P frames at the same time. P4
and P5 only check mappings.

Driver shape for P2/P4/P5: the same `wr8`/`wr32`-if-non-zero pattern as
`src_go_bits`/`dbg_bits` (`driver/ave_cmd.c:480`–`489`), with the layout
offsets `0xFCE4`, `0xFCEC`, `0xFFA8` in the 13.5 table next to `.dbg_bits`
(`driver/ave_abi.h:1622`) and module parameters defaulting to 0.

Firmware logging will not confirm P2/P4 by itself: see §7.

---

## 7. Side finding: why f41 printed no `QPY` line

`CController::Print` gates on `this+408` (docs/70 §6). The only AVC writers of
that byte:

```
584d8  ldrb w23,[x19,#408]            ; SetTranscode: save
58544  ldrb w9,[x19,#2684]            ;   ctrl+0xA7C (= wire 0xFCD8)
58550  ubfx w9,w9,#5,#1
58560  strb w9,[x19,#408]             ;   gate = bit 5
 ...   (the "Set Transcode" / "AVC::" prints f41 captured)
5949c  strb w23,[x19,#408]            ; restore
513ac  strb w8,[x24,#408]             ; ProcessLRMEStart, from another byte
```

**[C].** `setPipe`, which holds every `AVC COMMON::` print (`QPY` at `0x566a8`,
`mode_8x8_transform` at `0x56714`), runs with the gate at its saved value, not
at `0xFCD8` bit 5. That is exactly f41: SetTranscode's nine lines appeared and
setPipe's did not. docs/53's "what gates `0x566a8` is [U]" is answered: the
gate is only held open inside `SetTranscode`. **[C]**

---

## 8. Corrections to earlier documents (not edited here)

- **[70](70-intraest.md) §1.2**, line `26a`: the mask is `0x00080001` (bits 0
  and 19), not `0x08000001`. Bit 19 requires `chroma_format_idc == 0`, so the
  patch is dead code for 4:2:0. §5.2.
- **[70](70-intraest.md) §3 row 2 and §9 rank 2**: "a zeroed per-client struct
  replaces the defaults with 0" — the struct is the **per-slice-type** struct
  (0 = I, 1 = P, 2 = B), it is not zeroed, and `0x01000000` is written on
  purpose for every I slice. Rank 2 is closed. §0, §2.
- **[70](70-intraest.md) §1.3**: the per-QP words' bit 0 comes from
  `ctrl+0x2E0E4 + 12·[ctrl+0x1BA4]`, not from the per-QP entry. §3.
- **[70](70-intraest.md) §6 / §9.2**: `0xFCD8` bit 5 opens the print gate only
  inside `SetTranscode`; it cannot make `setPipe` print. §7.
- **[70](70-intraest.md) §8**, row `0xFCE4`: this is
  `enable_IPCM_in_IntraSlice` — "code macro-blocks in I slices as I_PCM" —
  not an unexplained "disable intra" arm. §3, §4.2.
- **[53](53-first-frame.md)** f39/f40 "Rank 2 … something rewrote the seed's
  low bit … a better lead than before": it was `InitEncodingParameters`,
  by design. Only the 23rd word's zero is open (§2.4).
- **[70](70-intraest.md) §1.1 / §11**: `objdump … <(python3 …)` prints
  "not an ordinary file" and nothing else with this binutils; §9 below writes
  the images to a temp directory instead.
- **[62](62-kext-field-map.md) §6.5**: add wire `0xFCDC..0xFCF4` and
  `0xFF98..0xFFAC` from §4.

---

## 9. Reproduce

```sh
# the model, with its 24 instruction anchors checked against the blob
python3 tools/modedec_costs.py                      # our I frame vs f40
python3 tools/modedec_costs.py --slice P            # P3
python3 tools/modedec_costs.py --fce4 1             # P2
python3 tools/modedec_costs.py --fcec 2             # P4

# seed (SetDefaultParameters), and ProcessInit's order
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x46770 -n 0x30
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x468b4 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x47490 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x47de0 -n 0x60
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4de2c -n 0x20
# slice type -> struct index
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x481b0 -n 0x20
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x482bc -n 0x0c
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x490e0 -n 0x0c
# host bytes -> ctrl (0xFCD8 block) and RC words -> ctrl+0xB10..
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cedc -n 0x90
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cdf0 -n 0x14
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d3d4 -n 0x24
# the enable loop, the pair, mode_8x8, I_PCM
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5e444 -n 0x410
# setPipe: lambdas, per-QP words, the 23 words, words 0/1
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x56330 -n 0x30
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57098 -n 0x5a0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57818 -n 0x18
# ProcessPipeReset's seed
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4f218 -n 0x64
# ConfigureMCPUs: w20/w21 into IntraEst and ModeDec DMem
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x61294 -n 0x1b0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x615a4 -n 0x1c8
# the print gate
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x584d8 -n 0x4
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x58544 -n 0x20
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5949c -n 0x4
# names: AVC DebugInit, HEVC DebugInit
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4e64c -n 0xb0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x39e5c -n 0x440 | grep -E 'ldr|adr'

# the two MCPU handlers (objdump refuses a pipe, so write the images out)
T=$(mktemp -d)
python3 -c "d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
for n,va,sz in (('modedec',0xe37a0,0xcde),('intraest',0xe3350,0x446)):
    open('$T/'+n+'.bin','wb').write(d[va+0x4000:va+0x4000+sz])"
objdump -D -b binary -m arm -M force-thumb $T/modedec.bin  | sed -n '/^ 282:/,/^ 510:/p'
objdump -D -b binary -m arm -M force-thumb $T/intraest.bin | sed -n '/^ 26a:/,/^ 308:/p'

# the tables
python3 -c "import struct;d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read();\
[print(hex(v),[hex(x) for x in struct.unpack_from('<%d%s'%(n,f),d,v+0x4000)]) for v,f,n in \
((0xd80d0,'H',52),(0xd8138,'I',52),(0xd8208,'I',52),(0xd8048,'I',17),(0xd808c,'I',17),(0xd7fc8,'I',2))]"
```

Scans used in §2.4, over a full `__TEXT` disassembly
(`AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x0 -n 0xec000 > fw.txt`):
`mov`+`movk #0x2, lsl #16` pairs resolving into `0x2CA2C..0x2E36C`, and
`#0x126, lsl #16` pairs into `0x1260000..0x1270000`; `grep -E '#0x2(c|d|e), lsl #12'`;
`grep -E '#(1796|304)\]'`. The second scan reproduces docs/70 §1.3's three
ModeDec addresses, which is its control. The seed coverage claim in §1.1 was
checked by tracking `x21`/`x10`/`x11` stores in `0x474b0..0x47e30`: every word
of `ctrl+0x2DF44..0x2E36B` is written.
