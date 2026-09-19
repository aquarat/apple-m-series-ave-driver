# The MB-3083 stall (F12/F13): the four entropy/SEB write channels, and the interrupt the firmware already told us about

Prompted by F12 (`results/f12-1789839843.kmsg`) and F13
(`results/f13-1789840219.kmsg`): one 1280x720 AVC IDR (80x45 = 3600 MBs, QP 30,
Baseline, CAVLC, one slice), macOS 13.5 firmware, t6001. All commands accepted,
no asserts, no DART/SMMU/AXI faults. With the per-slot colocated MV buffers
wired at Start_AVC wire `0xF6B0` ([60](60-md-recon-stall.md) §6.1) the stall
moved from MB ~34 to MB ~3083. F13 filled `encoder_addr_entropy` as the full
4x4 matrix instead of column 0 and is **byte-identical to F12 in every counter**.

Continues [57](57-pipe-hang.md), [58](58-pipe-start.md), [59](59-row1-stall.md),
[60](60-md-recon-stall.md) and [53](53-first-frame.md) §21-§23. Conventions as
docs/60: firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`); MCPU VAs
are offsets inside the embedded Thumb image; an MCPU sees encoder register
`base+X` at `0x40000000+X`, the AP at `0x40C000000+X`, so register `0x1130380`
is AP `0x40D130380`.

Labels per [00](00-methodology.md): **[C]** read from an instruction (VA cited),
**[I]** inferred (chain stated), **[U]** unknown. Static analysis only; nothing
here was run on hardware.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| **The firmware already named the failure** | F12 and F13 print `Cveseb buffer write full!` **four times, 1.4 ms after the Process command and ~2 s before the hang report**. That string is the handler for **bit 12 of `0x40D110140`** (`0x38ba0..0x38be0`), inside `CAVEPipeISRManager`. It appears in **no other run** in `results/` | C |
| Q1: what are `0x40D1303C0 + 0x40k` | The **four pipe-side entropy / SEB write-DMA channels**. Control is written `0x80030001` unconditionally (`0x55d54..0x55d70`); address ← `EncCommParams.encoder_addr_entropy[k_ch][j]` at `ctrl+0xEC0`, size ← the companion u32 table at `ctrl+0x10C0`, both indexed by `j = ctrl[4740]` | C |
| …what programs them | `ctrl+0xEC0` is copied **per frame** from **PICMGMT `+0xA00`** by `PipePrepareParam` (`0x487a4..0x48bea`), `ctrl[3768]` = `num_encoder_addr_entropy` rows (= **4** on our single-core arm, `0x5d064/0x5d074`). **`ctrl+0x10C0` has no writer anywhere in the image** | C (copy) / C-negative (size, Trap 3 caveat) |
| …are they Transcode-only / normally idle | **No.** They are *pipe* write channels, enabled on every AVC pipe start regardless of Transcode. The four *Transcode* channels are a different block, `0x11208C0 + 0x40i`, programmed in `ProcessTranscodeStart` (`0x58fd0..0x590f0`). docs/60 §3.2's "entropy ×4 … CAVLC is idle so not the blocker" is superseded | C |
| …can "enabled with size 0" block the pipe | Yes, and it is the only channel class in that state. `0x40D11013C = 0xf007` enables exactly bits 0,1,2,12,13,14,15 — the four `… buffer write full` sources are armed, and bit 12 fired | C |
| Q2: what stops at ~3083 | The syntax-element (SEB) path. CAVLC ran 3067 MBs into an on-chip buffer whose DMA drain has a **null address and zero size**, so nothing ever left; when the internal buffer filled, bit 12 fired and the whole chain back-pressured within ~30 MBs. No ring sized 3040-3090 MBs exists in host memory; the depth is on-chip and **[U]** | C (evidence) / I (mechanism) |
| Q3: recon `+0x14/+0x18` | **Not counters.** `+0x14 = 0x700D1 \| (LSB-gate byte << 2)` is a firmware constant (`0x552ec..0x552fc`, chroma `0x58050`); `+0x18` is identical in the *after Start_AVC* and *timeout* dumps, so it is static per-channel config. The only recon word that moved is **`+0x20`** (`0 → 0x0024002a` luma, `0x0024002c` chroma) | C |
| …neighbour `+0x14/+0x18` | These **are** hardware pointers: zeroed by `ResetDMANeighborRegs` (`0x27c88`), zero in the after-Start_AVC dump, non-zero at the timeout. Info `0xc00/0x980` = 48 and 38 records of 64 B in an 80-record ring; Pixel `0x3c00/0x7500`. Exact semantics **[U]** | C / U |
| Q4: still-zero host fields | `0x130780`'s address (`ctrl[7896]` ← **PICMGMT `+0x9E0`**, only writer `0x486a8`) also reads **0**, although the driver publishes `src_nbr[3][0] = 0xff0f0000` there. Two independent PICMGMT fields at `+0x9E0` and `+0xA00` read back as zero — that is the one apparatus question to settle before anything else (§4.1) | C |
| Q5 | `0x40D110140` in full (bit 12 is the answer), the four Transcode read channels for comparison, the MCPU MB counters, and a **host-side dump of the built Process command at `0x1340..0x1450`** which costs no run at all | — |
| Best next step | §6.1: prove the Process command really carries PICMGMT `+0x9E0/+0xA00` (host-side, free), then find the source of the `ctrl+0x10C0` size before spending a boot | — |

---

## 1. The thing the firmware already said

### 1.1 `Cveseb buffer write full!` is interrupt bit 12 of `0x40D110140`

```
38ba0  stp x20,x19,[sp,#-32]!  …
38bac  adrp x19, 0x21a000 ; add x19,x19,#0x7b8     ; the MMIO base pointer slot
38bb4  mov  w20, #0x140 ; movk w20, #0x111, lsl 16 ; = 0x1110140
38bbc  ldr  x8,[x19]
38bc0  adr  x0, 0xbfcf1                            ; "Cveseb buffer write full!\n"
38bc8  ldr  wzr,[x8,x20]                           ; read for side effect
38bcc  bl   0x94920                                ; unconditional log
38bd0  ldr  x8,[x19]
38bd4  mov  w0, #1
38bd8  ldr  w9,[x8,x20] ; orr w9,w9,#0x1000 ; str w9,[x8,x20]   ; W1C bit 12
38be4  ret
```

**[C]** The sibling handlers in the same block give the whole bit map of
`0x1110140`:

| bit | handler VA | meaning | log string |
|---|---|---|---|
| 0 | `0x38b04` | AXI error; also dumps four status words from `0x1124a64` and `0x1120000`/`0x1130000` | `Client:%d %d %d %d AXI Error: 0x%x …` (`0xbfcbe`) |
| 1 | `0x3839c` | signals semaphore `[this+416]` | — |
| 2 | `0x3840c` | signals `[this+432]` — the **pipe/frame done** bit the driver waits on | — |
| 3 | `0x38444` | `[this+440]` | — |
| 5 | `0x3847c` | `[this+448]` | — |
| 6 | `0x384b4` | `[this+456]`, also sets a byte `[this+472] = 1` | — |
| 11 | `0x383d4` | `[this+424]` | — |
| **12** | **`0x38ba0`** | **SEB buffer write full** | `Cveseb buffer write full!` (`0xbfcf1`) |
| 13 | `0x38bf0` | AVC transcode buffer write full | `AVC XC buffer write full!` (`0xbfd0c`) |
| 14 | `0x38c40` | HEVC XC 0 | `0xbfd27` |
| 15 | `0x38c90` | HEVC XC 1 | `0xbfd45` |

**[C]** for every row (each handler W1C-writes its own single bit to
`0x1110140`). All of these live in `CAVEPipeISRManager.cpp` (string `0xbfc56`),
i.e. they are **pipe-level**, not transcode-level, interrupts.

**Cross-check that this decode is right:** F13 reads
`0x40D11013C = 0xf007` = bits 0,1,2 + 12,13,14,15 — exactly the seven bits with
an error/semaphore handler that this session can take, and none of the others.
**[C]** (`0x11013C` had only been labelled "enable" by inference before; this
makes the label solid.)

### 1.2 It only ever fired in the two runs that got past MB 34

```
$ for f in results/f*.kmsg; do printf '%-26s %s\n' $(basename $f) $(grep -c Cveseb $f); done
f10 0   f11 0   f12 4   f13 4   f3..f9 0   frame1/2 0
```

**[C]** F10/F11 stalled at MB ~34 with CAVLC idle; F12/F13 reached CAVLC
entries 3067 and got four of these. The message therefore tracks *CAVLC having
produced output*, not the session setup.

### 1.3 The timing puts it exactly at the stall

F13: `Process: sending 6464 bytes` at `54.604407`; the four
`Cveseb buffer write full!` at `54.605866 … 54.606003`; nothing else until the
heartbeat's `PIPE HANG: 1, 1` at `56.590489`. **[C]** So the pipe ran free for
**1.46 ms**, produced ~3080 MBs, hit this condition four times within 140 µs,
and never moved again. That is the stall, logged by the firmware, with a name.
**[I]**, strong.

### 1.4 What "Cveseb" is

`MCPU_Seb5` is one of the unit names in `CAVEPipeMcpuController.cpp`
(`0xbfee4`, alongside `MCPU_MbInput`, `…_CAVLC`, `MCPU_Md5`, `MCPU_Rcr5`,
`MCPU_Bef5`). **[C]** "CVE SEB" = the encoder's **syntax-element buffer**: the
staging buffer between the pipe's entropy/CAVLC stage and the transcode ("XC")
stage, which is why bit 12 (pipe side) and bit 13 (`AVC XC`, transcode side)
are separate. **[I]**

Four messages, four write channels in exactly that role (§2). **[I]**

---

## 2. Q1 — the four channels at `0x40D1303C0 + 0x40k`

### 2.1 They are unconditionally enabled

`setPipe` (`CAVCController::setPipe`, `0x52f38`; `x23 = 0x1130700`,
`w24 = 0x80030001`, `x8` = the MMIO base pointer from `[0x21a000+1976]`):

```
55d50  add x9, x23, x8
55d54  sub x10, x9, #0x340 ; str w24,[x10]   ; 0x11303C0 = 0x80030001
55d5c  sub x10, x9, #0x300 ; str w24,[x10]   ; 0x1130400 = 0x80030001
55d64  sub x10, x9, #0x2c0 ; str w24,[x10]   ; 0x1130440 = 0x80030001
55d6c  sub x10, x9, #0x280 ; str w24,[x10]   ; 0x1130480 = 0x80030001
55d74  sub x10, x9, #0x318 ; str wzr,[x10]   ; 0x11303E8 (+0x28) = 0
55d7c/84/88                                   ; the same +0x28 for the other three
```

**[C]** There is **no gate**: not the multi-core split, not a Transcode flag,
not a NEED_LSB-style bit. Every AVC pipe start arms all four. Compare the
colocated writer, whose control word is gated on its address being non-zero
(`0x554f0`), and the stats/split writers, which get `0x80030000`.

F13 confirms it on hardware: after Start_AVC the four read `0x80030000`; after
Process they read `0x80030001`. **[C]** (`chan [after Start_AVC] 40d1303c0…` at
t = 54.596585 vs `chan [timeout] 40d1303c0…` at t = 56.626618.)

### 2.2 Address and size, and where they come from

Single-core arm (`ctrl[4664] != 2`, taken because the driver sends
`sve_num = 1`), `x25 = ctrl`, `j = ctrl[4740]`:

| channel | address register | from | size register | from | VA |
|---|---|---|---|---|---|
| `0x11303C0` | `0x11303CC` | `ctrl + 0xEC0 + 8j` | `0x11303D0` | `ctrl + 0x10C0 + 4j` | `0x55938..0x55970` |
| `0x1130400` | `0x113040C` | `ctrl + 0xEE0 + 8j` | `0x1130410` | `ctrl + 0x10D0 + 4j` | `0x55b48..0x55b78` |
| `0x1130440` | `0x113044C` | `ctrl + 0xF00 + 8j` | `0x1130450` | `ctrl + 0x10E0 + 4j` | `0x55c20..0x55c50` |
| `0x1130480` | `0x113048C` | `ctrl + 0xF20 + 8j` | `0x1130490` | `ctrl + 0x10F0 + 4j` | `0x55cf8..0x55d28` |

**[C]** (`add x11, x25, #0xa70` then `ldr x10,[x11 + 8j, #1104]` →
`ctrl + 0xa70 + 0x450 + 8j = ctrl + 0xEC0 + 8j`; `ldr w10,[x11 + 4j, #1616]` →
`ctrl + 0xa70 + 0x650 + 4j = ctrl + 0x10C0 + 4j`. Channels 1-3 use `#1136/#1152/
#1168` and `#1632/#1648/#1664`, i.e. the same tables at +0x20 / +0x10 strides.)

