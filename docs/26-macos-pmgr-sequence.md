# What macOS actually does to bring a PMGR device up

Read out of `data/blobs/kc.macho` (M1 Pro/Max kernelcache), kexts
`com.apple.driver.ApplePMGR` (file `0x603230`) and
`com.apple.driver.AppleT6001PMGR` (file `0x6b0da0`), plus the `j314c` ADT
(`data/blobs/adt.bin`). Every constant below cites the VA of the instruction or
datum it came from. Anything not read out of an image is marked **inferred** or
**unknown**.

**No hardware was touched to produce this document.** The proposals in
[§9](#9-proposals-for-the-human-operator-only) are proposals, not results.

Symbols for these kexts are not in `data/derived/kext-symbols.txt` (which is
`AppleAVE2`-only). They were extracted from each fileset entry's `LC_SYMTAB`;
the pattern is the same as `data/derived/t6000-symbols.txt`.

---

## 1. Headline — read this first

Everything below is a real difference from `apple-pmgr-pwrstate`, but the
single most important result is a **negative with a working control**:

> **Every extra thing macOS does for `VENC_SYS`, it also does for `AVD_SYS`.**
> `AVD_SYS` is driven by the *same* `apple-pmgr-pwrstate` code on the *same*
> silicon and AVD works. So none of the PMGR-side differences found here is a
> candidate explanation for "any AVE register access hangs the fabric" on its
> own.

Specifically: `notify_pmp` is set on **both** `VENC_SYS` and `AVD_SYS`
(§7); `ps_cfg16` (`PS_MIN`) is `0` for **both**; both are `perf`+`b7` devices
with one bridge each; both sit under `AFR`; both get the DPE and bridge
configuration at power-on; both are `mediaPsRegs` members. The
`notify_pmp`-set-on-`VENC_SYS`-but-not-`VENC_DMA` asymmetry that motivated this
task is exactly mirrored by `AVD_SYS` (set) versus its own children — it is the
normal shape of a "block root" device, not a VENC peculiarity.

The four differences that are *real* and worth testing anyway, in order:

1. **macOS never changes `PS_TARGET` while `AUTO_ENABLE` is set.** It clears
   bit 28 and waits for `PS_ACTUAL == 0xf` *first*, in a separate write, and it
   **panics** if it ever finds `AUTO_ENABLE` set with `PS_TARGET != 0xf`
   (`0xfffffe00097d89fc`–`0xfffffe00097d8a00` → `_setPSLevel.cold.5`
   `0xfffffe000981e23c` → `_panic`). Apple's own assertion text says it
   plainly: **`"Unexpected PS state MANUAL_PS=%x for %s"`**
   (`0xfffffe000760e59e`). Asahi writes `PS_TARGET` with `AUTO_ENABLE` left as
   it found it. This is a hardware-contract violation Asahi commits on every
   power cycle after the first.
2. **A full power-on is up to three writes and two polls**, not one write and
   one poll (§4.5).
3. **`PS_MIN` (19:16) is programmed from ADT `ps_cfg16`** before the level
   change (`0xfffffe00097f9820`). Asahi never writes that field. For VENC/AVD
   the value is 0 so this is a no-op *here*, but `AFR` — the shared parent of
   both — has `ps_cfg16 = 0x04`, i.e. macOS pins `AFR`'s auto-PM floor at
   `CLKGATE` and Asahi leaves it at `0`.
4. **`WAS_CLKGATED | WAS_PWRGATED` (bits 9:8) are written as 1** on every
   transition to a level ≤ 14, and cleared only on the final `AUTO_ENABLE`
   write. Asahi always writes them as 0. The live dump (`…0300` / `…03ff`
   everywhere) is consistent with "Linux has never cleared them".

---

## 2. The call chain

From `docs/10-power.md` the AVE kext reaches the platform through
`AppleARMIODevice::setDevicePowerState` →
`AppleT600xIO::enableDeviceClock` → `ApplePMGRFunctionClockGate::callFunction`
→ `ApplePMGR::_enableDevice(index, value & 1, (value >> 1) & 1, die)`.
That last hop is vtable `+0x948`; decoding the `AppleT6000PMGR` vtable
(`__ZTV14AppleT6000PMGR` in the `AppleT6001PMGR` fileset,
`0xfffffe0008254fa0`) slot `+0x948` gives `0xfffffe00097f82f4` =
`ApplePMGR::_enableDevice` — the chain closes.

From there:

```
ApplePMGR::_enableDevice            0xfffffe00097f82f4   (trace + runAction)
  ApplePMGR::_enableDeviceGated     0xfffffe00097f848c   (workloop-gated)
    ApplePMGR::_updateDeviceStatus  0xfffffe00097db160   (parent walk, refcount)
    ApplePMGR::_syncDeviceStatusChange 0xfffffe00097f9088 (applies the list)
      ApplePMGR::_setPSAutoMinLevel 0xfffffe00097d84dc   (writes PS_MIN)
      ApplePMGR::_setPSLevel        0xfffffe00097d86d8   (writes PS_TARGET)
        AppleT6000PMGR::readReg32   0xfffffe0009baa560   vptr+0x1050
        AppleT6000PMGR::writeReg32  0xfffffe0009ba8518   vptr+0x1058
        AppleT6000PMGR::waitReg32   0xfffffe0009ba849c   vptr+0x1060
          ApplePMGR::waitReg32      0xfffffe00097e851c
```

Vtable slots were decoded with the chained-fixup rule from
[00-methodology.md](00-methodology.md) trap 5 (low 32 bits of the pointer are
the image-relative *file offset*).

**Correction to [10-power.md](10-power.md).** That document names
`ApplePMGR::waitForActualPS` (`0xfffffe00097e5120`) as the synchronous wait. It
is not on this path. A BL/B immediate scan of the whole `__TEXT_EXEC` segment
finds **zero** callers of `waitForActualPS` (the same scan returns 20+ hits for
`_setPSLevel` and `_setPSAutoMinLevel`, so it discriminates). The wait actually
used by `_enableDevice` is `waitReg32`, called through vtable `+0x1060` from
`_setPSLevel` at `0xfffffe00097d8a94` and `0xfffffe00097d8c20`.

---

## 3. The register, as macOS uses it

`_setPSLevel` computes the address as

```
group  = deviceData->psreg            _getDevicePsRegGroup  0xfffffe00097d81ac (ldrb [x0,#11] @ ...81d8)
index  = deviceData->psidx            _getDevicePsRegIndex  0xfffffe00097d8200 (ldrb [x0,#10] @ ...822c)
regmap = psRegGroups[group].word0     0xfffffe00097d87b0
base   = psRegGroups[group].word1     0xfffffe00097d87b0
offset = base + index*8               0xfffffe00097d87b4   (add w24, w8, w24, lsl #3)
```

which is exactly Linux's `ps-regs[psreg].offset + psidx*8`, confirming
`0x28e5803b0` for `VENC_SYS` (`psreg = 10` → window 2 `+0x300`, `psidx = 22`).

Fields, with the bit numbers Asahi uses and what macOS is seen doing with each:

| bits | Asahi name | macOS behaviour | cite |
|---|---|---|---|
| 31 | `RESET` | written only by `_setPSReset`, always together with bit 10 | `0xfffffe00097ecec8`, `0xfffffe00097ecef8` |
| 30 | — | **read**; if set, the "already at target" fast path is refused | `0xfffffe00097d89b8` |
| 28 | `AUTO_ENABLE` | cleared before any level change; re-set as a *separate final write* only when the new level is `0xf` | `0xfffffe00097d8a04`, `0xfffffe00097d8ca4` |
| 27:24 | `PS_AUTO` | never written by any code read here | — |
| 19:16 | `PS_MIN` | written by `_setPSAutoMinLevel` from ADT `ps_cfg16 & 0xf` | `0xfffffe00097d8614` |
| 11 | `PARENT_OFF` | polled for **0** in the reset path and in the T600x force-wake workaround; not on the normal path | `0xfffffe00097ecea0`, `0xfffffe0009ba8734` |
| 10 | `DEV_DISABLE` | **read** (same fast-path refusal as bit 30); set/cleared by `_setPSReset` | `0xfffffe00097d89b8`, `0xfffffe00097ece58` |
| 9:8 | `WAS_CLKGATED`/`WAS_PWRGATED` | written **1** whenever the new level ≤ 14; written **0** on the final `AUTO_ENABLE` write | `0xfffffe00097d8aac`, `0xfffffe00097d8c98` |
| 7:4 | `PS_ACTUAL` | polled, mask `0xf0`, expected `level << 4` | `0xfffffe00097d8c08`, `0xfffffe00097d8bd0` |
| 3:0 | `PS_TARGET` | written; also what `_getPSLevel` returns | `0xfffffe00097d8aa0`, `0xfffffe00097d8388` |

### Legal PS values

`_enableDeviceGated` turns the `(on, flag)` pair into the numeric level
(`0xfffffe00097f853c`–`0xfffffe00097f8548`):

```
level = on ? 15 : (flag & 1 ? 4 : 0)
```

Chaining that with `docs/10-power.md`'s `AVE_PMGR` mapping
(`PowerOff/ClockOff/ClockOn` → `DevicePowerState 5/6/7` → `enableDeviceClock`
value `0/2/1` → `_enableDevice(idx, value&1, (value>>1)&1, die)`) gives:

| `_E_AVE_PMGR_PS` | `enableDeviceClock` | `(on, flag)` | PS register value |
|---|---|---|---|
| `PowerOff` (0) | 0 | (0, 0) | **0** |
| `ClockOff` (1) | 2 | (0, 1) | **4** |
| `ClockOn` (2) | 1 | (1, 0) | **15** |

So **exactly three legal values, 0 / 4 / 15** — Asahi's `PS_PWRGATE` /
`PS_CLKGATE` / `PS_ACTIVE`. "On" is the single value 15, not a range: the code
compares against `0xf` for equality throughout
(`0xfffffe00097d8a00`, `0xfffffe00097f9250`, `0xfffffe00097f9850`), and
`level > 14` is the test that selects the "don't set `WAS_*`" branch
(`0xfffffe00097d8aa4`). Levels 1–3 and 5–14 are never produced.

Timeouts, both raw `mach_absolute_time` ticks (`waitReg32` adds the argument to
the counter directly, `0xfffffe00097e8590` — it does **not** scale from µs):

| use | value | at 24 MHz |
|---|---|---|
| PS state change | `0x2ee00` = 192000 | 8.000 ms |
| reset-path polls | `0x2d0` = 720 | 30 µs |

(the 24 MHz timebase is **inferred** — but both values landing on exact round
times is corroboration.)

---

## 4. The power-up sequence, as pseudocode

### 4.1 `ApplePMGR::_enableDevice(deviceID, on, flag, die)` — `0xfffffe00097f82f4`

```
0xfffffe00097f8318  assert die < this->numDies                 // [this+0x631c]
0xfffffe00097f8394  kernel_debug(0x2700c001, deviceID, on!=0, 0, die, 0)   // trace only
0xfffffe00097f83e8  commandGate->runAction(_enableDeviceGated, deviceID, on, flag, die)
0xfffffe00097f8484  kernel_debug(0x2700c002, ...)              // trace only
```

Everything real happens under the command gate, i.e. **serialised across all
PMGR devices on the machine**.

### 4.2 `ApplePMGR::_enableDeviceGated(deviceID, on, flag, die)` — `0xfffffe00097f848c`

```
0xfffffe00097f84f4  dev   = _deviceIDToDeviceData(deviceID)
0xfffffe00097f850c  assert die < numDies
0xfffffe00097f852c  if ((dev->flags & 0x30) == 0x30) return    // no_ps AND perf -> nothing to do
0xfffffe00097f8548  level = on ? 15 : (flag & 1 ? 4 : 0)

0xfffffe00097f8564  if (_checkNotifyPMP(deviceID))
0xfffffe00097f859c      this->_waitForPMPReadyAction(this[0x1bdc] & 1 ? die : 0) // vptr+0xac0
0xfffffe00097f85b0  _waitForClusterPowerUp(dev, die)
0xfffffe00097f85c0  if (_checkNotifyXNUForClusterPowerGating(deviceID, &out))
0xfffffe00097f85cc      _waitForXNUClusterPowerGatingThreadCall()

0xfffffe00097f85f4  old = desired[die*800 + deviceID]          // byte cache at this+0x3e7d4
0xfffffe00097f85fc  if (old == level) return                   // idempotent, no register touched
0xfffffe00097f8600  desired[die*800 + deviceID] = level

0xfffffe00097f861c  if (level != 0) _syncDevicePerfDomainRequirement({deviceID,die}, 1)
0xfffffe00097f8640  _updateDeviceStatus(deviceID, level, old, list, &n, die)   // §4.3
0xfffffe00097f8648  if (n == 0) goto done

  /* ---- pre-transition notifications ---- */
0xfffffe00097f8690  if (level <= 4) for each nub: _notifyDeviceStatusChange(false, list, n)
0xfffffe00097f8764  for each entry with dev->flags.notify_pmp: _sendPMPCommand(14|15, ...)
0xfffffe00097f87c4  for each entry: _notifyPMC(deviceID, on, ...)
0xfffffe00097f8808  if (level != 0) _notifyXNUForClusterPowerGating(cluster, true, die)

0xfffffe00097f8930  lck_spin_lock(this->psLock)                // interrupts off
0xfffffe00097f8940  _syncDeviceStatusChange(list, n)           // §4.4  <-- ALL register work
0xfffffe00097f8958  lck_spin_unlock

  /* ---- post-transition notifications ---- */
0xfffffe00097f899c  if (level == 0) _notifyXNUForClusterPowerGating(cluster, false, die)
0xfffffe00097f89b4  if (dev[13] & 2) _triggerPostPowerOffActions(dev, die)
0xfffffe00097f89f0  if (on)         for each nub: _notifyDeviceStatusChange(true, list, n)
0xfffffe00097f8ab0  for each entry with notify_pmp: _sendPMPCommand(14, ...)
0xfffffe00097f8b1c  for each entry: _notifyPMC(...)
0xfffffe00097f8bc4  IOStateReporter::setChannelState(...)      // telemetry
```

Note the shape: notifications bracket the register work on **both** sides, and
the whole register transaction runs under a spinlock with interrupts disabled.

### 4.3 `ApplePMGR::_updateDeviceStatus(deviceID, newLevel, oldLevel, list, &n, die)` — `0xfffffe00097db160`

This is the dependency walker. It builds the ordered list that
`_syncDeviceStatusChange` then applies.

```
0xfffffe00097db25c  parentActiveRefs[die][deviceID]++   /  --     // byte arrays at this+0xd320,
0xfffffe00097db29c  parentClkRefs   [die][deviceID]++   /  --     // 800 entries each, per die
0xfffffe00097db368  if (parentActiveRefs != 0) level = 15         // a child still needs ACTIVE
0xfffffe00097db374  if (parentClkRefs    != 0) level = max(level, 4)
0xfffffe00097db384  if (dev->flags & BIT3 /* critical */) level = 15
0xfffffe00097db3ac  if (_debugEnabled(deviceID, ...))    level = 15

0xfffffe00097db678  for (i = 0; i < 4; i++) {                     // 0xfffffe00097db6a4
0xfffffe00097db678      p = _getDeviceParentsID(dev, i)
0xfffffe00097db67c      if (!p) break
0xfffffe00097db69c      _updateDeviceStatus(p, level, ..., list, &n, die)   // RECURSE FIRST
                    }
0xfffffe00097db6e8  intended[deviceID] = level
0xfffffe00097db720  if (dev->flags & BIT4 /* no_ps */) skip register entry
0xfffffe00097db748  if (_getPSLevel(group, index, die) != level)
                        list[n++] = { u16 deviceID; u8 level; u8 pad; u32 die; }
```

Parents are appended **before** the device itself, so the list is already in
power-up order.

`_getDeviceParentsID(dev, i)` — `0xfffffe00097d90bc` — on this machine takes the
`id2 != 0` branch (`0xfffffe00097d90dc`), giving **two `u16` parents at
`dev+4` and `dev+6`** (`0xfffffe00097d90ec`, `0xfffffe00097d9130`), i.e. exactly
m1n1's `u16id.parents`. (The `u8` form at `dev+4..7`, up to four parents, is at
`0xfffffe00097d915c`, and a fully packed v2 form with parents at `+8`/`+10` is
at `0xfffffe00097d9100`; neither applies here.)

