# Which translation block the AVE datapath uses, and what raised AIC 1028

Static analysis prompted by the first real encode on hardware,
`results/f3-1789384805.kmsg` (2026-09-14): Config/Open/Start_AVC/Process were
accepted, LRMEFS/LRMERC/Pipe/xcode started, then the firmware logged
`Uncompress Ref is not supported` (kmsg l.1000), 12x `AXI Error: 0x0 0x1 0x0 0x0`
and 7x `0x3 0x1 0x0 0x0` (l.1003-1057), LRME/PIPE heartbeat hangs (l.1060,
l.1108), and IRQ 130 (AIC 1028, both `apple_dart_irq` handlers) was disabled as
unhandled (l.1138-1165) with no `translation fault` line.

Everything is read from the **macOS 13.5** blobs. Reproduce with:

```sh
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr <VA> -n <LEN>   # any kernelcache VA
AVE_MACOS=13.5 python3 tools/disas.py --fw   --addr <VA> -n <LEN>   # firmware image VA
python3 tools/kext_extract.py data/blobs/macos-13.5/kc.macho \
        --symbols com.apple.driver.AppleT6000DART -o /tmp/t6000.syms
```

Labels per [00](00-methodology.md): **[C]** read from an instruction (VA cited),
**[I]** inferred (chain stated), **[U]** unknown. **Nothing here was run.**

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| Who programs the AVE DART complex on macOS | The kext `AppleT6000DART` only parses the ADT and forwards it; **all register programming is done by PPL's `t6000dart` IOMMU driver** in the kernel (`_t6000dart_get_desc` `0xfffffe000857ffd0`) | **C** |
| Same page tables on CPUDART and DART? | **Yes.** Every TTBR write loops over *all* DART-type instances with one per-SID table PA; TCRs, REMAP and TLB flushes are likewise per-instance copies of one config | **C** |
| What the SMMU instance gets | **No TTBR, no TCR, no page table, no REMAP.** At most one tunable at `+0x20` (CONFIG), error-status W1C, perf counters | **C** |
| Datapath instance | `DART` @ `0x40d030000` (the non-`CPUDART` DART), with the SMMU @ `0x40d020000` in its path | **I** (naming + ISP/DISP pattern; no static proof of bus wiring) |
| Stream IDs | ADT `sids = 0x8001` (SID 0 translated, SID 15 bypassed), **`remap = 1`: stream 1 uses SID 0's context**, programmed into REMAP `+0x80` on both DARTs | **C** (code) / **I** (value arithmetic) |
| Who shares AIC 1028 | All four blocks' fault/perf lines: CPUDART, DART, **SMMU**; the kext polls `DART+0x40` bit 31, `DART+0x100c`, `SMMU+0x40` bit 31, `SMMU+0x1008` bit 20 on the one interrupt | **C** |
| Why `apple_dart_irq` returned `IRQ_NONE` | It only tests `+0x40` bit 31 of its own DART; an SMMU fault (`0x40d020040` bit 31) or a perf-status bit has no Linux handler | **C** (Linux + kext) / **I** (that the SMMU was the source) |
| `AXI Error` values | Raw reads of AVE registers `0x40d120000`, `0x40d120004`, `0x40d130000`, `0x40d128000`; bit meanings unknown | **C** / **U** |
| `Uncompress Ref is not supported` | Non-fatal log in `CAVCController::setPipe`; printed when two firmware flags are both 0; execution continues | **C** / **U** (flag meaning) |
| Linux change | `iommus` must name **both** DARTs, and should also carry **SID 1**; do **not** add an SMMU node | **I** |

---

## 1. Structure: the kext parses, PPL programs

### 1.1 Instance decode (kext)

- `AppleT6000DART::start` (`0xfffffe0009b118e8`) walks the ADT `instance`
  records (12 bytes each, `0x9b11bcc`): tag `DAPF` (`0x44415046`, `0x9b11b14`)
  creates a DART instance **and** an APF (`[this+5360]++`, `[this+5560]++`,
  `0x9b11b74..b90`); `DART` (`0x9b11b30`) creates a DART instance (max 3,
  `0x9b11b5c`); `SMMU` (`0x9b11b24`) creates an SMMU instance (max 2,
  `0x9b11bb0`). **[C]**
