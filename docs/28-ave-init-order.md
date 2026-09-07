# AVE init order — IOKit probe/start down to the first register access

Static reading of `AppleAVE2.kext` in `data/blobs/kc.macho` (M1 Pro/Max
kernelcache). Purpose: find everything `AppleAVE2` asks the **rest of the
kernel** to do before it first touches an AVE register, because
[25-bringup-results.md](25-bringup-results.md) established that nothing the kext
does to its own register banks can be the thing that makes those banks respond.

Every claim below cites the VA it was read from. Anything not read out of the
image is marked **inferred** or **unknown**.

`AppleAVE2` `__TEXT_EXEC` is `0xfffffe0008b34bb0 .. 0xfffffe0008cd514c`
(`+0x1a059c`). "External" below means a `bl`/`blraa` whose target lies outside
that range.

Method note: the whole `__TEXT_EXEC` range was disassembled once and a call
graph built from `bl` / tail-`b` edges, with the logging idiom
(`AVE_Log_*`, `_printf`, `__os_log_internal`, `AVE_GetCurrTime`) filtered out
(Trap 1). Indirect (`blraa`) targets were resolved where the PAC blend
discriminator plus the vtable slot identifies them uniquely — see
[Resolving `blraa`](#resolving-blraa).

---

## 1. Headline

**The first AVE register access reached from `AppleAVE2Driver::start` is**

```
AVE_Reg::Write32(bank 2, offset 0x38, 1)
```

from `AVE_PMGR::SetClockGating(true)` → `AVE_SVECtrl::SetIdle(1)`
(`0xfffffe0008c91cbc`), called at `0xfffffe0008c1aeb0` from `AVE_HwC::Init`.
Bank 2 for `ave0` is ADT `reg[2]` = `0x20D050000`. Offset `0x38` is
`gsc_sAVE_SVECtrl_Reg_Rhea + 0x10c` (`0xfffffe00072748b4 + 0x10c`, value
`0x38`), selected by ChipType 7 = `t6001` (see
[§2.7](#27-the-first-write-in-detail)).

At that moment exactly **one** power domain has been raised: `PD_IOP` to
`PS_ClockOn` (`AVE_HwC::Init` `0xfffffe0008c1a3dc`). Nothing else in the block
has been powered, no clock has been requested, no reset pulsed, and no platform
function invoked that succeeds on `t6001`.

Two candidate "extra platform requests" were found and **both are dead ends on
this SoC**:

| Request | Where | Status on `t6001` |
|---|---|---|
| `function-set_perf_state_floor` | `AVE_PMGR::Init` `0xfffffe0008c4e6cc` | **Property does not exist anywhere in the j314c ADT** (0 occurrences; `function-mcc_dataset` occurs 7 times, so the test discriminates). `AVE_PMGR::SetPerfState` null-checks it at `0xfffffe0008c4f8e0` and bails. |
| `function-mcc_dataset` | `AVE_MCC::Init` `0xfffffe0008c47308` | Present in ADT, but `AVE_MCC::Enable` is only called from `AVE_HwC::StartUp` (`0xfffffe0008c1d0ec`) — **after** firmware boot, long after the first register write. |

And the `AVE_PMGR` → `AppleARMIODevice` chain was re-derived end to end: on
`t6001` it collapses to `ApplePMGR::_enableDevice(gate, on)` writing PS field
**`0xf`** for on and `0x0` for off — byte-identical to what Linux's
`apple-pmgr-pwrstate` writes. See [§4](#4-ave_pmgr-corrected-and-completed).

---

## 2. The ordered sequence

### 2.1 `AppleAVE2Driver::init` — `0xfffffe0008cc2f24`

| VA | Call |
|---|---|
| `0xfffffe0008cc2f6c` | `AVE_Cfg_Init()` |
| `0xfffffe0008cc31a4` | **EXT** `IOService::init(OSDictionary*)` (static super call through `__ZTV9IOService+0x278`, disc `0x43aa`) |
| `0xfffffe0008cc32b4`, `+0xc` | `AVE_Mutex_Create()` ×2 → `IOLockAlloc` |

### 2.2 `AppleAVE2Driver::probe` — `0xfffffe0008cc3754`

One non-logging call, and it is external:

```
0xfffffe0008cc3828  ldr x8, [0xfffffe0008cc3828 pcrel] -> 0xfffffe0007ed8f80
0xfffffe0008cc3830  ldr x1, [x8]        ; = _gAppleARMPEFindCoproviderKey
0xfffffe0008cc386c  blraa               ; provider->callPlatformFunction(
                                        ;     gAppleARMPEFindCoproviderKey,
                                        ;     false, provider, &out, 0, 0)
```

`vptr+0x6e8` = `__ZTV9IOService+0x6f8` = `IOService::callPlatformFunction`,
disc `0x0988`. `_gAppleARMPEFindCoproviderKey` is `0xfffffe000c6987a8`, exported
by `com.apple.driver.AppleARMPlatform`.

This corresponds to ADT `coprovider-group = ave` on `/arm-io/ave0` and
`/arm-io/ave1`. In the j314c ADT `coprovider-group` appears on exactly nine
nodes: `scaler0/1`, `apr0/1`, `jpeg0/1`, `ave0/1`, `avd0`. **`avd0` is in the
list with only one member and Asahi drives AVD without any coprovider
mechanism**, so this is a driver-pairing convenience, not a hardware enable.
Failure is non-fatal: `0xfffffe0008cc3874` `cbz w0, …` merely NULLs the out
slot.

### 2.3 `AppleAVE2Driver::start` — `0xfffffe0008cc3958`

Order recovered from the CFG (branch targets, not address order):

```
0xfffffe0008cc398c   AVE_Mutex_Lock
0xfffffe0008cc3a3c   AVE_Cfg_Get
   (0xfffffe0008cc4f74 AVE_DevInfo::GetDevID / 0xfffffe0008cc4f80
    AppleAVE2Driver::SetAbility, on a loop back to start+0xfc)
0xfffffe0008cc3b9c   EXT IOService::start(provider)          [super, ZTV+0x600, disc 0x3c68]
0xfffffe0008cc3d6c   AVE_Drv::Create
0xfffffe0008cc4038   AVE_Drv::Init(void*, _S_AVE_Cfg*)
0xfffffe0008cc3ea0   AVE_DevInfo::RetrieveSVEID(IORegistryEntry*, int*)
0xfffffe0008cc43cc   AVE_Drv::IO_init()                       <-- IOKit plumbing
0xfffffe0008cc427c   AVE_Drv::IO_start(IOService*, uint)      <-- HARDWARE
0xfffffe0008cc4adc   EXT IOService::PMinit()                   [ZTV+0x7d0, disc 0xe8de]
0xfffffe0008cc4ae0   AVE_AnalyticsService_Create
0xfffffe0008cc4aec   AVE_AnalyticsService_Start(svc, provider)
0xfffffe0008cc4be0   EXT provider->joinPMtree(this)            [ZTV+0x7e0, disc 0x4bc0]
0xfffffe0008cc4cfc   EXT IOService::registerPowerDriver(...)   [ZTV+0x7e8, disc 0xf8a6]
0xfffffe0008cc4d08   EXT IOService::changePowerStateTo(...)
0xfffffe0008cc4d14   EXT IOService::changePowerStateToPriv(...)
0xfffffe0008cc4e00   EXT IOService::registerService(0)         [ZTV+0x5f0, disc 0x7d59]
0xfffffe0008cc4574   AVE_Mutex_Unlock
```

Error path (`0xfffffe0008cc4448` onward): `AnalyticsService_Stop/Destroy`,
`AVE_Drv::IO_stop`, `IO_free`, `Uninit`, `Destroy`, then
`IOService::stop(provider)` (super, `ZTV+0x608`, disc `0xb085`) at
`0xfffffe0008cc44b0`.

Note the ordering: **all AVE hardware work happens inside `IO_start`, i.e.
before `PMinit` / `joinPMtree` / `registerPowerDriver`.** macOS does not use
IOKit power management to gate the first register touch; it drives PMGR
directly through `AVE_PMGR` first.

### 2.4 `AVE_Drv::IO_init` — `0xfffffe0008bd9428`

| VA | Call |
|---|---|
| `0xfffffe0008bd94e0` | **EXT** `IOWorkLoop::workLoop()` |
| `0xfffffe0008bd94f4` | **EXT** `IOCommandGate::commandGate(OSObject*, Action)` |
| `0xfffffe0008bd9528` | **EXT** `IOWorkLoop::addEventSource(cmdGate)` (`vptr+0x160`, disc `0x11e8`) |
| `0xfffffe0008bd96a0/b0` | `AVE_DevInfo` ctor + `AVE_DevInfo::Init(devID)` |
| `0xfffffe0008bd9908/18` | `AVE_SurfaceMgr` ctor + **`AVE_SurfaceMgr::Init`** — see [§5](#5-waits-on-other-services) |
| `0xfffffe0008bd9b5c` | `AVE_SurfaceMgr::CreateSurface(...)` (→ `IOSurfaceRoot::createSurface`) |
| `0xfffffe0008bd9bfc/c08` | `AVE_Crypto` ctor + `Init` |
| `0xfffffe0008bd9db8/ec/e20` | **EXT** `IOTimerEventSource::timerEventSource()` ×3 |
| `0xfffffe0008bd9e50/64` | `AVE_DLB` ctor + `Init` |
| `0xfffffe0008bda4c0/e0` | `AVE_SwC` ctor + `Init` |

No register access anywhere under `IO_init` (checked by reachability to
`AVE_Reg::Read32`/`Write32`).

### 2.5 `AVE_Drv::IO_start` — `0xfffffe0008bdafc0`

| VA | Call |
|---|---|
| `0xfffffe0008bdb1c4` | `AVE_HwC::Create()` |
| `0xfffffe0008bdb218` | **EXT** `IOFilterInterruptEventSource::filterInterruptEventSource(owner, action, filter, provider, **index 0**)` — index literal `mov w4, #0` at `0xfffffe0008bdb214` |
| `0xfffffe0008bdb260` | **`AVE_HwC::Init(...)`** — everything below |
| `0xfffffe0008bdb3b8/c4` | `AVE_HwC::Uninit` / `Destroy` (error path) |
| `0xfffffe0008bdb404` | **EXT** `OSObject::release()` on the event source (error path) |

The event source is *created* here but **not yet added to a work loop** — that
happens inside `AVE_HwC::Init` at `0xfffffe0008c1bac4`, i.e. **after** the first
register write.

### 2.6 `AVE_HwC::Init` — `0xfffffe0008c19de8` (the critical function)

Success-path order (the block `0xfffffe0008c1a590 .. 0xfffffe0008c1a7c4` is the
failure unwind and is excluded):

```
0xfffffe0008c1a084  AVE_DevInfo::GetDevType
0xfffffe0008c1a090  AVE_DevInfo::GetChipType
0xfffffe0008c1a09c  EXT IOMallocTypeImpl      ; AVE_SVECtrl
0xfffffe0008c1a0a0      AVE_SVECtrl::AVE_SVECtrl
0xfffffe0008c1a0b4  EXT IOMallocTypeImpl      ; AVE_AXI2AF
0xfffffe0008c1a0b8      AVE_AXI2AF::AVE_AXI2AF
0xfffffe0008c1a0cc  EXT IOMallocTypeImpl      ; AVE_PMGR
0xfffffe0008c1a0d0      AVE_PMGR::AVE_PMGR
0xfffffe0008c1a0f0  >>> AVE_PMGR::Init(provider, cfg, devInfo, id, pSVECtrl)
0xfffffe0008c1a3dc  >>> AVE_PMGR::SetPS(PD=0 IOP, PS=2 ClockOn, true)     [POWER ON]
0xfffffe0008c1a918  EXT IOMallocTypeImpl      ; AVE_DART
0xfffffe0008c1a91c      AVE_DART::AVE_DART
0xfffffe0008c1a93c  >>> AVE_DART::Init(provider, cfg, devInfo, id, i)     [IOMMU]
0xfffffe0008c1aa98      AVE_SurfaceMgr::SetDART(id, dart)
0xfffffe0008c1abdc      AVE_SurfaceMgr::EnableOp(mask)
0xfffffe0008c1abe8  EXT IOMallocTypeImpl      ; AVE_Reg
0xfffffe0008c1abec      AVE_Reg::AVE_Reg
0xfffffe0008c1ac10  >>> AVE_Reg::Init(provider, devInfo, id, dart, nBanks=6) [MMIO MAP]
0xfffffe0008c1ad74      AVE_SVECtrl::Init(cfg, devInfo, id, pReg)
0xfffffe0008c1aeb0  ### AVE_PMGR::SetClockGating(true)  -> FIRST REGISTER ACCESS
0xfffffe0008c1afa4/c0   AVE_DPM ctor + AVE_DPM::Init(devInfo, cfg, id, pPMGR)
0xfffffe0008c1b120/3c   AVE_DPE ctor + AVE_DPE::Init(cfg, devInfo, id, pReg)   [bank 0]
0xfffffe0008c1b2e8      AVE_DPE::Reset()
0xfffffe0008c1b428/44   AVE_MCC ctor + AVE_MCC::Init(provider, cfg, devInfo, id)
0xfffffe0008c1b5a0/c8   AVE_FwImg ctor + AVE_FwImg::Init(...)
0xfffffe0008c1b774/8c   AVE_FwLog ctor + AVE_FwLog::Init(...)
0xfffffe0008c1b960      AVE_AXI2AF::Init(devInfo, id, pReg)                    [bank 4]
0xfffffe0008c1bac4  EXT IOWorkLoop::addEventSource(interruptEventSource)
0xfffffe0008c1bbb4/d8   AVE_IOP ctor + AVE_IOP::Init(cfg, devInfo, id, reg, sve, axi, fwimg)
0xfffffe0008c1bd38      AVE_HwC::CreateFwHeartBeatTimer(...)
0xfffffe0008c1bd4c      AVE_SurfaceMgr::DARTMapSurface(...)
   -- guarded by `cmp w26, #0x1e` (DevType == 0x1e) at 0xfffffe0008c1bdcc; NOT taken on t6001 --
   0xfffffe0008c1bde4    AVE_PMGR::SetPS(PD=4 DMA/FE, ClockOn)
   0xfffffe0008c1bf0c    AVE_Reg::Read32(bank 5, 0x9C000)   ; device revision
   0xfffffe0008c1bf18    AVE_DevInfo::SetDevRevision
   0xfffffe0008c1bf2c    AVE_PMGR::SetPS(PD=4 DMA/FE, PowerOff)
0xfffffe0008c1bf38      AVE_SurfaceMgr::DisableOp
0xfffffe0008c1bf44      AVE_DART::SetActive(false)
0xfffffe0008c1bf5c      AVE_PMGR::SetPS(PD=0 IOP, PS=0 PowerOff)             [POWER OFF again]
```

So `AVE_HwC::Init` powers `IOP` on, probes, and powers it back **off**. Real
bring-up happens later in `AVE_HwC::StartUp` / `StartUpIOP`.

Verified by reachability that no callee before `0xfffffe0008c1aeb0` on the
success path can reach `AVE_Reg::Read32`/`Write32`. The only pre-`SetClockGating`
callee that can is `AVE_DPE::~AVE_DPE` at `0xfffffe0008c1a6b0`, which is on the
failure unwind.

### 2.7 The first write in detail

```
AVE_PMGR::SetClockGating(bool bGate)            0xfffffe0008c50970
  0xfffffe0008c50a44  ldrb w8, [x19, #160]      ; cached state
  0xfffffe0008c50a4c  cmp  w20, w8
  0xfffffe0008c50a50  b.eq skip                 ; no-op if already there
  0xfffffe0008c50a54  ldr  x0, [x19, #40]       ; this->m_pSVECtrl
  0xfffffe0008c50a5c  bl   AVE_SVECtrl::SetIdle(bGate)

AVE_SVECtrl::SetIdle(uint32 v)                  0xfffffe0008c91c80
  0xfffffe0008c91ca8  ldr  x0, [x0, #24]        ; this->m_pReg  (AVE_Reg*)
  0xfffffe0008c91cac  ldr  x8, [x20, #32]       ; this->m_pRegTable
  0xfffffe0008c91cb0  ldr  w2, [x8, #268]       ; offset = table[+0x10c]
  0xfffffe0008c91cb4  mov  w1, #0x2             ; bank 2
  0xfffffe0008c91cb8  mov  x3, x21              ; value
  0xfffffe0008c91cbc  bl   AVE_Reg::Write32
```

Table selection: `AVE_SVECtrl::Init` (`0xfffffe0008c90fbc`) calls
`AVE_DevInfo::GetChipType()` then `AVE_SVECtrl_GetReg(ChipType)`
(`0xfffffe0008c90e44`), which indexes `0xfffffe0007ee0f80` by `ChipType-1`
(21 entries). **ChipType 7 = `t6001` → `gsc_sAVE_SVECtrl_Reg_Rhea`
(`0xfffffe00072748b4`)**, whose `+0x10c` word is `0x38`.

Canary check on the same table (per [00-methodology.md](00-methodology.md)):
`+0x00` = `0xc` and `+0x04` = `0x10`, which match the already-confirmed
doorbell at bank 2 `+0x0C` and its W1C status at `+0x10`. The decode is
therefore consistent with an independently established fact.

So the first transaction is a 32-bit write of `1` to
`0x20D050000 + 0x38 = 0x20D050038` for `ave0`
(`0x307050038` for `ave1`).

### 2.8 `AVE_Reg::Init` — `0xfffffe0008c532d0` (how the banks get mapped)

```
0xfffffe0008c5333c  csel w24, w24, #5, eq    ; if (GetDevType() != 0x1e) nBanks = 5
loop i = 0 .. nBanks-1:
  0xfffffe0008c5336c  blraa  provider->mapDeviceMemoryWithIndex(i, 0)
                             [vptr+0x710 = __ZTV9IOService+0x720, disc 0xd52d]
  0xfffffe0008c53374  str x0, [this + 0x10 + 8*i]     ; IOMemoryMap*
  0xfffffe0008c5339c  blraa  map->getVirtualAddress()
                             [vptr+0x138 = __ZTV11IOMemoryMap+0x148, disc 0x34f6]
  0xfffffe0008c533a0  str x0, [this + 0x40 + 8*i]     ; kernel VA
```

`AVE_Reg::Read32`/`Write32` accept bank `0..5` (`cmp w1, #5; b.hi` at
`0xfffffe0008c53df4` / `0xfffffe0008c53e28`) and index `this+0x40+8*bank`.
`AVE_HwC::Init` passes `nBanks = 6` (`mov w5, #6` at `0xfffffe0008c1ac0c`), but
the clamp above means **five banks (ADT `reg[0..4]`) are mapped on `t6001`**.

**No `ioremap`-equivalent external call, no `IODeviceMemory` fiddling, no
platform call.** The kext simply maps what the ADT `reg` property already
describes.

#### Bank census (correction to docs/25)

Every `Read32`/`Write32` call site in the kext, classified by the bank literal
in `w1` (169 sites total):

| Bank | Sites | Who |
|---|---|---|
| 0 | 14 | `AVE_DPE` |
| 1 | 122 | `AVE_IOP`, `AVE_SVECtrl` scratch/msg |
| 2 | 21 | `AVE_SVECtrl` (idle, doorbell, IOP flags) |
| **3** | **0** | — |
| 4 | 9 | `AVE_AXI2AF` |
| 5 | 1 | `AVE_HwC::Init` revision read, `DevType == 0x1e` only |
| dynamic | 2 | `AVE_RegCfg_Apply` (`0xfffffe0008c547c0`, `0xfffffe0008c547dc`) — bank is a field of the `_S_AVE_RegCfg` struct, so no immediate exists to scan for (Trap 3). Its only callers are `AVE_RegCfgList_Apply` ← `AVE_HwC::SetDPE` (`0xfffffe0008c22c00`) and `AVE_RegCfgList_Process` ← `AVE_HwC::SetRegCfg` (`0xfffffe0008c26128`). **Neither is reachable from `AppleAVE2Driver::start`, `AVE_Drv::StartUp` or `AVE_HwC::StartUp`** — these are per-session/per-frame paths. |

**Bank 3 is `0x8E588000` (size `0x24`) for `ave0` — inside the PMGR window
`power-management@28e580000`, not AVE's own address space.** The kext maps it
and never touches it. This is a real qualification of docs/25's "every register
AppleAVE2 touches is inside AVE's own address space": the statement is true of
*accesses*, but the kext's `reg` list is not confined to the AVE block. What is
in that 36-byte PMGR window, and whether anything else in the system writes it
before AVE is used, is **unknown** and is the one lead this analysis could not
close (see [§7](#7-proposals-not-performed)).

---

## 3. Every external call in the window

"Window" = everything reachable from `AppleAVE2Driver::start` up to and
including `AVE_PMGR::SetClockGating` (cutting `AppleAVE2Driver::start` at
`0xfffffe0008cc427c`, `AVE_Drv::IO_start` at `0xfffffe0008bdb260`, and
`AVE_HwC::Init` at `0xfffffe0008c1aeb0`), plus `AVE_Drv::IO_init` and
`AppleAVE2Driver::init`/`probe`, which run earlier. 140 AVE functions, 24
distinct direct external targets.

### 3.1 The ones that could matter

| Target | Kext | Called from | What it is |
|---|---|---|---|
| `0xfffffe0008a901c0` `AppleARMFunction::withProvider(IORegistryEntry*, const char*)` | **AppleARMPlatform** | `AVE_PMGR::Init` `0xfffffe0008c4e6cc`, arg = `"function-set_perf_state_floor"` (string at `0xfffffe00072a62e6`) | Looks up an ADT platform function on the AVE nub. **Returns NULL on `t6001`** — the property does not exist in the ADT. |
| `0xfffffe0008a901c0` (same) | **AppleARMPlatform** | `AVE_MCC::Init` `0xfffffe0008c47308`, arg = `"function-mcc_dataset"` (string at `0xfffffe00072a49e0`) | SLC / memory-cache data-set programming. Present in the ADT, but only *invoked* from `AVE_MCC::Enable`, called from `AVE_HwC::StartUp` `0xfffffe0008c1d0ec` — after firmware boot. |
| `0xfffffe000bf47dd8` `IOMapper::copyMapperForDevice(IOService*)` | kernel | `AVE_DART::Init` `0xfffffe0008ba58c0` | Gets the DART. Linux equivalent: `apple-dart` + the DMA API. |
| `0xfffffe000beddae8` `IORegistryEntry::fromPath(...)` | kernel | `AVE_DART::GetMapperRegistryEntry` `0xfffffe0008ba4c30`, `…c64` | Finds the DART node by IORegistry path. |
| `0xfffffe000ab9523c` `IOSurfaceRoot::registerMapper(IOMapper*)` | **IOSurface** | `AVE_SurfaceMgr::SetDART` `0xfffffe0008c80cc4` | Registers AVE's DART with IOSurface so surfaces can be mapped for it. |
| `0xfffffe000ab952a8` `IOSurfaceRoot::unregisterMapper` | **IOSurface** | `AVE_SurfaceMgr::SetDART` `0xfffffe0008c80d7c` | Teardown of the above. |
| `0xfffffe000bf3eb80` `IOFilterInterruptEventSource::filterInterruptEventSource(...)` | kernel | `AVE_Drv::IO_start` `0xfffffe0008bdb218` | Creates the interrupt source, provider index **0**. |
| `0xfffffe000be5d83c` `OSMetaClassBase::safeMetaCast` | kernel | `AVE_PMGR::Init` `0xfffffe0008c4e6a0` (to **`AppleARMIODevice`**, metaclass ptr at `0xfffffe0007ed8f30`), `AVE_DART::RetrieveMapperInfo` `0xfffffe0008ba4d90`, `AVE_DevInfo::RetrieveSVEID` `0xfffffe0008bab56c` | Type checks. The `AVE_PMGR` one is what pins down §4. |
| `0xfffffe000bea099c` `OSSymbol::withCString` | kernel | `AVE_DART::AVE_DART` `0xfffffe0008ba3b8c`, `…b9c`; `AVE_DART::InstallErrorHandler` `0xfffffe0008ba481c` | DART error-handler registration keys. |

Additionally, from `AVE_Drv::IO_init` (which runs before `IO_start`):

| Target | Kext | Called from |
|---|---|---|
| `0xfffffe000bf3785c` `IOWorkLoop::workLoop()` | kernel | `AVE_Drv::IO_init` `0xfffffe0008bd94e0` |
| `0xfffffe000bf3b330` `IOCommandGate::commandGate(...)` | kernel | `AVE_Drv::IO_init` `0xfffffe0008bd94f4` |
| `0xfffffe000bf408dc` `IOTimerEventSource::timerEventSource(...)` | kernel | `AVE_Drv::IO_init` `0xfffffe0008bd9db8`, `…dec`, `…e20` |
| `0xfffffe000bf084ec` `IOService::nameMatching("IOSurfaceRoot", NULL)` | kernel | `AVE_SurfaceMgr::Init` `0xfffffe0008c80434` |
| `0xfffffe000bf07e90` `IOService::waitForMatchingService(dict, ~0ull)` | kernel | `AVE_SurfaceMgr::Init` `0xfffffe0008c80444` — **see §5** |
| `0xfffffe000ab921ac` `IOSurfaceRoot::createSurface(task, dict)` | **IOSurface** | `AVE_Surface::CreateIOSurface` `0xfffffe0008c697c0` |
| `0xfffffe000bf45be0` `IODMACommand::withSpecification(...)` | kernel | `AVE_DART_Pool_GetEntry` `0xfffffe0008ba39cc` |
| `0xfffffe000bf56bd0` `IOMemoryDescriptor::withOptions(...)` | kernel | `AVE_FwImg::InitCTRRImage` `0xfffffe0008bebbbc`, `…be0` |
| `0xfffffe000bf07cf0` `IOService::addMatchingNotification(...)` | kernel | `AVE_AnalyticsService_Start` `0xfffffe0008b5b08c` |
| `0xfffffe000bf08118` `IOService::serviceMatching(...)` | kernel | `AVE_AnalyticsService_Start` `0xfffffe0008b5b054` |

### 3.2 Housekeeping only (no hardware effect)

`_IOMallocTypeImpl` `0xfffffe000bed4cd4`, `_IOFreeTypeImpl` `0xfffffe000bed4efc`,
`_IOMallocTypeVarImpl` `0xfffffe000bed5514`, `_IOFreeTypeVarImpl`
`0xfffffe000bed5528`, `_IOLockAlloc` `0xfffffe000bed5df4`, `_IOLockFree`
`0xfffffe000bed5e74`, `_IOLockLock` `0xfffffe000b758c8c`, `_IOLockUnlock`
`0xfffffe000b75a0b4`, `_OSObject_typed_operator_new` `0xfffffe000be61184`,
`OSObject::OSObject` `0xfffffe000be61298`, `OSMetaClass::instanceConstructed`
`0xfffffe000be5ecd0`, `___bzero` `0xfffffe000b6e8d80`, `_vsnprintf`
`0xfffffe000bd22c90`, `_kernel_debug` `0xfffffe000bc8cacc` (ktrace only, 21
sites in the window), `OSDictionary/OSArray/OSNumber/OSString::with*`,
`IOSurface` accessors (`getSurfaceID`, `getAllocSize`, `getPixelFormat`,
`getProtectionOptions`, `getMapCacheAttribute`, `getMemoryDescriptor`,
`deviceLockSurface`, `deviceUnlockSurface`, `setValue`, `createFence`,
`IOFence::complete`), `_memcpy`, `_strcmp`, `_kernel_thread_start`.

### 3.3 External calls after the first write (rest of bring-up)

For completeness — reachable from `AVE_Drv::StartUp` (`0xfffffe0008be37d0`),
`AVE_Drv::ForcePowerOn` (`0xfffffe0008bdeb18`) and `AVE_HwC::StartUp`
(`0xfffffe0008c1cd8c`); 36 distinct targets, of which the only ones outside
`com.apple.kernel` / `IOSurface` are the memory-cache group in
**AppleARMPlatform**:

| Target | Called from |
|---|---|
| `0xfffffe0008ac3b6c` `MCDataStreamInfoObject::initWith(MCDataStreamId, uint)` | `AVE_MCC::EnableDS` `0xfffffe0008c48100` |
| `0xfffffe0008ac3824` `MCDataStream::getDSIDCount()` | `AVE_MCC::EnableDS` `0xfffffe0008c482a0` |
| `0xfffffe0008ac3838` `MCDataStream::copyDSIDs(uint*, uint)` | `AVE_MCC::EnableDS` `0xfffffe0008c4845c` |
| `0xfffffe0008ac3890` `MCDataStream::setDataStreamFlags(uint)` | `AVE_MCC` (via `EnableDS`) |

Plus `_IODelay` (`0xfffffe000bed5758`, `AVE_HwC::StartUpIOP`
`0xfffffe0008c1fda0` — the IOP-flag poll) and the allocation/OSObject set
already listed. `AVE_MCC::Enable` is reached from `AVE_HwC::StartUp`
`0xfffffe0008c1d0ec`, i.e. **after** `AVE_IOP::Start` (`0xfffffe0008c1dee8`) has
already written bank 1 and the firmware has booted. It configures SLC stashing,
not block enablement.

### 3.4 Resolvable indirect (`blraa`) external calls

| Site | Resolved target | How |
|---|---|---|
| `0xfffffe0008cc31a4` | `IOService::init(OSDictionary*)` | `__ZTV9IOService+0x278`, disc `0x43aa` |
| `0xfffffe0008cc386c` | `IOService::callPlatformFunction` | vptr`+0x6e8`, disc `0x0988` |
| `0xfffffe0008cc3b9c` | `IOService::start` (super) | `__ZTV9IOService+0x600`, disc `0x3c68` |
| `0xfffffe0008cc44b0` | `IOService::stop` (super) | `__ZTV9IOService+0x608`, disc `0xb085` |
| `0xfffffe0008cc4adc` | `IOService::PMinit` | vptr`+0x7c0` → ZTV`+0x7d0`, disc `0xe8de` |
| `0xfffffe0008cc4be0` | `IOService::joinPMtree` | vptr`+0x7d0` → ZTV`+0x7e0`, disc `0x4bc0` |
| `0xfffffe0008cc4cfc` | `IOService::registerPowerDriver` | vptr`+0x7d8` → ZTV`+0x7e8`, disc `0xf8a6` |
| `0xfffffe0008cc4e00` | `IOService::registerService(0)` | vptr`+0x5e0` → ZTV`+0x5f0`, disc `0x7d59` |
| `0xfffffe0008c5336c` | `IOService::mapDeviceMemoryWithIndex` | vptr`+0x710` → ZTV`+0x720`, disc `0xd52d` |
| `0xfffffe0008c5339c` | `IOMemoryMap::getVirtualAddress` | vptr`+0x138` → ZTV`+0x148`, disc `0x34f6` |
| `0xfffffe0008bd9528`, `0xfffffe0008c1bac4` | `IOWorkLoop::addEventSource` | vptr`+0x160` → ZTV`+0x170`, disc `0x11e8` |
| `0xfffffe0008c1a634` | `IOWorkLoop::removeEventSource` | vptr`+0x168` → ZTV`+0x178`, disc `0xa255` |
| `0xfffffe0008c1a608` | `IOEventSource::disable` | vptr`+0x178` → ZTV`+0x188`, disc `0x9a99` |
| `0xfffffe0008bdb404` | `OSObject::release` | vptr`+0x28` → ZTV`+0x38`, disc `0x3a87` |
| `0xfffffe0008c4efc8` | `AppleARMIODevice::setDevicePowerState` | vptr`+0x8c0` → ZTV`+0x8d0`, disc `0x23bd` |
| `0xfffffe0008c4f2c8` | `AppleARMIODevice::enablePsdService` | vptr`+0x8c8` → ZTV`+0x8d8`, disc `0xf811` — **not taken on t6001**, see §4 |
| `0xfffffe0008c5043c` | `AppleARMIODevice::resetPsdService` | vptr`+0x8d0` → ZTV`+0x8e0`, disc `0x8f95` |
| `0xfffffe0008c4fd14` | `AppleARMFunction::callFunction(void*,void*,void*)` | vptr`+0x140` → ZTV`+0x150`, disc `0x7547` |

### Resolving `blraa`

Two conventions appear, and confusing them shifts every offset by `0x10`:

* **Virtual call through an object.** `ldr x16,[x0]; autda x16,…; add x16,x16,#N`
  — here `x16` is the **vptr**, which points at `__ZTV<Class> + 0x10` (past
  offset-to-top and typeinfo). So slot `+N` = `ZTV + N + 0x10`.
* **Static super call.** `add x8, x28, #N; ldr x9,[x28,#N]` where `x28` was
  loaded from a `__got` slot holding `__ZTV<Class>` — here `N` is measured from
  the **ZTV symbol**, no `+0x10`.

Both were checked against the chained-fixup **diversity** field of the vtable
entry (bits 32–47 of the raw quadword; Trap 5 applies to the low 32 bits, which
are an image-relative file offset). A slot is only claimed here when its
diversity equals the `movk x17, #imm, lsl #48` at the call site.

Everything else that remains a genuine `blraa` with an object whose class cannot
be pinned down is left unresolved rather than guessed. In the window these are:
`AVE_SurfaceMgr::SetDART` `0xfffffe0008c807ec`/`…814`, and `AVE_Drv::IO_init`
`0xfffffe0008bda0ec`/`…10c`/`…134` (all on the teardown path).

---

## 4. `AVE_PMGR`, corrected and completed

### What object the vtable belongs to — confirmed

`AVE_PMGR::Init` at `0xfffffe0008c4e690`–`0xfffffe0008c4e6a4`:

```
adrp x8, 0xfffffe0007ed8000
ldr  x8, [x8, #3888]        ; -> 0xfffffe0007ed8f30
ldr  x1, [x8]               ; -> __ZN16AppleARMIODevice9metaClassE (0xfffffe0007eae330)
mov  x0, x20                ; the IOService* provider
bl   0xfffffe000be5d83c     ; OSMetaClassBase::safeMetaCast
str  x0, [x19]              ; this->m_pcProvider  (this+0x00)
```

So **`this+0x00` is the provider dynamic-cast to `AppleARMIODevice`**, and the
`vptr+0x8c0` call in docs/10 is on that object. Confirmed.

`AVE_PMGR::Init` also stores, at `0xfffffe0008c4e6cc`–`0xfffffe0008c4e6ec`:

* `this+0x08` = `AppleARMFunction::withProvider(provider, "function-set_perf_state_floor")`
* `this+0x38` = `AVE_DevCap_FindPDMap(GetDevID())` (the `aPD[11]` map)
* `this+0x40` = PDMap `+0x2c`
* `this+0x30` = `AVE_DevCap_FindHwFeature(GetDevID())` — **a bitmask, not a
  pointer**; read from `CEntry+0x18` (`0xfffffe0008ba9ff0`)

### `AVE_PMGR::SetPowerState` — there are TWO platform calls, and t6001 uses one

`AVE_PMGR::SetPowerState(PD, PS)` at `0xfffffe0008c4ea9c` branches on bit 1 of
the HwFeature mask (`ldr x8,[x19,#48]` `0xfffffe0008c4ebcc`; `tbnz w8,#1,…`
`0xfffffe0008c4ef28`):

**Path A — bit 1 clear (`t6001`)**, `0xfffffe0008c4ef50`–`0xfffffe0008c4efc8`:

```
w1 = table_0xfffffe0007274558[PS]     ; PS 0,1,2 -> 5, 6, 7
w2 = aPD[PD]                          ; index into the nub's gate array
provider->setDevicePowerState(w1, w2)     [vptr+0x8c0]
```

**Path B — bit 1 set**, `0xfffffe0008c4f250`–`0xfffffe0008c4f2c8`:

```
(w1, w3) = table_0xfffffe0007274540[PS]   ; PS0 -> (0, -1); PS1 -> (1, 10); PS2 -> (1, 10)
w2 = aPD[PD]
provider->enablePsdService(w1, w2, w3)    [vptr+0x8c8]
```

The HwFeature word for every device ID in `gsc_saAVE_DevCap`
(`0xfffffe0007edba00`, stride `0x48`, `CEntry+0x18`):

| devID | CEntry | HwFeature |
|---|---|---|
| 2 (`8320`) | `gsc_sAVE_DevCap_CEntry_8320` | `3` |
| 11,12,13 (`6000/6001/6002`) | `…CEntry_6001` etc. | **`0`** |
| 30,31,32 (`8142/6050/8150`) | | `1` |
| 33,34 (`8152/8160`) | | `3` |
| all others | | `0` |

Bit 1 is set only on devIDs 2, 33, 34. **`t6001` is devID 12 → HwFeature `0` →
Path A.** The test discriminates (three distinct values observed), so this is a
verified negative, not an untested one. `enablePsdService` is **not** an AVE
requirement on M1 Max.

`AVE_PMGR::ResetPSD` (`0xfffffe0008c5012c`) does call
`provider->resetPsdService(aPD[PD])` at `0xfffffe0008c5043c`, but it is reached
only from `AVE_PMGR::SetPState` (`0xfffffe0008c521cc`), which is reached only
from `AVE_DPM::ApplyIOP` (`0xfffffe0008c72a38`) — nowhere near the first
register touch.

### Where the platform value actually lands

```
AVE_PMGR::SetPowerState                         0xfffffe0008c4ea9c
 -> AppleARMIODevice::setDevicePowerState(st, idx)   0xfffffe0008a970dc
      idx bounds-checked against this+0xa0; gateId = this->gateIds[idx] (this+0xa8)
      st must be 5..7 (0xfffffe0008a970ec); val = table_0xfffffe0007168060[st-5]
                                             = {5->0, 6->2, 7->1}
   -> provider->enableDeviceClock(gateId, val)    [vptr+0x8a8]
      = AppleT600xIO::enableDeviceClock            0xfffffe0009aa305c
      -> AppleARMFunction::callFunction(gateId, val, 0)   [vptr+0x140, disc 0x7547]
         = ApplePMGRFunctionClockGate::callFunction 0xfffffe000980ab54
         -> ApplePMGR::_enableDevice(               [vptr+0x948, disc 0x5a17]
                gateId & 0x0FFFFFFF,                0xfffffe000980ace8
                val & 1,                            0xfffffe000980acec   "on"
                (val >> 1) & 1,                     0xfffffe000980acc4   "flag"
                gateId >> 28)                       0xfffffe000980ab7c   die
            -> ApplePMGR::_enableDeviceGated        0xfffffe00097f848c
```

and in `_enableDeviceGated` at `0xfffffe00097f853c`–`0xfffffe00097f8548`:

```
ubfiz w8, w23, #2, #1      ; (flag & 1) << 2
cmp   x24, #0              ; on?
mov   w9, #0xf
csel  w23, w9, w8, ne      ; target PS = on ? 0xF : (flag ? 4 : 0)
```

**Therefore, per AVE power domain:**

| `_E_AVE_PMGR_PS` | `DevicePowerState` | `enableDeviceClock` value | PMGR PS field written |
|---|---|---|---|
| 0 `PowerOff` | 5 | 0 | `0x0` |
| 1 `ClockOff` | 6 | 2 | `0x4` |
| 2 `ClockOn` | 7 | 1 | **`0xF`** |

**This closes docs/25 hypothesis 4 for the on-state.** macOS's "AVE domain on"
is a PS-field write of `0xf`, which is exactly `PS_ACTIVE` in Linux's
`apple-pmgr-pwrstate`. There is no second field, no settle poll visible at this
layer, and no additional device touched. The only state Linux cannot express is
the intermediate `0x4` (`ClockOff`), which AVE uses only via the `AVE_DPM`
tuning ladder — never before the first register access.

### Perf state: absent on this SoC

`AVE_PMGR::SetPerfState(PD, PState)` at `0xfffffe0008c4f7d0`:

```
0xfffffe0008c4f8e0  ldr x8, [x19, #8]      ; the AppleARMFunction
0xfffffe0008c4f8e4  cbz x8, error
...
0xfffffe0008c4fcc4  stur w8, [x29,#-84]    ; = aPD[PD]                (arg1, out/in)
0xfffffe0008c4fccc  stur w8, [x29,#-88]    ; = table_0xfffffe0007274564[PState]
                                           ;   PState 0..4 -> 0,0,1,1,2   (arg2)
0xfffffe0008c4fd14  blraa                  ; fn->callFunction(&idx, &floor, &byte)
```

`function-set_perf_state_floor` occurs **0 times** in
`data/blobs/adt.bin` (the j314c device tree), against 7 occurrences of
`function-mcc_dataset` in the same file — so the search is capable of a positive
and the absence is real. `AppleARMFunction::withProvider` therefore returns NULL,
the guard above fires, and every `SetPerfState` on M1 Max fails. The whole
`AVE_DPM` perf ladder is inert on this part.

Note the same two function names are referenced by `AppleAVD`
(`0xfffffe0007192669`, `0xfffffe00071922ad`), `AppleJPEGDriver`,
`AppleProResHW`, `AppleM2ScalerCSCDriver` and `AppleH11ANEInterface`. **AVD
works under Asahi with neither.**

### Callers and call order of `AVE_PMGR`

`AVE_PMGR::SetPS` (`0xfffffe0008c4f578`) callers:

| Caller | VA | Args |
|---|---|---|
| `AVE_HwC::Init` | `0xfffffe0008c1a3dc` | `(IOP, ClockOn, true)` — **the only power-up before the first register write** |
| `AVE_HwC::Init` | `0xfffffe0008c1bde4` | `(DMA/FE, ClockOn, true)` — `DevType == 0x1e` only |
| `AVE_HwC::Init` | `0xfffffe0008c1bf2c` | `(DMA/FE, PowerOff, true)` |
| `AVE_HwC::Init` | `0xfffffe0008c1bf5c` | `(IOP, PowerOff, true)` |
| `AVE_HwC::Init` | `0xfffffe0008c1a7a4` | error unwind |
| `AVE_HwC::Uninit` | `0xfffffe0008c1c638` | |
| `AVE_PMGR::Uninit` | `0xfffffe0008c4e45c` | |
| `AVE_PMGR::SetPState` | `0xfffffe0008c522a8`, `…d8`, `…364` | |
| `AVE_DPM::SetHw` | `0xfffffe0008c720d4` | |
| `AVE_DPM::TuneUpHw` / `TuneDownHw` | `0xfffffe0008c722e0` / `0xfffffe0008c72618` | |
| `AVE_DPM::ApplyDCS` / `ApplyFAB` | `0xfffffe0008c73024` / `0xfffffe0008c735c4` | |

`SetPS` → `SetPSUp` (`0xfffffe0008c51988`) → `SetPSDependencyUp`
(`0xfffffe0008c5172c`) → `SetPowerState` (`0xfffffe0008c51aac`), walking the
dependency tree already documented in [10-power.md](10-power.md). `PD_IOP` is
the root and has no up-dependencies, so `SetPS(IOP, ClockOn)` raises exactly one
gate: `aPD[0] = 0` → ADT `power-gates[0]` = **456** for `ave0`.

**So at the instant of the first register write, macOS has raised gate 456
only.** Our Linux experiment had `venc_sys`, `venc_dma`, `venc_pipe4`,
`venc_pipe5` and `venc_me0` all on — a strict superset — and still hung.

---

## 5. Waits on other services

Exactly one hard wait exists in the whole path:

```
AVE_SurfaceMgr::Init(AVE_DevInfo*)                0xfffffe0008c80400
  0xfffffe0008c80428  x0 = "IOSurfaceRoot"        (string at 0xfffffe00072ab64f)
  0xfffffe0008c80434  bl IOService::nameMatching(name, NULL)
  0xfffffe0008c80440  x1 = 0xFFFFFFFFFFFFFFFF     ; timeout = infinite
  0xfffffe0008c80444  bl IOService::waitForMatchingService(dict, ~0)
  0xfffffe0008c8044c  cbz x0, fail
```

`AppleAVE2` blocks in `AVE_Drv::IO_init` until `IOSurfaceRoot` exists. That is a
userspace-buffer-plumbing dependency (IOSurface is macOS's IOKit-side surface
registry) and has **no Linux analogue and no hardware effect** — a V4L2 driver
uses videobuf2 instead.

The only other matching call is asynchronous and cosmetic:
`AVE_AnalyticsService_Start` (`0xfffffe0008b5b040`–`0xfffffe0008b5b08c`) does
`serviceMatching` + `addMatchingNotification` for a telemetry service; it runs
*after* `IO_start` returns and cannot gate anything.

**No other blocking service wait, no publish/notify handshake, and no dependency
on any other hardware driver exists in the path to the first register access.**
The only IOKit matching calls anywhere reachable from `start` are the four
above (`nameMatching`, `waitForMatchingService`, `serviceMatching`,
`addMatchingNotification`). This was established by enumerating every external
call in the window (§3), not by grepping for a name.

---

## 6. What this rules out

Read against the open hypotheses in [25-bringup-results.md](25-bringup-results.md):

1. **"AppleAVE2 asks another kext to enable the block."** The complete external
   call list for the window is §3. Every entry is one of: memory allocation,
   locking, ktrace, IORegistry lookup, IOSurface plumbing, IOMapper/DART,
   interrupt-source creation, MMIO mapping, or the `AppleARMFunction` lookup for
   a property that **does not exist on this SoC**. There is no enable call.
   The kext does *not* invoke `callPlatformFunction` for anything except
   `gAppleARMPEFindCoprovider` in `probe`, which `avd0` also uses and Asahi
   ignores.

2. **Hypothesis 4 (macOS's PS handling differs from Asahi's).** Traced to the
   PS-field value: macOS writes `0xf`, same as Linux. Closed for the on-state.

3. **"AVE_PMGR asks for more than one thing per domain."** It has the *capacity*
   to (`enablePsdService`, `resetPsdService`, `callFunction`), but on `t6001`
   the HwFeature mask is `0`, the perf-state ADT property is absent, and
   `resetPsdService` is only reachable from the DPM ladder. **One thing per
   domain, and it is the same thing Linux does.**

4. **Hypothesis 1 (incomplete power — `venc_me1`).** *Not* addressed by this
   analysis, and now looks less likely: macOS reaches the first register write
   with only gate 456 (`IOP`) raised, so a fully-powered pipeline is evidently
   not a precondition for bank 2 responding on macOS.

What is **not** ruled out, and is the residue:

* Whatever the 36-byte PMGR-window aperture at `0x8E588000` (`ave0` `reg[3]`,
  `ave1` `reg[3]` = `0x8E680260`) is for. `AppleAVE2` maps it and never uses it;
  something else in the system may.
* Anything done before `AppleAVE2` loads at all — by iBoot, by
  `AppleT600xIO`/`ApplePMGR` at platform init, or by the `AppleARMIODevice` nub
  construction. This analysis starts at `probe`; it says nothing about the state
  the nub is already in.

---

## 7. Proposals (not performed)

Per `AGENTS.md`, these are written down, not run.

1. **Compare `_enableDeviceGated`'s full body against `apple-pmgr-pwrstate`.**
   This document read only the PS-value computation
   (`0xfffffe00097f853c`). The rest of `ApplePMGR::_enableDeviceGated`
   (`0xfffffe00097f848c`, ~2 KB) may contain a settle poll, an auto-clock-gate
   write, or a `_checkNotifyPMP` handshake (`0xfffffe00097f8564`) that Linux
   omits. **Pure static work, no hardware.**

2. **Find who owns `0x8E588000`.** Search the kernelcache for other consumers of
   that address or of an ADT node whose `reg` covers it. Cross-check the ADT for
   a node at `0x8E588000`. **Pure static work.**

3. **m1n1 hypervisor trace remains the decisive test**, as docs/25 concluded.
   This analysis narrows what to look for: on macOS, the first AVE MMIO
   transaction is a 32-bit write of `1` to `0x20D050038`, immediately after a
   single PMGR gate-456 activation and five `mapDeviceMemoryWithIndex` calls. If
   a trace shows any bus transaction between those, that transaction is the
   answer.

---

## Reproducing

```sh
# the first write
python3 tools/disas.py --kext --addr 0xfffffe0008c91c80 -n 0x50   # AVE_SVECtrl::SetIdle
python3 tools/disas.py --kext --addr 0xfffffe0008c1aea8 -n 0x10   # its call site

# the two platform-function lookups
python3 tools/disas.py --kext --addr 0xfffffe0008c4e6c0 -n 0x14   # PMGR, set_perf_state_floor
python3 tools/disas.py --kext --addr 0xfffffe0008c472fc -n 0x10   # MCC, mcc_dataset

# the two AppleARMIODevice entry points
python3 tools/disas.py --kext --addr 0xfffffe0008c4ef50 -n 0x80   # setDevicePowerState path
python3 tools/disas.py --kext --addr 0xfffffe0008c4f250 -n 0x80   # enablePsdService path

# the ADT negative (must print 0 for the first and 7 for the second)
python3 -c "d=open('data/blobs/adt.bin','rb').read(); \
  print(d.count(b'function-set_perf_state_floor'), d.count(b'function-mcc_dataset'))"
```
