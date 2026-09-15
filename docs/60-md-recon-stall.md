# The ModeDecision / ReconLuma stall (F11): the `+0x14` mailbox, where the two cores are, and what they output

Prompted by F11 (`results/f11-1789492087.kmsg`, [53](53-first-frame.md) §22):
one 1280x720 AVC IDR (80x45 MBs, QP 30, Baseline, CAVLC, 1 slice), macOS 13.5
firmware, t6001. MbInput produced 48 / consumed 34. ModeDecision
`+8 0x201f, +0x14 0x80000126` and ReconLuma `+8 0x201f, +0x14 0x8000012b`; every
other stage idle.

Continues [58](58-pipe-start.md), [59](59-row1-stall.md). Conventions as
docs/59. Firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`). MCPU VAs
are offsets inside the embedded Thumb image. Images: MbInput `0xdadc0`,
MotionEst #1 `0xe0dd0`, IntraEst `0xe3350`, ModeDecision `0xe37a0`, ReconLuma
`0xe4480`, ReconChroma `0xe4a90`, CAVLC `0xe5650`. So `modedec:0x1ac` is file
offset `0xe37a0 + 0x4000 + 0x1ac`. An MCPU sees encoder register `base+X` at
`0x40000000+X`. The AP sees `base+X` at `0x40C000000+X`. MCPU DMem
`0x10000000+x` is AP `0x40C000000 + DMem window + x`. The DMem windows are
ModeDecision `0x1468000`, ReconLuma `0x1488000` and CAVLC `0x14c8000`
([58](58-pipe-start.md) §1.2).

Labels per [00](00-methodology.md): **[C]** read from an instruction (VA
cited), **[I]** inferred (chain stated), **[U]** unknown. Static analysis only;
nothing here was run.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| Q1: `+0xc/+0x10/+0x14` | A **request mailbox** from each MCPU to the encoder hardware. `+0xc` and `+0x10` are arguments; the MCPU zeroes them or loads values before each post. `+0x14` is the command. The MCPU sets one or more of bits 31/30/29 and **busy-waits until the other side clears them**. Bits below 29 are left in place, so a completed post reads back as its low bits (MbInput `0x24`, ReconChroma `0x1`, IntraEst/ME `0`) | C (MCPU side) |
| Who clears the bits | **Not the ASC firmware.** It stores the three addresses per core but never loads them. **Not another MCPU**: each core touches only its own blocks. **Not the kext**, which has no MCPU references. So it is the encoder hardware | C (scans) / I (hardware) |
| Bit 31 | "Request not yet granted": the MCPU stops until the hardware grants. It is neither an error flag nor a fault marker. `0x126` / `0x12b` are the per-MB **start-of-MB** request codes of ModeDecision and ReconLuma. Both are constants in the code (`modedec:0x1c4`, `reconluma:0x1e4`). The field meanings of the codes are unknown | C / U (codes) |
| Q2: where the two cores are | **Inside their IRQ 27 handler, at the first mailbox wait**, which runs before any per-MB work. `+0xc = +0x10 = 0` and `+0x14` equal exactly what the handler writes just before that poll. ReconLuma does nothing between the post and the poll, so it **is** in the poll loop. ModeDecision does three register/DMem reads in between, so a fault there is possible but unlikely | C / I |
| "Enabled and pending, not serviced" | **Normal while a core is in its handler.** Each handler acknowledges `+8` bit 13 only at its very end (`modedec:0xad0`, `reconluma:0x51c`). The cores took the interrupt; they are waiting for the hardware to grant their request | C |
| Fault / PRIMASK | Neither image has a `cps`/`msr` instruction. HardFault, MemManage, BusFault and UsageFault all go to a `wfi` loop. A fault would look identical on the host interface. Only the MCPU stack (readable DMem) tells poll from fault (§5.2) | C / I |
| What `~MB 34` means | **No specific ring.** 34 counts MBs injected into the encode chain, not ModeDecision's position. The backlog behind a stuck stage fills whatever buffers exist, and MbInput then parks on its 15-MB lookahead (48 < 34 + 15). Per-core MB counters exist in DMem and settle the position (§5.1) | C (counters) / I |
| Q3: outputs | The MCPUs write only their own stage registers. Stage data leaves through hardware write-DMA channels (§3). **One of them is disabled on our session and not on macOS: the colocated-MV writer `0x40D130380`.** The Start_AVC colocated table (wire `0xF6B0`) is zero. `setRefPointers` copies that zero over our per-frame MV address (`PICMGMT+0x8B8`), and `setPipe` then writes the writer's control word as **0** and its size as **`0x40`**, one 64-byte MB record | C (code) / I (runtime match) |
| Q4: neighbour writers | The firmware leaves writer `+0x14/+0x18` at zero (pointers or stride) and they read zero at the hang. Neighbour rows are addressed per row (`mbW<<6`, `mbW<<10`), so **an unchanged `0xA5` fill is expected for a healthy pipe until row 0 finishes**. It carries no extra information here. The source stage and the `+4/+8` words are unknown | C / I / U |
| Q5: reads | ModeDecision / ReconLuma / CAVLC MB counters, both MCPU stacks, stage context registers, and full snapshots (before and at timeout) of every pipe write-DMA block, the colocated writer especially (§5) | — |
| Best next step | One run: publish the colocated table (§6.1) **plus** the §5 reads. The reads decide the next step even if the change does nothing | — |

---

## 1. Q1: the host-interface mailbox `+0xc / +0x10 / +0x14`

### 1.1 Every post in the seven images

`hif` = the core's host-interface base (`0x41xx2000`, `0x41168000`, `0x41188000`).

| core | where | `+0xc` | `+0x10` | `+0x14` written | waits until | VA |
|---|---|---|---|---|---|---|
| ModeDecision | IRQ 27 entry | 0 | 0 | `0x80000126` | bit 31 clear | `modedec:0x1be..0x1d0`, poll `0x238..0x252` |
| ModeDecision | IRQ 27 end (after stage go `0x4126a080 = 1`, `0xa9e..0xaa0`) | — | — | `0xE0000005` | bits 31..29 clear | `0xaa4..0xaaa`, poll `0xab0..0xace`; then `+8 = 0x2000` `0xad0..0xad4` |
| ReconLuma | IRQ 27 entry | 0 | 0 | `0x80000001 + 0x12a = 0x8000012b` | bit 31 clear | `reconluma:0x1c2..0x1e8`, poll `0x1ec..0x20e` |
| ReconLuma | IRQ 27 end (after go `0x4128a080 = 1`, `0x4e0..0x4e2`) | — | — | `0xE000000A` | bits 31..29 clear | `0x4e6..0x4ec`, poll `0x4f4..0x51a`; `+8 = 0x2000` `0x51c..0x520` |
| ReconChroma | IRQ 27, mid-handler | 0 | 0 | `0x80000135` | bit 31 clear | `reconchroma:0x634..0x64e`, poll `0x650..0x66c` |
| ReconChroma | IRQ 27 end | — | — | `0xE0000001` | bits 31..29 clear | `0x904..0x918`, poll `0x91c..0x93a`; `+8 = 0x2000` `0x940` |
| IntraEst | IRQ 27 entry | 0 | 0 | `0x80000025` | bit 31 clear | `intraest:0x1be..0x1dc`, poll `0x1e0..0x1fa` |
| IntraEst | IRQ 27 end, before go `0x4124a080 = 1` (`0x33c..0x346`) | — | — | `0x60000000` | bits 30..29 clear | `0x316..0x33a`; `+8 = 0x2000` `0x34a..0x34e` |
| MotionEst #1 | IRQ 29 | 0 | 0 | `0x80000024` | (poll later) bit 31 clear | `me1:0x366..0x37e`, poll `0x568..0x582` |
| MotionEst #1 | IRQ 29 end | — | — | `0x60000000` | bits 30..29 clear | `me1:0x584..0x5aa`; `+8 = 0x8000` `0x5d4..0x5d8` |
| MbInput | IRQ 30 (consumer) | 0 | 0 | `0xC0000024 \| …` | **no wait** | `mbinput:0x172a..0x1740` |
| MbInput | IRQ 32 | 0 | 0 | `0xE00B0004`, then `0xE0060004 \| [0x41170c10]<<28` | bit 31 clear (before and after each) | `mbinput:0x17a0..0x1834` |
| CAVLC | IRQ 27, conditional | `0x26` | 0 | `0x8004010F` | bit 31 clear | `cavlc:0x116c..0x11ce` |
| CAVLC | IRQ 27, conditional | `0x36` | var | `0xC0080004` | bit 31 clear | `cavlc:0xbbc..0xc08` |
| CAVLC | IRQ 27 end | (`0x18` / 0) | (0) | `0x2…` / `0xA004…` `\| 0x10000000` | bits 31..29 clear | `cavlc:0x11d0..0x1266`; `+8 = 0x2000` `0x1268..0x126c` |

**[C]** for every row: the values are movw/movt immediates and the addresses
are `hif+0x14` loaded into the named register. The poll loops are unrolled
copies of either `ldr; cmp #0; bpl exit` (bit 31) or `cmp r0, r1 lsr #29`
(bits 31..29).