### 4.4 `ApplePMGR::_syncDeviceStatusChange(list, n)` — `0xfffffe00097f9088`

Direction is decided from the *first* entry
(`0xfffffe00097f9580`–`0xfffffe00097f9598`):

```
if (_getPSLevel(first) >= first->level)  -> DOWN: iterate list in REVERSE
else                                     -> UP:   iterate list FORWARD
```

**UP branch** (`0xfffffe00097f9638` onward), per entry:

```
0xfffffe00097f97ec  if (dev->flags & BIT4 /* no_ps */) skip register work entirely
0xfffffe00097f9800  _acquireDeviceLocks(dev, die)
0xfffffe00097f9808  if (!(dev->flags & BIT2))
0xfffffe00097f9820      _setPSAutoMinLevel(group, index, dev->ps_cfg16 & 0xf, die)   // PS_MIN
0xfffffe00097f9828  autoPmEn = !(dev->flags & BIT2)
0xfffffe00097f9848  _setPSLevel(group, index, deviceID, entry->level, autoPmEn, die)
0xfffffe00097f9850  if (entry->level == 15) {
0xfffffe00097f98b0      this->_configureDPEWithoutPMP(regmap, offset, 0, die)   // vptr+0x10e0
0xfffffe00097f991c      _initDeviceBridges(dev, die)
0xfffffe00097f994c      _setPerfState(perfDomain[15], dev->id1, die)
                    }
0xfffffe00097f9960  _releaseDeviceLocks(dev, die, token)
```