- The `reg` index of a DART/SMMU instance is its record index (`ldr w2,[x23,x22,lsl#2]`
  at `0x9b11c84` / `0x9b11d04`, handed to `getDeviceMemoryWithIndex` in
  `_dartSetupInstance` `0x9b13a1c` / `_smmuSetupInstance` `0x9b140c0`); APFs
  take indices after DARTs+SMMUs (`w12 = dart+smmu`, `0x9b11c00`). `_setup`
  requires `#reg == DARTs + SMMUs + APFs` (`0x9b122dc..f4`). **[C]**
- For `dart-ave0` (`FPAD"CPUDART"`, `TRAD"DART"`, `UMMS"SMMU"`) this gives:

  | kext/PPL object | reg | AP-phys | Linux today |
  |---|---|---|---|
  | DART instance 0 (`CPUDART`) | reg[0] | `0x40d040000` | `dart_ave0_0`, in `iommus` |
  | DART instance 1 (`DART`) | reg[1] | `0x40d030000` | `dart_ave0_1`, **not** in `iommus` |
  | SMMU instance 0 (`SMMU`) | reg[2] | `0x40d020000` | absent |
  | APF 0 of DART 0 | reg[3] | `0x40d044000` | `ave0` reg "dapf" |

  **[C]** (code above + ADT).

### 1.2 Hand-off to PPL

- `_dartSetupInstance` stores the mapped VA at `init + i*0x1a0 + 24` (`0x9b13a6c`)
  and its `dart-tunables-instance-%d` (`0x9b13b68`); `_smmuSetupInstance` stores
  the SMMU VA at `init + 1272` (`0x9b14110`) and `smmu-tunables-instance-%d`
  (`0x9b14208`). `start` calls `_pmap_iommu_init` (`0x9b11e10`). **[C]**
- `_t6000dart_get_desc` returns descriptor `0xfffffe0007b1c048` (name
  `"t6000dart"`), whose six ops (chained-fixup targets, KC base +
  low 32 bits) are **init** `0xfffffe0008bef028`, iovmalloc `…8bf0880`,
  **map** `…8bf0ea4`, unmap `…8bf1c3c` (`b map` with x6=0), iovmfree `…8bf0ba4`,
  **ioctl** `…8bf011c`. init is confirmed by its size/version check
  (`0x8bef054`: size `0x718`, magic `0x6000000000200000`); ioctl by its
  `0x6000..0x600a` jump table (`0x8bf0138`); map/iovmfree by their panic
  strings (`0x8bf18bc`, `0x8bf0e5c`). iovmalloc is **[I]** by table order.
- PPL copies the instance VAs: DARTs into `[st + 928 + i*0x300]`, count
  `[st+3336]` (`0x8bef38c`, `0x8bef458/470`); APF into `[st + 936 + i*0x300]`
  (`0x8bef4b0`); SMMUs into `[st + 3232 + j*0x20]`, count `[st+3340]`
  (`0x8bef550`, `0x8bef68c`). Plain MMIO write stubs: DART `0x857ffb4`, APF
  `0x857ff98`, SMMU `0x857ff7c`. **[C]**
- Order on power-up: `_powerUp` issues ioctl `0x6008` (compute/validate,
  `0x9b14ec4`) then calls `_recoverFromPowerDown` (`0x9b14ef8`), which issues
  ioctl `0x6007` (`0x9b15f2c`). The `0x6007` handler (`0x8bf044c`) powers up
  **SMMU 0, other SMMUs, then DART 0, other DARTs** (`0x8bf0474`, `0x8bf0678`,
  `0x8bf06d0`, `0x8bf0728`). **[C]**

---

## 2. Q1 — one IOVA space on both DARTs

### 2.1 Page tables

