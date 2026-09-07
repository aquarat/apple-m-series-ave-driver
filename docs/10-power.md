# AVE power and clock sequencing

Recovered from `AppleAVE2.kext` in `kc.macho` (M1 Pro/Max kernelcache). Every
offset and constant below cites the VA of the instruction or datum it was read
from. Anything not read out of the image is marked **inferred** or **unknown**.

Target part throughout is `t6001` (M1 Max, `j314c`).

## Headline

**`AVE_PMGR` performs no MMIO of its own.** It is a bookkeeping layer over
`AppleARMIODevice`, the generic Apple ARM I/O nub. Every state change goes out
as a virtual call on the provider:

```
AVE_PMGR::SetPowerState(PD, PS)
  -> AppleARMIODevice::setDevicePowerState(state, index)     vtable +0x8c0
     -> AppleT600xIO::enableDeviceClock(gateId, value)       vtable +0x8a8
        -> ApplePMGRFunctionClockGate::callFunction(...)     vtable +0x140
           -> AppleT6001PMGR / ApplePMGR::_enableDevice(idx, on, flag, die)
```

`index` is a **position in the ADT `power-gates` / `clock-gates` list**, and
`AppleARMIODevice` turns it into the gate id (456, 457, …). This is exactly the
mechanism Linux's `apple-pmgr-pwrstate` already implements; a Linux AVE driver
needs `power-domains` phandles in DT and `pm_runtime`/`genpd`, **not** a
register poke.

The one exception is clock gating, which is *not* a PMGR gate — see
[Clock gating](#clock-gating) below.

## Enums

Recovered from the kext's own name tables (chained-fixup pointers resolved
against the kernelcache base `0xfffffe0007004000`).

`_E_AVE_PMGR_PD` — `gsc_piaAVE_PMGR_PDName` at `0xfffffe0007ee0e18`, 11 entries:

| # | Name |
|---|---|
| 0 | `IOP` |
| 1 | `IOP_Mid` |
| 2 | `IOP_Mid2` |
| 3 | `IOP_Max` |
| 4 | `DMA/FE` |
| 5 | `Pipe4/HME` |
| 6 | `Pipe5/MDINTRA` |
| 7 | `ME0/ME` |
| 8 | `ME1/MDINTER` |
| 9 | `FAB` |
| 10 | `DCS` |

Eleven domains, matching the eleven ADT gates exactly.

`_E_AVE_PMGR_PS` — `gsc_piaAVE_PMGR_PSName` at `0xfffffe0007ee0e70`:

| # | Name |
|---|---|
| 0 | `PowerOff` |
| 1 | `ClockOff` |
| 2 | `ClockOn` |

Ordered, and compared with `<` / `>` throughout (`AVE_PMGR::SetPS` at
`0xfffffe0008c4f68c`), so higher = more on.

`_E_AVE_PMGR_PState` — `gsc_piaAVE_PMGR_PStateName` at `0xfffffe0007ee0e88`:

| # | Name |
|---|---|
| 0 | `Off` |
| 1 | `VMin` |
| 2 | `VMid` |
| 3 | `VMid2` |
| 4 | `VMax` |

## PD → ADT gate index

`AVE_PMGR::Init` (`0xfffffe0008c4e580`) stores
`AVE_DevCap_FindPDMap(devInfo->GetDevID())` at `this+0x38`
(`0xfffffe0008c4e6b4`, `0xfffffe0008c4e6d8`). The map begins with
`int aPD[11]`; a negative entry means the domain does not exist on this chip
(`AVE_PMGR::CheckPDAvail`, `0xfffffe0008c524c4`–`0xfffffe0008c524cc`).

For `t6001` the map is `_gc_sAVE_DevCap_PDMap_Caster` at `0xfffffe0007267920`
(reached via `gsc_saAVE_DevCap[12].+0x10 -> gsc_sAVE_DevCap_CEntry_6001+0x20`;
the `devID` table is `gsc_saAVE_DevCap` at `0xfffffe0007edba00`, stride `0x48`).
Its `aPD[11]` is:

```
0, 1, 9, 8, 7, 3, 4, 5, 6, 10, -1
```

Cross-referenced against `ave0`'s ADT `power-gates`
(`456, 457, 458, 301, 302, 303, 304, 300, 459, 600, 602` — index 0..10):

| PD | index | `ave0` gate | `ave1` gate |
|---|---|---|---|
| `IOP` | 0 | 456 | 549 |
| `IOP_Mid` | 1 | 457 | 550 |
| `IOP_Mid2` | 9 | 600 | 601 |
| `IOP_Max` | 8 | 459 | 552 |
| `DMA/FE` | 7 | 300 | 367 |
| `Pipe4/HME` | 3 | 301 | 368 |
| `Pipe5/MDINTRA` | 4 | 302 | 369 |
| `ME0/ME` | 5 | 303 | 370 |
| `ME1/MDINTER` | 6 | 304 | 371 |
| `FAB` | 10 | 602 | 603 |
| `DCS` | −1 | *not present on t6001* | |

