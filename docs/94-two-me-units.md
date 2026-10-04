# 94. Two motion-estimation units: why two references stalled, and the fix

*2026-10-04, M2 (t8112). The stall that blocked B-frames and multiple
references since 2026-09-26 (docs/81, docs/92 §2, docs/93) is solved. The
encoder has two motion-estimation units; a frame with two active references
needs both, and the firmware programs the second one only when the session
says so.*

## Result

| run (M2, docs/53 parameters) | before | with `session_multi_me=1` |
|---|---|---|
| bs1: H.264 IPP, frame 2 with two L0 references | `PIPE HANG: 3, 3` at frame 2 | **frame 2 completes** (2580 bytes; 2494 with one reference) |
| hb3: HEVC, two references | hangs at frame 2 | **all four frames complete** |
| b4: H.264 IDR, then {B1, P2} | P2 completes, B1 hangs | **P2, then B1 complete**; decodes I B P, Y 48.59 / **49.56** / 49.27 dB against the source |
| V4L2, `session_dpb=3 session_hevc_refs=2 session_ref_spacing_p=2` | hangs | H.264 and HEVC 60-frame streams complete; HEVC P slices carry `refs l0 2`, RPS {-1, -2} |
| one-reference control | 44.308053 dB | 44.308053 dB (byte-identical: the field changes nothing for one reference) |

The tests that decided it:

- **T1, ME1 off** (`me1_off=1`: venc_me0 held, venc_me1 left off, as macOS
  has it by default). One-reference control 44.308053 dB; **bs1 still hangs**.
  So it is not "ME1 powered but unprogrammed".
- **T2, both units** (`session_multi_me=1`: Start wire 0xFCEA = 1, ME1
  powered as the driver always did). **bs1 completes.** The second reference
  needs the second unit, programmed.
