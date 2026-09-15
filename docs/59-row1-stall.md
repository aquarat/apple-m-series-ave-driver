# The row-1 stall: what the MCPUs are waiting for, the neighbour DMA, and what to read next

Prompted by F10 (`results/f10-1789490043.kmsg`, [53](53-first-frame.md) §21):
one 1280x720 AVC I-frame (80x45 MBs, QP 30, Baseline, CAVLC), macOS 13.5
firmware `AppleAVE2FW-6070.11.1`, t6001 (DevID 14). At the Pipe hang
`0x40D12002C = 0x00010009` (currMbRow 1). MbInput host-if `+0 0x48000, +4 0,
+8 0x2201f`. IntraEst and CAVLC `+0 0x2000, +4 0, +8 0x1f`.

This continues [57](57-pipe-hang.md), [58](58-pipe-start.md) and
[53](53-first-frame.md) §18-§21. VAs are 13.5 firmware image VAs (file offset =
VA + `0x4000`). **MCPU VAs** are offsets inside the embedded Cortex-M image:
`mbinput:0x1230` means firmware file offset `0xdadc0 + 0x4000 + 0x1230`.
Images are MbInput `0xdadc0`, MotionEst #1 `0xe0dd0`, IntraEst `0xe3350`,
ModeDecision `0xe37a0`, ReconLuma `0xe4480`, ReconChroma `0xe4a90` and CAVLC
`0xe5650` ([58](58-pipe-start.md) §1.2). The MCPU sees encoder register
`base+X` at `0x40000000+X`, so MCPU `0x41168000` is AP `0x40D168000`
([58](58-pipe-start.md) §1.3). Its DMem `0x10000000+x` is readable by the host at
AP `0x40C000000 + DMem window + x`.

Labels follow [00](00-methodology.md): **[C]** read from an instruction (VA
cited), **[I]** inferred (the chain is stated), **[U]** unknown. Nothing here
was run on hardware.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| Host-interface layout | `+0` is the per-bit event **enable**, `+8` is the **raw W1C pending** register. MCPU NVIC IRQ *k* corresponds to host-if bit *k−14*. This holds for 4 cores and 7 handlers | C (per handler) / I (general rule) |
| MbInput `+0 = 0x48000` | exactly what MbInput `main` writes (`mbinput:0x1c8..0x1cc`). Bit 16 (the "inject the next MB" event) is **off**, which MbInput's consumer does itself when it has nothing to inject. Bits 15 and 18 are on | C |
| MbInput `+8 = 0x2201f` | bits 15 and 18 not pending: **no new source-MB event has arrived**. Bit 16 not pending: **the next stage is not asking for an MB**. Bits 13 and 17 are pending but no MbInput handler exists for them | C (bits) / I (meaning) |
| IntraEst / CAVLC `0x2000 / 0x1f` | `main` enabled bit 13 (`intraest:0x192..0x1a0`, `cavlc:0x198..0x1a6`). No MB request is pending: **idle and not being offered an MB** | C / I |
| What MbInput is | A 15-MB lookahead gate. Source-analysis events (bit 15) *produce* 192-byte MB records. Bit-16 events *consume* one record into the encode chain, and only while produced ≥ consumed + `[base+0x1170110]`. The firmware sets that register to **15** (`0x61220..0x61234`) | C |
| Does "currMbRow 1" mean the encode reached row 1? | **Not necessarily.** `0x112002C` is the source reader, which is the pipeline *head*. The encode chain behind MbInput runs ≥ 15 MBs behind it. A head at MB 80..95 puts the tail inside row 0 | I |
| Stall location from the MCPU words | Downstream of MbInput injection and upstream of (or inside) the stages that raise MB requests to IntraEst / CAVLC. The chain is back-pressured: B (the stage MbInput injects into) is not ready, and the head is not producing. The ME, ModeDecision and Recon host-ifs were not read | I |
| Who reads top neighbours | Hardware, not MCPUs. Two DMA **readers** (`0x1120C00` info, `0x1120C40` pixels) and two **writers** (`0x1130600`, `0x1130640`) point at the **same buffer**, SrcNeighbor Info[0] / Pixel[0]. The writers store row *n*, the readers fetch it for row *n+1*. No MCPU image touches these blocks | C (programming) / I (roles) |
| Is that programming gated | Only by `ctrl[4664]==2` (multi-core split, `VP+0x10d88 > 1`). On our session all four channels are programmed with control `0x80030001`, address and size | C |
| Neighbour sizes | Info `mbW<<6` = 5120 B, Pixels `max(mbW<<10, 0x4000)` = 81920 B. Our 80 KiB slots are exactly big enough | C (formula) / I (`ctrl+0xA98` = mbW) |
| DevType counts | **Irrelevant to the Pipe.** Firmware reads only **entry 0** of Info and Pixel, at Start_AVC and per frame. The kext copies non-zero entries until the first zero; there is no count field on the wire. Data (entry chosen by an index) is used only by Transcode, which never starts | C |
| Best next step | A read-only diagnostic run: MbInput's produced/consumed counters decide whether the tail is in row 0 or row 1 (§5). Then the ranked changes in §6 | — |