So the two tables are, inside `EncCommParams` (= `ctrl + 0xa70`, memset to 0 at
session init by `memset(ctrl+0xa70, 0, 0x244b0)` at `0x467c8..0x467d8` **[C]**):

- `EncCommParams + 0x450` = `ctrl + 0xEC0` = **`encoder_addr_entropy[16][4]`**,
  u64, row stride `0x20`, column stride `8`. Named by the firmware's own log
  `encoder_addr_entropy[%d] %016llx` (`0xc694f`, used at `0x55a90`, `0x55b34`,
  `0x55c0c`, `0x55ce4`) and the assert
  `EncCommParams.encoder_addr_entropy[i][EncCommParams.transcode_buffer_id] != 0`
  (`0xc6eb3`). **[C]**
- `EncCommParams + 0x650` = `ctrl + 0x10C0` = the matching u32 **size/credit**
  table, row stride `0x10`, column stride `4`. **[C]** for the layout (it is
  read with the identical index arithmetic, one element per address element).

### 2.3 The address table is filled per frame from PICMGMT `+0xA00`

`CAVCController::PipePrepareParam` (`0x480b4`; `x19 = ctrl` `0x48144`,
`x26 = PICMGMT` `= [x1,#16]` `0x480e0`, `x23 = the pipe command` `0x4821c`):

