# The reset fetch: what the DART actually rejected, and what Linux must do

Static investigation, 2026-09-13. **Nothing here was run on hardware.**
Every claim is marked **confirmed** (read from source, a binary at a cited
address, or a live read-only file), **inferred** (chain stated) or **unknown**.

Question: an iBoot-preloaded AVE firmware with physically split TEXT
(`0x10000b28000`) and DATA (`0x10001a90000`), a hard-locked RVBAR of
`0x0102010000b28001`, and a DART whose page tables cover 38 bits. How does
the reset fetch get satisfied on macOS, and what must Linux do?

Sources used: m1n1 `m1n1-src/` (commit `940439b`), Asahi linux `asahi`
branch (`drivers/iommu/apple-dart.c`, `drivers/media/platform/apple/isp/*`,
fetched 2026-09-13), the restore ADT `data/blobs/adt.bin`, and the **macOS
13.5** kernelcache `data/blobs/macos-13.5/kc.macho` (AppleAVE2 and
AppleT6000DART, both with symbols), which is the release this machine's
coprocessor firmware comes from ([31](31-bringup-state.md), "the dump").

---

## 0. Short answer

1. **The fault was never a translation failure.** Every AVE fault we logged
   reads `status:0x80000800 ... code:0x800 (unknown)`. Bit 11 of the t8020/t6000
   DART error register is **`NO_DAPF_MATCH`** (m1n1
   `proxyclient/m1n1/hw/dart8020.py:10-22`). Linux's decoder has no name for
   it, so it printed `(unknown)` (`apple-dart.c:59-68` defines bits 0-4
   only). **Confirmed.** The DART's **DAPF** (address filter) refused the
   address. The DART never looked for a page table entry: we had a verified
   mapping at IOVA `0xb28000` and still got `NO_DAPF_MATCH`, never
   `NO_TTBR/PMD/PTE`. So "RVBAR modulo the DART width" is refuted.
2. **The firmware's own bootstrap shows its physical address map has two
   windows** (13.5 image, §2.4). **Confirmed:**
   - **TEXT is addressed at its real DRAM address.** The bootstrap takes
     `adr x0, 0` (`0x28c`, MMU off, so this is the physical execution
     address = RVBAR base `0x10000b28000`), and at `0x528`-`0x534` builds its
     TEXT page-table entry from that address.
   - **DATA is addressed at `0x1f0_0000_0000 + DVA`.** At `0x308`-`0x324` it
     loads DATA's base from a literal iBoot patched to `0x1f0000ec000`
     (DATA VA `0xec000` with high word `0x1f0`), and at `0x464`-`0x4ac` puts
     its own page tables inside that DATA and writes `ttbr0_el1`.
3. `0x1f0_0000_0000`-`0x1f0_ffff_ffff` is **exactly DAPF entry 0** in
   `dart-ave0`'s ADT `filter-data-instance-0`, and also in `dart-isp0`'s. That
   4 GiB window matches the DART's 32-bit DVA space (`AS 32`).
   **Inferred:** the window is the DART-translated path. DATA at DVA `0xec000`
   must be mapped by the DART to physical `0x10001a90000`, like ISP's
   `iommu-addresses`.
4. `dart-ave0`'s first instance is the **CPUDART, a DART with a DAPF**
   (`instance` = `DAPF:"CPUDART"`, DAPF block = `reg[3]` = AP `0x40d044000` =
   CPUDART + `0x4000`, the same shape as `dart-isp0`). **Confirmed.** Linux
   never programs a DAPF. In translate mode it leaves `BYPASS_DAPF` clear
   (`apple-dart.c:1543-1545`). m1n1 programs DAPFs only for aop/mtp/pmp/isp
   (`src/dapf.c:173-176`, `kboot.c:2925`). **Confirmed.**
5. **Answer (best supported, H1).** The reset fetch is a *physical* fetch of
   TEXT through the CPUDART's DAPF. The DAPF must admit TEXT's DRAM range
   (`0x10000b28000`, size `0xec000`, the 13.5 TEXT size), and the ADT's two
   entries do not. DATA is reached through the DAPF-admitted `0x1f0...` window
   and translated by the DART at DVA `0xec000`. This is also a sensible
   security split: the kernel controls DART page tables, so it can remap DATA
   (which AppleAVE2 does restore, `RestoreCTRRData`) but cannot redirect
   TEXT. **Inferred.** It explains both hardware results (§4). Every
   ingredient is individually confirmed.