---

## 1. Q2 first — decoding the stall words

### 1.1 `+0` is enable, `+8` is raw pending, IRQ *k* ↔ bit *k−14*

Every handler acknowledges exactly one `+8` bit, and that bit is its own IRQ
minus 14:

| core | IRQ (vector) | handler | ack write | bit | VA |
|---|---|---|---|---|---|
| MbInput | 29 | `0x1231` | `+8 = 0x8000` | 15 | `mbinput:0x1344..0x1348` |
| MbInput | 30 | `0x1363` | `+8 = 0x10000` | 16 | `mbinput:0x1788..0x178c` |
| MbInput | 32 | `0x17a1` | `+8 = 0x40000` | 18 | `mbinput:0x1830..0x1834` |
| MbInput | 33 | `0x1353` | `+8 = 0x80000` | 19 | `mbinput:0x1352..0x135e` |
| IntraEst | 27 | `0x1a9` | `+8 = 0x2000` (`[r0-12]`, r0 = `0x41242014`) | 13 | `intraest:0x34a..0x34e` |
| CAVLC | 28 | `0x1af` | `+8 = 0x4000` | 14 | `cavlc:0x532..0x53e` |
| MotionEst #1 | 14 | `0x5ed` | `+8 = 1`, `+0 &= ~1` | 0 | `me1:0x5ec..0x5fe` |

**[C]** Handler addresses come from the vector tables (word 16+IRQ of each image).
MbInput's IRQ 28 handler (`0x1123`) clears `+0` bit 14 on exit
(`mbinput:0x121c..0x1224`). IRQ 29 turns bit 14 on and bit 15 off when the
source event carries the "last MB" flag (`0x41175804` bit 26,
`mbinput:0x1272..0x1286`). **[C]**

`+0` is therefore the per-bit enable, and the cores change it at run time. F10
shows bits 13 and 17 pending in MbInput's `+8` while `+0` does not enable them
and MbInput has no vector for IRQ 27 or 31. So `+8` reports **raw status,
unmasked**. **[I]**, strong.

The ASC side matches ([58](58-pipe-start.md) §1.2): its ISR manager writes
`0xffffffff` to `+8` and acknowledges bits 8 and 12.

### 1.2 `+0` values are the ones `main` writes

- MbInput `main` (`mbinput:0x184..0x1d2`) enables NVIC 29, 30, 32, 33 and 28.
  If DMem `0x10000240 & 0x3c == 0` it writes `2` to `0x41170284`. It then
  writes **`+0 = 0x48000`** (`0x1c8..0x1cc`) and idles in `wfi`. **[C]**
- IntraEst `main` (`intraest:0x184..0x1a6`): NVIC 27, `+0 |= 0x2000`.
- ModeDecision (`modedec:0x184..0x1aa`), ReconLuma (`reconluma:0x184..0x1a6`),
  ReconChroma (`reconchroma:0x184..0x1aa`) and CAVLC (`cavlc:0x184..0x1aa`)
  do the same with their own blocks: `0x41262000`, `0x41282000`, `0x412a2000`,
  `0x412c8000`.
- MotionEst #1 (`me1:0x184..0x1aa`): NVIC 29 and 14, `+0 |= 0x8000`. **[C]**

