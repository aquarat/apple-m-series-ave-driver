# Where the fetched source pixels go, and what configures that destination

Static analysis of the 13.5 kext and firmware, cross-read against the register
dumps in `results/f21-1789893453.kmsg`. **Nothing here was run on hardware**;
every number attributed to hardware is quoted from an existing log.

Conventions as [62](62-kext-field-map.md): firmware VAs are 13.5 image VAs
(file offset = VA + `0x4000`); **wire** = byte offset in the command on IPC
channel 1; VP + `0x60` = wire. Labels per [00](00-methodology.md): **[C]** read
from an instruction (VA cited), **[I]** inferred with the chain stated, **[U]**
unknown.

Reproduce anything below with

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x549bc -n 0x80
```

**The `AVE_MACOS=13.5` is not optional.**

The address map used throughout: the firmware's MMIO base pointer is
`*(0x21a7b8)`, and firmware offset `X` is AP physical `0x40C000000 + X`
(`AVE_BANK_FABRIC` in `driver/ave_hw.h:37`). So fw `0x1120000` = AP
`0x40D120000` and fw `0x1110128` = AP `0x40D110128`. **[C]** — the mapping is
forced by three independent matches: fw `0x1120010` (`0x54320`) is the register
the driver reads at `0x40D120010` and finds its own luma IOVA; fw `0x1170110`
= 15 (`0x61234`) is the MbInput lookahead the MCPU reads at `0x41170110`
([59](59-row1-stall.md) §1.3); fw `0x1110128` is the driver's `SRCDMAGO
0x40D110128`.

---

## 0. The short version

1. **There is no destination.** The source channel is a *read* DMA that streams
   into the pipe. Nothing in the firmware programs a DRAM staging surface, an
   SRAM base or a tile buffer for it, and the two registers that looked like
   they might (`+0x04`, `+0x08`) are **read-only hardware descriptors**: they
   hold the same values before the session is configured and after a whole
   frame, and **no instruction anywhere in the firmware image reads or writes
   them**. §1, §2.
2. **`+0x18` = `0x00002005` is correct and expected.** It is what
   `ProcessPipeReset` writes (fw `0x4ee28`). `0x00072065` is the power-on value
   — `setPipe` writes that constant only on the *compressed*-input arm
   (`0x54944`), which we do not take. Seeing `0x2005` after a frame is positive
   evidence that `ProcessPipeReset` ran. §1.3.
3. **`SRCDMAGO` is asserted, twice, unconditionally.** `setPipe` writes the
   whole word with bit 0 set at fw `0x57dcc`, and `setPipeGo` ORs bit 0 in
   again at fw `0x62044`. Reading 0 afterwards is consistent with a
   self-clearing go and is **not** evidence that the fetch never started. §3.
4. **Two host fields we have never set land in `SRCDMAGO`**: wire `0xFCE9`
   (u8) in bit 3 and wire `0xFECC` (u8) in bits 4+. [62](62-kext-field-map.md)
   §6.5 listed both as "firmware reads it, destination unknown". They are now
   located. Wire `0xFCE9` *also* gates whether `ProcessPipeReset` initialises a
   third source-reader channel at `0x1120100`. §3.2, §3.3.
5. **3845 and `y 47` both say the MB-event grid is 48 rows tall, not 45**:
   `3845 = 80 × 48 + 5`, and `y = 47` is impossible in a 45-row grid. The
   source *reader* is right (`+0x2C` = row 44, column 79 = the last MB of a
   45-row picture); it is the stage downstream of it that walks three extra
   rows. §4.
6. **The residual is zero because intra estimation never ran.** F21's own log:
   `IntraEst curMB 0x00000000` and `hif IntraEst +0x14 0x00000000`, while
   ModeDec, ReconLuma and ReconChroma all carry non-zero work words and all
   three MCPU counters read 3845. An I-frame whose intra estimator never
   produced a mode or a residual is exactly a frame of I_16x16 DC macroblocks
   with no coefficients — uniform 128, independent of the source, and the same
   byte count for any input. §5.

---

## 1. The source-read register file at `0x40D120000` / `+0x80`

`CAVCController::setPipe` (fw `0x52f38`, `0x2200` bytes) holds
`w25 = 0x1120FA4` (fw `0x54208`/`0x5421c`) and addresses the block as
`base + (x25 - K)`. [62](62-kext-field-map.md) §6.2 decoded the host-supplied
words. This section completes the block.

The decode below comes from a whole-image MMIO map: every `str`/`stur` and
every `ldr`/`ldur` in the 236k-instruction `__TEXT` disassembly whose base
resolves to `*(0x21a7b8) + constant`, with the constant tracked through
`mov`/`movk`/`add`/`sub`. The map reproduces every fact in
[62](62-kext-field-map.md) §6.2 independently, which is the control on it.

### 1.1 The complete table

| off | AP | written at | value | source |
|---|---|---|---|---|
| `+0x00` | `0x40D120000` | fw `0x549c0` | `0x80034045` linear / `0x80034047` compressed (`w22 = 0x80034024`, `+0x21`/`+0x23`) | firmware constant; arm from PICMGMT `+0x6F3` |
| `+0x04` | `0x40D120004` | **never** | reads `0x000000c0` | **hardware, read-only** (§1.2) |
| `+0x08` | `0x40D120008` | **never** | reads `0x000000c0` | **hardware, read-only** (§1.2) |
| `+0x0C` | `0x40D12000C` | fw `0x54a08` | `(wire 0xFCE8 << 16) \| (ctrl[0xA88] << 8)`; bits 0..7 always 0 | host u8 + SPS-derived code (20 for 4:2:0 8-bit) |
| `+0x10` | `0x40D120010` | fw `0x54320` | low 32 of PICMGMT `+0x8C0` | **host, per frame** |
| `+0x14` | `0x40D120014` | fw `0x54874` (linear) / `0x54918` (compressed) | PICMGMT `+0x8C8` (or `+0x918`) | **host, per frame** |
| `+0x18` | `0x40D120018` | fw `0x54944` **compressed arm only**; and `0x4ee28` in `ProcessPipeReset` | `0x00072065` / `0x00002005` | firmware (§1.3) |
| `+0x1C` | `0x40D12001C` | fw `0x54998` | 0 on the linear arm | firmware |
| `+0x20` | `0x40D120020` | fw `0x54818` | `(PICMGMT[0x8F8]+u16[0x91E]) \| ((PICMGMT[0x8FC]+u16[0x920])<<16)` — origin | host, correctly 0 ([62](62-kext-field-map.md) §6.3) |
| `+0x24`,`+0x28` | | `0x54888`/`0x54894` linear, `0x54958`/`0x54968` compressed | 0 on the linear arm | |
| `+0x2C` | `0x40D12002C` | — | **the only register the firmware reads**: `ProcessPipeDone` fw `0x596a0`, `(v>>16)` = `currMbRow` | hardware progress |
| `+0x50` | `0x40D120050` | fw `0x54334` | `wire 0xFEC0 & 3` | host |
| `+0x54`,`+0x58` | | never | read `0x00000002`, `0xffffffff` | hardware, read-only |
| `+0x5C` | `0x40D12005C` | fw `0x54230` | `(ctrl[4732]&0xff) \| (ctrl[96·id+8472]<<8) \| (ctrl[8380]<<16)` | firmware |
| `+0x80`..`+0xA8` | | mirror of `+0x00`..`+0x28` at `+0x80`; cfg `0x80034055`/`57` fw `0x549d4` | chroma channel; the whole chroma block is skipped when `chroma_format_idc == 0` (`cbz` fw `0x54340`, `0x549c8`) | |
| `+0xD0` | `0x40D1200D0` | fw `0x547ec` | `wire 0xFEC0 >> 2` | host |

All **[C]**. The two blocks are one hardware template instantiated twice at a
`0x80` stride: luma at `+0x00`, chroma at `+0x80`, each with its own `+0x2C`
progress word (F21 shows `002c004f` in both).

### 1.2 `+0x04` and `+0x08` are read-only hardware descriptors — settled

Two independent proofs.

**Static.** The whole-image MMIO map finds **no writer and no reader** of
`0x1120004`, `0x1120008`, `0x1120084` or `0x1120088` anywhere in the firmware
`__TEXT`. The same map does find every one of the twenty-odd writers listed in
§1.1, including the ones that are only reachable through `sub`-from-a-register
addressing, so it is not blind to the idiom (Trap 3). **[C]**

**Empirical, from `results/f21-1789893453.kmsg`.** The driver dumps the block
twice, once after `Start_AVC` — i.e. before `Process`, with address, stride and
format all still zero — and once at the end of the frame:

```
[after Start_AVC] 40d120000: 80034044 000000c0 000000c0 00000000 00000000 00000000 00072065 ...
[after Start_AVC] 40d120080: 80034054 00c00140 00c000c0 00000000 00000000 00000000 00072065 ...
[timeout]         40d120000: 80034045 000000c0 000000c0 00001400 fd300000 00000500 00002005 ... 002c004f
[timeout]         40d120080: 80034055 00c00140 00c000c0 00001400 fd280000 00000500 00002005 ... 002c004f
```

`+0x04` and `+0x08` are **bit-identical before the channel is programmed and
after it has fetched a whole picture**. They are therefore neither a
destination address, nor a size, nor a progress counter, nor anything the host
influences. **[C]**

The same shape appears in every other channel in F21's dump — the recon-luma
writer at `40d130240` reads `+0x04 = 01000003`, `+0x08 = 00080030`; the channel
at `40d130380` reads `03d00004` — so `+0x04`/`+0x08` are a **per-channel
read-only descriptor pair** present on the whole DMA fabric. **[I]**

Their *content* is **[U]**. Read as `(hi16, lo16)` the source pair partitions
something: luma `(0, 0xC0)`, chroma `(0xC0, 0x140)` — i.e. luma occupies
`[0, 0xC0)` and chroma `[0xC0, 0x140)` of a 0x140-unit resource, with the
second word giving each channel's own extent. That is what an internal FIFO or
line-buffer partition map looks like. **[I]**, and it does not matter: it is
fixed silicon state, not a knob.

> Recorded because the shape is a trap. `0xC0 = 192` and `0x140 = 320` are
> `4 × 48` and `4 × 80`, which invites reading them as "the picture is 80 × 48
> macroblocks" and joining them to the 3845/`y 47` puzzle in §4. **That reading
> is wrong**: the registers hold those values before the session knows the
> picture size at all. Two numbers agreeing is not a chain.

### 1.3 `+0x18` = `0x00002005`, and where `0x00072065` comes from

`0x00072065` is written by `setPipe` at fw `0x54944` — but that store is on the
**compressed-input arm**, reached only by `cbnz w11, 0x54934` at fw `0x54878`
where `w11 = PICMGMT[0x6F3]` (`bInputCompressed`). Our `+0x6F3` is 0, so we
take `0x5487c` instead and never write `+0x18`. **[C]**

`CAVCController::ProcessPipeReset` writes `0x00002005` to `+0x18` and `+0x98`
(fw `0x4ee28`, `0x4ee2c`: `stur w12,[x9,#-232]` / `[x9,#-104]` with
`x9 = base + 0x1120100`), and `0x00004004` to `+0x00` and `+0x80`
(`0x4ee14`, `0x4ee24`). **[C]**

