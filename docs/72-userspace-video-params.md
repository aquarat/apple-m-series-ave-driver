# What macOS's user-space encoder puts in `VIDEO_PARAMS` and `AVEFWRCSettings`

[62](62-kext-field-map.md) §1.2 showed that the kext passes the scalar half of
`AVE_VIDEO_PARAMS` through from user space byte for byte. That left "what does
macOS actually send" open for every scalar field. This document answers it from
the user-space encoder, which we had never had until now.

Static analysis only. Nothing here was run on hardware.

Conventions as [62](62-kext-field-map.md) / [70](70-intraest.md):

- **VP** = offset in `AVE_VIDEO_PARAMS`; **wire** = VP + `0x60` in `AVC_INIT`.
- **RC** = offset in `AVEFWRCSettings`, which sits on the wire at `0xFF30`
  (VP + `0xFED0`). So wire = `0xFF30` + RC.
- User-space VAs are VAs in the arm64e `AppleVideoEncoder` bundle executable
  (`__TEXT` vmaddr 0, so VA = file offset). Firmware VAs are 13.5 image VAs, as
  elsewhere.
- **S** is the encoder's per-session storage (`encoderPrivateStorage`, the
  `CMBaseObject` derived storage). Its layout against the kernel structure is
  derived in §1.3.
- Labels follow [00](00-methodology.md). **[C]** means read from an instruction,
  with the VA cited. **[I]** means inferred, with the chain stated. **[U]** means
  unknown.
- Some firmware rows were produced by a delegated analysis and then spot-checked
  here. They are marked *(d)*. The spot-checks are listed in §7.

---

## 0. Verdicts

| # | question | answer | label |
|---:|---|---|---|
| 1 | Which binary builds `VIDEO_PARAMS` on an M1 Mac | `/System/Library/Video/Plug-Ins/AppleVideoEncoder.bundle`. It is a standalone arm64e bundle on the system volume, not a dyld-shared-cache dylib. Its `version.plist` gives `ProjectName AppleAVE2`, and it carries `LC_SOURCE_VERSION 670.13.2`. `AppleAVEEncoder.bundle` is the T2/Intel `AppleAVEBridge` plug-in (x86_64/i386); it was used as the negative control. | [C] |
| 2 | **VP+0x08, wire 0x68, for a plain VideoToolbox H.264 session** | **0, the same value we send.** `AVE_SetEncoderDefault` seeds it with `0xF` (`0x29a74`/`0x29a78`). For any session with `usageMode == 0`, `AVE_EnableH264FWRCSettings` then **clears it** (`0x3aa7c`; `0x3aa44` when RealTime is true). That covers Baseline and Main, with or without the RealTime key. | [C] |
| 3 | What the bits of VP+0x08 are | The firmware dumper names the field `EnableSelStatsFlags`, and user space treats 0 as `STATISTICS_DISABLED` (`0x2bbdc`). It is a **statistics-output selector**. Bits 0–3 are the `0xF` default, which survives only when `usageMode ≠ 0`: private usage modes and multipass (`usageMode 0x2710`). The private `EnableStatsCollect` key writes the word verbatim (`0x1c588`), but it is applied before `AVE_EnableH264FWRCSettings`, so it too survives only with `usageMode ≠ 0` [I, from the call order]. Bit 27 is set only for HEVC or for a `defaults write` debug key. Bit 4 is never set on its own. IntraEst never tests the mode-word bits these feed *(d)*. | [C] / *(d)* |
| 4 | docs/70 rank 3 ("IntraEst DMem mode word is 0 because we leave VP+8 at 0") | **Closed.** macOS sends the same 0 for this field. The one mode-word bit that differs in macOS's rate-controlled default is bit 0, which comes from RC+0x42 `bEnableLamdaMod`, not from VP+8. macOS clears that bit too in its own fixed-QP path (`0x3b8f0`). docs/70 §3's bit-source map is also off by one byte; §7 gives the correction. | [C] |
| 5 | wire 0xFCE4 (VP+0xFC84, `enable_IPCM_in_IntraSlice`) and wire 0xFCE3 (VP+0xFC83, `disable_intra_mode`) | **0 and 0 — the same as ours.** 0xFCE4 is written at `0x29a94`. 0xFCE3 is covered by the 8-byte zero store over VP+0xFC7C..0xFC83 at `0x29a98`. No later writer exists on the default path. | [C] |
| 6 | The [62](62-kext-field-map.md) §1.4 scalars VP 0x0C–0x20 | All 0, **except**: <br>– VP+0x10 `MaxMvsPer2Mb` is 16/32/64, set from the level (`0x37508`/`0x37534`). <br>– VP+0x14 `MaxSubMbRectSize` is 576 for Baseline below level 3.1 (`0x37518`–`0x37550`). <br>– VP+0x18 `BFrames` and VP+0x1D `bEnableAdaptB` are 1 by default. Both are cleared for Baseline (`0x3b75c`/`0x3b760`). <br>None of these reaches intra estimation. See §4. | [C] |
| 7 | **What macOS sends non-zero that we leave zero, in the mode-decision / λ path** | **The ME/MD λ scale words and the per-QP λ table in the RC block.** <br>– RC+0x68..0x78 = `0x400` ×5 (wire `0xFF98..0xFFA8`); RC+0x7C = 0 (`0x29ba4`, `0x29bb0`). <br>– RC+0x90..0x15F: 52-entry table `max(1, round(2^((QP−12)/6)))`, i.e. 1,…,1,2,2,2,2,3,…,81,91 (`0x29be0`–`0x29c7c`). <br>We send zeros in both. The firmware multiplies the scales into the ME and ModeDec λ registers (`0x55ecc`, `0x5633c`, `0x56354`). The table becomes ModeDec's per-MB intra-cost bias *(d)*. With zeros, mode decision runs with **λ = 0 and no intra bias**. <br>macOS also keeps these non-zero in its own fixed-QP path. It is the most substantive divergence left in this command. | [C] for user-space values and fw λ multiply; *(d)* for the table consumer |
| 8 | Other non-zero macOS defaults | `skip_mode` = 3 (feeds the ReconL/ReconC `SKIPMODE` registers); `pix_pck` = 1; `input_bitdepth` = 8; `sSliceMap` slice 0 = {0, height}; VP+0xFD44 = 1; VP+0xFE54 = 16; the multipass block = −1 ("use default"); plus the rate-control fields. Full table in §5. | [C] |

**In short.** VP+0x08 and the 0xFCE3/0xFCE4 bytes are not where we differ from
macOS. The difference is in `AVEFWRCSettings`:

- the five λ scale words;
- the per-QP λ table.

Both feed ModeDecision directly. §6 has a one-variable proposal.

---

