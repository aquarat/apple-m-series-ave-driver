# How Apple routes the AVE coprocessor's instruction fetch

Written 2026-09-08 from static analysis of `data/blobs/kc.macho` (macOS 13
kernelcache for `mac13j`), `data/blobs/adt.bin`, and the m1n1 checkout in
`m1n1-src/`. No hardware was touched.

## Short answer

**It doesn't.** `0x10000b28200` is not an address Apple's stack ever produces.

`ASC+0x50000` is the AVE coprocessor's **RVBAR** — an ARM reset-vector base
register. Apple's `AppleAVE2` writes it with the **DART address (IOVA) of the
firmware surface it just allocated**, so on a real Apple boot the fetch is a
small IOVA on stream 0 that the DART translates normally.

The 44-bit value sitting in that register on our machine is **iBoot's**, and it
is a *physical* address. We inherit it only because our write to the register
does not take. Every line of investigation that tried to make a 41-bit address
reach memory — bypass, identity domains, `bypass-address` offsets, a second
translation stage — was chasing iBoot's leftover, not Apple's design.

The remaining problem is therefore narrow and different from the one
[31](31-bringup-state.md) states: **make the RVBAR write take.**

---

## 1. `ASC+0x50000` is RVBAR, and its field layout is an ARM RVBAR's

**Confirmed.** `AVE_IOP_Config_Nyx(AVE_Reg *reg, uint64_t base)` at
`0xfffffe0008c353f0`:

```
fffffe0008c354dc:  mov  w2, #0x50000
fffffe0008c354e0:  bl   AVE_Reg::Read64(bank 1, 0x50000)      ; read, for the log line only
...
fffffe0008c35558:  and  x8, x20, #0x3fffffff800               ; base & bits[41:11]
fffffe0008c3555c:  mov  x9, #0x102000000000000                ; tag 0x0102 in bits[63:48]
fffffe0008c35560:  orr  x3, x8, x9
fffffe0008c3556c:  mov  w2, #0x50000
fffffe0008c35570:  bl   AVE_Reg::Write64(bank 1, 0x50000, x3)
```

Three things follow, all read out of those instructions:

- The address field is **bits [41:11]** — a 2 KiB-aligned address, up to 42
  bits. ARM's architectural `RVBAR_ELx` is defined exactly this way: address in
  the high bits, **bits [10:0] RES0**. That alignment is the signature.
- **Apple writes bit 0 as zero.** The `orr` combines only the masked base and
  the `0x0102` tag. Our readback has bit 0 set, so nothing Apple's driver does
  set it.
- The `0x0102` tag in bits [63:48] is AVE-private. A scan of every
  `movz/movk xN, #0x102, lsl #48` in the whole kernelcache
  (`scratchpad/scan_imm.py`) finds it **only** in the eighteen
  `AVE_IOP_Config_*` variants and one unrelated FairPlay site. It is not a
  generic ASC idiom. **Confirmed.**

**Corroboration from m1n1** (`m1n1-src/proxyclient/m1n1/hw/isp.py:13`,
`hw/ane.py:12`):

```python
ISP_ASC_RVBAR   = 0x1050000, Register64      # ISP
ASC_IO_RVBAR    = 0x1050000, Register32      # ANE
ISP_ASC_CONTROL = 0x1400044
ISP_ASC_STATUS  = 0x1400048
```

`0x1400044 - 0x1050000 = 0x3B0044`. Our AVE offsets are `0x400044`
(CPU_CONTROL, a known-good canary) and `0x50000`, and
`0x400044 - 0x50000 = 0x3B0044` — **identical spacing**. So bank1+0x50000 sits
at the RVBAR position of the standard Apple ASC register block.
**Confirmed by construction; the identification itself is inferred** (from an
exact structural match plus the RES0-alignment mask, not from a symbol).

And m1n1's CPU-side RVBAR handling names the low bit
(`m1n1-src/src/smp.c:27`):