Note index **2** (`ave0` gate 458, `ave1` gate 551) is **not referenced by any
PD** on `t6001`. Its purpose is unknown. `_gc_sAVE_DevCap_PDMap_Rhea`
(`0xfffffe0007267530`) does map `DCS` to index 2, so the slot is meaningful on
some other part — but not this one.

Confidence: the `aPD` values are read directly from the image and are
**confirmed**. Their pairing with the ADT list assumes `AppleARMIODevice`'s
per-device gate array (`this+0xa8`, count at `this+0xa0`; see
`0xfffffe0008a970e0` and `0xfffffe0008a97118`) is built from the ADT property in
order. Which of `power-gates` / `clock-gates` fills it was **not determined** —
but for `ave0` and `ave1` the two ADT lists are byte-identical, so the resulting
gate id is the same either way.

## Dependency graph

`AVE_PMGR::Init` also stores `PDMap + 0x2c` at `this+0x40`
(`0xfffffe0008c4e6d4`). That address is the **down** dependency set;
`AVE_PMGR::SetPSUp` adds `+0xe0` to reach the **up** set
(`add x1, x25, #0xe0` at `0xfffffe0008c51a7c`), while `AVE_PMGR::SetPSDown`
passes it unchanged (`0xfffffe0008c51e3c`).

```c
struct _S_AVE_DevCap_PDDepSet {   /* 0xe0 */
    int32_t  count;               /* AVE_PMGR_FindPD, 0xfffffe0008c4e2dc */
    struct _S_AVE_PMGR_PDDep {    /* 0x14, stride from 0xfffffe0008c4e2ec */
        int32_t  pd;              /* +0x00 */
        int32_t  nDepSet;         /* +0x04, CheckPDDepUp 0xfffffe0008c511c4 */
        uint32_t aPDFlag[3];      /* +0x08 .. +0x10 */
    } entry[11];
};
struct _S_AVE_DevCap_PDMap {      /* 0x1f8 stride, 0xfffffe0007267338-0xfffffe0007267140 */
    int32_t aPD[11];              /* +0x00 */
    PDDepSet down;                /* +0x2c */
    PDDepSet up;                  /* +0x10c */
    int32_t  tail[3];             /* +0x1ec — purpose unknown (0x1f0,0x1e0,0x1e0) */
};
```

`aPDFlag[]` are 11-bit masks over `_E_AVE_PMGR_PD`.

### `t6001` up dependencies

Read from `_gc_sAVE_DevCap_PDMap_Caster + 0x10c`, `count = 10` (`DCS` absent).
`aPDFlag[0]` is the mask that gets raised; further entries are *alternatives*
accepted by the check (`CheckPDDepUp` returns success if **any** listed mask is
already up — loop at `0xfffffe0008c511dc`–`0xfffffe0008c51208`).

| PD | must be up first |
|---|---|
| `IOP` | *(none — root)* |
| `IOP_Mid` | `IOP` |
| `IOP_Mid2` | `IOP_Mid` |
| `IOP_Max` | `IOP_Mid2` |
| `DMA/FE` | `IOP` |
| `Pipe4/HME` | `DMA/FE` |
| `Pipe5/MDINTRA` | `DMA/FE` |
| `ME0/ME` | `Pipe4/HME` **or** `Pipe5/MDINTRA` (`nDepSet = 2`) |
| `ME1/MDINTER` | `ME0/ME` |
| `FAB` | *(none)* |

So the power-up order is a two-branch tree rooted at `IOP`:

```
IOP ──┬─ IOP_Mid ── IOP_Mid2 ── IOP_Max          (the "perf state" ladder)
      └─ DMA/FE ──┬─ Pipe4/HME ─────┐
                  └─ Pipe5/MDINTRA ─┴─ ME0/ME ── ME1/MDINTER
FAB   (independent)
DCS   (absent on t6001)
```

### `t6001` down dependencies

Read from `_gc_sAVE_DevCap_PDMap_Caster + 0x2c`, `count = 10`. `aPDFlag[0]` is
what must come down first; `aPDFlag[2]` is the **peer** mask (see below).

| PD | must be down first | peer mask |
|---|---|---|
| `IOP` | `IOP_Mid`, `DMA/FE` | — |
| `IOP_Mid` | `IOP_Mid2` | — |
| `IOP_Mid2` | `IOP_Max` | — |
| `IOP_Max` | *(none)* | — |
| `DMA/FE` | `Pipe4/HME`, `Pipe5/MDINTRA` | — |
| `Pipe4/HME` | `ME0/ME` | `Pipe5/MDINTRA` |
| `Pipe5/MDINTRA` | `ME0/ME` | `Pipe4/HME` |
| `ME0/ME` | `ME1/MDINTER` | — |
| `ME1/MDINTER` | *(none)* | — |
| `FAB` | *(none)* | — |