Every core read in F10 therefore **ran `main`**. That confirms
[58](58-pipe-start.md) §7.2 on hardware (the images execute). **[C]**/**[I]**

### 1.3 MbInput is a lookahead gate, and its consumer is parked

- **Producer, IRQ 29 / bit 15** (`mbinput:0x1230..0x1350`). It reads the MB
  position from `0x41175800/804`: bits 16..27 = MB y, bits 0..12 = MB x, bit 26 =
  last. It computes the MB QP. With DMem `0x10000104 != 0` it uses
  `(y*mbW + x) / [0x10000104]` (`mbW = 0x41170004 & 0x1fff`); otherwise it takes
  a per-row byte `DMem[0x10000004 + y]`. It writes the QP into bits 4..11 of
  `0x41175804` (`0x128a..0x12de`). It increments **produced** `DMem 0x100008a8`
  (`0x12e4..0x12ea`), copies the stage's MB statistics from `0x4117c000` into a
  17-entry ring of 192-byte records at `DMem 0x100008b0` through `0x1d4`
  (`0x12ee..0x1314`), acknowledges the stage (`0x41170100 = 1`, `0x133e..0x1340`),
  and re-enables bit 16 once produced ≥ consumed + `[0x41170110]`
  (`0x1326..0x133a`). **[C]**
- **Consumer, IRQ 30 / bit 16** (`mbinput:0x1362..0x1794`). It pops a ring record
  into `0x41171180..0x4117123c` (`0x14aa..0x1534`, `0x165a..0x16f2`), posts
  `+0x14 = 0xc0000024 | …` (`0x1740`), writes `0x41170108 = 1` (`0x1750`) and
  increments **consumed** `DMem 0x10001584` (`0x1754..0x175e`). Unless the
  drain flag `DMem 0x10001570` is set, it **clears enable bit 16 when produced <
  consumed + `[0x41170110]`** (`0x1770..0x1786`). With the flag set it counts
  `0x41170110` down instead (`0x1762..0x176a`). **[C]**
- **Last MB, IRQ 28 / bit 14** (`mbinput:0x1122..0x122e`). It sets the drain
  flag (`0x114c`), produces the final records and uses a polled handshake on
  `0x41170104`. **[C]**
- The ASC writes **`base+0x1170110 = 15`** in `ConfigureMCPUs`
  (`0x61220..0x61234`: `x9 = base+0x117010c`, `str w10(=15),[x9,#4]`). It also
  sets `0x117010c |= 2`, `0x1170124 = 0` and `0x1170128 = 0x3fff3fff`
  (`0x61238..0x61264`). **[C]**

The records copied from `0x4117c000` are sums and extremes of per-MB
statistics (`mbinput:0x1dc..0x366` feeding `0x36e..`). The ASC later logs
`VarSum/VarMax/VarMin` from MCPU memory (`CollectDataFromCpus`, string
`0xc7067` at `0x5a1c0`). So the producer is **source analysis** (the head of
the pipe) and the consumer feeds the **encode chain**, 15 MBs behind. **[I]**

### 1.4 What F10's words say

| bit | enabled | pending | reading |
|---|---|---|---|
| 15 (source MB event) | yes | **no** | the head has not delivered a new MB |
| 16 (encode chain wants an MB) | **no** (consumer parked itself) | **no** | the next stage is not ready. If it were starved, bit 16 would be pending |
| 18 (IRQ 32: two `+0x14` messages `0xe00b0004`, `0xe0060004 \| [0x41170c10]<<28`, `mbinput:0x17a0..0x1838`) | yes | no | not fired [U: meaning] |
| 13, 17 | no | yes | no MbInput handler exists [U] |
| 0..4 | no | yes (all cores) | the same on every core, so probably level status common to all host-if blocks [U] |

**[C]** bit decode. **[I]** the reading: the head is not producing and the
chain is not accepting, while IntraEst and CAVLC are not being offered MBs.
That is **back-pressure from a stage between MbInput's injection and the
IntraEst / CAVLC request points**: the hardware MotionEst, IntraEst,
ModeDecision or Recon stages, or a DMA channel one of them waits on. The
host-ifs of MotionEst (`0x40D188000`), ModeDecision (`0x40D262000`),
ReconLuma (`0x40D282000`) and ReconChroma (`0x40D2A2000`) were not read in
F10. **[U]** which stage.

### 1.5 `0x40D12002C`: row 1 is the head, not the tail

- `ProcessPipeDone` (`0x59690..0x596c4`) takes `>> 16` as currMbRow and marks
  the frame done iff `currMbRow + 1 == ctrl[0xA94]` (pic_height_in_mbs). HEVC
  does the same with `(v>>16 & 0xfffe) + 2` (`0x763e0..0x7640c`). The field is
  the last row the reader has reached. **[C]**
- Only those two functions read `0x112002C`. The low 16 bits (`0x0009`) are
  never decoded in firmware. **[U]** A column index would be consistent (MB 89).
- The reader sits in the source-DMA block (`0x1120000` control word, `+0x14`
  luma stride from PICMGMT `+0x8C8`, `+0x20` crop origin, `setPipe`
  `0x5480c..0x549c0`). MbInput injects into the encode chain only while it
  holds ≥ 15 unconsumed records (§1.3). **So the tail is at least 15 MBs behind
  the head**, more if the hardware buffers further. **[I]**

If the low half is a column (head at MB 89), consumed ≤ 74 and **no MB of row 1
has entered the encode chain**. The stall would then be inside row 0 and have
nothing to do with top neighbours. If it is not a column, the head can be
anywhere in 80..159 and row 1 may have entered. The MbInput counters settle
this (§5).

---

## 2. Q1 — what row 1 needs that row 0 did not

### 2.1 The MCPUs do not handle neighbour data

- IntraEst's only per-MB handler (`intraest:0x1a8..0x352`) does the following.
  It posts `+0x14 = 0x80000025` and polls bit 31. It reads the MB context at
  `0x41243000/3180/3184`. It writes intra parameters (`0x4124a1c8..a1d8`),
  optionally a λ from its QP table at `0x412`. It posts `+0x14 = 0x60000000`,
  polls bits 29-30, writes go `0x4124a080 = 1` and acknowledges bit 13. Its
  one row-aware piece (`0x2d0..0x366`) saves `0x4124a1d4/d8` into DMem
  `0x10000768/76c` at MB (0,0), restores them at `x != 0`, and writes zero at
  `x == 0, y != 0`. Register bookkeeping only; nothing waits. **[C]**
- CAVLC's IRQ 28 handler (`cavlc:0x1ae..0x546`) accumulates per-MB bits and QP
  statistics into DMem; there is no top-nC array. **[C]**
- Every register constant (movw/movt pair) in the nine images falls in the
  core's own stage blocks only: MbInput `0x4116xxxx/0x4117xxxx`, MotionEst #1-#3
  `0x4118/0x4119`, IntraEst `0x4124`, ModeDecision `0x4126`, ReconLuma
  `0x4128`, ReconChroma `0x412a`, CAVLC `0x412c/0x412d`. None falls in the DMA
  blocks `0x4112xxxx` / `0x4113xxxx`. **[C]** (annotated scan of the pairs; the
  same scan finds each core's known host-if base, so it is not blind).
  Table-driven access is not excluded (Trap 3). **[I]**

So the top-neighbour line buffer is a **hardware** concern, read and written
through DMA. **[I]**, strong.

### 2.2 The neighbour DMA channels (AVC `setPipe`)

Registers are `x21 = 0x1120fa4`, `x23 = 0x1130700`, and `w24 = 0x80030001`
(`0x554a0..0x554a4`, no later writer of w24 before use):

| channel | ctrl | address | size | VA |
|---|---|---|---|---|
| reader, Info | `0x1120C00 = 0x80030001` | `0x1120C0C = ctrl+4544` (src_nbr_info) | `0x1120C10 = ctrl+0x8c28` | `0x557a0`, `0x557e4`, `0x557f4` |
| reader, Pixels | `0x1120C40 = 0x80030001` | `0x1120C4C = ctrl+4560` (src_nbr_pixels) | `0x1120C50 = ctrl+0x8c2c` | `0x557a4`, `0x55810`, `0x5581c` |
| writer, Info | `0x1130600 = 0x80030001` | `0x113060C = ctrl+4552` (dst_nbr_info) | `0x1130610 = ctrl+0x8c28` | `0x5583c`, `0x55850`, `0x55860` |
| writer, Pixels | `0x1130640 = 0x80030001` | `0x113064C = ctrl+4568` (dst_nbr_pixels) | `0x1130650 = ctrl+0x8c2c` | `0x55840`, `0x55884`, `0x5588c` |

(log strings `encoder_addr_src_nbr_info/pixels`, `dst_nbr_info/pixels`
`0xc68f3`, `0xc68c4`, `0xc6894`, `0xc6920`.) Before these writes `0x1130620`
and `0x1130660` are zeroed (`0x55708`, `0x5572c`). **[C]**

**Gates.** With `ctrl[4664] == 2 && ctrl[4668] != 0` the readers are zeroed
(sizes `0x40`) (`0x55730..0x55784`). With `ctrl[4664] == 2 && ctrl[4668] == 0`
the writers are zeroed (`0x55894..0x558b4`). `ctrl[4664] = (VP+0x10d88 > 1) ? 2
: 1` and `ctrl[4668] = VP[0x10d90 + 8*VP[0x10dac]]`
(`InitEncodingParameters` `0x5ca88..0x5cac4`), which is the multi-core split
(top / bottom part). For a single-core session all four channels are
programmed. **[C]** code. `VP+0x10d88` is wire `0x10DE8` (VP = cmd `+0x60`),
which the driver sends as `sve_num = 1` (`driver/ave_abi.h:1319`, "single core
= 1"). So `ctrl[4664] = 1`, and both the reader and writer pairs are programmed.
**[C]** for the driver value, **[I]** for the wire mapping (the same `+0x60`
convention as docs/57 §4.2).

**No flag like NEED_LSB_PLANES gates them**, and none of their asserts
(`6990/6991/6993/6994`) fired. **[C]**

**Pointers.** `ResetDMANeighborRegs` (`0x27c88`) zeroes `+0x14/+0x18` of all four
(`0x1120C14/C18/C54/C58`, `0x1130614/618/654/658`). `StartEventPipe` calls it
before every pipe start ([57](57-pipe-hang.md) §2.2), and `setPipe` calls it
again when the per-client "resume" byte `ctrl[0x2108 + 2*ctrl[4732]]` is 0
(`0x53048` → `0x53108..0x53114`). With that byte set (continuing a partially
encoded frame) the pointers are loaded instead. Reader `+0x14` = one row
(`mbW<<6` info, `mbW<<8` pixels), `+0x18` = `(rows-1)*row % size`. Writer
`+0x14` the same, `+0x18` = `rows*row % size` (`0x5304c..0x53100`,
`rows = ctrl[5176] >> 2`). **[C]** So `+0x10` is a ring size, `+0x14` a per-row
stride and `+0x18` a start offset. **[I]**

### 2.3 Sizes and buffer identity

- `ctrl+0x8c28 = mbW << 6`, `ctrl+0x8c2c = max(mbW << 10, 0x4000)`, written by
  `stp w8,w9,[x21,#32]` with `x21 = ctrl+0x8c08` (`0x5df8c..0x5dfb4`; x21 comes
  from `0x5ce64`, saved at `0x5d0c4` and reloaded at `0x5dbc4`). **[C]**
  `ctrl+0xA98` is mbW. `ctrl+0xA94` is pic_height_in_mbs by `ProcessPipeDone`'s
  compare, and `0xA98` is its neighbour with the same `≤ 128`-after-align use
  (`0x5d2b4..0x5d2d4`). **[I]** strong. The pixel floor `0x4000` equals the
  kext's `AVE_CalcBufSizeOfSrcNeighbor*` floor ([53](53-first-frame.md) §12).
- At 1280: Info ring **5120 B**, Pixel ring **81920 B = 80 KiB**. The driver's
  slot is 80 KiB (kmsg l.963), so Pixel fits exactly and Info has 16× headroom.
  **[C]** for our sizes.
- **src and dst are the same buffer.** `InitEncodingParameters` stores
  `[x23+16]` (wire `0xF7D0`, Info[0]) into both `ctrl+4552` and `ctrl+4544`
  (`0x5d874..0x5d88c`), and `[x23+48]` (wire `0xF7F0`, Pixel[0]) into
  `+4568` and `+4560` (`0x5d890..0x5d89c`). Per frame, `PipePrepareParam` does
  the same from PICMGMT `+0x980` (Info[0]) and `+0x9A0` (Pixel[0])
  (`0x48678..0x486a0`). **[C]** The reader fetches row *n−1* from the ring the
  writer filled. **[I]**

### 2.4 Answer

On the firmware side, row ≥ 1 needs the **neighbour readers** to find what the
**neighbour writers** stored for row 0, in SrcNeighbor Info[0] / Pixel[0].
The firmware programs all four channels unconditionally for our session,
with sizes that fit our buffers. **Nothing in the firmware is left
unprogrammed for row 1.** **[C]** A hardware-level failure of that loop
remains possible and was not observed: for example a writer whose output never
lands, or a reader that needs a condition the firmware cannot see. **[U]** It
would stall exactly at the first MB of row 1 *in the encode chain*, which §1.5
shows F10 has not yet proved was reached.

---

## 3. Q3 — SrcNeighbor counts, sizes, indices

| group | wire (Start_AVC) | PICMGMT (Process) | firmware reads | used by | Conf |
|---|---|---|---|---|---|
| Info | `0xF7D0 [0..3]` | `+0x980 [0..3]` | entry **0** only (`0x5d874`, `0x48678`) | Pipe neighbour reader+writer | C |
| Pixel | `0xF7F0 [0..3]` | `+0x9A0 [0..3]` | entry **0** only (`0x5d890`, `0x48694`) | Pipe neighbour reader+writer | C |
| Data | `0xF810 [0..3]` | `+0x9C0 [0..3]` | `VP+0xF7B0 + 8*byte` (`0x5d8b0..0x5d8bc`), then `PICMGMT+0x9C0+8k` in `ProcessTranscodeStart` (`0x583ac`) | **Transcode only** (`SetTranscode` asserts `0x5869c`) | C |
| FwData | `0xF830 [0..3]` | `+0x9E0 [0..3]` | no firmware reader found | — | U |

- **Kext.** `AVE_CHM_SetDataInfo_FwBuf` copies each of the four groups entry by
  entry from the client (`+0xD0230`, `+0xD0250`, `+0xD0270`, `+0xD0290`)
  **until the first zero** (`cbz x0` at `0xfffffe0008eb0bec`, `…0c24`,
  `…0c5c`, `…0c94`). A fifth, 4×16 table goes to PICMGMT `+0xA00..+0xBF8`
  (`0xfffffe0008eb0cb8..0d0c`). **[C]** No count field exists; "count" is
  simply how many surfaces the client allocated.
- **So the DevType 14 counts in docs/47 (Info/Pixel 1) change nothing.** With
  one entry the kext fills `[0]` and leaves `[1..3]` zero. We fill all four,
  and the firmware reads only `[0]`. **[C]** for the reads cited. That no other
  reader of entries 1..3 exists is **[I]** (immediate/offset scan, Trap 3).
- There is **no per-frame neighbour rotation** for the Pipe. The index byte
  applies only to Data (Transcode). **[C]**
- **Sizes.** Info needs 5120 B (our slot 80 KiB) and Pixel needs 81920 B (our
  slot 80 KiB, exact). Data and FwData are not used by the Pipe. **[C]**/**[I]**