So the sequence F21 records — `0x00072065` after `Start_AVC`, `0x00002005`
after the frame — is exactly "power-on default, then `ProcessPipeReset` ran".
The register is doing the right thing and carries no information about the
failure. **[C]**

The same reading applies to `+0x00`: `0x80034044` after `Start_AVC`,
`0x80034045` after `setPipe`. **Bit 0 of the config word is the channel
enable**, and it is set. `0x80034055` for chroma differs by bit 4 (channel
select) and `...47` by bit 1 (compressed). **[I]** on the bit names, **[C]** on
the values and the arms.

### 1.4 There is no second layout hiding in the block

`+0x04`/`+0x08` were the only candidates for "an internal SRAM target". The
alternative — that the source channel uses the *generic* channel layout and we
have mis-based it — is ruled out by comparing the two templates:

| | generic DMA channel | source picture reader |
|---|---|---|
| example | `CDMAController::ConfigureMBIFWData` fw `0x3c6e8`, base `0x1120C80` | `0x1120000` / `0x1120080` |
| `+0x00` | cfg `0x80030001` (`0x3c704`) | cfg `0x80034045` |
| `+0x0C` | **DRAM address** (`0x3c6f8`) | format word |
| `+0x10` | **size**, `align64` (`0x3c700`) | **DRAM address** |
| `+0x14` | — | **stride** |
| `+0x18` | burst/chunk, `0x20`/`0x180`/`0x400` (`0x3c6fc`) | reset `0x2005` |