## 1. The binary and how it was obtained

### 1.1 Where the encoder is

The `ipsw` info for the 13.5 IPSW lists three DMGs: FileSystem `096-63007-081`,
SystemOS cryptex `096-62994-081` (the dyld shared cache), and AppOS. The
FileSystem DMG's mtree (`Firmware/096-63007-081.dmg.mtree`) lists
`/System/Library/Video/Plug-Ins/` with two AVE candidates. Each has its
executable as a regular file with an inode, which a shared-cache dylib would not
have.

| bundle | executable | what it is | evidence |
|---|---|---|---|
| **`AppleVideoEncoder.bundle`** | arm64e `MH_BUNDLE`, 1 271 712 B | **the AppleAVE2 encoder**. It registers `com.apple.videotoolbox.videoencoder.ave.avc` ("Apple Video Encoder - AVC family") and `.ave.hevc`, for arm64e | `version.plist`: `ProjectName AppleAVE2`, `SourceVersion 670013002000000`; `LC_SOURCE_VERSION 670.13.2.0.0`, `LC_BUILD_VERSION macOS 13.5`; exports `_AVE_H264CreateInstance`, `_AppleAVEVA_DriverInit`, `_AppleAVEVA_DriverPreInit` [C] |
| `AppleAVEEncoder.bundle` | fat x86_64 + arm64e | the T2/Intel bridge encoder. **Negative control** | `ProjectName AppleAVEBridge`; `CMExecutableArchitectures` x86_64/i386; `IORegistryRequiredKey IOAVEHEVCEncode` [C] |

The extraction is checked end to end. All 304 SHA-256 page hashes in
`AppleVideoEncoder`'s own code directory match the bytes we pulled. The same
holds for both slices of the control. So the remote APFS read returned Apple's
bytes, not a plausible-looking corruption. Provenance and hashes are in
`data/blobs/macos-13.5/PROVENANCE-userspace.txt`.

### 1.2 Who calls the user client

The session command blobs go to the kext through `IOConnectCallStructMethod` in
the `AppleAVEVA_*` driver layer:

- selector 0, `0x4d0` bytes at `0x9cb00`/`0x9cb04`;
- selector 3 `IO_Prepare`, `0x39C70` bytes at `0x9fab8`–`0x9fac8`;
- selector 4 `IO_Start`, `0x39C70` bytes at `0xa485c`–`0xa4868`;
- selector 5, `0x18` bytes at `0xa1e8c`–`0xa1e94`.

The selector numbers and sizes match the 13.5 table in
[46](46-abi-13.5-commands-session.md) §6. [C]

`AppleAVEVA_DriverPreInit` and `AppleAVEVA_DriverInit` both build
`AVE_SessionSettings_UserKernel_Data` by `memcpy` from a descriptor of pointers
(`0x9eaf8`–`0x9ec20`, `0xa2fac`–`0xa303c`). The copy lands at driver-ctx + `0x30`:

| block | size | pInfo offset |
|---|---:|---:|
| RC | `0x680` | `+0` |
| RC-driver | `0x128` | `+0x680` |
| VP | `0xFED0` | `+0x7B0` |
| DRV | `0x3A0` | `+0x10680` |
| SPS | `0x6AC` | `+0x10A20` |
| PPS | `0x184` | `+0x110CC` |
| slice block | `0x984` | `+0x11250` |

These are exactly the offsets `AVE_Client_Config` reads on the kext side
([62](62-kext-field-map.md) §1.2). [C]

No store into the copied VP/RC region was found between those copies and the
`IO_Prepare`/`IO_Start` calls. That rests on a scan of stores off the ctx
registers in `DriverPreInit`/`DriverInit`, and is [I].

### 1.3 Where the descriptor points: the `S` layout

The H.264 encode path calls `AppleAVEVA_DriverInit` at `0x20c50`. Its descriptor
at `sp+0x2d8` is filled at `0x20bbc`–`0x20c10` [C]:

| slot | points at | block |
|---:|---|---|
| [1] | S+0x860 | VP |
| [3] | S+0xB0 | RC |
| [4] | S+0x730 | the 0x128 RC-driver block |
| [5] | S+0x10AD0 | SPS |
| [6] | S+0x1117C | PPS |
| [7] | S+0x11300 | the 0x984 slice block |

So:

```
RC  r  <->  S + 0xB0  + r        VP  v  <->  S + 0x860 + v        DRV d <-> S + 0x10730 + d
```

Two cross-checks confirm the mapping:

- User space's own dumper (`0x2daa0`) prints `VideoParams.ui32Width` from S+0x860, `EnableSelStatsFlags` from S+0x868 and `AVEFWRCSettings.usageMode` from S+0x130. [C]
- `AVE_H264CreateInstance` zero-fills both regions: `bzero(S+0xB0, 0x7AC)` (`0x5f48`–`0x5f60`) and `bzero(S+0x860, 0x11424)` (`0x5f44`, `0x5f64`–`0x5f70`). It then writes VP+0xFEC4/0xFEC8 (`iNumViews`, `iLayerNum`) = 1/1 (`0x5f74`/`0x5f78`). [C]

So every field below that user space never writes is **0 by construction**, not
by assumption. Field names come from the firmware's own dumper,
`CHEVCController::DebugInit` (fw `0x39d10`, `x25` = VP; `x24` = VP+0xFC78).
User space prints the same names (`VideoParams.bDisableIntra4x4`,
`VideoParams.mode_8x8_transform`, `AVEFWRCSettings.ui32InitialQpI`, …) at
`0x34118`–`0x3471c` and `0x31858`–`0x31e64`. [C]

### 1.4 The order things happen in

For one H.264 session, reading the call sites in the listing [C] (whether the
first-frame block runs only once is [I]):

1. `AVE_H264StartSession` (`sub_1f254`). Stores width/height (`0x1f474`), then
   calls **`AVE_SetEncoderDefault`** (`sub_2985c`, called at `0x1f4c4`).
2. The client's `VTSessionSetProperty` calls go to `AVE_SetProperty_internal`
   (`sub_ed3c`). "No special keys" means none of those branches runs, apart from
   ProfileLevel.
3. First `AVE_H264EncodeFrame` (`sub_1ff38`):
   - `AVE_ManageSessionSettings` (`sub_34b1c`, `0x2042c`). This runs, in order:
     - `AVE_WeMayHaveDefaultWritesToParse` (`defaults write` overrides; every one
       is skipped at its −1 sentinel);
     - `AVE_PrepareRealTimeParameters`;
     - `AVE_SetNewEncoderDefaultBasedOnProfileAndLevel`, which calls
       `AVE_H264NewDefaultsBasedOnProfileUsageDefault` (`sub_3ae80`), which calls
       **`AVE_EnableH264FWRCSettings`** (`sub_3a908`, `0x3aee8`, unconditional);
     - the level restriction and `MaxMvsPer2Mb` logic.
   - `AVE_ValidateEncoderParameters` (`sub_2a7b0`, `0x206f4`/`0x207ec`).
   - `AVE_PrepareSequenceHeader`.
   - `AppleAVEVA_DriverInit` (`0x20c50`), which carries the copy in §1.2.