6. **What Linux must do:**
   - (a) program the AVE DAPF as m1n1's `dapf_init_t8020()` does, with the
     ADT entries plus a TEXT entry;
   - (b) give the DART a mapping DVA `0xec000` → `0x10001a90000` for DATA
     (reserved-memory + `iommu-addresses`, as m1n1 does for ISP);
   - (c) keep translate mode, then start the core.

   These are experiments E2/E3 (§6), gated on E1 for ISP comparison.
7. **macOS 13.5's AppleAVE2 never writes RVBAR on t6001** (Castor = type 8,
   Nyx = 9, and 13.5 skips `Config` for iBoot-loaded types > 5, §2.1), and it
   never maps the firmware into a DART: it keeps only `phys`/`size` from
   `segment-ranges` (§2.2). **Confirmed.** Doc 42's conclusion holds for the
   version that matches the running firmware.

---

## 1. The ISP precedent

### 1.1 Nobody on Linux writes ISP's RVBAR

- `isp-regs.h:14` defines `ISP_COPROC_RVBAR 0x1050000`, and **no code uses
  it** (grep across `drivers/media/platform/apple/isp/`). **Confirmed.**
- m1n1 `src/isp.c` powers ISP only to read its version, then powers it off
  again (`isp.c:103-117`). It never touches RVBAR. m1n1's
  `proxyclient/m1n1/hw/isp.py:13` names `ISP_ASC_RVBAR` but nothing uses it.
  **Confirmed.**
- So ISP runs on Linux with **whatever RVBAR iBoot left**, like AVE.
  Whether ISP's RVBAR is locked, and whether its base field is an IOVA or a
  physical address, is **unknown**. It has never been read on this machine.
  Experiment E1 reads it.

### 1.2 How ISP's segments reach IOVA 0 / 0x980000

`dt_reserve_asc_firmware()` (`src/kboot.c:1781-1830`) reads the **live**
ADT `segment-ranges` (`{u64 phys, u64 iova, u64 remap, u32 size, u32 unk}`,
`src/adt.h:78-84`). For each segment it emits an `apple,asc-mem` no-map
reserved-memory node plus `iommu-addresses = <&dev iova size>`, using
`seg->iova | base` for ISP (`remap=false`, base `isp_iova_base()`) and
`seg->remap` for DCP/SIO (`kboot.c:1868, 2239`). `isp_iova_base()` is `0` on
t600x and **`0x10000000000` on t602x/t8122** (`src/isp.c:40-49`). So RTKit
DVAs carrying bit 40 are a real convention on newer SoCs. **Confirmed.**

apple-dart does **not** skip its probe reset for these regions: the DART is
unlocked here (`locked: 0` in this boot's journal for `40d040000` and
`3860e8000`), so `apple_dart_hw_reset()` runs (`apple-dart.c:1418-1423`). The
reserved-region direct mappings are recreated afterwards by the IOMMU core
when the device attaches. **Confirmed** for the reset; the direct-mapping
mechanism is standard `of_iommu_get_resv_regions` behaviour and was not
re-read here.

ISP firmware then boots with no host-side RVBAR write. **Inferred:** ISP's
reset vector therefore resolves through a DVA the DART translates (IOVA 0 is
mapped to TEXT), or the DAPF m1n1 programmed admits it. E1 tells these apart.

### 1.3 apple-isp's power-up sequence (for comparison, not proposed)

`isp_reset_coproc()` (`isp-fw.c:215-262`): `EDPRCR (0x1010310) = 2`; four
fabric registers (`0x738/0x798/0x7f8/0x858`) `= 0xff00ff`; six IRQ masks
`= 0xffffffff`; poll `0x818` and `0x81c` to 0; poll `CPU_STATUS` for the
`IN_WFI` bit. Then `isp_firmware_boot_stage1()` (`:274-330`) does
`CONTROL = 0`, `CONTROL = 0x10` and waits for the firmware to write
`0x8042006` into `ISP_GPIO_7`. **Confirmed.** AppleAVE2 does none of the
preamble ([42](42-asc-firmware-ownership.md) §4 census), so it is not proposed
for AVE. Two things are still worth noting. ISP's first magic flows
**firmware → host**, while our AVE handshake writes `0x08042006` host → firmware
([34](34-boot-handshake.md)). And apple-isp waits for WFI before `RUN`.

