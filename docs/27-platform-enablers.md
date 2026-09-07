# Platform-level kexts as candidate AVE enablers

Static analysis only. No hardware was touched: nothing was inserted, no overlay
was applied, no register was read. Everything below comes from
`data/blobs/kc.macho` and `data/blobs/adt.bin`.

**Question.** [25-bringup-results.md](25-bringup-results.md) established that
any access to the AVE block hangs the SoC fabric even with all Linux power
domains on, and concluded that the enabler must be at platform level, outside
`AppleAVE2.kext`. This document examines the four candidates named in that
conclusion.

**Verdict up front: none of them is it.** All four are eliminated, three of
them with a decisive control case. The investigation did, however, produce two
corrections to the map of AVE's own `reg` ranges that matter more than the
negative result — see §6.

---

## 0. Verdict table

| Candidate | What it actually does | Prerequisite for AVE MMIO? |
|---|---|---|
| `AppleT6000SOCTuner` | Watches PMGR device power-state changes for **display, ANE, CIO/Thunderbolt, audio-LLT, VddCio, AFNC1, AFR** and forwards to `ApplePMGRNub` / the MCC platform function. Its ADT device sets contain **no VENC device**. | **No** |
| `AppleT6000` | `AppleT600xIO::processDevice` is a bare super-call. The `VENC0/1 …` strings are entries in `_mcDataStreams`, the memory-cache **data-stream (DSID) name table**, not an enable sequence. | **No** |
| `AppleARMPlatform` | `AppleARMIODevice` consumes exactly `dma-coherent, clock-ids, clock-gates, service-gates, service-reset, service-name, power-gates, pin-groups`. `ave0` carries only `clock-gates`, `clock-ids`, `power-gates` of that set — all already modelled in Linux. There is no generic per-device tunable mechanism it could be hiding. | **No** |
| `coprovider-group = "ave"` | An IOKit **service-discovery** key used by `AppleARMPE::callPlatformFunction("AppleARMPEFindCoprovider"/"…Remove…")`. Pure software grouping of sibling instances. | **No** |
| `function-mcc_dataset` (`M$DS`) | Allocates system-level-cache **data-stream IDs** from `/arm-io/mcc`. Called at step 12 of `AVE_HwC::Init`, i.e. *after* AVE registers have already been read and written. | **No** |
| *(bonus)* PMGR fabric bridges | Real per-device mechanism, and VENC **is** in the table — but `applyBridgeTunables` is an empty stub on this SoC. | **No** |

---

## 1. `com.apple.driver.AppleT6000SOCTuner` (file offset `0x6b0030`)

Two classes, `AppleSOCTuner` (base) and `AppleT6000SOCTuner` (override).
Segment layout: `__TEXT` VA `0xfffffe00076b4030`, `__TEXT_EXEC` VA
`0xfffffe0009b7bbd0` (`0x1a8c` bytes), `__DATA` VA `0xfffffe000c728f78`.

### 1.1 What it binds to

Its ADT node is `/arm-io/pmgr/soc-tuner`, `compatible = "soc-tuner,t6000"`,
phandle 155 — a **child of `/arm-io/pmgr`**, so its provider is an
`ApplePMGRNub`. Confirmed by the tail of `AppleSOCTuner::start`
(`0xfffffe0009b7c82c`), which at `0xfffffe0009b7cddc`–`0xfffffe0009b7cde0`
loads the saved provider from `this+144` and calls
`ApplePMGRNub::enableDeviceStatusChangeNotifications()`
(`0xfffffe0009817618`). It has **no `reg` property**, so it performs no MMIO of
its own.

### 1.2 Where the values come from

`AppleSOCTuner::start` reads, in order (each via `getDTProperty`
`0xfffffe0009b7ce18`, then a same-named `PE_parse_boot_argn`
`0xfffffe000c000f14` override):

| name | string VA | flag stored | ADT read at | boot-arg override at |
|---|---|---|---|---|
| `soc-tuning` | `0xfffffe00076b4bee` | `this+153` | `0xfffffe0009b7c880` | `0xfffffe0009b7c8ac` |
| `fb-caching` | `0xfffffe00076b4bf9` | `this+154` | `0xfffffe0009b7c8d8` | `0xfffffe0009b7c904` |
| `mcc-power-gating` | `0xfffffe00076b4c04` | `this+155` | `0xfffffe0009b7c930` | `0xfffffe0009b7c95c` |
| `dcs-power-gating` | `0xfffffe00076b4c15` | `this+155` | *(none — boot-arg only)* | `0xfffffe0009b7c988` |
| `soc-tuner-debug` | `0xfffffe00076b4c26` | `this+136` | *(none)* | `0xfffffe0009b7c9cc`, gated on `PE_i_can_has_debugger` (`0xfffffe000c00313c`) |