### 1.2 What the register is

- **`+0x14` layout.** Bits 31..29 are handshake flags. The MCPU sets them and
  waits for them to clear. Of the other bits, CAVLC and MbInput put a field in
  23..16 (`0x04`, `0x08`, `0x0B`, `0x06`). Every begin-of-MB code for the stages
  from ModeDecision onward has bit 8 set (`0x126`, `0x12b`, `0x135`, `0x10f`);
  the earlier stages' codes do not (`0x24`, `0x25`). **[C]** for the bit
  patterns. What the codes select is **[U]**.
- **`+0xc`, `+0x10`** are arguments. The MCPU writes them just before a post:
  zero for most, `0x26`/`0x36`/`0x18` for CAVLC. **[C]** Some writer other
  than the MCPU also sets them. ReconChroma's only writes to its `+0xc/+0x10`
  are zeros (`reconchroma:0x63e`, `0x642`; no other store through
  `hif+0xc/+0x10` in the image), yet F11 reads `0x35 / 0x35`. That is the low
  byte of its `0x135` request, echoed back on grant. **[I]** The echo is not
  universal: IntraEst and ME read `0/0` after their `0x25`/`0x24` requests.
  Their end post may reset the arguments; **[U]**.
- **Who clears the flags.**
  - The ASC firmware builds `CAvePipeMcpu` with `[324] = hif+0xc`,
    `[328] = hif+0x10`, `[332] = hif+0x14` and `[336] = ASC irq source`, from
    literal pools `0xdad70` (MbInput) … `0xdabe0` (ReconChroma) into `q0`
    (ctor `0x91374`, stored via `x8 = this+0x104` at `0x9190c..0x919c4`).
    A scan for loads of fields `[272..336]` in the MCPU class
    (`0x912d0..0x92740`), the controller (`0x39280..0x39c00`),
    `ConfigureMCPUs`, `CollectDataFromCpus` and the multi-pass collectors
    finds only `[280]` (`hif+8`, ISR manager, `0x919e8`), `[272]/[276]`
    (`StartUnit` zeroing `+0/+4`, `0x91cc8..0x91d38`) and `[336]`. Nothing
    reads `[324]/[328]/[332]`. An immediate scan for `mov #0x200c/0x2010/0x2014/
    0x800c/0x8010/0x8014` finds eight hits, and none of them pairs with a stage
    base `movk #0x116/0x118/0x124/0x126/0x128/0x12a/0x12c` (they are `0x11e8010`,
    `0x1508010`, `0x44072014`, …). **[C]**, positive control: the same
    field scan finds the `[280]` and `[272]` users. Access through a computed
    base elsewhere is not excluded (Trap 3). **[I]**
  - The ASC's MCPU ISR (`ServiceRoutine` `0x921ac`) reads `hif+8`, acks bits
    8 and 12, and for bit 8 does a read-for-side-effect of `hif+0x20`
    (`ldr wzr,[x9,w8]` `0x92268`). It never touches `+0x14`. **[C]**
  - Every register constant in the nine MCPU images lies in the core's own
    blocks ([59](59-row1-stall.md) §2.1). **[C]**
  - The kext has no MCPU reference ([58](58-pipe-start.md) §0). **[C]**
  - So the flags are cleared by **encoder hardware**. **[I]**, strong.
