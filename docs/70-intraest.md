# What dispatches work to IntraEst, and what stops it

Static analysis of the macOS 13.5 firmware and kext, cross-read against
`results/f21-1789893453.kmsg` and docs/53's F21-F24 entries. **Nothing here was
run on hardware.**

Conventions as [62](62-kext-field-map.md) and [69](69-source-to-modedec.md):
firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`); **wire** = byte
offset in the command on IPC channel 1; VP + `0x60` = wire; `base` = the
firmware's MMIO window `*(0x21a7b8)` = AP `0x40C000000`, so firmware offset `X`
is AP `0x40C000000 + X`. Labels per [00](00-methodology.md): **[C]** read from
an instruction (VA cited), **[I]** inferred with the chain stated, **[U]**
unknown.

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x55e2c -n 0xa0
```

**The `AVE_MACOS=13.5` is not optional.**

---

## 0. The short version

1. **The dispatch is hardware.** Nothing in the ASC firmware, and nothing in any
   other MCPU image, posts work to IntraEst. The IntraEst MCPU's `main` unmasks
   **its own IRQ 27** and parks in `wfi`; the intra-estimation hardware raises
   that IRQ once per macroblock, the MCPU programs QP/λ/mode words into the
   stage's register block and writes the per-MB **go** at `0x40D24A080 = 1`.
   §1.
2. **The premise needs retracting: "IntraEst never ran" is not what F21/F22
   measured.** All three signals the conclusion rests on are exactly what a
   *fully running* IntraEst leaves behind:
   - `hif +0xc` and `+0x10` = 0 — **the handler writes 0 to both at the top of
     every macroblock and never writes them again**. MbInput, which
     demonstrably ran 3845 macroblocks, reads `0`/`0` in the same dump. The
     driver's own log calls them "counters"; they are not.
   - `hif +0x14` = 0 — the handler's **last** post is `0x60000000`, whose low
     bits are zero. MbInput reads `0x24` because *its* last post carries
     payload `0x24`. A completed IntraEst leaves `0x00000000`.
   - `curMB = 0` — read-only hardware state, **with no negative control taken**.
     ModeDecision has the byte-identical register at `0x40D263180` and it has
     never been read.

   The one thing the dump *does* prove is positive: `hif +0 = 0x2000` is written
   only by IntraEst's `main`, so the core booted, ran to `main`, unmasked IRQ 27
   and parked. §2.
3. **The register the driver prints as `0x40D143180` is `0x40D243180`.** The
   code is right (`AVE_BANK_DPE + 0x143180`, bank 0 = `0x40D100000`); the AP
   label in the prompt and in docs/53 is off by `0x100000`. §4.
4. **A host word turns the firmware's own instrumentation on.** `ctrl+0xA7C` is
   a debug bitfield copied verbatim from **wire `0xFCD8` (u32)**, which the
   driver has always sent as zero. Bit 5 sets the per-controller print gate that
   `CController::Print` tests; with it clear, every `AVC COMMON::` line —
   including `QPY %d nQuant %d`, `disable_intra_mode: %x`, `search_range`,
   `MESATDSCALING`, `Context Height`, and `ProcessAvcPipeDone currMbRow %d
   pic_height_in_mbs %d` — is dropped before it reaches the TERMINAL ring the
   driver already drains. §6.
5. **The AVE_DPE tables are correct** — byte-for-byte against the kext, right
   set, right order, right counts — and they **cannot** be the mode-decision
   cost ladder: the cost/λ path is `0x40D24A1C8`/`0x40D24A1CC` and the
   twenty-three registers at `0x40D26A0AC..0x40D26A104`, none of which is in the
   DPE block. §5.
6. **48 rows is 45 rounded up to a multiple of four**, which is the unit the
   pipe counts MB rows in throughout `setPipe`. No host field sets it. §7.

---

## 1. The dispatch path

### 1.1 The IntraEst MCPU image, read end to end

Image at fw `0xe3350`, `0x446` bytes, Thumb ([58](58-pipe-start.md) §1.2).

```sh
objdump -D -b binary -m arm -M force-thumb <(python3 -c "import sys;\
 d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read();\
 sys.stdout.buffer.write(d[0xe3350+0x4000:0xe3350+0x4000+0x446])")
```

Vector table: `SP = 0x10001000`, reset `0x125`, and **image offset `0xac`
holds `0x1a9`** — vector index 43 = 16 + 27, i.e. **IRQ 27**. **[C]**

`main` (image `0x184`):

```
184  NVIC_ISER0 (0xE000E100) = 0x08000000      ; enable IRQ 27
192  r0 = 0x41242000                           ; IntraEst host-if, MCPU view
19a  r1 = [r0] ; orr #0x2000 ; [r0] = r1       ; set bit 13 of hif +0
1a4  wfi ; b .
```

**[C].** `0x41242000` is `0x40000000 + 0x1242000`; the MCPU sees the encoder
register file at `0x40000000 + (firmware offset)` ([58](58-pipe-start.md) §1.3),
so this is AP `0x40D242000` = the driver's `AVE_BANK_DPE + 0x142000`.