(The first three are present in `/arm-io/pmgr/soc-tuner` as `soc-tuning = 0`,
`fb-caching = 0`, `mcc-power-gating = 1`; `dcs-power-gating` and
`soc-tuner-debug` are not ADT properties at all on this board.)

`AppleT6000SOCTuner::initTuner` (`0xfffffe0009b7be18`) reads four more from the
same node: `fr-scaling-wa` (`0xfffffe00076b46d4` → `this+308`), `cio-config`
(`…46e2` → `this+309`), `usb-audio-wa` (`…46ed` → `this+310`), `vdd-cio-pg`
(`…46fa` → `this+311`).

`start` then builds three `AppleARMFunction` handles via
`AppleARMFunction::withProvider` (`0xfffffe0008a901c0`):

- `function-mcc_ctrl` (`0xfffffe00076b4c6d`) at `0xfffffe0009b7caec` → `this+200`
- `function-dispidle_ctrl` (`0xfffffe00076b4cd7`) at `0xfffffe0009b7cca4` → `this+216`
- `function-error_handler` (`0xfffffe00076b4705`) at `0xfffffe0009b7bf00` → `this+312`

In the ADT, `function-mcc_ctrl = 129:Mem$()` (phandle 129 = `/arm-io/mcc`) and
`function-error_handler = 133:Afre()` (phandle 133 = `/arm-io/error-handler`).

The remaining `start` work is IOReporting boilerplate: `IOStateReporter::with`
(`0xfffffe000bf9e3fc`), `IOReporter::addChannel`, `IOReportLegend`
(`0xfffffe000bfa1474`, `0xfffffe000bfa1c4c`).

### 1.3 What it tunes, and for which devices

`AppleT6000SOCTuner::handleDeviceStatusChange(uint*)` (`0xfffffe0009b7bf20`,
`0x75c` bytes) is the only substantive method. Every `bl` in it resolves to one
of exactly eight targets:

```
0xfffffe0009b7d0ec  AppleSOCTuner::enableFrScaling(bool)
0xfffffe0009b7d104  AppleSOCTuner::enableAfnc1PowerGate(bool, uint)
0xfffffe0009b7d110  AppleSOCTuner::enableCioReconfig(uint, bool)
0xfffffe0009b7d11c  AppleSOCTuner::enableVddCio(bool, uint)
0xfffffe0009b7d128  AppleSOCTuner::enableLltLimit(bool, uint)
0xfffffe0009b7d214  AppleSOCTuner::enableMCCPowerGating(bool)
0xfffffe0009b7d364  AppleSOCTuner::restoreANE(uint, uint)
0xfffffe000bed5768  IOLog
```

The `AppleSOCTuner::enable*` methods in the `0xfffffe0009b7d0bc`–`…d160` block
are three-instruction forwarders — `bti c; ldr x0,[x0,#144]; b <ApplePMGRNub…>`
— straight into `ApplePMGRNub::enableDeviceAutoPowerGating`,
`…AutoClockGating`, `enableIdleHandshake`, `enableMarconi`, `enableFrScaling`,
`enableSlpDdrForS2R`, `enableAfnc1PowerGate`, `enableCioReconfig`,
`enableVddCio`, `enableLltLimit`, `enableMtpClockSlowdown`, `resetAtcPs`,
`enableGpuSyncWin`, `setSOCSleepState` (`0xfffffe0009815b08` … `0xfffffe0009815ea4`).

The three MCC methods instead call the `function-mcc_ctrl` handle
(`this+200`) through its vtable slot `+0x140` with a small selector:
`setMCCConfig` selector **1** (`0xfffffe0009b7d194`), `enableMCCPowerGating`
selector **2** (`0xfffffe0009b7d238`), `cleanMCCDisplayDataSet` selector **3**
(`0xfffffe0009b7d2a8`). `hintFBCaching` (`0xfffffe0009b7d2ec`) uses the
`function-dispidle_ctrl` handle at `this+216`.