```
4879c  ldr  w8,[x19,#3768]        ; ctrl+0xEB8 = num_encoder_addr_entropy
487a0  cbz  w8, 0x48bec           ; nothing copied when it is 0
487a4  ldrb w9,[x23,#52]
487a8  cbz  w9, 0x48898           ; -> "replicate column 0 into all four" variant
…
48800  ldr  x14,[x13,#2560] ; str x14,[x12,#3776]   ; PICMGMT+0xA00+32i -> ctrl+0xEC0+32i
48808  …+8 -> +8   48810  …+16 -> +16   48818  …+24 -> +24
```

**[C]** `x26` is PICMGMT beyond doubt: the same function reads `[x26,#3244]`
(`+0xCAC` = `frame_type`), `[x26,#3248]` (`+0xCB0` = `ctx_index`),
`[x26,#3088]` (`+0xC10` = `out_coded_hdr`) and `[x26,#2232]` (`+0x8B8` =
`recon_mv`), all of which match the driver's own `process_avc` layout.

The count: `InitEncodingParameters` writes `ctrl[3768] = 4` on the single-core
arm (`0x5d038 ldr w8,[x19,#4664]; cmp #2; b.ne 0x5d05c; 0x5d064 mov w10,#4;
0x5d074 str w10,[x19,#3768]`). **[C]** The other writer of that field
(`0x83600`) is the HEVC init. So the copy is four rows of four u64 =
PICMGMT `+0xA00 .. +0xA7F`.