## "Peer" is **not** the other AVE instance

This is worth stating flatly, because it constrains the driver design.

`AVE_PMGR::CheckPeerDown` (`0xfffffe0008c51528`) takes a mask, and its whole
body (`0xfffffe0008c51618`–`0xfffffe0008c51648`) is:

```
for (i = 0; i < 11; i++)
    if (mask >> i & 1)
        if (this->m_aPS[i] > ps)   /* this+0x48 + 4*i */
            return 0;              /* a peer still needs this domain */
return -1009;
```

The array indexed is `this+0x48`, this instance's own per-PD state array
(same array `AVE_PMGR::GetPS` returns from, `0xfffffe0008c52034`). The mask is
an 11-bit `_E_AVE_PMGR_PD` mask. There is no cross-object access anywhere in the
function.

For `t6001` the only non-zero peer masks are `Pipe4/HME ↔ Pipe5/MDINTRA`
(`0x040` / `0x020`). So "peer" means **the sibling pipe domain inside the same
AVE instance**, which shares a downstream consumer (`ME0/ME`) and must not be
gated while the sibling still needs it.

`AVE_PMGR::CheckPeerUp` (`0xfffffe0008c5151c`) is a three-instruction stub:

```
fffffe0008c5151c: bti c
fffffe0008c51520: mov w0, #0x0
fffffe0008c51524: ret
```

and a scan of every `BL` in `__TEXT_EXEC` found **no callers** at all.

**Conclusion: `ave0` and `ave1` are not coupled by `AVE_PMGR`.** Each
`AppleAVE2Driver` instance constructs its own `AVE_PMGR` with its own
`id` (`AVE_PMGR::Init` requires `id < 4`, `0xfffffe0008c4e688`) and its own
provider nub. Nothing in this class reaches the other encoder. Confidence:
high for `AVE_PMGR`; **not checked** for other classes (`AVE_Drv`,
`AppleAVE2Driver`, or the `coprovider-group = "ave"` ADT property, which may
still impose driver-level serialisation).

## The algorithm

```
SetPS(pd, ps, apply)                       0xfffffe0008c4f578
    cur = m_aPS[pd]
    if cur > ps:  rc = SetPSDown(pd, ps, apply)
    elif cur < ps: rc = SetPSUp(pd, ps, apply)
    else: return 0
    if apply && rc == -1008:                /* no dep entry for this PD */
        rc = SetPowerState(pd, ps)          /* go straight to hardware */
    return rc

SetPSUp(pd, ps, apply)                     0xfffffe0008c51988
    rc = SetPSDependencyUp(m_pPDMap->up, pd, ps, apply)
    if apply && rc == 0: rc = SetPowerState(pd, ps)

SetPSDependencyUp(set, pd, ps, apply)      0xfffffe0008c5172c
    e = find entry with e->pd == pd; if none return -1008
    rc = CheckPDDepUp(e, ps)
    if !apply || rc == 0: return rc
    for i in 0..10:                         /* deps not met -> raise them */
        if e->aPDFlag[0] >> i & 1: rc = SetPSUp(i, ps, true)   /* recursive */
    return rc

SetPSDown / SetPSDependencyDown            0xfffffe0008c51e0c / 0xfffffe0008c51b94
    same shape, but additionally:
    rc = CheckPeerDown(e->aPDFlag[2], pd, ps)   0xfffffe0008c51d2c
    if rc == 0: return 0                        /* peer still up -> abort */
    rc = CheckPDDepDown(e, ps)
    if rc == 0: return 0
    lower every PD in e->aPDFlag[0] via SetPSDown
```

Predicates:

| function | VA | meaning |
|---|---|---|
| `CheckPSFlagUp(mask, ps)` | `0xfffffe0008c50dec` | 0 iff every PD in `mask` has `m_aPS >= ps` |
| `CheckPSFlagDown(mask, ps)` | `0xfffffe0008c50fd8` | 0 iff every PD in `mask` has `m_aPS <= ps` |
| `CheckPDDepUp(dep, ps)` | `0xfffffe0008c511c4` | 0 if **any** of `dep->aPDFlag[0..n-1]` passes `CheckPSFlagUp` |
| `CheckPDDepDown(dep, ps)` | `0xfffffe0008c513e0` | 0 only if **all** pass `CheckPSFlagDown` |
| `CheckPDAvail(pd)` | `0xfffffe0008c524a4` | 0 if `pd <= 10` and `m_pPDMap[pd] >= 0` |

Recursion terminates because the graph is a DAG and each level moves toward
`IOP` (up) or the leaves (down).

## The hardware call

