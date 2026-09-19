# The MB-3083 stall (F12/F13): the four entropy/SEB write channels, and the interrupt the firmware already told us about

Prompted by F12 (`results/f12-1789839843.kmsg`) and F13
(`results/f13-1789840219.kmsg`): one 1280x720 AVC IDR (80x45 = 3600 MBs, QP 30,
Baseline, CAVLC, one slice), macOS 13.5 firmware, t6001. All commands accepted,
no asserts, no DART/SMMU/AXI faults. With the per-slot colocated MV buffers
wired at Start_AVC wire `0xF6B0` ([60](60-md-recon-stall.md) §6.1) the stall
moved from MB ~34 to MB ~3083. F13 filled `encoder_addr_entropy` as the full
4x4 matrix instead of column 0 and is **byte-identical to F12 in every counter**.

**Resolved in §10:** the firmware overwrites the per-frame entropy table from a
**Start_AVC** record before it ever reads it, so no change to the Process
command could have mattered. The table belongs at Start_AVC wire `0xF830`.

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
| **Why it reads zero although we send it (§10)** | **`setRefPointers` overwrites PICMGMT `+0x980..+0xBFF` on every frame** (`0x2c98c..0x2ca4c`) from a per-DPB-slot record that `ProvideReferenceFrames` filled at **Start_AVC** from **wire `0xF830`** (`0x2ba98..`, `x8 = VP + 0xF770`). The driver has never written `0xF830`: `ave_abi.h` mislabels it as the fourth SrcNeighbor group (which really lives at wire **`0xFED0`**) and leaves it `AVE_OFF_NONE`. So the entropy table must go in **Start_AVC**, not Process | C (copy) / I (wire, 3-way cross-check) |
| …are they Transcode-only / normally idle | **No.** They are *pipe* write channels, enabled on every AVC pipe start regardless of Transcode. The four *Transcode* channels are a different block, `0x11208C0 + 0x40i`, programmed in `ProcessTranscodeStart` (`0x58fd0..0x590f0`). docs/60 §3.2's "entropy ×4 … CAVLC is idle so not the blocker" is superseded | C |
| …can "enabled with size 0" block the pipe | Yes, and it is the only channel class in that state. `0x40D11013C = 0xf007` enables exactly bits 0,1,2,12,13,14,15 — the four `… buffer write full` sources are armed, and bit 12 fired | C |
| Q2: what stops at ~3083 | The syntax-element (SEB) path. CAVLC ran 3067 MBs into an on-chip buffer whose DMA drain has a **null address and zero size**, so nothing ever left; when the internal buffer filled, bit 12 fired and the whole chain back-pressured within ~30 MBs. No ring sized 3040-3090 MBs exists in host memory; the depth is on-chip and **[U]** | C (evidence) / I (mechanism) |
| Q3: recon `+0x14/+0x18` | **Not counters.** `+0x14 = 0x700D1 \| (LSB-gate byte << 2)` is a firmware constant (`0x552ec..0x552fc`, chroma `0x58050`); `+0x18` is identical in the *after Start_AVC* and *timeout* dumps, so it is static per-channel config. The only recon word that moved is **`+0x20`** (`0 → 0x0024002a` luma, `0x0024002c` chroma) | C |
| …neighbour `+0x14/+0x18` | These **are** hardware pointers: zeroed by `ResetDMANeighborRegs` (`0x27c88`), zero in the after-Start_AVC dump, non-zero at the timeout. Info `0xc00/0x980` = 48 and 38 records of 64 B in an 80-record ring; Pixel `0x3c00/0x7500`. Exact semantics **[U]** | C / U |
| Q4: still-zero host fields | `0x130780`'s address (`ctrl[7896]`) reads **0** for the same reason, one table earlier: its Start_AVC source is wire **`0xFED0`** (`0x5d8a0`, `x8+1792`), which the driver leaves `AVE_OFF_NONE`. Two anomalies, one mechanism (§4.1, §10.3) | C |
| Q5 | `0x40D110140` in full (bit 12 is the answer), the four Transcode read channels for comparison, the MCPU MB counters, and the four entropy channels' `+0xC/+0x10` | — |
| **Best next step** | **§7.1: send `encoder_addr_entropy` in Start_AVC at wire `0xF830 + 0x20i + 8j` (u64, 64-byte aligned, rows 0-3, all four columns), and `src_nbr_set[3]` at wire `0xFED0`** | — |

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