- **Reading the F11 values:**

  | core | `+0xc/+0x10/+0x14` | reading |
  |---|---|---|
  | ModeDecision | `0 / 0 / 0x80000126` | first request of the IRQ 27 handler posted, **not granted** |
  | ReconLuma | `0 / 0 / 0x8000012b` | same |
  | ReconChroma | `0x35 / 0x35 / 0x1` | last handler's end post `0xE0000001` completed. The `0x80000135` grant left `0x35` in the arguments. **ReconChroma has completed at least one MB** [I] |
  | IntraEst, MotionEst | `0 / 0 / 0` | consistent with a completed `0x60000000` end post, or with never having run. Not discriminating |
  | MbInput | `0 / 0 / 0x24` | a `0xC0000024` post was completed (bits 31/30 cleared) |
  | CAVLC | `0 / 0 / 0` | not discriminating |

**Answer to Q1:** bit 31 in `+0x14` means **"request posted, waiting for the
hardware grant"**. It is neither an error nor a fault marker, and not
specifically "blocked on output". Whether the grant waits for input data,
output room or a downstream stage is decided inside the hardware and is not
visible in the MCPU or ASC code. **[C]** for the handshake; **[U]** for the grant
condition.

---

## 2. Q2: why ModeDecision and ReconLuma stay in their handlers

### 2.1 The handlers up to the stuck point

**ModeDecision IRQ 27, `modedec:0x1ac`** (vector word 43 = `0x1ad`):

```
1ac  push {r4-r7,lr}; stmdb {r8-r11}; sub sp,#20
1b6  r5 = 0x41262008                     ; hif+8
1c0  str 0,[r5,#4]  ; +0xc = 0
1c2  str 0,[r5,#8]  ; +0x10 = 0
1c4  r0 = 0x80000126 ; 1d0 str r0,[r5,#12]   ; +0x14 = request
1e6  ldrh sl,[0x41263000]                ; stage MB context
1ea  DMem[0x10000a1c] += 1  (1ea..1f4)   ; per-MB counter (bss, zeroed at reset 0x14c..0x162)
1ec  ldr fp,[0x41263228]
1f6  bcc 230  (counter < 3) / else maybe read 0x4126b000, 0x4126b02c, DMem[0x10000004+]
238  poll +0x14 until bit 31 clear       ; <- F11
```

**ReconLuma IRQ 27, `reconluma:0x1a8`** (vector 43 = `0x1a9`):

```
1c6  ldrh r2,[0x41283000]; 1d4 ldr ip,[0x41283228]
1dc  +0xc = 0; 1e0 +0x10 = 0; 1e4..1e8 +0x14 = 0x8000012b
1ec  poll +0x14 until bit 31 clear       ; <- F11
210  DMem[0x10000280] += 1  (210..21e)   ; per-MB counter (bss 0x1000027c..0x1000028c, reset 0x14c..0x162)
```

**[C]** for both listings. ReconLuma has nothing between the post (`0x1e8`) and
the poll (`0x1ec`), so with `+0x14 = 0x8000012b` and `+8` bit 13 still set it
**is in that poll loop**. The only alternative would be a bus fault on reading
its own `+0x14`. **[C]** code path / **[I]** conclusion. ModeDecision performs
three register reads and a DMem update between post and poll. A fault on one
of them would leave the same host-interface picture. That is **[I] low**: the
reads hit the stage's own context block, the same kind IntraEst reads without
trouble, and §5.2 settles it.