---

## 2. macOS 13.5, not 26.6.2: what the kext does

### 2.1 AppleAVE2 13.5 never writes RVBAR on t6001

`AVE_HwC::StartUpIOP` (`0xfffffe0008f119e0`), at `0xfffffe0008f1221c`-`f1224c`:

```
ldr  x0, [x19,#144]      ; AVE_Firmware*
ldrb w8, [x0]            ; iBoot-loaded flag
cbz  w8, do_config       ; not iBoot-loaded -> Config
ldr  x0, [x19,#48]
bl   0xfffffe0008ede5d4  ; IOP type
cmp  w0, #0x5
b.gt skip_config         ; iBoot-loaded AND type > 5 -> skip
do_config: bl AVE_IOP::Config(GetBaseAddr())
```

The `gs_saAVE_IOP_If` table (`0xfffffe0007bc39c8`, 5 slots per type,
decoded from chained fixups) gives type 1 Tyche, 2 Thanatos, 3 Hypnos, 4 Rhea,
5 Cronus, 6 Panda, 7 Acis, **8 Castor**, **9 Nyx**, 10 Atlas, 11 Hera,
12-14 Tethys, 15 Themis. Castor and Nyx are both > 5, so **on t6001 13.5 skips
the only RVBAR writer**, as 26.6.2 does ([42](42-asc-firmware-ownership.md)
§1.4). **Confirmed** (the variant choice itself is from doc 42). The writer
formula is unchanged: `and x8, x20, #0x3fffffff800`, `orr` with
`0x0102000000000000`, `W64(0x50000)` at `0xfffffe0008f1d7xx`.

### 2.2 The kext never maps the firmware into the DART

`AVE_Firmware::RetrieveInfo` (`0xfffffe0008ef4968`) reads `pre-loaded`, then
`segment-ranges`. For each 32-byte entry it stores **only `phys` (+0) and
`size` (+24)** (`0xfffffe0008ef4d30`-`4d40`). It requires 1-2 segments, and
for 2 requires both sizes non-zero with a sum ≤ `0x400000`
(`0xfffffe0008ef4d5c`-`4d74`). **The `iova` and `remap` fields are never
read.** **Confirmed.** So on macOS the host kext does not put AVE's firmware
into any DART address space. Whatever makes the reset fetch work is set up
below the kext.

### 2.3 AppleT6000DART 13.5: DAPF, remap, and privileged restore

- `_apfSetupInstance` (`0xfffffe0009b13ca0`) parses
  `filter-data-instance-%d` as 24-byte entries: count = len/24, ≤ 17
  (`cmp w0,#0x197`). It stores start (+0), end (+8), `u16 @+20`, and a
  permission word built from bytes 16-18 (`0xfffffe0009b13eac`-`3ef0`). There
  are no extra ranges in that function. **Confirmed.** Whether anything
  appends firmware ranges later is **unknown**.
- `remap` (`0xfffffe0009b13418`-`13544`) is an array of 4-byte entries
  `{u8 from_sid, u8 to_sid, ...}`, both < 16, and it panics with
  `"remap of%s SID %u to%s SID %u"` if either SID is bypassed. `dart-ave0`'s
  `remap = 1` is therefore **SID 1 → SID 0**, not a bypass. `dart-dcp`'s `5`
  is SID 5 → 0, `dart-usb*`'s `0x100` is SID 0 → 1. **Confirmed** for the
  parse; which master uses SID 1 is **unknown**. Linux never programs the
  remap registers (`0x80`-`0x8c`).
- `_recoverFromPowerDown` (`0xfffffe0009b15edc`) makes a privileged call
  (`bl 0xfffffe000855dd00`, `w1 = 0x6007`) and then **verifies TTBRs against
  saved values** (`_DART_TTBR` loop, `0xfffffe0009b15fa8`-`5fe8`). DART state
  on macOS is restored after power-down by a layer below the kext.
  **Confirmed** for the call; its semantics (PPL/monitor, and whether it
  restores DAPF) are **unknown**.

So on macOS, DART and DAPF state for a power-gated coprocessor is actively
restored on power-up. On Linux, for an unlocked DART, apple-dart resets
TCR/TTBR and nothing ever touches the DAPF. **Inferred** from the above.