- **Per-SID tables are global, not per instance.** The L1 table for SID *s*
  lives at `[st + s*0x38 + 32]` (read in DART power-up `0x8beead4`, allocated
  in iovmalloc `0x8bf0964/0x8bf0aac`). **[C]**
- **TTBR writes fan out to every DART instance.** `0x8bf1c4c` takes
  (sid, PA, valid), forms `TTBR = (PA>>12 & 0x3ffffffc) | valid<<31`
  (`0x8bf1c98..ca0`), and loops `i = 0 .. [st+3336]` calling `0x8bf220c`
  (`0x8bf1ca4..cc4`), which writes `reg_i + 0x200 + sid<<4` (`0x8bf2230..34`,
  `0x8bf22a0`). iovmalloc calls it when it installs a new L1 table
  (`0x8bf0acc`). **[C]**
- DART power-up (`0x8bee928`, per instance) rewrites the same TTBRs from the
  shared table for every SID in `sids` (`0x8beeab0..eaf8`). **[C]**
- TLB flush helper `0x8bf20f0` is run for each DART instance by the loop at
  `0x8bf00a8..c4`, and by power-up (`0x8beed20`). **[C]**
- **Consequence:** on macOS the AVE mapper's IOVAs are valid on CPUDART and on
  DART alike, for the same SID. `ave0`'s `iommu-parent` is 320 =
  `mapper-ave0`, `reg = 0` (13.5 ADT), i.e. one mapper on SID 0. **[C]**

### 2.2 TCR, stream enable, remap (per DART instance, same values)

From DART power-up `0x8bee928` and the config pass `0x8befc74` (ioctl `0x6008`):