**DOWN branch** (`0xfffffe00097f9194` onward), per entry, reverse order:

```
0xfffffe00097f9248  cur = _getPSLevel(group, index, die)
0xfffffe00097f9264  if (cur == 15) _tearDownDeviceBridges(dev, die)
0xfffffe00097f92b4  if (cur == 15) this->_cleanupDPEWithoutPMP(regmap, offset, 0, die) // vptr+0x10e8
0xfffffe00097f93a0  _setPerfState(...)
0xfffffe00097f93c0  _setPSLevel(group, index, deviceID, entry->level, /*autoPmEn=*/0, die)
0xfffffe00097f93d4  _releaseDeviceLocks(dev, die)
```

Note the asymmetry: on the way **down** `autoPmEn` is hard-coded `0`
(`0xfffffe00097f93bc`) and `PS_MIN` is not touched; on the way **up** both come
from the ADT.

### 4.5 `ApplePMGR::_setPSLevel(group, index, deviceID, level, autoPmEn, die)` — `0xfffffe00097d86d8`

Signature confirmed by the log format at `0xfffffe000760e521`:
`"ApplePMGR: [Die %u] %s: %s_PS: %#x: MANUAL_PS=%d, AUTO_PM_EN=%s"`.

```
0xfffffe00097d8704  assert group < numPsRegGroups                  // [this+0x60c8]
0xfffffe00097d8730  assert index < _getMaxDevicePsRegSize()
0xfffffe00097d873c  assert (psRegGroups[group].validRegsMask >> index) & 1   // ADT ps-regs mask
0xfffffe00097d879c  assert die < numDies
0xfffffe00097d87b4  offset = psRegGroups[group].base + index*8

0xfffffe00097d89a8  v = readReg32(regmap, offset, die)
0xfffffe00097d89c4  if ((v & 0xf) == level && (v & 0x40000400) == 0) {   // bits 30 and 10 clear
0xfffffe00097d8ae0      if (((v >> 28) & 1) == autoPmEn) return          // nothing to do at all
                    }

0xfffffe00097d89f0  pd = _deviceIDToPowerDomainData(deviceID)      // ADT `power-domains`, by dev->pd

  /* ---- WRITE 1: tear down auto-PM before touching the level ---- */
0xfffffe00097d89f8  if (v & BIT28) {
0xfffffe00097d8a00      assert (v & 0xf) == 0xf                    // else panic (cold.3)
0xfffffe00097d8a0c      v &= 0xEFFFFCFF                            // clear AUTO_ENABLE + WAS_*
0xfffffe00097d8a3c      writeReg32(regmap, offset, v, die)
0xfffffe00097d8a94      waitReg32(regmap, offset, 0xf0, 0xf0, 0x2ee00, die)   // PS_ACTUAL == 15
                    }

  /* ---- WRITE 2: the level change ---- */
0xfffffe00097d8a9c  v &= 0xFFFFFCF0                                // clear PS_TARGET + WAS_*
0xfffffe00097d8aa0  v |= level
0xfffffe00097d8aa8  if (level <= 14) {
0xfffffe00097d8aac      v |= 0x300                                 // set WAS_CLKGATED|WAS_PWRGATED
0xfffffe00097d8b70      if (pd && pd->flags & 1) _setPwrGateSleepDepth(pd[7], pd[6], deviceID, 3, die)
0xfffffe00097d8b98      else if (pd && pd->flags & 4) _setPwrGateRetention(..., enable=0, ...)
0xfffffe00097d8ad0      else _setPwrGateRetentionV2(deviceID, false, die)
                    }
0xfffffe00097d8bc8  writeReg32(regmap, offset, v, die)
0xfffffe00097d8c20  waitReg32(regmap, offset, 0xf0, level << 4, 0x2ee00, die)  // PS_ACTUAL == level

  /* ---- WRITE 3: re-arm auto-PM, only when fully on ---- */
0xfffffe00097d8c24  if (level == 15) {
0xfffffe00097d8c4c      _setPwrGateRetentionV2(deviceID, true, die)   // or SleepDepth/Retention
0xfffffe00097d8c98      w = v & 0xFFFFFCFF                            // clear WAS_* (W1C)
0xfffffe00097d8ca0      if (autoPmEn) {
0xfffffe00097d8ca4          w |= BIT28
0xfffffe00097d8cd4          writeReg32(regmap, offset, w, die)
                        }
                    }
```