```c
#define RVBAR_LOCK BIT(0)
#define RVBAR_ADDR GENMASK(47, 12)
```

with, at line 153, the comment *"This also clears RVBAR_LOCK, so that HV can set
RVBAR later when the core is running"* — m1n1 deliberately writes CPU RVBARs
**without** the lock bit. Apple's AVE code does the same thing for the ASC.
That bit 0 on the *ASC* RVBAR is a lock is **inferred** (analogy to the CPU
RVBAR + Apple never setting it + our write being ignored), not confirmed from
any binary.

Decoding our cold-boot readback with this layout:

```
0x0102010000b28001
  bits[63:48] = 0x0102          AVE tag, same value Apple writes
  bits[41:11] = 0x10000b28000   reset vector, 2 KiB aligned
  bit  0      = 1               set by iBoot; Apple's own write clears it
```

## 2. Apple's base is a DART address, not a physical address

**Confirmed.** `AVE_FwImg::GetBaseAddr()` at `0xfffffe0008bee3ac`, tail at
`0xfffffe0008bee474`:

```
fffffe0008bee474:  ldrb w8, [x19]            ; flag
fffffe0008bee478:  tbnz w8, #0, ->return 0
fffffe0008bee47c:  ldr  x0, [x19, #128]      ; AVE_Surface *
fffffe0008bee480:  cbz  x0, ->return 0
fffffe0008bee484:  ldr  w1, [x19, #32]
fffffe0008bee48c:  bl   AVE_Surface::GetDARTAddr(w1, 0)
```

**Confirmed.** `AVE_IOP::Config()` at `0xfffffe0008c4073c` dispatches through
the per-ChipType interface table and passes that value straight in:

```
fffffe0008c40840:  adrp x10, ...            ; gsc_saAVE_IOP_If @ 0xfffffe0007ee07f0
fffffe0008c40848:  smaddl x8, w21, #0x28, #-40   ; (ChipType * 0x28) - 0x28
fffffe0008c40860:  ldr  x22, [x9]           ; entry[0] = Config fn
fffffe0008c40868:  ldr  x20, [x19, #24]     ; AVE_Reg *
fffffe0008c4086c:  bl   AVE_FwImg::GetBaseAddr
fffffe0008c40870:  mov  x1, x0              ; arg1 = base
fffffe0008c40874:  mov  x0, x20
fffffe0008c4087c:  blraa x22, x17
```

**Variant resolved** (Trap 4): `gsc_saAVE_IOP_If + 7*0x28 - 0x28` decodes,
using the chained-fixup rule from Trap 5 (low 32 bits = image-relative file
offset), to file offset `0x1c313f0`, which is the file offset of
`AVE_IOP_Config_Nyx` (`0xfffffe0008c353f0`). t6001 -> ChipType 7 -> `_Nyx`
is confirmed end to end.

So on macOS the sequence is: allocate the firmware surface, map it through the
DART on the mapper's SID, and write **that IOVA** into RVBAR. The fetch is a
translated fetch, on stream 0, at a small address.

That is consistent with the fault Linux reports: `stream:0`. The stream is
right. Only the address is wrong.

> **Correction (2026-09-13, [44](44-reset-fetch-path.md)).** The logged fault
> code `0x800` is `NO_DAPF_MATCH` (DART error bit 11, m1n1 `dart8020.py`), not
> a translation miss. Linux printed it as `(unknown)`. The DVA-width
> arithmetic below is correct but was never the obstacle. The 13.5 firmware
> bootstrap fetches TEXT **physically** (through the CPUDART's DAPF, which has
> no entry for it on Linux) and reaches DATA via a `0x1f0_0000_0000` DVA
> window.

## 3. No page table on this DART can ever hold `0x10000b28200`

Two independent sources give the same page-table geometry.

**Apple's own constants** (`AppleT6000DART`, `__DATA_CONST`):

```
__ZL21t6000dart_ttIndexMask  @0xfffffe00076a49e0 = { 0x00c00000, 0x003ff800, 0x000007ff }
__ZL22t6000dart_ttIndexShift @0xfffffe00076a49f8 = { 22, 11, 0 }
```