---

### 2.4 The 13.5 firmware bootstrap: TEXT physical, DATA in the `0x1f0` window

Image `data/blobs/macos-13.5/ave_h13c.bin`, TEXT VA 0 size `0xec000`, DATA
VA `0xec000` size `0x134000`, `RTSZ = 0x220000`
([43](43-macos-13.5-firmware.md)). The running dump matches its TEXT except
4 bytes at `0x423c`. Disassembled with
`AVE_MACOS=13.5 tools/disas.py --fw --addr 0x200 -n 0x400`:

```
204  mrs x1, currentel ; cmp 3 ; eret to EL1       ; reset entry (b +0x204 at 0)
230  adr x0, 0x0 ; msr vbar_el1, x0
28c  adr x0, 0x0 ; mov x23, x0                    ; x23 = physical TEXT base (MMU off)
294  ldr x1, =0 (lit 0x898) ; and #0xfffffffff ; sub x0,x0,x1 ; mov x9,x0
308  adr x0, 0x423c ; ldp w0, w1, [x0]
310  add x0, x0, x1, lsl #32                     ; x0 = iBoot-patched DATA base
314  cbnz -> 324 ; else adrp x0, 0xec000         ; fallback: DATA contiguous with TEXT
324  mov x22, x0                                  ; x22 = 0x1f0000ec000 on this machine
448  adrp x1, 0x104000 ; ... adrp x2, 0xec000 ; sub x1,x1,x2 ; add x20,x1,x22
                                                  ; page tables at DATA + 0x18000
4a4  msr ttbr0_el1, x2   (or ttbr1_el1 at 4ac)
528  mov x1, x23 ; ubfx x1,#14,#28 ; bfi x4,x1,#14,#28 ; str x4,[x0],#8
                                                  ; TEXT PTE = physical TEXT PFN
548  adr x0, 0x4000 ; sub x0,x0,x23 ; add x0,x0,x13 ; msr vbar_el1 ; ... msr sctlr_el1
```

The patched structure at `0x4230` in the live dump reads `0x1c8, 0xec000, 0,
0xec000, 0x1f0, 0xeb5dc, 0, 0xeea23`. `0xeb5dc` is the VA of the
`RTKit-2062.141.1.release` string (dump `0x10000c135dc`), so this is a
firmware info block. The only field iBoot filled is the 64-bit DATA base at
`0x423c` = `0x1f0000ec000`. **Confirmed.**

TEXT's DRAM extent from the dump (`0x10000b2c000`-`0x10000c15000` non-zero)
agrees with `0xec000` from `0x10000b28000` to within one 4 KiB page.