- The PICMGMT `+0xA00` 4×16 table the kext publishes is not written by our
  driver. Its firmware reader was not located. **[U]** It is worth a later
  look; the Pipe asserts nothing about it.

---

## 4. Q4 — other per-row consumers

| consumer | buffer / register | we send | firmware on zero | stalls at row 1? | Conf |
|---|---|---|---|---|---|
| Neighbour info/pixels | §2 | yes (80 KiB) | asserts | only if the HW loop fails | C / U |
| MbInput per-row QP table | MCPU DMem `0x10000004 + y` (`mbinput:0x12ba..0x12c4`), written by `ConfigureMCPUs` | firmware-owned | — | no: a wrong byte gives a wrong QP, not a wait | C / I |
| MbInput lookahead | `0x1170110 = 15` (`0x61234`) | firmware-owned | — | deadlocks only if the HW source stage cannot buffer ≥ 15 MBs, which would fail on macOS too | I |
| MB-address / MBStats FW data | `ctrl+0x1bb0/0x1bb8` → reader `0x1120C8C`, writer `0x113070C`, ctrl `0x80030001` (`0x55154..0x551a4`) | no | gated by `ctrl[0x23B38+1175]`. Gate off → writer `0x1130700 = 0x80030000`, size `0x40` (`0x54fdc..0x54ff4`). Gate on + zero → assert 6727/6659 | no assert seen, so the gate is off and the channel is **disabled, not waiting** | C |
| stats DMA | writer `0x113070C/710/700` (`0x54c80..0x54c88`) | no | gated by `[ctrl+0x122b]+2429/2430`. Gate on + zero → assert 6697 | no assert, so the gate is off | C |
| ME SFS results readers ("RDDMAMESFSRSLTS") | 4 channels, base `0x1120F8C+0x40i`, stride `F90`, size `F98`, credit `F9C`, `F94` (`0x532a8..0x53434`) | LowResResults = 0 → base never written (`cbz` `0x533c0`) | with `ctrl+0x23FEC` (async LRME, wire `0xFCE9` [I]) = 0: stride 0, credit 0, `F94 = 1`. With async: credit = size, `F94 = 0` | possible only in async mode. The kext's per-frame `SetDataInfo_FwBuf` does not write PICMGMT `+0xC28..C40` either (writes at `0xC08/0xC10/0xC18` only), so macOS probably leaves them zero too | C / I |
| CAVLC top-nC | hardware (neighbour Info). The CAVLC MCPU keeps no row array | — | — | same as row 1 | I |
| Rate-control row stats | MbInput ring (§1.3), CAVLC IRQ 28 accumulators | firmware-owned | — | no | C |
| Recon writer | `0x1130240..` (programmed since F7) | yes | — | writes 32-px tile rows (2 MB rows), so it would first flush at MB row 2 | I |