Applied to the *page number* (`dva >> 14`): level 0 = dva bits [37:36] (2 bits,
the four TTBRs), level 1 = bits [35:25] (11), level 2 = bits [24:14] (11).
`AppleT6000DART::DART_VO_L0_INDEX` at `0xfffffe0009b170b4` extracts exactly
`ubfx w8, w1, #22, #2` and panics if it is nonzero.

**m1n1** (`proxyclient/m1n1/hw/dart8020.py`, which covers `dart,t6000`):

```python
L0_SIZE = 4   # TTBR count
L0_OFF  = 36
L1_OFF  = 25
L2_OFF  = 14
IDX_BITS = 11
```

Both say the same thing: **maximum DVA = 2^38 = 256 GiB**. The fetch address
`0x10000b28200` is `2^40 + 0xb28200`. It is out of range by a factor of four
even for Apple's *full* geometry, and Linux additionally clamps `ias` to 32.

**Confirmed: the fetch address is unmappable, under macOS as much as under
Linux.** This is not a Linux limitation. It is proof that the address in the
register is not one any DART was ever meant to translate — i.e. it did not come
from Apple's driver.

Related, from the same kext: `AppleT6000DART::_dartValidateConfig` at
`0xfffffe0009b17e90` reads `PARAMS1` bits [27:24] as `log2(page size)` and
panics unless it equals the ADT's `page-size`, and reads `PARAMS2` bit 0 as
"full bypass supported". Those are the same two register fields Linux's
`apple-dart` prints as `pagesize 4000` and `bypass support: 1`.
**Confirmed: Linux and Apple read the identical hardware config words.** There
is no hidden wider-input mode.

## 4. What is actually at physical `0x10000b28000`

`/proc/device-tree/memory@10000000000/reg` starts at `0x1000218c000`, and the
lowest `asc-firmware` carve-out is `0x10000c68000`. So `0x10000b28000` is DRAM
that **Linux never owns and never writes** — it holds whatever iBoot left.

The 1 KiB dump in `data/blobs/iboot-ave-stub-1k.bin` was taken at
`0x10000b28200`, not at the base. Decoded:

```
+0x000: 14000000  b   .                 ; park
+0x004: d5384241  mrs x1, currentel
+0x008: d3420c21  ubfx x1, x1, #2, #2
+0x00c: f1000c3f  cmp x1, #3            ; EL3?
+0x018: d51e4021  msr elr_el3, x1
+0x01c: d28078a1  mov x1, #0x3c5        ; EL1h, DAIF masked
+0x020: d51e4001  msr spsr_el3, x1
+0x024: d69f03e0  eret
+0x030: 10ffee80  adr x0, -0x230        ; = 0x10000b28000
+0x034: d518c000  msr vbar_el1, x0
+0x038: d53800a0  mrs x0, mpidr_el1
```

So the image's base is `0x10000b28000` and it sets `VBAR_EL1` to that base.
It is **not** m1n1: m1n1's `_vectors_start` (`m1n1-src/src/start.S:23`) begins
`mov x9, '0' ; b cpu_reset`, which does not match. **Confirmed by comparison.**
Whose stub it is — iBoot's coprocessor park stub is the obvious guess — is
**unknown** and does not matter: it is not an AVE firmware we can use, and
nothing in Apple's driver points at it.

Note the fault address is base + 0x200. Under `VBAR_EL1 = base`, `+0x200` is
the "Current EL with SPx, synchronous" vector — consistent with the core
resetting to `base`, aborting on the very first fetch, and vectoring to
`base+0x200`, which aborts too. That is a self-sustaining loop and explains the
260 kHz fault storm. **Inferred**; the alternative (the ASC's reset PC is
literally RVBAR+0x200) is not excluded.

## 5. ADT `/arm-io/dart-ave0` decoded