> **Update (2026-09-13, [49](49-dapf-write-reset.md)).** E3a showed that
> programming this DAPF from Linux *after* apple-dart has reset and enabled
> the DART (overlay `variant=3`) resets the machine on the first write. The
> block identity is not in doubt — Apple's own `pmap-io-ranges` tags
> `0x40d044000` as `'DAPF'` — but E3's "keep translation, then program the
> DAPF" order is the opposite of every known-good DAPF write (m1n1 writes
> DAPFs before any DART is configured, and its `aop.py` programs the DAPF
> before `dart.initialize()`). The E3 plan below stands as the *goal*; the
> *mechanism* for installing the entries is revised in docs/49 §6 (N1: quiesce
> the DART, write in m1n1's order, restore; N3: have m1n1 do it at boot).

## 3. dart-ave0 in the ADT, decoded (restore tree)

| property | dart-ave0 | dart-isp0 | meaning |
|---|---|---|---|
| `instance` | DAPF"CPUDART", DART"DART", SMMU"SMMU" | DAPF"DARTLLT", DART"DARTBULK", DART"DARTRT", SMMU×2 | a DAPF-type instance is a DART with a DAPF at +0x4000, listed after the SMMUs in `reg` |
| `reg` | `20D040000`, `20D030000`, `20D020000`, `20D044000` | 6 entries, DAPF = `reg[5]` = `1860EC000` | AVE DAPF = `reg[3]` |
| `sids` / `bypass` | `0x8001` / `0x8000` | same | SID 15 bypassed |
| `remap` | `1` (SID 1 → 0) | absent | §2.3 |
| `dart-options` | 37 | 45 | unknown bits |
| `bypass-address` | 32 bytes, last u16 = `0x0200` | same | only consulted when full bypass is unsupported (26.6.2 string at `0xfffffe00076a5843`); CPUDART supports it |
| `vm-base` | 0 | 0 | — |
| `filter-data-instance-0` | **2 entries** | 14 entries | below |

`filter-data-instance-0` for `dart-ave0`, decoded with m1n1's
`dapf_t8020_config` layout (`src/dapf.c:12-20`):

| # | start | end | r0 (hi<<4\|lo) | r4 |
|---|---|---|---|---|
| 0 | `0x1f000000000` | `0x1f0ffffffff` | `0x33` | 1 |
| 1 | `0x506000000` | `0x507c6c000` | `0x31` | 1 |

`dart-isp0`'s list opens with the **same** `0x1f000000000`-`0x1f0ffffffff`
entry, followed by small MMIO windows (`0x28e584000`, `0x28e0b8000`,
`0x39a000000`, …). **Confirmed** decode. Meanings **unknown**: what
`0x1f0_0000_0000` is, what `0x506000000`-`0x507c6c000` is, the r0 bits,
whether `end` is inclusive. Neither AVE entry covers DRAM, so **the restore
ADT's filter would not admit `0x10000b28200` either**. Either the live ADT
carries more entries (iBoot injects `segment-ranges`, so it could inject these
too), or the firmware is admitted by something else. **Unknown**, and E2 is
the read that settles what is programmed now.

> **Addendum (coordinator, 2026-09-13): the second entry is the *other*
> instance's MMIO.** Walking the 13.5 ADT properly and applying `/arm-io`
> `ranges` (bus `0x0` → AP `0x200000000`): `ave0` is at bus `0x20d…` = AP
> `0x40d…` (DART `0x40d040000`, the one we use), `ave1` at bus `0x307…` = AP
> `0x507…`. `dart-ave0`'s entry 1, `0x506000000`-`0x507c6c000`, spans
> **ave1**'s fabric (`0x506000000`) through its ASC; `dart-ave1`'s entry 1 is
> `0x40d050000`-`0x40dc69000`, **ave0**'s SVE through ASC. The two filter lists
> are cross-wired relative to node names (identical in the 26.6.2 and 13.5
> ADTs). **Confirmed** as data; **unknown** whether Apple's DART driver pairs
> them crosswise, whether the ADT is simply mislabelled, or whether ave0's core
> is meant to reach ave1. Consequence for E3: do not copy "the ADT entries"
> blindly. Program the `0x1f0` window, the TEXT entry, and ave0's own MMIO
> window (`0x40d050000`-`0x40dc69000`, i.e. `dart-ave1`'s entry), and treat
> which MMIO entry is correct as a variable to test, not a given.

`0x29209c000` = `error-reflector` (bus) on both, for the record.

---

## 4. Hypotheses

**Hardware facts to reconcile** ([31](31-bringup-state.md),
[41](41-apple-fetch-path.md)):

- (a) DMA domain: `NO_DAPF_MATCH` at `0x10000b28200` (mostly) and `+0x280`,
  stream 0, at ~70k/s, with IOVA `0xb28000` verifiably mapped.
- (b) Identity domain (`TCR = BYPASS_DAPF|BYPASS_DART`): no faults, no page
  of the 16 MiB window changed, `CPU_STATUS 0x2c`.
- (c) Started core: `0x2c` throughout, never `RUNNING`, where DCP's live core
  reads `0x2d`.
- (d) RVBAR is locked, survives power gating, and holds the physical TEXT
  address. The tag lists iBoot filled use AP-physical addresses.

| # | hypothesis | for | against | verdict |
|---|---|---|---|---|
| **H1** | Two windows: TEXT fetched physically from DRAM through the CPUDART's DAPF, which must admit TEXT's range; DATA via the DAPF-admitted `0x1f0...` window, translated by the DART at DVA `0xec000`. macOS/iBoot provide both; Linux provides neither | the bootstrap (§2.4) addresses TEXT physically and DATA at `0x1f0000ec000`; DAPF entry 0 is exactly that window; (a) is `NO_DAPF_MATCH` at `+0x0/+0x200` (physical TEXT not admitted); **(b) is now explained**: in bypass the TEXT fetch passes, then DATA at `0x1f0000ec000` goes out untranslated to no memory, so the core aborts with nothing for the DART to report and writes nothing, matching the no-change result and `0x2c` | the restore-ADT filter has no TEXT entry, so where macOS's TEXT admission comes from is unknown (live ADT, iBoot-programmed and lost to gating, or the privileged restore in §2.3) | **best supported** |
| H2 | `0x10000b28000` is a DVA with bit 40 as a window flag (cf. t602x `isp_iova_base`) | t602x ISP precedent | (a) with `0xb28000` mapped; the bootstrap uses `0x1f0`, not `0x100`, as its DVA window | **refuted** on t6000 |
| H3 | iBoot configured the DAPF/DART for AVE, and VENC gating before Linux lost it. macOS restores on power-up (§2.3), Linux does not | `venc_sys` off on a fresh boot; macOS explicit restore | ISP working after m1n1 programs its DAPF suggests DAPF state survives gating | **compatible with H1**. It is about who admits TEXT, not the mechanism. E2 decides |
| H4 | RVBAR interpreted modulo the DART input width | — | (a) with IOVA `0xb28000` mapped | **refuted** |
| H5 | `remap` routes the fetch to bypassed SID 15 | Linux never programs remap | `remap = 1` is SID 1→0, and the kext panics on remapping bypassed SIDs (§2.3) | **refuted** |
| H6 | `bypass-address` offsets a bypass window | property present | only used without full-bypass support; CPUDART has it; the fetch is SID 0 | **refuted** |

## 5. Side item: the coprocessor at CpAd `0x285000000`

`/arm-io/ane0` has `reg` at bus `0x84000000`, size `0x2000000`, i.e. AP
`0x284000000`-`0x285ffffff`. CpAd `0x285000000` = base + `0x1000000`, and WrAd
`0x285400000` = base + `0x1400000`, the same offsets as ISP (`coproc +
0x1400044` is CONTROL, `isp-regs.h:16`). **ANE**, the neural engine.
**Confirmed** by address containment. The same arithmetic pins the rule for
every ASC seen so far: RVBAR = CpAd + `0x50000` (ISP `0x1050000`; AVE
`0x40d800000 + 0x50000`; ANE `ane.py:12` `0x1050000`).

---

## 6. Next experiments (ranked). None of these has been run.

Common to all:

- Machine state on 2026-09-13: the kernel disabled the AVE DART IRQ on this
  boot, so **E2 and E3 need a fresh boot**. E1 does not touch AVE.
- Nothing may be installed. Load modules from the tree only.
- Recovery without a console is a reboot. Every step below is RAM-only.

### E1 — Read ISP's RVBAR while ISP is running (read-only, no AVE)

**Why first:** it decides whether a physical RVBAR is normal for an ASC that
demonstrably works on Linux.

**Change:** a throwaway module that `ioremap`s AP `0x385050000` (8 bytes,
64-bit RVBAR), `0x385400044` (CONTROL) and `0x385400048` (STATUS), plus the
ISP DAPF block at AP `0x3860ec000` (`0x400` bytes, the first 16 entries at
`0x40` spacing). It reads each once and prints. No writes.

**Precondition, checked by the operator's script immediately before insmod:**
`/sys/bus/platform/devices/384000000.isp/power/runtime_status` reads
`active`, which means the camera is streaming (e.g. a `ffmpeg -f v4l2 -i
/dev/video0 -f null -` left running in another terminal). Re-check after.

**Reading it** (H1 predicts the second or third case for ISP, by symmetry
with AVE's two windows):
- RVBAR base `0x1f000000000`, i.e. the DVA window plus IOVA 0 → ISP boots
  TEXT *through the DART* (m1n1 maps ISP TEXT at IOVA 0), and AVE's physical
  TEXT fetch is a deliberate difference. The DAPF TEXT entry in E3 is then
  AVE-specific.
- RVBAR base = `0x10000c68000` (ISP TEXT physical) → ISP boots TEXT
  physically too, and something on Linux admits it. The ISP DAPF dump then
  shows the admitting entry, and AVE needs the same kind of entry.
- RVBAR base ~`0` → neither window. The model in §0 is incomplete for ISP.
- The DAPF dump should reproduce `dart-isp0`'s 14 ADT entries as m1n1 wrote
  them. That validates the register layout used in E2/E3, and shows whether
  they survive ISP's power cycling (bearing on H3).

**Negative control:** STATUS must read with `RUNNING` set (as DCP did). If it
does not, ISP was not actually up and the RVBAR read is not meaningful.

**Risk:** low. It reads another driver's MMIO while its domain is on. If the
camera stops mid-read, the domain may gate and the read can hang the fabric
(reboot recovers). Mitigate by keeping the stream running for the whole load
and not reading in a loop. No persistence.

### E2 — Dump AVE's CPUDART and DAPF state before apple-dart touches it (read-only)

**Change:** on a **fresh boot**, apply the `variant=1` overlay (no DART
node, so apple-dart never binds and never resets) with two extra `reg`
entries, and have the driver read them at stage 6-8 **without starting the
core**:

```dts
/* added to ave0 in ave-overlay-noiommu.dts (draft, not created) */
reg = <0x4 0x0d100000 0x0 0x45c000>,
      <0x4 0x0d800000 0x0 0x800000>,
      <0x4 0x0d050000 0x0 0x008000>,
      <0x2 0x8e588000 0x0 0x000024>,
      <0x4 0x0c000000 0x0 0x1000000>,
      <0x4 0x0d040000 0x0 0x4000>,     /* cpudart */
      <0x4 0x0d044000 0x0 0x4000>;     /* dapf    */
reg-names = "dpe", "asc", "sve", "unk3", "axi2af", "cpudart", "dapf";
```

Driver: `insmod apple-ave.ko stop_after=8` plus a dump at the end of stage 8
of `cpudart+0x00..0x2fc` (PARAMS, ERROR, CONFIG lock bit 15, REMAP
`0x80-0x8c`, `0xf0` DAPF_LOCK, `0xfc` ENABLED_STREAMS, TCR[16], TTBR[16×4])
and `dapf+0x00..0x3ff` (16 entries × `{+0 r0, +4 r4, +8 start, +0x10 end}`).

**Reading it:** do any DAPF entries exist? Is entry 0 the `0x1f0` window,
and is there an entry admitting TEXT at `0x10000b28000`? If iBoot's TEXT entry
is present and survived gating, H3 is refuted and something else strips it.
If it is absent, E3 must add it. What are TCR[0]/TCR[1]/TCR[15], and are
TTBRs valid (iBoot state surviving gating)? What is REMAP[1]?

**Control:** the same dump on a `variant=0` boot, after apple-dart's reset
(expect TCR/TTBR cleared and DAPF identical, since Linux does not touch it).
E1's ISP DAPF read is the positive control for the DAPF layout.

**Risk:** low to moderate. The DAPF block is in the ADT `reg` list and its
t6000 layout is the one m1n1 writes for ISP (`dapf.c:150-151`), but reads of
it have never been done on AVE. Reads happen only with `venc_sys` on (stage 6
holds the runtime-PM reference). **The core is not started**, so the missing
IOMMU backstop of `variant=1` is irrelevant. Stop before stage 9 (IPC DMA
allocation would otherwise run without an IOMMU). No persistence.

### E3 — Admit the firmware through the DAPF, keep translation, start the core

**Only after E1 and E2**, and only if they support H1.

**Change:** `variant=0` overlay plus a `dapf` `reg` entry as in E2 (the
DART stays bound in translate mode, so `BYPASS_DAPF` stays clear and the DAPF
is the backstop). Before `ave_asc_start`, program the DAPF exactly as
`dapf_init_t8020()` does (`src/dapf.c:35-41`: per entry `+0x04 = r4`,
`+0x08 = start`, `+0x10 = end`, then `+0x00 = r0`, next entry `+0x40`):

1. the two ADT entries (§3), verbatim. Entry 0 is the `0x1f0` DVA window
   the bootstrap uses for DATA;
2. AVE TEXT, physical: `0x10000b28000` - `0x10000c14000` (13.5 TEXT size
   `0xec000`; end encoding follows ADT entry 0's `...ffff` form, so
   `0x10000c13fff`), `r0 = 0x33`, `r4 = 1`. This stays clear of the GPU DATA
   near `0x10000c65000` ([31](31-bringup-state.md)).

And give the DART the DATA mapping the bootstrap will use (DVA `0xec000` →
physical `0x10001a90000`), the same way m1n1 describes ISP's segments. Draft
overlay fragment (not created):

```dts
/ { fragment@1 { target-path = "/reserved-memory"; __overlay__ {
      ave_fw_data: asc-firmware@10001a90000 {
          compatible = "apple,asc-mem";
          reg = <0x100 0x01a90000 0x0 0x134000>;   /* DATA VM size; physical
                                                       extent unverified */
          no-map;
          iommu-addresses = <&ave0 0x0 0x000ec000 0x0 0x134000>;
      };
}; }; };
/* and on ave0: memory-region = <&ave_fw_data>; */
```

The size is DATA's VM size. Only `0x16000` of it is non-zero in the dump
(the bootstrap puts its page tables at DATA + `0x18000`, so the segment is
certainly larger). Whether `0x10001a90000 + 0x134000` = `0x10001bc4000` is
all AVE's is **unknown**. It is below ISP's DATA carve-out at
`0x10001d70000`, and the dump shows one unexplained non-zero page at
`0x10001af0000`. Alternatively, map it from the driver with `iommu_map()` at
IOVA `0xec000` (as `ave_fw.c` already maps our own image). That avoids adding
a reserved-memory node to the overlay. The range is outside Linux's `/memory`
node, so nothing in Linux can have allocated it either way.
Then start the core with the existing liveness sampling.

**Reading it (H1 predictions):** `NO_DAPF_MATCH` at `+0x0/+0x200`
disappears. If the DATA mapping is wrong, the next faults name addresses
`0x1f0000ecxxx` (window reached) or report `NO_PTE` at DVA `0xecxxx`, which
pins the window convention.
Either `CPU_STATUS` shows `RUNNING` and scratch or IRQ activity follows, or
**new faults at new addresses** appear (BSS/heap outside the admitted ranges,
or MMIO). Each fault then names the next range the firmware needs, which the
bypass experiment could never show.

**Negative control:** the same run with step 1 only (ADT entries, no
firmware ranges) must still fault `NO_DAPF_MATCH` at `+0x200`. If it stops
faulting, the DAPF writes are not doing what we think.

**Risk: moderate.** These are writes to a filter block whose meaning is
inferred (r0 bits and end inclusivity unknown). A bad write can at worst hang
the fabric; a reboot recovers. If the firmware runs, it can write **only**
inside the admitted ranges (iBoot's own TEXT/DATA copies, re-created by iBoot
on the next boot) plus the ADT MMIO windows. Its translated DMA still goes
through the DART. That is a tighter backstop than the identity domain was.
It still inherits the known post-power-off DART IRQ hazard
([31](31-bringup-state.md)). No persistence.

### E4 — (deprioritised) persistent alternatives

- Adding `{"/arm-io/dart-ave0", 3}` to m1n1's `dapf_entries[]`
  (`src/dapf.c:173-176`) would program only the ADT entries, which do not
  cover the firmware. By itself it would not test H1, and it changes the
  installed m1n1 stage 2 on the ESP, which **is** a bootability risk (recover
  from macOS by restoring `m1n1/boot.bin`). Not recommended until E3 shows
  what entries are needed.
- Retrying the identity domain: uninformative. H1 already explains its
  outcome (the TEXT fetch passes, then DATA at `0x1f0000ec000` goes out
  untranslated), and bypass suppresses the very fault reports that would show
  progress.

---

## 7. Corrections this makes

- [42](42-asc-firmware-ownership.md) §3.4 ("the address is a device address
  the DART is expected to translate"): **half right.** DATA is translated
  (`0x1f0` window, DVA `0xec000`), but TEXT, and so the reset vector, is
  deliberately physical.
- [31](31-bringup-state.md) "the dump" row for list `0x1000165eaa0`
  ("unidentified"): it is **ANE** (§5).

- [41](41-apple-fetch-path.md) §3 and its hardware-verification row "A 41-bit
  fetch cannot be translated". The arithmetic is right, but the DART never
  claimed a translation failure: the logged code `0x800` is `NO_DAPF_MATCH`.
  The obstacle is the address filter, not page-table reach.
- [31](31-bringup-state.md) "the fetch cannot be satisfied by translation" /
  "untranslatable": same correction.
- [04](04-roadmap.md) phase 3 blocker text ("a 38-bit DART cannot translate")
  is restated in terms of the DAPF.