**This is the only writer of bit 13 in `+0`, and F21/F22/F23/F24 all read
`+0 = 0x00002000`.** The IntraEst core therefore boots, copies nothing (its
`.data` is zero-length), zeroes 12 bytes of `.bss` at `0x10000768`, runs `main`,
unmasks IRQ 27 and parks. **[C]**

### 1.2 The per-macroblock handler (image `0x1a8`..`0x352`)

```
1a8  r2 = 0x41243180                       ; per-MB context block
1bc  r1 = ldrh [0x41243000]                ; MB word
1be  r0 = 0x41242014                       ; hif +0x14
1c8  r3 = [0x41243228]
1cc  [r0-8]  = 0                           ; hif +0x0C = 0
1d0  [r0-4]  = 0                           ; hif +0x10 = 0
1d6  r1 >>= 10                             ; per-MB QP = bits 10..15 of 0x41243000
1dc  [r0]    = 0x80000025                  ; hif +0x14: request
1e0..1fa     poll until bit 31 clear       ; grant
1fc  r4 = [0x41243184]
20a  if (r4 < 0x10000000) goto 0x308       ; nothing to program
214  r4 = [DMem 0x10000764]                ; ASC-supplied mode word
21e  if (r4 bit2)  -> 0x22e : 0x4124a1cc = [0x41243008], 0x4124a1c8 = [0x41243004]
222  if (r4 bit4)  -> 0x248 : QP path, skipped when QP == 63
226  if (r4 bit0)  -> 0x24c with r1 = [0x41243228]
     else            -> 0x26a (programs nothing)
24c  0x4124a1c8 = QP                        ; QPY
250  if (byte [DMem 0x10000000] bit 0):
262    r1 = imageTable[0x412 + QP] ; <<4
266    0x4124a1cc = r1                      ; nQuant / lambda
26a  if ((word [DMem 0x10000000] & 0x08000001) == 0x08000001): patch 0x4124a1d4 bit0
294  if (word [DMem 0x10000000] bit 12):    0x4124a1d0, 0x4124a1d4, 0x4124a1d8
2d0  if (byte [DMem 0x10000002] bit 2):     save/restore 0x4124a1d4/d8 in DMem
308  [0x41243254] = [0x41243258] = [0x4124325c] = 0
316  [r0] = 0x60000000 ; poll until bits 29-30 clear
346  [0x4124a080] = 1                       ; per-MB GO
34e  [r0-12] = 0x2000                       ; ack hif +8 bit 13
```

**[C]** for every line. This matches and completes
[59](59-row1-stall.md) §2.1.

### 1.3 The ASC side: five words and one enable, and nothing else

A whole-image MMIO map (every `str`/`ldr` whose base resolves to
`*(0x21a7b8) + constant`, constants tracked through `mov`/`movk`/`add`/`sub`
and register-offset forms) finds **exactly six accesses in
`0x1240000..0x1250000`**:

| AP | fw VA | function | value |
|---|---|---|---|
| `0x40D24A1C8` | `0x55e98` | `CAVCController::setPipe` | **QPY**, `ldrb w24,[x21,#789]` (fw `0x55e38`) |
| `0x40D24A1CC` | `0x55eb8` | `setPipe` | **nQuant**, `table[0xd8414 + 4*(max(QPY,12)-12)] << 4` |
| `0x40D24A1D0` | `0x57130` | `setPipe` | per-QP word 0 |
| `0x40D24A1D4` | `0x57120` | `setPipe` | per-QP word 1 |
| `0x40D24A1D8` | `0x57118` | `setPipe` | per-QP word 2, bit 0 from another ctrl byte |
| `0x40D24A394` | `0x60338` | `ConfigureMCPUs` | **`= 1`, the stage enable**, skipped when `bSkipMcpu` (`ctrl+0x23FE4`) is set |

**[C].** The two log strings that name the first pair are `0xc6971`
`"AVC COMMON:: QPY %d nQuant %d\n"` (emitted at fw `0x566a8`) — see §6.

The per-QP words at `+0x1D0..+0x1D8` come from a 12-byte-per-QP table at
`ctrl+0x2E0FC + 12*QPY` (fw `0x570a4`..`0x570cc`). **[C]**

**No firmware instruction reads or writes `0x1243000..0x1243fff`** (the per-MB
context block the MCPU reads) in the whole image, exactly as with
`0x1170000`/`0x1170004` ([69](69-source-to-modedec.md) §3.4). That block is
populated by hardware. **[C]** for the scan; **[I]** that no table-driven access
exists (Trap 3) — the same scan reproduces every write in
[62](62-kext-field-map.md) §6.2 and [69](69-source-to-modedec.md) §1.1, which is
the control on it.

### 1.4 Answer to "who posts work to IntraEst"

**A hardware sequencer, through IRQ 27 on the IntraEst MCPU.** Not ModeDec, not
MbInput, not the ASC firmware.

- The ASC firmware never touches IntraEst's host-interface block or its per-MB
  context block (§1.3). **[C]**
- Every register constant in the nine MCPU images falls inside that core's own
  stage blocks ([59](59-row1-stall.md) §2.1); ModeDec's image addresses only
  `0x4126xxxx`, IntraEst's only `0x4124xxxx`. No MCPU dispatches another.
  **[C]**
- The only host-visible gate on the ASC side is `0x40D24A394 = 1`, and it is
  written whenever `bSkipMcpu == 0`, which our Config selects
  ([58](58-pipe-start.md) §1.1). **[C]**