---

## 5. Q5 — read-only diagnostics that pinpoint the stage

All addresses lie inside the mapped bank 0 (`0x40D100000 + 0x45C000`) or in the
DMA arena the driver owns. The firmware reads the MCPU windows itself
([58](58-pipe-start.md) §7.0). **Order matters:** read `+8` registers last
(W1C semantics, §1.1; a *read* is not expected to clear, but that is **[U]**).

**Tier 1: decides row 0 or row 1 (MbInput DMem = AP `0x40D408000 + x`):**

| AP | MCPU name | meaning | what it discriminates |
|---|---|---|---|
| `0x40D4088A8` | `DMem 0x100008a8` | **produced** (source MBs seen) | head position in MBs |
| `0x40D409584` | `DMem 0x10001584` | **consumed** (MBs injected into the encode chain) | tail upper bound: `< 80` → stall inside row 0; `≥ 80` → row 1 reached |
| `0x40D409570` | `DMem 0x10001570` (u8) | drain flag | 1 would mean the last MB was seen (not expected) |
| `0x40D409580`, `0x40D409588` | ring indices | consistency check | |
| `0x40D170110` | lag | expect 15, falling only when draining | |
| `0x40D175800`, `0x40D175804` | last source event | `>>16 & 0xfff` = y, `& 0x1fff` = x, bit 26 last | cross-check with `0x40D12002C` low half |
| `0x40D170004` | mbW field | expect `& 0x1fff = 80` | apparatus |