Raw bytes re-read from `data/blobs/adt.bin` with m1n1's parser.

| property | raw | meaning | status |
|---|---|---|---|
| `sids = 0x8001` | | bitmask of valid SIDs: 0 and 15. Read by `isValidSID` at `0xfffffe0009b14bec` (bit test on `[this+2240]`) | confirmed |
| `bypass = 0x8000` | | bitmask of **bypassed** SIDs: 15 only. `isBypassedSID` at `0xfffffe0009b14c20` (bit test on `[this+2304]`). **SID 0 is not bypassed under Apple either** | confirmed |
| `bypass-address` | 32 bytes, all zero except `[30..31] = 02 00` | **16 × u16, one per SID.** `_dartSetup` at `0xfffffe0009b13974` requires `getLength() == 0x20` and copies it as two 16-byte halves. `_dartValidateConfig` (`0xfffffe0009b17f1c`-`0xfffffe0009b17f94`) uses it only when `PARAMS2` bit 0 (full bypass) is **clear**: for each valid+bypassed SID it reads `u16 tbl[sid]` and panics *"full bypass mode not supported and bypass-address property not specified"* if zero. Here only SID 15 has an entry, value `2` | confirmed |
| | | **Correction to [31](31-bringup-state.md): this is not `0x200000000`.** As u64s the blob is `{0,0,0,0x0002000000000000}`; as the per-SID u16 array the driver actually reads, it is `tbl[15] = 2`. The "bypass adds the `/arm-io` 0x200000000 offset" hypothesis has no basis. Also: `40d040000` reports `bypass support: 1`, so for that instance `bypass-address` is never consulted at all | confirmed |
| `remap = 1` | one u32 | array of SID-remap records. `_dartSetup` at `0xfffffe0009b13a1c` requires `length % 4 == 0`, takes `count = length/4`, and copies **bytes [0] and [1] of each 4-byte record** into a pair array at `init_data+0x6ac`, validating both are < 16. Here the single record gives the pair `(1, 0)`. The hardware target is the DART8020 `REMAP` register block at `0x80..0x8f` (16 byte entries, SID -> SID) | pair values confirmed; which element is source and which is destination is **unknown** |
| | | Either way this record concerns SIDs 0 and 1, not 15, so it is **not** a mechanism that could move the ASC fetch between the translated and bypassed SIDs | inferred |
| `page-size = 16384` | | cross-checked against `PARAMS1[27:24] = 14` or panic (`0xfffffe0009b17ee0`) | confirmed |
| `vm-base = 0` | | start of the DVA space the mapper allocates from; read at `0xfffffe0009b133e4` | confirmed |
| `dart-options = 37` (`0x25`) | | stored whole as a flag word by `IODART::start` at `0xfffffe000a72a3a4`/`0xfffffe000a72a430` (`[this+2440]`). Decoded uses: **bit 4** = "bypass not permitted" (`0xfffffe0009b1393c`: if set, `bypass`/`apf-bypass` must be zero; `0xfffffe0009b139c4`: if set, `bypass-address` is an error) — **clear** here, so bypass is allowed; **bit 3** = PIO aperture properties are read (`0xfffffe0009b13b58`) — clear here. Bits 0, 2, 5 are set; meanings **unknown** | as marked |
| `real-time` (present, empty) | | boolean; `_setup` at `0xfffffe0009b12c08` sets `init_data+1812` | confirmed |
| `ioa-parent = <0x402000000, 0x4000>` | | **not a translation stage.** `_setup` at `0xfffffe0009b12c60` fetches it as OSData, requires `getLength() == 0x10` (two u64s), passes them to `IODeviceMemory::withRange(addr, size)`, maps it, and stores the resulting *virtual* address at `init_data+1744`. It is a 16 KiB **MMIO block the DART driver maps and touches**, mandatory when `real-time` is set (panic string *"realtime agents require an ioa-parent"*, `_setup.cold.2` at `0xfffffe0009b18fd0`). The value is used verbatim as a physical address — it is **not** run through the `/arm-io` `ranges` translation | confirmed |
| | | It cannot be what routes the fetch: it is a register block on the CPU's side of the bus, and the fault we observe is raised by the DART itself, which means the transaction already reached the DART's translation path | inferred |
| `diag-config = 0x80002100` | | the string `diag-config` **does not appear anywhere in the kernelcache**. Nothing in macOS consumes it; it is iBoot-side | confirmed (grep over the whole 118 MB image) |
| `error-reflector = 0x29209c000` | | consumed by `AppleT6000PlatformErrorHandler`, not by the DART driver (the only occurrence of the string is in that kext's cstrings at file offset `0x6932cf`) | confirmed |
| `filter-data-instance-0` | 48 bytes | DAPF (address-protection filter) table for instance 0, consumed by `AppleT6000DART::_apfSetupInstance` (`0xfffffe0009b141f4`, string xref at `0xfffffe0009b142c4`). As u32: `0, 0x1f0, 0xffffffff, 0x1f0, 0x30300, 1, 0x6000000, 5, 0x7c6c000, 5, 0x10300, 1`. Record layout **not decoded** — several groupings were tried and none produced coherent ranges | unknown |
| `instance` | 36 bytes | three 12-byte records. The 4-byte LE tags decode unambiguously to **DAPF** (`FPAD`), **DART** (`TRAD`), **SMMU** (`UMMS`), matching the class's `_dartSetup*` / `_apfSetup*` / `_smmuSetup*` methods. Exact record layout uncertain (the first record's trailing bytes read as `"CPUDART\0"`) | tags confirmed, layout unknown |
| | | Consequence worth noting: **the four `reg` entries are a DART/DAPF/SMMU complex, not four DARTs.** Linux instantiates `40d040000` and `40d030000` as two independent `apple-dart` devices. That is a modelling difference, though both do answer `PARAMS` reads plausibly | inferred |