Both confirmed **[C]**, and the generic layout is confirmed a third time by the
entropy channels ([62](62-kext-field-map.md) §0: size → `+0x10`, address →
`+0x0C`) and a fourth time by F21's `40d130380` row
(`80030001 · 00a80008 · 03d00004 · fd500000 · 00038400`: cfg, descriptor,
descriptor, address, size).

The source reader is a **2D picture reader**, a different template from the
linear channels, and it has no destination field because it has no destination
in memory.

---

## 2. The destination: there isn't one in memory

### 2.1 The fabric splits readers from writers

Across the whole firmware, channels at `0x112xxxx` are fed with addresses of
surfaces the pipe *consumes* and channels at `0x113xxxx` with surfaces the pipe
*produces*. F21's own dump is the cleanest evidence: at the timeout, every
`40d130xxx` channel holds a live IOVA and a size
(`40d1303c0: … fe000000 000f0000` is an entropy/SEB buffer; `40d130240` holds
`fdc00000 0000a000` and the progress word `002c004f`), while `40d120bc0` — a
reader — holds `00000000` at `+0x0C`.

The driver already labels them this way (`ave_session.c:2057`, "neighbour DMA
channels: readers `0x40D120C00..`, writers `0x40D130600..`"). **[I]**, but from
two independent sides.

### 2.2 Nothing publishes an input staging surface

- **No Start-time source table.** [62](62-kext-field-map.md) §6.1: none of
  `AVE_CHM_SetFwBuf`'s 24 destination ranges is an input surface, and
  `setRefPointers` rebuilds only PICMGMT `+0x980..+0xBF8`. **[C]**
- **No per-frame staging address.** `AVE_CHM_SetDataInfo_FwBuf` writes the
  input as four words only — luma address, luma stride, chroma address, chroma
  stride (kext `0xeb0904`/`08`/`10`/`14`). **[C]**
- **No firmware-side staging allocation.** The only firmware-allocated buffer
  `setPipe` attaches near the source path is the **statistics** writer at
  `0x1130700`: address and `align64` size at fw `0x54c80`/`0x54c84`, config
  `0x80030001` at `0x54c88`, logged as `AVC COMMON:: stats_DMA_addr` /
  `statsDMABufferSz` (strings `0xc632a`, `0xc634f`). Its address is
  `EncCommParams.encoder_addr_fw_data`, asserted non-zero (string `0xc61d8`) —
  that is **SrcNeighborFwData, wire `0xFED0`, which the driver does publish**
  (`src_nbr_set[3]`). No gap. **[C]**

So the fetched pixels are **streamed into the pipe**, not landed in memory.
The question "where do they go" has no memory answer, and the correct question
is the one in §5: which stage was supposed to consume the stream, and did it.

---

## 3. The hand-off to MbInput

### 3.1 `SRCDMAGO` is asserted twice and is self-clearing

`SRCDMAGO` is fw `0x1110128` = AP `0x40D110128`. In `setPipe`,
`x26 = 0x111000C` (fw `0x54a30`/`0x54a40`, reloaded `0x54d58`/`0x54d68`) and
the register is `[x26 + base + 284]`. **[C]**

Two writers:

```
57d60  ldr  w8, [x8, #284]       ; read, logged "setPipe read: SRCDMAGO 0x%x"  (str 0xc6b9e)
...
57dcc  str  w8, [x9, #284]       ; write, logged "setPipe write: SRCDMAGO 0x%x" (str 0xc6bbb)
57dd0  ldr  w8, [x9, #284]       ; read back for the log
```

and, later, in `CAVCController::setPipeGo` (fw `0x61fec`):

```
6203c  ldr  w11, [x9, x10]       ; x10 = 0x1110128
62040  orr  w11, w11, #0x1
62044  str  w11, [x9, x10]
```

**[C].** Both are on straight-line code: every path through `setPipe`'s
`0x57c60..0x57dcc` converges at `0x57d40` (the branches at `0x57c94`,
`0x57d1c`, `0x57d3c` all land there or fall through), and `setPipeGo`'s OR is
unconditional.

**So bit 0 is written as 1 twice per frame.** The driver reading
`0x40D110128 = 0` after the frame is therefore either a self-clearing go or a
register that the pipe reset clears — not evidence that the go was never
asserted. **[I]**, but strongly: `ProcessLRMEStart` (fw `0x51310..0x51380`)
does a read-modify-write of the same register that preserves every bit except
bit 3, which is only sensible on a register whose other bits persist, and
`setPipeGo`'s `orr #1` would be pointless on a level that `setPipe` had already
left set.

### 3.2 What the `SRCDMAGO` word is made of

From fw `0x57d6c`–`0x57dcc`:

```
w8  = (ctrl[5136] & 1)                 -> bits 1 and 2   (bfi w8,w8,#1,#1 ; lsl #1)
bit 3 = (*(u32*)(ctrl+0x13A3C) != 0) ? 1 : wire 0xFCE9
bits 4+ = wire 0xFECC                  (ldrb w10,[x19,#1636] ; orr w8,w8,w10,lsl #4)
bit 0 = 1                              (orr w8,w8,#0x1)
```

**[C]** for every line. The two wire offsets resolve like this, and both hops
are read out of the disassembly:

| wire | `InitEncodingParameters` | `setPipe` base | lands in |
|---|---|---|---|
| `0xFCE9` u8 | fw `0x5cfcc` `ldrb w8,[x23,#1321]` → `0x5cfd0` `strb w8,[x22,#40]` | `x24 = ctrl+0x23B38`, `[x24,#1204]` | `SRCDMAGO` bit 3 |
| `0xFECC` u8 | fw `0x5cfe4` `ldrb w9,[x23,#1804]` → `0x5d004` `strb w9,[x22,#472]` | `[x24,#1636]` | `SRCDMAGO` bits 4..11 |

with `x23 = VP + 0xF760` and `x22 = ctrl + 0x23FC4`, so wire `= 0xF7C0 + N` for
`[x23,#N]` (anchored by `[x23,#1320]` = wire `0xFCE8`, fw `0x5d118`, and
`[x23,#1792]` = wire `0xFEC0`, fw `0x5d018` — the two offsets
[62](62-kext-field-map.md) §6.2 already confirmed), and
`[x24,#N] = [x22,#N-1164]` (anchored by `[x24,#1608] = [x22,#444]` = wire
`0xFEC0` and `[x24,#1205] = [x22,#41]` = wire `0xFCE8`). **[C]**

**This closes two of the "firmware reads it, we never set it" rows in
[62](62-kext-field-map.md) §6.5.** `0xFCE9` and `0xFECC` were listed there with
no destination. They go into the source DMA's go register.

`ctrl+0x13A3C` (fw `0x53224`: `add x17,x19,#0x13,lsl#12 ; add x17,x17,#0xa3c`)
is the LRME result slot; when it is zero `setPipe` calls `setLRME`
(fw `0x57d30`, `bl 0x5144c`). `ctrl[5136]` is `cmdinfo+0x20`, copied at fw
`0x480f4`. Neither is host-settable. **[C]**

### 3.3 Wire `0xFCE9` also gates a third source-reader channel

`ProcessPipeReset` (fw `0x4edc0`):

```
4ede0  mov  w9,#0x3fec ; movk #0x2      ; ctrl + 0x23FEC  == [x24,#1204] == wire 0xFCE9
4ede8  ldrb w9,[x19,x9]
4edf0  cbnz w9, 0x4ee08                 ; non-zero -> SKIP
4edf4  add  x9, x8, x0                  ; x0 = 0x1120100
4edfc  str  0x4004, [x9]                ; 0x1120100 = 0x4004
4ee04  str  0x2005, [x9,#24]            ; 0x1120118 = 0x2005
```

**[C].** `0x1120100` takes exactly the values `ProcessPipeReset` gives the luma
(`0x1120000`) and chroma (`0x1120080`) readers, at exactly the same two
offsets, so it is a **third instance of the same picture-reader template** at a
`0x80` stride. With `wire 0xFCE9 = 0` — which is what we send — that third
channel is initialised; with it non-zero it is left alone. Whether it is the
low-res/LRME source reader is **[U]**, but `setPipe` also writes `0x1120100`
and `0x1120118` in the LRME branch at fw `0x57cb0`–`0x57cc8`, which is the
obvious reading. **[I]**

### 3.4 What MbInput actually consumes

[59](59-row1-stall.md) §1.3 has this from the MCPU images, and nothing here
contradicts it:

- the **hardware** source stage raises IRQ 29 and publishes the macroblock
  position in `0x40D175800`/`804` (bits 16..27 = MB y, 0..12 = MB x, bit 26 =
  last);
- MbInput's producer handler computes the MB QP, writes it into bits 4..11 of
  `0x40D175804`, increments **produced** (`DMem 0x100008a8`), copies the
  stage's per-MB statistics from `0x40D17C000` into a 17-entry ring of
  **192-byte records** in its own DMem, acknowledges the stage
  (`0x40D170100 = 1`) and re-enables the consumer IRQ once
  `produced ≥ consumed + [0x40D170110]` (= 15);
- the consumer pops a record into `0x40D171180..123C`, posts
  `hostif+0x14 = 0xc0000024|…`, writes `0x40D170108 = 1` and increments
  **consumed**.

So the hand-off is a **credit-gated ring in MCPU DMem, entirely hardware- and
firmware-owned**. The records carry *MB metadata and statistics*, not pixels;
the pixels go from the source reader to the estimation engines over an internal
bus that no register in this path names.

**Every register in the hand-off is programmed by the ASC firmware, not the
host.** `0x1170110 = 15`, `0x117010c |= 2`, `0x1170124 = 0`,
`0x1170128 = 0x3fff3fff` come from `CAVCController::ConfigureMCPUs`
(fw `0x61220`–`0x61264`); the MB-grid registers `0x1110000`/`0x1110004` come
from `setPipe` fw `0x531c0`/`0x531d8`. **There is no host field in the
hand-off.** **[C]**

Worth stating because it is the one thing we could have been missing and are
not: `0x1170000` and `0x1170004` — the MBIF geometry the MCPU reads — have **no
writer in the firmware image** (whole-image scan, including single-`mov`
immediates). They are populated by hardware from the pipe geometry. **[C]**

---

## 4. 3845, and `y 47` in a 45-row picture

The picture is 80 × 45 = 3600 macroblocks. F21 reports:

```
MbInput produced 3845 consumed 3845 drain 0x1 lag 0
last src event 0x002f0048 0x02000000 (y 47 x 72 last 0)
ModeDec entries 3845  ReconLuma granted 3845  CAVLC entries 3845
srcdma 0x40D12002C 0x002c004f -> currMbRow 44
coded header: MB counts I 3600 P 0 skip 0 = 3600 of 3600 expected
```

### 4.1 The reader is right; the stage behind it is not

`0x40D12002C` = `0x002c004f` is `currMbRow 44`, column 79 — the last macroblock
of a 45-row, 80-column picture — and `ProcessPipeDone` (fw `0x596a0`) only
declares the frame finished when `currMbRow + 1 == ctrl[0xA94]`
(`pic_height_in_mbs`). The completion arrived, so `ctrl[0xA94] = 45` and the
**source reader walked exactly the right picture**. **[C]**

`y = 47` therefore cannot come from the reader. It comes from the *source
analysis* stage that raises the MB event, and a `y` of 47 is impossible in a
45-row grid.

### 4.2 The arithmetic

`3845 = 80 × 48 + 5`. With 80 columns and 48 rows the last full event is
`(47, 79)`; 3845 events is that plus five. `y = 47` is the last row of a 48-row
grid. Two independent numbers, one geometry. **[I]**

The five trailing records are almost certainly the flush: MbInput's IRQ-28
"last MB" handler sets the drain flag and "produces the final records"
([59](59-row1-stall.md) §1.3), and F21 shows `drain 0x1` with
`produced == consumed` and `lag 0`, i.e. the drain ran to completion. **[I]**

The recorded event `(47, 72)` is not the 3845th — `47·80 + 72 + 1 = 3833` — so
either the register is stale by twelve events or the flush records do not
advance it. Either way it does not change the conclusion. **[U]** which.

Three decompositions were tested and rejected, recorded so nobody re-derives
them:

| candidate | arithmetic | why it fails |
|---|---|---|
| `3600 + 240 + 5`, the 240 being a low-res/LRME pass at 320×192 (20 × 12 MBs) | exact | the low-res grid's `y` maxes at 11; it cannot produce `y = 47` |
| `+0x04`/`+0x84` encode `(4×48, 4×80)` | `0xC0 = 4·48`, `0x140 = 4·80` | §1.2: those registers hold the same values **before** the picture size is known |
| the picture is 1280 × 768 throughout | 768/16 = 48 | the reader's `+0x2C` and `ProcessPipeDone` both say 45 rows |

### 4.3 Where 48 could come from — and what would decide it

`setPipe` programs the geometry from **two different `ctrl` fields**, and they
are computed differently:

| register | written at | value | source |
|---|---|---|---|
| `0x40D111000` | fw `0x531c0` | a flag word | assorted |
| `0x40D111004` | fw `0x531d8` | `(ctrl[0xA94] & 0x1FFF) << 16 \| (ctrl[0xA98] & 0x1FFF)` | **MB units**, SPS-derived |
| `0x40D11E0004`, `0x40D11F0004` | fw `0x53200`, `0x53210` | `((ctrl[0xA90]+15)>>4) << 16 \| ((ctrl[0xA8C]+15)>>4)` | **pixels**; `ctrl+0xA8C`/`+0xA90` ← VP `+0x00`/`+0x04` = wire `0x60`/`0x64`, `InitEncodingParameters` fw `0x5ced0`–`0x5ced8` (`ldr x8,[x20] ; lsr x9,x8,#32 ; stp w8,w9,[x26]`, `x26 = ctrl+0xA8C` from fw `0x5cb44`) **[C]** |
| `0x40D11E0008`, `0x40D11F0008` | fw `0x53250`, `0x53260` | same, 11-bit fields | as above |
| `0x40D11011C` | fw `0x5415c` | `ctrl[0x1438] & 0xFFF`, logged **"Context Height 0x%x"** (string `0xc6186`) | firmware running state (fw `0x59dc8`) |
| `0x40D110120` | fw `0x541b4`/`0x541e4` | `[sp,#184] & 0x1FFF` | |
| `0x40D1190108`, `0x40D11E0108`, `0x40D11F0108` | fw `0x5412c`–`0x54148` | the same "Context Height" | |

All **[C]**. The driver sends `cw = mb_align(1280) = 1280` and
`ch = mb_align(720) = 720` at wire `0x60`/`0x64` (`ave_cmd.c:403`–`425`), so
`0x11E0004` should read `(45<<16)|80` and `0x111004` `(45<<16)|80`. **If either
reads 48, we have the field.** None of these registers has ever been read on
hardware.

**Candidate explanations, ranked:**

1. **The three extra rows are the pipeline's own vertical drain**, not a
   configuration error: the analysis stage runs three MB rows past the picture
   to flush its vertical context, and MbInput dutifully counts the events. This
   is benign and would be true on macOS too. **[I]**, and the cheapest to
   believe because the reader, `ProcessPipeDone` and the coded MB count are all
   correct. Note `3` is also `ceil(15/80)+2`, i.e. of the order of the 15-MB
   lookahead — but that is numerology, not a derivation.
2. **"Context Height" (`ctrl+0x1438`) is 48.** It reaches four separate blocks
   (`0x1190108`, `0x1190908`, `0x11E0108`, `0x11F0108`) plus `0x111011C`. Its
   writer is fw `0x59dc8` (`str w25,[x19,#5176]`) in the frame-completion path,
   with `str wzr` at `0x59d30` and `0x5ba64` — i.e. it is *running state*, not
   a host field. **[C]** on the plumbing, **[U]** on the value.
3. **A padded height field we do not send.** The kext-side hunt found nothing:
   [62](62-kext-field-map.md) §1.4 enumerates every `VIDEO_PARAMS` scalar under
   `0x28` and only `0x00`/`0x04` are dimensions. **[C]**

Whichever it is, **it is not by itself a reason for a grey picture**: 3845
macroblocks were produced, consumed, mode-decided, reconstructed and entropy
coded, and the coded header still counted exactly 3600.

---

## 5. What produces "no residual"

### 5.1 The discriminator that already exists

F17 encoded a 226-value horizontal ramp. F18 encoded a constant luma 200. Both
produced **2709 bytes**, byte-for-byte the same length, and both decoded to a
uniform frame. A mode decision that had seen pixels could not produce the same
bit count for a ramp and for a flat plane — the intra modes and the DC
residuals differ. So the pixels reached neither the transform **nor the mode
decision**. ([53](53-first-frame.md) §28 and the F18 section.) **[C]**

And the value matters: a source of all-zeros would encode as *black*, because
the first macroblock's DC prediction with no neighbours is 128 and the residual
would be −128. Uniform 128 with no residual is what you get when the encoder
computes **no difference at all**.

### 5.2 F21 names the stage

```
diag MbInput produced 3845 consumed 3845 ... IntraEst curMB 0x00000000
diag hif MbInput     +0 0x00050000 +4 0 +c 0          +10 0          +14 0x00000024
diag hif IntraEst    +0 0x00002000 +4 0 +c 0          +10 0          +14 0x00000000
diag hif ModeDec     +0 0x00006000 +4 0 +c 0x00000026 +10 0x00000026 +14 0x00000005
diag hif ReconLuma   +0 0x00002000 +4 0 +c 0x0000002b +10 0x0000002b +14 0x0000000a
diag hif ReconChroma +0 0x00006000 +4 0 +c 0x00000035 +10 0x00000035 +14 0x00000001
diag hif MotionEst   +0 0x00008000 +4 0 +c 0          +10 0          +14 0x00000000
diag hif CAVLC       +0 0x00006000 +4 0 +c 0          +10 0          +14 0x10000000
diag MCPU counters ModeDec entries 3845 ReconLuma granted 3845 CAVLC entries 3845
```

`IntraEst curMB` is `0x40D143180`, the MB context word IntraEst's per-MB
handler reads ([59](59-row1-stall.md) §2.1). It is **zero after 3845
macroblocks**, and IntraEst's host-interface `+0x14` — the register every stage
posts its request word into — is **zero**, while ModeDec, ReconLuma and
ReconChroma all carry non-zero work words. IntraEst's enable word `+0 =
0x2000` shows bit 13 *is* armed, so the stage is configured and simply never
got anything to do. **[C]** for the readings.

**An I-frame whose intra estimator never ran is a frame of default I_16x16 DC
macroblocks with no coefficients.** That is uniform 128, source-independent,
and the same size every time. It is the whole observation, in one line. **[I]**

MotionEst being idle is expected on an I-frame and is not part of this.

### 5.3 Every configuration that could produce it, and the host field behind each

| # | mechanism | host field / knob | what we send | can it be ruled out statically? |
|---:|---|---|---|---|
| 1 | The source reader's transactions are tagged with a **stream id the DART does not translate**, so no pixel data returns and the estimators are never offered a macroblock | none — the DT `iommus` property and `ave_dart_restore_datapath()` | SIDs 0 and 1 only | **No.** F21: `CPUDART TCR[2..15] = 0` and `DART1 TCR[2..15] = 0` — no TRANSLATE, no BYPASS. The ADT says `sids = 0x8001`, i.e. **SID 0 and SID 15**, with `bypass = 0x8000` ([56](56-datapath-dart.md) §2.3). **SID 15 is a declared AVE stream that Linux has never configured.** |
| 2 | `SRCDMAGO` bits 4+ (wire `0xFECC`) and bit 3 (wire `0xFCE9`) select a source mode / port that is off at 0 | wire `0xFECC` u8, wire `0xFCE9` u8 | **0 / 0** | No. Both are pass-through `VIDEO_PARAMS` bytes the kext neither writes nor validates ([62](62-kext-field-map.md) §1.2), so no value is knowable from either binary. §3.2 |
| 3 | `0x40D120050` / `0x40D1200D0` (wire `0xFEC0`) select the source layout | wire `0xFEC0` u16 | 0; swept once at 9 (s3), registers moved, picture unchanged | Partly: `0` and `9` are dead. The other 14 low values are not. |
| 4 | Bits 0..7 of the format word `0x40D12000C` are a mode/bypass field | none — `setPipe` fw `0x549f0`–`0x54a08` always leaves them 0 | 0 | **Yes, as a host issue.** The firmware never sets them, so macOS runs with them 0 too. |
| 5 | `bEnableFwOverride` / `bEnableMBInputCtrl` divert MB input to a firmware-supplied stream | `VIDEO_PARAMS` booleans; the paired surface is **MBInputCtrl, wire `0xFCC8`, 2 × u64**, which the kext publishes and we do not | both 0, table unpublished | **Yes.** The firmware asserts `(bEnableFwOverride==0) \|\| (bEnableMBInputCtrl==0)` (string `0xc90a2`) and with both 0 no override is armed. A *sixth* missing table exists, but it is inert. |
| 6 | The quantiser zeroes everything | `qp` at Start_AVC | fixed QP, sane | **Yes.** QP cannot make a flat-200 plane code as 128, and cannot make a ramp and a flat plane the same length. |
| 7 | The recon MSB/LSB split (`NEED_LSB_PLANES`, wire `0xFD7D`) corrupts what the estimators read back | wire `0xFD7D` u8 | 1 | No, but it is downstream of the first macroblock, which is already wrong. Low prior. |
| 8 | Source geometry mismatch (§4) starves the estimators of valid macroblocks | see §4.3 | — | No, but the counters say the macroblocks *were* produced and consumed. Low prior. |

**Ranked, most to least likely: 1, 2, 3, 8, 7.** 4, 5 and 6 are closed.

Row 1 is first for four reasons that no other candidate satisfies together:
the output is independent of the source; no DART, SMMU or AXI fault is latched
(a *disabled* stream need not fault, while a mistranslating one would); the
datapath DART's `ERROR` is non-zero in every run (`0x0a0d0000` in F21) and has
never been decoded; and the ADT explicitly names a second AVE stream that this
driver has never touched.

The one thing row 1 does not explain cleanly is **why 128 rather than 0**: a
dropped read usually returns zeros, and zeros would encode as black. The
resolution is §5.2 — if the fetch never *completes*, the estimators are never
offered a macroblock at all, so nothing is ever subtracted and the DC
prediction stands. That is consistent with `IntraEst curMB = 0` and with the
frame still finishing, because the MB *events* come from the address generator,
not from returned data.

---

## 6. Driver changes this analysis supports

### 6.1 Rank 1 — give every declared stream a translation

`dts/t6001-ave.dtsi:42` binds one stream:

```
iommus = <&dart_ave0 0>;
```

The ADT for `dart-ave0` gives `sids = 0x8001` — **streams 0 and 15**
([41](41-apple-fetch-path.md), [56](56-datapath-dart.md) §2.3) — and
`remap = 1` folds stream 1 onto stream 0's context, which Linux does not
program either. F21 confirms the result on hardware: `TCR[2..15] = 0` on both
DARTs.

Two edits, both small:

1. `dts/t6001-ave.dtsi`: `iommus = <&dart_ave0 0>, <&dart_ave0 1>, <&dart_ave0 15>;`
   (or all sixteen). `apple-dart` attaches the same page tables to each stream
   listed, so this costs nothing but TTBR writes.
2. `driver/ave_dapf.c:433`: `static const unsigned int sids[] = { 0, 1 };` →
   iterate every bit set in `ENABLED_STREAMS` (`0x0000ffff`), so
   `ave_dart_restore_datapath()` mirrors all of them onto DART1 and reads them
   back. The existing per-SID read-back and mismatch count already cover it.

Add the REMAP register to the dump while there: F21 prints
`REMAP[0] 0x03020100` on the datapath DART, where macOS programs `0x03020000`
(stream 1 → SID 0). That is a one-word difference that has never been looked
at, and `ave_dapf.c` already reads the register.

### 6.2 Rank 2 — expose the two new `SRCDMAGO` fields

`driver/ave_abi.h` has `src_mode` (wire `0xFEC0`) and `src_cfg_byte` (wire
`0xFCE8`). Add two more with the same machinery (`ave_cmd.c:477`–`480`):

```c
u32 src_go_bits;   /* wire 0xFECC, u8 -> SRCDMAGO 0x40D110128 bits 4+  (fw 0x57da8/0x57db4) */
u32 src_go_bit3;   /* wire 0xFCE9, u8 -> SRCDMAGO bit 3, and gates ProcessPipeReset's
                    * init of the third reader channel 0x40D120100 (fw 0x4edf0) */
```

with module parameters `session_src_go` / `session_src_go3`, and print the
expected `SRCDMAGO` word next to the read-back the driver already does
(`ave_session.c:2353`). This is sweep infrastructure, not a fix — but it is the
only remaining host input to the source DMA that has never been on the wire,
and it costs nothing to add.

### 6.3 Free diagnostics to fold into the next load

All read-only, all in bank 0:

- `0x40D111000`, `0x40D111004` — pipe MB geometry, expect `(45<<16)|80`;
- `0x40D11E0004`, `0x40D11F0004`, `0x40D11E0008`, `0x40D11F0008` — the
  pixel-derived geometry, expect the same;
- `0x40D11011C` — **"Context Height"**, and `0x40D110120`;
- `0x40D120100`, `0x40D120110`, `0x40D120114`, `0x40D120118` — the third
  reader channel (§3.3): if `+0x110` is non-zero it has an address we never
  supplied, and if it is zero with `+0x100` enabled that is a channel armed
  with nothing;
- `0x40D175800`/`804` sampled *during* the frame rather than after it, which
  would settle whether the `(47, 72)` event is stale;
- nothing new is needed for the DART error state — `ave_dapf_dump_dart()`
  already prints DART1's `ERROR` and `ERROR_ADDR` and all sixteen TCR/TTBR
  sets. What is missing is a *decode* of DART1's `ERROR`: F21 shows
  `0x0a0d0000 addr 0x00000000dc7b82d0`, which under the CPUDART's own format
  (`DART_ERR_STREAM(v) = (v>>24)&0xf`) would read "stream 10" with no `FLAG`
  bit and code 0. Earlier runs show `0x0c0f0000`. A latched fault should carry
  a code; these do not, so the register is either not a fault latch on this
  instance or the format differs. **[U]**, and worth ten minutes because a
  non-zero `ERROR_ADDR` on the datapath DART has never been explained.

### 6.4 The one-load experiment

**Map the source planes into every enabled stream, and dump all sixteen TCR /
TTBR sets before and after.** One load, one variable, and it can say no:

- picture changes → cause 1, and the fix is §6.1;
- picture unchanged and the new TCRs read back TRANSLATE with the CPUDART's
  TTBRs → cause 1 is dead, and the next load sweeps wire `0xFECC` (§6.2).

The dump alone is free and already almost implemented (`ave_dapf.c:400`
iterates sixteen TCRs); the change is only the `sids[]` array and the DT
property. Because it is read-mostly and adds translations rather than removing
them, it does not widen the blast radius beyond what the driver already has.

---

## 7. Corrections and annotations to earlier documents

- [62](62-kext-field-map.md) §6.2 lists `0x40D120018` as "`0x00072065`,
  compressed arm only". Correct, and complete it: the value the linear arm
  leaves there is `0x00002005`, written by `ProcessPipeReset` (fw `0x4ee28`),
  and `0x00072065` is the power-on default. §1.3.
- [62](62-kext-field-map.md) §6.5 lists wire `0xFCE9` and `0xFECC` with no
  destination. Both now have one: `SRCDMAGO` bit 3 and bits 4+. §3.2.
- [53](53-first-frame.md) "the stream-id hypothesis" proposes dumping TCR and
  TTBR for all sixteen streams. `results/f21-1789893453.kmsg` **already
  contains that dump** — `TCR[2..15] = 0` on both DARTs — so the read has been
  done and the remaining question is whether the source DMA uses one of them.
  The ADT's `sids = 0x8001` makes SID 15 the specific candidate, not a blind
  sweep. §5.3 row 1.
- [59](59-row1-stall.md) §1.5 notes the low half of `0x40D12002C` is never
  decoded in firmware and "a column index would be consistent". F21's
  `0x002c004f` = row 44, column 79 at the end of a 45×80 picture confirms it is
  a column. **[C]**

---

## 8. Reproduce

```sh
# the source-read block, whole
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54200 -n 0x820

# ProcessPipeReset: the 0x2005 / 0x4004 defaults and the wire 0xFCE9 gate
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4edc0 -n 0x80

# SRCDMAGO: setPipe's write, and setPipeGo's
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57c60 -n 0x180
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x61fec -n 0x60

# the two new wire offsets, both hops
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cfc0 -n 0x60   # 0xFCE9, 0xFECC -> ctrl
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d110 -n 0x20   # 0xFCE8 anchor

# the geometry writes
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x531b8 -n 0xd0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54108 -n 0x60   # "Context Height"

# the generic channel template, for the layout comparison in 1.4
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x3c5d8 -n 0x140

# the hardware side
grep -E 'chan \[|TCR\[|diag hif|diag MbInput|diag MCPU counters' \
     results/f21-1789893453.kmsg
```

The whole-image MMIO map that §1.2's negative rests on is reproducible: dump
`__TEXT` with `--addr 0x0 -n 0xec000`, then track `mov`/`movk`/`add`/`sub`
constants and the `ldr x8,[x?,#1976]` after `adrp x?, 0x21a000` that loads the
MMIO base, and record every `str`/`stur`/`ldr` whose base resolves into
`0x1000000..0x2000000`. It reproduces all of [62](62-kext-field-map.md) §6.2
independently, which is the control that the scan is not blind.
