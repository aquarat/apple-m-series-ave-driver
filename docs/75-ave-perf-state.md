# AVE performance state: what "VMax" does on t6001, and how Linux could ask for it

Static analysis of the macOS 13.5 kernelcache (`data/blobs/macos-13.5/kc.macho`:
`com.apple.driver.ApplePMGR`, `AppleT6001PMGR`, `AppleAVE2`) and the 13.5
`j314cap` ADT. The only hardware data used is run f73 (the lead's read of
`0x28e5d8000..+0x700`, below). **Nothing here was run on hardware.**

Labels per [00](00-methodology.md): **[C]** read from an instruction (VA
cited) or an ADT value, **[I]** inferred with the chain stated, **[U]**
unknown. Kernel VAs are 13.5. They disassemble with
`AVE_MACOS=13.5 python3 tools/disas.py --kext --addr <VA> -n 0x40` (the tool
reads the whole kernelcache; the PMGR symbols come from
`tools/kext_extract.py … --symbols com.apple.driver.ApplePMGR`).

```sh
python3 tools/pmp_ptd_map.py      # every address and message value below, with 4 controls
```

---

## 0. Verdicts

| # | question | verdict | label |
|---:|---|---|---|
| 1 | What does raising AVE to VMax do in PMGR? | **No PMGR register is written for it.** The VENC pseudo-devices (456/457/458/459/600/602) are `no_ps` ("virtual"), so PMGR does no PS, clock or bridge work for them. The only hardware effect is **one 64-bit write into the PMP's dashboard ("PTD")**. On t6001 that is `0x28e3d0888` for ave0. The value is `SOC level \| FAB0 level << 32 \| 1 << 61`: VMax = `0x2000000000000003`, and with the FAB vote `0x2000000300000003`. The PMP coprocessor then owns the voltage and clock change. §1, §3. | [C] AP side; [I] PMP side |
| 2 | Is perf block 9 idx 36 the VENC perf state? | **No. It is a PMGR performance *counter*.** ADT `flags.perf` is Apple's `kDeviceFlagPerfCounter`. `perf_block/perf_idx` locate a counter: control at `base + perf.offset + 0x100 + idx*0x10`, count at `+8`/`+0xc`. **The f73 zeros mean "counter disabled", not "perf state 0".** The address f73 read is correct. m1n1's `dump_pmgr.py` is the one that is wrong: it drops `perf.offset`. §2. | [C] |
| 3 | Which states and frequencies exist? | VENC's SoC request has levels **0..3** (VNOM = 1, VMID2 = 2, VMAX = 3), plus a FAB0 vote of 3. The PMP's DVFS domain for AVE0 is `AVEMSR0_AVE`, which has **4 states**. **No frequency or voltage for it exists in the ADT or in ApplePMGR.** They live in the PMP firmware. §4. | [C] levels; [I] domain; [U] MHz |
| 4 | Who changes the voltage? | **The PMP** (`/arm-io/pmp`, ASC at `0x28ec00000`, firmware `t6000pmp`). The AP only writes requests. There is no SEP, SMC, SPMI or TVM step on this path. §3.4. | [C] AP side |
| 5 | What does Linux do? | `apple-pmgr-pwrstate` handles PS registers only. The pseudo-devices have no PS register, so Linux never models them. **Nothing in Linux writes SOC-DEV-DVFS.** Asahi does have `apple-pmp-report` (`drivers/pmdomain/apple/pmp-report.c`), which writes the power-state half (SOC-DEV-PS-REQ), and its DT has an entry for VENC: `pmp_report_venc_sys: report@10`. That entry is `status = "disabled"`. The PMP firmware runs only if Asahi's `apple,t6000-pmp-v2` driver (`pmp.rs`) booted it. That node is also `disabled` unless m1n1 enabled it. §5. | [C] sources; [U] target state |
| 6 | Does this explain 150 Mpixel/s? | **Plausible, not shown.** Under Linux the PMP receives no AVE vote at all. That is macOS's **VMin** state, where `AVE_DPM_SetIOP(PL 1)` powers only gate 456, which sends nothing to the PMP. If the PMP is not running at all, the AVE rail and clock are whatever iBoot left. R2 below measures the VENC clock directly and settles it. | [I] |

**The next step is §7 R0 and R1**: shell reads and six 64-bit MMIO reads, no writes.
They say whether the PMP is alive and what it has been told about AVE0.

---

## 1. The VMax path in macOS 13.5, instruction by instruction

### 1.1 AVE kext side

`AVE_DPM_SetIOP(pmgr, PL)` (`0xfffffe0008ee8c4c`) switches over PL 0..4 with
the jump table at `0xfffffe0008ee8e34`. Each case calls
`AVE_PMGR::SetPS(pd, ps, 1)` (`0xfffffe0008f2ca14`). The 13.5 PD enum is IOP 0,
IOP_Mid 1, DCS 2, …, IOP_Max 8, IOP_Mid2 9, FAB 10
([10](10-power.md) version note). **[C]**