### 2.2 "Enabled and pending" is the in-handler state

Each handler acks `+8` bit 13 as its **last** action (`modedec:0xad0`,
`reconluma:0x51c`, `intraest:0x34a`, `reconchroma:0x940`, `cavlc:0x1268`).
**[C]** A core parked anywhere in its handler therefore shows `+0` bit 13
enabled and `+8` bit 13 pending. The cores did take IRQ 27 and are waiting for
the grant. Neither image contains `cpsid`/`msr PRIMASK/BASEPRI`
(`grep cpsi|PRIMASK|BASEPRI` over the disassembly: 0 hits). Their fault
vectors 3-6 point at `wfi` loops `0x17d`/`0x181`
(`modedec`/`reconluma` vector words 3..6). **[C]** A faulted core would sit in
Handler mode in `wfi` with the same host-interface picture. **[I]**

### 2.3 Which one is the root

`+8` bit 13 is pending on MbInput, MotionEst, ModeDecision and ReconLuma. It is
not pending on IntraEst, ReconChroma or CAVLC. MbInput and MotionEst have no
bit-13 handler and do not enable it. **[C]** (F11 words, vector tables:
MbInput IRQs 1/28/29/30/32/33, ME1 IRQs 14/29). Bit 13 therefore looks like a
generic "this stage holds an MB" status. **[I]**

Two orderings fit:

- **ReconLuma is the root.** It cannot start its MB. ModeDecision cannot start
  the next one because ReconLuma has not taken the previous output.
  ReconChroma finished its copy of that MB and idles. CAVLC starves.
- **ModeDecision is the root.** Its start request waits for an output channel
  (§3.3). ReconLuma's grant waits for data ModeDecision emits at its next
  start. The dependency cannot be read statically.

The per-core counters separate these two (§5.1). **[I]**

### 2.4 What "~MB 34" is

MbInput consumed 34 and produced 48. Its consumer parks while
`produced < consumed + 15` ([59](59-row1-stall.md) §1.3), and `48 < 49`. The
head (source analysis) has not produced a 49th record. **[C]** arithmetic. So
34 is the number of MBs **injected**, bounded by how much the upstream stages
buffer before back-pressure reaches the head. It is not ModeDecision's MB
index. **[I]** No 32-entry FIFO or 2-row tile was found that would stop at 34.
The recon writer's tile row is `40960` B = 40 tiles
(`0x552c0..0x552e4`, width from `ctrl+0xA8C`), one 32-px row covering two MB
rows, which gives nothing at MB 34. **[C]** The colocated writer, as programmed
on our session, has room for **one** 64-byte MB record (§3.3). That would stop
ModeDecision after one or two MBs and leave a ~30-MB backlog upstream. **[I]**

IntraEst's `0x40D243180 = 0x00020001` does not decode as a position consistent
with 34. ModeDecision uses bits 16..17 of its `0x41263180` as a 2-bit index
(`modedec:0x2c2`, `0x2f6..0x2fa`), not as a row. **[U]** Do not read it as
"(x=1, y=2)".

---

## 3. Q3: what ModeDecision and ReconLuma output, and to where

### 3.1 Not through the MCPU

Both images write only their stage blocks. ModeDecision writes `0x4126xxxx`
(context `0x41263000..`, parameters `0x4126a0a4..0x4126a104`, go `0x4126a080`,
`0x4126a084`, `0x4126b000`). ReconLuma writes `0x4128xxxx` (context
`0x41283000..`, go `0x4128a080`, parameters `0x4128a088..`, `0x4128a294..2b4`).
Their DMem holds only the per-MB counters, a few saved words and the ASC
parameter block. **[C]** ([59](59-row1-stall.md) §2.1 scan plus these handlers.)
Pixel, MV and coefficient data flow in hardware to the pipe write-DMA channels
programmed by `setPipe`.

### 3.2 The pipe write-DMA channels (AVC `setPipe` `0x52f38`, single-core arm)

Channel layout as for the neighbour writers: `+0` control, `+0xc` address,
`+0x10` size, `+0x14/+0x18` pointers or stride ([59](59-row1-stall.md) §2.2).
`x23 = 0x1130700`.

| AP block | purpose (firmware string / assert) | programmed from | our session at F11 | VA |
|---|---|---|---|---|
| `0x40D130240` | recon luma (tile data `+0xc`, metadata `+0x1c`, row `+0x10 = 40960`, `+0x14 = 0x700D1\|flag<<2`) | Start_AVC DPB recon table, NEED_LSB_PLANES | **programmed** `0x800314b1` (F11) | `0x552b4..0x55400` |
| `0x40D130300` | recon chroma (`+0x1c`, size, …) | same, `this[2692]` | programmed (`+0x31c = 0xfe810000`) | `0x58010..0x58050` |
| **`0x40D130380`** | **colocated MV store** (`encoder_addr_dst_colo`, asserts 6889/6890) | `ctrl+5080` ← `PICMGMT+0x8B8` ← `setRefPointers` ← Start_AVC colocated table wire `0xF6B0` | **control 0, size `0x40`, address not written** (§3.3) | `0x554e0..0x555bc` |
| `0x40D1303C0`, `…400`, `…440`, `…480` | entropy ×4 (`encoder_addr_entropy[%d]`) | `ctrl+0xEC0+32k` ← `PICMGMT+0xA00+32k` (`PipePrepareParam` `0x48800..`); size `ctrl+0x10C0+16k` | programmed `0x80030001`, 960 KiB buffers; size source **[U]** | `0x55908..0x55d70` |
| `0x40D130600`, `…640` | neighbour info / pixel writers | `PICMGMT+0x980/+0x9A0` | programmed (F11) | `0x5583c..0x5588c` |
| `0x40D130700` | MB stats / MB address FW data | `ctrl+0x1bb0/0x1bb8`, gate `ctrl+0x23FCF` (wire `0xFF76`) | gate off → `0x80030000`, size `0x40` | `0x54fdc..0x54ff4`, `0x55154..0x551a4` |
| `0x40D130780` | multi-core split context | `ctrl+7896/7880` | single core → `0x80030000` | `0x5590c..0x55948` |

