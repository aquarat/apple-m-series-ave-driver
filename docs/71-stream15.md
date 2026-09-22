# Stream 15: who issues on it, what macOS gives it, and whether it feeds IntraEst

Static analysis of the macOS 13.5 kext, the PPL `t6000dart` driver, the 13.5
firmware and the Apple Device Tree, cross-read against the F22-f35 logs in
`results/`. **Nothing here was run on hardware.** Prompted by the fault every
run since F22 logged on the datapath DART:

```
apple-dart 40d030000.iommu: translation fault: status:0x8f0f0002 stream:15
                            code:0x2 (NO PMD FOR IOVA) at 0x7f100000
```

Conventions as [62](62-kext-field-map.md) and [69](69-source-to-modedec.md):
firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`); kext VAs are
13.5 kernelcache VAs; **wire** = byte offset in the command on IPC channel 1,
VP + `0x60` = wire; firmware offset `X` in the MMIO window `*(0x21a7b8)` is AP
`0x40C000000 + X`. Labels per [00](00-methodology.md): **[C]** read from an
instruction (VA cited), **[I]** inferred with the chain stated, **[U]** unknown.

```sh
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008befe14 -n 0xd0
AVE_MACOS=13.5 python3 tools/disas.py --fw   --addr 0x5590c -n 0x60
```

**`AVE_MACOS=13.5` is not optional.**

---

## 0. The short version

| Q | Answer | Conf |
|---|---|---|
| Q5 Is the stream-15 engine the source-neighbour feed for intra estimation? | **No.** The four source-neighbour DMA channels (Info/Pixel read+write, Data, FwData) are programmed with the full 32-bit IOVA (`str w` of a 64-bit load, no masking) and their *first* access would fault at `0x7f000000`/`0x7f050000`/`0x7f0f0000`, not `0x7f100000`; FwData is written with its enable bit clear on our single-core arm. On hardware, f33b/f34 (stream 15 translating, IOVAs below 2 GiB, zero faults) and f35 (stream 15 detached) all produced the same 2709-byte frame. Whatever stream 15 carries, the blank frame does not depend on it | **C** (channels) / **C** (runs) |
| Q1 Which engine issues on stream 15 | **Not identifiable statically.** Neither binary assigns stream IDs: the kext uses one mapper (`/arm-io/dart-ave/mapper-ave`, SID 0) for every surface, no per-channel field in the DMA templates carries a stream, and the SMMU that sits in the datapath is locked and never programmed by macOS. Ruled out: the source reader, the neighbour channels, the pipe MCPUs, and the pipe-clock-gating path (that is the ASC CPU). §3 | **U**, with the negatives **C** |
| Q2 What macOS programs for stream 15 | PPL writes **TCR[15] = `bypass-address[15] << 16` = `0x20000` on the datapath DART** (no `TRANSLATE_ENABLE`, and no `BYPASS_DART` because that instance's PARAMS2 bit 0 is clear) and `0x20100` on the CPUDART; REMAP[3] stays identity (`0x0f0e0d0c`). The 10-bit field at TCR[25:16] is the bypass window's upper physical address bits; 2 selects PA `0x2_0000_0000`, which is the **arm-io MMIO bus**, not DRAM. Every coprocessor DART on the SoC has the same `bypass = 0x8000, bypass-address[15] = 2`. No host field supplies an address to this stream: the SrcNeighbor tables are ordinary SID-0 IOVAs from `AVE_Surface::GetDARTAddr` | **C** (code) / **I** (window meaning) |
| Q3 Why bit 31 | No firmware store masks or shifts an address to 31 bits, and no firmware constant equals `0x7f100000` or `0xff100000`. If the address is a truncated arena IOVA, the truncation is in hardware and is not a plain bit-31 drop: `0x7f100000` is not any buffer's base but `FwData[0] + 0x10000` = `Info[0] + 0x100000` = the region end rounded up to 64 KiB / 1 MiB. A constant unrelated to our buffers fits the data equally well | **C** (no fw masking) / **U** (field) |
| Q4 What the driver should do | **(c): do not attach stream 15**, which is what macOS effectively does on the datapath DART (TCR `0x20000` translates nothing). Bypass (a) is impossible on `0x40d030000` (no bypass support; apple-dart refuses an identity domain). Keep the 32-bit DMA mask. §6 | **C** / **I** |

Side result: the long-open identity of `ave0` register bank 3 (`0x8E588000`,
36 bytes) is settled in §4.3: it is the AVE sub-block **PMGR power-state
window**, which 13.5 DART-maps and hands to the firmware as Config `+0x48`
(`reg_dart_addr`) **only on ChipType <= 5**. On t6001 (ChipType 7) the kext
sends 0, exactly what this driver sends.

---

## 1. What the fault says

- `status = 0x8f0f0002`: bit 31 FLAG; stream = bits 27:24 = **15**; code =
  bits 23:0 = `0x0f0002`: bit 1 NO_PMD, bit 10 (WRITE) **clear**, so it is a
  **read**; bits 19:16 = `0xf` are unnamed in every public decode (m1n1
  `dart8020.py` `R_ERROR`, Linux `apple-dart.c` `DART_T8020_ERROR_*`). The
  same `0xf` in bits 19:16 appears in the error DART1 had latched before our
  session (`ERROR 0x000f0400 addr 0xdcfb00f0`, a stream-0 *write*,
  `results/f24-1789897178.kmsg` l.436), so it is not the stream number. **[C]**
  on the decode, **[U]** on bits 19:16.
- The address is page-exact: the F4 stream-0 faults walk the luma buffer at
  16 KiB multiples (`0xfe000000`, `0xfe03c000`, `0xfe054000`, ...), so the
  DART reports the faulting page, not a coarser region. The access is in
  `[0x7f100000, 0x7f104000)`. **[C]**
- The error register latches the first fault after the handler's clear, so
  each of the three events is a *first* access at that page: the engine's
  first address is `0x7f100000` three times per frame, not a sweep that
  starts lower. **[I]** (Linux handler semantics + F4's monotone sequence).
- Timing (F22, `results/f22-1789896258.kmsg` l.1929-1941): Process sent at
  `286.110661`; faults at **+1.19 ms**, **+7.60 ms**, **+7.91 ms**; the
  Process reply at +8.23 ms. One access near pipe start, two just before
  completion. F24 is the same shape (+0.84, +8.04, +8.41 ms, reply +8.74 ms).
  **[C]**
- Under the 32-bit mask our SrcNeighbor arena is 16 slots of 80 KiB from
  `0xff000000` (`ave_session_alloc_nbr`, `driver/ave_session.c:1565`), so
  with bit 31 restored the page is `FwData[0] (0xff0f0000) + 0x10000`, which is
  also `Info[0] (0xff000000) + 0x100000`. No IOVA the driver publishes equals
  `0xff100000` (every `%pad`/`%#llx` in F24 was listed; the nearest are the
  arena and `shmem 0xff2c0000`). **[C]**