**But the source is not the table we sent** — `setRefPointers` has already
overwritten PICMGMT `+0xA00` from a Start_AVC-time record. See **§10**.

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

## 4. Q4 — host-supplied fields still zero

### 4.1 RESOLVED: the firmware throws our per-frame values away

*(Was: "PICMGMT `+0x9E0` reads back as zero too — settle this first". The
coordinator built the same Process command in userspace and dumped wire
`0x1340..0x1450`: all sixteen entropy entries are present at
`0x13C8 + 0x20i + 8j`, the SrcNeighbor group-3 field is present at `+0x9E0`,
and the command is 6464 bytes against a declared size of 6464. **The host side
is not at fault.** The firmware-side reason is §10.)*

`ctrl[7888]` and `ctrl[7896]` have exactly **two** writers in the image:

```
5d8a0  ldr x8,[x23,#1808] ; str x8,[x19,#7896]   ; InitEncodingParameters, Start_AVC
5d8a8  ldr x8,[x23,#1808] ; str x8,[x19,#7888]
486a4  ldr x8,[x26,#2528] ; str x8,[x19,#7896]   ; PipePrepareParam, PICMGMT + 0x9E0
486ac  ldr x8,[x26,#2528] ; str x8,[x19,#7888]
```

**[C]** Both sources are the **same host field** seen from two sides
(§10.3): `x23 + 1808` is Start_AVC wire **`0xFED0`**, and PICMGMT `+0x9E0` is
what `setRefPointers` copies *from* that same Start_AVC field every frame. The
driver leaves `start_avc.src_nbr_set[3] = AVE_OFF_NONE`
(`driver/ave_abi.h:1391`), so the field is zero at Start_AVC, `setRefPointers`
writes that zero over PICMGMT `+0x9E0`, and `0x40D13078C` reads 0 — exactly as
observed, with the bytes we sent in Process discarded in between. The same
mechanism, one table further on, is what zeroes the entropy addresses.

### 4.2 The rest of the late-consumed fields

| field | wire / PICMGMT | firmware use | our state | Conf |
|---|---|---|---|---|
| `encoder_addr_entropy[i][j]` | **Start_AVC wire `0xF830 + 0x20i + 8j`** (§10.3); the per-frame PICMGMT `+0xA00` copy is overwritten from it | `ctrl+0xEC0` → the four SEB writers' address | **0** at the channel — we never write `0xF830` | C / I (wire) |
| entropy size/credit | `ctrl+0x10C0` — **no firmware writer, no located host field** | the four SEB writers' size | **0** | C / U |
| `num_encoder_addr_entropy` | firmware-set = 4 | gates the copy and `SetTranscode` | 4 | C |
| SrcNeighbor FwData / split-context address | **Start_AVC wire `0xFED0`** (`0x5d8a0`); PICMGMT `+0x9E0` is overwritten from it | `0x113078C`, `0x1120E4C` | **0**; channel control `0x80030000` (disabled), so harmless in itself | C / I (wire) |
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

### 6.1 Costs no run at all — done

The coordinator dumped the built Process command at wire `0x1340..0x1450` in
userspace: all sixteen entropy entries present at `0x13C8 + 0x20i + 8j`, the
SrcNeighbor group-3 field present at `+0x9E0`, 6464 bytes against a declared
6464. The host side is clean; §10 is the firmware-side answer. Keep the dump as
a regression check, and add the same one for the **Start_AVC** command at wire
`0xF7D0..0xFA30` once §7.1 is implemented.

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

### 7.1 `encoder_addr_entropy` is a **Start_AVC** table, and we send it only in Process (high — this is the bug)

**Why.** §10. `setRefPointers` overwrites PICMGMT `+0xA00..+0xBFF` on every
frame from a per-DPB-slot record that `ProvideReferenceFrames` filled at
Start_AVC from **wire `0xF830`**, which the driver has never written. Whatever
the Process command carries at PICMGMT `+0xA00` is destroyed before
`PipePrepareParam` reads it, so `ctrl+0xEC0` is zero, so the four SEB write
channels get address 0, so the SEB never drains and raises
`Cveseb buffer write full` (§1) after ~3070 MBs.

This also explains, with no extra assumption, why F13's 4x4 fill was
byte-identical to F12 (both were overwritten) and why PICMGMT `+0x9E0` reads
zero (§4.1, the same mechanism one table earlier).

**Change — the one that matters.** In the **Start_AVC** command, write the
entropy table at:

| | |
|---|---|
| wire offset | **`0xF830 + 0x20·i + 8·j`**, `i = 0..15` rows, `j = 0..3` columns |
| width | u64 little-endian IOVA |
| alignment | 64 bytes (`& 63 == 0`, assert `CAVCController_H13C.cpp:8021`) |
| rows that matter | `i = 0..3` — `num_encoder_addr_entropy` is 4 on the single-core arm (fw `0x5d064/0x5d074`) and `setPipe` reads exactly rows 0-3 |
| columns | fill all four; `setPipe` reads column `ctrl[4740]` and the host does not choose it |
| buffer size | unchanged: the driver's existing `AVE_CalcBufSizeOfEntropyCoding` figure, 960 KiB each, 16 buffers already allocated |
| table extent | `0xF830 .. 0xFA2F` (512 B). Leave rows 4-15 zero, or point them at the same buffers — nothing reads them for the Pipe |

In `driver/ave_abi.h` this is a new `start_avc.entropy_set = 0xf830`,
`entropy_stride_i = 0x20`, `entropy_stride_j = 0x08`, `entropy_max = 4`,
`entropy_cols_max = 4`, and `ave_cmd_build_start_avc` grows the same loop the
Process builder already has. **Keep** the Process-side write: it is harmless,
and if a future firmware stops overwriting PICMGMT it becomes the right place
again.

**Second change, same family (free, do it in the same patch).** Set
`start_avc.src_nbr_set[3] = 0xfed0` (currently `AVE_OFF_NONE`,
`driver/ave_abi.h:1391`) and publish the fourth SrcNeighbor group there. Two
firmware sites read that exact field (`0x5d8a0` at Start_AVC and `0x2ba7c` per
frame), and it is what feeds `0x40D1120E4C` / `0x40D113078C`. It is not known
to be needed for the Pipe, but it is a one-line witness that the whole
mechanism is understood (§7.1 *Confirm*).

**Apparatus check.** At the timeout
`0x40D1303CC / 0x113040C / 0x113044C / 0x113048C` must hold the low 32 bits of
the four entropy IOVAs. If they are still 0, the wire offset is wrong — re-read
`0x2ba14..0x2bc90` and check that `x8 = VP + 0xF770` still holds (§10.2).

**Confirm.** No `Cveseb buffer write full!` in the log; `0x40D113078C =
0xff0f0000` (the group-3 SrcNeighbor IOVA, from the second change); the entropy
buffers' `0x5A` fill overwritten; MbInput consumed → 3600; then *Fixed*.
**Refute.** The four addresses appear, the buffers are written, and the stall
stays at ~3083 with bit 12 still firing — then the size question (§7.2) is real
and the SEB needs `ctrl+0x10C0` as well.

### 7.2 The channel size `ctrl+0x10C0` may still be zero (medium, but only after 7.1)

**Why.** §2.4: `EncCommParams + 0x650` has **no writer anywhere in the image**,
host-fed or computed, and no companion table sits next to the addresses in
either the Start_AVC record (§10.2: the per-slot copy is u64 addresses only,
`x8+96 … x8+600`, with nothing u32-shaped after it) or in `PipePrepareParam`
(§10.4: its only stores into `ctrl` in the whole `3700..4600` range are
`3776/3784/3792/3800` and `4544/4552/4560/4568`).

Two readings, and 7.1's run separates them:

- **macOS leaves it zero too** and `+0x10` is not a hard length for this
  channel class — the SEB's record size is fixed in hardware and the field is a
  credit that only matters in a mode we are not in. Then 7.1 alone fixes the
  stall. This is the reading the evidence currently favours: a firmware that
  *never* writes a field cannot depend on it for its own normal path.
- **There is a writer behind a computed base** (Trap 3) that only runs on a
  path we have not entered — most plausibly one keyed on
  `ctrl[4740]`/`transcode_buffer_id`, since every reader indexes by it.

**Change.** None yet; there is no wire offset to send because no host field
feeds it. If 7.1 is refuted, re-attack it from the kext: find the callers of
`AVE_CalcBufSizeOfEntropyCoding` (`0xfffffe0008ea5bd0`) and
`AVE_CalcBufNumOfEntropyCoding` (`0xfffffe0008ea5ba8`) and see whether either
size reaches a command field, and look for a u32 table at Start_AVC wire
`0xFA30` (immediately after the address table) with the same 4-entry rows.

**Confirm.** `0x11303D0 + 0x40k` non-zero at the timeout without us sending
anything — which would mean the firmware computes it once the address is live.