### 4.6 `ApplePMGR::_setPSAutoMinLevel(group, index, level, die)` — `0xfffffe00097d84dc`

```
0xfffffe00097d8538  assert (psRegGroups[group].mask >> index) & 1
0xfffffe00097d85d8  v = readReg32(regmap, offset, die)
0xfffffe00097d85dc  if (((v >> 16) & 0xf) == level) return
0xfffffe00097d8610  v &= 0xFFF0FCFF               // clear PS_MIN (19:16) and WAS_* (9:8)
0xfffffe00097d8614  v |= level << 16
0xfffffe00097d8670  writeReg32(regmap, offset, v, die)   // tail call, vptr+0x1058
```

### 4.7 `ApplePMGR::waitReg32(regmap, offset, mask, value, timeout, die)` — `0xfffffe00097e851c`

```
0xfffffe00097e8590  deadline = mach_absolute_time() + timeout      // RAW TICKS, not µs
0xfffffe00097e8594  do { v = readReg32(...) } while ((v & mask) != value && now <= deadline)
0xfffffe00097e861c  on expiry: one more read, then a diagnostic path that formats the
                    device name and register map (0xfffffe00097e8670 onward)
```

`AppleT6001PMGR::waitReg32` (`0xfffffe0009ba849c`) overrides it with two chip
quirks before tail-calling the base:

- `regmap == 2 && offset == 0xc00 && die != 0` → **return immediately**
  (`0xfffffe0009ba84b0`). `ps-regs[11]` is `{reg 2, +0xc00, mask 0x1}`.
- `regmap == 0 && offset == 0x1e8` (**this is `AFR`** — `psreg 4` → window 0
  `+0x100`, `psidx 29` → `+0xe8`) or `regmap == 0 && offset == 0x18014`:
  if a chip-revision word `[this+0x73908] >= 2` and two flags at
  `[this+0x73920]` / `[this+0x73924]` are non-zero, **skip the wait**
  (`0xfffffe0009ba84dc`–`0xfffffe0009ba84f4`).