The `[x23,#52] == 0` variant (`0x48898..0x48b7f`, vectorised tail at `0x48b40`)
copies **the same source word four times** into the four columns — `ldur x11,
[x10,#-96]` ×4 with `x10 = PICMGMT + 0xA60 + 32i` and
`x9 = ctrl + 0xF38 + 32i` — i.e. it replicates `encoder_addr_entropy[i][0]`.
**[C]** So on the `[x23,#52] == 0` path the channel gets
`encoder_addr_entropy[channel][0]` whatever `ctrl[4740]` is, and F13's extra
three columns were necessarily a no-op; on the `!= 0` path column
`ctrl[4740]` is the one that matters. Which path runs depends on a pipe-command
byte we do not control, so **keep all four columns filled** (F13's change is
right, it just cannot be the fix). The driver comment "the pipe's four write
channels come from the other columns" (`driver/ave_cmd.c:618-620`) is the part
that is wrong: they come from the four **rows**, all at the same column.

### 2.4 The size table has no writer

An exhaustive scan of the disassembled image for any store whose effective
address can be `ctrl + 0x10C0 .. 0x11BF` — `#4288`…`#4351` against any base,
`#1616`…`#1679` against a `ctrl+0xa70` base, `#0x10c0` as an immediate — finds
**only reads**: `0x5596c`, `0x55b74`, `0x55bf8`, `0x55cd0` (setPipe) and
`0x58fd8`, `0x7015c`, `0x73588`… (the Transcode reader and the HEVC twin).
**[C]** for the scan; positive control: the same scan finds the readers, and
finds the writers of the neighbouring fields `ctrl+0xEB8` (`0x5d074`) and
`ctrl+0x1BC0` (`0x58928`, the real `bfrcredit`). Access through a computed base
is not excluded (Trap 3). **[I]**

Two named tables that *are* written must not be confused with this one:

- `ctrl + 0x1BC0` = `bfrcredit[…]`, written by `ProcessTranscodeStart`
  (`0x58928`, `0x58dfc..0x58e8c`) from a per-client field `[client+11748]`.
- `ctrl + 0x1CC0` = `wrDmaBinAddr[transcode_buffer_id][i]`, written in the same
  loops from `encoder_addr_entropy`.

Both are Transcode-side and neither is what `setPipe` reads. **[C]**

### 2.5 Answer to Q1

The four channels are the **pipe-side entropy / syntax-element-buffer
writers**, one per `encoder_addr_entropy` row. They are enabled on every AVC
pipe start with no gate. Their address comes from the per-frame PICMGMT table
at `+0xA00`; their size comes from an `EncCommParams` field that nothing in the
firmware writes, so it must be host-supplied through a path we have not yet
located. They are **not** Transcode-only and **not** normally idle during the
pipe: the Transcode's own entropy channels are the separate `0x11208C0 + 0x40i`
block. "Enabled with address 0 and size 0" is precisely the state that produces
`Cveseb buffer write full`. **[C]** for the programming, **[I]** for the
causal link.

---

## 3. Q2 — what stops at MB ~3083

### 3.1 The whole chain stopped together

| stage | count at the timeout | lag behind MbInput |
|---|---|---|
| MbInput produced (source analysis) | 3097 | — |
| MbInput consumed (injected) | 3083 | 14 (the 15-MB lookahead gate, [59](59-row1-stall.md) §1.3) |
| ModeDecision handler entries | 3078 | 5 |
| ReconLuma grants | 3074 | 9 |
| **CAVLC entries** | **3067** | **16** |

**[C]** (F13 kmsg). Every stage is within 30 MBs of the next. That is the
signature of back-pressure arriving at the **tail** and propagating up in one
step — not of a stage failing on its own. The most downstream counted stage is
CAVLC, and the next thing after CAVLC is the SEB. **[I]**

Consistent with this, the colocated writer had written 3040 MB records
(194 560 / 64) of a 3600-record buffer, i.e. **it was not full**; the recon
writers had live addresses and sizes; the neighbour writers were advancing.
None of the buffers we own had run out. **[C]**

### 3.2 There is no 3040-3090-sized ring in host memory

Checked, with the sizes the firmware actually programs at 80x45:

| resource | capacity in MBs | VA of the formula |
|---|---|---|
| colocated MV | 230 400 / 64 = **3600** | `0x54ee0..0x54efc` |
| recon luma tile ring | 40 960 B = one 32-px tile row (2 MB rows) | `0x552c0..0x552e4` |
| neighbour Info | 5 120 / 64 = 80 (one row) | `0x5df8c` |
| neighbour Pixel | 81 920 / 1024 = 80 (one row) | `0x5df8c` |
| MbInput record ring | 17 records (`mbinput:0x12ee`) | — |
| coded data | 2 MiB, not per-MB | — |

**[C]** Nothing is sized near 3083. **[I]** The depth that ran out is therefore
**on-chip**, inside the SEB, and its size is **[U]**. At QP 30 a 720p intra MB
is on the order of 30-50 bytes of syntax elements, so 3067 MBs is ~100-150 KiB
— a plausible on-chip staging buffer, but that is arithmetic, not a read.
**[I] weak.**

### 3.3 Row 38 is not special

3083 = 38·80 + 43, and the last source event was y 38 x 48. Nothing in
`setPipe`, `PipePrepareParam` or the MCPU images keys on a row number other
than 0 and `pic_height_in_mbs - 1` (`ProcessPipeDone` `0x59690`). **[C]** The
row is where the frame happened to be when the on-chip buffer filled; on a
different QP or picture it would be a different MB. **[I]** — and that is a
**falsifiable prediction**: re-run at QP 45 (fewer bits/MB) and the stall MB
must move *later*; at QP 10 it must move *earlier*. That costs one boot and
discriminates §6.1 from any fixed-size-ring theory on its own.