---

## 2. Stream assignment

### 2.1 The ADT: SID 15 bypass is a platform convention, not an AVE property

`data/blobs/macos-13.5/adt.bin`, parsed directly (`tools/adt_dump.py` needs
the `construct` module, absent here; the node format is trivial):

| node | `sids` | `bypass` | `bypass-address` | `remap` |
|---|---|---|---|---|
| dart-ave0 / dart-ave1 | `0x8001` | `0x8000` | `[15] = 2` | `1` (stream 1 -> SID 0) |
| dart-isp0 | `0x8001` | `0x8000` | `[15] = 2` | - |
| dart-sio | `0x8007` | `0x8000` | `[15] = 2` | - |
| dart-aop | `0x8081` | `0x8000` | `[15] = 2` | - |
| dart-sep, dart-pmp | `0x8001` | `0x8000` | `[15] = 2` | - |
| dart-acio0..2 | `0x8003` | `0x8000` | `[15] = 2` | `0x102` |
| dart-dcp, dart-dcpext0..3 | `0x8001` | `0x8000` | (none) | `5` |
| dart-disp0, dart-dispext0..3 | `0x8011` | `0x8000` | (none) | - |
| dart-ane0 / ane1 | `0xa001` | `0xa000` | `[13] = 2, [15] = 2` | - |
| dart-usb0..2, dart-jpeg0/1, dart-apcie*, dart-scaler*, dart-avd0 | no 15 | - | - | - |