So on some silicon revisions macOS does not trust `AFR`'s `PS_ACTUAL` to
settle. `AFR` is the shared parent of `VENC_SYS` and `AVD_SYS`.

---

## 5. `AppleT6000PMGR::writeReg32` — the force-wake workaround

`AppleT6001PMGR`'s copy is at `0xfffffe0009ba8518`. Before performing the write
it walks a 10-entry table `workaroundPSRegsForceWakeUP`
(`0xfffffe00076b67c8`, entry stride `0x10` at `0xfffffe0009ba8810`, count 10 at
`0xfffffe0009ba858c`):

```c
struct { u32 regmap; u32 offset; u32 forceWakeB; u32 forceWakeA; } [10];
```

```
0xfffffe0009ba85b8  if (entry.offset != offset) continue
0xfffffe0009ba85c4  if (entry.regmap != regmap) continue
0xfffffe0009ba8600  cur = readReg32(regmap, offset, die) & 0xf
0xfffffe0009ba8604  if ((newValue & 0xf) != 0) continue          // only on the way DOWN
0xfffffe0009ba8608  if (cur == 0)             continue
0xfffffe0009ba8654  if (A) writeReg32(regmap, A, read(A) | BIT29, die)
0xfffffe0009ba86d0         writeReg32(regmap, B, read(B) | BIT29, die)
0xfffffe0009ba86f8         writeReg32(regmap, offset, newValue, die)    // the real write
0xfffffe0009ba8750         waitReg32(regmap, offset, 0x800, 0, 0x2ee00, die)   // BIT11 == 0
0xfffffe0009ba87ac         waitReg32(regmap, offset, 0x00f0, 0, 0x2ee00, die)  // PS_ACTUAL == 0
0xfffffe0009ba87cc         writeReg32(regmap, B, valB & ~BIT29, die)
0xfffffe0009ba87ec  if (A) writeReg32(regmap, A, valA & ~BIT29, die)
```

Table contents (T6001 and T6000 are byte-identical):

| regmap | offset | forceWake B | forceWake A |
|---|---|---|---|
| 0 | `0x208` | `0x1f8` | — |
| 0 | `0x228` | `0x208` | `0x1f8` |
| 0 | `0x200` | `0x1f0` | — |
| 2 | `0x150` | `0x148` | — |
| 2 | `0x170` | `0x168` | — |
| 2 | `0x178` | `0x170` | `0x168` |
| 3 | `0x1a8` | `0x1a0` | — |
| 3 | `0x1b0` | `0x1a8` | `0x1a0` |
| 3 | `0x1c0` | `0x1b8` | — |
| 3 | `0x1c8` | `0x1c0` | `0x1b8` |

**No VENC register is in this table** — window 2 `+0x3b0` is absent, as are
window 2 `+0x8000..0x8020`. The test discriminates: the same decode does
produce ten valid rows, and the same `regmap == 2` value that VENC uses appears
three times with different offsets. So this is a genuine negative.

### `mediaPsRegs`

`AppleT6000PMGR::quiesceHW` (`0xfffffe0009b95db8` in the T6001 fileset) carries
a table `mediaPsRegs` (`0xfffffe00076b6670`) of `{regmap, offset}` pairs.
Decoded against the ADT it is exactly the media block set:

```
window 0: SCODEC, ANE_SYS, AVD_SYS, ANE_SYS_CPU, +6 unmapped (0xc000..0xc028)
window 2: JPG, MSR0, MSR0_ASE_CORE, VENC_SYS, GFX,
          6 unmapped (0x4018..0x4040), VENC_DMA/PIPE4/PIPE5/ME0/ME1
window 3: MSR1, MSR1_ASE_CORE, VENC1_SYS, ANE1_SYS, ANE1_SYS_CPU,
          6 unmapped, VENC1_DMA..ME1, PRORES
```

`quiesceHW` is the **suspend** path, not the power-on path, so this is not part
of bring-up. It is recorded here because it is the only place in the
kernelcache where VENC's PS register appears as a literal, and because it
confirms that VENC and AVD are handled as members of the same class.

---

## 6. `_setPSReset` — how macOS resets a device

`ApplePMGR::_setPSReset(group, index, deviceID, die, bool)` —
`0xfffffe00097eccb4`. Relevant because `reset_control_reset()` on `venc_sys`
also hung ([25-bringup-results.md](25-bringup-results.md) stage 7c).

```
0xfffffe00097ecdb4  v = readReg32(...)
0xfffffe00097ecdd4  if (v & BIT28)                                  // auto-PM armed
0xfffffe00097ecdf4      _setPSLevel(group, index, deviceID, 15, /*autoPmEn=*/0, die)
0xfffffe00097ece2c  w = v & 0xFFFFFCFF                              // clear WAS_*
0xfffffe00097ece74  writeReg32(w | 0x400)                           // DEV_DISABLE
0xfffffe00097ecea0  if (adt "reset-noaccess-poll")
0xfffffe00097eceb4      waitReg32(mask 0x800, value 0, 0x2d0, die)  // BIT11 == 0
0xfffffe00097ecec0  IODelay(1)
0xfffffe00097ecef8  writeReg32(w | 0x80000400)                      // RESET | DEV_DISABLE
0xfffffe00097ecf38  waitReg32(mask 0x80000000, value 1, 0x2d0, die) // see note
0xfffffe00097ecf40  IODelay(1)
0xfffffe00097ecf8c  writeReg32(w | 0x400)                           // RESET deasserted
0xfffffe00097ecfe0  waitReg32(mask 0x80000000, value 0, 0x2d0, die) // RESET == 0
0xfffffe00097ecfec  IODelay(1)
0xfffffe00097ed064  waitReg32(mask 0x800, value 0, 0x2d0, die)      // BIT11 == 0
0xfffffe00097ed068  if (original had BIT28)
0xfffffe00097ed088      _setPSLevel(group, index, deviceID, 15, /*autoPmEn=*/1, die)
```

*Note on `0xfffffe00097ecf38`:* `waitReg32` compares `(v & mask) == value`
(`0xfffffe00097e85c0`), so `mask = 0x80000000, value = 1` can never be
satisfied. In practice this is a fixed ~30 µs settle after asserting `RESET`.
Whether that is intentional or an Apple bug is **unknown**; either way the
observable behaviour is a delay.