`AVE_PMGR::SetPowerState(pd, ps)` — `0xfffffe0008c4ea9c`.

1. Reject `pd > 10` (`0xfffffe0008c4eb8c`) or `ps >= 3` (`0xfffffe0008c4eb94`).
2. Reject if `m_pPDMap[pd] < 0` (`0xfffffe0008c4ebc8`).
3. If `m_aPS[pd] == ps` already, log and return 0 (`0xfffffe0008c4ee84`).
4. Dispatch, branching on **bit 1 of the chip's `HwFeature` word**
   (`tbnz w8, #1` at `0xfffffe0008c4ef28`; `HwFeature` comes from
   `AVE_DevCap_FindHwFeature` and is stored at `this+0x30`,
   `0xfffffe0008c4e6ec`). For `t6001` `HwFeature == 1`
   (`gsc_sAVE_DevCap_CEntry_6001+0x10`), so bit 1 is clear and the **first**
   path is taken.

**Path A — `HwFeature & 2 == 0` (this is the M1 Max path):**

```
provider->setDevicePowerState(state = gsc_iaAVE_PMGR_PSMap[ps],
                              index = m_pPDMap[pd])
```
`ldr x9, [x16, #2240]` at `0xfffffe0008c4efbc` — vtable `+0x8c0` =
`AppleARMIODevice::setDevicePowerState`. Arguments loaded at
`0xfffffe0008c4ef74` (state) and `0xfffffe0008c4ef90` (index).

`gsc_iaAVE_PMGR_PSMap` at `0xfffffe0007274558` = `{5, 6, 7}`:

| `_E_AVE_PMGR_PS` | `AppleARMIODevice::DevicePowerState` |
|---|---|
| `PowerOff` (0) | 5 |
| `ClockOff` (1) | 6 |
| `ClockOn` (2) | 7 |

`setDevicePowerState` (`0xfffffe0008a970dc`) validates `5 <= state < 8`
(`0xfffffe0008a970ec`), then translates through a kernel-side 3-entry table at
`0xfffffe0007168060` = `{0, 2, 1}` (`0xfffffe0008a970fc`, `0xfffffe0008a97134`)
and tail-calls `enableDeviceClock(gateId, value)` on its own provider
(vtable `+0x8a8`, `0xfffffe0008a97150`).

Net effect for `t6001`:

| PD state | `DevicePowerState` | value handed to `enableDeviceClock` |
|---|---|---|
| `PowerOff` | 5 | 0 |
| `ClockOff` | 6 | 2 |
| `ClockOn` | 7 | 1 |

`AppleT600xIO::enableDeviceClock` (`0xfffffe0009aa305c`) forwards to a cached
`AppleARMFunction` (vtable `+0x140` = `callFunction`, `0xfffffe0009aa30f0`).
`ApplePMGRFunctionClockGate::callFunction` (`0xfffffe000980ab54`) splits the
gate id into `die = id >> 28` (`0xfffffe000980ab7c`) and
`index = id & 0x0fffffff` (`0xfffffe000980ac28`), requires `value < 4`
(`0xfffffe000980ac84`), and calls
`ApplePMGR::_enableDevice(index, value & 1, (value >> 1) & 1, die)`
(`0xfffffe000980acc4`–`0xfffffe000980acf4`, vtable `+0x948` =
`__ZN9ApplePMGR13_enableDeviceEmmmm`; `AppleT6001PMGR` inherits it unchanged).

**Path B — `HwFeature & 2 != 0` (not used on `t6001`):**

```
provider->enablePsdService(gsc_iaAVE_PSD_PSMap[ps][0],
                           m_pPDMap[pd],
                           gsc_iaAVE_PSD_PSMap[ps][1])
```
`ldr x9, [x16, #2248]` at `0xfffffe0008c4f2bc` — vtable `+0x8c8` =
`AppleARMIODevice::enablePsdService`. `gsc_iaAVE_PSD_PSMap` at
`0xfffffe0007274540` (`adrp`+`add` at `0xfffffe0008c4f254`) is
`{{0, -1}, {1, 10}, {1, 10}}`.

On success (either path) the object state is updated:
`m_aPS[pd] = ps` (`str w21, [x27]` at `0xfffffe0008c4f4d4`) and
`m_aPState[pd] = (ps ? 1 : 0)` (`0xfffffe0008c4f4fc` / `0xfffffe0008c4f504`).

### Polling and timeouts

`AVE_PMGR` does **no polling and applies no delay of its own** — there is no
loop, no `IODelay`, and no timer anywhere in `SetPowerState`, `SetPS`,
`SetPSUp`, or `SetPSDown`. The synchronous wait is entirely inside the platform
PMGR driver.

`ApplePMGR::waitForActualPS(die, index, targetPS, timeout_us, int *err)` at
`0xfffffe00097e5120`:

- reads `mach_absolute_time()` (`0xfffffe00097e5190`),
- converts `timeout_us * 1000` ns to a deadline (`mov w8, #0x3e8` /
  `umull` at `0xfffffe00097e519c`),
- busy-spins reading the actual PS field and comparing against the target
  (`0xfffffe00097e51ac`–`0xfffffe00097e51d8`),
- on expiry writes `0xe00002d6` = `kIOReturnTimeout` (`0xfffffe00097e51e0`).

**What is polled:** the PMGR `PS` register's *actual* field for the device,
compared with the *target* field — i.e. exactly the `PS_ACTUAL`/`PS_TARGET`
protocol Linux's `apple-pmgr-pwrstate` already implements. **The timeout value
is caller-supplied and I did not resolve which value the device path passes**
(`waitForActualPS` is reached virtually, so no `BL` call site exists to read the
immediate from). Unknown.

## Clock gating is a different mechanism

`AVE_PMGR::SetClockGating(bool)` — `0xfffffe0008c50970` — is **not** a PMGR
gate. It:

1. returns immediately if the value is unchanged (`0xfffffe0008c50a44`),
2. calls `AVE_SVECtrl::SetIdle(v)` on `this+0x28` (`0xfffffe0008c50a54`,
   target `0xfffffe0008c91c80`),
3. on success caches the flag at `this+0xa0` (`0xfffffe0008c50ae8`;
   `GetClockGating` reads it back at `0xfffffe0008c50cf4`).

`AVE_SVECtrl::SetIdle` is a single register write (`0xfffffe0008c91ca8`–
`0xfffffe0008c91cbc`):

```
AVE_Reg::Write32(m_pReg /*+0x18*/, bank = 2, offset = m_pRegTbl[+0x10c], value)
```

`AVE_Reg::Write32` (`0xfffffe0008c53e58`) indexes a base array at `AVE_Reg+0x40`
by bank (`0xfffffe0008c53e70`), so **bank = ADT `reg` entry index** — see
[01-hardware.md](01-hardware.md). Bank 2 for `ave0` is `0x20D050000`.

`m_pRegTbl` is `AVE_SVECtrl_GetReg(devInfo->GetChipType())`, stored at
`AVE_SVECtrl+0x20` (`0xfffffe0008c91010`). `AVE_SVECtrl_GetReg`
(`0xfffffe0008c90e44`) indexes a table at `0xfffffe0007ee0f80` by
`chipType - 1`. `chipType` for `devID = 12` (`t6001`) is **7**, read from
`gsc_saAVE_DevCap[12] + 0x8` (`AVE_DevID2ChipType`, `0xfffffe0008babb40`;
`AVE_DevCap_Find`, `0xfffffe0008ba9eac`) — which selects
`gsc_sAVE_SVECtrl_Reg_Rhea` (`0xfffffe00072748b4`). Its `+0x10c` word is
**`0x38`**.

So on `ave0` the AVE clock-gate/idle register is (**inferred** from the above
chain, not observed on hardware):

```
0x20D050000 + 0x38 = 0x20D050038      write 1 = gate, 0 = ungate
```

Other chips: `gsc_sAVE_SVECtrl_Reg_Hypnos` `+0x10c` = `0x3c`,
`gsc_sAVE_SVECtrl_Reg_Themis` `+0x10c` = `0x118`.

This is a useful data point for `reg[2]`, whose role
[01-hardware.md](01-hardware.md) currently lists as unknown.

## Performance states

`_E_AVE_PMGR_PState` is **not** a DVFS voltage/frequency table on this part. It
is a ladder of IOP clock domains.

`AVE_PMGR::SetPState(pd, pstate, apply, flags)` — `0xfffffe0008c5205c` —
branches on whether the `function-set_perf_state_floor` platform function
exists (`ldr x8, [x19, #8]; cbz` at `0xfffffe0008c52174`). That pointer is set
in `Init` by looking up the property named
`"function-set_perf_state_floor"` (string at `0xfffffe00072a62e6`) via
`AppleARMFunction::withProvider` (`0xfffffe0008c4e6cc`).

**`ave0` and `ave1` in the `j314c` ADT do not carry that property**
(`data/derived/adt-ave-nodes.txt` lists only `function-mcc_dataset`), so on M1
Max the pointer is null and the second path is taken.

**Path with no platform function (M1 Max):** apply
`gsc_saAVE_PMGR_PState_PDPSMap` at `0xfffffe0007274578`, a 5 × 36-byte table
(`umaddl x8, w9, w10, x8` with `w10 = 36` at `0xfffffe0008c52290`;
row = `{int n; struct {int pd; int ps;} pair[4];}`, walked at
`0xfffffe0008c5229c`–`0xfffffe0008c522b4`), each pair issued as
`SetPS(pd, ps, apply=1)`:

| PState | actions |
|---|---|
| `Off` (0) | `IOP` → `PowerOff` |
| `VMin` (1) | `IOP_Max` → `PowerOff`, `IOP_Mid2` → `PowerOff`, `IOP_Mid` → `PowerOff`, `IOP` → `ClockOn` |
| `VMid` (2) | `IOP_Max` → `PowerOff`, `IOP_Mid2` → `PowerOff`, `IOP_Mid` → `ClockOn` |
| `VMid2` (3) | `IOP_Max` → `PowerOff`, `IOP_Mid2` → `ClockOn` |
| `VMax` (4) | `IOP_Max` → `ClockOn` |

So "raising the perf state" literally means ungating one more step of the
`IOP → IOP_Mid → IOP_Mid2 → IOP_Max` chain. The `up` dependency graph makes each
step pull in its predecessor automatically. Five discrete states, no frequency
or voltage values anywhere in the kext.

**Path with the platform function (other parts):**
`AVE_PMGR::SetPerfState(pd, pstate)` — `0xfffffe0008c4f7d0` — no-ops if
`m_aPState[pd] == pstate` (`0xfffffe0008c4f930`), then calls
`callFunction(&index, &level, &out)` on the `AppleARMFunction`
(vtable `+0x140`, `0xfffffe0008c4fcfc`) with
`index = m_pPDMap[pd]` (`0xfffffe0008c4fcc4`) and
`level = gsc_iaAVE_PMGR_PStateMap[pstate]` (table at `0xfffffe0007274564`,
loaded at `0xfffffe0008c4fcc8`) = `{0, 0, 1, 1, 2}`. So the floor levels handed
to the platform are only `0`, `1`, `2`.

## `ResetPSD`

`AVE_PMGR::ResetPSD(pd)` — `0xfffffe0008c5012c` — is gated on
`HwFeature & 2` (`tbnz w8, #1` at `0xfffffe0008c50218`). **On `t6001`
`HwFeature == 1`, so `ResetPSD` returns 0 without doing anything.**

Where it does run it calls `provider->resetPsdService(m_pPDMap[pd])`
(vtable `+0x8d0`, `ldr x9, [x16, #2256]` at `0xfffffe0008c50430`) and then
clears `m_aPS[pd]` and `m_aPState[pd]` to 0 (`0xfffffe0008c507b4`,
`0xfffffe0008c507d0`).

## Sequencing in context

There is no `AVE_HwC::PowerOn` / `PowerOff` — those methods live on `AVE_DPM`
(`0xfffffe0008c73acc` / `0xfffffe0008c74044`). A `BL`-scan of the whole kext
found `AVE_PMGR` entry points called from exactly two classes: `AVE_HwC`
(`Init`, `Uninit`) and `AVE_DPM`.

### `AVE_HwC::Init` (`0xfffffe0008c19de8`)

Bring-up order, from the call sequence (logging calls elided):

| VA | call |
|---|---|
| `0xfffffe0008c1a0d0` | `AVE_PMGR::AVE_PMGR()` |
| `0xfffffe0008c1a0f0` | `AVE_PMGR::Init(provider, cfg, devInfo, id, sveCtrl)` |
| `0xfffffe0008c1a3dc` | **`SetPS(IOP, ClockOn, apply=1)`** (args at `0xfffffe0008c1a3d0`–`0xfffffe0008c1a3d8`) |
| `0xfffffe0008c1a93c` | `AVE_DART::Init` |
| `0xfffffe0008c1ac10` | `AVE_Reg::Init` — MMIO is mapped **after** power-up |
| `0xfffffe0008c1ad74` | `AVE_SVECtrl::Init` |
| `0xfffffe0008c1aeb0` | **`SetClockGating(true)`** (`mov w1, #1` at `0xfffffe0008c1aeac`) |
| `0xfffffe0008c1afc0` | `AVE_DPM::Init(devInfo, cfg, id, pmgr)` |
| `0xfffffe0008c1b13c` | `AVE_DPE::Init`, then `AVE_DPE::Reset` |
| `0xfffffe0008c1b444` | `AVE_MCC::Init` |
| `0xfffffe0008c1b5c8` | `AVE_FwImg::Init` |
| `0xfffffe0008c1b78c` | `AVE_FwLog::Init` |
| `0xfffffe0008c1b960` | `AVE_AXI2AF::Init` |
| `0xfffffe0008c1bbd8` | `AVE_IOP::Init` |
| `0xfffffe0008c1bd38` | `AVE_HwC::CreateFwHeartBeatTimer` |
| `0xfffffe0008c1bd4c` | `AVE_SurfaceMgr::DARTMapSurface` |
| `0xfffffe0008c1bde4` | `SetPS(DMA/FE, ClockOn, 1)` — **conditional**, see below |
| `0xfffffe0008c1bf0c` | `AVE_Reg::Read32(bank 5, offset 0x9c000)` → `AVE_DevInfo::SetDevRevision` — same conditional |
| `0xfffffe0008c1bf2c` | `SetPS(DMA/FE, PowerOff, 1)` — same conditional |
| `0xfffffe0008c1bf38` | `AVE_SurfaceMgr::DisableOp`, `AVE_DART::SetActive(false)` |
| `0xfffffe0008c1bf5c` | **`SetPS(IOP, PowerOff, 1)`** — unconditional |