## 6. Where Linux and Apple actually differ

Not where [31](31-bringup-state.md) supposed.

| | Apple | Linux `apple-dart` |
|---|---|---|
| input address space | 38 bits (4 TTBRs x 64 GiB) | 32 bits (`ias = 32`) |
| page size | 16 KiB from `PARAMS1[27:24]` | same |
| full-bypass detect | `PARAMS2` bit 0 | same (`bypass support:`) |
| bypass on probe | leaves iBoot's `bypass = 0x8000` (SID 15) | `apple_dart_hw_reset` clears TCR for all streams |
| SID remap regs (`0x80`) | programmed from ADT `remap` | never touched — iBoot's values survive |
| fetch stream | SID 0, translated | SID 0, translated |

The `ias` gap is real but **irrelevant here**: 38 bits does not reach 2^40
either. Nothing about a wider input, a bypass offset, or an IOA stage would
make `0x10000b28200` resolvable. Do not implement any of them.

## 7. What Linux would have to do

**Revert the bypass work.** The identity/bypass domain, the `iommu_map` skip on
identity domains, and reading the base out of the register in `ave_fw.c` are all
built on the premise that the register's value must be honoured. Apple's driver
overwrites it. The correct behaviour is the original one: allocate and map the
firmware, then write **our** IOVA into RVBAR.

The write is:

```c
u64 iova;                              /* where we mapped the image */
writeq((iova & 0x3FFFFFFF800ULL) | (0x0102ULL << 48), bank1 + 0x50000);
```

with bit 0 left clear and the IOVA 2 KiB-aligned (16 KiB-aligned in practice,
since that is the DART page size). Everything else in
[31](31-bringup-state.md)'s start sequence stays.

The one blocker is the observed fact that this write does not take. What is
established and what is not:

- **Confirmed:** the readback after our write is byte-identical to the cold-boot
  value.
- **Inferred:** bit 0 is a lock, by analogy with `RVBAR_LOCK` in m1n1's
  `smp.c:27` and because Apple's own write clears it.
- **Unknown:** whether the lock clears on a block reset, and what resets it.