**[C]** for addresses, values and VAs, except where marked. Which stage feeds
which channel is not in any code read. Assigning colocated MVs and neighbour
Info (both 64 B/MB) to ModeDecision, and recon and neighbour pixels to the
Recon stages, is **[I]** from what those products are.

### 3.3 The colocated writer is disabled on our session

1. Per frame, `H264VideoEncoderDPB::ManageDPBBuffer` calls
   `CAVECommonDPB::setRefPointers` (`bl` at `0x2d6e8`). That function looks up
   the recon luma address `[x2,#72]` in the DPB table `[dpb+4704]` (16-byte
   entries, `0x2c35c..0x2c4a8` / `0x2c618..0x2c774`) and, on a match, writes
   `table[512 + 8k]` to **`PICMGMT+0x8B8`** (`str x17,[x1,#2232]` `0x2c4b0`,
   `0x2c5f8`; `str x11,[x1,#2232]` `0x2c784`). **[C]**
2. `table[512 + 8k]` is filled at Start_AVC from the colocated table, wire
   `0xF6B0` (`ldr x21,[x21,x3]` with `x3 = 0xf650`, `str x21,[x20,#512]`
   `0x2b78c..0x2b790`). **[C]** ([53](53-first-frame.md) §13.2.) The driver
   leaves that table zero ([53](53-first-frame.md) §13 item 6). **[C]**
3. F11's recon writer shows DPB slot 0's address (`+0x24c = 0xfe820000`), not
   the per-frame `0xfd800000` the driver sent. So `setRefPointers` did run
   and matched slot 0. **[C]** (kmsg l.1006 vs l.1063). By the same call,
   `PICMGMT+0x8B8` was overwritten with `0`, and the driver's per-frame
   `MV 0xfdaa3000` was lost. **[I]**, strong.
4. `ProcessPipeStart` runs **after** that: `PipePrepareParam` (`bl 0x480b4` at
   `0x52e28`) copies `PICMGMT+0x8B8` to `ctrl+5080` (`0x48668..0x48674`), then
   `setPipe` (`0x52e40`). **[C]**
5. `setPipe`: `ldr x10,[x19,#5080]; cbz x10, 0x5554c` (`0x554ec..0x554f0`) →
   `w21 = 0x40` (`0x5554c`) → `0x1130390 = 0x40`, **`0x1130380 = w10 = 0`**
   (`0x555b0..0x555bc`). The non-zero arm instead writes `0x113038C = dst_colo
   + coloDataContextOffset`, `0x1130390 = bfrSizeColoData`, and
   `0x1130380 = 0x80030001` (`0x5559c..0x555bc`). **[C]**
6. Sizes. `coloDataContextOffset = ctrl[5176]·mbW·64` (`0x54ee0..0x54eec`);
   `ctrl[5176]` is the chunk's start row, 0 on a fresh frame (written 0 at
   `0x59d30`, `0x5ba64`). `bfrSizeColoData = pic_height_in_mbs·mbW·64`
   (`[sp,#124]` from `0x53038..0x53044`, `lsl #6` `0x54efc`) = **230 400 B**
   at 80x45. **[C]** formula / **[I]** that `ctrl+0xA94/0xA98` are
   height/width in MBs (docs/59 §2.3). The kext sizes the Colocated surface at
   `128·MBs` = 460 800 B, twice this (docs/47 l.300, docs/16). **[C]**
7. **macOS fills this table.** The kext loop at `0xfffffe0008eaef74..0xeaefb8`
   writes an IOVA into `block+0xF650[set][slot]` for every non-null Colocated
   surface. **[C]** That Colocated surfaces exist for an AVC encode session is
   **[I]** from docs/16 §surface set.

**Consequence.** With our zero table, a writer that macOS always has enabled is
left with control `0` and room for one 64-byte record, while the hardware
produces a colocated record per MB. **[I]** No assert protects it: the asserts
sit on the non-zero arm, which is why docs/54 filed it as "safely zero". Whether
the hardware back-pressures or drops on a disabled writer is **[U]**. The
firmware writes control `0` for disabled neighbour writers on the multi-core
arm too (`0x5589c..0x558ac`), so "disabled" may be a supported state. That
keeps this at *medium*, not high.

### 3.4 The other buffers checked