Two things worth flagging for a Linux driver:

- **`AVE_PMGR::SetPS(IOP, ClockOn)` is the very first hardware action**,
  before the DART or any register mapping. Everything else in `Init` runs with
  only the `IOP` branch of the tree powered.
- **`Init` leaves the block fully powered off.** The last `SetPS` takes `IOP`
  to `PowerOff`, which cascades down the `down` graph. Runtime power-up is
  `AVE_DPM`'s job.

The `DMA/FE` power-cycle and revision read at `0xfffffe0008c1bdd4`–
`0xfffffe0008c1bf2c` sit behind `cmp w26, #0x1e; b.ne` at
`0xfffffe0008c1bdcc`. The only non-logging definition of `x26` in the function
is `AVE_DevInfo::GetDevType()` at `0xfffffe0008c1a084`/`0xfffffe0008c1a088`,
which would make the branch apply to `devType == 30` = `t8150` only, and
therefore **not** to `t6001` (`devType 10`). **Inferred, not proven** — the
register is reused by intervening logging blocks and I could not rule out a
different live value. If the branch *is* taken on M1 Max the effect is only an
extra transient `DMA/FE` on/off around one register read; it is not part of the
steady-state sequence either way.

### `AVE_PMGR::Uninit` (`0xfffffe0008c4e374`)

```
SetPowerState(DCS, PowerOff)   0xfffffe0008c4e440  (direct, bypasses deps)
SetPS(IOP, PowerOff, apply=1)  0xfffffe0008c4e458  (cascades)
zero the object
```

### `AVE_DPM` — runtime power management

`AVE_DPM` drives per-workload power levels (`_E_AVE_DPM_PL`) and is the only
runtime consumer:

| method | VA of the `AVE_PMGR` call | behaviour |
|---|---|---|
| `SetHw(PL)` | `0xfffffe0008c720d4`, `0xfffffe0008c720f0` | for each of 11 PDs in mask `m_pDPMMap[0]`: `SetPS(pd, PL ? ClockOn : PowerOff, 1)`; then `SetClockGating(PL == 0)` |
| `TuneUpHw(mask)` | `0xfffffe0008c722e0`, `0xfffffe0008c722f8` | for each PD in `m_pDPMMap[1] & mask`: `SetPS(pd, ClockOn, 1)`; then `SetClockGating(false)` |
| `TuneDownHw(mask)` | `0xfffffe0008c72618`, `0xfffffe0008c72648` | per PD, `SetPS(pd, PowerOff or ClockOff, 1)`; then `SetClockGating(true)` conditionally |
| `ApplyIOP(PL, n)` | `0xfffffe0008c72a38` | `SetPState(IOP, VMin \| VMid \| VMid2, apply=1, 0)` (`0xfffffe0008c72a0c`, `0xfffffe0008c72a1c`, `0xfffffe0008c72a2c`) |
| `ApplyDCS(PL)` | `0xfffffe0008c73024` | `SetPS(DCS, PL < 2 ? PowerOff : ClockOn, 1)` (`0xfffffe0008c73014`–`0xfffffe0008c7301c`) |
| `ApplyFAB(PL)` | `0xfffffe0008c735c4` | `SetPS(FAB, PL < 4 ? PowerOff : ClockOn, 1)` (`0xfffffe0008c735b4`–`0xfffffe0008c735bc`) |

Note `ApplyIOP` only ever selects `VMin`/`VMid`/`VMid2` — `VMax` is not reachable
from `AVE_DPM`.

The `m_pDPMMap` masks come from `AVE_DevCap_FindDPMMap(devID, clientType,
encType)` and are 3 × `uint32` PD masks. `AVE_DPM` reads them from `this+0x20`
(`0xfffffe0008c720b4`, `0xfffffe0008c722bc`) — that the field holds the DPMMap
is **inferred** from the access shape, not from reading `AVE_DPM::Init`.

For `t6001` (`gsc_sAVE_DevCap_CEntry_6001` selects the `_Panda` variants at
`+0x68`, `+0xb0`, `+0xf8`):

| DPMMap | `[0]` | `[1]` | `[2]` |
|---|---|---|---|
| `LRME_Panda` `0xfffffe0007268588` | `DMA/FE, Pipe5/MDINTRA, ME0/ME` | same | + `ME1/MDINTER` |
| `AVC_Panda` `0xfffffe00072685b8` | `DMA/FE, Pipe4/HME, ME0/ME` | same | + `ME1/MDINTER` |
| `HEVC_Panda` `0xfffffe00072685dc` | `DMA/FE, Pipe5/MDINTRA, ME0/ME` | same | + `ME1/MDINTER` |