**[C]**. Every DART that fronts a coprocessor declares SID 15 bypassed with
the same window value 2; DARTs for plain DMA engines do not. Linux drives
DCP, ISP, SIO, AOP and ANE with SID 0 only and none of them needs SID 15, so
stream 15 is something those drivers can live without. **[C]** for the ADT,
**[I]** for the generalisation.

### 2.2 What the 13.5 kext does with streams: nothing per-stream

- `AVE_DART::RetrieveMapperInfo` looks up **one** registry entry,
  `/arm-io/dart-ave/mapper-ave` (string at `0xfffffe0008ed8ec0`), and reads
  `iomd-cache-size` (`0x8ed8fec`). The ADT `mapper-ave0` has `reg = 0`, i.e.
  SID 0. There is no second mapper and no other SID anywhere in the kext:
  `bypass`, `sid`, `stream` do not occur in its strings except in the DART
  error print `"DART Error %p %d | IsWrite: %d SID: 0x%llx Addr: 0x%llx AXI
  ID: 0x%llx"` (`AVE_DART::ErrorHandler`). **[C]**
- `AVE_Surface::GetDARTAddr(j, off)` (`0xfffffe0008f35fa0`): requires bit
  `16+j` of `[this+280]` (`0x8f35fcc..fd4`), returns `[this+200+8j]->[24] + off`
  (`0x8f35fdc..fec`). `j` is the per-surface DART slot; every caller in
  `AVE_CHM_SetFwBuf` passes `w1 = [chm+0x18]` (`ldr w1,[x19,#24]`
  `0x8eaee04`; `x26 = x19+0x18` `0x8eaee54`), the same slot for every table.
  **[C]**
- The SrcNeighbor tables are filled exactly that way: four loops of four
  `GetDARTAddr(slot, 0)` results at VP `+0xF770` (wire `0xF7D0`, Info),
  `+0xF790` (`0xF7F0`, Pixel), `+0xF7B0` (`0xF810`, Data), `+0xFE70` (`0xFED0`,
  FwData) - `0x8eaf16c..0x8eaf24c`. **[C]** So under macOS these are ordinary
  SID-0 IOVAs from the one mapper, identical in kind to every other surface.
  Nothing in the kext gives any engine a physical address for them.
- The DART kext (`AppleT6000DART`) parses `sids`/`bypass`/`remap`/
  `bypass-address` and forwards them to PPL ([56](56-datapath-dart.md) §1);
  no AVE-side code touches REMAP or TCR.

### 2.3 What PPL programs for stream 15

Config pass `0x8befc74` (ioctl `0x6008`), per DART instance (`0x300` stride):

| step | VA | what |
|---|---|---|
| `w21 = PARAMS2` of the instance | `ldr w21,[x8,#4]` `0x8befcb8` | `x8 = st + 928 + i*0x300` |
| iterate `sids` mask `[st+3344]` | `0x8befe14..e34` | `rbit/clz` per set bit |
| `w12 = bypass-address[sid] & 0x3ff` | `ldrh w11,[x10,#3368]; and w12,w11,#0x3ff` `0x8befe40..44` | ADT u16 per SID |
| bypassed SID (`[st+3348]` bit set): `TCR = w12 << 16` | `lsl w24,w12,#16` `0x8befe50` | |
| ... plus `0x100` only if PARAMS2 bit 0 | `tbnz w21,#0 -> orr w24,w24,#0x100` `0x8befe54`, `0x8befed8` | |
| `bypass-address == 0xffff`: take TCR[25:16] from hardware | `ldr w10,[x11,(sid\|0x40)*4]; ubfx w10,w10,#16,#10` `0x8befe70..74` | reads the live TCR |
| translated SID: `TCR = 0x80 \| w12 << 16` | `mov w24,#0x80; bfi w24,w12,#16,#10` `0x8befe8c..90` | |
| `apf-bypass` SIDs get `0x1000` | `0x8befe9c..ea8` | AVE has none |
| write TCR, cache it | `bl 0x8bf1e5c` `0x8befec0`; `str w24,[x8,#1624]` `0x8befec8` | |