| PL | calls (VA) | ADT gates that change |
|---|---|---|
| 0 | `SetPS(IOP, Off)` `0x…8c9c` | 456 off |
| 1 VMin | `SetPS(IOP_Mid, Off)`, `SetPS(IOP, On)` `0x…8d20` | 457 off, 456 on |
| 2 VMid | `SetPS(IOP_Mid2, Off)`, `SetPS(IOP_Mid, On)` `0x…8d44` | 600 off, 457 on |
| 3 VMid2 | `SetPS(IOP_Max, Off)`, `SetPS(IOP_Mid2, On)` `0x…8d68` | 459 off, 600 on |
| 4 VMax | `SetPS(IOP_Max, On)` `0x…8d8c` | 459 on |

The up-dependency ladder IOP → IOP_Mid → IOP_Mid2 → IOP_Max
([10](10-power.md)) means that **at VMax 456, 457, 600 and 459 are all on**.
`ApplyFAB(PL ≥ 4)` turns on 602, and `ApplyDCS(PL ≥ 2)` turns on 458. **[C]**
for the table. The PL policy is `AVE_DPM_DecideIOPPower` (`0xfffffe0008ee6e94`).
A non-zero `[dpm+168]` yields PL 1 (`0x…6f24`–`0x…6f38`, then
`max`/`min` at `0x…715c`). A non-zero `[dpm+188]` yields PL 4
(`0x…7144`–`0x…7158`). Otherwise thresholds decide (`0x…73a0`). The result is
clamped by a per-config min/max (`0x…716c`, `0x…717c`). What the two fields
mean, and the threshold arm, were **not decoded**.

### 1.2 What the gates are (ADT `/arm-io/pmgr` `devices`)

| id2 | name | flags | byte1 (level) | byte2 (domain) | id1 |
|---|---|---|---|---|---|
| 456 | VENC-SYS-V | `no_ps` | 0 | 0 | 0 (parent 294 = VENC_SYS) |
| 457 | VENC-SOC-VNOM | `notify_pmp,no_ps` | **1** | **32** | 17 |
| 600 | VENC-SOC-VMID2 | `notify_pmp,no_ps` | **2** | **32** | 17 |
| 459 | VENC-SOC-VMAX | `notify_pmp,no_ps` | **3** | **32** | 17 |
| 602 | VENC-FAB0-VMAX | `notify_pmp,no_ps` | **3** | **33** | 17 |
| 458 | VENC-MEM-FAST | `no_ps` | 0 | 0 | 0 |
| 294 | VENC_SYS | `notify_pmp,perf,b7` | – | 0 | 17 |

m1n1 calls byte1 `unk1_0` and byte2 `unk1_1`. **[C]** ADT. ave1's set is
549/550/601/552/603/551 on VENC1_SYS (364), id1 35.

The code gives these fields their meaning. **[C]** for each:

- `no_ps` (bit 4) = Apple's `kDeviceFlagVirtual`: no register work. See
  `_syncDeviceStatusChange` `tbnz w8,#4` at `0xfffffe0009869ae4` (down) and
  `0x…9fd4` (up), and the assert string `!(device->flags & kDeviceFlagVirtual)`.
- byte2 = **perf-domain ID**. `_deviceIDToPerfDomain` `ldrb w8,[x0,#2]`
  `0xfffffe000986906c` indexes the map at `this+0x2a80`, which the
  `perf-domains` parser fills from record byte 3 (`0xfffffe000984934c`,
  `strh` `0x…9384`). The ADT `perf-domains` table has **32 = `PMP-SOC`, 33 =
  `PMP-FAB0`**, 34..36 = FAB1..3 and 37 = `PMP-DCS`.
- byte1 = the **level** requested in that domain. It becomes the bit number
  in the PMP mask (§1.4, `0xfffffe0009866a94` `ldrb w9,[x0,#1]; lsl`).
- id1 = the **PMP soc-device ID**. `/arm-io/pmp/iop-pmp-nub` `soc-device`
  record 16 has id 17 = `AVE0`; record 31 has id 35 = `AVE1`. `tools/pmp_ptd_map.py` C4.

### 1.3 PMGR: `_enableDevice(459, on)` → `_enableDeviceGated` (`0xfffffe000986932c`)

1. `(flags & 0x30) == 0x30` → assert (`0x…93c8`). 459 is `0x12`, so it passes.
   `level = on ? 15 : …` (`0x…93d8`).
2. `_checkNotifyPMP(459)` (`0xfffffe0009868f90`) is true, so
   `_waitForPMPReadyAction` (`0x…9400`). **macOS blocks until the PMP is up.**