That is: **AVC uses the `Pipe4/HME` branch, HEVC and LRME use the
`Pipe5/MDINTRA` branch** — which is exactly why `ME0/ME` has two alternative up
dependencies and why the two pipes are peers of each other on the way down.

## Error codes

| value | hex | meaning (from context) |
|---|---|---|
| −1001 | `0xfffffc17` | bad parameter (`0xfffffe0008c4ed98`, `0xfffffe0008c5234c`) |
| −1002 | `0xfffffc16` | chip not supported (`0xfffffe0008c4ea94`) |
| −1008 | `0xfffffc10` | no dependency entry for this PD (`0xfffffe0008c51850`) |
| −1009 | `0xfffffc0f` | dependency / flag / peer check failed (`0xfffffe0008c51640`) |
| −1011 | `0xfffffc0d` | provider call failed (`0xfffffe0008c4f02c`) |
| −1015 | `0xfffffc09` | PD out of range or no map (`0xfffffe0008c524d4`) |

## `AVE_PMGR` object layout

| offset | field | cite |
|---|---|---|
| `0x00` | `AppleARMIODevice *m_pcProvider` (`OSDynamicCast`) | `0xfffffe0008c4e6a4`; metaclass GOT `0xfffffe0007ed8f30` → `__ZN16AppleARMIODevice9metaClassE` |
| `0x08` | `AppleARMFunction *` for `function-set_perf_state_floor` | `0xfffffe0008c4e6d0` |
| `0x10` | `const _S_AVE_Cfg *` | `0xfffffe0008c4e6f0` |
| `0x18` | `AVE_DevInfo *` | `0xfffffe0008c4e6f0` |
| `0x20` | `uint32_t id` (instance, `< 4`) | `0xfffffe0008c4e6f4` |
| `0x28` | `AVE_SVECtrl *` | `0xfffffe0008c4e950` |
| `0x30` | `HwFeature` bitmask | `0xfffffe0008c4e6ec` |
| `0x38` | `const int *m_pPDMap` (`aPD[11]`) | `0xfffffe0008c4e6d8` |
| `0x40` | `const PDDepSet *` (down; up at `+0xe0`) | `0xfffffe0008c4e6d8`, `0xfffffe0008c51a7c` |
| `0x48` | `int m_aPS[11]` | `0xfffffe0008c52034` |
| `0x74` | `int m_aPState[11]` | `0xfffffe0008c52470` |
| `0xa0` | `bool m_bClockGating` | `0xfffffe0008c50cf4` |

## What this means for a Linux driver

1. **Use `genpd`, not registers.** Add `power-domains` to the AVE node
   referencing the eleven PMGR gates in the ADT order, and let
   `apple-pmgr-pwrstate` do the work. The AVE-specific part is only the
   ordering.
2. **Order matters and is not alphabetical.** `IOP` first, then either
   `IOP_Mid → IOP_Mid2 → IOP_Max` (perf ladder) or
   `DMA/FE → {Pipe4/HME | Pipe5/MDINTRA} → ME0/ME → ME1/MDINTER` (datapath).
   `FAB` is independent. Down is the exact reverse, plus the
   `Pipe4/HME ↔ Pipe5/MDINTRA` peer rule.
3. **`ave0` and `ave1` are independent** as far as `AVE_PMGR` is concerned.
4. **Clock gating is an AVE register write**, plausibly `0x20D050038` for
   `ave0`, not a PMGR operation. It is asserted once during `Init` and
   toggled by `AVE_DPM` around work.
5. **A minimal bring-up needs only `IOP` at `ClockOn`.** The datapath domains
   are not needed until an encode job runs.
6. Three ADT gates are not obviously accounted for on `t6001`: index 2
   (gate 458 / 551) is unmapped, and `DCS` is absent. Powering them on
   unconditionally may or may not be harmless — **unknown**.

## Not determined

- The `waitForActualPS` timeout value used on the device path.
- Which ADT property (`power-gates` vs `clock-gates`) fills
  `AppleARMIODevice`'s gate array — moot for AVE, since the two lists are
  identical.
- What ADT gate index 2 (`ave0` 458, `ave1` 551) is for on `t6001`.
- The three trailing `int`s at `_S_AVE_DevCap_PDMap + 0x1ec`
  (`0x1f0, 0x1e0, 0x1e0`).
- Whether `w26` at `0xfffffe0008c1bdcc` really is `GetDevType()` (see above).
- Whether any coupling between `ave0` and `ave1` exists *outside* `AVE_PMGR`
  (`coprovider-group = "ave"` in the ADT suggests the possibility; not
  investigated).