| buffer | could it fill after N MBs? | Conf |
|---|---|---|
| LowResResults (ME SFS readers) | readers, inert in sync LRME ([59](59-row1-stall.md) §4); an I-frame does no ME search | C / I |
| MBStats / MB address / stats | writer deliberately set to `0x80030000` by the firmware's gate-off path, a designed mode | C |
| neighbour Info/Pixel | sized exactly one row (5120 / 81920 B), programmed; flush per row | C / I |
| recon luma/chroma | programmed; tile row 40960 B; our slot is large enough (no DART fault) | C |
| entropy ×4 | programmed; CAVLC (the likely writer) is idle, so it is not the blocker | C / I |
| **colocated** | **disabled, size `0x40`** | C / I |

---

## 4. Q4: the neighbour writers

- The firmware writes writer `+0x20 = 0` (`0x55708`, `0x5572c`) and, via
  `ResetDMANeighborRegs` (`0x27c88`), `+0x14 = +0x18 = 0`. In resume mode
  `+0x14` is one row and `+0x18` a row-multiple ([59](59-row1-stall.md) §2.2).
  **[C]** F11 reads writer `+0x14 = +0x18 = 0` and reader `+0x14 = +0x18 = 0`,
  so the hardware advanced neither. **[C]** (kmsg l.1136, l.1148). The pointers
  are row-granular **[I]**, so with nothing yet in row 0 finished, **an
  unchanged `0xA5` fill is what a working pipe would also show at MB 34**.
  The fill result adds no evidence; repeat it only after row 0 has passed.
- Neither the reader's `+0x20 = 2`, `+0x24 = 0xffffffff` nor any writer
  `+4/+8` (`0x01400008/0x03f80008` Info, `0x01480010/0x04000008` Pixel) has
  a firmware writer or reader in the code scanned. **[U]** The high halves
  differ by 8 or 16 between the Info and Pixel instances, which looks more like
  a per-instance constant than a fill level. **[I] weak** A snapshot before
  Process settles it (§5.4).
- Which stage emits neighbour data: **[U]**, no MCPU code touches the channels.
  Info records are 64 B/MB, the same size as the colocated record, which
  suggests ModeDecision. Pixel records are 1024 B/MB, which suggests
  ReconLuma/ReconChroma. **[I] weak**

---

## 5. Q5: read-only reads for the next run

All targets are inside mapped bank 0 (`0x40D100000..0x40D55C000`).
**Do not read any host-interface `+0x20`**: the ASC reads it for a side
effect (`0x92268`). Read `+8` registers and the stage-context blocks last.
Whether they have read side effects is **[U]**.

### 5.1 Per-core MB counters (decide root and position)

| AP | MCPU | meaning | VA |
|---|---|---|---|
| `0x40D468A1C` | ModeDecision DMem `0x10000a1c` | IRQ 27 entries, **including** the stuck one (incremented before the poll) | `modedec:0x1ea..0x1f4` |
| `0x40D488280` | ReconLuma DMem `0x10000280` | IRQ 27 entries that **passed** the first grant | `reconluma:0x210..0x21e` |
| `0x40D4C87A4` | CAVLC DMem `0x100007a4` | IRQ 27 entries | `cavlc:0x5c8..0x5dc` |

**[C]** All three are in the images' `.bss` (zeroed by reset: ModeDecision
`0x100009b0..0x10000a34`, ReconLuma `0x1000027c..0x1000028c`, CAVLC
`0x1000078c..0x100007c0`). The ASC parameter copy into ModeDecision DMem ends
below `0x9b0` (`0x61720..0x61768`); that it never reaches the counters is
**[I]**.

Interpretation:
- **MD = 1 (or 2), RL = 0 or 1, CAVLC ≤ RL**: stalls at the very first MB or
  two. That fits the one-record colocated writer (§6.1) or a missing enable.
- **MD ≈ RL + 1, both ≫ 2**: a resource that fills after N MBs. Divide the ring
  size by the per-MB record size to identify it.
- **MD ≫ RL + 1**: ReconLuma is the root; ModeDecision is buffered behind it.

### 5.2 MCPU stacks (poll or fault)

Read `0x40D468F60..0x40D468FFF` (ModeDecision) and `0x40D488F60..0x40D488FFF`
(ReconLuma).

Expected in the poll loop:

- **Idle frame.** Reset pushes 4 words (`push {r4,r6,r7,lr}` at `0x124`) and
  `main` is a leaf, so SP is `0x10000FF0` at `wfi`. The IRQ frame is at DMem
  `0xFD0..0xFEF`: `r0, r1, r2, r3, r12, lr = 0x175, pc, xPSR`. The stacked
  `pc` is **`0x1aa`** for ModeDecision (`wfi` at `0x1a8`) and **`0x1a6`** for
  ReconLuma (`wfi` at `0x1a4`), and the stacked `xPSR` has IPSR = 0. **[C]**
  for addresses; the basic frame layout (no FPU) is **[I]**.
- **Handler saves.** `EXC_RETURN 0xFFFFFFF9` at `0xFCC`, `r4..r7` at
  `0xFBC..0xFC8`, `r8..r11` at `0xFAC..0xFB8`. SP is `0x10000F98` for
  ModeDecision (`sub sp,#20`) and `0x10000FA8` for ReconLuma (`sub sp,#4`).
  **[C]**