**Tier 2: which stage holds the chain.** For each host-if base read `+0, +4, +8,
+0xc, +0x10, +0x14`. `+0x14` bit 31 set is a stuck mailbox, and this discriminates
by itself.

| stage | AP host-if | expected if idle-starved | if holding |
|---|---|---|---|
| MbInput | `0x40D168000` | as F10 | `+0x14` bit 31 set |
| MotionEst | `0x40D188000` | `+0 0x8000` | `+8` bit 15 pending, or `+0x14` busy |
| IntraEst | `0x40D242000` | as F10 | `+0x14` `0x80000025` stuck |
| ModeDecision | `0x40D262000` | `+0 0x2000` | `+8` bit 13 pending, or `+0x14` busy |
| ReconLuma | `0x40D282000` | `+0 0x2000` | same |
| ReconChroma | `0x40D2A2000` | `+0 0x2000` | same |
| CAVLC | `0x40D2C8000` | as F10 | same |

Also read `0x40D243180` (IntraEst's view of the current MB: y `>>16 & 0xfff`, x
`& 0x1fff`, as decoded by `intraest:0x2f4..0x2fc`). That is the last MB IntraEst
was offered. **[I]** field layout from the MbInput equivalent.

**Tier 3: the neighbour loop.**

- Read `0x40D120C00..0x40D120C5C` and `0x40D130600..0x40D13065C` (the
  programmed words plus the `+0x04/+0x08/+0x14/+0x18/+0x1C` neighbours). Expect
  ctrl `0x80030001`, addresses `0xff000000` / `0xff050000`, sizes `0x1400` /
  `0x14000`. Any word that moves from 0 indicates progress. **[I]**