**Which devices it watches.** The ADT node declares `#device-sets = 21` and
`device-set-0 … device-set-20`. Resolving each id against
`/arm-io/pmgr` `devices` (values `≥ 0x10000000` are the die-1 mirror):

| set | id | PMGR device |
|---|---|---|
| 0 | 277 | `DISP0_FE` |
| 1 | 491 | `FR-SCALING-V` |
| 2–5 | 530–533 | `CIO0..3_RECONFIG-V` |
| 6–9 | `0x10000000\|530..533` | die-1 CIO reconfig |
| 10 | 455 | `USB-AUDIO-V` |
| 11 | `0x10000000\|455` | die-1 |
| 12 | 483 | `AUDIO-LLT-V` |
| 13 | `0x10000000\|483` | die-1 |
| 14 | 55 | `ANE_SYS` |
| 15 | 365 | `ANE1_SYS` |
| 16 | `0x10000000\|55` | die-1 ANE |
| 17 | `0x10000000\|365` | die-1 ANE1 |
| 18 | 522 | `VDD_CIO-V` |
| 19 | `0x10000000\|522` | die-1 |
| 20 | 39 | `AFR` |

These 21 sets correspond one-to-one with the 21 `AppleSOCTuner: New … state:
%u` format strings at `0xfffffe00076b47aa`–`0xfffffe00076b4b43`.

**No VENC device appears anywhere in the tuner's ADT node.** The VENC PMGR
devices are ids 294, 300–304, 364, 367–371, 456–459, 501, 508, 518, 549–552,
600–603; none of them is a `device-set-N` value, and `soc-tuner` has no other
device-bearing property.

*Test discrimination (Trap 2):* the same lookup produces correct, meaningful
names for every id that **is** present (`DISP0_FE`, `ANE_SYS`, `AFR`, …), so it
would have found a VENC id had one been there.

**Verdict — not a prerequisite.** `AppleT6000SOCTuner` performs no MMIO, never
names a VENC device, and everything it does reaches the hardware through
`ApplePMGRNub` or the MCC platform function, neither of which touches AVE's
address space.

---

## 2. `com.apple.driver.AppleT6000` (file offset `0x6923b0`)

Segments: `__TEXT` VA `0xfffffe00076963b0` (+`0x933a`), `__cstring`
`0xfffffe0007696ac8` (+`0x79ef`), `__TEXT_EXEC` VA `0xfffffe0009aa26a0`
(+`0x10204`), `__DATA` VA `0xfffffe000c6f9d10` (+`0xe9f0`).

### 2.1 The VENC strings — corrected attribution

The eight strings are at `0xfffffe000769c2ab` (`VENC0 RD`), `…2b4`
(`VENC0 WR`), `…2bd` (`VENC0 DCS RD`), `…2ca` (`VENC0 DCS WR`), `…2d7`, `…2e0`,
`…2e9`, `…2f6` (the `VENC1` equivalents). All are inside AppleT6000's
`__cstring`.

Chased through chained fixups (Trap 5: the low 32 bits of a `__DATA` pointer
are the image-relative **file offset**), the only references to them are single
pointers at file offsets `0x56fe0e0`, `0x56fe178`, `0x56fe210`, `0x56fe2a8`,
`0x56fe340`, `0x56fe3d8`, `0x56fe470`, `0x56fe508` — a stride of `0x98`.
Mapping back to VAs, those live inside:

```
_mcDataStreams                                   0xfffffe000c6fa278
AppleT6000PlatformErrorHandler::_fabricCommands  0xfffffe000c6f9dd8  (0x4a0 bytes)
```

`_mcDataStreams` spans `0xfffffe000c6fa278` to the next `__DATA` symbol
`__realmain` at `0xfffffe000c708380` — `0xE108` bytes, exactly **379 × `0x98`**.
The `VENC0 RD` name pointer is at entry 212 offset `+0x88`. Each entry also
carries an 8-byte tag at `+0x80`:

| entry | tag | name |
|---|---|---|
| 212 | `AFVEN0RD` | `VENC0 RD` |
| 213 | `AFVEN0WR` | `VENC0 WR` |
| 214 | `DCVNC0RD` | `VENC0 DCS RD` |
| 215 | `DCVNC0WR` | `VENC0 DCS WR` |
| 216–219 | `AFVEN1RD` … `DCVNC1WR` | `VENC1 …` |