The ADT property `reset-noaccess-poll = 1` names bit 11: Apple calls it the
**"no access"** bit, not "parent off". m1n1's `pmgr_reset_device` does the same
DEV_DISABLE/RESET dance but **without** the bit-11 poll and **without** first
disarming `AUTO_ENABLE`. Linux's `apple_pmgr_reset_assert`/`deassert` likewise
does not disarm `AUTO_ENABLE`.

---

## 7. Which ADT `devices` fields drive which behaviour

`ApplePMGR::_deviceIDToDeviceData(id)` — `0xfffffe00097d91cc`:

```
0xfffffe00097d91e0  assert 0 < id < 0x320                      // 800 device ids
0xfffffe00097d91f0  idx = u16 map[this+0x9d40 + id*2]          // must not be 0xffff
0xfffffe00097d9210  return *(void**)(this+0x90a8) + idx*48     // <-- stride 48
```

Stride 48 = `sizeof(struct pmgr_device)` in m1n1
(`m1n1-src/src/pmgr.c:23`). Every field macOS reads lands on an m1n1/`adt.py`
field, so **`DeviceData` is the raw ADT `devices` entry** — no reformatting:

| ADT field (m1n1 `PMGRDevices`) | offset | what macOS does with it | cite |
|---|---|---|---|
| `flags.on` (bit 0) | +0 | **no consumer found** on the `_enableDevice` path (the `& 1` test at `0xfffffe00097f8570` is on `this[0x1bdc]`, not on `DeviceData`) | — |
| `flags.notify_pmp` (bit 1) | +0 | gates `_sendPMPCommand(14/15)` per entry; with `b7` also drives `_checkNotifyPMP` | `0xfffffe00097f8730`, `0xfffffe00097f8038` |
| `flags.b2` (bit 2) | +0 | **suppresses `PS_MIN` programming and forces `AUTO_ENABLE = 0`** | `0xfffffe00097f9808`, `0xfffffe00097f9824` |
| `flags.critical` (bit 3) | +0 | pins the device at level 15 in `_updateDeviceStatus` | `0xfffffe00097db384` |
| `flags.no_ps` (bit 4) | +0 | skip all register work (virtual device) | `0xfffffe00097f9150`, `0xfffffe00097db720` |
| `flags.perf` (bit 5) | +0 | with `no_ps`, early-return from `_enableDeviceGated` | `0xfffffe00097f852c` |
| `flags.b7` (bit 7) | +0 | with `notify_pmp`, makes `_checkNotifyPMP` true | `0xfffffe00097f8038` |
| `unk1_1` (+2) hi byte of `unk2_0` at +13 | +13 | bit 1 gates `_triggerPostPowerOffActions` | `0xfffffe00097f89a4` |
| `id1` | +3 | device id fallback when `id2 == 0` | `0xfffffe00097f9128` |
| `parents[0..1]` (u16) | +4, +6 | recursive dependency walk, up to 4 slots tried | `0xfffffe00097d90ec` |
| `psidx` | +10 | PS register index (`offset = base + psidx*8`) | `0xfffffe00097d822c` |
| `psreg` | +11 | PS register group (index into ADT `ps-regs`) | `0xfffffe00097d81d8` |
| `ps_cfg16` | +15 | low nibble = **`PS_MIN`**; bit 6 = "has cluster power gating" | `0xfffffe00097f9810`, `0xfffffe00097f95c8` |
| `offset` / `group` | +16..19 | the v2 addressing form (`(w>>3)&0x1FFFFF`, `w>>24`) — **not used on t6001** | `0xfffffe00097d8238` |
| `unk2_3` lo byte | +24 | **bridge count** for `_initDeviceBridges` | `0xfffffe00097fa9b0` |
| `unk2_3` hi byte | +25 | cluster id for `_notifyXNUForClusterPowerGating` | `0xfffffe00097f93e0` |
| `id2` (u16) | +26 | primary device id | `0xfffffe00097f9120` |
| `unk3` lo byte | +28 | **bridge id / bridge-index selector** | `0xfffffe00097fa994` |
| `name[16]` | +32 | `%s` in every log line | `0xfffffe00097f88a0` |
| `pd` | +14 | index into ADT `power-domains` → retention/sleep-depth variant | `0xfffffe00097d89f0` (**inferred**: the lookup is `_deviceIDToPowerDomainData`, the field it reads was not traced) |

t6001 takes the **v1** branch of `_getDevicePsRegGroup`/`Index`
(`_getEdtPropertyVer() == 1`, the word at `[this+0x1c08]`). **Inferred but
strongly corroborated**: only the v1 branch yields `psreg 10`/`psidx 22` →
`0x28e5803b0`, the address Linux already proves correct; the v2 branch would
read `offset`/`group`, which are both zero for every VENC and AVD device on
`j314c`.

Also consumed, outside `devices`:

- **`ps-regs[g].mask`** — asserted before every read *and* every write
  (`0xfffffe00097d873c`, `0xfffffe00097d8538`, `0xfffffe00097d82e8`). Apple's
  assertion text at `0xfffffe000760e4bc` gives the field its real names:
  `"(_psRegGroups[(group)].validRegsMask & (0x1 << (index))) ||
  (_getEdtPropertyVer() == kEdtPropertyV2)"`. For `ps-regs[10]` the mask is
  `0x01FF843F`; bit 22 (`VENC_SYS`) is set, so the assertion passes. A clear
  mask bit is a **panic**, not a skip.

  That string also names `[this+0x1c08]`: it is `_getEdtPropertyVer()`, and the
  value `2` is `kEdtPropertyV2`. So the v1/v2 selector discussed above is the
  **ADT (EDT) property version**, and t6001 is v1.
- **`power-domains`** — retention/sleep-depth variant selection. No VENC or AVD
  device is in the list on `j314c` (`pd = 0` for both).