---

## 2. VP+0x08 (wire 0x68), bit by bit

### 2.1 What user space writes

| writer | VA | value | when |
|---|---|---|---|
| `AVE_SetEncoderDefault` | `0x29a74`/`0x29a78` | **`0xF`** | every session, at StartSession |
| `AVE_SetProperty_internal`, key `EnableStatsCollect` (private) | `0x1c588` | the CFNumber, verbatim | only if a client sets that key |
| `AVE_EnableH264FWRCSettings`, `usageMode == 0`, RealTime ≠ 1, DRV+0x88 = 0 | `0x3aa7c` | **0** | **the plain-session path** (§2.2) |
| same, `usageMode == 0`, RealTime = 1 | `0x3aa44` | 0 | a `kVTCompressionPropertyKey_RealTime = true` session |
| `AVE_H264NewDefaults…`, RCFlag 3 → FIXQP, or RCModeDriver 3 | `0x3b8dc` | 0 | macOS's fixed-QP path (§5.1) |
| same, lossless (`[x22+0x61c]`) | `0x3b928` | 0 | lossless |
| `AVE_ValidateEncoderParameters`, `bIsLossless` | `0x2bbdc` | 0 = `STATISTICS_DISABLED` (log string at `0x2bb90`) | lossless |
| (not a writer) `bEnableCrcQPMod` check | `0x2c4dc`–`0x2c568`, entered only if RC+0x46 ≠ 0. The log says "Forcing EnableSelStatsFlags to disabled" (`0x2c528`), but the store that follows, `0x2c568` `str wzr,[x19,#0x100]`, clears `eStaticAreasLowQpSel` (RC+0x50), not VP+8. The log text and the code disagree; the code is what is sent | — | CRC-QP-mod sessions (RC+0x46 ≠ 0) |
| `AVE_ValidateEncoderParameters`, `[x24+0x36b]` or `codecID` (`[x24+0x2d0]`) ≠ 0 | `0x2c85c`–`0x2c868` | **`0x08000000`** (bit 27 only) | HEVC, or the `defaults write` flag below |
| `AVE_ManageSessionSettings`, `defaults write` key at DW+0x328 ≠ −1 | `0x35074`–`0x35090` | `0x08000000` | developer override only |
| `sub_3bab8` (called from `sub_25984`, next to a second `SetEncoderDefault`) | `0x3bb64` | 0 | multipass re-initialisation [I] |

All rows are [C] for the store and the condition. "Default path" means the chain
in §1.4.

### 2.2 Why the plain session ends at 0

`AVE_EnableH264FWRCSettings` (`0x3a9a4`–`0x3aa8c`) [C]:

```
if (RC.usageMode   /* S+0x130 */ != 0)   { RCModeDriver=0; RCFlag=1; DRV+0x30=1; return; }  // 0xF survives
if (RC.RealTimeClient /* S+0x104 */ == 1){ RCFlag=1; EnableSelStatsFlags=0; ... }           // 0x3aa44
else if (DRV+0x88 == 0)                  { RCFlag=1; EnableSelStatsFlags=0;                  // 0x3aa7c
                                           bFlatAreaLowQpEn=1; eStaticAreasLowQpSel=1; ... }
```

The four inputs, and why each takes this path:

- `usageMode` is 0 unless the client sets the private `kVTCompressionPropertyKey_Usage`. That key's handler is at `0x12c60`, and the SetEncoderDefault store is at `0x29bc8`.
- `ManageSessionSettings` sets `usageMode` to `0x2710` only when VP+0xFE6E is set, or RC+0x24 == 2 (`0x35504`–`0x35524`). Both are 0 by default. `0x2710` is the multipass value: it keeps `0xF`, which is what a statistics field would need for multipass.
- `RealTimeClient` stays at its `0xCDCDCDCD` "unset" sentinel (`0x29bd4`, from constant `0x115f90`) unless `kVTCompressionPropertyKey_RealTime` is set (`0x1640c`).
- DRV+0x88 is set only by a `defaults write` RC override (`0x352c0`, `0x352d8`).

So Baseline, Main, RealTime on or off all send **`EnableSelStatsFlags = 0`**.
Nothing on the path sets it again before `DriverInit`. The only candidates are
the table rows above, and each needs lossless, HEVC, multipass or a
`defaults write` key. [C]

### 2.3 What the bits do in the firmware

`InitEncodingParameters` splits the word at fw `0x5cf70`–`0x5cfc0` [C]:

| bit | ctrl byte | where it goes |
|---|---|---|
| 27 | `ctrl+0x23FD5` | no reader outside InitEncodingParameters *(d)* |
| 1 | `ctrl+0x23FD6` | no reader outside InitEncodingParameters *(d)* |
| 0 \| (VP+0x1D ≠ 0) | `ctrl+0x23FD7` | `ConfigureMCPUs` `0x61294` `ldrb w8,[x26,#18]` → mode word **bit 4** |
| 2 | `ctrl+0x23FD8` | `0x61324` → mode word **bit 7** (`lsl #7`, `0x61364`) |
| 3 | `ctrl+0x23FD9` | `0x61328` → mode word **bit 10** (`lsl #10`, `0x61354`) |
| 4, 5 | `ctrl+0x23FDA`, `ctrl+0x23FDB` | no reader outside InitEncodingParameters *(d)* |

The same mode word goes to DMem+0 of MbInput, MotionEst, IntraEst, ModeDec and
ReconLuma *(d)*.

**The IntraEst image never tests bits 4, 7 or 10** *(d, whole 0x446-byte image
read)*. ReconChroma uses bit 7 and bit 4 only to accumulate hardware readouts
into DMem, which is statistics [I]. That fits the field's name.

---

## 3. wire 0xFCE3 and 0xFCE4

These are the `x24 = VP+0xFC78` block of the firmware dumper. User space
initialises the whole block in `AVE_SetEncoderDefault` [C]:

| VP | wire | name (fw dumper VA) | macOS | VA | we send |
|---:|---:|---|---:|---|---:|
| 0xFC78 | 0xFCD8 | `verbose` (0x39e5c) | 0 (bzero) | `0x5f70` | 0, or `session_dbg` |
| 0xFC7C–7E | 0xFCDC–DE | `bDisableIntra4x4/8x8/16x16` (0x39e70..98) | 0 | `0x29a98` (`str xzr`, 8 bytes) | 0 |
| 0xFC7F | 0xFCDF | `bRestrictInter4x4` | 0 | `0x29a98` | 0 |
| 0xFC80 | 0xFCE0 | `search_range` u16 | 0 (= ±192H×±96V) | `0x29a98` | 0 |
| 0xFC82 | 0xFCE2 | `disable_skip_mode` | 0 | `0x29a98` | 0 |
| **0xFC83** | **0xFCE3** | **`disable_intra_mode`** | **0** | **`0x29a98`** | 0 |
| **0xFC84** | **0xFCE4** | **`enable_IPCM_in_IntraSlice`** | **0** | **`0x29a94`** | 0, or `session_ipcm` |
| 0xFC85 | 0xFCE5 | — (→ mode word bit 18, fw `0x5d0ac`/`0x61330`) | 0 | `0x29b10` | 0 |
| 0xFC86 | 0xFCE6 | `bSkipThrdEn` | 0 | `0x29aa0` | 0 |
| 0xFC87 | 0xFCE7 | `no_bipred` | 0 (bzero) | — | 0 |
| **0xFC88** | **0xFCE8** | **`pix_pck`** | **1** | `0x29b1c` | 0, or `session_src_cfg` |
| 0xFC89 | 0xFCE9 | — | 0 (bzero) | — | 0, or `session_src_bit3` |
| 0xFC8A | 0xFCEA | — | 0 | `0x29aa8` | 0 |
| 0xFC8C | 0xFCEC | `mode_8x8_transform` | 2, then **0 for Baseline and Main** (§4.2) | `0x29a9c`; `0x2b1d8` | 0 |
| **0xFC90** | **0xFCF0** | **`skip_mode`** u16 | **3** | `0x29b08` | 0 |
| 0xFC94 | 0xFCF4 | `qcoeff_cancel` | 0 | `0x29b0c` | 0 |

No other user-space writer of VP+0xFC7C..0xFC84 was found on the default path.
That rests on `tools/ua_vp_stores.py` over the whole H.264 range. The tool
cannot follow `AVE_SetProperty_internal`'s branches, so a private property that
sets these bytes cannot be excluded. That does not matter for "no special keys".
[C]/[I]

**The name of 0xFCE4 matters.** It is not a general "disable intra" switch:

- The firmware dumper calls it `enable_IPCM_in_IntraSlice`.
- `DebugInit` describes the same control as "Disable all intra modes in intra slices: %d(i.e., code macro-blocks in I slices as I_PCM)" (string at fw VA `0xc7520`).
- macOS never sets it.

So 1 is an experiment (I_PCM), not a macOS default. The driver's
`session_ipcm` is correctly off by default. [C]

---

## 4. The [62](62-kext-field-map.md) §1.4 scalars

| VP | wire | name | macOS, plain H.264 | VA | fw use | we send |
|---:|---:|---|---|---|---|---:|
| 0x0C | 0x6C | `bEnableFwOverride` | 0 | `0x29a7c` | both set together trips an assert (fw `0x5ca38`); mode word 0x764 bit 2 *(d)* | 0 |
| 0x0D | 0x6D | `bEnableMBInputCtrl` | 0. Set only for `usageMode == 1` (`0x357b4`) | `0x29a7c` | MB-input divert | 0 |
| 0x0E | 0x6E | `bEnableContextSwitchInTheMiddleOfAFrame` | 0 | `0x29aa4` | params+42 | 0 |
| 0x0F | 0x6F | `bDisableBinCountsInNALunitCheck` | 0 | `0x29aa4` | params+38 | 0 |
| 0x10 | 0x70 | `MaxMvsPer2Mb` | **16** for level ≥ 3.1, **32** at 3.0, **64** below | `0x374f4`–`0x37534` | if ≠ 0: params+15 = (v < 17) (fw `0x5d3c0`) | 0 (skips) |
| 0x14 | 0x74 | `MaxSubMbRectSize` | **576** for Baseline below level 3.1, else 0 | `0x37518`–`0x37550` | → `ctrl+0x2C1F8` | 0 |
| 0x18 | 0x78 | `BFrames` | 0 for Baseline (`0x3b75c`). For Main with frame reordering on (the default, RC+0x10 = 1): the client's count, else 3 or 1 (`0x3b51c`–`0x3b540`) | | RC init only | 0 |
| 0x1C | 0x7C | `bClosedGOP` | 0 (bzero) | — | — | 0 |
| 0x1D | 0x7D | `bEnableAdaptB` | **1** by default (`0x29a84`); **0 for Baseline** (`0x3b760`) | | ORed into mode-word bit 4; RC init | 0 |
| 0x20 | 0x80 | `LowDelay` | 0 | `0x2c048` | RC init | 0 |

For our session (1280×720, Baseline, I/P only), this block agrees with macOS
except for VP+0x10/0x14. Those are level limits for motion vectors, and do not
reach intra estimation. [C] for the values, [I] for "irrelevant to intra".

### 4.1 Level