3. Store the byte `desired[die*0x280 + 459] = 15` (`0x…9434`). It returns
   early if the value is unchanged (`0x…9430`).
4. `_syncDevicePerfDomainRequirement({459,die})` (`0x…9450` →
   `0xfffffe000986989c`) finds perf domain 32 (`PMP-SOC`). That domain's
   AP-side level bitmaps are **never populated**: the device-table parser
   skips bitmap setup for domain IDs above 31 (`sub w8,w3,#1; cmp w8,#0x1e;
   b.hi` at `0xfffffe000984b074`–`0x…b080`). So the computed level is 0, and
   `_handleSOCPerfStateRequest` (`0xfffffe000986ae54`) returns on
   "current == requested" (`0x…af04`). **No AP perf-state or TVM action
   happens.** This also explains why the `PMP-*` domains never reach
   `AppleT6000PMGR::setPerfState` (`0xfffffe0009b9cab4`). That function
   accepts only the CPU/APSC domain IDs 2, 5 and 13 and panics on anything
   else (jump table at `0xfffffe0009b9d210` → `.cold.1/2/3`).
5. `_updateDeviceStatus` (`0xfffffe00098530b4`) appends `{459, 15, die}` to the
   change list (`0x…3530`). In 13.5 there is no `no_ps` filter at this point.
6. The notifications before the transition send nothing on power-up
   (`0x…9504`–`0x…9518`: `(level off) xor (b6 clear)` selects the
   *pre* slot only when powering down).
7. `_syncDeviceStatusChange` (`0xfffffe00098699ec`) skips the entry because
   it is virtual. **No register is written.**
8. After the transition, for PMP version 2 (`[this+432] == 2`,
   `0x…95f0`), each entry with `notify_pmp` gets
   `_sendPMPCommand(14, {devID, on = (level == 15)}, die)` (`0x…9658`–`0x…9660`).

`[this+432]` is loaded from a feature table (`0xfffffe000983f698`). That it
equals the ADT `pmp = 2` is **[I]**. `_initPMPv2` only proceeds when it is 2
(`0xfffffe0009840a44`), and the v2 PTD structures exist in this ADT.

Power-down is the mirror: the notification is sent *before* the transition
(command 14, or 15 for `b7` devices, `0x…9524`), with `on = 0`.

### 1.4 The PMP message: `_sendPMPCommand` → `_pmpWriteDashBoard` → `_pmpWriteDashBoardSetVirtualDeviceState`

- `_sendPMPCommand` (`0xfffffe0009858f5c`), v2 path: commands 12, 14 and 15 go
  to `_pmpWriteDashBoard` (jump table `0xfffffe00098590a0`). **[C]**
- `_pmpWriteDashBoard` (`0xfffffe00098665a8`): for commands 14/15 it takes
  `{u16 devID, u32 on}` (`0x…6608`, `0x…660c`). A virtual device goes to
  `…SetVirtualDeviceState`; a real one goes to `…SetDeviceState` (`0x…6620`). **[C]**
- `_pmpWriteDashBoardSetVirtualDeviceState(devID, on, die)` (`0xfffffe00098669bc`):
  - `k = id1 + die * soc-device-die-offset(64)` (`0x…6a24`–`0x…6a2c`);
    `socIdx = tbl[0x3594c + 4k]` and `slot = tbl[0x36154 + 4k]`. Either one
    being −1 means "not a PMP device" (`0x…6a40`, `0x…6a58`). `_initPMPv2`
    builds both tables from the `soc-device` records: `socIdx` = record index,
    `slot` = a running count of records whose word `+44` is non-zero
    (`0xfffffe0009840dd8`–`0x…0e20`). **AVE0: socIdx 16, slot 9. AVE1:
    socIdx 31, slot 18.**
  - byte2 selects one of six 32-bit masks in a 24-byte per-device record:
    32 → word 0 (SOC), 33..37 → words 1..5 (jump table `0x…6ce4`). The code
    sets or clears `1 << byte1` in it (`0x…6a94`–`0x…6ae8`).
  - It builds `msg = hibit(w0) | hibit(w1)<<32 | hibit(w2)<<36 | hibit(w3)<<40 |
    hibit(w4)<<44 | hibit(w5)<<48 | 1<<61`, where `hibit` = index of the
    highest set bit, 0 if empty (`0x…6af4`–`0x…6b88`). The log text is
    `"PTD-SOC-DEV-DVFS: Device=0x%x level=%d msg=0x%llx"`.
  - It calls `ApplePTD::_writePTD(range SOC-DEV-DVFS, range.base + slot, msg)`
    (`0x…6c14`). **There is no doorbell and no wait.**