A fault leaves a **second** 8-word frame directly below that SP: ModeDecision
`0xF78..0xF97`, ReconLuma `0xF88..0xFA7`. Its `xPSR & 0x1ff = 0x2b`
(exception 43 = IRQ 27), and its `pc` lies inside the handler (ModeDecision
`0x1ac..0xadc`, ReconLuma `0x1a8..0x52a`). Stale words from earlier `memcpy`
calls (`push {r0,r4-r9,lr}` at `0xc64`/`0x58c`) can sit there too, so check
`xPSR`, not just "non-zero". **[I]**

### 5.3 Stage context and go registers

`0x40D263000, 0x40D263180, 0x40D263184, 0x40D263228, 0x40D26A080, 0x40D26A084,
0x40D26B000, 0x40D26B02C` (ModeDecision); `0x40D283000, 0x40D283180,
0x40D283184, 0x40D283228, 0x40D28A080` (ReconLuma); `0x40D243000, 0x40D243184,
0x40D24A080` (IntraEst); `0x40D2A3184` (ReconChroma); `0x40D2D0184` (CAVLC).
Every address is one the handlers read (§2.1; `intraest:0x1bc..0x1fc`,
`reconchroma:0x36e..0x388`, `cavlc:0x562`). **[C]** Their meaning beyond
"the stage's per-MB context / go" is **[U]**; they are for comparison across
stages and runs.

### 5.4 Write-DMA snapshots (after Start_AVC, before Process, and at the timeout)

Read the whole `0x40` window of each block:
`0x40D130240` (recon luma), `0x40D130300` (recon chroma), **`0x40D130380`
(colocated)**, `0x40D1303C0`, `0x40D130400`, `0x40D130440`, `0x40D130480`
(entropy), `0x40D130600`, `0x40D130640` (neighbour), `0x40D130700` (stats),
`0x40D130780` (split); also reader `0x40D120BC0` (source colocated).

Expected with the current driver: `0x40D130380 = 0`, `0x40D130390 = 0x40`.
**[C]** A word that differs between the two snapshots is hardware state; a
word that is the same both times is configuration or a constant.

---

## 6. Ranked causes, each with a driver change and a confirming observation

Proposals for the operator (AGENTS.md). Run all of §5 on every attempt.
**Fixed** means: MbInput consumed reaches 3600, ModeDecision/ReconLuma `+0x14`
bit 31 clear, `0x40D110140` bit 2 set, `StartCount 1-1-1-1`, completion `0x0E06`.

### 6.1 Colocated-MV writer disabled: Start_AVC colocated table left zero (medium)

**Why.** §3.3: it is the only per-frame write-DMA channel of a
ModeDecision-type product that macOS enables and we leave disabled. As
programmed it holds exactly one MB record, and the stall sits in ModeDecision's
start handshake.

**Change.** In Start_AVC, for set 0 and each DPB slot used (`k = 0 ..
max_num_ref_frames`, i.e. 2 slots), allocate a colocated buffer in the session
arena and write its IOVA to **wire `0xF6B0 + 8·k`**, u64, 64-byte aligned
(`& 63 == 0`, assert 6890). Size: the kext's **`128·mbW·mbH` = 460 800 B** at
1280x720, rounded up to 64. This is twice the `230 400 B` the firmware
programs, so the offset (0 on frame 1) has headroom. Optionally fill it with
`0x5A` so writes are visible. The per-frame `recon_mv` at PICMGMT `+0x8B8` can
stay; `setRefPointers` overwrites it (§3.3 step 3).

**Apparatus check.** At the timeout `0x40D130380 = 0x80030001`,
`0x40D13038C` = slot-0 colocated IOVA (low 32 bits) and
`0x40D130390 = 0x38400`. If these are unchanged, the table did not land. Check
the wire offset against `AVE_VIDEO_PARAMS` block `+0xF650` = wire `0xF6B0`
(docs/53 §13.2).

**Confirm.** Colocated fill bytes overwritten, the §5.1 counters moving past the
F11 values, and then Fixed. **Refute.** The writer is programmed, the bytes
change, and the stall persists: drop to 6.2/6.3, guided by the counters.

### 6.2 ReconLuma's grant is blocked by a hardware resource we do not see (medium-low, no change yet)

**Why.** §2.3: ReconLuma is the most downstream stuck stage, and ReconChroma
(its sibling) completed an MB. Its inputs and outputs besides colocated are
programmed. The block would sit in the hardware's grant logic: an
input-from-ModeDecision FIFO, the recon tile writer, or the neighbour pixel
writer.

**Change.** None until §5.1/§5.4 point somewhere. If MD ≫ RL, or the recon
writer's pointer words move between snapshots while ReconLuma sits at its first
grant, the recon/neighbour-pixel writers are next. One sub-candidate to test
cheaply: neighbour **Pixel** size is exactly the minimum
(`max(mbW<<10, 0x4000)` = 81 920 B = our 80 KiB slot, docs/59 §2.3). Keep the
wire size but give the slot a 1-row guard (allocate 2×). Nothing else changes.
**Confirm.** RL counter rises. **Refute.** Identical counters.

### 6.3 ModeDecision faulted between post and poll (low)

**Why.** §2.1: three reads after the post. On Apple silicon an unpowered
sub-block can fault or hang. DPE leaves one CAC bit explicitly 0
(`0x40D1DC4A4`, docs/58 §5.1), whose role is unknown.

**Change.** None; §5.2 decides. A second exception frame with IPSR `0x2b` and
`pc` in `0x1e6..0x236` would promote this and name the faulting read.
**Refute.** A single frame with stacked `pc = 0x1aa`.