**Does this need m1n1?** Probably not, and this is worth being precise about.
The CPU RVBAR needs m1n1 because it is written from EL3 before the lock is set.
This register is **ordinary device MMIO inside the AVE block** — a kernel driver
at EL1 that has the bank mapped has exactly the same access to it that m1n1
would. Raising the exception level buys nothing by itself. What m1n1 *would*
buy is (a) earlier timing, before whatever iBoot-era step sets the lock has an
effect on the AVE block, and (b) a way to poke and power-cycle the block
interactively without loading a driver. So: **m1n1 is a convenient
experimental harness here, not a required privilege escalation.**

## 8. Two steps of Apple's sequence we do not implement

Both found while tracing this, both cheap, both plausible candidates for why
the register is unwritable at the moment we write it:

- **`AVE_IOP::Stop()` is called from `AVE_HwC::StartUpIOP+0x4b8`
  (`0xfffffe0008c1d80c`), before `Config` and `Start`.** [31](31-bringup-state.md)'s
  table of `StartUpIOP` calls omits it. `AVE_IOP::Stop` (`0xfffffe0008c40da4`)
  dispatches to the per-variant `AVE_IOP_CheckIdle_Nyx`
  (`0xfffffe0008c357dc`) and polls with `IODelay` until the core reports idle.
  **Apple stops and confirms the core is idle before touching RVBAR.**
  **Confirmed.**
- **`AVE_PMGR::ResetPSD(_E_AVE_PMGR_PD)` (`0xfffffe0008c5012c`) is called from
  `AVE_PMGR::SetPState+0x170` (`0xfffffe0008c521cc`).** A power-state-domain
  reset of the AVE block is part of Apple's power path. If bit 0 is a lock, a
  PSD reset is the obvious thing that would clear it. **Confirmed that the call
  exists; unknown whether it runs on this SoC or whether it clears RVBAR.**

> **Correction (2026-09-13).** Neither step is actually missing.
> `AVE_IOP::Stop` performs **no register write** — it only polls
> `CheckIdle` (`CPU_STATUS & 3`) until three consecutive reads are idle
> ([09](09-firmware-load.md) §2.5), so it cannot unlock anything. `ResetPSD`
> is gated on `HwFeature & 2` and **returns without doing anything on t6001**,
> where `HwFeature == 1` ([10](10-power.md)). The call sites exist; on this SoC
> they do nothing that could clear the lock.

## 9. Proposed experiments — for the operator, not for an agent

None of these have been run. In rough order of value per reboot:

1. In one boot, with the AVE domain freshly powered: read `bank1+0x50000`;
   write `0`; read back. Then write `(0x0102 << 48)` alone; read back. This
   distinguishes "write-ignored / read-only" from "only the address field is
   locked" and costs nothing.
2. Confirm the domain really power-gates: read the register, `rmmod`, verify
   `pm_genpd_summary` shows the `venc_*` domains off, `insmod`, read again. If
   the value survives a genpd cycle, genpd is not resetting the block and a
   harder reset is needed.
3. Implement `AVE_IOP::Stop`-equivalent (poll the core idle) before writing
   RVBAR, and re-test the write.
4. Only if 1-3 fail: look for the AVE analogue of the ASC `EDPRCR` register.
   By the same offset arithmetic that identified RVBAR, m1n1's
   `ISP_ASC_EDPRCR = 0x1010310` maps to AVE `bank1 + 0x10310`. **Unverified
   arithmetic on an unverified analogy** — treat as a lead, not an address.

Do not repeat the identity-domain experiment. It is now explained: bypass takes
the fetch off the translation path, the physical address it then presents is a
park stub that immediately branches to itself, so nothing faults and nothing
changes. Both halves of that observation are consistent with the core doing
nothing useful, which is what the page checksums said.

## 10. Corrections this makes to earlier documents