---

## 4. Q4 — host-supplied fields still zero, and one apparatus problem

### 4.1 PICMGMT `+0x9E0` reads back as zero too — settle this first

`ctrl[7888]` and `ctrl[7896]` have exactly **one** writer in the image:

```
486a4  ldr x8,[x26,#2528]  ; PICMGMT + 0x9E0
486a8  str x8,[x19,#7896]
486ac  ldr x8,[x26,#2528]
486b0  str x8,[x19,#7888]
```

**[C]** (immediate scan for `#7888]` / `#7896]` returns these two stores and
nothing else.) `setPipe` then writes `ctrl[7896]` to `0x113078C` and
`ctrl[7888]` to `0x1120E4C` (`0x55934..0x55944`, `0x5590c..0x55930`). **[C]**

F13 at the timeout: `0x40D130780: 80030000 … +0xC = 00000000 +0x10 = 00001400`.
**[C]** So `PICMGMT + 0x9E0 = 0` as the firmware read it — while the driver
publishes `src_nbr[3][0] = 0xff0f0000` at `process_avc.src_nbr_set[3] = 0x9e0`
(`driver/ave_abi.h:1509`, `driver/ave_cmd.c:634-637`, log line
`SrcNeighbor 4 entries/group at 0xff000000 0xff050000 0xff0a0000 0xff0f0000`).

The entropy address at `+0xA00` is zero in the same way. Two adjacent
host-published PICMGMT fields, two different code paths in the firmware, both
reading zero. The fields that demonstrably *did* arrive (`+0x980`/`+0x9A0`, the
neighbour addresses at `0x40D130600/640 = ff000000/ff050000`) are **also** set
at Start_AVC by `InitEncodingParameters` (`0x5d874..0x5d89c`,
[59](59-row1-stall.md) §2.3), so they are **not** evidence that the per-frame
block landed.

**We therefore have no positive evidence that any per-frame PICMGMT field above
`+0x8C8` reaches the firmware, and two pieces of negative evidence.** Static
analysis says the copy at `0x487a4` is ungated once `ctrl[3768] = 4`, and the
driver's builder writes `base(0x9c8) + 0xa00` and `+ 0x9e0` inside a 6464-byte
command whose bounds check passes. The two cannot both be true. **[U]** — and
it is cheap to settle host-side (§5.1) before spending a boot on anything else.

### 4.2 The rest of the late-consumed fields

| field | wire / PICMGMT | firmware use | our state | Conf |
|---|---|---|---|---|
| `encoder_addr_entropy[i][j]` | PICMGMT `+0xA00`, wire `0x9c8+0xa00 = 0x13C8` | `ctrl+0xEC0` → the four SEB writers' address | **0** at the channel | C |
| entropy size/credit | `ctrl+0x10C0` — **no firmware writer, no located host field** | the four SEB writers' size | **0** | C / U |
| `num_encoder_addr_entropy` | firmware-set = 4 | gates the copy and `SetTranscode` | 4 | C |
| split-context address | PICMGMT `+0x9E0` (`src_nbr` group 3) | `0x113078C`, `0x1120E4C` | **0**; channel control `0x80030000` (disabled), so harmless in itself | C |
| MB stats / MB address | gate `ctrl+0x23FCF`, wire `0xFF76` | `0x1130700` `0x80030000`, size `0x40` | designed off-state, no assert; **not** a stall candidate | C ([60](60-md-recon-stall.md) §3.2) |
| stats DMA | gate `[ctrl+0x122b]+2429/2430` | gate off | same | C ([59](59-row1-stall.md) §4) |
| ME SFS results readers | `low_res_results`, PICMGMT `+0xC28` | inert in sync LRME | zero on purpose | C |
| rate-control row stats | MbInput 192-B record ring, firmware-owned | — | not host-supplied | C |
| `sSVEMap.iNum` | asserted `== 1` in `CollectDataFromCpus` (`0xc71d5`) | end-of-frame only | never reached | C |
| `curr_bitstream_addr_dst`, `wrDmaBinAddr` | Transcode | `SetTranscode` | Transcode never starts | C |

---

## 5. Q3 — decoding the channel words

The cleanest tool here is the **diff between the two dumps the driver already
takes**: `chan [after Start_AVC]` and `chan [timeout]`. A word that differs is
hardware or `setPipe` state; a word that is identical in both is a constant.