So the correct owner is **`_mcDataStreams`, not `_fabricCommands`** — the two
symbols are `0x4a0` bytes apart, which is why the earlier reading placed them
"near" the error handler. `_mcDataStreams` is consumed by
`MCPolicyMgr::postDataStreamEvaluateAction(MCDataStream*)`,
`MCPolicyMgrT6000::updateDataStreamQuota`, `…getDataStreamCacheUsage` and
`AppleT6000MemCacheController::deprioritizeDSID` — i.e. these are
**system-level-cache data-stream (DSID) descriptors** used for SLC quota
accounting, in the AMCC/MCC register domain. `AF…` = Apple Fabric stream,
`DC…` = DCS stream.

The `25-bringup-results.md` reading ("fabric agent labels used for error and
performance counting… not an enable sequence") reaches the right conclusion for
a slightly wrong reason: they are **memory-cache** stream labels, and the
conclusion that they are not an enable sequence stands, now on firmer ground.

### 2.2 Does `AppleT6000` do anything per-device at boot?

`AppleT600xIO` is the per-SoC arm-io platform driver. Its per-device hooks:

- `AppleT600xIO::processDevice(IOService*)` — `0xfffffe0009aa303c`, **`0x20`
  bytes**: loads a GOT'd base-class vtable and `braa`s straight to the
  superclass implementation (`0xfffffe0009aa3040`–`0xfffffe0009aa3058`). It
  adds nothing.
- `AppleT600xIO::enableDeviceClock(uint, uint)` — `0xfffffe0009aa305c` — and
  `enableDevicePower(uint, uint, uint*)` — `0xfffffe0009aa3130`. Both lazily
  create an `AppleARMFunction` (cached at `this+240` / `this+248`) and call it
  through vtable slot `+0x140` with `(device, state)`. Generic clock/power
  gating; no AVE-specific path, no MMIO of its own.

The remaining classes in the kext are `AppleT6000MemCacheController` +
`MCPolicyMgrT6000` (AMCC/SLC, §5), `AppleT6000PlatformErrorHandler` (AMCC plane
error decode — see `_amccPlaneDecodeTagPipeAFError`, and the ADT
`function-error_handler = 133:Afre()` → `AppleT6000PlatforErrorAfrEnableFunction`,
"AFR enable"), and `AppleT6000SleepPower`.

**Verdict — not a prerequisite.** The VENC references are cache-stream names.
There is no per-device boot action beyond the generic clock/power gate that
Linux already performs.

---

## 3. `com.apple.driver.AppleARMPlatform` (file offset `0x1628c0`)

The generic device wrapper is `AppleARMIODevice`. The complete list of ADT
properties it consumes sits contiguously in its `__cstring`:

```
0xfffffe000716c1e2  dma-coherent
0xfffffe000716c1ef  clock-ids
0xfffffe000716c1f9  clock-gates
0xfffffe000716c205  service-gates
0xfffffe000716c213  service-reset
0xfffffe000716c221  service-name
0xfffffe000716c238  power-gates
0xfffffe000716c244  pin-groups
```

`/arm-io/ave0` declares `clock-gates`, `clock-ids` and `power-gates` from that
list, and nothing else. Both gate lists are identical:

```
456 VENC-SYS-V, 457 VENC-SOC-VNOM, 458 VENC-MEM-FAST,
301 VENC_PIPE4, 302 VENC_PIPE5, 303 VENC_ME0, 304 VENC_ME1, 300 VENC_DMA,
459 VENC-SOC-VMAX, 600 VENC-SOC-VMID2, 602 VENC-FAB0-VMAX
```

That is the 11-entry list already recorded in
[10-power.md](10-power.md). There is **no additional generic tunable
mechanism**: no `*-tunables` string exists in `AppleARMPlatform`'s `__cstring`,
and `ave0` carries no property that the platform expert reads and Linux does
not.

The one generic mechanism that *is* present is `AppleARMFunction` — the
consumer of every `function-*` property.
`AppleARMFunction::withProvider(IORegistryEntry*, const char*)`
(`0xfffffe0008a901c0`) resolves `{phandle, 4-char name, args}` into a callable
object; the callers seen above (`function-mcc_ctrl`, `function-dispidle_ctrl`,
`function-error_handler`, and AVE's own `function-mcc_dataset`) all go through
it. It is a call-forwarding primitive, not a device-enable step.

**Verdict — not a prerequisite.**

---

## 4. `coprovider-group = "ave"`

Consumed by `AppleARMPE::callPlatformFunction` (`0xfffffe0008a9b714`). Three
`OSSymbol` globals are created in `__GLOBAL__sub_I_AppleARMPE.cpp`
(`0xfffffe0008a9c1c8`):

```
0xfffffe000c6987a8  gAppleARMPEFindCoproviderKey    <- "AppleARMPEFindCoprovider"   (0xfffffe000716c786)
0xfffffe000c6987b0  gAppleARMPERemoveCoproviderKey  <- "AppleARMPERemoveCoprovider" (0xfffffe000716c79f)
0xfffffe000c6987b8  gAppleARMPECoproviderGroupKey   <- "coprovider-group"           (0xfffffe000716c7ba)
```

`callPlatformFunction` compares its function symbol against the first two
(`0xfffffe0008a9b744`, `0xfffffe0008a9b758`). In both branches it casts an
argument to a registry entry, calls `getProperty(gAppleARMPECoproviderGroupKey)`
through vtable slot `+0x2c8` (`0xfffffe0008a9b80c` and `0xfffffe0008a9b8a0`),
then `getCStringNoCopy` on the result (slot `+0x198`). It is a **string-keyed
lookup of sibling IOServices** — find/remove a service belonging to the same
group.

Every node in the ADT carrying the property:

```
/arm-io/scaler0, /arm-io/scaler1  -> "scaler"
/arm-io/apr0,    /arm-io/apr1     -> "apr"
/arm-io/jpeg0,   /arm-io/jpeg1    -> "jpeg"
/arm-io/ave0,    /arm-io/ave1     -> "ave"
/arm-io/avd0                      -> "avd"
```

**Control case:** `avd0` carries `coprovider-group = "avd"`, and Asahi's AVD
driver reaches AVD's registers with no coprovider registration whatsoever. The
property therefore cannot be a hardware prerequisite for a block's address space
responding.

**Verdict — not a prerequisite.** It is IOKit service discovery for multi-
instance engines.

---

## 5. `function-mcc_dataset` — phandle 129, `M$DS`

**Phandle 129 is `/arm-io/mcc`** (`compatible = "mcc,t6000"`, eight `reg`
windows of 16 MB each, `config-data`, `dramcfg-data`). Its driver is
`AppleT6000MemCacheController` — the AMCC / system-level-cache controller, with
`_mccReadReg32/_mccWriteReg32(amcc_aperture_t*, …)`, `_mccFlush`,
`mccEnableCacheMode`, `getDSIDGroupQuota`, `setDSIDGroupQuota`,
`deprioritizeDSID` and the `_mcDataStreams` table from §2.1.

`function-mcc_dataset = 129:M$DS()` is present on **every large DMA master**,
not just AVE:

```
/arm-io/isp0, /arm-io/disp0, /arm-io/ave0, /arm-io/ave1,
/arm-io/avd0, /arm-io/ane0, /arm-io/sgx
```

`/arm-io/pmgr` and `/arm-io/pmgr/soc-tuner` carry the sibling
`function-mcc_ctrl = 129:Mem$()`.

On the AVE side, `AVE_MCC::Init(IOService*, cfg, AVE_DevInfo*, uint)`
(`0xfffffe0008c47250`) references the string `"function-mcc_dataset"`
(`0xfffffe00072a49e0`) at `0xfffffe0008c47300`. The class's other methods are
`RetrieveDSID(IORegistryEntry*, uint*)` (`0xfffffe0008c476fc`),
`GetDSID(_E_AVE_MCC_DSType, uint*)` (`0xfffffe0008c48c68`),
`EnableDS`/`DisableDS` (`0xfffffe0008c47db8`, `0xfffffe0008c48880`) and
`Enable`/`Disable`. Its assertion strings — `dsType < AVE_MCC_DSType_Max`,
`(dsType < AVE_MCC_DSType_Max) && (piDSID != nullptr)`, and the log
`MCC %s | %p %d | Handle: %p | Data Stream: %s` — confirm that the platform
function returns **data-stream IDs**, matching the `AFVEN0RD`/`DCVNC0WR` entries
in `_mcDataStreams`.

Two independent reasons this cannot be the enabler:

1. **Ordering.** [11-interrupts-bringup.md](11-interrupts-bringup.md) §8.4 gives
   the `AVE_HwC::Init` call order. `AVE_MCC::Init` is step **12**
   (`0xfffffe0008c1b444`), after `AVE_Reg::Init` (step 7, `0xfffffe0008c1ac10`,
   maps the banks), `AVE_SVECtrl::Init` (step 8, `0xfffffe0008c1ad74`) and
   `AVE_DPE::Init` + `AVE_DPE::Reset` (step 11, `0xfffffe0008c1b13c` /
   `0xfffffe0008c1b2e8`). Apple's own driver has already read and written AVE
   registers before it ever calls `M$DS`. `AVE_MCC::Enable` is later still —
   step 4 of `AVE_HwC::StartUp` (`0xfffffe0008c1d0ec`).
2. **Control case.** `avd0` carries the identical property, and Asahi's AVD
   driver never calls it.

**Verdict — not a prerequisite.** It is SLC cache-partitioning for AVE's DMA
traffic, and it affects memory traffic, not register decode.

---

## 6. Two corrections to the AVE `reg` map (the useful part)

While resolving the above, two of AVE's five `reg` ranges turned out to be
**PMGR-owned windows, not AVE's own address space**. This matters because
`25-bringup-results.md` rests on the claim that "every register `AppleAVE2`
itself touches is inside AVE's own address space (banks 0–4)".

### 6.1 Bank 3 (`0x8E588000`, `0x24`) is the PMGR power-state register block for VENC

Previously "unknown" ([12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md) §4.2,
[01-hardware.md](01-hardware.md)). It is arithmetic, and it closes exactly:

- `/arm-io/pmgr` `reg[2]` = `0x8E580000`, size `0x80000`.
- `ps-regs[13]` = `{reg: 2, offset: 0x8000, mask: 0x1F}`.
- Base = `0x8E580000 + 0x8000` = **`0x8E588000`** — `ave0`'s `reg[3]` base.
- Devices with `psreg == 13`: `VENC_DMA` (psidx 0), `VENC_PIPE4` (1),
  `VENC_PIPE5` (2), `VENC_ME0` (3), `VENC_ME1` (4). `mask = 0x1F` = exactly
  those five.
- PS registers are 8 bytes apart, so `VENC_ME1` is at `+0x20`, and a window of
  **`0x24`** covers `VENC_DMA` through `VENC_ME1`'s 32-bit PS register and
  nothing more. `ave0`'s declared size is `0x24`.

`ave1` corroborates independently: its `reg[3]` is `0x8E680260` size `0x7DC4`.
`/arm-io/pmgr` `reg[3]` = `0x8E680000`; `ps-regs[15]` = `{reg 3, offset 0x200}`
and `VENC1_SYS` has `psidx = 12` → `0x200 + 96 = 0x260`; `ps-regs[17]` =
`{reg 3, offset 0x8000, mask 0x1F}` holds `VENC1_DMA … VENC1_ME1`, ending at
`0x8020 + 4 = 0x8024`. `0x8024 − 0x260 = 0x7DC4` — the declared size, to the
byte.

Adding the `0x2_00000000` IO base that m1n1 applies, `ave0`'s bank 3 is
**`0x28E588000 … 0x28E588024`**, inside Linux's existing
`power-management@28e580000` aperture.

Consequence: bank 3 is **not** AVE address space. It is the same PMGR register
file Linux's `apple-pmgr-pwrstate` already drives.

### 6.2 Bank 4 (`0x20C000000`, 16 MB) is PMGR fabric bridge #2, owned by `VENC_SYS`

`/arm-io/pmgr` declares `#bridges = 193`, `bridge-reg-index = 48`,
`bridge-settings-version = 1`, `bridge-counter-version = 3`,
`axi2af-axi-config`, `optional-bridge-mask` and `device-bridges`.

`ApplePMGR::initDriver` reads `bridge-reg-index` into an OSData at
`0xfffffe00097ce460`–`0xfffffe00097ce4ac` and then, in the bridge-mapping loop,
computes the reg index as `bridge-reg-index + bridge`:

```
fffffe00097ce830:  ldr  w10, [sp, #384]      ; bridge-reg-index = 48
fffffe00097ce834:  add  w1,  w10, w25        ; w25 = bridge number
fffffe00097ce838:  mov  w2,  #0x0
fffffe00097ce844:  blraa                      ; mapDeviceMemoryWithIndex(48+bridge, 0)
```

So pmgr `reg[48 … 240]` are the 193 bridge register windows. The first twenty
(`reg[48]`–`reg[67]`) are 16 MB each and are the ones with a non-`0xFFFF`
`axi2af-axi-config` entry — the **AXI2AF** bridges.

`device-bridges` is 50 entries of `{u32 id; u8 bridges[0x48]}` (`0x4C` each,
3800 bytes). `ApplePMGR::_initDeviceBridges` (`0xfffffe00097fa954`) confirms the
shape: a per-device byte at `DeviceData+28` indexes a `u16` map at `this+25228`
to get an entry index, and the entry's bridge bytes start at `+4` with stride
`0x4C` (`0xfffffe00097fa9e0`–`0xfffffe00097fa9ec`).

The `id` key is the PMGR device field m1n1 calls `unk3` — *inferred*, but
corroborated five ways: it is the only device field with exactly 50 non-zero,
all-distinct values spanning 1…50, matching the 50 entries; and the resulting
assignment reproduces five independently known base addresses:

| PMGR device | bridge | window | matches |
|---|---|---|---|
| `VENC_SYS` (294) | 2 | `0x20C000000` | **`ave0` `reg[4]`** |
| `VENC1_SYS` (364) | 131 | `0x306000000` | **`ave1` `reg[4]`** |
| `AVD_SYS` | 3 | `0x86000000` | `avd0` `reg[0]` |
| `ANE_SYS` | 4 | `0x84000000` | `ane0` `reg[0]` |
| `ISP_SYS` | 5 | `0x184000000` | `isp0` `reg[0]` |

The `unk3 = id1` alternative was tested first and **rejected**: only 34
distinct non-zero `id1` values exist (1…26, 30…37), too few for 50 entries, and
it produced nonsense assignments (`DISPDFR_FE` owning AVD's aperture).

So `25-bringup-results.md`'s "**No entry in the pmgr `device-bridges` list
contains any VENC device**" is **wrong**: `VENC_SYS` and `VENC1_SYS` both have
entries, and their bridge windows are precisely `ave0`/`ave1` bank 4.

**What PMGR then does with that window** (this is why the bridge is still not
the answer):

- `_initDeviceBridges` runs on every device power-state transition — its callers
  are `_syncDeviceStatusChange+0x894` (`0xfffffe00097f991c`),
  `_resetDeviceGated` (`0xfffffe00097fa8d4`), `_syncDevicePsStatusV1`
  (`0xfffffe00097fc174`) and `V2` (`0xfffffe00097fc5e8`).
- It calls `_configureBridge(bridge, die)` (`0xfffffe00097faa00`) and then, if
  the bridge has counters, `_enableBridgeCounters(bridge, 1, 1, die)`
  (`0xfffffe00097faa40`).
- `_configureBridge` (`0xfffffe00097f1a3c`) branches on
  `_bridgeSettingsVersion`. The ADT says 1, so it takes the path at
  `0xfffffe00097f1b94`: test bit `bridge` of `_optionalBridges`
  (`ldrb`/`lsr`/`tbz` at `…1b9c`–`…1ba8`); bit **clear** ⇒ tail-call the virtual
  `applyBridgeTunables(bridge, die)` at `0xfffffe00097f1bc8`. Bridge 2's bit is
  clear (`optional-bridge-mask` bytes 0–4 are `0x00`), so the call is made.
- **But both `ApplePMGR::applyBridgeTunables` implementations in this
  kernelcache — `0xfffffe0009803fec` and `0xfffffe00098050e8` — are
  `bti c; ret`.** Empty stubs.

So on t6000 PMGR applies **no** AXI2AF tunables to any bridge. `bridge-settings`
is absent from the ADT, consistent with version 1 meaning "driver-managed" —
and indeed the *device* driver owns it: `AVE_AXI2AF::ApplyTunables`
(`0xfffffe0008c20d20`), which [12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md)
§4.4 already found has no `RegCfg_Default` table for 6000/6001/6002.

The only thing PMGR actually writes into `0x20C000000` is performance
counters: `bridge-counters[2]` is non-zero (`0x04088000`), so
`_enableBridgeCounters` (`0xfffffe00097f31e8`, which dispatches on
`bridge-counter-version = 3` to `_enableBridgeEventCounters`,
`0xfffffe00097f2024`) runs on each `VENC_SYS` transition. Telemetry, not enable.

**Net effect on the standing argument.** The premise "bank 4 is inside AVE's
own address space, so configuring the fabric would require touching the very
space that hangs" is **not established** — bank 4 is a PMGR-owned bridge
window, mapped by `ApplePMGR` at boot and written by it at power-state changes.
The *conclusion* (AXI2AF is not the enabler) nevertheless survives, for a
different and stronger reason: **the SoC's own AXI2AF tunable hook is an empty
function**, so macOS applies nothing there either.

---

## 7. What this leaves

Nothing examined here is a plausible prerequisite for AVE's address space
responding. The remaining hypotheses from
[25-bringup-results.md](25-bringup-results.md) are unaffected, and one of them
gains supporting evidence:

- **Hypothesis 1 (incomplete power — `venc_me1`) is strengthened.** `VENC_ME1`
  (id 304) is a **real** PMGR device — `psreg = 13`, `psidx = 4` — not one of
  the `psreg == 0` pseudo-devices that were correctly eliminated. Its PS
  register is at `0x8E580000 + 0x8000 + 4*8 = 0x8E588020`, i.e. `0x28E588020`
  with the IO base. It is the only member of `ave0`'s `power-gates` list with a
  real PS register that Linux does not drive.
- **Hypothesis 4 (a difference between Asahi's and macOS's PS handling)** is
  the right place to keep looking, and is being pursued separately against
  `ApplePMGR`. Anything found there should be cross-checked against §6.2, since
  `_syncDeviceStatusChange` is on the same path.

### Proposed checks — **not performed**

Recorded as proposals only. Trap 6 applies: a safety argument that names a
mechanism must cover every part of the action.

1. **Read `ave0`'s bank 3 (`0x28E588000 … +0x24`).** This is the PMGR PS
   register file for `VENC_DMA`/`PIPE4`/`PIPE5`/`ME0`/`ME1`, inside the
   `power-management@28e580000` aperture that `apple-pmgr-pwrstate` already
   maps and polls. Reading those five words would show the actual PS state of
   `VENC_ME1` — the one domain Linux cannot currently reach — and settle
   hypothesis 1 without touching AVE at all. It is *not* an AVE access; it
   should still be done through the m1n1 hypervisor rather than a hand-rolled
   mapping, because the fabric-hang failure mode gives no diagnostics.
2. **Do not treat bank 4 as "probably safe" because PMGR owns it.** It is a
   different physical window from banks 0–2, and macOS writes it while
   `VENC_SYS` is on — but nothing here shows it decodes independently of the
   VENC power/reset state, and the three hangs already recorded all came from
   equally reasonable-looking predictions.
3. **The m1n1 hypervisor trace remains the correct next step**, and §6.1/§6.2
   sharpen what to watch: writes to `0x28E588000+` (the VENC PS registers,
   including `ME1`) and to `0x40C000000`/`0x20C000000` (bridge 2) around the
   moment macOS first touches `0x20D800000 + 0x400044`.

---

## 8. Corrections this document makes to the existing record

- [12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md) §4.2 / §6 and
  [01-hardware.md](01-hardware.md): **bank 3 is identified** — the PMGR
  power-state registers for `VENC_DMA`…`VENC_ME1` (`ps-regs[13]`). It is no
  longer unknown, and it is not AVE address space.
  `driver/ave_hw.h`'s `AVE_BANK_UNKNOWN3` and `dts/t6001-ave.dtsi`'s
  `reg-names = "…, unk3, …"` should be renamed accordingly (not changed here).
- [25-bringup-results.md](25-bringup-results.md): "No entry in the pmgr
  `device-bridges` list contains any VENC device" is **wrong**; `VENC_SYS` owns
  bridge 2 (`0x20C000000`) and `VENC1_SYS` owns bridge 131 (`0x306000000`).
- [25-bringup-results.md](25-bringup-results.md): "every register `AppleAVE2`
  touches is inside AVE's own address space (banks 0–4)" is **false** — bank 3
  is PMGR, bank 4 is a PMGR bridge window. The AXI2AF elimination survives, but
  on the ground that `ApplePMGR::applyBridgeTunables` is an empty stub, not on
  the containment argument.
- [25-bringup-results.md](25-bringup-results.md): the `VENC0/1` strings in
  `AppleT6000` belong to `_mcDataStreams` (SLC data-stream descriptors), not to
  `AppleT6000PlatformErrorHandler::_fabricCommands`; the two symbols are `0x4a0`
  bytes apart.