**[C]** for every line. So:

- **CPUDART** (`0x40d040000`, bypass support 1): `TCR[15] = 0x20100`
  (`BYPASS_DART` + window 2); `TCR[0] = 0x80`.
- **datapath DART** (`0x40d030000`, bypass support 0 - Linux prints `bypass
  support: 0` for it, `results/f22-1789896258.kmsg` l.9, i.e. PARAMS2 bit 0
  clear): **`TCR[15] = 0x20000`** - neither `TRANSLATE_ENABLE` nor
  `BYPASS_DART`. **[C]** on the arithmetic, **[C]** on PARAMS2 from the log.
- REMAP: PPL starts from the identity vector `00 01 .. 0f`
  (`ldr q0,[x8,#3440]` `0x8bef2d0`, bytes verified in the kernelcache at
  `0xfffffe000700ed70`), sets `table[src] = dst` (`0x8bef324`) and rejects a
  remap that names a bypassed SID (`0x8bef310..320`). With `remap = 1` only
  byte 1 changes: REMAP[0] = `0x03020000`, **REMAP[3] = `0x0f0e0d0c`**, stream
  15 maps to itself. F24 read exactly `REMAP[3] = 0x0f0e0d0c` from both DARTs
  (l.157). **[C]**

What TCR `0x20000` *does* on a DART without bypass support is **[U]**: it
carries the window field but no mode bit. Two facts bracket it: Linux treats
TCR = 0 as "DMA blocked", and our F21 (TCR[15] = 0) and f35 (stream 15 not
attached) runs completed frames with no fault on stream 15. Whatever the
stream-15 master does when it is neither translated nor bypassed, macOS on
this SoC leaves it in that state on the datapath DART. **[I]**, strong.

### 2.4 The bypass window is MMIO, not DRAM

TCR[25:16] is 10 bits wide (`bfi ... #16, #10`, `ubfx ... #16, #10`). The
DART's output address space is 42 bits (`AS 32 -> 42` in Linux's probe line)
and its input is 32 bits, so a 10-bit field completes PA[41:32]: **window
value 2 = PA `0x2_0000_0000` + 32-bit AXI address**. On this SoC that is the
`arm-io` bus (`/arm-io ranges`: bus `0x0` -> parent `0x2_0000_0000`, size
`0x4_0000_0000`, [30](30-address-translation-bug.md)); DRAM starts at
`0x8_0000_0000`. **[I]**, medium-high: it is the only encoding in which the
field width, the DART's address widths and the value 2 fit together, and it
explains why every *coprocessor* DART has it (a coprocessor that must reach
SoC peripherals below 4 GiB of bus space: PMGR at bus `0x8e580000`, AIC,
GPIO) and no plain DMA engine's DART does. If instead the unit were 16 GiB,
2 would be DRAM base; nothing in either binary decides this, so it stays
**[I]**.

Consequence either way: **there is no host field that supplies a physical
address to stream 15.** If the window is MMIO, the master addresses
peripherals by their bus address and needs nothing from the host. If it were
DRAM, macOS would still be handing every buffer out as a SID-0 IOVA (§2.2),
which through such a window would read the wrong physical page - so the
stream-15 master cannot be a buffer consumer on macOS either.

---

## 3. Which engine issues on stream 15

### 3.1 The engines that read the SrcNeighbor tables - and their address width

All in `CAVCController::setPipe` (`0x52f38`) unless noted; `base` =
`*(0x21a7b8)`; ctrl fields are the 64-bit copies made by
`InitEncodingParameters` (`0x5d874..0x5d8c8`) from the wire.