- **Host memory, not a register:** before Start_AVC, fill the SrcNeighbor
  arena with `0xA5`. At the timeout, dump the first 64 bytes and bytes
  `0x13C0..0x13FF` of Info[0] (`0xff000000`) and Pixel[0] (`0xff050000`).
  Unchanged `0xA5` means the **writers never wrote row 0**. Changed means they
  did, which moves the suspect to the reader or elsewhere. The CPU side of a
  coherent buffer can be read safely.
- Also read `0x40D120F80..0x40D12105F` (ME SFS results readers, §4) and
  `0x40D110124` / `0x40D150100` (written in the sync-LRME arm `0x57ce4..0x57d18`).

---

## 6. Ranked causes, each with a change and a confirming observation

These are proposals for the operator (AGENTS.md). Run the §5 reads on every
attempt; they are informative whatever else changes.

### 6.1 The stall is inside row 0 of the encode chain, downstream of MbInput (medium)

**Why.** §1.4-§1.5: the chain is not ready and the head is lookahead-gated. A
row-1 head is the expected picture for a tail stuck at MB ≈ 60-75. The
upstream/downstream picture fits a stage between MbInput injection and the
IntraEst/CAVLC request points. MotionEst (I-frame, sync-LRME ME SFS readers
disabled, §4) and ModeDecision/Recon are unread.

**Change.** None first: Tier 1 + Tier 2 reads. **Confirm:** `consumed < 80`
with one of MotionEst/ModeDecision/Recon showing pending-but-unserviced `+8`
bit 13/15 or a busy `+0x14`. **Refute:** `consumed ≥ 80`, which promotes 6.2.

### 6.2 The neighbour write → read loop does not close at row 1 (medium-low; the only row-1-specific mechanism)

**Why.** §2: the only data an MB of row 1 needs that row 0 did not. It is fully
programmed, so a failure would be below the firmware (hardware or DART).

**Changes, one at a time:**

1. *Pattern + dump* (§5 Tier 3). This is diagnostic and changes nothing in
   behaviour.
2. *Separate slots per role.* Keep the wire as it is; the firmware forces
   src == dst anyway (§2.3), so this cannot be changed from the host. **Do not
   spend a run on it.**
3. *Grow the Info slot guard.* Not needed: 5120 B ≤ 80 KiB (§2.3).
4. The mapping is not the difference. Every session arena, SrcNeighbor and
   recon alike, comes from the same `dma_alloc_coherent`
   (`driver/ave_session.c:486`), which is bidirectional. **[C]** A no-write
   result would therefore point at the hardware writer, not at host permissions.

**Confirm:** Info/Pixel bytes changed from `0xA5` and `consumed ≥ 80`.
Fixed: currMbRow reaches 44, `0x40D110140` bit 2, `StartCount 1-1-1-1`,
completion `0x0E06`.

### 6.3 Async-LRME / ME SFS results readers expected on this configuration (very low)

**Why.** §4: four reader channels with base 0. They are harmless in sync mode
(stride and credit 0). If our wire byte `0xFCE9` (`ctrl+0x23FEC`, [I] offset) is
non-zero, they become credit-gated readers with no base, and the MotionEst stage
would wait. SRCDMAGO bit 3 reflects the same byte (`0x57d90..0x57d98`).

**Status.** No driver table or builder names wire `0xFCE9`: a grep of
`driver/ave_abi.h`, `ave_cmd.c` and `ave_session.c` for `fce9` or `async` finds
nothing. So the byte is sent as zero from the zeroed command buffer, which is
sync mode with the readers inert. **[I]** This drops to **very low**. Confirm
with a hex dump of Start_AVC offset `0xFCE9` in the next kmsg before discarding
it.