- **`pwrgate-regs`** — the second register file touched by
  `_setPwrGateRetention*`. On `j314c` every window-2 and window-3 entry has
  `mask = 0`; only `{reg 0, +0x9c0e0, mask 0xffffffff}` covers anything. So
  **AVD (window 0) plausibly has a retention register and VENC (window 2) does
  not** — *inferred*, because the device→pwrgate-index mapping inside
  `_setPwrGateRetentionV2` (`0xfffffe00097ec270`) was not traced. Note this
  asymmetry runs the *wrong way* to explain the hang: it gives AVD extra
  handling that VENC does not need.
- **`device-bridges` + `bridge-settings-version` (= 1) + `bridge-reg-index`** —
  `_initDeviceBridges` (`0xfffffe00097fa954`) reads `dev[24]` as a count and
  `dev[28]` as the bridge selector, then calls `_configureBridge`
  (`0xfffffe00097faa00`) and `_enableBridgeCounters` (`0xfffffe00097faa40`).
  `VENC_SYS` has count 1, selector `0x22`; `AVD_SYS` has count 1, selector
  `0x10`. **This corrects a claim in [25-bringup-results.md](25-bringup-results.md)**:
  "no entry in the pmgr `device-bridges` list contains any VENC device" was
  searching the wrong side of the relation — `device-bridges[i].subdevs` holds
  *bridge sub-register ids*, not PMGR device ids. The device→bridge link lives
  in the `devices` entry itself, and VENC has one.

### The VENC / AVD parent chains

```
VENC_SYS (294)  -> AVEMSR-V (519, no_ps) -> AFR (39, ps_cfg16 = 0x04)
AVD_SYS  (56)   ->                          AFR (39)
VENC1_SYS (364) -> AFNC5_LW0 (345) -> AFNC5_LS -> AFNC5_IOA -> AFI (37)
                -> AVEMSR-V (519)  -> AFR (39)
```

Asahi's DT gives `venc_sys` and `avd_sys` the *same* single parent phandle
(`0xa8`, read from `/proc/device-tree/soc/power-management@28e580000/power-controller@3b0`
and `…@28e080000/power-controller@270`). Collapsing `AVEMSR-V` is correct —
it is `no_ps`, so macOS writes no register for it either; it exists only for
refcounting and for the `avemsr-tvm` voltage-margin domain (ADT property
`avemsr-tvm = 1`, string table at `0x603a13`, consumed by
`AppleT6000PMGR::enableTVM`, `0xfffffe0009b5cce8`).

`ave-tvm` and `avemsr-tvm` are the only two AVE-specific knobs anywhere in the
PMGR node. They are per-voltage-rail margin settings applied at driver init,
not per power-on, and Asahi never programs them. **Unknown** whether they
matter for register accessibility; nothing read here suggests they gate the
fabric.

---

## 8. The deliverable list: what a "write PS, poll, done" driver does *not* do

Ordered by how likely it is to matter, most first.

1. **Disarm `AUTO_ENABLE` before changing `PS_TARGET`, as its own write, and
   wait for `PS_ACTUAL == 0xf`.** `0xfffffe00097d89f8`–`0xfffffe00097d8a94`.
   macOS treats "`AUTO_ENABLE` set with `PS_TARGET != 0xf`" as an impossible
   state and panics with `"Unexpected PS state MANUAL_PS=%x for %s"`
   (`0xfffffe00097d8a00` → `0xfffffe000760e59e`). Asahi sets `AUTO_ENABLE`
   after power-on and then writes `PS_TARGET = 0` on the next power-off with
   bit 28 still set — the exact state macOS asserts cannot happen.
2. **Re-arm `AUTO_ENABLE` as a separate third write, only after
   `PS_ACTUAL == 0xf` has been observed**, and only when the target is 15
   (`0xfffffe00097d8c98`–`0xfffffe00097d8cd4`). Asahi's set-then-poll leaves a
   window where auto-PM is armed while `PS_ACTUAL` is still moving.
3. **Program `PS_MIN` (19:16) from ADT `ps_cfg16` before the level change**
   (`0xfffffe00097f9820`). Zero for VENC and AVD; **`0x04` for `AFR`**, their
   common parent. Asahi leaves `AFR`'s `PS_MIN` at 0, i.e. permits auto-PM to
   take the shared media fabric root all the way to power-gated.
4. **Write `WAS_CLKGATED | WAS_PWRGATED` as 1 on every downward transition**
   (`0xfffffe00097d8aac`) and as 0 on the final `AUTO_ENABLE` write
   (`0xfffffe00097d8c98`). Asahi always writes 0. If these are W1C status bits,
   macOS clears them and Asahi never does; if they are controls, macOS sets them
   on the way down and Asahi never does. Either way the states differ.
5. **Refuse the "already at target" shortcut when bit 30 or bit 10
   (`DEV_DISABLE`) is set** (`0xfffffe00097d89b8`–`0xfffffe00097d89c4`) — a
   read-modify-write is forced instead.
6. **Walk parents recursively and refcount them in software, before touching
   any register** (`0xfffffe00097db678`), forcing a parent to 15 if any child
   needs 15 and to ≥4 if any child needs 4 (`0xfffffe00097db368`,
   `0xfffffe00097db374`), and pinning `critical` devices at 15
   (`0xfffffe00097db384`). Linux gets the same *effect* from genpd, but only
   for the parents the DT models.
7. **Apply the whole change set as one ordered list**: parents first on the way
   up, children first on the way down, decided from the first entry's current
   `PS_TARGET` (`0xfffffe00097f9598`), with the *entire* list applied under one
   spinlock with interrupts disabled (`0xfffffe00097f8930`) inside a
   machine-wide command gate (`0xfffffe00097f83e8`).
8. **Notify before and after, four separate mechanisms**: PMGR nubs
   (`_notifyDeviceStatusChange`, `0xfffffe00097f8690` / `0xfffffe00097f89f0`),
   the PMP dashboard (`_sendPMPCommand` 14/15, `0xfffffe00097f8764` /
   `0xfffffe00097f8ab0`), the PMC (`_notifyPMC`, `0xfffffe00097f87c4`), and XNU
   cluster power gating (`0xfffffe00097f8808`, `0xfffffe00097f899c`). Plus
   `_waitForPMPReadyAction` (`0xfffffe00097f859c`) and
   `_waitForClusterPowerUp` (`0xfffffe00097f85b0`) *before* anything is written.