So the gating condition, stated exactly: **IntraEst receives a macroblock iff
the hardware intra-estimation stage raises IRQ 27, which it does per macroblock
once `0x40D24A394` is 1 and the pipe is running.** There is no per-frame
firmware "start IntraEst" call to point at.

---

## 2. Why "IntraEst never ran" does not follow from the dump

This is the correction that matters most, and it is a textbook
[00](00-methodology.md) Trap 2 — *a test that cannot return a different answer
for the case you believe differs*.

| signal | F21/F22 value | what a **running** IntraEst leaves | discriminates? |
|---|---|---|---|
| `hif +0x0C` | 0 | **0** — handler writes 0 at image `0x1cc` every MB, never again | **no** |
| `hif +0x10` | 0 | **0** — handler writes 0 at image `0x1d0` every MB, never again | **no** |
| `hif +0x14` | 0 | **0** — last post is `0x60000000` (image `0x31a`), low bits zero | **no** |
| `hif +0` | `0x2000` | `0x2000` — written by `main` | positive: the core ran |
| `curMB 0x40D243180` | 0 | **[U]** — hardware state, never written by firmware or MCPU | untested |

**The negative control already in the same log:** `MbInput` produced and
consumed 3845 macroblocks, and its `hif +0xc` and `+0x10` also read **0**. A
register pair that reads 0 for a stage that provably ran cannot be evidence that
another stage did not run. **[C]** (both values are in
`results/f21-1789893453.kmsg`).

`ModeDec +0xc/+0x10 = 0x26` and `ReconLuma = 0x2b` are not counters either —
3845 macroblocks would not leave 0x26. Those MCPUs simply write something else
into the same two words later in their handlers; IntraEst's handler does not.
**[I]**, from IntraEst's image being read end to end and ModeDec's prologue
(`0x1be`–`0x1d0`) zeroing the identical pair before posting `0x80000126`.
**[C]** for ModeDec's prologue.

`curMB` is the only untested signal, and ModeDecision carries the **byte-
identical block**: ModeDec's handler reads `ldrh [0x41263180 - 0x180]`,
`ldr [0x41263180 + 168]` and `ldr [0x41263180 + 4]` with the same
`cmp #0x10000000` test (image `0x1cc`, `0x1da`, `0x1e6`, `0x1ec`, `0x282`,
`0x28a`). **[C]** So `0x40D263180` is a true negative control for
`0x40D243180`, and it has never been read.

**Consequence.** "IntraEst never received a macroblock" is currently **[U]**,
not **[C]**, and docs/53's "The answer, probably" section and
[69](69-source-to-modedec.md) §0.6 / §5.2 should be annotated accordingly. The
symptom to explain is the one that *is* measured: **3600 macroblocks coded as
the same `mb_type` with no coefficients anywhere, independent of the source.**

---

## 3. Every configuration that produces a whole frame of I_16x16 DC with no
residual, and the host field behind each

Ranked. Rows 1-3 are new; rows 6-9 restate
[69](69-source-to-modedec.md) §5.3 with what has since been measured.

| # | mechanism | register / host field | what we send | status |
|---:|---|---|---|---|
| **1** | **The quantiser the hardware actually uses is not QP 30.** `setPipe` takes QPY from `[x21,#789]` (fw `0x55e38`, `x21 = [sp,#176]`; which structure that is, **[U]**) and derives nQuant = `table[0xd8414 + 4*(max(QPY,12)-12)] << 4`, then writes both to `0x40D24A1C8`/`0x40D24A1CC` and to ReconLuma (`0x40D28A090/94`) and CAVLC (`0x40D2DA08C..98`) | Start_AVC `qp_i/qp_p/qp_b` (wire `0xFFB4/B8/BC`), `qp_min` `0xFF88`, `qp_max` `0xFF8C`; RC mode wire `0x2A8` | `session_qp = 30`, min 10, max 51, `AVE_RC_FIXQP` | **Untested.** Six registers hold the answer and none has ever been read. A QP that arrives as 51 (or a λ of 0) codes every residual to zero for *any* source and gives the same byte count each time |
| **2** | **The twenty-three ModeDecision cost registers are written with zero.** `ProcessPipeReset` writes `0x01000001` to `0x40D26A0AC..0x40D26A104` (fw `0x4f218`–`0x4f278`); `setPipe` then overwrites each from `ctrl + 0x2CA2C + client*1800 + 0x6AC + 4k`, **skipping the write only when the value already equals `0x01000001`** (fw `0x57128`–`0x57638`). A zeroed per-client struct therefore *replaces* the defaults with 0 | none directly — `CAVCController::SetDefaultParameters` seeds the struct with `0x01000001` (fw `0x468b4`: `mov w9,#0x01000001` / `dup v0.4s,w9`), and that runs only when `ctrl+0xA68` is 0 | n/a | **Untested, and cheap to read.** 23 read-only words in bank 0 |
| **3** | **IntraEst's DMem mode word is zero, so the MCPU programs neither QP nor λ per macroblock.** `ConfigureMCPUs` assembles the word at MCPU DMem `0x10000000` (AP `0x40D448000`) at fw `0x61294`–`0x61440` from seven `ctrl+0x23FC4`-relative bytes; the handler's every branch at image `0x21e`–`0x2da` tests it | bit 0 ← `ctrl+0x23FC7`; **bit 4 ← VP+8 bit 1**; **bit 7 ← VP+8 bit 0 (or VP+0x1D)**; **bit 9 ← wire `0xFCE4`**; **bit 10 ← VP+8 bit 2**; bit 18 ← `ctrl+0x2413B` | **wire `0x68` (VP+8) = 0**, wire `0xFCE4` = 0 | **Open.** With the word zero the handler still runs and still kicks `0x40D24A080`, but leaves QP/λ at whatever `setPipe` wrote. Harmful only if `setPipe`'s values are also wrong — i.e. this compounds row 1 rather than standing alone |
| 4 | `disable_intra_mode` | wire `0xFCE3` u8 → `ctrl+0x23FDF`; used at fw `0x5e748` (`cbz` → if **non-zero**, clear bit 0 of two ctrl words) | **0** | **Closed.** 0 is the enabling value |
| 5 | "Disabling Intra Modes!!!!!!!!!!!!!" (string `0xc5466`, fw `0x5e814`) | wire `0xFCE4` u8 → `ctrl+0x23FDE`; `cbz w8` at fw `0x5e80c` skips the whole arm | **0** | **Closed as a cause.** 0 skips it. It is *not* closed as row 3's bit 9 |
| 6 | `bEnableFwOverride` / `bEnableMBInputCtrl` divert MB input | VIDEO_PARAMS booleans + MBInputCtrl table wire `0xFCC8` | both 0, table unpublished | **Closed** (firmware asserts they are not both set; [69](69-source-to-modedec.md) §5.3 row 5) |
| 7 | source stream not translated | DT `iommus` | SIDs 0, 1, 15 | **Closed by F22** |
| 8 | `SRCDMAGO` mode bits | wire `0xFECC`, `0xFCE9` | swept 0 and 1 | **Closed by F23/F24** — both confirmed wired, neither changes the picture |
| 9 | format word bits 0..7 of `0x40D12000C` | none (firmware always leaves 0) | 0 | **Closed** |