| Step | Register | Value for AVE | VA |
|---|---|---|---|
| 1 | REMAP `+0x80,+0x84,+0x88,+0x8c` | table `[st+3296..3308]`, only if `remap` present | `0x8beea34..ea98` |
| 2 | ENABLED_STREAMS `+0xfc` | saved value (bits then set per SID by ioctl `0x6005`/`0x6004`, `0x8beef24`/`0x8beeedc`, from `enableTranslation` `0x9b14d68`) | `0x8beeaac` |
| 3 | TTBR `+0x200+sid<<4` | shared L1 PA | `0x8beeaf4` |
| 4 | STREAM_SELECT `+0x34` = `0xffff`, STREAM_COMMAND `+0x20` = 0 | | `0x8beeb04`, `0x8beeb0c` |
| 5 | `dart-tunables-instance-N` (offset `0x60` OR'd with `[st+3356]`) | live-ADT only | `0x8beeb4c..eba4` |
| 6 | **DAPF slots**: `+4, +8, +0xc, +0x10, +0x14`, then `+0` | `filter-data-instance-0` | `0x8beebe8..ec8c` |
| 7 | perf counters `+0x1000..` | only with `enable-perf-counters` (AVE: absent) | `0x8beeca8` |
| 8 | TLB flush | | `0x8beed20` |
| 9 | TCR `+0x100+sid<<2` | saved `[inst+1624+sid*4]` | `0x8beed64..ed7c` |

**[C]** for the sequence. TCR values from `0x8befc74`: a translated SID gets
`0x80` (`TRANSLATE_ENABLE`, `0x8befe8c`) plus `0x1000` only if the SID is in
`apf-bypass` (`0x8befe9c..ea8`; AVE has none) → **TCR[0] = `0x80` on both
DARTs**, which is exactly Linux's `tcr_enabled`. A bypassed SID gets
`bypass-address[sid] << 16` and `0x100` only if `PARAMS2` bit 0 (`0x8befe50`,
`0x8befed8`). **[C]** For SID 15 that is `0x20100` on CPUDART and `0x20000` on
DART (ADT `bypass-address[15] = 2`, bypass support 1/0 per docs/overlay) **[I]**.

Aside for [49](49-dapf-write-reset.md) (annotation, not a retraction): §4 there
says the DAPF write order on macOS is unknown. It is step 6 above: macOS writes
the DAPF **after** ENABLED_STREAMS and TTBRs but **before** the TCRs and the TLB
flush, in the order r4, start_lo, start_hi, end_lo, end_hi, r0, via the plain
stub `0x857ff98` when unlocked (verify-only `0x8bf1f18` when `DART+0xf0` bit 0
is set, `0x8beef34`). **[C]**

### 2.3 SIDs and the remap

- ADT `dart-ave0`: `sids = 0x8001`, `bypass = 0x8000`, `remap = 1`,
  `dart-options = 0x25`, `bypass-address` = 32 bytes, last u16 = 2. **[C]**
- `remap` is parsed as 4-byte entries, byte 0 = source, byte 1 = destination
  (`0x9b1346c..b3544`, stored at init `+1704` count / `+1708..` pairs); PPL
  builds an identity table and sets `table[src] = dst` (`0x8bef2c8..f324`),
  rejecting self-remaps and bypassed SIDs (`0x8bef2f0..f320`). **[C]**
- For AVE: source 1, destination 0 → REMAP `+0x80` = bytes `00 00 02 03` =
  **`0x03020000`**, `+0x84..+0x8c` identity; written on **both** DART instances.
  **[I]** (arithmetic on confirmed code). The register semantics ("stream *i*
  uses the TCR/TTBR of `REMAP[i]`") are **[I]**.
- So some AVE bus master emits **stream 1**, which macOS folds onto SID 0's
  translation. SID 1 is never in `sids`, so it has **no TTBR/TCR of its own**
  on macOS (`0x8befe14` iterates `sids` only). **[C]** Which master emits
  stream 1 (ASC CPU vs encoder DMA) is **[U]**.

### 2.4 Which instance the encoder datapath uses — verdict

No code in either binary names the bus wiring; macOS does not need to know,
because §2.1 makes every DART instance translate identically. The answer is
therefore **[I]**, from three independent lines:

1. The ADT names the DAPF-type instance `CPUDART`; the other is plain `DART`.
2. The same pattern elsewhere: `dart-isp0` = `DART LLT` (DAPF) + `DART BULK` +
   `DART RT` + `SMMU BULK` @ `DART BULK − 0x4000` + `SMMU RT` @ `DART RT − 0x4000`;
   `dart-disp0` = `DART` `0x38b304000` + `SMMU` `0x38b300000`. Linux drives ISP
   with all three DARTs and DISP0 with its DART, **no SMMU node in either**, and
   both work on this machine (`/proc/device-tree`: `isp@384000000` →
   `3860e8000/3860f4000/3860fc000`; `display-subsystem` → `38b304000`).
3. On Linux, CPU fetches/IPC through `0x40d040000` work and every earlier
   translation fault came from it (overlay comment), while the first datapath
   activity produced AXI errors with nothing reported by either apple-dart.

**Confidence: medium-high** that the datapath goes through `0x40d030000`
(with the SMMU behind it); **medium** that it tags transactions with stream 1.
Either way the fix in §6 covers both.

---

## 3. Q2 — what the SMMU is given

- **Setup:** `_smmuSetup` (`0x9b13f74`) only records `smmu-clock-gating`
  (`0x9b13fdc`) as bit 31 of init `+1768` (`0x9b14010..28`, OR `0x1200` when
  `[this+160]` is negative — **[I]** that this is `diag-config`, `0x80002100`)
  and perf flags `0x40003000` into `+1772` when `[this+2416]` is set
  (`0x9b13fec..4004`; the same gate `_setup` tests before reading
  `enable-perf-counters`, `0x9b11fd8`). PPL only applies `+1772` when
  `enable-perf-counters` is present (`[st+3403]`, `0x8bf1b6c`). **[C]**
- **PPL init:** at most **one** SMMU tunable, and it must target offset
  **`0x20`** (panics otherwise, `0x8bef574..f5bc`). **[C]**
- **SMMU power-up** `0x8bf1a2c`: reads `SMMU+0x0` and requires bit 21 (lock
  feature), lock = `SMMU+0x20` bit 15 (`0x8beefcc..eff0`); applies the tunable,
  OR-ing `[st+3360]` into the `0x20` value (`0x8bf1b14..b60`); if perf enabled,
  restores `+0x1020`, writes `+0x1008 = 0x100000`, `+0x1000` (`0x8bf1b6c..b98`).
  **Nothing else.** No write to any TTBR/TCR/REMAP-shaped offset exists for the
  SMMU in the PPL `t6000dart` text `0x8bee800..0x8bf23e0` (every use of the
  SMMU base `[st+3232]`/`st+0xca0`: `0x8beefe8`, `0x8bef540/550`, `0x8bf01ac/1b4`,
  `0x8bf0214`, `0x8bf058c`, `0x8bf05fc`, `0x8bf1a1c`, `0x8bf1a5c`, `0x8bf1ae4`,
  `0x8bf207c`; plus the write stub `0x857ff7c`). **[C]**
- **Runtime touches:** ioctl `0x6000` W1Cs `SMMU+0x40` and `+0x1008`
  (`0x8bf0218..22c`); `0x6003` clears `+0x1008` bit 20 (`0x8bf0614..62c`);
  `0x6002` writes `status & 0x27c` to `SMMU+0x40` then reads
  it back (`0x8bf018c..1b8`); `0x600a` sets a window index at `SMMU+0x5c`,
  bounded by `(SMMU+0x0 & 0xffff)` (`0x8bf0578..5ac`), used by
  `_smmuCaptureHardwareState` to read `SMMU+0x2000..0x3ffc` (`0x9b173d4..7410`). **[C]**
- **Register map known so far** (from `_smmuCaptureRegs` `0x9b1776c` and the
  above): `+0x0` params (bit 21 lock-capable, low 16 = table entries), `+0x20`
  config (bit 15 lock, bit 31 clock gating **[I]**), `+0x40` error status (bit 31
  pending, W1C bits `0x27c`), `+0x44/+0x48/+0x4c/+0x50/+0x58/+0x60/+0x64`
  captured (error info/address **[I]**), `+0x5c` table window, `+0x1000/+0x1004/
  +0x1008/+0x1020` perf, `+0x2000..0x3fff` table window (read-only use). **[C]**
  offsets; meanings **[I]**.
- Error masks: DART `0xb7f` (`0x8bf05c8`) vs SMMU `0x27c` — the SMMU never
  reports TTBR/STE-level faults (bits 0,1), only PTE/protect/AXI-class ones.
  Together with the read-only table window this suggests the SMMU is a
  hardware translation cache or checker fed from the DART's configuration, not
  an independently programmed MMU. **[I]**, low-medium confidence.
- **Can apple-dart drive it?** No, and it must not try: apple-dart's reset
  writes TCR `+0x100..`, TTBR `+0x200..`, `+0xfc`, `+0x20/+0x34` and reads
  PARAMS for page size — registers macOS never writes on this block. **[C]**
  (Linux `apple_dart_hw_reset`, `drivers/iommu/apple-dart.c`) / **[I]** that it
  would be harmful.
- **Why ISP/DISP0 work without SMMU nodes:** macOS itself gives the SMMU no
  translation state, so there is nothing Linux is failing to provide beyond an
  optional CONFIG tunable. **[I]** The restore ADT has no `smmu-tunables-*`
  or `dart-tunables-*` for any node at all (negative control: no DART has
  tunables either), so whether iBoot injects one into the live ADT is **[U]**;
  if it does, iBoot has already applied it before Linux boots.

---

## 4. Q3 — AIC 1028

- `dart-ave0` has **one** interrupt, 1028; `_setup` refuses more than one
  (`0x9b123bc..c4`). **[C]**
- `interruptPending` (`0x9b162c8`) treats the interrupt as pending if **any**
  of: `DART_i + 0x40` bit 31 (`0x9b162ec/0x9b16338`), `DART_i + 0x100c & 0x2d`
  when perf is enabled (`0x9b1630c`), `SMMU_j + 0x40` bit 31 (`0x9b16364`),
  `SMMU_j + 0x1008` bit 20 when perf enabled (`0x9b16374`).
  `_interruptAction` (`0x9b15934`) handles both families and logs
  `"Spurious DART/SMMU interrupt"` (`0x9b15bb0`). **[C]** So CPUDART, DART and
  SMMU all share AIC 1028. **[C]**
- Linux `apple_dart_t8020_irq` reads its own `+0x40` and returns `IRQ_NONE`
  unless bit 31 is set; otherwise it prints `translation fault …`. **[C]**
  (asahi `apple-dart.c`, `DART_T8020_ERROR_FLAG`).
- No `translation fault` line was printed, so neither `0x40d040000+0x40` nor
  `0x40d030000+0x40` had bit 31 set when the storm ran. The remaining sources
  are the **SMMU error status `0x40d020040` bit 31** and the perf-status bits
  (`DART+0x100c`, `SMMU+0x1008`). Linux never enables perf counters, and macOS
  does not for AVE (no `enable-perf-counters`), so the SMMU fault is the
  best-supported source. **[I]**, medium-high confidence. Direct confirmation
  needs one read of `0x40d020040` (proposal E-S1, §7).

---

## 5. Q4 and Q5 — the firmware prints

### 5.1 `Client:%d %d %d %d AXI Error: 0x%x 0x%x 0x%x 0x%x`

- String at fw VA `0xbfcbe`; sole reference `adr x0` at `0x38b60` in
  **`CAVEPipeISRManager::AxiErrorHandler`** (`0x38b04`), registered as firmware
  interrupt source **2** by the ISR manager constructor (`0x380d4..380ec`). **[C]**
- Register base `[0x21a7b8]` is the firmware's AVE window whose `+0x1800000` is
  the ASC bank (docs/40), i.e. AP-phys `0x40c000000` (cross-check: the
  scratch-register log `fffffffff5050034` = `sve` bank `0x40d050034`). **[C]**
- The handler (`0x38b14..38b8c`):
  - reads and discards `base+0x1110140` → **`0x40d110140`** (`0x38b38`);
  - the four `Client:` integers are `this+0x4a64/68/6c/70` (`0x38b48..4c`);
  - the four hex values are, in order, **`0x40d120000`**, **`0x40d120004`**,
    **`0x40d130000`**, **`0x40d128000`** (`0x38b50..5c`, args `0x38b68..74`);
  - then sets bit 0 of `0x40d110140` (`0x38b84..8c`, ack/mask **[I]**).
  **[C]**
- `+0x1120000`/`+0x1130000` are DMA-side blocks: `ResetDMANeighborRegs`
  (`0x27c88`) clears `+0x1120c14..` and `+0x1130614..`, and
  `ProcessPipeReset` writes `0x4004` to `+0x1120000` (`0x4ee08..4ee14`). **[C]**
- So `0x0 0x1 …` then `0x3 0x1 …` = `0x40d120000` read 0 then 3, `0x40d120004`
  read 1, the other two 0. Bit meanings are **[U]**; no decode string exists in
  13.5 or 26.6.2. Hypothesis only: `3` = AXI `DECERR`, `1` = a valid/count
  flag — would fit a master whose transactions were rejected by the IOMMU path.

### 5.2 `Uncompress Ref is not supported`

- String fw VA `0xc6530`, sole reference `0x54ffc` in
  **`CAVCController::setPipe`**. **[C]**
- Reached when `u32 [this+0x13a3c] == 0` (`0x53218..53224`, tested `0x54ce4..ec`)
  **and** `u8 [this+0x24058] == 0` (`x24 = this+0x23b38`, `0x52f60`; tested
  `0x54f7c..80`). It logs and branches back to `0x54cf0`, the normal path
  (`encoder_addr_src_colo`). **Not an assert.** **[C]**
- It sits right after the recon luma/chroma address programming; the other
  branch asserts `(sRecon.Y_LSB & 127) == 0` (`0x54f98`), so the flags select a
  compressed-reference mode. **[I]** Which Start_AVC/Process field feeds them is
  **[U]**. For an I-frame-only session the reference is not read back for
  prediction, so this is unlikely to be the cause of the hang. **[I]**

---

## 6. What Linux must change

### 6.1 Device tree (overlay) — recommended

```dts
ave0: video-encoder@40d100000 {
	...
	iommus = <&dart_ave0_0 0>, <&dart_ave0_0 1>,
		 <&dart_ave0_1 0>, <&dart_ave0_1 1>;
};
```

- **Both DART nodes** (`0x40d040000` and `0x40d030000`), because macOS maps
  every IOVA identically on both (§2.1, **[C]**) and the datapath very likely
  uses the second (§2.4, **[I]**). This is exactly the ISP pattern.
- **SID 1 on both**, because macOS remaps stream 1 onto SID 0 (§2.3). apple-dart
  never writes REMAP (`+0x80`), so the Linux equivalent is to put SID 1 in the
  same domain: it gets the same TTBR and TCR `0x80` as SID 0. `of_xlate`
  accepts several SIDs per DART (`set_bit(sid, sidmap)`). Cost if stream 1 is
  unused: none. **[I]** Counter-evidence to keep in mind: `dart-dcp` has
  `remap = 5` and Linux's DCP works with SID 0 only, so SID 1 may turn out to be
  unnecessary.
- **No SMMU node, and never bind `apple,t6000-dart` to `0x40d020000`** (§3).
- Keep `dart_ave0_1` without bypass expectations (it reports no bypass support);
  the device must stay in a translated domain. apple-dart also requires equal
  `pgsize`/`ias` across the DARTs of one device (`of_xlate` returns `-EINVAL`
  otherwise); if the attach fails, check that first. **[C]** (Linux source),
  **[U]** whether the two PARAMS agree.
- SID 15 (bypass, `bypass-address` offset 2) is not needed for this session
  and is left out. **[U]** whether any AVE block uses it.

### 6.2 Driver (`driver/`) — suggested, not required for the DT fix

- **Handle the SMMU's share of AIC 1028** so a fault is visible instead of
  disabling IRQ 130: map `0x40d020000 +0x4000` (third `reg` entry, e.g.
  `"smmu"`, same `VENC-DART` power as the DARTs), `request_irq(1028,
  IRQF_SHARED)`, and in the handler: read `+0x40`; if bit 31 is clear return
  `IRQ_NONE`; else log `+0x40, +0x44, +0x48, +0x4c, +0x50, +0x58, +0x60, +0x64`
  and write back `status & 0x27c` (what macOS does, `0x8bf01a4..1b0`). **[I]**
  that W1C of those bits clears bit 31.
- Optionally log the firmware's AXI registers (`0x40d120000/4`, `0x40d128000`,
  `0x40d130000`) from the AVE interrupt path, to correlate with SMMU/DART status.
- Nothing needs programming in the SMMU for translation (§3).

---

## 7. Proposed hardware checks (operator-run, per AGENTS.md)

- **E-S1 (discriminates Q3):** reproduce the f3 run with the SMMU handler of
  §6.2 (or a one-shot `readl(0x40d020040)` from the AVE driver while VENC is
  powered, before IRQ 130 is disabled). Bit 31 set ⇒ SMMU fault confirmed;
  also record `0x40d030040` and `0x40d040040` as the negative control.
- **E-S2 (the fix):** apply §6.1 with only `<&dart_ave0_0 0>, <&dart_ave0_1 0>`
  first, then add SID 1 if AXI errors persist. Success criterion: no `AXI
  Error`, no LRME/PIPE HANG, a completion on IO_T2H, and IRQ 130 not disabled.
  Any `translation fault … stream:N` from `40d030000` names the datapath SID
  directly.

## 8. Not settled

- Physical wiring of CPU vs DMA masters to the two DARTs and to the SMMU **[U]**.
- Which master emits stream 1 **[U]**; whether live-ADT `smmu-tunables-instance-0`
  exists **[U]**.
- Bit meanings of the SMMU `+0x40` status and of the firmware AXI registers **[U]**.
- The two flags behind `Uncompress Ref is not supported` **[U]**.