9. **Configure the DPE after power-on** and clean it up before power-off —
   `_configureDPEWithoutPMP` / `_cleanupDPEWithoutPMP` (vtable `+0x10e0` /
   `+0x10e8`, called at `0xfffffe00097f98b0` / `0xfffffe00097f92b4`) with
   `(regmap, psRegOffset, 0, die)`. **A second register block written at
   power-on.**
10. **Configure the device's fabric bridge after power-on** and tear it down
    before power-off (`_initDeviceBridges` `0xfffffe00097f991c`,
    `_tearDownDeviceBridges` `0xfffffe00097f9264`). VENC_SYS has bridge
    selector `0x22`, AVD_SYS `0x10`. **A third register block.**
11. **Program retention / sleep depth around every transition**
    (`_setPwrGateRetentionV2` etc., `0xfffffe00097d8ad0`, `0xfffffe00097d8c4c`)
    — the ADT `pwrgate-regs` file. **A fourth register block.**
12. **Assert the `ps-regs` mask bit** before every access
    (`0xfffffe00097d873c`) — a panic, not a skip.
13. **Use an 8 ms timeout** (`0x2ee00` raw ticks, `0xfffffe00097d8a80`), and on
    some silicon revisions **skip the `PS_ACTUAL` wait for `AFR` entirely**
    (`0xfffffe0009ba84dc`).
14. **A force-wake dance around power-down of ten specific PS registers**
    (§5) — sets bit 29 on one or two neighbouring registers, then polls bit 11
    and `PS_ACTUAL`, then clears bit 29. **VENC is not in that table.**
15. **On reset**, disarm `AUTO_ENABLE` first, poll bit 11 ("no access") after
    asserting `DEV_DISABLE`, and re-arm `AUTO_ENABLE` afterwards (§6). Neither
    m1n1 nor Linux does the disarm or the poll.

---

## 9. What this does and does not explain, and proposals

### Does not explain

Items 6–12 above are all done for `AVD_SYS` too, and AVD works under Asahi with
none of them. Item 14 excludes VENC by name. Item 3's ADT value is 0 for both
VENC and AVD. `notify_pmp` is set on both. So **the PMGR PS protocol is not
where VENC and AVD differ**, and the "macOS's PS handling differs" hypothesis
from [25-bringup-results.md](25-bringup-results.md) §4 is, as far as the PS
register itself goes, **eliminated** — with a positive control rather than by
failing to find something.

The one asymmetry found runs backwards: AVD is in a PMGR window that has a
non-zero `pwrgate-regs` mask and VENC is in one that does not.

### Might still explain something

- **`AFR`.** It is the shared parent, it is the one device in either chain with
  a non-zero `ps_cfg16` (`PS_MIN = 4`), and `AppleT6001PMGR::waitReg32` has a
  revision-gated special case for exactly its PS register
  (`regmap 0, offset 0x1e8`, `0xfffffe0009ba84c0`). Under Asahi its `PS_MIN`
  is 0. If auto-PM is allowed to take `AFR` below clock-gated while a VENC
  access is in flight, a fabric timeout on the VENC agent is a plausible
  outcome — and AVD would be exposed to the same risk, so this is a *weak*
  hypothesis, but it is the only one with a VENC-side-relevant knob that Asahi
  provably leaves at the wrong value.
- **Item 1** (writing `PS_TARGET` with `AUTO_ENABLE` armed) is a genuine
  contract violation on Asahi's side, on every power cycle, for every domain.
  It is cheap to eliminate.

### Proposals — for the human operator only

These are **not** to be run by an agent. See `AGENTS.md`.

1. **Read-only first.** Dump all six VENC PS registers plus `afr`
   (`0x28e0801e8`) and `avd_sys` (`0x28e080270`) immediately after
   `pm_runtime_resume_and_get` succeeds, via `devmem`/m1n1 — *not* from the
   driver. Confirm `PS_ACTUAL == 0xf` and `PS_TARGET == 0xf` on all five VENC
   domains simultaneously. Stage 6 in
   [25-bringup-results.md](25-bringup-results.md) proved genpd is happy; it did
   not prove `PS_ACTUAL` is 15.
2. **Adopt macOS's write order.** Patch `apple_pmgr_ps_set` locally to clear
   `APPLE_PMGR_AUTO_ENABLE` and wait for `PS_ACTUAL == PS_ACTIVE` before
   writing `PS_TARGET`, and to set `AUTO_ENABLE` only after `PS_ACTUAL` has
   reached the new target. This is item 1 and costs one extra register write.
3. **Set `AFR`'s `PS_MIN` to 4**, matching `ps_cfg16`, before any AVE access.
   Single 32-bit RMW on `0x28e0801e8`, bits 19:16.
4. **Only then** retry a single 32-bit read of bank 1 `+0x400048`.

Steps 1–3 are all things macOS demonstrably does and Asahi demonstrably does
not; step 1 costs nothing and would settle whether the domains are actually up.

---

## 10. Not determined

- The meaning of PS register **bit 30**. It is read and used as a veto on the
  fast path (`0xfffffe00097d89b8`) and never written.
- Whether bits 9:8 are write-1-to-clear status or control. Both readings are
  consistent with the code; the live register dump is consistent with W1C.
- `_setPwrGateRetentionV2`'s device → `pwrgate-regs` index mapping
  (`0xfffffe00097ec270`), hence whether VENC really has no retention register.
- What `_configureDPEWithoutPMP` writes. Only its call site and arguments were
  read; the body (`0xfffffe0009baaba8`, a thunk) was not followed.
- What `_configureBridge` (`0xfffffe00097f1a3c`) writes beyond the counter
  event-select registers it visibly touches.
- Why `_setPSReset` polls for `(v & 0x80000000) == 1` (§6 note).
- Whether `ave-tvm` / `avemsr-tvm` have any bearing on register accessibility.
- The value of `[this+0x1c08]` on t6001 (the v1/v2 device-format selector) was
  inferred from which branch produces the known-correct address, not read from
  the initialisation code.