Note what rows 1-3 have in common and what the closed rows do not: they all
produce **"the encoder computed a difference and then threw it away"**, which is
the observation, whereas rows 6-9 produce "the encoder never saw the pixels",
which four boots have now failed to demonstrate.

---

## 4. `0x40D243180` and the IntraEst host-interface block

### 4.1 Address correction

The driver reads `ave_read(ave, AVE_BANK_DPE, 0x143180)`
(`driver/ave_session.c:1946`). `AVE_BANK_DPE` is bank 0 =
**`0x40D100000`** (`driver/ave_hw.h:33`), so the register is AP
**`0x40D243180`** = firmware offset `0x1243180` = the MCPU's `0x41243180`. The
code is correct; the AP label used in docs/53 and in the F21-F24 write-ups
(`0x40D143180`) is wrong by `0x100000`. **[C]**

### 4.2 The per-MB context block at `0x40D243000`

| AP | MCPU view | used by the handler as |
|---|---|---|
| `0x40D243000` | `0x41243000` | u16; **bits 10..15 = the macroblock QP** (`ldrh` then `lsr #10`, image `0x1bc`/`0x1d6`); QP 63 means "leave the stage QP alone" (image `0x248`) |
| `0x40D243004`, `+8` | `0x41243004/8` | alternative QP / λ pair, taken when DMem word bit 2 is set |
| `0x40D243180` | `0x41243180` | **`curMB`** — the block's base word, read but never written by firmware or MCPU; `[+4]` is compared against `0x10000000` and a value below that makes the handler program nothing |
| `0x40D243184` | `+4` | the "is there work" word (see above) |
| `0x40D243228` | `+0xa8` | a second QP source |
| `0x40D2432B4` | `+0xb4` | u16 folded into `0x40D24A1D4` bits 16..31 |
| `0x40D243254/8/C` | `+0xd4/d8/dc` | zeroed by the handler tail |

**[C]** for every offset. What a **correctly running** IntraEst would read:
`0x40D243000` with a sane QP in bits 10..15 (30 → `0x7800`), `0x40D243184`
`>= 0x10000000`, and `0x40D243180` non-zero *if* the hardware advances it — that
last is **[U]** and is exactly what the ModeDec control in §2 settles.

### 4.3 The host-interface block at `0x40D242000`

| off | meaning | value a completed frame leaves |
|---|---|---|
| `+0x00` | stage enable; MCPU `main` sets bit 13 | `0x00002000` |
| `+0x04` | zeroed by `StartUnit` | 0 |
| `+0x08` | raw pending / ack; handler writes `0x2000` to ack IRQ 27 | `0x0000401f` observed; **[U]** |
| `+0x0C` | handler writes 0 every MB | **0, always** |
| `+0x10` | handler writes 0 every MB | **0, always** |
| `+0x14` | request/grant: `0x80000025` then `0x60000000`, each polled until its high bits clear | **0, always** |

**[C]** for the writes. The row that matters: **none of `+0x0c`, `+0x10`,
`+0x14` can distinguish a stage that ran from one that did not, for this
particular MCPU image.**

---

## 5. The AVE_DPE tunables — checked, and cleared