- `ApplePTD::_writePTD` (`0xfffffe000987aa60`) =
  `writeReg64(RegMap 8, 0x10000 + index*8, msg, die 0)` (`0x…aa78`–`0x…aa84`).
  `ApplePTD::_readPTD` (`0xfffffe000987a9c4`) reads `RegMap 8 + index*16` and `+8`.
- RegMap 8 is ADT pmgr `reg[0x29]`: `initRegMap(8, 0x29, …)` at
  `0xfffffe0009b8ad78`. pmgr `reg[41]` translates to **`0x28e3c0000`**, size
  `0x20000`. The `/arm-io/pmp` node's `ptd-update-reg-index = 3` names
  `reg[3] = 0x28e3d0000`, size `0xc00`, which is exactly the `+0x10000` write
  side. **[C]** (script control C3).

`ptd-range` (`iop-pmp-nub`, 32-byte records) gives `SOC-DEV-DVFS` base 264,
count 40. Its `ptd-ranges` selector in the pmgr node is `[10,11,12,13,2]`,
matched at `0xfffffe0009840c28`–`0x…0c78`. **[C]**

| | ave0 | ave1 |
|---|---|---|
| SOC-DEV-DVFS index | 264 + 9 = **273** | 264 + 18 = **282** |
| write (AP phys) | **`0x28e3d0888`** | **`0x28e3d08d0`** |
| read-back (value, then status at +8) | `0x28e3c1110` | `0x28e3c11a0` |

| AVE state | SOC mask | message |
|---|---|---|
| VMin (456 only) / after release | 0 | `0x2000000000000000` |
| VMid (457) | bit 1 | `0x2000000000000001` |
| VMid2 (+600) | bits 1,2 | `0x2000000000000002` |
| VMax (+459) | bits 1..3 | `0x2000000000000003` |
| VMax + FAB (602) | SOC 3, FAB0 3 | `0x2000000300000003` |

Only one step is inferred here. On the way down from VMid, 457 turning off
sends SOC = 0, which is the `0x2000000000000000` row: **[I]** from the same
code with an empty mask. Nothing is ever sent at VMin itself, because 456 is
not `notify_pmp`.

**VENC-MEM-FAST (458) does nothing on t6001.** Its byte2 = 0, so it has no
perf domain (`0xfffffe0009869078`). Its `notify_pmp` is clear, so no PMP
command goes out, and it is virtual, so no register is written. **[C]**

### 1.5 VENC_SYS itself (294): the power-state half

VENC_SYS is real and `notify_pmp,b7`, so the same step 8 lands in
`_pmpWriteDashBoardSetDeviceState` (`0xfffffe0009866cf8`):

- It reads `SOC-DEV-PS-REQ` (index 248), sets or clears `1 << socIdx` (bit 16
  for AVE0), and writes it back (`0x…6dc0`–`0x…6de8`).
- If soc-device word `+8` has bit 1 set (AVE0 = 3, so yes) it waits
  (`0x…6e8c`). The wait polls `PMP-STATUS` (index 1). If that is non-zero it
  then polls `SOC-DEV-PS-ACK` (index 256) every 100 µs until the bit matches,
  and panics after 15 s (`0x…6eac`–`0x…7054`, the panic text is at
  `0xfffffe0007470c6b`). **If `PMP-STATUS` is 0 it does not wait**
  (`0x…6f8c`).
- Addresses: PS-REQ read `0x28e3c0f80`, write `0x28e3d07c0`; PS-ACK `0x28e3c1000`;
  PMP-STATUS `0x28e3c0010`.

**Independent corroboration.** Asahi's `pmp-report.c` has, for t600x,
`tgt_read = 0xf80, tgt_write = 0x107c0, actual = 0x1000, status = 0x10`.
m1n1's `proxyclient/tools/pmp_assist.py` computes the same four from the ADT.
Both are relative to `0x28e3c0000`. This document derived them from Apple's
code, and they agree (script control C2).

---

## 2. The perf registers are counters (the f73 question)

`ApplePMGR::_getSOCPerfCounter(block, idx, die)` (`0xfffffe00098633a0`):

```
entry  = perfRegs[block]            ; ADT perf-regs {reg, offset, size, unk}, [this+0x2968]
v1 (perf-counter-version != 2):
  ctl  = entry.offset + 0x100 + idx*0x10     0x…33fc, 0x…3400
  lo   = ctl + 8                              0x…3404
  hi   = ctl + 0xc                            0x…3408
v2: 0x2c + idx*0x1c, +0xc, +0x10              0x…3410..0x…341c
read hi, lo, hi until hi is stable            0x…3430..0x…3508
```

The format selector `[this+0x2974]` is the ADT `perf-counter-version`,
**defaulting to 1 when absent** (`csinc` at `0xfffffe000983fa10`). The j314c
ADT has no such property, so t6001 uses **v1**. **[C]**