| channel | word | after Start_AVC | timeout | reading | Conf |
|---|---|---|---|---|---|
| recon luma `0x130240` | `+0x04` | `01000003` | `01000003` | static channel descriptor | C |
| | `+0x08` | `00080030` | `00080030` | static | C |
| | `+0x0C/+0x10` | `0 / 0` | `fdc20000 / 0000a000` | address / tile-row size, `setPipe` `0x553f8`, `0x552e4` | C |
| | **`+0x14`** | `000700d1` | `000700d1` | **`0x700D1 \| (ctrl LSB-gate byte << 2)`, a constant** (`0x552e8..0x552fc`) | C |
| | **`+0x18`** | `00380008` | `00380008` | **unchanged — static, not a counter** | C |
| | `+0x1C` | `0` | `fdc00000` | the second (LSB) plane address, `0x552b8` | C |
| | **`+0x20`** | `00000000` | **`0024002a`** | **the only word the hardware moved** | C |
| | `+0x24..+0x2C` | `0` | `1 1 1` | hardware | C |
| recon chroma `0x130300` | `+0x14` | `000700d1` | `000700d1` | same constant (`0x58050`) | C |
| | `+0x18` | `00a00008` | `00a00008` | static | C |
| | `+0x20` | `0` | `0024002c` | hardware | C |
| colocated `0x130380` | `+0x14/+0x18` | `0 / ffffffff` | `1 / 0` | hardware | C |
| entropy ×4 | `+0x04/+0x08` | `00b00010 / 03d40004` (+`0x10` per channel) | unchanged | static descriptors | C |
| | `+0x24` | `0` | `1` | hardware | C |
| | `+0x28` | `ffffffff` | `0` | `setPipe` zeroes it (`0x55d74`) | C |
| nbr Info `0x130600` | `+0x14/+0x18` | `0 / 0` | `00000c00 / 00000980` | **hardware ring pointers** | C |
| nbr Pixel `0x130640` | `+0x14/+0x18` | `0 / 0` | `00003c00 / 00007500` | same | C |

So, answering Q3 precisely:

- **`000700d1`, `00380008`, `00a00008` are not progress.** The first is an
  explicit constant `setPipe` writes; the other two are byte-identical before
  and after the frame.
- **`0x0024002a` / `0x0024002c` are the recon writers' only live state.** They
  differ between luma and chroma by 2 in the low half and are equal in the high
  half (`0x24` = 36). A position of `(y=36, x=42)` would be MB 2922, 150 behind
  ReconLuma's 3074, so a straight MB coordinate does not fit. **[U]**; read
  them again on a run that stalls at a *different* MB (§3.3) and the mapping
  falls out of two data points.
- **The neighbour writers' `+0x14/+0x18` are the real progress words.**
  `ResetDMANeighborRegs` (`0x27c88`) zeroes them before every pipe start
  (**[C]**, and the after-Start_AVC dump confirms it), so the timeout values
  are hardware. Info: ring `0x1400` = 80 records of 64 B; `+0x14 = 0xc00` = 48
  records, `+0x18 = 0x980` = 38 records, 10 apart. Pixel: ring `0x14000` = 80
  records of 1024 B; `+0x14 = 0x3c00` = 15 records, `+0x18 = 0x7500` = 29.25
  records — **not** an integral record count, so the Pixel writer works in
  sub-MB chunks and the two channels' pointers are not in the same units.
  **[C]** for the arithmetic, **[U]** for the meaning. The coincidence that
  Info's two pointers are 48 and 38 records while the last source event was
  `y 38 x 48` is noted and **not** relied on; the units do not match.

None of these words identifies a stage that is further behind than the CAVLC
counter already does.

---

## 6. Q5 — what to read, and in what order

### 6.1 Costs no run at all (do this first)

1. **Dump the built Process command before sending it.** In
   `ave_session_frame()`, after `ave_cmd_build_process_avc` succeeds, print
   `cmd[0x1340..0x1450]` (that is PICMGMT `+0x978 .. +0xA88`: the four
   `src_nbr` groups and the whole entropy matrix). This settles §4.1 with no
   hardware: either the IOVAs are there, in which case the firmware's reads of
   PICMGMT `+0x9E0/+0xA00` are the mystery and `process_avc.picmgmt = 0x9c8`
   needs re-deriving, or they are not, in which case the bug is in the builder.
2. Print `abi->cmd[AVE_OP_PROCESS_AVC].size` next to the "sending N bytes" line
   so `w->size` is visible.

### 6.2 Reads to add to the next run (bank 0, read-only)

| address | why | expected if §7.1 is right |
|---|---|---|
| **`0x40D110140`** (full word, at the timeout **and** before Process) | the answer is in bit 12; also shows whether bit 2 (frame done) ever set | bit 12 latched or already W1C-cleared by the handler; bit 2 clear |
| `0x40D11013C` | enable, confirm `0xf007` | `0xf007` |
| `0x40D1208C0`, `+0x40`, `+0x80`, `+0xC0` (16 words each) | the **Transcode** entropy read channels, for comparison with the pipe writers | `0x80030000`, address 0 (Transcode never ran) |
| `0x40D1124A64` … `+0x10` | the four words the AXI-error handler dumps (`0x38b48`) | unknown; free context |
| `0x40D1120E40` (16 words) | the sibling of `0x1130780` fed by `ctrl[7888]` ← PICMGMT `+0x9E0` | address 0 if §4.1 holds |
| `0x40D468A1C`, `0x40D488280`, `0x40D4C87A4` | MCPU ModeDecision / ReconLuma / CAVLC MB counters ([60](60-md-recon-stall.md) §5.1) | ≈ 3078 / 3074 / 3067 — confirms the host-visible counters |
| `0x40D2C8000 + 0/4/8/0xc/0x10/0x14` | CAVLC host-if: is CAVLC the stage now parked in a `+0x14` wait? | `+0x14` bit 31 set would name CAVLC as the blocked stage |
| `0x40D1303C0 + 0x20` and `+0x24` on each of the four | the only moving words in those channels | |

Keep the whole §5.4 snapshot set from docs/60 (both snapshots, before Process
and at the timeout) — the before/after diff is what made §5 readable.

### 6.3 Firmware-side diagnostic