### 5.1 The tables are right

Regenerated from `data/blobs/macos-13.5/kc.macho` and compared entry by entry
against `driver/ave_dpe_tables.h`:

```
CfgSet_Castor_6000 (file 0x22c018, chained-fixup low 32 bits = file offset):
  +0x20 CAT Default  0x22c9c0  count 1
  +0x30 CAT 8bit     0x22c9d0  count 0
  +0x40 CAT 10bit    0x22c9d4  count 0
  +0x50 CAC Default  0x22c9d8  count 0x7c (124)
  +0x60 CAC 8bit     0x22d198  count 0x7b (123)
  +0x70 CAC 10bit    0x22d948  count 0x7b (123)
```

All 248 `{offset, clear, set}` triples match the header exactly, every entry is
width 4, and the 10-bit table is **not** identical to Default (it starts at
offset 4; Default's `+0x000` entry `{clear 0x01ff0000, set 0x00030000}` is
Default-only). **[C]** — and that corrects [58](58-pipe-start.md) §5.1's
"10-bit equals Default".

### 5.2 The right set, in the right order, for an 8-bit AVC session

`AVE_DPE::ApplyType(this, type)` (kext `0xfffffe0008ee4174`) applies the CAT
table for `type` then the CAC table for `type`, with `type < 3`
(`0xfffffe0008ee425c`: `cmp w20,#3 / b.ge`). `AVE_DPE::Reset`
(`0xfffffe0008ee4f48`) does:

```
ApplyType(0)                                  ; Default
if   [this+32] != 0  -> ApplyType(2)          ; a 10-bit client is registered
elif [this+28] != 0  -> ApplyType(1)          ; an 8-bit client is registered
else                 -> (nothing)
Enable()                                      ; 0x40D1DC400 |= 3, 0x40D1DC000 |= 1
```

**[C]** (`0x4f50`, `0x4f54`, `0x4fb4`→`0x5068` `mov w1,#2`, `0x5018`→`0x50bc`
`mov w1,#1`, `0x50c4`, `0x50cc`). So the end state macOS reaches for one 8-bit
client is **Default then 8-bit then Enable**, which is exactly what
`ave_dpe_program()` does (`driver/ave_drv.c:387`-`396`). The 8-bit table has no
`+0x000` entry, so Default's `[24:16] = 3` survives — also correct.

**Nothing else in the block is written.** `AVE_DPE::Add`
(`0xfffffe0008ee55d4`) only bumps the per-bit-depth client counts and applies
type 2 or 1; `AVE_DPE::Enable` is the two OR-writes above. The firmware never
touches `0x11D8000..0x11E0000` (immediate scan with a positive control at
`0x11E0000..0x11E0200`). **[C]**

### 5.3 Could a wrong DPE table make DC always cheapest? No

The mode-decision cost path is **elsewhere and now named**:

| what | AP | source |
|---|---|---|
| IntraEst QPY | `0x40D24A1C8` | `setPipe` fw `0x55e98` |
| IntraEst λ (`nQuant`) | `0x40D24A1CC` | `setPipe` fw `0x55eb8`, from the λ table at fw `0xd8414` |
| IntraEst per-QP mode words | `0x40D24A1D0/D4/D8` | `setPipe` fw `0x57118`-`0x57130`, from `ctrl+0x2E0FC + 12*QP` |
| ModeDecision cost ladder, 23 words | `0x40D26A0AC..0x40D26A104` | `ProcessPipeReset` default `0x01000001`, `setPipe` override from `ctrl+0x2CA2C + client*1800 + 0x6AC` |
| `MESATDSCALING` | `0x40D190630` | `setPipe` fw `0x5668c`, log string `0xc6990` |

**[C].** None of these is in `0x40D1DC000`/`0x40D1DC400`. The DPE block is 40
single-bit enables, one explicit disable, and 41 pairs of ≤ 9-bit values that
change with **bit depth** — not with QP, not with slice type, not with mode.
**[I]**, but the negative half is **[C]**: the cost registers have identified
writers and they are not DPE.

### 5.4 One loose end worth a line in the next log

F21 reads back `DC400 = 0x00032901` after the driver's `|= 3`. Bit 0 stuck, bit
1 did not. Either bit 1 of `0x40D1DC400` is not a storage bit on this instance,
or something clears it. macOS does the same write, so this is not a divergence
from Apple — but the driver's read-back check deliberately masks `~3` at
offset 0 and so cannot see it. **[U]**, low priority.

---

## 6. The firmware will answer all of this itself: wire `0xFCD8`

`CController::Print` (fw `0x924d4`) is:

```
924d4  ldrb w8, [x0, #408]        ; x0 = the controller object
924e4  cbz  w8, 0x92508           ; -> return 0, print nothing
924f8  bl   0x9496c               ; vsnprintf + CLogger
```

**[C].** Every `AVC COMMON::` line in `setPipe`, and every line
`CAVCController::DebugInit` emits, goes through it.

`this+408` is set in `setPipe`:

```
58544  ldrb w9, [x19, #2684]      ; ctrl+0xA7C
58550  ubfx w9, w9, #5, #1        ; bit 5
58560  strb w9, [x19, #408]
```

**[C].** And `ctrl+0xA7C` is a verbatim copy of a host word:

```
5cedc  ldr  w8, [x23, #1304]      ; x23 = VP + 0xF760  -> VP + 0xFC78
5cee0  str  w8, [x19, #2684]
```

**[C].** With the anchor `[x23,#1320]` = wire `0xFCE8`
([69](69-source-to-modedec.md) §3.2), `wire = 0xF7C0 + index`, so
`[x23,#1304]` is **wire `0xFCD8`, u32** — the row
[62](62-kext-field-map.md) §6.5 lists with an empty "goes to" column. It is the
controller's debug-verbosity bitfield:

| bit | effect | VA |
|---|---|---|
| any bit set | `CAVCController::DebugInit` is called at all | `0x4de5c` `cbz` → `0x4de68` `bl 0x4dea4` |
| **5** | **`CController::Print` gate — every `AVC COMMON::` line** | `0x58550` |
| 1 | extra section in `ProcessPipeReset`'s dump | `0x4ed58` `tbnz #1` |
| 3 | extra `DebugInit` section (SPS/PPS/RC dump) | `0x4e704` `tbz #3` |
| 4 | extra `DebugInit` section | `0x4decc` `tbnz #4` |
| 7 | two more per-frame dumps | `0x5b7a8`, `0x5c690` `tbz #7` |

**[C]** for every test. The driver already drains the TERMINAL channel and
prints firmware lines as `fw[n]| ...` (`driver/ave_ipc.c:370`); F13 shows it
working (`Cveseb buffer write full!`, `ENC: StartCount 1-1-1-0`). F21 captured
exactly one firmware line because **the AVC controller's print gate has been
zero in every run we have ever done.**

Lines that would appear with bit 5 set, all already in the image:

| string VA | text | answers |
|---|---|---|
| `0xc6971` | `AVC COMMON:: QPY %d nQuant %d` | §3 row 1 |
| `0xc6a02` | `AVC COMMON:: disable_intra_mode: %x` | §3 row 4 |
| `0xc69de` | `AVC COMMON:: disable_skip_mode: %x` | — |
| `0xc69bf` | `AVC COMMON:: search_range: %x` | — |
| `0xc6a27` | `AVC COMMON:: mode_8x8_transform: %x` | — |
| `0xc6990` | `AVC_MODEDECISIONCONFIG_MESATDSCALING(%d) = %x` | §5.3 |
| `0xc6186` | `Context Height 0x%x` | §7 |
| `0xc4bef` | `ProcessAvcPipeDone currMbRow %d pic_height_in_mbs %d …` | §7 |
| `0xc5466` | `Disabling Intra Modes!!!!!!!!!!!!!` | §3 row 5 |
| `0xc7520` | `Disable all intra modes in intra slices: %d …` (DebugInit) | §3 row 5 |

**Risk.** `CLoggerInterProcessor::PrintInterProcessor` allocates from shared
memory and sends synchronously; a very verbose setting slows the frame and can
overrun the 512-slot TERMINAL ring. Start with **bit 5 alone (`0xFCD8 = 0x20`)**,
which is per-frame traffic only, not `DebugInit`'s hundreds of lines. The
driver's `fwlog_rs` rate limit must be raised or the interesting lines will be
dropped in the kernel, not in the firmware.

---

## 7. 3845, and `y 47` in a 45-row picture

`3845 = 80 × 48 + 5`, and 48 is **45 rounded up to a multiple of four**. The
pipe counts macroblock rows in groups of four throughout:

- `setPipe` fw `0x58518`/`0x5851c`: `add w9, w9, #3 ; and w9, w9, #0xfffffffc`
  applied to an MB-row count derived from `ctrl+0xA94` (`pic_height_in_mbs`),
  `ctrl+0x1438` ("Context Height") and `ctrl+0x1408`, then floored at 8,
  shifted left 16 and masked with `0x01FC0000` before being stored at
  `[x8,#-252]` in the `0x119xxxx` block (fw `0x58534`; the exact offset depends
  on `x24`, which this pass did not resolve — **[U]**). **[C]** for the
  arithmetic.
- [59](59-row1-stall.md) §2.2 already records `rows = ctrl[5176] >> 2` for the
  neighbour-ring stride — "Context Height" is in units of **four** MB rows.
  **[C]**

So a 45-row picture is walked as 48 event rows by whatever runs on that grid,
the last three produce no coded macroblocks, and the coded header still reports
3600 of 3600. **[I]**, and it supersedes
[69](69-source-to-modedec.md) §4.3 candidate 1 by giving the rounding an
instruction rather than a plausibility argument.

**No host field sets 48.** The two geometry registers `setPipe` programs
(`0x40D111004` in MB units from `ctrl+0xA94/0xA98`, `0x40D11E0004` in pixels
from wire `0x60`/`0x64`) both carry 45/80 for our session, and the source reader
and `ProcessPipeDone` both agree on 45 ([69](69-source-to-modedec.md) §4.1).
`Context Height` (`ctrl+0x1438`) is firmware running state with no host writer
(fw `0x59dc8`, `0x59d30`, `0x5ba64`). **[C]**

This is benign and is not a reason for a grey picture.

---

## 8. New host fields located (additions to [62](62-kext-field-map.md) §6.5)