| channel | AP | address reg | source | size reg | cfg | VA |
|---|---|---|---|---|---|---|
| Info **read** | `0x40D120C00` | `+0x0C` | `ctrl+4544` = wire `0xF7D0` (`src_nbr_info`) | `+0x10` = `ctrl+0x8c28` | `0x80030001` | `0x557e4`, `0x557f4`, `0x557a0` |
| Pixel **read** | `0x40D120C40` | `+0x0C` | `ctrl+4560` = wire `0xF7F0` | `+0x10` = `ctrl+0x8c2c` | `0x80030001` | `0x55810`, `0x5581c`, `0x557a4` |
| Info **write** | `0x40D130600` | `+0x0C` | `ctrl+4552` (same wire word) | `+0x10` | `0x80030001` | `0x55850`, `0x55860`, `0x5583c` |
| Pixel **write** | `0x40D130640` | `+0x0C` | `ctrl+4568` (same wire word) | `+0x10` | `0x80030001` | `0x55884`, `0x5588c`, `0x55840` |
| FwData **read** | `0x40D120E40` | `+0x0C` | `ctrl+7888` = wire `0xFED0` | `+0x10` = `ctrl+7880` | **`0x80030000`** (`w24 - 1`) | `0x55918`, `0x55928`, `0x55930` |
| FwData **write** | `0x40D130780` | `+0x0C` | `ctrl+7896` (same wire word) | `+0x10` = `ctrl+7880` | **`0x80030000`** | `0x5593c`, `0x55944`, `0x55948` |
| Data **read** (transcode) | `0x40D120CC0` | `+0x0C` | `ctrl+4576` = wire `0xF810+8*idx` | `+0x10` = `align64(56*N)` | `0x80030001` | `SetTranscode` `0x58714`, `0x5871c`, `0x58724` |
| Data **write** (transcode) | `0x40D130680` | | `ctrl+4584` | | | `0x5872c`, `0x587b8` |

**[C]** for all rows. Every address store is `ldr xN, [ctrl, #off]` followed
by `str wN` of the same register: the low 32 bits go out untouched. There is
no `and #0x7fffffff`, no `bic #0x80000000`, no `lsr`/`ubfx` by 16, 20 or 31
anywhere in `setPipe`/`SetTranscode` (the only right shifts in
`0x52f38..0x58494` are `lsr #32` for the `_MSB` halves of 64-bit recon/ref
addresses at `0x56c54..0x5708c`, `lsr #10` for a lambda table at `0x55ed8`,
and small `lsr #1..#8` on sizes). The whole-image search for `#0x7fffffff` /
`#0x80000000` finds only RTKit internals (`_RTK_mc_release` `0xb7010`,
`_RTK_timer_cancel` `0xb4180`), a HEVC transcode count test (`0x682cc`) and
the `0x80000000` *config words* `ProcessPipeReset` writes (`0x4ef8c`,
`0x4f108`). **[C]**

The FwData pair is reached on the single-core arm (`ctrl+4664 != 2`, `b.ne
0x5590c` at `0x558e8`; `ctrl+4664 = 1` for us per [54](54-assert-map.md)
§3.3) with `cfg = 0x80030000`, i.e. bit 0 clear; the arm that ORs a flag byte
into bit 0 (`ldrb w9,[x9,#1165]; orr` `0x559f4..f8`) is the two-core arm. F24
read back `40d130780: 80030000 ... ff0f0000 00001400` (l.2120). **[C]** So on
our path FwData is programmed but not enabled, and the neighbour readers'
first access would be `Info[0]`/`Pixel[0]`, whose bit-31-dropped pages are
`0x7f000000` and `0x7f050000` - not what the DART reports (§1). **None of
these channels is the stream-15 master.** **[C]**

### 3.2 Other masters, and why they are not it

- **Source picture reader** (`0x40D120000`/`+0x80`): stream 0 - F4 faulted on
  stream 0 walking the input luma ([53](53-first-frame.md) §15), and F21/F24
  show it holding the full `0xfd300000`/`0xfe000000`. **[C]**
- **Recon / MV / entropy / coded writers** (`0x40D130240..0x40D130480`): hold
  full 32-bit IOVAs with bit 31 set in F24 (`fdc00000`, `fe000000`, ...) and
  produce output, so they translate on stream 0 (or on stream 1 remapped to
  0). **[C]**