### 6.4 MB stats / MB-address writer expected by ModeDecision (low)

**Why.** Same shape as 6.1 (size `0x40`), but the firmware writes `0x80030000`
on a designed gate-off path. macOS's value of wire `0xFF76` (the gate) is
unknown (docs/54 §8).

**Change.** Only if 6.1 is refuted and §5.1 shows MD stuck at 1-2: find the
kext writer of wire `0xFF76` statically before sending a non-zero byte.

### 6.5 Neighbour write/read loop (very low for this stall)

§4: F11's neighbour evidence is expected from a healthy pipe at MB 34, so the
row-1 mechanism of docs/59 §6.2 does not apply. Keep the fill diagnostic and
re-evaluate once `consumed ≥ 80`.

### Suggested next run

One boot, F11's parameters plus: §6.1 (colocated table) and the §5.1-§5.4
reads, snapshots before Process and at the timeout. The counters and stacks are
informative whatever the colocated change does.

---

## 7. Corrections and notes for other documents (not edited here)

1. **docs/53 §22 "Reading"**: "an enabled pending event that is not serviced
   means those cores are not taking their per-MB interrupt". They did take it.
   Bit 13 stays pending until the handler's final ack, and both cores are inside
   the handler waiting for the hardware grant (§2.2). **[C]**
2. **docs/54 §5 and docs/53 §13 item 6** ("colocated table … safely zero … not a
   first-frame risk"): zero is assert-safe, but it leaves the colocated writer
   `0x40D130380` disabled with size `0x40` on every frame (§3.3). "Safe" holds
   only for asserts. **[C]**/**[I]**
3. **docs/54 §2** row `ctrl+0x23FD4` and "the colocated block in `setPipe` is
   entered because of it": the `cmp w8,#1` at `0x54f14` compares
   `[[ctrl+8·ctx+0x2E370]+16]`, which is loaded from `[sp,#160]` (`x21` at
   `0x52fc0`, `0x5324c`) and not from `ctrl+0x23FC4`. The same field is compared
   with `2` at `0x57c1c` and `0x57f0c`. It looks like `slice_type` (1 = B), not
   the width-derived flag. The dst-colocated writer is gated only on its
   address (`0x554f0`). **[C]** for the loads; the slice-type reading is
   **[I]**.
4. **docs/59 note from review** ("`encoder_addr_entropy` … its consumer is the
   Transcode stage"): `setPipe` also programs four **pipe** write-DMA channels
   from `encoder_addr_entropy[k][transcode_buffer_id]` (`0x11303C0 + 0x40k`,
   `0x55908..0x55d70`, log `encoder_addr_entropy[%d]` `0xc694f`). **[C]**
5. **docs/59 §1.1/§5 Tier 2** ("`+0x14` bit 31 set is a stuck mailbox"): refine
   to "request posted, hardware has not granted". It is the MCPU's normal
   per-MB wait, not a fault (§1). **[C]**

---

## 8. Reproduce

```sh
S=/tmp/mcpu; mkdir -p $S
python3 - <<'EOF'
d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read()
for n,va,sz in [('mbinput',0xdadc0,0x18ec),('intraest',0xe3350,0x446),('modedec',0xe37a0,0xcde),
                ('reconluma',0xe4480,0x606),('reconchroma',0xe4a90,0xbb8),('cavlc',0xe5650,0x1500),
                ('me1',0xe0dd0,0x6f0)]:
    open(f'/tmp/mcpu/{n}.bin','wb').write(d[va+0x4000:va+0x4000+sz])
EOF
for n in modedec reconluma reconchroma intraest cavlc me1 mbinput; do
  objdump -D -b binary -m arm -M force-thumb $S/$n.bin > $S/$n.S; done
sed -n '/^ *1ac:/,/^ *260:/p;/^ *a9e:/,/^ *adc:/p' $S/modedec.S     # MD request, counter, poll, end post
sed -n '/^ *1a8:/,/^ *222:/p;/^ *4e0:/,/^ *52a:/p' $S/reconluma.S   # RL request, poll, counter, end post
sed -n '/^ *634:/,/^ *66e:/p;/^ *8fe:/,/^ *942:/p' $S/reconchroma.S # RC request (+0xc/+0x10 = 0), end post
sed -n '/^ *548:/,/^ *5e0:/p' $S/cavlc.S                            # CAVLC IRQ 27 counter
grep -c 'cpsi\|PRIMASK\|BASEPRI' $S/modedec.S $S/reconluma.S       # 0
# ASC side
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x91374 -n 0x60    # hif +0xc/+0x10/+0x14 into q0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x921ac -n 0xc0    # MCPU ISR (acks 8/12, reads +0x20)
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2c35c -n 0x160   # setRefPointers colocated lookup
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2c618 -n 0x170
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2b770 -n 0x28    # Start table 0xF650 -> DPB +512
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x52e10 -n 0x34    # PipePrepareParam then setPipe
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x48668 -n 0x10    # ctrl+5080 <- PICMGMT+0x8B8
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54ee0 -n 0x40    # coloDataContextOffset / bfrSize
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x554dc -n 0xe4    # dst colocated writer, zero arm
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x558b4 -n 0x4c0   # split + entropy writers
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eaef5c -n 0x60  # kext fills 0xF650 table
```