| wire | width | fw read | goes to | our value |
|---|---|---|---|---|
| **`0xFCD8`** | u32 | `0x5cedc` | **`ctrl+0xA7C`, the controller debug bitfield; bit 5 is the `CController::Print` gate (fw `0x924d4`, `0x58550`)** | **0** |
| `0xFCE3` | u8 | `0x5cf30` → `ctrl+0x23FDF` | `disable_intra_mode`; non-zero clears bit 0 of two `ctrl+0x2D…` words (fw `0x5e748`) | 0 |
| `0xFCE4` | u8 | `0x5cf58` → `ctrl+0x23FDE` | the "Disabling Intra Modes" arm (fw `0x5e808`) **and bit 9 of IntraEst's DMem mode word** (fw `0x612e4`/`0x61304`) | 0 |
| `0xFCE2` | u8 | `0x5cf28` → `ctrl+0x23FE3` | `disable_skip_mode`; also `0x40D19026C` bit 0 (fw `0x56408`, `0x56424`) | 0 |
| `0x68` (VP+8) | u32 | `0x5cf70` | **seven** flag bytes at `ctrl+0x23FD5..0x23FDB` from bits 27, 1, 0, 2, 3, 4, 5 — not six as [62](62-kext-field-map.md) §1.4 says. Bits 1, 0 and 2 reach IntraEst's DMem mode word at bits 4, 7 and 10 (fw `0x61294`, `0x61324`, `0x61328`) | **0** |

**[C]** for every read and every destination.

---

## 9. Ranked causes, and the change each implies

| rank | cause | host field / register | change |
|---:|---|---|---|
| **1** | **The QP or λ the hardware runs with is not the QP we asked for**, so every residual quantises to zero regardless of source | Start_AVC `qp_i/p/b`, `qp_min/max`, RC mode; lands in `0x40D24A1C8`/`0x40D24A1CC` | none yet — **read** the six registers and turn on the firmware's own `QPY %d nQuant %d` line (wire `0xFCD8 = 0x20`) |
| **2** | **The 23 ModeDecision cost words are zero** because `setPipe` overwrote the `0x01000001` reset defaults from a zeroed per-client struct | `ctrl+0x2CA2C + client*1800 + 0x6AC`, no direct host field | **read** `0x40D26A0AC..0x40D26A104`; if they are 0 rather than `0x01000001`, the per-client struct is the target |
| **3** | **IntraEst's DMem mode word is zero**, so the MCPU programs no per-MB QP or λ | wire `0x68` bits 0/1/2, wire `0xFCE4` | expose wire `0x68` as `session_vp8` (u32) alongside the existing pass-throughs; do not sweep blind — read `0x40D448000` first, it is in bank 0 and free |
| 4 | IntraEst really does get nothing | — | settle it with the `0x40D263180` negative control (below), not with more source-path sweeps |
| 5 | `0x40D1DC400` bit 1 does not latch | DPE Enable | note only |

### 9.1 The concrete driver change for rank 1

`driver/ave_abi.h`, in the 13.5 layout next to `src_cfg_byte`/`src_mode`:

```c
u32 dbg_bits;   /* wire 0xFCD8, u32 -> ctrl+0xA7C. Bit 5 is the
                 * CController::Print gate (fw 0x924d4 `ldrb w8,[x0,#408]`,
                 * set from ctrl+0xA7C bit 5 at fw 0x58550/0x58560).
                 * Bits 1/3/4/7 add DebugInit sections. */