- **Pipe MCPUs**: their DMem is `0x1000xxxx`, their register window
  `0x40000000 + offset` ([58](58-pipe-start.md) §1.3); `ConfigureMCPUs`
  (`0x60298..0x62400`) loads no 64-bit address field from ctrl (its only
  64-bit loads are the MMIO base `[x8,#1976]`). They have no DRAM pointers to
  dereference. **[C]**
- **The pipe-clock-gating path** is the ASC CPU, not a DMA (§4.3). **[C]**
- **The ASC itself** fetches and does IPC through the CPUDART
  (`0x40d040000`, every earlier CPU-side fault was there); this fault is on
  `0x40d030000`. **[C]**

### 3.3 What is left

Neither binary assigns stream IDs to masters, and the per-channel words
`+0x04/+0x08` are read-only descriptors with no stream field
([69](69-source-to-modedec.md) §1.2). The remaining places a stream tag can
originate are hardware:

1. the **SMMU** at `0x40d020000` in the datapath, which has a locked
   (`+0x20` bit 15) read-only table window `+0x2000..0x3ffc` sized by
   `PARAMS & 0xffff` ([56](56-datapath-dart.md) §3) - a per-master table is
   the natural place for "this AXI ID carries stream 15", and an all-ones
   default tag (`0xf`) for an entry nothing configured would look exactly
   like this; **[I]**, weak;
2. a fixed hardware master that always uses stream 15 because it targets the
   MMIO window of §2.4 (the platform-wide convention).

Both are consistent with every observation, including the one that matters:
**the frame is the same whether stream 15's reads fault (F22-f32), succeed
(f33b/f34) or are dropped (F21, f35).** Which master it is remains **[U]**.
A note on the address: `0x7f100000 = round_up(FwData[0] + 0x1400, 64 KiB)` =
`round_up(Info[0] + 0x1400, 1 MiB)` = `round_up(Pixel[0] + 0x14000, 1 MiB)`
= `arena + 1 MiB`. A truncated *end-of-region* value fits as well as a
truncated base plus 64 KiB, and so does a constant that has nothing to do
with our buffers; the arena-move experiment in §7 is what separates these.

---

## 4. Q2/Q3 side findings that close older questions

### 4.1 No 31-bit field in the firmware

Stated in §3.1: every AVC address programmed by `setPipe`, `SetTranscode`,
`ProcessPipeReset` and `ConfigureMBIFWData` is a 32-bit `str w` of an
unmasked 64-bit load, with `_MSB` registers taking `lsr #32`. There is no
firmware constant `0x7f100000`/`0xff100000`/`0x27f100000` in `__TEXT`,
`__const` or `__DATA` (byte-pattern search). **[C]** If a field is 31 bits
wide it is inside the hardware, and bit 31 is not a flag the firmware sets.

### 4.2 The firmware's own IOVA -> pointer convention is not it either

`MappedMemory::HwToTarget(hw)` (`0x20c00`) returns `hw - bsp_physical_base`
(`ldr x8,[x8,#48]` `0x20c20`, global `0x21c030`, set by
`_bsp_physical_base_set` `0xa550c`). That is the ASC's CPU pointer for a DMA
address, used through the CPUDART; it does not reach the datapath DART. **[C]**

### 4.3 Register bank 3 is the PMGR PS window, DART-mapped only on ChipType <= 5