- [31](31-bringup-state.md), "the fetch cannot be satisfied by translation":
  correct, and now proven with Apple's own page-table geometry rather than
  Linux's `AS 32 -> 42` print. But the conclusion drawn from it — "therefore the
  fetch is meant to bypass" — does not follow, and is wrong. The fetch is meant
  to be *at a different address*.
- [31](31-bringup-state.md), `bypass-address = 0x200000000`: wrong. It is a
  16-entry per-SID u16 table with only `tbl[15] = 2` set, and it is only
  consulted on DART instances whose `PARAMS2` bit 0 is clear — which
  `40d040000` is not.
- [31](31-bringup-state.md), "iBoot has left a real firmware at physical
  `0x10000b28000`": there is real AArch64 code there, but it is a generic
  EL3->EL1 bootstrap with a park loop at its `+0x200` vector, not an AVE
  firmware, and nothing in `AppleAVE2` refers to it.
- [31](31-bringup-state.md)'s `StartUpIOP` call table is missing
  `AVE_IOP::Stop` at `+0x4b8`.

---

## Hardware verification (run on the machine, 2026-09-08)

> See the correction at §3: the "cannot be translated" row is arithmetically
> true, but the fault was a DAPF rejection ([44](44-reset-fetch-path.md)).

| Claim | Test | Result |
|---|---|---|
| RVBAR/CONTROL offsets are a standard ASC layout | `0x400044 - 0x50000 = 0x3b0044`; m1n1's ISP `0x1400044 - 0x1050000 = 0x3b0044` | **confirmed**, identical |
| A 41-bit fetch cannot be translated | DART max DVA `2+11+11+14 = 38` bits; `0x10000b28200` needs 41 | **confirmed** |
| The register is locked, not merely mis-written | wrote `0x0102000000000000`, `0x0`, and `0xffffffffffffffff`; **every one ignored**, not one bit moved | **confirmed** |
| A reset clears the lock | `reset_control_reset()` returned 0; RVBAR unchanged, still ignores writes | **refuted** |

Power gating does not clear it either - the value is identical on every probe
after the VENC domains have been fully off.

So the register is hard-locked to a physical address that this DART can never
translate, and nothing reachable from EL1 has been found that clears it.

## Where that leaves the two modes

The two agents appeared to disagree about whether the kext writes RVBAR. They
do not: the writer is gated on `m_bIBootLoaded`
([42](42-asc-firmware-ownership.md)), so there are two modes.

| | RVBAR holds | DART's job |
|---|---|---|
| iBoot loaded the firmware | a **physical** address | must pass it through untranslated |
| the kext loaded it | a **DART IOVA** | translates normally |

We are in the first, and Linux cannot get to the second because the register is
locked. Which means the target is not to change RVBAR at all — it is to put
the DART back into the state iBoot left it in, where an untranslated 41-bit
fetch reaches memory.

That reframes the failed bypass attempt. `apple_dart` **resets the DART on
probe**, destroying iBoot's configuration, before anything of ours runs. What
we tried afterwards was Linux's idea of an identity domain, which is not the
same thing as the configuration iBoot left behind.

**Next experiment:** an overlay that does not declare the DART node at all and
gives the AVE node no `iommus`, so `apple_dart` never binds and never resets
it. If iBoot's configuration is what makes the fetch work, the core should
execute with the DART untouched. This needs a reboot, since the DART on this
boot has already been reset.

> **Update (2026-09-13): the premise is almost certainly void.** On a fresh
> boot, with nothing of ours loaded, `pm_genpd_summary` already shows
> `venc_sys` **off-0** — and `venc_sys` is the DART's power domain
> (`power-domains = <0x1d>`). Whatever iBoot programmed into the DART was lost
> to power gating (or never existed, if iBoot never powered VENC) before any
> overlay could preserve it. The `variant=1` overlay would therefore test a
> freshly reset DART with no IOMMU backstop, which is all cost and no
> information. Not run. The one thing that would revive it is evidence that
> DART register state survives a power-off — RVBAR does survive, but it is a
> lock register and plausibly not in the gated domain at all.