For 1280×720 at 30 fps, `AVE_ManageSessionSettings` picks level 3.1: 108 000
MB/s is exactly the 3.1 limit ("restrict based on number of macroblocks per
second", `0x365fc`). That gives `MaxMvsPer2Mb = 16`. [I]

### 4.2 `mode_8x8_transform` follows the profile

`SetEncoderDefault` sets `mode_8x8_transform = 2` (`0x29a9c`). It also sets the
PPS `transform_8x8_mode_flag` byte: `str w8,[x20,#0xb1c]` with `w8 = 0x01000001`
(`0x29e60`–`0x29e68`) makes both `deblocking_filter_control_present_flag` and
`transform_8x8_mode_flag` 1.

`AVE_ValidateEncoderParameters` then acts on Baseline (66) and Main (77)
(`0x2af00`–`0x2af18`). When the flag is set, it logs "profile_idc = %d and
transform_8x8_mode_flag is true. setting it to false." and clears both
(`0x2b1d4` `strb wzr,[x24,#0xcf3]`, `0x2b1d8` `str wzr,[x24,#0xc]`).

So macOS sends **0** for Baseline and Main, the same as us. [C]

In the firmware, 0 clears bit 0 of two IntraEst per-slice mode words (fw
`0x5e7ec`–`0x5e804`), and `(v & 3)` goes to `0x40D28A088` (fw `0x57818`–`0x5782c`).
[73](73-modedec-costs.md) row 6 reads that as the Intra8x8 enable. That is
consistent with this: Main/Baseline has no 8x8. [C] for the stores, [I] for
the meaning.

---

## 5. Full diff: what macOS sends that we do not

### 5.1 Two macOS configurations

| path | how it is reached | what it does |
|---|---|---|
| **A: plain VT session** | default | `EnableH264FWRCSettings` sets `RCFlag = 1` (`AVE_RC_ON`) (`0x3aa74`) |
| **B: macOS fixed-QP** | macOS's own FIXQP arm in `AVE_H264NewDefaults…` (`0x3b89c`–`0x3b900`), taken when `RCFlag == 3` or `RCModeDriver == 3` survives to that point. `AVE_EnableH264FWRCSettings` runs first and rewrites `RCFlag = 1` on every branch except the one where DRV+0x88 ≠ 0 (§2.2). So in practice B needs a private RC-override property that also sets DRV+0x88: the shared tail at `0x18c08`/`0x18c0c` (`strb 1,[x28,#0x2d8]`) is reached from the RC-mode handlers at `0x167a0`, `0x1695c` and `0x16a98` [C]. The private `EnableQPModulation = false` writes `RCFlag = 3` (`0x1bbc0`/`0x1bbc4`, log "Forced Fixed QP encoding") but does not set DRV+0x88, so on a `usageMode == 0` session its 3 is overwritten [I, from the order] | `RCFlag == 3` → **`RCFlag = 2` (`AVE_RC_FIXQP`)**, `RCModeDriver = 0` (`0x3b8a8`–`0x3b8b0`). `RCModeDriver == 3` → `RCModeDriver = 2`, `RCFlag = 0` (`0x3b8bc`–`0x3b8d4`). Both then clear `EnableSelStatsFlags`, `bEnableQPMod`, `bEnableQPModRefresh`, `bEnableLamdaMod`, `bFlatAreaLowQpEn`, RC+0x4D and `eStaticAreasLowQpSel` (`0x3b8dc`–`0x3b900`) |

The driver runs FIXQP, so **path B is the closest macOS configuration**. Path B
leaves every λ field and table from `SetEncoderDefault` untouched: none of the
stores at `0x3b8a8`–`0x3b964` reaches RC+0x68..0x643. [C]

### 5.2 `AVEFWRCSettings` (wire = 0xFF30 + RC)

User-space values are [C] at the cited VA. "fw" gives where the firmware reads
the field.

| RC | wire | name | A: plain | B: macOS FIXQP | we send | fw consumer | intra/λ? |
|---:|---:|---|---|---|---|---|---|
| 0x00 | 0xFF30 | `ui32Bitrate` | `w·h·1.5·0.15·30` (`0x29b30`–`0x29b60`), then the client's/level logic | same | `session_bitrate` | RC | no |
| 0x04 | 0xFF34 | `ui32IdrPeriod` | 30 (`0x29b60`) | same | key interval | RC | no |
| 0x10 | 0xFF40 | `bAllowFrameReordering` | **1** (`0x29bb4`) | 1 | 0 | RC/GOP | no |
| 0x18 | 0xFF48 | `ui32AverageNonDroppableFrameRate` | `0xCDCDCDCD` ("unset", `0x29c58`) | same | 0 (FIXQP) | RC | no |
| 0x1C | 0xFF4C | `ui32ExpectedFrameRate` | 30 (`0x29b6c`) | same | fps | RC | no |
| 0x20 | 0xFF50 | `ui32RCFlag` | **1** (`0x3aa74`) | **2** (`0x3b8ac`) | 2 | fw `0x5ceb4` | — |
| 0x24 / 0x28 | 0xFF54 / 0xFF58 | bitrate selector / alt bitrate | 0 / = bitrate (`0x29b78`) | same | 0 / 0 | RC | no |
| 0x30 | 0xFF60 | (float) | **1.0f** (`0x29b74`) | same | 0 | RC init `0x5db1c` | no |
| 0x40 | 0xFF70 | `bEnableQPMod` | **1** (`0x29b80`) | 0 | 0 | → DMem 0x764 bit 0 (per-MB QP from `0x41243228`) *(d)* | QP |
| 0x42 | 0xFF72 | `bEnableLamdaMod` | **1** (`0x29b8c`) | 0 (`0x3b8f0`) | 0 | → mode-word bit 0 (fw `0x5d178` → `ctrl+0x23FC8`; `0x612d8`): IntraEst/ModeDec substitute their image λ tables per MB *(d)* | **λ** |
| 0x43 | 0xFF73 | `bEnableVarianceQPMod` [I: Validate clears this byte under the log "bEnableCrcQPMod set and bEnableVarianceQPMod set … Forcing bEnableVarianceQPMod to disabled", `0x2c56c`–`0x2c5f0`] | **1** (`0x29b8c`, same `strh 0x101`) | **1** (not cleared) | 0 | → DMem 0x764 **bit 9** (fw `0x5d188` → `ctrl+0x23FC9`; `0x61434`/`0x61440`). No IntraEst/ModeDec test found *(d)* | [U] |
| 0x48 | 0xFF78 | `bEnableQPModRefresh` | 1 (`0x29b84`) | 0 | 0 | `ctrl+0x23FC7` | no |
| 0x4B | 0xFF7B | `bFlatAreaLowQpEn` (named by the user-space BPP log, `0x3ac30`) | set to 1 (`0x3aa80`) and cleared if BPP is low (`0x3ab80`/`0x3abd8`); then **0**: Validate keeps it only if `RCFlag == 1 && bEnableQPMod && !bEnableQPModRefresh`, and QPModRefresh is 1 by default (`0x2c3f4`–`0x2c4c0`) | 0 | 0 | flat-MB low-QP *(d)* | QP |
| 0x46 | 0xFF76 | `bEnableCrcQPMod` [I, from the log at `0x2c528`] | 0 (`0x29cac`); 1 only for `usageMode 0x26` (`0x35720`) | 0 | 0 | `ctrl+0x23FCF` → DMem 0x764 bit 5 *(d)* | no |
| 0x4D | 0xFF7D | — | 1 (`0x29c6c`), then **0**: Validate clears it when `RCFlag == 1` (`0x2c4c4`–`0x2c4d8`) | 0 (`0x3b8f8`) | 0 | not read in InitEncodingParameters *(d)* | no |
| 0x50 | 0xFF80 | `eStaticAreasLowQpSel` | 1 (`0x3aa84`) | 0 | 0 | static-area low QP | QP |
| 0x54 | 0xFF84 | `RealTimeClient` | `0xCDCDCDCD` unless RealTime set (`0x29bd4`, `0x1640c`) | same | 0 | fw `0x5cee4`: `ctrl+0xA80 = (v ≠ 0)` | no |
| 0x58 | 0xFF88 | `SoftMinQP` | `0xCDCDCDCD` (`0x29bd4`) | same | 0 | RC (fw clamps ≥ 51 to 0) | no |
| 0x5C | 0xFF8C | (max QP) | 51 (`0x29bd4`) | same | 51 | RC | no |
| **0x68** | **0xFF98** | **`ME_FullPelLambda`** | **0x400** (`0x29ba0`/`0x29ba4`) | **0x400** | **0** | `ctrl+0xB10` → `(v·nQuant + 0x200) >> 10` → `0x40D190500/504/508/600/604/608` (fw `0x5d3d8`, `0x55ecc`–`0x55f2c`) | **λ** |
| **0x6C** | **0xFF9C** | **`ME_SubPelLambda`** | **0x400** | 0x400 | **0** | `ctrl+0xB14` → `0x40D19050C..518`/`60C..618`, clamped (fw `0x55ed4`–`0x55f9c`) | **λ** |
| **0x70** | **0xFFA0** | **`ME_LowResLambda`** | **0x400** | 0x400 | **0** | `ctrl+0xB18` → setLRME `0x40D150134/138` *(d)* | λ (P only) |
| **0x74** | **0xFFA4** | **`MD_InterLambda`** | **0x400** | 0x400 | **0** | `ctrl+0xB1C` → **`0x40D26A0A0`** (fw `0x56350`–`0x5635c`) | **λ** |
| **0x78** | **0xFFA8** | **`MD_IntraLambda`** | **0x400** (`0x29bb0`, constant `0x116128`) | 0x400 | **0** | `ctrl+0xB20` → **`0x40D26A09C`** (fw `0x5d3f0`, `0x56330`–`0x5634c`) | **λ** |
| 0x7C | 0xFFAC | `MD_IntraOffset` | 0 (`0x29bb0`) | 0 | 0 | ModeDec `3·t[QP] + offset` *(d)* | same |
| 0x80 | 0xFFB0 | `usageMode` | 0 | 0 | 0 | fw `0x5dbe8` | no |
| 0x84–0x8C | 0xFFB4–BC | `ui32InitialQpI/P/B` | 26/26/26 (`0x29bdc`, `0x29c60`) | the fixed QPs | `session_qp` | FIXQP QP | — |
| **0x90–0x15F** | **0xFFC0–0x1008F** | **per-QP λ table, 52 × u32** | **1×16, 2×4, 3×3, 4×3, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25, 29, 32, 36, 40, 45, 51, 57, 64, 72, 81, 91** (constant `0x116188`, stored `0x29be0`–`0x29c14`, last 16 B at `0x29c74`/`0x29c7c`) | same | **0** | fw memcpy `ctrl+0x15E4 ← RC+0x90, 0x5B4 B` (`0x5d3a0`–`0x5d3b8`). ConfigureMCPUs copies 0xD0 B to ModeDec DMem 0x1C (`0x61194`–`0x611a4`). ModeDec writes per MB: `0x40D26A0A4` = `3·t[QP] + MD_IntraOffset`, `0x40D26A0AC` = `−t[QP]`, `0x40D26A0FC/100` = `(−t)<<1` *(d)* | **intra cost** |
| 0x160–0x22F | 0x10090–0x1015F | table 2, 52 × u32: 0×16, 1,1,1,1, 2,2,2, 3,3,3, 4, 5,5, 6…28 | constant `0x116258` | same | 0 | → MotionEst DMem 0x30.. (`0x6120c`–`0x61214`). The AVC ME image does not read it *(d)* | no [I] |
| 0x230–0x643 | 0x10160–0x10573 | 29 × {λ, 8 × 16λ} | memcpy from `0x116328`, 0x414 B (`0x29c78`–`0x29c8c`) | same | 0 | same as table 2 *(d)* | no [I] |
| 0x644 / 0x648 / 0x64C | 0x10574–7C | — | 1 / 1 / 1 (`0x29ca4`, `0x29ca8`) | same | 0 | RC init (fw `0x5da74`–`0x5da84`) | no |

The table-1 values follow `max(1, round(2^((QP−12)/6)))`, the √λ curve of the H.264
reference encoder. That identification is [I]; the values are [C].

### 5.3 `AVE_VIDEO_PARAMS` scalars not covered in §3 and §4

| VP | wire | macOS | VA | fw consumer | we send |
|---:|---:|---|---|---|---|
| 0xFCB0 / 0xFCBC | 0xFD10 / 0xFD1C | `sao_enb_config` 0xFFFF / `sao_eo_bo_offset_config` −1 | `0x29b28`/`0x29b20` | HEVC only | 0 |
| 0xFCC0 | 0xFD20 | `input_bitdepth` **8** | `0x29b18` | not read by AVC `InitEncodingParameters` (no `[x23,#1376]` load in `0x5c9c8`–`0x5e0c0`) [C for the scan] | 0 |
| 0xFD44 | 0xFDA4 | **1** | `0x29aac` | fw `0x5d10c` → `params+368` | 0 |
| 0xFD48 | 0xFDA8 | `bSliceEncodingMode` 0 | `0x29ab0` | | 0 |
| 0xFD4C / 0xFD50 / 0xFD54 | 0xFDAC / 0xFDB0 / 0xFDB4 | `sSliceMap`: iNum = **1**, slice 0 = {0, **height in pixels**} | `0x29abc` (constant `0x116110`), `0x29ac4`; `kVTCompressionPropertyKey_…Slices` rewrites it (`0x142b0`) | fw memcpy 0x104 → `client+0xCC` (`0x14414`); consumer [U] | iNum = 1, slice 0 = {0, **0**} |
| 0xFE54 | 0xFEB4 | **16** | `0x29acc` | fw `0x5d008` → `params+380` | 0 |
| 0xFE60 | 0xFEC0 | from the pixel buffer (`[sp+0xd9]` of `AVE_VerifyImageBuffer`) on devices with `[S+0x11d2c] < 14`, else 0 | `0x2a124`–`0x2a128`, `0x2a0dc` | `src_mode` ([69](69-source-to-modedec.md)) | 0 / `session_src_mode` |
| 0xFE9C–0xFEB0 | 0xFEFC–0xFF10 | 0, 0, **−1, −1, −1, −1** | `0x29ae0`, `0x29ae4`, `0x29af4` | multipass block: `ctrl+0x24D4C..60`; fw substitutes 6 and 0x1305 for negative values (`0x5ce1c`–`0x5ce38`); readers are the multipass/LRME paths | 0 |
| 0xFEC4 / 0xFEC8 | 0xFF24 / 0xFF28 | `iNumViews` / `iLayerNum` = 1 / 1 | `0x5f78` | kext asserts | 1 / 1 |

### 5.4 How the firmware's mode word would look

This is [I]: the §5.2/§5.3 values fed through the *(d)* bit map.

- **IntraEst DMem word 0**
  - macOS path A, Baseline: bit 0 set (`bEnableLamdaMod`), plus bits 4/7/10 = 0 (EnableSelStatsFlags 0, AdaptB 0).
  - macOS path B: **0**, the same as our f39/f40 reading ([53](53-first-frame.md) end).
- **DMem 0x764**
  - path A: bits 0 (QPMod), 6 (static areas, with QPMod) and 9. Bit 7 (`bFlatAreaLowQpEn`) ends 0 (§5.2).
  - path B: **bit 9 only** (RC+0x43).
  - ours: 0.

So the zero we measured is what macOS's own fixed-QP configuration would
produce, apart from bit 9, whose effect is [U].

---

## 6. What this implies

1. **Retire VP+0x08 as a suspect.** macOS sends 0 there for every plain H.264
   session. The docs/70 rank-3 mechanism is closed, and its bit map was wrong
   (§7).
2. **The λ block is the real difference.** Our ModeDecision runs with:
   - `MD_IntraLambda` = `MD_InterLambda` = 0, so `0x40D26A09C` and `0x40D26A0A0` get 0 regardless of QP (fw `0x5633c`, `0x56354`);
   - ME λ = 0;
   - a zero per-QP intra-bias table, so `0x40D26A0A4` is `0 + offset` and `0x40D26A0AC/0FC/100` are 0 *(d)*.
   macOS path B sends `0x400` (×1.0 of nQuant) and the table.

   With λ = 0 the decision is distortion-only: no rate term, no intra bias. That
   biases mode *choice*. On its own it does not zero the *residual*, which is
   quantised from QPY/nQuant, and those are correct (f39: QPY 30, nQuant `0x80`).
   So this is a defect worth fixing, not a demonstrated cause of the grey frame.
   [I]

**Proposal. Not performed; for the operator ([AGENTS](../AGENTS.md)).**

Write RC+0x68..0x78 = `0x400` ×5 (wire `0xFF98`..`0xFFA8`, u32) and the 52-entry
table at wire `0xFFC0`. Leave `bEnableLamdaMod` 0, as macOS path B does. That is
one change: "send what macOS path B sends in the λ block".

What would confirm it took effect, in the same run:

- `0x40D26A09C` / `0x40D26A0A0` read back as `nQuant` (`0x80` at QP 30), where they read 0 today [I, from fw `0x5633c`];
- `0x40D26A0A4` bits 4..15 = `3·t[30] = 3·8 = 24`, and `0x40D26A0AC` = `−8` (12-bit) *(d)*.

**Negative outcome:** those registers change but the frame does not. That would
move the search below the configuration, which is the direction
[73](73-modedec-costs.md) §6 already points.

`skip_mode = 3` and `pix_pck = 1` are second-order. The skip modes act in
ReconL/ReconC (fw `0x5783c`, `0x57874`, strings `AVC_RECONLCONFIG_SKIPMODE`,
`AVC_RECONCCONFIG_SKIPMODE`). [69](69-source-to-modedec.md) already swept
`pix_pck` and saw no change.

---

## 7. Corrections and annotations to earlier documents

These annotate; nothing is retracted in place.

1. **[70](70-intraest.md) §3 row 3 and §8, the source of each mode-word bit.**
   `ConfigureMCPUs` indexes from `x26 = ctrl+0x23FC5`, not `ctrl+0x23FC4`:
   - `0x6056c` `add x8,x19,#0x23,lsl #12`
   - `0x60584` `add x26,x8,#0xfc5`

   `InitEncodingParameters` uses `x22 = ctrl+0x23FC4` (`0x5cdd8`/`0x5cde0`).
   Cross-check: `0x5cfa0` `strb [x22,#19]` and `0x61294` `ldrb [x26,#18]` are the
   same byte. [C, re-read here]

   Corrected map:

   | mode-word bit | source |
   |---:|---|
   | 4 | VP+8 bit 0, or VP+0x1D ≠ 0 |
   | 7 | VP+8 **bit 2** |
   | 10 | VP+8 **bit 3** |
   | 9 | wire **0xFCE3** `disable_intra_mode` (`0x612e4` `ldrb [x26,#26]` = `ctrl+0x23FDF`, the byte `0x5cf30` writes) |
   | 18 | wire 0xFCE5 (`0x61330`) |
   | 0 | RC+0x42 `bEnableLamdaMod` (`0x612d8` `[x26,#3]` = `ctrl+0x23FC8`, written at `0x5d178`–`0x5d184`) |

   VP+8 bit 1 does **not** reach the word. [C]
2. **[70](70-intraest.md) §8 and [62](62-kext-field-map.md) §1.4 names.**
   - VP+0x08 is `EnableSelStatsFlags`.
   - 0x0C–0x0F are `bEnableFwOverride`, `bEnableMBInputCtrl`, `bEnableContextSwitchInTheMiddleOfAFrame` and `bDisableBinCountsInNALunitCheck`.
   - 0x10 is `MaxMvsPer2Mb`, 0x14 `MaxSubMbRectSize`, 0x18 `BFrames`, 0x1C `bClosedGOP`, 0x1D `bEnableAdaptB` and 0x20 `LowDelay`.
   - Wire 0xFCE4 is `enable_IPCM_in_IntraSlice`, and wire 0xFCE8 (the driver's `src_cfg_byte`) is `pix_pck`.

   All come from the firmware dumper at `0x39d10`–`0x3a4c8`, confirmed by user
   space printing the same names from the same offsets. [C]
3. **[65](65-pframes.md) §2.2 (last paragraph) and §2.3 ("no host field for …
   lambda in the AVC path"; the HEVC printer's fields are "off the HEVC init
   command, not `AVE_VIDEO_PARAMS`").** Both are contradicted.
   - The AVC `CAVCController::InitEncodingParameters` reads RC+0x68..0x7C of the
     AVC command (fw `0x5d3d8`, `0x5d3f0`), and the AVC `setPipe` programs them
     (fw `0x55ecc`, `0x5633c`, `0x56354`).
   - The printer at `0x39d10` dumps the same `VIDEO_PARAMS`/RC layout that the
     AVC path reads at the same offsets. For example, `verbose` is VP+0xFC78 in
     the printer and `[x23,#1304]` in AVC InitEncodingParameters (`0x5cedc`).

   So there are six host λ fields, plus the RC+0x90 table (§5.2). [C]
4. **[73](73-modedec-costs.md) row 9** named RC+0x74..0x7C "from the HEVC twin".
   Those names stand. The dumper is `CHEVCController::DebugInit`, but it prints
   the shared `VIDEO_PARAMS`/RC struct that the AVC path reads at the same
   offsets. The values macOS sends are now known: `0x400`, `0x400`, 0. [C]
5. **[62](62-kext-field-map.md) §1.4 "none of them is on the SEB path … safe at
   zero".** Still true, but the `sSliceMap` row needs a note: macOS fills slice 0
   as {0, height}, and we send {0, 0}. The consumer of the entries is [U]. CAVLC
   produced whole frames with {0, 0}, so this is recorded, not ranked.

**Spot-checks behind the *(d)* rows.** Each was re-read here with `tools/disas.py`:

- `0x5d3a0`–`0x5d3bc` (the 0x5B4 copy);
- `0x6116c`–`0x611b8` (three 0xD0 copies);
- `0x5d3d8`–`0x5d3f4` (λ → `ctrl+0xB10..0xB24`);
- `0x55ec0`–`0x55f2c` and `0x56330`–`0x5635c` (λ × nQuant → registers);
- `0x6056c`/`0x60584` and `0x61290`–`0x612e4`, `0x61320`–`0x61368`, `0x61420`–`0x61440` (mode-word and 0x764 sources);
- `0x5e7b8`–`0x5e804` (`mode_8x8_transform`);
- `0x5e124`–`0x5e138` and `0x57830`–`0x57874` (`skip_mode`).

Not re-read here: the MCPU-image claims, namely the IntraEst image never testing
bits 4/7/10, the ModeDec per-MB formulas at image `0x514`–`0x554`, and the AVC
MotionEst image not reading DMem `0x30..0x513`. The delegated analysis's
disassemblies of those images are in the scratchpad, not in the repo.

---

## 8. Open items

- Whether the kext changes RC λ fields per frame. The string `AVE_SendRC
  bUpdateLambdaTable = %d` exists in the 13.5 kext and was not traced. [U]
- `params+368` (VP+0xFD44 = 1 on macOS) and `params+380` (VP+0xFE54 = 16): no
  consumer traced. [U]
- VP+0xFE60 on macOS for an NV12 source: the byte comes from
  `AVE_VerifyImageBuffer`'s output at `+0x91`, which was not decoded. [U]
- `AVE_SetProperty_internal` (66 KB) was read only for the keys cited. A private
  property could set further fields. That is irrelevant to "no special keys", but
  the claims above say "default path" for that reason.
- The dyld shared cache (SystemOS cryptex) was **not** extracted. It is a
  deflated 3.8 GB zip member, so it cannot be range-read. The fetch was stopped
  at 2.06 GB after 40 minutes; the command to finish it unattended is in
  `PROVENANCE-userspace.txt`. It is not needed for this result, because the
  encoder is a standalone bundle (§1.1). But no search of the cache has excluded
  another user-space component that touches `VIDEO_PARAMS`. The `IO_Prepare`/
  `IO_Start` callers found here are all inside the bundle (§1.2), so another
  writer would have to go through this bundle's `AppleAVEVA_*` layer. [I]

---

## 9. Reproduce

```sh
# 0. tools (host is aarch64 Ubuntu)
sudo apt-get install -y python3-fsapfs libfsapfs-utils lzfse
mkdir -p data/blobs/tools && cd data/blobs/tools
curl -LO https://github.com/blacktop/ipsw/releases/download/v3.1.724/ipsw_3.1.724_linux_arm64.tar.gz
tar xzf ipsw_3.1.724_linux_arm64.tar.gz && cd ../../..
U=https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw
data/blobs/tools/ipsw info --remote "$U"          # FileSystem 096-63007-081.dmg

# 1. the encoder bundle, ~4 MB of range requests out of the 7.5 GB system DMG
python3 tools/fetch_userspace.py --list-parts      # partition 4 = Apple_APFS
P=/System/Library/Video/Plug-Ins
python3 tools/fetch_userspace.py \
  $P/AppleVideoEncoder.bundle/Contents/MacOS/AppleVideoEncoder \
  $P/AppleVideoEncoder.bundle/Contents/Info.plist \
  $P/AppleVideoEncoder.bundle/Contents/version.plist \
  $P/AppleAVEEncoder.bundle/Contents/MacOS/AppleAVEEncoder      # negative control
B=data/blobs/macos-13.5/userspace/fs$P/AppleVideoEncoder.bundle/Contents/MacOS/AppleVideoEncoder
sha256sum $B        # b1f8fd38242dfa532d9de56f68cc0f2d46311df10e7071bc4abd55ee834da803

# 2. disassemble (about a minute) and list every VP/RC store
data/blobs/tools/ipsw macho disass $B -x __TEXT.__text --no-color > /tmp/ave_ua.s
python3 tools/ua_vp_stores.py /tmp/ave_ua.s 0x5be0 0x43d84 x0=0  > /tmp/s0.txt
python3 tools/ua_vp_stores.py /tmp/ave_ua.s 0x5be0 0x43d84 auto  > /tmp/s1.txt
sort -u /tmp/s0.txt /tmp/s1.txt | grep -E 'sub_2985c|sub_3a908|sub_3ae80|sub_2a7b0|sub_34b1c'

# 3. the functions this document reads
grep -n -A700 '^sub_2985c:' /tmp/ave_ua.s | less   # AVE_SetEncoderDefault
grep -n -A200 '^sub_3a908:' /tmp/ave_ua.s          # AVE_EnableH264FWRCSettings (0x3aa2c..0x3aa8c)
grep -n -A600 '^sub_3ae80:' /tmp/ave_ua.s          # AVE_H264NewDefaultsBasedOnProfileUsageDefault
sed -n '/^0x00020bbc/,/^0x00020c54/p' /tmp/ave_ua.s # DriverInit descriptor -> S+0x860 / S+0xB0
sed -n '/^0x00005f44/,/^0x00005f78/p' /tmp/ave_ua.s # bzero(S+0xB0,0x7AC), bzero(S+0x860,0x11424)
python3 - <<'EOF'                                    # the RC constant tables
import struct; d=open('data/blobs/macos-13.5/userspace/AVE.bin','rb').read()
print(struct.unpack('<52I', d[0x116188:0x116188+0xd0]))  # RC+0x90 per-QP table
print(struct.unpack('<4I',  d[0x115f90:0x115fa0]))        # RC+0x54..0x60
print(hex(struct.unpack('<I', d[0x116128:0x11612c])[0]))  # MD_IntraLambda
EOF

# 4. firmware side (names and consumers)
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x39d10 -n 0x7c0    # VP dumper, names
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cf70 -n 0x50     # VP+8 split
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d3a0 -n 0x58     # RC+0x90 copy, lambdas
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x56330 -n 0x30     # MD lambda -> 0x40D26A09C/0A0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x6056c -n 0x1c     # x26 = ctrl+0x23FC5
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x61290 -n 0x1b0    # mode word / DMem 0x764
```