### 7.3 The SEB drains somewhere else and the four channels are a red herring (low)

**Why.** "`Cveseb` = the `encoder_addr_entropy` writers" is **[I]**, from the
unit name `MCPU_Seb5`, the pipe-vs-XC split of bits 12/13, and the count of
four. No code read says which channel the SEB writes to. §10 weakens this
further: the four channels are now a *known* host-side omission with a known
fix, which is the ordinary shape of every bug this bring-up has had.

**Change.** None. If 7.1 lands and bit 12 still fires, the remaining degenerate
pipe writers are `0x40D130700` (stats, deliberately disabled) and
`0x40D130780` (fixed for free by 7.1's second change).

### 7.4 The stall MB is a fixed-count on-chip resource unrelated to the SEB (low)

**Why.** 3083 is oddly reproducible (F12 and F13 byte-identical).

**Change.** None; it rides along on any run. Encode the same frame at QP 45 and
at QP 10 (`session_qp`) with nothing else changed. Stall MB moves with the
bitrate ⇒ the SEB mechanism is right. Stays at exactly 3083 ⇒ this document's
mechanism is wrong. Cheapest discriminator in the list, and it does not depend
on 7.1 landing.

### 7.5 Colocated / recon / neighbour buffers (very low)

All are programmed with live addresses and sizes at the timeout, all had room
left, none has an error bit set. No change. **[C]**


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
6. **docs/59 §3 and `driver/ave_abi.h:1391/1509`**, the fourth SrcNeighbor
   group: **wire `0xF830` is not `SrcNeighborFwData`, it is
   `encoder_addr_entropy[16][4]`** (§10.3). The real fourth group is at wire
   **`0xFED0`** (`0x5d8a0`, `0x2ba78`). Nothing broke so far only because
   `start_avc.src_nbr_set[3]` is `AVE_OFF_NONE`, so the driver never wrote
   `0xF830` at all. The per-frame `process_avc.src_nbr_set[3] = 0x9e0` is a
   correct PICMGMT offset but is overwritten by `setRefPointers` every frame.
7. **docs/60 §3.3 item 3** ("`setRefPointers` did run and matched slot 0 …
   `PICMGMT+0x8B8` was overwritten with 0"): correct, and much broader than
   recorded — the same function overwrites **PICMGMT `+0x980` through
   `+0xBFF`** from the Start_AVC DPB record (§10.1). Any per-frame IOVA the
   driver publishes in that range is discarded. **[C]**
8. **docs/54's "`encoder_addr_entropy` … the last unconditional assert left on
   the per-frame path"**: the table is a *session* input, not a per-frame one.
   The `:8020/:8021` asserts in `SetTranscode` test the copy that came from
   Start_AVC by way of `setRefPointers`. **[I]**

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
# WHY ctrl+0xEC0 IS ZERO (section 10)
eval $M --addr 0x2c930 -n 0x160         # setRefPointers: PICMGMT +0x980..+0xBFF overwritten
eval $M --addr 0x2ca50 -n 0x40          # ... continuing to +0xBF8
eval $M --addr 0x2b614 -n 0x14          # ProvideReferenceFrames: w8 = 0xf770
eval $M --addr 0x2b838 -n 0x8           # x8 = VP + 0xF770
eval $M --addr 0x2ba00 -n 0x120         # the per-slot record fill; x8+96 = entropy table
eval $M --addr 0x2bb20 -n 0x1a0         # ... to x8+600 (64 u64)
eval $M --addr 0x5d874 -n 0x50          # InitEncodingParameters: [x23,#16]=0xF7D0, [x23,#1808]=0xFED0
eval $M --addr 0x48140 -n 0x160         # PipePrepareParam gates: ctrl[5176] fresh-frame path
eval $M --addr 0xf020 -n 0x70           # ProcessAvcEncode memmoves the whole 6464-byte command
eval $M --addr 0x14558 -n 0xd0          # SendCommandToQueue: PICMGMT = cmd + 0x9c8 (AVC arm)
# kext
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb0cb8 -n 0x60   # PICMGMT+0xA00 fill
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ea5b88 -n 0x90   # entropy size helpers
```


---

## 10. Why `ctrl+0xEC0` is zero although the host sends the table

This section answers the one question left after the coordinator settled the
host side. The short version: **`encoder_addr_entropy` is a Start_AVC table,
not a per-frame one.** The firmware overwrites the per-frame copy from a
session-time record before it ever reads it — the same trick docs/60 found for
the colocated MV pointer, applied to a much larger block.

### 10.1 `setRefPointers` overwrites PICMGMT `+0x980 .. +0xBFF` every frame

`CAVECommonDPB::setRefPointers(AVE_PICMGMT_PARAMS *x1, ReferenceFrameInfoData *)`
(`0x2c314`), called per frame from `H264VideoEncoderDPB::ManageDPBBuffer`
(`0x2d294`, `bl` at `0x2d6e8`):

```
2c98c  ldr x9,[x0,#4704]                       ; the per-frame DPB record
2c990  ldr x10,[x9,#896]  ; str x10,[x1,#2432] ; -> PICMGMT + 0x980  SrcNbr Info[0]
2c998  ldr x10,[x9,#928]  ; str x10,[x1,#2464] ; -> +0x9A0  Pixel[0]
2c9a0  ldr x10,[x9,#960]  ; str x10,[x1,#2496] ; -> +0x9C0  Data[0]
 …                                             ; entries [1..3] of each group
2c9f0  ldr x10,[x9,#992]  ; str x10,[x1,#2528] ; -> +0x9E0  FwData[0]
2ca10  ldr x10,[x9,#1024] ; str x10,[x1,#2560] ; -> +0xA00  entropy[0][0]
2ca18  ldr x10,[x9,#1032] ; str x10,[x1,#2568] ; -> +0xA08  entropy[0][1]
 …  continuing in lockstep to  [x9,#1528] -> [x1,#3064] = PICMGMT + 0xBF8
```

**[C]** for every store (a straight linear copy of 64 u64, `[x9,#1024+8n] →
PICMGMT+0xA00+8n`). `x1` is PICMGMT by the mangled signature, and the same
function is already known to overwrite `+0x8B8` (docs/60 §3.3) and `+0xC20`
(docs/53). **[C]**

This is why the coordinator's host-side dump and the hardware disagree: both
are right. We put the table on the wire; `setRefPointers` runs first and
replaces it.

### 10.2 The record it copies from is filled at Start_AVC, from wire `0xF830`

`CAVECommonDPB::ProvideReferenceFrames(u32, AVE_VIDEO_PARAMS *x2)` (`0x2b530`):

```
2b61c  mov w8, #0xf770
2b83c  add x8, x2, x8                          ; x8 = VP + 0xF770
2ba14  ldr x9,[x8]        ; str x9,[x0,#952]   ; SrcNbr Info[0]
2ba20  ldr x9,[x8,#32]    ; str x9,[x0,#984]   ; Pixel[0]
2ba28  ldr x9,[x8,#64]    ; str x9,[x0,#1016]  ; Data[0]
 …                                             ; entries [1..3] of each group
2ba78  ldr x9,[x8,#1792]  ; str x9,[x0,#1048]  ; FwData[0]
2ba98  ldr x9,[x8,#96]    ; str x9,[x0,#1080]  ; entropy[0][0]
2baa0  ldr x9,[x8,#104]   ; str x9,[x0,#1088]  ; entropy[0][1]
 …  linear to  [x8,#600] -> [x0,#1584]         ; 64 u64 = encoder_addr_entropy[16][4]
```

**[C]** `x0` is the DPB object (`str wzr,[x0,#4696]` sits in the middle of the
block, next to the `[x0,#4704]` pointer `setRefPointers` dereferences).

The two listings interlock with a constant offset of **56**: `[x9,#896] =
[x0,#952]`, `[x9,#928] = [x0,#984]`, `[x9,#992] = [x0,#1048]`, `[x9,#1024] =
[x0,#1080]`, `[x9,#1528] = [x0,#1584]`. **[I]**, but over twenty independent
stores agree, so the record `[dpb+4704]` points at is `dpb + 56`. That is the
same record whose `+512 + 8k` holds the colocated pointers
(`str x21,[x20,#512]` `0x2b790`, docs/60 §3.3), which is the one field of it
that F13 proves arrives on hardware (`0x40D13038C = 0xfd500000`, the Start_AVC
slot-0 colocated buffer). **[C]**

### 10.3 The wire offsets

`AVE_VIDEO_PARAMS` is the Start_AVC command `+0x60`, so **wire = VP + 0x60**
(docs/53 §13.2, and the driver's own `colocated_set = 0xf6b0` for VP `+0xF650`).
`InitEncodingParameters` reads the same block through `x23 = VP + 0xF760`
(`0x5d874`: `[x23,#16]` = VP `+0xF770` = wire `0xF7D0` =
`encoder_addr_src_nbr_info`, which is the driver's `src_nbr_set[0]`). **[C]**
Both sites therefore agree that `x8 = VP + 0xF770 = wire 0xF7D0`.

| firmware offset | VP | **wire** | field | driver today |
|---|---|---|---|---|
| `x8 + 0 .. 24` | `0xF770` | `0xF7D0` | SrcNeighbor **Info**[0..3] | `src_nbr_set[0]` ✓ |
| `x8 + 32 .. 56` | `0xF790` | `0xF7F0` | SrcNeighbor **Pixel**[0..3] | `src_nbr_set[1]` ✓ |
| `x8 + 64 .. 88` | `0xF7B0` | `0xF810` | SrcNeighbor **Data**[0..3] | `src_nbr_set[2]` ✓ |
| **`x8 + 96 .. 607`** | **`0xF7D0`** | **`0xF830 .. 0xFA2F`** | **`encoder_addr_entropy[16][4]`**, u64, row stride `0x20`, column stride `8` | **not written** |
| `x8 + 1792 .. 1816` | `0xFE70` | `0xFED0` | SrcNeighbor **FwData**[0..3] | `AVE_OFF_NONE` |

**[C]** for the firmware offsets; **[I]** for the wire column, from the
`+0x60` convention, cross-checked three ways: `src_nbr_set[0..2]` already work
at exactly these offsets, `InitEncodingParameters` reaches the FwData field as
`[x23,#1808]` = VP `+0xFE70` (`0x5d8a0`) which is the same field
`ProvideReferenceFrames` reads as `x8+1792`, and `colocated_set` lands where
docs/53 §13.2 put it.

**This is the bug.** docs/59 §3 and `driver/ave_abi.h` treat the fourth
SrcNeighbor group as sitting at wire `0xF830` (it is `AVE_OFF_NONE` at
Start_AVC, so nothing was ever written there and nothing broke). `0xF830` is
not the fourth SrcNeighbor group — it is the **first row of
`encoder_addr_entropy`**. The real fourth group is at `0xFED0`, half a command
away.

### 10.4 The gates, for completeness

The coordinator asked which gates could skip the copy. All of them are open on
our session, so the copy did run — and copied zeros:

- `ProcessPipeStart` skips `PipePrepareParam` only when `ctrl[4664] == 2`
  (`0x52e10`), the multi-core split. We send `sve_num = 1` → 1. **[C]**
- Inside `PipePrepareParam`, `cbz w9, 0x481e8` at `0x48184` with
  `w9 = ctrl[5176]` (`0x480d8`) takes the **fresh-frame** path; the non-zero
  (chunk-resume) path returns before any field copy. `ctrl[5176]` is the chunk
  start row, 0 on a fresh frame (`0x59d30`, `0x5ba64`). **[C]**
- `0x48638` is reached as the **normal exit of the 16-iteration reference loop**
  (`cmp x13,#0x10 ; b.eq 0x48638` at `0x484b0`), not a special case, and
  `0x4863c/0x48644` forward both `frame_type == 0` and `== 3` to `0x48668`.
  Our IDR sends 3, and the firmware history line confirms
  `FrameType 3` reached it. **[C]**
- `ctrl[3768]` (`num_encoder_addr_entropy`, the `cbz` at `0x487a0`) is **4**:
  `InitEncodingParameters` `0x5d05c..0x5d074` writes 4 on the
  `ctrl[4664] != 2` arm. **[C]**
- `ctrl[4740]` (the column index) is `cmd[44]` of the pipe command
  (`0x52da4/0x52db0`), a firmware-internal field; filling all four columns
  makes it irrelevant.
- `[x23,#52]` is `cmd[52]` of the same pipe command. Non-zero selects the
  four-column copy (`0x487e0..0x4888f`); zero selects a variant that
  **replicates column 0** into all four (`0x48898..0x48b7f`). Either way the
  source is PICMGMT `+0xA00 + 32i`. **[C]**
- The command is not truncated: `CFlowControllerBase::ProcessAvcEncode`
  memmoves the whole command with a hardcoded `mov w2, #0x1940` = **6464**
  bytes (`0xf050..0xf05c`), and `SendCommandToQueue` then takes PICMGMT as
  `cmd + 0x9c8` (`0x145c8`). **[C]** So every byte we send is visible to the
  firmware — it is simply overwritten.