`_enableSOCPerfCounter` (`0xfffffe00098625e8`), v1 control word:
`bit0 = enable`, `bit1 = !arg4`, with a read-modify-write (`0x…26a8`–`0x…26bc`).
Blocks whose ADT `unk` = 1 also poll `entry.offset + 4` bit 0 afterwards
(`0x…2734`–`0x…2778`). **[C]**

- `flags.perf` (bit 5) is `kDeviceFlagPerfCounter`. `_snapshotDevicePerfCounter`
  does `tbz w8,#5` → assert `"device->flags & kDeviceFlagPerfCounter"`
  (`0xfffffe0009863a08`). The counter it reads is `(perf_block, perf_idx)` =
  `dev[9], dev[8]` (`0x…3b9c`). **[C]**
- Counters come on at boot only for "AON" blocks (`perf-regs` unk 1..3,
  `_enableAONPerfCounters` `0xfffffe00098561c8`–`0x…61d4`). Block 9 has
  unk 0, so under macOS it too is **off until an IOReport client enables it**.
  **[C]**

**So f73 read the right addresses.** VENC_SYS's counter control is
`0x28e5d8340`, inside the window read, and it is 0 because nobody enabled it.
The "no" branch of f73 is therefore settled. The formula is Apple's, and the
zeros carry no DVFS information. No read-only access to block 9 can
discriminate further, because every counter in it is disabled under Linux.
The only discriminator is a write (§7 R2). `+0x004 = 0x35ba9813` is
block-level. Apple reads `+4` only for `unk = 1` blocks. **[U]** meaning;
reading it twice would show whether it is a timer.

Counter addresses (script output): VENC_SYS `0x28e5d8340`, **VENC0 clock
`0x28e5d8410`**, MSR0 `0x28e5d8310`, VENC1_SYS `0x28e6d0330`, VENC1 clock
`0x28e6d0350`. The PMGR `events` counters for the AVEMSR voltage states,
`AVEMSR0_VMIN` … `AVEMSR0_VMAX`, sit in block 8 at `0x28e070460`…`0x28e070530`.
Clock counters are exported to IOReport as `"CLK<id>"`
(`_snapshotClockPerfCounter` `0xfffffe0009863d10`). That they count clock
cycles is **[I]**.

---

## 3. The mechanism, stated plainly

### 3.1 Two channels, both through the PMP's PTD

| channel | index | written when | payload |
|---|---|---|---|
| `SOC-DEV-PS-REQ` | 248 | VENC_SYS (294) goes on or off | bitmask, bit = soc-device index (AVE0 = 16) |
| `SOC-DEV-PS-ACK` | 256 | the PMP writes it | bitmask the AP polls |
| `SOC-DEV-DVFS` | 264 + slot | any VENC pseudo-device with `notify_pmp` goes on or off | per-device level vote, §1.4 |
| `PMP-STATUS` | 1 | the PMP writes it | non-zero means alive (Asahi: bit 0 = ready) |

### 3.2 What the PMP does with it (outside the AP)

**[I].** The PMP arbitrates votes per DVFS domain and moves the rail and the
clocks. The chain: `iop-pmp-nub` `dvfs-domain` lists `AVEMSR0` (12),
`AVEMSR0_AVE` (13) and `AVEMSR0_MSR` (14), each with **4** states. AVE0's
`soc-device` record names domain 13 in its first domain slot (`+24`). AVD0
names 10 (`SOC0_AVD`, 3 states) and MSR0 names 14. The PMGR `events` list
counters `AVEMSR0_VMIN / _F1_F0 / _V1 / _F2_F0 / _F2_F1 / _V2 / _VMAX`, the
same rail. The level counts match. VENC votes up to 3 and its domain has 4
states. AVD votes up to 2 (AVD-SOC-VMAX byte1 = 2) and `SOC0_AVD` has 3.

### 3.3 Things that are *not* on the path

**[C]** for the AP side:

- **AP-side perf domains, `voltage-states*`, TVM.** `enableTVM` is reached from
  `_handleSOCPerfStateRequest` only for domains with flag bit 1 (`0x…af3c`).
  `PMP-SOC` has flags 0, and step 4 of §1.3 returns before that anyway.
  `avemsr-tvm`/`dcs-tvm`/`fab-tvm` are init-time margin settings
  ([26](26-macos-pmgr-sequence.md) §7), not per-request.