- Emulation first (docs/93's tools): the same snapshot run with the
  firmware's copy of the field (`ctrl+0x23292`) patched to 1 adds 30 writes
  to the ME1 bank (DPE+0xF0000), the same 30 the ME0 bank (DPE+0xE0000)
  gets; with 0 the ME1 bank gets none.

How it was found: docs/93 showed the firmware programs a two-reference frame
completely and every write lands, so the cause had to be outside the
firmware's per-frame code. A static inventory of the macOS kext's hardware
set-up (below) found that macOS powers ME1 per command, only for sessions
that ask for both units (the kext's `iMultiMECnt`, wire 0xFCEA), and that
the H14G firmware gates every ME1-bank write on the same field.

Driver switches (experiment level; the V4L2 controls follow in docs/81's
V4L2 implementation):

| switch | what |
|---|---|
| `session_multi_me=N` | Start wire 0xFCEA (u16) = N: the firmware programs both ME units |
| `me1_off=1` | hold venc_me0 but leave venc_me1 off (macOS's default state); on an unclean teardown the ME0 holder is the one abandoned |

**The M1 Pro (t6000, H13S) has the same fix** (2026-10-04, module reloads
on a machine whose encoder was idle): bs1, hb3 and b4 complete with
`session_multi_me=1`, with frame sizes byte-identical to the M2's (2580;
1207/456/494/413; 2232/2158), and the b4 stream is byte-identical to the
M2's (B frame Y 49.56 dB). The H13S firmware reads the same field
(InitEncodingParameters `ldrh [x22,#0x52a]`) and has the same 46 writes to
the ME1 bank as H14G. The control with the current build matches the
installed one exactly (44.255294 dB).

Speed (V4L2, 720p, fixed QP, 60 frames, per frame):

| | M1 Pro H.264 | M1 Pro HEVC | M2 H.264 | M2 HEVC |
|---|---|---|---|---|
| 1 reference | 2.67 ms | 3.54 ms | 3.22 ms | 4.32 ms |
| 1 reference, `session_multi_me=1` | 2.69 ms | 3.51 ms | | |
| 2 references, `session_multi_me=1` | **5.91 ms** | **6.66 ms** | 3.22 ms | 4.60 ms |

Both ME units cost nothing by themselves; a second reference costs the M1
Pro ~2x, the M2 almost nothing.

Compression with two references (bench/hevc-efficiency, M2, five Xiph
clips, `results-m2-2ref.csv`; PCHIP BD-rate against plain fixed QP on the
same machine, negative = fewer bits):

| variant | VMAF | PSNR-Y |
|---|---|---|
| P-frame QP +3 (docs/92) | -1.6 % | -2.1 % |
| two references, `ave-cqp-x-2ref` | **-0.1 %** | **-0.6 %** |
| two references + P QP +3 | -1.6 % | -2.7 % |

A second P reference buys almost nothing on these clips; its value is that
it makes B-frames possible (worth ~9 % to x265 `medium`, docs/92). Against
x265 `medium` the gap stays ~20 %.

What macOS does differently is that it powers ME1 only while such a session
runs; the driver keeps ME1 on whenever it is loaded, which is harmless (T1/T2).
**ME1's idle power, measured** (M2, 2026-10-04): whole-machine "Total
System Power" (SMC), idle, 90 s per phase after 15 s to settle, three rounds
of driver loaded / loaded with `me1_off=1` / unloaded: 3.157 / 3.145 /
3.116 W (means; the rounds spread by ±0.03 W). Holding ME1 on costs ~12 mW,
below the noise; the whole loaded driver ~40 mW. Not worth per-session
power switching for now.

---

The static analysis follows (kernelcache VAs are the macOS 13.5 M2
kernelcache; [C] confirmed from an instruction, [I] inferred, [U] unknown).

## 0. Verdict

| # | question | answer | label |
|---|---|---|---|
| 1 | Does the kext write any AVE register bank our driver does not? | **No.** On M2 the kext's own MMIO is DPE tunables + DPE enable (bank 0), ASC start/config (bank 1), SVE scratch/doorbell/IRQ/idle (bank 2). Bank 3 (PMGR PS) is never accessed by the CPU and not DART-mapped for the firmware on ChipType 10; bank 4 (AXI2AF) is **not even mapped** for DevType 15. Every one of these writes is already in the driver (§1). | C |
| 2 | Then what differs on the macOS side? | **Power of the pipe sub-domains, per command.** Before every session command the kext powers Pipe4 (AVC) or Pipe5 (HEVC), ME0 always, and **ME1 only if the session's VP byte at wire `0xFCEA` is non-zero**; it powers them down again when idle. macOS user space sends `0xFCEA = 0` (docs/72), so on macOS **ME1 is off during a normal AVC/HEVC encode, B-frames included**. Our driver holds ME1 **on** for the whole session (`power_me1=1`, default since f68). | C (kext), C (user-space default, docs/72) |
| 3 | What is wire `0xFCEA`? | The firmware's **MultiME** switch: `EncCommParams+0x56`. Every firmware write to the **DPE+0xF0000 bank** (the twin of the DPE+0xE0000 bank that gets reference 0/1's per-reference slots in docs/93) is gated on it, including the per-reference slots and the ME config word, which setPipe *mirrors* E→F only when it is set (§3). So: MultiME=0 → firmware programs only the E bank and the kext powers ME1 off; MultiME=1 → firmware programs both banks and the kext powers ME1 on. | C (gates), I (E bank = ME0, F bank = ME1) |
| 4 | Best candidate for the two-reference stall | **ME1 powered but unprogrammed** (our state: MultiME=0 with ME1 on — a state macOS never produces). If the ME hardware uses a powered ME1 for the second reference (or splits work across powered ME units), ME1 runs from reset-default registers while the firmware has only programmed ME0 → stall at the first macroblocks that need the second reference. One reference never needs the second unit, which would explain why P frames with one reference work. | I |
| 5 | Everything else the kext asks platform drivers for | PMGR virtual devices (VNOM/VMAX = perf votes, docs/90 §9; VENC-MEM-FAST/DCS = virtual, no register; FAB/IOP_Mid2 not mapped on M2), DART `setActive`/IOMD software cache attributes, MCC data-stream IDs (Config `+0x58/+0x59`, stored by H14G but no reader found), DART SID remap 1→0. None is a convincing two-reference cause; ranked in §6. | C/I per row |

## 1. Inventory of the kext's MMIO paths (M2)

### 1.1 How registers are mapped

`AVE_Reg::Init` (`0xfffffe0008f11390`) loops over the ADT `reg` entries and
calls `provider->mapDeviceMemoryWithIndex(i, 0)` (vt+0x710, `0x…1142c`) then
`IOMemoryMap::getVirtualAddress()` (vt+0x138, `0x…11470`), storing the VA at
`this+0x38+8i` (`0x…11484`). **[C]** `Read32/Write32/Read64/Write64`
(`0x…11ed0`..`0x…11f64`) index that array by bank (≤4). **[C]**

**Bank 4 is skipped for DevType 15.** At `0x…113ec..0x…1140c`:
`sub w25, DevType, #0x13; … cmp x23,#4; b.ne map; cmn w25,#4; b.hi map;
str xzr,[x19,#0x30]` — bank 4 is mapped only for DevType 16/17/18. M2 is
DevType 15, so bank 4 (`0x266000000`, AXI2AF window) is **not mapped**.
`AVE_AXI2AF::Init` has the same test (`0x…662c4..0x…662dc`, `str xzr,[x19,#0x18]`),
so `AVE_AXI2AF::ApplyTunables` (called from `AVE_HwC::PowerOn`, `0x…ef769c`)
does nothing on M2. **[C]** (DevTypes 16–18 are the ones with
`AXI2AF_RegCfg_Default_Hera_602x` tables. **[I]**)

### 1.2 Every `AVE_Reg` call site (callers.py over the whole kext)

| bank | who | what (M2 path) | ours |
|---|---|---|---|
| 0 DPE | `AVE_DPE::ApplyTunables(j,cfg,n)` `0x…ec640c/642c` (read, bic, orr, write); `AVE_DPE::Enable` `0x…ec6d78` (CAC `|=3`), `0x…ec6fe8` (CAT `|=1`), then polls; `AVE_DPE::Disable` | `Reset` (`0x…ec70e4`): DevType≥8 → type 0 (CAT+CAC Default) `0x…ec73c0`, then type 2 (10-bit) if a 10-bit client is registered, else type 1 (8-bit) if an 8-bit client is, then `Enable` `0x…ec753c`. `Add(bitdepth)` per CHM (`AVE_HwC::SetDPE`, `0x…ef9dd8`): 10-bit → type 2 (`0x…ec7a44`), else type 1 (`0x…ec7ab0`). | `ave_dpe_program()`: Default + 8-bit + enable at probe. Same registers/values for 8-bit sessions. **[C]** (10-bit sessions on macOS get the `CAC_10bit_Atlas_8112` table at `0xfffffe000722be78` instead; not relevant to B-frames.) |
| 1 ASC | `AVE_IOP_Start_Atlas` `0x…eff7a0..7e4`: `+0x400808=1`, `CPU_CONTROL=0`, `+0x400400=0x10000`, `CPU_CONTROL=0x10`; `AVE_IOP_Config_Atlas` `0x…eff604`: Write64 `0x50000`; `CheckIdle`, `GetCurrTime*` reads | identical | `ave_asc_start()` same order/values **[C]** |
| 2 SVE | `AVE_SVECtrl::{SetIOPFlag, WriteScratch, ClearIOPFlag, SendIOPMsg, SetIntr, ClearIntr, SetIdle}`; offsets from `AVE_SVECtrl_GetReg(ChipType)`; ChipType 10 → `gs_sAVE_SVECtrl_Reg_Rhea` (pointer table `0xfffffe0007bd7bb8`, entry 10): `{0xc,0x10,0x8,0x18,…,0x38}` | `SetIdle(v)` = `SVE+0x38 = v` (`0x…f23e1c`, table+0x2c) | same offsets (ave_hw.h). SVE+0x38: macOS writes 0 before every CHM command (§2.1); ours writes 0 only with `session_sve_ungate=1` — **bs8 already tested it on the two-reference case: no change** (docs/53). |
| 3 PMGR PS | only `GetDARTAddr(3,0)` in `MakeFwCmd_Config` `0x…edd26c` → Config `+0x48` | `DARTMap(3)` runs only for ChipType ≤ 5 (`AVE_HwC::Init` `0x…ef22ec..2300`), so on M2 Config `+0x48 = 0` | 0 **[C]** |
| 4 AXI2AF | `ReadReg/WriteReg/ApplyTunables/CheckIdle/BlockTraffic` | bank unmapped on DevType 15 (§1.1) | not touched — matches **[C]** |
| dynamic | `AVE_RegCfg_Apply` `0x…f128c0/28dc` | only via `AVE_HwC::SetRegCfg` (← `AppleAVE2Driver::SetRegCfg` ← `UserClient::Config` feature `0x40000000`, `0x…e82024`) and per-CHM `RegCfgList_Apply` in `SetDPE` (list filled only by `SetRegCfg`). `IO_Config` is **not** in the 13.5 external-method table (`gs_saExternalMethods` `0xfffffe0007bd6388`, 9 entries), so this is unreachable from user space. | — **[C]** (table), **[I]** (dead) |

No other `mapDeviceMemoryWithIndex`, no `ml_io_*`, no raw physical access in
the kext. **[C]** (external-call scan, `ext_calls.txt`; vtable-call scan,
`vt_calls.txt`).

### 1.3 Platform calls the kext makes (non-MMIO)

| call | site | effect on M2 |
|---|---|---|
| `AppleARMIODevice::setDevicePowerState(state, idx)` (vt+0x8c0) | `AVE_PMGR::SetPowerState` | PMGR power state of ADT `power-gates[idx]`; PD→idx map for ChipType 10 = `gs_iaAVE_PMGR_PDMap_Panda` (`0xfffffe000b80f164`, chosen via `0xfffffe0007bd7938`[9]) = IOP→387 VENC-SYS-V, IOP_Mid→368 VENC-AVE-VNOM, DCS→369 VENC-MEM-FAST, Pipe4→193, Pipe5→194, ME0→195, ME1→196, DMA→192, IOP_Max→370 VENC-AVE-VMAX; IOP_Mid2, FAB → −1 (absent). **[C]** |
| `callPlatformFunction("setActive", 1/0)` on the `mapper-ave` DART | `AVE_DART::Init/SetActive/Uninit` `0x…ebc3ac`, `0x…ebf1a4` | DART power/availability (AppleT8110DART) **[C]** |
| `callPlatformFunction("setParameter","iomd-early-reclaim",1)` | `AVE_DART::Init` `0x…ebc5d4` | IODARTMapper software mapping cache **[I]** |
| `callPlatformFunction("setIomdCacheAttribute", md, attr, 1)` | `AVE_DART::SetMapCacheAttr` `0x…ebfb1c`; attrs from `gs_saDARTAttrConversion` = `{1:"iomdEarlyReclaim", 2:"iomdEarlyPurge"}` | software IOMD cache policy, not a hardware cache attribute **[C]** (names), **[I]** (no HW effect) |
| `callPlatformFunction("cacheFlushInactive")` | `AVE_DART::Flush` `0x…ebf5c0` | software **[I]** |
| `AppleARMFunction::withProvider("function-mcc_dataset")`, `MCDataStream` | `AVE_MCC::Init` `0x…f09e70`; `EnableDS` `0x…f0a714` | SLC data-stream allocation; DSIDs go to Config `+0x58/+0x59` (`0x…edd280..294`) **[C]** |
| `function-clock_req_interrupt` (present in the M2 `ave` ADT node) | — | **not referenced by AppleAVE2**; the only user of that string in the kernelcache is AppleH13CameraInterface **[C]** (string scan) |
| PE boot-args, IOSurface, IODMACommand, IOMapper::copyMapperForDevice | various | software |

## 2. The per-command pipe power management (the new part)

### 2.1 What the kext does around every session command [C]

`AVE_HwC::SendFwCmd(CHM*, cmd, size, …)` `0xfffffe0008eddb40`, when the HwC
state `[this+0xa0] == 3` (`0x…eddbf0..bf8`):

1. `AVE_DPM_RetrievePipe(dpm, cmd+0x24)` (`0x…eddc18..c20`) writes a mask of
   the pipe domains that are **currently off** into the command header
   `+0x24`: bit 3 Pipe4, bit 4 Pipe5, bit 5 ME0, bit 6 ME1
   (`0xfffffe0008ecad58..adec`; written only when DPM is started,
   `[dpm+0x18] != 0`, `0x…ecad50`).
2. `AVE_DPM_TuneUpPipe(dpm, codec==0, codec==1, *(u8*)(client+0xE0AC2))`
   (`0x…eddc24..c40`; `codec` = `[client+0xE0D84]`).
3. `AVE_DPM_TuneUpPipe` (`0xfffffe0008eca318`), if `[dpm+0x18]`
   (`0x…eca3b4`): `SetPS(Pipe4, ClockOn)` if AVC (`0x…eca3cc`),
   `SetPS(Pipe5, ClockOn)` if HEVC (`0x…eca3e4`), **`SetPS(ME0, ClockOn)`
   always** (`0x…eca3f8`), **`SetPS(ME1, ClockOn)` only if the client byte is
   non-zero** (`cbz w22` `0x…eca3fc`, `0x…eca410`), then
   `SetClockGating(false)` = SVE+0x38 = 0 (`0x…eca41c`).
4. `AVE_DPM_TuneDownPipe` (`0xfffffe0008eca72c`, from `AVE_HwC::Process`)
   powers them down: with no MultiME client, ME1 → PowerOff (or ClockOff if
   `RegOptCnt`) (`0x…ecac00..ac1c`); with no commands pending, ME1, ME0,
   Pipe4/5 down (`0x…ecac30..ac9c`).
5. DPM is started by `AVE_DPM_PowerOn` (`0xfffffe0008ecc018`): only
   `SetPS(DMA, ClockOn)` (`0x…ecc0e4`), the IOP/DCS/FAB perf levels, then
   `AVE_DPM_Start` sets `[dpm+0x18]=1` (`0x…ec89c4`). Nothing else ever
   powers PD 6 except `AVE_HwC::ShutDownIOP` (all four ClockOn before Halt,
   `0x…ef69a4..69e0`). **[C]** (complete `SetPS` caller list, callers.py.)

So on macOS the firmware boots and takes Config with Pipe4/Pipe5/ME0/ME1
**off**; the first CHM command powers Pipe4 (+Pipe5 as ME0's ADT parent) and
ME0; ME1 only for a MultiME session. **[C]** for the kext; the ADT parents
(ME0/ME1: PIPE5+PIPE4) from the j473 pmgr `devices` table. **[C]**

### 2.2 The client byte is wire 0xFCEA [C]

- `AVE_Client_Config` copies the user's `AVE_VIDEO_PARAMS` (0xFED0 bytes) to
  `client+0xD0E38` (`memcpy` `0x…eae630`, source `in+0x7B0`).
- Start_AVC and Start_HEVC copy that block to **cmd+0x60**
  (`0x…e8bfa8..bfac`, `0x…e8c638..c648`), i.e. wire = VP + 0x60.
- `client+0xE0AC2` = VP + `0xFC8A` = **wire 0xFCEA**. The kext's own name for
  it: `AVE_CHM_CalcPipeStats` (`0x…e96fd0`) counts it as **`iMultiMECnt`**
  (format string "… iMultiMECnt: %d iRegOptCnt: %d").
- No kext instruction writes `client+0xE0AC2` (offset scan) — it is purely
  the user-space value. **[C]** for the scan, **[I]** for "no other writer".
- macOS user space leaves VP+0xFC8A = 0 on the default H.264 path
  (docs/72 table, US `0x29aa8`). **[C]** (docs/72)

## 3. What the firmware does with MultiME (H14G) [C]

`CAVCController::InitEncodingParameters`: `ldrh w8,[x27,#0x52a]` (x27 = VP+0xF760,
so VP+0xFC8A) → `strh w8,[x26,#0x56]`, x26 = ctrl+0x2323C = EncCommParams
(fw `0x4e6a0..0x4e6b0`, `0x4e930`..`0x4e93c`). So MultiME = `ctrl+0x23292`.

Every firmware access to the **0x11F0000** window (= AP `0x2671F0000` =
DPE+0xF0000) is gated on that byte, each sitting next to an unconditional
access to the same offset in the **0x11E0000** window (DPE+0xE0000):

| fw VA | function | what |
|---|---|---|
| `0x4936c..0x49388` | `CAVCController::setPipe` | ME config word (search range bits, `ctrl+0xF7C` etc.) → `0x11E0000`; **if `[x17,#0x4e2]` (MultiME) → same word to `0x11F0000`** |
| `0x46ca4..0x46cbc` | setPipe | per-reference slots: E bank always, F bank (`0x11F000C+4i`, `0x11F4118+4i`) only if MultiME |
| `0x46800..0x46838`, `0x473c0..0x473cc`, `0x4b090..0x4b0ac` | setPipe | more E/F pairs, F gated |
| `0x43584..0x435c0`, `0x436b0`, `0x43778`, `0x43814` | `CAVCController::ProcessPipeReset` | F-bank resets gated on `[x20,#0x2e]`, x20 = ctrl+0x23264 (`0x43420`) → `ctrl+0x23292` = MultiME |
| `0x1f410`, `0x1f56c` | `CAVECommonController::program_async_gmv_params` | F writes gated on `#0x4e2` |
| `0x60f94`.., `0x61564`.., `0x641c4`.., `0x78368` | HEVC `setPipe`, `ProcessPipeReset` | same pattern |

(`f_bank_gates.txt` lists all 49 constant sites; the 19 without a gate in the
preceding 60 instructions are address set-ups whose stores are gated at the
store, e.g. `0x46c18/0x46c38` are spilled and consumed at `0x46cac` after
`cbz` `0x46ca8`. **[C]** for the ones read; not every one was walked. **[I]**
for "all".)

docs/93's two-reference diff wrote `0x2671E0010` (E bank, reference-1 slot)
and nothing in `0x2671F….` — exactly the MultiME=0 pattern. **[C]** (docs/93
table)

**Reading:** E bank = ME0, F bank = ME1 (two identical motion-estimation
units; the kext powers ME1 iff the firmware programs the F bank). **[I]**,
strong: two independent binaries couple the same flag to "ME1 power" and
"F-bank programming".

## 4. Platform drivers on t8112

### 4.1 PMGR (ApplePMGR / AppleT8110PMGR)

| AVE PD | ADT dev | flags / perf (j473 ADT) | effect | ours |
|---|---|---|---|---|
| IOP | 387 VENC-SYS-V | no_ps, parent VENC_SYS | refcount on VENC_SYS (real PS, notify_pmp) | genpd venc_sys |
| IOP_Mid | 368 VENC-AVE-VNOM | no_ps, notify_pmp, level 1 / domain 1 | SOC perf vote (docs/90 §9: SOC already at top) | none (measured no effect) |
| IOP_Max | 370 VENC-AVE-VMAX | no_ps, notify_pmp, level 2 / domain 1 | same | none |
| DCS | 369 VENC-MEM-FAST | no_ps only, level 0 / domain 0 | **nothing**: virtual, no perf domain, no PMP notify (same code path as t6001's 458, docs/75 §1.4) | — |
| FAB, IOP_Mid2 | — | PDMap −1 | no-op | — |
| DMA | 192 VENC_DMA (psreg 12 idx 0) | real | on from DPM_PowerOn | on |
| Pipe4/Pipe5 | 193/194 | real | per command (§2) | always on |
| ME0 | 195 (power-controller@8018) | real, parents PIPE5+PIPE4 | per command, always for a session | always on (holder) |
| **ME1** | **196 (power-controller@8020)** | real, parents PIPE5+PIPE4 | **only with MultiME** | **always on (holder, `power_me1=1`)** |

**[C]** ADT and kext tables; DCS "nothing" is **[I]** from the identical ADT
flags and the t6001 ApplePMGR analysis (docs/75).

### 4.2 DART / SMMU (AppleT8110DART + PPL `t8110dart`)

- ADT `dart-ave`: `sid` = 0 and 15, `bypass-15`, `remap = 1`, `vm-base`
  0x800000000, no `dart-tunables-instance-*`, no `real-time`,
  `trans-idle-timeout`, `retention`. **[C]**
- `remap` is parsed as byte pairs `(src,dst)` (`_dartSetup`
  `0xfffffe0009b375ac..0x…b376c4`, length must be a multiple of 4): `01 00 00 00`
  → **SID 1 remapped to SID 0**. Linux instead attaches streams 0 and 1 to
  the same domain (variant=4). Functionally the same translations; TLB
  sharing differs. **[C]** parse, **[I]** equivalence.
- `_smmuSetupInstance` (`0xfffffe0009b38420`) only maps the SMMU registers;
  `_smmuSetup` sets a software flag. No SMMU writes in the kext. **[C]**
- PPL `t8110dart` (descriptor `0xfffffe0007b34048`, init
  `0xfffffe0008bea2e0`) per-instance set-up (`0xfffffe0008bee608`):
  DART`+0x80 = 0`, ADT tunables (none for dart-ave), `+0x228/+0x22c`
  saved/restored, optional "client partition" blocks written at
  `inst<<6 | {0x8,0xc,0x10,0x14}` and a 0x333/0xFFFF table (only when the
  kext supplies partitions — not for dart-ave **[I]**), DART`+0x104 = 0`,
  then SID_CONFIG (TCR) / TTBR writes. The exact TCR bits PPL writes for SID 0
  were **not** decoded **[U]**; compare with Linux's TCR via `regdump=1`
  if this is ever pursued.

### 4.3 MCC data streams

Config `+0x58/+0x59` = `AVE_MCC::GetDSID(0)` twice (`0x…edd280..294`).
H14G `ProcessConfig` stores them to `this+0x560/0x564` (fw `0xb4b0..0xb4c0`);
`ProcessInitStage2` passes them to the controller's vt+0x150 (fw
`0xe998..0xe9bc`), a setter that stores them at `ctrl+0x241d0`
(fw `0x546c8`). **No reader of `ctrl+0x241d0/0x241d4` was found** in H14G.
**[C]** for the chain, **[I]** for "unused". Our `session_dsid` = 0.

## 5. Header `+0x24`

On macOS the CHM command header `+0x24` carries the RetrievePipe "was off"
mask (§2.1). H14G's flow-controller handlers read priority at `+0x20`
(`SetClientPriority` callers, e.g. fw `0xbe0c`) and **no `CFlowControllerBase`
handler reads host header `+0x24`** (scan). **[C]** scan, **[I]** unused.
Ours: 0, which is what macOS would send when every pipe is already on.

## 6. Candidates, ranked

| rank | candidate | macOS | ours | evidence | why it could matter for 2 refs |
|---|---|---|---|---|---|
| **1** | **ME1 (VENC_ME1, PMGR PS `0x23b708020`) power during a MultiME=0 session** | **off** (only ME0 powered per command) | **on** (`power_me1=1`) | kext `0x…eca3fc/a410`, `0x…eddc38`; fw gates §3 | A powered but unprogrammed ME1 is a state macOS never creates. If the ME hardware dispatches the second reference (or part of the search) to any powered ME unit, ME1 runs from reset defaults → stall right after the first MBs that need ref 1. One reference never uses it. Same kext/firmware logic on t6001 (PDMap Nyx), so it also explains the M1 Max. |
| **2** | **MultiME = 1** (wire 0xFCEA) with ME1 on | 0 by default; when 1 the firmware mirrors ME0's config/per-ref slots into ME1 (F bank) | 0 | fw `0x49380`, `0x46ca8`, `0x43588` | The consistent alternative state: both units programmed. |
| 3 | Pipe power sequencing at firmware boot/Config | Pipe4/5/ME0/ME1 **off** while the firmware boots and runs Config; powered just before the first session command; idled between bursts | all on from probe | §2.1 | The firmware may initialise units it finds powered at boot differently; low, untestable without restructuring power-up. |
| 4 | DART SID 1 → SID 0 remap | remap | SIDs 0 and 1 share a domain | §4.2 | translation identical; low. |
| 5 | MCC DSIDs (Config +0x58/+0x59) | MCDataStream IDs | 0 | §4.3 | no reader in H14G; very low. |
| — | eliminated | SVE+0x38 ungate (bs8), PMP/perf votes (docs/90 §9), VENC-MEM-FAST (virtual), AXI2AF (unmapped on DevType 15), bank 3 to firmware (ChipType ≤5 only), DPE tunables (same), SetRegCfg/SetDMACfg (unreachable), IOMD cache attributes (software), `function-clock_req_interrupt` (unused by AVE) | | | |

## 8. Things not determined

- The exact PPL `t8110dart` SID_CONFIG/TTBR values for dart-ave **[U]**.
- Whether the ME hardware decides unit usage from power state **[U]** — that
  is what T1 tests.
- Whether macOS user space ever sets MultiME (e.g. above some resolution)
  **[U]**; only the default path (0) is known. *Later (docs/89 §8.3): no
  user-space writer sets it at all **[C/I]**, so macOS runs every B frame with
  MultiME 0 and ME1 off; a B frame in that state has not been run here (T1
  was a two-reference P frame).*
- 13.5 H14G consumer of header `+0x24` and of the MCC DSIDs: none found **[I]**.