```

wired the same way as `src_go_bits` (`ave_cmd.c` `wr32_opt`), with a module
parameter `session_dbg` defaulting to **0** and documented as `0x20`. Then raise
`ave->fwlog_rs` (or bypass it while `session_dbg` is non-zero) so the firmware's
lines reach the kernel log.

And fold these read-only bank-0 words into the existing diag line — all of them
are inside the mapped window and none has ever been read:

```
0x24A1C8 0x24A1CC 0x24A1D0 0x24A1D4 0x24A1D8 0x24A394   /* IntraEst */
0x26A0AC .. 0x26A104 (23 words)                          /* ModeDec cost ladder */
0x243000 0x243180 0x243184                               /* IntraEst per-MB ctx */
0x263000 0x263180 0x263184                               /* ModeDec  — the control */
0x448000 0x448764                                        /* IntraEst DMem mode words */
0x190630 0x19026C                                        /* MESATDSCALING, skip-mode word */
```

(bank `AVE_BANK_DPE`, i.e. the offsets above minus nothing — `0x143180` style;
`0x24A1C8` here means `ave_read(ave, AVE_BANK_DPE, 0x14A1C8)`, AP
`0x40D24A1C8`.)

### 9.2 The one-load experiment

**Set wire `0xFCD8 = 0x20` and un-rate-limit the firmware log. One variable,
one load, and it can come out "no".**

- If `fw[…]| AVC COMMON:: QPY 30 nQuant <n>` appears with a sane non-zero
  `nQuant`, **rank 1 is dead** and the same run's `0x40D26A0AC` dump decides
  rank 2 on the spot.
- If `QPY` is not 30, or `nQuant` is 0, rank 1 is the cause and the fix is on
  the Start_AVC QP path, not the source path.
- If **no** `AVC COMMON::` line appears at all, the answer is "no" to the whole
  mechanism: bit 5 is not the gate, or the AVC controller's print path is dead
  on 13.5 — and the run still returns the register dump in §9.1, including the
  `0x40D263180` control that settles whether `curMB = 0` ever meant anything.

This is worth a reboot in a way that F18/F22/F23/F24 were not: it does not
confirm a mapping we already believe, it makes the firmware state its own
parameters, and every one of the five open questions in this document has a
format string waiting for it.

---

## 10. Corrections and annotations to earlier documents (not edited here)

- **[53](53-first-frame.md)**, "The answer, probably: IntraEst never ran": the
  three MCPU registers it cites cannot distinguish a running IntraEst from an
  idle one (§2). `IntraEst +0xc/+0x10/+0x14 = 0` is what a *completed* IntraEst
  leaves, and MbInput — which ran 3845 macroblocks — reads `0`/`0` for the same
  pair in the same log. The claim should be marked **[U]** pending the
  `0x40D263180` control.
- **[53](53-first-frame.md)** and the F21-F24 sections: `IntraEst curMB` is AP
  **`0x40D243180`**, not `0x40D143180`. The driver reads the right register.
- **[69](69-source-to-modedec.md)** §0.6 and §5.2: same correction; "the
  residual is zero because intra estimation never ran" is **[I]** resting on a
  non-discriminating test.
- **[69](69-source-to-modedec.md)** §4.3 candidate 1: the three extra MB rows
  have an arithmetic source — MB-row counts are rounded up to a multiple of four
  (fw `0x58518`/`0x5851c`), and 45 → 48. §7.
- **[62](62-kext-field-map.md)** §1.4: VP+`0x08` produces **seven** flag bytes
  at `ctrl+0x23FD5..0x23FDB`, not six, and three of them reach IntraEst's DMem
  mode word. §8.
- **[62](62-kext-field-map.md)** §6.5: wire `0xFCD8` now has a destination —
  `ctrl+0xA7C`, the firmware's debug-verbosity bitfield. §6.
- **[58](58-pipe-start.md)** §5.1: the CAC 10-bit table is **not** identical to
  Default (it lacks the `+0x000` entry). The rest of §5.1 verifies exactly. §5.1.
- **[58](58-pipe-start.md)** §5.1 "What it is": the DPE block cannot be the
  mode-decision cost ladder; the cost registers are at `0x40D24A1C8/1CC` and
  `0x40D26A0AC..104`. §5.3.

---

## 11. Reproduce

```sh
# the IntraEst MCPU image, whole
objdump -D -b binary -m arm -M force-thumb <(python3 -c "import sys;\
 d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read();\
 sys.stdout.buffer.write(d[0xe3350+0x4000:0xe3350+0x4000+0x446])")
# the ModeDecision image, for the negative control in section 2
objdump -D -b binary -m arm -M force-thumb <(python3 -c "import sys;\
 d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read();\
 sys.stdout.buffer.write(d[0xe37a0+0x4000:0xe37a0+0x4000+0xcde])") | sed -n '/^ 1ac:/,/^ 340:/p'

# setPipe: QPY / nQuant, and the 23 ModeDecision cost words
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x55e2c -n 0xa0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57128 -n 0x520
# ProcessPipeReset's 0x01000001 defaults
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4f218 -n 0x70
# SetDefaultParameters seeding the per-client struct
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x468b0 -n 0x60
# ConfigureMCPUs: the stage enables, and IntraEst's DMem mode word
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x60298 -n 0x110
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x61294 -n 0x1b0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x6166c -n 0x80
# the debug gate
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x924d4 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x58540 -n 0x30
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cedc -n 0x10
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4de5c -n 0x20
# the intra-mode host bytes
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cf28 -n 0xb0   # VP+8 -> 7 bytes
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5e720 -n 0x110  # their use
# the MB-row rounding
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x584f8 -n 0x40

# DPE tables, regenerated and diffed against driver/ave_dpe_tables.h
python3 - <<'PY'
import sys, struct; sys.path.insert(0, 'tools'); import disas
d = open('data/blobs/macos-13.5/kc.macho', 'rb').read()
tbl = lambda fo, n: [struct.unpack_from('<4I', d, fo + 16*i) for i in range(n)]
for name, fo, n in (('CAT_Default', 0x22c9c0, 1), ('CAC_Default', 0x22c9d8, 0x7c),
                    ('CAC_8bit', 0x22d198, 0x7b), ('CAC_10bit', 0x22d948, 0x7b)):
    print(name, n, [hex(x) for x in tbl(fo, n)[0]])
PY

# hardware side, already captured
grep -E 'diag hif|diag MbInput|diag MCPU|dpe:' results/f21-1789893453.kmsg
grep -c 'fw\[' results/f21-1789893453.kmsg          # 1 -- the print gate is off
```

The whole-image MMIO map §1.3 rests on is reproducible the same way as
[69](69-source-to-modedec.md) §8: disassemble `__TEXT` (`0x0`..`0xec000`), track
`mov`/`movk`/`add`/`sub` constants and the `ldr x?,[x?,#1976]` after
`adrp x?, 0x21a000`, and record every access whose base resolves into
`0x1000000..0x2000000`. It reproduces [62](62-kext-field-map.md) §6.2 and
[69](69-source-to-modedec.md) §1.1 independently, which is the control that it
is not blind.