- **`ApplePMGR::_setPerfState`** (`0xfffffe000986b4ac`) accepts only ANE/ANE1
  (asserts `perfDomainID == kPerfDomainIDANE || … ANE1`). In 13.5,
  `_syncDeviceStatusChange` calls it only for perf domains with the
  "nub-managed" flag (`0x…a0d0`). VENC_SYS has byte2 = 0, so no perf domain.
  **This corrects [26](26-macos-pmgr-sequence.md) §4.4** ("`_setPerfState(perfDomain[15], dev->id1)`"
  on every UP; that was 26.6.2's shape, or a misread).
- **PMGR clock or PS registers for 456..602**: none (virtual).

### 3.4 Who else is involved

Only the PMP. There is no SEP, SMC or doorbell call on either path
(`_ringPMPDoorbell` is not called from `SetVirtualDeviceState` or
`SetDeviceState`). The one hard dependency is **PMP readiness**. macOS waits
for it before any `notify_pmp` device transition (§1.3 step 2).

---

## 4. Available states and frequencies

- Request levels: 0, 1 (VNOM), 2 (VMID2), 3 (VMAX) in `PMP-SOC`; 3 in
  `PMP-FAB0`. **[C]** The AVE kext's PLs map onto them one-to-one (§1.1).
- ADT `voltage-states0/2/9` are `{index, 0}` pairs for AP-side rails 0, 2 and 9.
  There is no `voltage-states12` (AVEMSR). **No MHz or mV for AVE anywhere in
  the ADT, ApplePMGR or AppleAVE2.** **[U]** Apple's 4K60 rating implies the
  top state is roughly 3× the observed rate. It does not imply that Linux sits
  in the bottom state.
- `ave0 clock-ids = [350]` is not in the pmgr `clocks` table. **[U]**

---

## 5. Linux today

**[C]** from source; `AsahiLinux/linux` branch `asahi`, fetched 2026-09-24.

- `drivers/pmdomain/apple/pmgr-pwrstate.c` touches only the PS register
  (`PS_TARGET`, `PS_ACTUAL`, `AUTO_ENABLE`, `PS_MIN`, reset bits). It has no
  PMP or PTD code.
- `drivers/pmdomain/apple/pmp-report.c` is a genpd per `report@N` child of
  `pmp_report@28e3c0000` (`apple,t6000-pmp-v2-report`, power domain
  `ps_pms_sram`). On power on or off it does a read-modify-write of bit N via
  `tgt_read`/`tgt_write`. If `status & 1` is set it polls `actual` (100 µs,
  50 ms). **It implements §1.5 and not §1.4.** `t600x-die0.dtsi` has
  `pmp_report_venc_sys: report@10` (power domain `ps_venc_sys`,
  **`status = "disabled"`**) and `report@22` "pmp-venc1", also disabled.
  `avd_sys` (report@11) is enabled. Several display/IOA entries are
  `apple,always-on`.
- `drivers/soc/apple/pmp.rs` (`apple,t6000-pmp-v2`) boots the PMP
  (`CPU_CONTROL |= CPU_RUN`) and forwards the `iop-pmp-nub` properties as
  `apple,tunable-*`. m1n1's `dt_set_pmp` (`src/kboot.c:2068`) sets the node
  `okay` if it exists. **Whether this kernel and m1n1 do that on the target is
  [U].** R0 answers it.
- Discrepancy, for ave1 only. Apple sets PS-REQ bit = soc-device *index*
  (31 for AVE1). Asahi's DT uses `id1 − 1` (`0x22` = 34). The two agree for
  every device below id 27, including AVE0 (16). In addition,
  `u64 bit_val = 1 << ent->id` is an `int` shift, undefined for id ≥ 31.
  **Do not use the Asahi venc1 entry as is.**

---

## 6. Safety

- **Voltage.** The AP never writes a voltage, PLL or clock mux on this path.
  It writes a request, and the PMP firmware sequences rail then clock. **[C]**
  for the AP side, **[I]** for the PMP. This removes the obvious hang
  ("clock raised before voltage"). The PMP is the designed agent for exactly
  this.
- **A wrong PTD index is the real hazard.** Neighbouring entries are other
  devices' DVFS slots (272 = DISPEXT1, 274 = AVD0). Other ranges are
  PMP-internal: `PMS-PMGR-PKT` at 16, `SMC-TO-PPM` at 322. A mis-aimed write
  asks the PMP to do something to another block. Use only indices computed by
  `tools/pmp_ptd_map.py` with its controls passing.
- **Access to the PTD block.** It sits in pmgr `reg[41]`, and Asahi puts it
  under `ps_pms_sram`. Before the first read, show that `PMS_SRAM` and `PMP`
  are powered by reading their PS registers in the pmgr window Linux already
  uses (R1a). If `pmp_report` is bound on the target, Linux already reads
  `0x28e3c0010`/`0xf80`/`0x1000` and writes `0x28e3d07c0`. That makes those
  accesses proven, but only in that case.
- **If the PMP is not running:** writes land in the PTD and nothing happens.
  macOS itself tolerates this (`PMP-STATUS == 0` skips the ACK wait). No hang
  is expected, but the experiment would then measure nothing.
- **Order.** Follow macOS: DVFS vote *after* VENC_SYS is on and reported
  (post-transition), and release (`0x2000000000000000`) *before* VENC_SYS is
  gated (pre-transition). A vote left behind after gating costs power. It
  should not hang. **[I]**
- **Mid-encode clock changes** are normal under macOS (`AVE_DPM` retunes at
  runtime). Still, for the first run, apply the vote before `Config` and hold
  it for the whole run.

---

## 7. Ranked proposals (for the lead/operator; reads first)

One variable each. Addresses come from `tools/pmp_ptd_map.py` (controls C1–C4 pass).

| rank | action | predicted | what a "no" means |
|---:|---|---|---|
| **R0** | **No MMIO.** On the target: `find /proc/device-tree -name 'pmp*'`; `cat …/pmp@28e700000/status …/pmp_report@28e3c0000/status` (if present); `grep -i pmp /sys/kernel/debug/pm_genpd/pm_genpd_summary`; `dmesg \| grep -i -e pmp -e ptd` | Either (a) `pmp` okay, `apple-pmp` probed, `pmp-avd-sys`/`pmp-disp0` genpds listed, and `pmp-venc-sys` absent (disabled); or (b) no PMP driver | (b) means the PMP is not running. §1.4 cannot take effect until Asahi's PMP driver is enabled (kernel config + m1n1 DT). That is a prerequisite, not an AVE change. R1b is then still useful as confirmation (STATUS 0). |
| **R1a** | Read `0x28e0802d8` (PMP PS) and `0x28e0802e0` (PMS_SRAM PS). ADT `ps-regs[5]` = pmgr reg0 `+0x200`, psidx 27/28, mask bits set; same window as `venc_sys`'s neighbours that Linux uses | `PS_ACTUAL` (bits 7:4) = `0xf` for both | Either one not `0xf` → the PTD block may be unclocked. **Do not do R1b.** |
| **R1b** | After R1a passes, 64-bit reads (`readq`, as `pmp-report.c` does): `0x28e3c0010` (PMP-STATUS), `0x28e3c0080`/`88`/`90`/`98`/`a0`/`a8`/`b0`/`b8` (DVFS-STATE ×4), `0x28e3c0f80` (+8) (PS-REQ), `0x28e3c1000` (+8) (PS-ACK), **`0x28e3c1110` (+8) (AVE0 DVFS)**, `0x28e3c1120` (AVD0 DVFS, control). Once before power-on and once with the firmware running. | PMP running: STATUS bit 0 = 1. **Controls**: PS-REQ has the always-on report bits set (12 disp0, 13 dispext0, and 29/30 per Asahi's DT numbering). **Bit 16 (AVE0) = 0**, and PS-ACK is the same. **AVE0 DVFS value = 0**, meaning the PMP has never had a VENC vote. | STATUS 0 → PMP not running (see R0). Control bits clear while `pmp_report` is bound → the read side does not mirror writes, and the "value = 0" readings mean nothing. AVE0 DVFS non-zero → someone (iBoot?) voted. Record it; the hypothesis weakens. |
| **R2** | **One PMGR write (a counter, not a state).** Set bit 0 of `0x28e5d8410` (VENC0 clock counter; RMW, bit 1 stays 0 = Apple's `arg4 = 1`). Read `0x28e5d8418`/`0x28e5d841c` (hi, lo, hi) at two times ~1 s apart, idle and while encoding. Optionally the same for `0x28e5d8340` (VENC_SYS) and the AVEMSR0 event counters `0x28e070460`…`0x28e070530`. | A count that increases during an encode. Δ/Δt gives the VENC clock rate **if** it is a cycle counter [I]. The AVEMSR0 events show which rail state is resident. | Still 0 with bit 0 set → wrong control bit or a gated counter block. This changes nothing else, but R4 would then have no direct clock evidence; fall back to timing. This is a PMGR write, so it is the operator's call under AGENTS.md. |
| **R3** | **PS-REQ via existing code.** Overlay: `&pmp_report_venc_sys { status = "okay"; };`. Make it a power domain of ave0 (after `venc_sys`), so `apple-pmp-report` sets bit 16 after `ps_venc_sys` is on and clears it before it is off (the macOS order). No new register code. | PS-REQ bit 16 = 1, PS-ACK bit 16 = 1 within 50 ms, and encode time unchanged or slightly better | ACK timeout → the PMP does not recognise AVE0 at bit 16 (index model wrong) or is not running. Stop and re-derive. |
| **R4** | **The DVFS vote.** After R3 (VENC_SYS on, reported), before `Config`: `writeq(0x2000000300000003, 0x28e3d0888)`. Read back `0x28e3c1110`. Keep R2's counter running. At teardown, before any `venc_*` domain is gated: `writeq(0x2000000000000000, 0x28e3d0888)`. Variant R4′ for a smaller step: `0x2000000000000001` (VNOM). | Read-back equals the value written. The VENC0 clock rate rises. The 1080p Process round trip drops from 14 ms toward ~5 ms, 4K from 54 ms toward ~17 ms (only if the clock is the whole story). | Read-back matches but clock and time are unchanged → the PMP ignores the vote (it may need something macOS does that this document does not trace, e.g. `_waitForPMPReadyAction`) or AVE0's rail is already at max. Clock up but time unchanged → the bottleneck is elsewhere (one frame in flight, memory/fabric; try FAB0 = 3 alone). |

**Not proposed:** ave1 until the PS-REQ bit numbering disagreement (§5) is
resolved. Also not proposed: any write to perf-counter, DVFS or PTD addresses
not produced by the script. Also not proposed: any attempt to set the AVE
clock or rail from the AP. Nothing traced here tells the AP how, and on
this SoC it is the PMP's job.

---

## 8. Corrections to earlier documents (not edited here)

- **[26](26-macos-pmgr-sequence.md) §4.4, §1**: in 13.5 the UP branch calls
  `_setPerfState` only for nub-managed perf domains (ANE). VENC_SYS has no
  perf domain (byte2 = 0), so no AP perf-state write accompanies VENC
  power-on. §3.3.
- **[26](26-macos-pmgr-sequence.md) §7 table**: `flags.perf` (bit 5) is
  `kDeviceFlagPerfCounter`, and `perf_idx/perf_block` locate a PMGR counter.
  `flags.no_ps` is `kDeviceFlagVirtual`. `unk1_0`/`unk1_1` are the requested
  level and the perf-domain ID; `id1` is the PMP soc-device ID.
- **[10](10-power.md) "Performance states"**: the ladder does more than ungate
  clocks. It is a **SoC-voltage vote to the PMP**. 457/600/459 = SOC levels
  1/2/3, 602 = FAB0 level 3. 458 ("DCS"/MEM-FAST) has no effect on t6001.
- **`driver/ave_drv.c` `perf_dump` comment** (and m1n1 `dump_pmgr.py`): the
  window is right. It is a counter window, not a perf-state window. m1n1's
  formula lacks `perf.offset`.

---

## 9. Reproduce

```sh
python3 tools/pmp_ptd_map.py
python3 tools/kext_extract.py data/blobs/macos-13.5/kc.macho --symbols com.apple.driver.ApplePMGR -o /tmp/pmgr.sym
d() { AVE_MACOS=13.5 python3 tools/disas.py --kext --addr "$1" -n "$2"; }
d 0xfffffe0008ee8c4c 0x1f0   # AVE_DPM_SetIOP: PL -> SetPS table
d 0xfffffe000986932c 0x400   # _enableDeviceGated
d 0xfffffe000986989c 0x150   # _syncDevicePerfDomainRequirement
d 0xfffffe000984b038 0x80    # parser: domains > 31 get no AP bitmap
d 0xfffffe0009858f5c 0x178   # _sendPMPCommand (v2 jump table)
d 0xfffffe00098665a8 0x12c   # _pmpWriteDashBoard
d 0xfffffe00098669bc 0x340   # ...SetVirtualDeviceState: mask -> msg -> writePTD
d 0xfffffe0009866cf8 0x3c8   # ...SetDeviceState: PS-REQ, ACK wait
d 0xfffffe000987aa60 0x44    # ApplePTD::_writePTD: RegMap 8, 0x10000 + 8*idx
d 0xfffffe0009b8ad78 0xc     # initRegMap(8, 0x29)
d 0xfffffe0009840dd8 0x50    # _initPMPv2: soc-device index / dvfs slot tables
d 0xfffffe00098633a0 0x180   # _getSOCPerfCounter
d 0xfffffe00098625e8 0x1b0   # _enableSOCPerfCounter
d 0xfffffe000983fa04 0x14    # perf-counter-version default 1
d 0xfffffe0009b9cab4 0x80    # AppleT6000PMGR::setPerfState domain switch
```

## 10. Not determined

- The PMP's state→MHz/mV table for `AVEMSR0_AVE`, and its default with no votes.
- Whether the PMP applies a DVFS vote for a device whose PS-REQ bit is clear
  (hence R3 before R4).
- The meaning of message bit 61 (always set) and of the PTD status word at `+8`.
- The `AVE_DPM_DecideIOPPower` thresholds, i.e. when macOS actually chooses VMax.
- The soc-device word `+8` bit 2 (it gates the ACK wait on power-up only).
- What perf block 9 `+0x004` is.