`setPipe` logs `encoder_addr_entropy[%d] %016llx` (`0xc694f`) with the value it
is about to program, via the level-gated logger `0x924d4`
(`0x55a90`, `0x55b34`, `0x55c0c`, `0x55ce4`). **[C]** Raising the firmware log
level for that subsystem (docs/33) prints `ctrl+0xEC0` directly and separates
"the host's table never arrived" from "setPipe read the wrong column". The
driver already captures `fw[0]|` lines, so this needs no new plumbing.

---

## 7. Ranked causes, each with a driver change and the observation that confirms it

Proposals for the operator (AGENTS.md); nothing here was run. **Fixed** means:
MbInput consumed reaches 3600, `0x40D110140` bit 2 set, `StartCount 1-1-1-1`,
completion id `0x0E06`, `frame.h264` non-empty.

### 7.1 The entropy / SEB write channels are enabled with a null address and zero size (high)

**Why.** §1: the firmware logs `Cveseb buffer write full!` four times, at the
exact moment of the stall, only in the runs that reached CAVLC, on a
pipe-level interrupt bit that this session has enabled. §2: the four channels
in that role are armed unconditionally and read address 0, size 0. §3: every
buffer we own still had room, and the whole chain stopped within 30 MBs of the
tail.

**Change.** Two parts, and the second is the one still missing a wire offset:

1. *Address.* Make PICMGMT `+0xA00` actually arrive. Wire offset
   `process_avc.picmgmt (0x9c8) + 0xA00 = 0x13C8`, u64, 64-byte aligned
   (assert `CAVCController_H13C.cpp:8021`). Keep the full 4x4 fill (§2.3):
   the four channels index the four **rows**, and which column is read is a
   firmware choice we do not control. Size per buffer: the driver's existing
   `AVE_CalcBufSizeOfEntropyCoding` figure (960 KiB) is what macOS allocates;
   keep it. Verify with §6.1 **before** the run.
2. *Size.* The channel's `+0x10` comes from `EncCommParams + 0x650`
   (`ctrl + 0x10C0 + 0x10·i + 4·j`), which **nothing in the firmware writes**
   (§2.4). Locate its host field before the run: the candidates are a
   Start_AVC/Config table the kext writes that we have not decoded, or a
   PICMGMT field adjacent to `+0xA00`. Grep the kext for stores of a *size*
   next to `AVE_CHM_SetDataInfo_FwBuf`'s `0xfffffe0008eb0cb8` loop and for
   callers of `AVE_CalcBufSizeOfEntropyCoding`
   (`0xfffffe0008ea5bd0`). Until it is found, the address alone may not be
   enough: a channel with a real address and size 0 is still a zero-length
   ring.

**Apparatus check.** At the timeout `0x40D1303CC/0x113040C/0x113044C/0x113048C`
must hold the four entropy IOVAs (low 32 bits) and `0x11303D0 + 0x40k` a
non-zero size. If they are still 0, the table did not land and §7.2 is the
live problem, not this.

**Confirm.** No `Cveseb buffer write full!` in the log, the entropy buffers'
first bytes changed from their fill pattern, consumed reaches 3600, then
*Fixed*. **Refute.** The channels are programmed, the buffers are written, and
the stall stays at ~3083 with bit 12 still firing — then the SEB drain is not
these channels and §7.3 takes over.

### 7.2 The per-frame PICMGMT block above ~`+0x9C0` is not reaching the firmware (high, and it gates 7.1)

**Why.** §4.1: `PICMGMT + 0x9E0` and `PICMGMT + 0xA00` both read back as zero
through two unrelated firmware paths, while the driver publishes both. Every
PICMGMT field we can *prove* arrived is also set at Start_AVC.

**Change.** None on the wire until §6.1 says which side is wrong. If the bytes
are in the command buffer, re-derive `process_avc.picmgmt` and `picmgmt_size`
from the kext (`0xfffffe0008eac9bc`, `0xfffffe0008eac9c4`) and from
`ProcessAVC`'s **two** PICMGMT bases — `x21 + 0x9c8` (`0x145c8`) and
`x21 + 0x55b0` (`0x145ac`), selected by a branch upstream of `0x145a0`
(**[C]**, the selector itself is **[U]**). If the bytes are *not* in the
buffer, the bug is in `ave_cmd_build_process_avc` and costs no run to fix.

**Confirm.** `0x40D113078C` becomes `0xff0f0000` (the group-3 SrcNeighbor IOVA)
in the same run that the entropy addresses appear. That single register is the
cheapest witness that the per-frame block landed.

### 7.3 The SEB drains somewhere else and the four channels are a red herring (medium-low)

**Why.** The mapping "`Cveseb` = the `encoder_addr_entropy` writers" is **[I]**,
from the unit name `MCPU_Seb5`, the pipe-vs-XC split of bits 12/13, and the
count of four. No code read says which channel the SEB writes to.

**Change.** None. Decide it with reads: if §7.1's change makes the four
channels live and bit 12 still fires, the SEB drain is one of
`0x40D130700` (stats, deliberately disabled, size `0x40`) or `0x40D130780`
(split context, address 0 per §4.1) — both of which are also degenerate on our
session, and both of which §7.2 would fix for free.

**Confirm.** Bit 12 stops firing when a *different* channel is given an
address. **Refute.** Bit 12 keeps firing with every pipe write channel
programmed.

### 7.4 The stall MB is a fixed-size on-chip resource unrelated to the SEB (low)