- `AVE_HwC::Init` calls **`AVE_Reg::DARTMap(3)`** at `0xfffffe0008f0fe90`
  (`mov w1,#3` `0x8f0fe8c`), the function's only caller, guarded by `ldur
  w8,[x29,#-128]; cmp w8,#5; b.gt skip` (`0x8f0fe7c..84`) where `[x29-128]`
  is `w27 = AVE_DevInfo::GetChipType()` (`bl 0x8ede5d4` `0x8f0e268`; `stur
  w27,[x29,#-128]` `0x8f0e2c0`). t6001 is ChipType 7 ([00](00-methodology.md)),
  so on this machine the mapping is **skipped**. **[C]**
- `AVE_Reg::GetDARTAddr(type, off)` (`0x8f2fb44`): `type < 5`; returns
  `[this+136+8*type]->[24] + off`, or `0 + off` when the slot is empty
  (`cbz x8` `0x8f2fb58`). Config `+0x48` is `GetDARTAddr(3, 0)`
  ([46](46-abi-13.5-commands-session.md) row `0x48`), so **macOS 13.5 sends
  `reg_dart_addr = 0` on t6001** - what this driver sends. **[C]**
- Bank 3 = ADT `ave0 reg[3]` = bus `0x8E588000`, 36 bytes, inside
  `/arm-io/pmgr` (`0x8E580000 + 0x80000`; Linux `power-management@28e580000`).
  The driver already reads it as the PS registers of VENC_DMA, PIPE4, PIPE5,
  ME0, ME1 (`driver/ave_session.c:2500-2516`). **[C]** This closes
  [12](12-dart-surfaces-mmio.md) §6 and [01](01-hardware.md)'s "unknown" row.
- Firmware consumer: `ProcessConfig` stores `+0x48` to `this[1368]` (`0xe554`,
  `0xe578`); `ProcessInitStage2` (`0x13e3c`), `AcquireAndTriggerPIPE`
  (`0x7bc8`), `GetStatsEventLRMEFS` (`0xaab0`) and `GetStatsEventTranscode`
  (`0xb484`) pass it as arg 1 of `vt+136`, which in `CFlowController`'s
  vtable (`0xecfc0`) is **`CFlowController::SetPipeClockGating(u64 pmgrAddr,
  bool on)`** (`0x3c8f8`) - off when the pipe is armed, on when its stats are
  collected. The function returns at once unless `this[1359]` is set
  (`ldrb w8,[x0,#1359]; cbz` `0x3c908..90c`), asserts `pmgrAddr != 0`
  (`0xc2e8f`, `CFlowController.cpp:120`) otherwise, converts with
  `HwToTarget` (`x9 = x1 - [0x21c030]` `0x3c9d8`) and **has the ASC CPU write**
  `0x3ff` to `+0`, `+8`, `+16` (off) or `0x304` to `+0..+24` (on)
  (`0x3c9cc..0x3ca38`). No instruction in the image stores to `this[1359]`
  (`ProcessConfig` writes only 1357, 1358, 1368, 1376, 1380 at `0xe54c..0xe578`),
  so pipe clock gating through this window is compiled out of our path.
  **[C]** It is CPU MMIO through a DART mapping, not a DMA master, and not
  stream 15.

---

## 5. Q5 - is stream 15 what feeds intra estimation?

**No**, on three independent grounds:

1. The source-neighbour feed is the Info/Pixel reader pair at
   `0x40D120C00/C40`, fed by the writer pair at `0x40D130600/640`
   ([59](59-row1-stall.md) §2.2). Both carry full 32-bit IOVAs (§3.1) and
   their first access is `Info[0]`, not `Info[0] + 1 MiB`. **[C]**
2. Intra estimation is dispatched by hardware per macroblock and its MCPU
   has no DRAM access ([70](70-intraest.md) §1); the pixels it works on come
   from the source reader on stream 0. **[C]**
3. Measured: f33b and f34 (31-bit mask, stream 15 translating, **zero
   faults** - its reads completed for the first time) coded the same
   2709-byte frame as F22-f32 (faulting) and f35 (stream 15 detached,
   dropped). Three states of stream 15, one output. **[C]** (logs relayed by
   the operator; `results/f33b-*`/`f34-*`/`f35-*` hold only the header lines
   because those runs were captured over netconsole).

What is still unset on the *actual* neighbour path is measurable and cheap:
the only run that checked whether the neighbour **writers** store anything
was F11 (`results/f11-1789492087.kmsg` l.1151/1154: `Info[0]: 0 of 81920
bytes changed`, `Pixel[0]: 0 of 81920 bytes changed`), and F11 stalled at
row 1 before the pipe could have written a row. **No completing run has
re-taken that measurement** (`session_nbr_fill=1` appears in no other log).
If a completing frame leaves both buffers untouched, the pipe's own top-row
neighbour exchange is dead on stream 0, and that - not stream 15 - is the
next thing between the pipe and a non-DC frame. See §7.

---

## 6. Q4 - what the driver should do

**Option (c): leave stream 15 unattached** (overlay `variant=4`: streams 0
and 1 on both DARTs), and keep the **32-bit DMA mask**.

- It is macOS's effective configuration on the datapath DART: TCR[15] =
  `0x20000`, translating nothing (§2.3). Our F21 and f35 runs are that state.
- Option (a), bypass, cannot be built on `0x40d030000`: PARAMS2 bit 0 is
  clear, `apple_dart_domain_alloc_identity` refuses (`apple-dart.c:872`,
  `:1169`), and a bypass window that is MMIO (§2.4) would not carry a DRAM
  buffer anyway. On the CPUDART bypass is possible but that is not the DART
  that faulted.
- Option (b), translating with sub-2 GiB IOVAs, changes nothing in the
  frame (f33b/f34) and only exists to silence a fault whose master does not
  consume our data. With the 32-bit mask restored it also re-introduces the
  three faults per frame, which cost nothing but log lines; if the overlay
  keeps stream 15 for diagnostic reasons, `ave_dart_restore_datapath`
  (`driver/ave_dapf.c:438`) already mirrors its TCR/TTBR from the CPUDART and
  needs no change.
- Document the choice where the overlay declares streams: the comment in
  `dts/t6001-ave.dtsi` (`/* ADT: page-size 16384, sids 32769 */`) should say
  that SID 15 is the platform-wide bypass stream with window 2 (§2.1, §2.4),
  that macOS gives it no translation on this DART, and that it is left out
  on purpose. Nothing else in `driver/` needs to change for stream 15.

The `ave_drv.c` comment block at lines 1005-1030 (the 31-bit mask rationale)
describes the stream-15 engine as the source-neighbour reader; §3.1 and §5
show that identification is wrong, so the mask should go back to 32 bits
with the comment rewritten, independent of the hang the coordinator is
handling.

---

## 7. Proposed hardware checks (operator-run, per AGENTS.md)

**E-S15-1 - does the fault address follow the arena?** (one run, one
variable, can say no). Overlay `variant=5` (so the fault is visible), 32-bit
mask, and move the SrcNeighbor arena: e.g. `session_nbr_kb=96` (slot 96 KiB,
arena 1.5 MiB, so `FwData[0]` and `arena + 1 MiB` both move), or allocate a
1 MiB decoy before the arena. Read the three fault lines.
- Address moves with the arena (to `(new arena + 0x100000) & 0x7fffffff` or
  `(new FwData[0] + 0x10000) & 0x7fffffff`) -> the master is fed a truncated
  arena IOVA; which of the two formulas it follows names the table
  (Info vs FwData), and §3.3's "constant" reading dies.
- Address stays at `0x7f100000` -> it is a constant unrelated to our
  buffers (the MMIO-window master of §2.4); stream 15 is closed as a lead.
Either outcome is decisive; neither can hang the machine differently from
F22-f32, which ran this configuration in every run from F22 to f32.

**E-S15-2 - the neighbour writers on a completing frame** (what §5 says is
actually unmeasured): F24's configuration plus `session_nbr_fill=1`. The
driver already reports how many bytes of `Info[0]`/`Pixel[0]` changed.
`0 of 81920` on a frame that reached 3845 macroblocks means the stream-0
neighbour exchange never stores, and the blank frame has a candidate that
is not stream 15. Any non-zero count retires that candidate.

---

## 8. Not settled

- The identity of the stream-15 master (§3.3) **[U]**; whether the SMMU's
  locked table tags it **[U]**.
- The meaning of DART ERROR bits 19:16 (`0xf` on both stream-15 reads and the
  pre-session stream-0 write) **[U]**.
- The unit of the TCR[25:16] window (4 GiB vs 16 GiB) **[I]**, §2.4.
- What a DART without bypass support does with TCR `0x20000` **[U]**, §2.3.