### 6.4 The PICMGMT `+0xA00` 4×16 table the kext publishes and we do not (low, unknown role)

**Why.** It is the only per-frame IOVA table macOS always fills (`0xfffffe0008eb0cb8..0d0c`) that
our Process leaves zero and that has no known firmware assert to protect it.
Its firmware reader is **[U]**.

**Change.** Static first: locate readers of PICMGMT `+0xA00..+0xBF8` (x27/x1-relative
`#2560..#3064` in `setPipe`, `ConfigureMCPUs`, `SetTranscode`). No run until a
reader is found.

### 6.5 MbInput lookahead deadlock on this silicon (very low)

`consumed == produced − 15` exactly with the source stage idle would mean the
hardware cannot hold 15 MBs. That is a firmware/silicon mismatch, not a host
bug, and would fail on macOS as well. **Change:** none. Record the counters.

### Suggested next run

One boot, no behavioural change: §5 Tiers 1-3 and the `0xA5` SrcNeighbor
fill. With `session_lsb=1 power_me1=1 dpe_tunables=1` as in F10, so the result
is comparable. The counters decide whether 6.1 or 6.2 is chased next.

---

## 7. Corrections and notes for other documents (not edited here)

1. **docs/53 §21 "Reading"**: "a stall at row 1 is what something needed only
   from the second row on would produce". The row comes from the source reader,
   which runs ≥ 15 MBs ahead of the encode chain (§1.5). Row 1 there is
   compatible with a row-0 stall. **[I]**
2. **docs/47 SrcNeighbor counts row**: correct for the kext, but without effect
   on the Pipe. The firmware reads only entry 0 of Info/Pixel (§3). **[C]**
3. **docs/58 §1.3** ("an MCPU … has no DMA path of its own"): confirmed for all
   seven images read here. They also never touch the neighbour DMA blocks (§2.1).
   **[C]**
4. **docs/58 §7.2**: the MCPU "not executing" cause is ruled out on hardware.
   F10's `+0` words are the values each `main` writes (§1.2). **[C]**

---

## 8. Reproduce

```sh
# MCPU images (Thumb); IRQ k handler = vector word 16+k
python3 - <<'EOF'
d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
for n,va,sz in [('mbinput',0xdadc0,0x18ec),('intraest',0xe3350,0x446),('modedec',0xe37a0,0xcde),
                ('reconluma',0xe4480,0x606),('reconchroma',0xe4a90,0xbb8),('cavlc',0xe5650,0x1500),
                ('me1',0xe0dd0,0x6f0)]:
    open(f'/tmp/{n}.bin','wb').write(d[va+0x4000:va+0x4000+sz])
EOF
objdump -D -b binary -m arm -M force-thumb /tmp/mbinput.bin | sed -n '/ 184:/,/ 1d2:/p'     # main, +0 = 0x48000
objdump -D -b binary -m arm -M force-thumb /tmp/mbinput.bin | sed -n '/1122:/,/1796:/p'    # IRQ 28/29/33/30 handlers
objdump -D -b binary -m arm -M force-thumb /tmp/intraest.bin | sed -n '/ 184:/,/ 36a:/p'
objdump -D -b binary -m arm -M force-thumb /tmp/me1.bin | sed -n '/ 5ec:/,/ 60a:/p'
# ASC firmware
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x61218 -n 0x50    # MbInput lag = 15
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x554a0 -n 0x420   # neighbour reader/writer programming
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x27c88 -n 0x48    # ResetDMANeighborRegs
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x53018 -n 0x100   # resume-mode neighbour pointers
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d874 -n 0x30    # src == dst, entry 0 (Start_AVC)
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x48668 -n 0x40    # src == dst, entry 0 (per frame)
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5df84 -n 0x34    # neighbour sizes
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5ca80 -n 0x48    # ctrl[4664]/[4668]
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x532a8 -n 0x190   # ME SFS results readers
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54a50 -n 0x5b0   # stats / MB-address channels
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x59688 -n 0x40    # currMbRow
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb0bd8 -n 0x138  # kext SrcNeighbor groups + 0xA00 table
```

---

## Note from review (2026-09-15)

§Q3's "the kext also fills a 4x16 table at PICMGMT `+0xA00..+0xBF8` that we
leave zero" is partly out of date: that range is `EncCommParams.encoder_addr_entropy`
(docs/54), and the driver has written its `[i][0]` entries for i < 4 since
`f0806f1` (F3 onwards, `session: entropy: 4 buffers of 960 KiB`). The
remaining `[i][1..3]` and rows 4..15 are still zero. Its consumer is the
Transcode stage, which has not started in any run, so it cannot explain a
Pipe stall; kept as a note rather than a cause.