**Why.** 3083 is oddly reproducible (F12 and F13 byte-identical).

**Change.** None; it is a free rider on any other run. Encode the *same* frame
at QP 45 and at QP 10 (`session_qp`) with everything else unchanged. If the
stall MB moves with the bitrate, the resource is bit-count-limited and §7.1 is
right. If it stays at exactly 3083, the resource is MB-count-limited and this
document's mechanism is wrong.

**Confirm/refute** as stated — this is the single cheapest discriminator in the
list and it does not depend on finding the size field.

### 7.5 Colocated / recon / neighbour buffers (very low)

All are programmed with live addresses and sizes at the timeout, all had room
left, and none of them has an error bit set. `0x40D120BC0` (the colocated
reader) is `0x80030000` with size `0x38400` — disabled, which is correct for an
IDR with no references. No change. **[C]**

---

## 8. Corrections to other documents (not edited here)

1. **docs/60 §3.2**, row "entropy ×4": "programmed … CAVLC (the likely writer)
   is idle, so it is not the blocker". The channels are *enabled* but have a
   null address and zero size, and F12/F13 show CAVLC running 3067 MBs. They
   are the leading cause, not excluded. **[C]**
2. **docs/60 note 4** ("`setPipe` also programs four pipe write-DMA channels
   from `encoder_addr_entropy[k][transcode_buffer_id]`"): correct, but the
   index is `encoder_addr_entropy[channel][ctrl[4740]]` with
   **row = channel, column = `ctrl[4740]`** (`ctrl+0xEC0 + 0x20·channel +
   8·j`), and the size comes from a *second*, unwritten table at `ctrl+0x10C0`.
   **[C]**
3. **docs/59 note from review** ("its consumer is the Transcode stage, which
   has not started in any run, so it cannot explain a Pipe stall"): wrong. The
   pipe has its own four entropy write channels. **[C]**
4. **driver `ave_cmd.c:618-620`** and `ave_abi.h` `entropy_cols_max = 4`
   ("the pipe's four write channels come from the other columns"): the pipe's
   four channels come from the four **rows**, all at column `ctrl[4740]`, and
   the firmware replicates column 0 across all four columns when the pipe
   command's byte 52 is zero (§2.3). Filling column 0 of rows 0..3 is
   sufficient; F13's 4x4 fill was correctly a no-op.
5. **docs/57/58's `0x40D11013C`**: now confirmed as the interrupt-enable
   register for `0x40D110140`, and `0xf007` is exactly the seven bits with
   handlers this session can take (§1.1). **[C]**

---

## 9. Reproduce

```sh
cd ~/Projects/apple-ave-driver
M="AVE_MACOS=13.5 python3 tools/disas.py --fw"

# The interrupt bit map of 0x1110140 (bit 12 = "Cveseb buffer write full")
eval $M --addr 0x38380 -n 0x190     # bits 1,11,2,3,5,6 handlers
eval $M --addr 0x38b04 -n 0x1e0     # bit 0 (AXI error), 12, 13, 14, 15
python3 - <<'EOF'
d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read(); import re
for m in re.finditer(rb'[ -~]*buffer write full[^\x00]*', d): print(hex(m.start()-0x4000), m.group())
for s in (b'MCPU_Seb5', b'CAVEPipeISRManager', b'encoder_addr_entropy bfrcredit'):
    i=d.find(s); print(hex(i-0x4000), s)
EOF
grep -c Cveseb results/f*.kmsg          # only f12 and f13

# setPipe: the four entropy channels
eval $M --addr 0x558b4 -n 0x4c0         # single-core and multi-core arms, addr+size
eval $M --addr 0x55d40 -n 0x60          # the four control words = 0x80030001
# PipePrepareParam: PICMGMT+0xA00 -> ctrl+0xEC0, and PICMGMT+0x9E0 -> ctrl[7888/7896]
eval $M --addr 0x480b4 -n 0x90          # prologue: x19=ctrl, x26=PICMGMT, x23=cmd
eval $M --addr 0x48600 -n 0x300         # the field copies and the entropy loop
eval $M --addr 0x48898 -n 0x60          # the "replicate column 0" variant
eval $M --addr 0x48b40 -n 0x60          # its vectorised tail
# num_encoder_addr_entropy = 4 on the single-core arm
eval $M --addr 0x5d020 -n 0x80
# the size table ctrl+0x10C0 has readers only
S=/tmp/fw; mkdir -p $S
python3 -c "open('$S/t.bin','wb').write(open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()[0x4000:0x4000+0xd8000])"
objdump -D -b binary -m aarch64 $S/t.bin > $S/fw.S
grep -nE '#(42[89][0-9]|43[0-4][0-9])\]|#1616|#0x10c0' $S/fw.S
# the Transcode side, for contrast
eval $M --addr 0x58f80 -n 0x1d0         # 0x11208D0 + 0x40i read channels
eval $M --addr 0x588d0 -n 0xd0          # bfrcredit at ctrl+0x1BC0, wrDmaBinAddr at +0x1CC0
# recon channel words
eval $M --addr 0x552a8 -n 0x60          # +0x10 size, +0x14 = 0x700D1|gate<<2
eval $M --addr 0x58010 -n 0x50          # the chroma twin
# kext
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb0cb8 -n 0x60   # PICMGMT+0xA00 fill
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ea5b88 -n 0x90   # entropy size helpers
```
