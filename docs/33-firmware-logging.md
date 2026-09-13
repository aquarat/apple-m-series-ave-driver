# Firmware logging: getting AVE's own instrumentation onto the host

Recovered by static disassembly of `AppleAVE2.kext` (inside `kc.macho`) and
`ave_h13c.bin`. **No hardware was used.** Every constant cites the VA of the
instruction it was read from. Firmware VAs are image VAs; file offset =
`VA + 0x4000` for both firmware segments.

Marking: **[C] confirmed** — read out of the disassembly. **[I] inferred**.
**[?] unknown**.

> **Version note (2026-09-13).** This document is **macOS 26.6.2** and stands
> for 26.6.2. The mechanism it describes **does not exist on macOS 13.5**, the
> firmware this machine runs ([43](43-macos-13.5-firmware.md)); see
> [45](45-abi-13.5-boot-ipc.md) §2.3. On 13.5: the firmware has no
> `AVE_Log_*` symbols and no `gs_psCfg` (§3, §9.3's `0x195090`/`0x1950b0` have
> no counterpart); its instrumentation calls RTKit `CLogger::Print` (893 call
> sites) and `CLogger::Assert` (913), with no per-subsystem level table (§8);
> the kext has no `AVE_FwLog` class and no `FwLog` surface (§1, §4); there is no
> `_S_AVE_Fw_Cfg` block, so nothing carries a log address (§6). The
> host-driven branch binds `CLoggerInterProcessor` to the **`TERMINAL`** IPC
> channel (`0xa636c..0xa63a0`) and the kext dispatches that channel to
> `ProcessIntr_Log` (`0xfffffe0008f0d790`) — so on 13.5 the firmware log needs
> a completed handshake. RTKit's crashlog buffer on 13.5 is `malloc(0x400)` with
> its pointer at `0xeefd0` (`0xada74..0xadaa4`), where 26.6.2 uses
> `0x1347c0`/`0x2649a0`.
> All **[C]**; that `Print` output reaches the `TERMINAL` ring end to end is
> **[I]**.

---

## 0. The short answer

**Is a crashlog readable right now, without working IPC? Yes in principle, no
in practice at the current bring-up stage — and the reason why is itself the
most useful finding in this document.**

* The firmware's crash handler does **not** write a separate host-visible
  crashlog blob. `RTK_platform_crashlog_complete` (`0xe0660`) prints the
  exception type and every call-stack frame through **`AVE_Log_Output`**
  (`0xe06b4`, `0xe0740`, `0xe07d0`), at subsystem 2 / level 3. So crashes land
  in the same ring as everything else. **[C]**
* That ring lives in a **host-allocated, DART-mapped surface** (`FwLog`,
  surface index 29). Reading it needs **no IPC channel, no doorbell and no
  interrupt** — the host just polls two counters in the shared header.
  **[C]**
* But `AVE_Log_Output` returns immediately if the log context pointer
  `gs_psCfg` is NULL (`0xa99c`/`0xa9a0`), and that pointer is only set by
  `AVE_Log_Init` (`0xab8c`), which the firmware only reaches with a real buffer
  if the host has staged a **boot configuration structure** and pointed the
  firmware at it **through the SVE scratch registers before starting the ASC**.
* **We do not do that today.** `docs/31` records that our driver goes
  `AVE_IOP::Config` → `AVE_IOP::Start` with `scratch[0..7]` all zero. Apple
  writes `scratch[0] = 0x08042006` and the config-struct IOVA into
  `scratch[1..2]` *before* `Config`. With `scratch[0]` not matching the magic,
  the firmware takes a fallback path in which it calls
  `AVE_Log_Init(NULL, 0)` **and** `AVE_DevInfo::Init(0, 0, 0, 0, 0, 0)` —
  i.e. it comes up believing it is device 0 with no logging. **[C]**

So the single change that unlocks firmware logging is the same change that may
well explain "the coprocessor starts and goes silent". See §6 and §10.

A second, weaker oracle *is* available today and costs nothing: the firmware's
`__DATA` segment is inside the buffer we DMA-allocate and map at IOVA 0, so the
host can read firmware globals directly. `__rtk_crashlog_local_buffer`
(IOVA `0x2649a0`) going non-zero proves the firmware executed
`RTK_crashlog_init`. See §9.3.

---

## 1. Where log output goes

There are three sinks in the image. Only one matters.

| Sink | Firmware code | Reaches the host? |
|---|---|---|
| **`FwLog` shared ring** | `AVE_Log_Output` (`0xa948`) | **Yes** — host polls the surface. This is the AVE-specific logger and carries all 2697 call sites. |
| RTKit `CLoggerInterProcessor` | `0xdf850`, `Channel` `0xdfa60` | Only over an IOP channel named **`TERMINAL`** (`0x13153e`). `AVE_IPC_ChName2ID` (`0xfffffe0008c4208c`) knows only `""`, `"IO"`, `"IO_T2H"`, so `AppleAVE2` never binds it. |
| RTKit `CLoggerSharedBuffer` | `0xdf760`, constructed at `0xe17b4` | Constructed only when a pointer computed earlier in `CPlatformEnvironment` is non-NULL; not chased. **[?]** |

`CLogger::Print` (`0xcf7c8`) is the fallback `AVE_Log_Output` takes when the
shared ring is absent (`0xaac8`..`0xaad4`, format `"%s\n"` at `0x11f672`). It
dispatches through `CLogger::vprint` (`0x19e638`) into whichever RTKit logger
was constructed — i.e. the `TERMINAL` channel. **Not useful to us.**

### The surface

`AVE_GetSurfaceCfg(29)` is called from `AVE_FwLog::Init` at
`0xfffffe0008b406b4` (`mov w0, #0x1d`). Entry 29 of the config table at
`0xfffffe0007ee1050` (stride `0x10`) is:

| field | value |
|---|---|
| name | **`FwLog`** |
| flags | `0x10d18` (identical to `FwIPC`) |

Sizing, from `AVE_FwLog::Init` (`0xfffffe0008b40510`):

```
size = pCfg->[+0x0C]                                0xfffffe0008b405bc
if (size <= 0x100) size = 0x20000                   0xfffffe0008b405c0 / c4 / c8
size = AVE_NextPow32(size)                          0xfffffe0008b405d0   (0xfffffe0008b3bdb0)
surfaceSize = size + 0x200                          0xfffffe0008b406b0
CreateSurface(cfg 29, surfaceSize, instance=id)     0xfffffe0008b406e8
AVE_FwLog::Reset(surface, id, size)                 0xfffffe0008b40780
```

`pCfg` is `_S_AVE_Cfg`, filled by `AVE_Cfg_RetrieveBootArgs`; `+0x0C` is the
`ave-fwlogsize` boot arg (§7). **Default ring = 128 KiB, surface = `0x20200`**,
which `AVE_SurfaceMgr::CreateSurface` rounds up to a 16 KiB multiple
(`0x24000`) per `docs/15`. **[C]**

---

## 2. The shared header (`0x1DC` bytes at the start of the surface)

Both sides agree on this layout exactly, which is the strongest kind of
evidence available here (`docs/00`, "both sides agreeing").

| Offset | Size | Meaning | Written by | Read by |
|---|---|---|---|---|
| `+0x000` | u32 | surface total size (informational) | host `0xfffffe0008b401a4` | — |
| `+0x004` | u32 | **byte offset of the ring** = `0x200` | host `0xfffffe0008b401ac` | fw `0xabf0` (`ldrsw x9,[x8,#4]`) |
| `+0x008` | u32 | **ring size in bytes** | host `0xfffffe0008b401ac` | fw `0xaa8c` (`ldrsw x21,[x8,#8]`) |
| `+0x00C` | 256 B | **per-subsystem config bytes** (`_S_AVE_Log`) | host `AVE_Log_Load` `0xfffffe0008c46cfc` | fw `AVE_Log_CheckLevel` `0xa934` (`ldrb w8,[x8,#12]`) |
| `+0x10C` | u32 | ring-fill percent threshold (default **25**) | host `0xfffffe0008c465a8` | host `AVE_FwLog::CalcProcTimeout` `0xfffffe0008b40f20` |
| `+0x110` | u32 | max poll interval (default **20000**) | host `0xfffffe0008c465a0` | host `0xfffffe0008b40f34` |
| `+0x114`..`+0x153` | — | **[?]** zeroed, never touched in the paths read | | |
| `+0x154` | u32 | **host read counter** (free-running byte count) | host `0xfffffe0008b41eb0` | host `0xfffffe0008b41b8c` |
| `+0x158`..`+0x197` | — | **[?]** | | |
| `+0x198` | u32 | **firmware write counter** (free-running byte count) | fw `0xab3c` (`str w9,[x8,#408]`) | host `0xfffffe0008b41b88` |
| `+0x19C`..`+0x1DB` | — | **[?]** | | |
| `+0x200` | ring | log text | fw `0xaab8`/`0xaaf4`/`0xab10` | host `0xfffffe0008b41648` |

The whole `0x1DC` block is zeroed by `AVE_FwLog::Reset`
(`0xfffffe0008b40138`..`0xfffffe0008b4017c`, a `stp q0,q0` run to `+0x1CF`
plus one `str q0` at `+0x1CC`). The firmware's *local* fallback allocates
exactly `malloc(0x1DC)` (`0xabfc`/`0xac00`) — same size, independently. **[C]**

The default values `25` and `20000` also appear independently in the firmware's
fallback initialiser (`0xac3c`/`0xac44` = 20000, `0xac48`/`0xac4c` = 25), which
confirms the `+0x10C`/`+0x110` identification.

### Ring discipline

* Entries are **NUL-terminated variable-length strings packed back to back**.
  The firmware writes `strlen+1` bytes and advances the counter by that
  (`0xaa7c` `add w23, w8, #1`; `0xab30` `add w9, w24, w23`). **[C]**
* Position is `counter % ringSize` (`sdiv`/`msub` at `0xaa94`/`0xaa98`
  firmware, `0xfffffe0008b414d0`/`d4` host). Wrapping copies are split into two
  `memcpy`s (`0xaae0`..`0xab10`). **[C]**
* Counters are **u32 in shared memory**; the host promotes to 64-bit and adds
  `0x100000000` when `wr < rd` (`0xfffffe0008b41ba8`). **[C]**
* **No head/tail pointer, no per-entry header, no lock visible to the host.**
  The firmware serialises with an RTKit lock private to itself
  (`0xaa80`/`0xab48`).
* Cache maintenance: the firmware `RTK_cache_clean`s the written bytes
  (`0xab1c`, `0xab28`) and then the counter word separately
  (`0xab34`..`0xab40`, `add x0, x8, #0x198` / `mov w1, #4`). It
  `RTK_dcache_invalidate`s the whole block once at `AVE_Log_Init`
  (`0xabe4`). **[C]**

### Entry text format

`AVE_Log_Output` builds the line into a 256-byte stack buffer:

```
level >= 0 :  "%lld %d AVEIOP%d %s: " + vsnprintf(fmt, ...)      fmt 0x11f65c
              args: GetCurrTime(), subsystem, GetSVEID(), levelStr
level <  0 :  "AVEIOP%d: "            + vsnprintf(fmt, ...)      fmt 0x11f651
```

(`0xa9a8` time, `0xa9b4` `AVE_DevInfo::GetSVEID`, `0xaa14`/`0xa9ec` format
selection, `0xaa24` `AVE_SNPrintf`, `0xaa50` `AVE_VSNPrintf` with limit
`255 - n`.) **[C]**

`levelStr` indexes a 9-entry table at `0x135a68` by `abs(level) & 0xf`
(`0xa9d0`/`0xa9d4`), out-of-range → `""` (`0x132cf1`):

| level | 0–2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|
| string | `""` | `CRIT` | `ERR` | `WARN` | `INFO` | `VERB` | `DBG` |

The kext carries the identical table, `gsc_piaAVE_Log_LevelStr`
(`0xfffffe0007ee0b50`). **[C]**

---

## 3. Firmware side — what actually emits

```
_AVE_Log_CheckLevel(uint subsys, char level)          0xa8f8
_AVE_Log_Output(uint subsys, char level, const char* fmt, ...)   0xa948
_AVE_Log_Init(void* hwAddr, int size)                 0xab8c
_AVE_Log_Uninit()                                     0xae50
```

There is no `AVE_Log_Default`, `AVE_Log_CheckConsole` or `AVE_Log_GetLevelStr`
in the firmware — those three are **kext** symbols
(`0xfffffe0008c46390`, `0xfffffe0008c463c8`); the firmware has only the four
above. (Checked against `data/derived/symbols.txt`, which lists every
`AVE_Log*` symbol in the image.)

### `AVE_Log_CheckLevel` — the gate

```
0xa8f8  w8 = subsys ; w0 = 0
0xa904  w9 = abs(level)
0xa908  if (subsys > 0xFF) return 0
0xa914  x10 = gs_psCfg          ; *(0x195090)
0xa918  if (x10 == 0) return 0                 <-- no config => nothing logs
0xa91c  if (subsys < 5) return 1               <-- subsystems 0..4 ALWAYS log
0xa930  return (abs(level) & 0xf) <= (cfg[12 + subsys] & 0xf)
```

Two consequences worth stating plainly:

* **Subsystems 0–4 bypass the level table entirely.** Asserts (subsystem 3) and
  crash reports (subsystem 2) are emitted unconditionally once `gs_psCfg` is
  set. **[C]**
* Higher level byte = more output. `3 = CRIT` … `8 = DBG`. Setting a
  subsystem's byte to `8` enables everything for it. **[C]**

### `AVE_Log_Init` — the two paths

```
AVE_Log_Init(hwAddr, size):
  hwAddr != 0  (shared, what we want):
     0xaba8  new(40)
     0xabc4  MappedMemory(hwAddr, size, /*cacheable?*/ true)
     0xabd0  handle -> *(0x1950a0)
     0xabd4  HwToTarget()  -> ctx
     0xabd8  ctx  -> *(0x195090)     = gs_psCfg
     0xabe4  RTK_dcache_invalidate(ctx, size)
     0xabf0  ring = ctx + (int)ctx[+4]
  hwAddr == 0  (local fallback):
     0xac00  ctx = malloc(0x1DC)   -> *(0x1950a8), also gs_psCfg
     0xac30  memset(ctx+0x0C, 5, 0x100)     ; every subsystem = WARN
     0xac44  ctx[+0x110] = 20000
     0xac4c  ctx[+0x10C] = 25
     0xac40  ring = NULL            ; -> CLogger::Print fallback
  0xac6c  ring -> *(0x1950b0)
  0xac84  lock -> *(0x1950b8)
```

Firmware globals worth knowing (all in `__DATA`, so **host-readable at these
IOVAs** once the image is mapped at IOVA 0):

| IOVA | symbol / meaning |
|---|---|
| `0x195090` | `gs_psCfg` — pointer to the log context. Non-zero ⇒ logging is live. |
| `0x1950a0` | `MappedMemory` object for the shared buffer |
| `0x1950a8` | local fallback buffer (only set when no shared buffer) |
| `0x1950b0` | ring base pointer (`ctx + 0x200`); zero ⇒ fallback path |
| `0x1950b8` | RTKit lock handle |

`AVE_Log_Init` asserts on failure and then spins forever
(`0xacdc`/`0xad6c` → `AVE_Log_Output(3, 3, "ASSERT: %s:%d %s", ...)`,
`bsp_assert_fail` `0xe08fc`, `AVE_History_Add`/`Print`, then
`0xad28`/`0xad2c` — `nop; b .`). Assert strings:
`"gs_psCfg != NULL"` (`0x11f61a`, line 187) and
`"gs_pMutex != (RESOURCE)0"` (`0x11f638`, line 197), file
`"./AppleAVE2FW/Firmware/Log/AVE_Log.cpp"` (`0x11f5f3`). **[C]**

### Instrumentation density

Counted over the whole `__TEXT` segment by scanning `BL` targets:

* **2697** call sites to `AVE_Log_Output`
* **3582** call sites to `AVE_Log_CheckLevel`
* **33** distinct subsystem ids

---

## 4. Host side — `AVE_FwLog`

Nine methods, all in the class map. Subsystem id for `AVE_FwLog`'s own logging
is `0x42`.

| Method | VA | Role |
|---|---|---|
| `Init(_S_AVE_Cfg const*, AVE_SurfaceMgr*, uint id)` | `0xfffffe0008b40510` | sizes the ring, creates surface 29, calls `Reset` |
| `Reset(AVE_Surface*, uint id, uint ringSize)` | `0xfffffe0008b400f0` | maps, zeroes `0x1DC`, calls `Update`, writes `hdr[0/4/8]` |
| `Update()` | `0xfffffe0008b4036c` | copies the host's level table into `hdr+0x0C` |
| `GetInfo(uint64* pAddr, int* pSize)` | `0xfffffe0008b40a20` | returns surface **DART IOVA** and size |
| `Process()` | `0xfffffe0008b41aa4` | drains the ring |
| `PrintEntry(uint64 wr, uint64 rd)` | `0xfffffe0008b413d8` | decodes and re-emits one entry; returns bytes consumed |
| `CalcProcTimeout()` | `0xfffffe0008b40db8` | poll interval from ring fill |
| `Print`, `Uninit` | `0xfffffe0008b40fc8`, `0xfffffe0008b3ff18` | diagnostics / teardown |

`AVE_FwLog` object layout (partial, **[C]** except where noted):
`+0x00` u32 instance id (used as the `GetDARTAddr` index, `0xfffffe0008b40ac0`);
`+0x10` `AVE_Surface*` (`0xfffffe0008b40ab8`);
`+0x18` kernel VA of the header (`0xfffffe0008b40130`);
`+0x20` u32 ring offset `0x200`; `+0x24` u32 ring size (`0xfffffe0008b4018c`).

### `Reset` — what the host stamps into the header

```
0xfffffe0008b4012c  hdrKVA = AVE_Surface::GetKernelAddr(surf, 0)   -> this+0x18
0xfffffe0008b40138.. zero 0x1DC bytes
0xfffffe0008b40184  AVE_FwLog::Update(this)                        ; fills hdr+0x0C
0xfffffe0008b4018c  this+0x20 = 0x200 ; this+0x24 = ringSize
0xfffffe0008b40194  n = AVE_Surface::GetSize(surf)
0xfffffe0008b401a4  hdr[0x00] = n
0xfffffe0008b401ac  hdr[0x04] = 0x200 ; hdr[0x08] = ringSize
```

### `Process` — the drain loop

```
0xfffffe0008b41b88  wr = hdr[0x198] ; rd = hdr[0x154]
0xfffffe0008b41b90  if (wr == rd) return 0                    ; nothing new
0xfffffe0008b41ba8  if (wr <  rd) wr |= 0x100000000           ; 32-bit wrap
0xfffffe0008b41c70  if (wr > rd + ringSize) -> OVERFLOW recovery
0xfffffe0008b41ddc  loop (max 512 iterations, 0xfffffe0008b41e98):
                       rd += AVE_FwLog::PrintEntry(this, wr, rd)
                       until rd >= wr
0xfffffe0008b41eb0  hdr[0x154] = rd
```

**Overflow recovery** (`0xfffffe0008b41d48`..`0xfffffe0008b41dbc`): restart at
`wr - ringSize * f`, where `f` is `0.8` for rings ≤ `0x20000` and `0.85` above
(doubles at `0xfffffe0007238f00` / `+8`, selected at
`0xfffffe0008b41d60`/`64`), then scan forward at most 256 bytes for the next
NUL and resume after it. Logged as `"... LOG OVERFLOW %lld %lld %d"`
(`0xfffffe00072798a0`). **[C]**

### `PrintEntry`

```
0xfffffe0008b414cc  dataOff, ringSize = hdr fields (ldpsw from this+0x20)
0xfffffe0008b414d0  rdPos = rd % ringSize ; wrPos = wr % ringSize
0xfffffe0008b414e4  ring = hdr + dataOff
0xfffffe0008b41648  n = strnlen(ring + rdPos, 256)
0xfffffe0008b41660  AVE_Log_Print(subsystem 0x41, level -6, "%s\n", entry)
```

`0xfffffe0008b41690` handles the wrap case by joining the tail and head
fragments. Returns bytes consumed so the caller can advance `rd`. **[C]**

Because `gs_sAVE_Log[0x41]` defaults to `6` (§7) and the emitting level is
`-6` (`abs` = 6), firmware lines pass the host's own filter by default.

### Polling cadence

`CalcProcTimeout` (`0xfffffe0008b40db8`):

```
0xfffffe0008b40e88  wr = hdr[0x198] ; rd = hdr[0x154]
0xfffffe0008b40ea0  if (wr == rd) return 0
0xfffffe0008b40f14  pct = (wr - rd) * 100 / ringSize
0xfffffe0008b40f20  cmp against hdr[0x10C] (=25)
0xfffffe0008b40f30  timeout = pct > 25 ? 15000 : 60000
0xfffffe0008b40fa8  clamped against 20000 (= hdr[0x110])
```

Units not established **[?]**; microseconds is the natural reading. The point
for a Linux driver is simply: **the ring is polled, never signalled.** Nothing
in `AVE_Log_Output` rings a doorbell.

`AVE_HwC::Process` calls `AVE_FwLog::Process` at `0xfffffe0008c21d44`.

---

## 5. `_S_AVE_Log` — the 264-byte config block

This is what sits at `hdr+0x0C`, and it is the whole of the log configuration.

| Offset | Size | Meaning |
|---|---|---|
| `+0x000`..`+0x0FF` | 256×u8 | one **conf byte** per subsystem id 0–255 |
| `+0x100` | u32 | ring-fill percent threshold (25) |
| `+0x104` | u32 | max poll interval (20000) |

Conf byte:

| bits | meaning | read at |
|---|---|---|
| `0:3` | maximum level to emit (`3`=CRIT … `8`=DBG) | fw `0xa938`, host `0xfffffe0008c4637c` |
| `4` | host-side "also print to console" | `AVE_Log_CheckConsole` `0xfffffe0008c463c0` (`ubfx w0, w8, #4, #1`) |
| `5:7` | **[?]** | |

The firmware masks with `0xf`, so bit 4 is harmless in the shared copy.

Host defaults, set once by `AVE_Log_Init()` (`0xfffffe0008c46530`) into
`gs_sAVE_Log_Default` (`0xfffffe000c69acf8`) and copied to `gs_sAVE_Log`
(`0xfffffe000c69ae00`):

```
0xfffffe0008c46558  every subsystem = 5 (WARN)
0xfffffe0008c46580  [4]  = 6
0xfffffe0008c46590  [11] = 6 ; [12] = 6      (sturh 0x0606 at +11)
0xfffffe0008c46584  [44] = 6
0xfffffe0008c46588  [61] = 6
0xfffffe0008c46598  [65] = 6                 ; 0x41 = AVE_FwLog::PrintEntry
0xfffffe0008c46594  [101]= 6
0xfffffe0008c465a8  [0x100] = 25 ; [0x104] = 20000
```

A one-shot guard word lives at `0xfffffe000c69af08` and doubles as a
configuration **generation counter**: `AVE_Log_Load` returns it
(`0xfffffe0008c46ce4`) and `AVE_Log_Update` only applies a table whose
generation is newer (`0xfffffe0008c46bac`). **[C]**

---

## 6. How the buffer address reaches the firmware — the boot config struct

This is the part that is missing from our driver.

### Host: `AVE_HwC::StartUpIOP` (`0xfffffe0008c1d354`)

```
0xfffffe0008c1d690   AVE_IPC::Alloc(pIPC, 0x38, &cfgKVA)      ; 56 bytes out of the FwIPC ChkPool
0xfffffe0008c1d978   AVE_HwC::MakeFwCfg(this, cfgKVA)          ; fills the struct
0xfffffe0008c1da60   AVE_SVECtrl::SetIOPFlag(pSVE, 0)          ; scratch[0] = 0x08042006
0xfffffe0008c1db50   iova = AVE_IPC::Kernel2DARTAddr(pIPC, cfgKVA)
0xfffffe0008c1dc74   AVE_SVECtrl::WriteScratch(pSVE, 1, (u32)iova)
0xfffffe0008c1dd08   AVE_SVECtrl::WriteScratch(pSVE, 2, (u32)(iova >> 32))
0xfffffe0008c1ddfc   AVE_IOP::Config
0xfffffe0008c1dee8   AVE_IOP::Start
```

`SetIOPFlag(idx)` is `Write32(bank 2, scratchTable[idx], 0x08042006)` —
the magic is built at `0xfffffe0008c91258`/`0xfffffe0008c9125c`. Scratch
registers are bank 2 `+0x18 .. +0x34` (`docs/08` §6), i.e. **`0x20D050018`
for `ave0`, `0x307050018` for `ave1`**.

`AVE_IPC::Alloc` returns a **kernel VA**: the ChkPool hands back a DART
address which is converted at `0xfffffe0008c43338`..`0xfffffe0008c4335c`
(`dart - GetDARTAddr + GetKernelAddr`). Hence the explicit `Kernel2DARTAddr`
before the scratch write. **[C]**

### `_S_AVE_Fw_Cfg` — 56 bytes (`AVE_HwC::MakeFwCfg`, `0xfffffe0008c1cbf0`)

| Off | Size | Host source | VA | Firmware use |
|---|---|---|---|---|
| `+0x00` | u32 | `AVE_HwC+0x48` **[?]** | `0xfffffe0008c1cc14` | `AVE_DevInfo::Init` arg 1 |
| `+0x04` | u32 | `AVE_DevInfo::GetDevID` | `0xfffffe0008c1cc20` | `_E_AVE_DevID` (t6001 → **12**) |
| `+0x08` | u32 | `AVE_DevInfo::GetDevNum` | `0xfffffe0008c1cc2c` | (t6001 → **2**) |
| `+0x0C` | u32 | `AVE_DevInfo::GetDevNumPerGroup` | `0xfffffe0008c1cc38` | |
| `+0x10` | u64 | `AVE_DevInfo::GetDevSubIDFlag` | `0xfffffe0008c1cc44` | |
| `+0x18` | u32 | `AVE_DevInfo::GetDevRevision` | `0xfffffe0008c1cc50` | |
| `+0x20` | **u64** | **`AVE_FwLog::GetInfo` → FwLog surface DART IOVA** | `0xfffffe0008c1cc70` | `AVE_Log_Init` arg 1 |
| `+0x28` | **u32** | **FwLog surface size** | `0xfffffe0008c1cc6c` | `AVE_Log_Init` arg 2 |
| `+0x30` | u32 | `AVE_Cfg_Get()->[0x30]` | `0xfffffe0008c1cc60` | not read in the range examined **[?]** |

`AVE_FwLog::GetInfo` itself:
`*pAddr = AVE_Surface::GetDARTAddr(surf, this[0], 0)` (`0xfffffe0008b40ac8`),
`*pSize = AVE_Surface::GetSize(surf)` (`0xfffffe0008b40ad4`).

### Firmware: `CPlatformEnvironment::CPlatformEnvironment` (`0xe0fec`)

```
0xe1020  w21 = 0x08042006
0xe1138  v = CGPIOManager::instance->vtable[5](0)     ; read scratch[0]
0xe115c  cmp w0, w21
0xe1170  if (v != magic) -> 0xe1174: zero EVERY field, log addr = 0, size = 0
0xe1198  v2 = ...Read(2)   ; scratch[2]
0xe11c4  v1 = ...Read(1)   ; scratch[1]
0xe11f0  addr = v1 | (v2 << 32)
0xe11fc  MappedMemory(addr, 0x38, false) ; 0xe1204 HwToTarget()
0xe1208.. byte-wise unaligned reads of +0x00,+0x04,+0x08,+0x0C,+0x10(64),+0x18,+0x20(64),+0x28
0xe1380  AVE_Log_Init(cfg[+0x20], cfg[+0x28])
0xe13a4  AVE_DevInfo::Init(cfg[+0x00], cfg[+0x04], cfg[+0x08], cfg[+0x0C], cfg[+0x10], cfg[+0x18])
```

The "GPIO" accessor is **`CPlatformGPIOManager::Read(uint)`** at `0xe1e54`
(vtable `0x1388e8`, slot `+0x38`):

```
0xe1e54  if (idx >= this[104]) assert-fail            ; count = 8, set at 0xe1dc4
0xe1e64  base = *(0x2649c8)
0xe1e68  off  = this[112] + idx*4                     ; this[112] = 0x01050018, set at 0xe1dec/0xe1df0
0xe1e74  return *(volatile u32*)(base + off)
```

`0x01050018` + a window base of `0x20C000000` is exactly `0x20D050018` — the
`ave0` scratch-register-0 address derived independently in `docs/08` §6, with
the same count of **8**. So this is **confirmed**, not inferred: the firmware
reads the same SVE scratch registers the kext writes.

`CPlatformEnvironment` is constructed via `CEnvironment` and
`RTK_platform_init` (`0xe18c0`, called at `0xe1058`) runs **before** the
`AVE_Log_Init` at `0xe1380`.

---

## 7. Can the host raise the log level? Yes — three ways

### 7.1 Directly, and this is what a Linux driver should do

The level table is **in host memory**. Writing `hdr[0x0C + subsys] = 8` gives
DBG for that subsystem. The firmware reads it live on every
`AVE_Log_CheckLevel` (`0xa934`) and never writes it back in the shared path
(the `memset(ctx+0x0C, 5, 0x100)` at `0xac30` is only on the *local* branch).
**[C]**

**Caveat [?]:** the firmware maps the log buffer with `MappedMemory(..., true)`
(`0xabc0`, third arg 1) versus `false` for the config struct (`0xe11f8`), and
invalidates it exactly once at `AVE_Log_Init` (`0xabe4`). If that flag means
"cacheable", level bytes written *after* the firmware boots may be seen late or
not at all. **Write the table before starting the coprocessor**, and treat
runtime level changes as unproven.

### 7.2 Apple's own mechanism — per-session, from userspace

The whole 264-byte `_S_AVE_Log` travels in the `Open` ioctl:

* `AVE_UCCmd_CheckParam_Open` → `AVE_Log_Check(_S_AVE_Log*)` at
  `0xfffffe0008cb2728`
* `AppleAVE2UserClient::Open` → `AVE_Log_Update(gen, _S_AVE_Log*)` at
  `0xfffffe0008cb558c`
* `AVE_HwC::ProcessInputCmd_Open` → `AVE_FwLog::Update` at
  `0xfffffe0008c0e29c`, which re-runs `AVE_Log_Load` into `hdr+0x0C`.

So Apple pushes a fresh level table into the shared header on **every client
open**. **[C]**

### 7.3 Boot args

From `AVE_Cfg_RetrieveBootArgs`:

| boot arg | width | VA of the `PE_parse_boot_argn` call | effect |
|---|---|---|---|
| `ave-fwlogsize` (`0xfffffe00072809a2`) | u32 | `0xfffffe0008b63ab8` | `_S_AVE_Cfg+0x0C` → FwLog ring size |
| `ave-log` (`0xfffffe00072809de`) | u64 | `0xfffffe0008b63b70` | `AVE_Log_PresetCfg(0x218, v)` at `0xfffffe0008b63c18` |

`AVE_Log_PresetCfg(w0, mask)` (`0xfffffe0008c46d9c`) decodes
`base = ((w0 >> 2) & ~0x3F) - 0x40` (`0xfffffe0008c46e68`..`0xfffffe0008c46e70`)
and `conf = w0 & 0xFF`, expands the 64-bit mask into a bitmap over subsystems
`base + 0..63`, and calls `AVE_Log_PresetConf(bitmap, 32, conf)`
(`0xfffffe0008c46ed4`), which sets the conf byte of every marked subsystem from
**5 upward** (`0xfffffe0008c46d48`, `mov w22, #5`).

With the hard-coded `0x218`: `base = 64`, `conf = 0x18` = level **8 (DBG)**
with the console bit set. **So `ave-log` only reaches subsystems 64..127.** The
richest subsystems (140 = AVC debug, 145 = HEVC debug, 226/227 = DPB/rate
control) are **out of its range** — another reason to write the table
directly. **[C]**

`AVE_Log_SetLevel` (`0xfffffe0008c465fc`) and `AVE_Log_GetLevel`
(`0xfffffe0008c4670c`) exist but have **no call sites** in the kext.

---

## 8. Subsystem census

Recovered by scanning every `BL` to `AVE_Log_CheckLevel` in the firmware and
walking back up to 8 instructions for the `mov w0,#imm` / `mov w1,#imm` pair.
33 distinct ids. Named by the functions that use them.

| id | hex | sites | levels used | representative users |
|---|---|---|---|---|
| 2 | 0x02 | 11 | 3 | `RTK_platform_crashlog_complete`, `CPlatformEnvironment` — **crash reports** |
| 3 | 0x03 | 857 | 3 | **assertions** — every `ASSERT: %s:%d %s` in the image |
| 16 | 0x10 | 1 | 5 | `CAVEClient::MCTF_ClearPendingEncode` |
| 50 | 0x32 | 21 | 8 | `PrepareROIFlags`, `CHEVCController::PipePrepareParam` |
| 73 | 0x49 | 36 | 4–7 | `AVE_FPS` |
| 85 | 0x55 | 2 | 4 | `AVE_DevInfo::Init` |
| 128 | 0x80 | 159 | 4–8 | `CFlowControllerBase` |
| 129 | 0x81 | 70 | 4,7,8 | `CFlowControllerBase` encode trigger paths |
| 130 | 0x82 | 97 | 4–8 | **`CAVECommonController::CmdProcessor`** |
| 131 | 0x83 | 37 | 4–8 | `CAVEPriorityQueue` |
| 132 | 0x84 | 1 | 4 | `CFrameType::FrameType` |
| 135 | 0x87 | 10 | 4–8 | `AVE_HEVC_PrepareSliceHeader` |
| **140** | **0x8c** | **602** | mostly 8 | **`CAVCController::DebugInit` / `DebugStats`** |
| **145** | **0x91** | **1018** | mostly 8 | **`CHEVCController::DebugInit` / `DebugEncode`** |
| 150 | 0x96 | 33 | 4–8 | `COFController` |
| 151 | 0x97 | 15 | 6,7 | `CDMVController` |
| 170 | 0xaa | 17 | 6,8 | `NotificationToHost`, pipe |
| 172 | 0xac | 13 | 4,6,8 | ISR managers |
| 173 | 0xad | 16 | 8 | `CAVERefManager` |
| 174 | 0xae | 19 | 4–7 | `CAVEPipeMcpuController` |
| 180 | 0xb4 | 26 | 4,8 | `PlatformIOPIPCManager::InitMailboxRoute`, `CFSM` |
| 181 | 0xb5 | 11 | 4,8 | `CController::PostCmdSynchronous` |
| 193 | 0xc1 | 1 | 8 | `GetMaxLowerDeltaQP` |
| 195 | 0xc3 | 6 | 4,7 | `AVC_SPS` |
| 196 | 0xc4 | 8 | 4,6,7 | `AVC_PPS` |
| 208 | 0xd0 | 1 | 4 | `AVC_FindProfileIdc` |
| 209 | 0xd1 | 4 | 4 | `AVC_FindLevelIdc`, `HEVC_FindProfileIdc` |
| 212 | 0xd4 | 7 | 4,7 | `AVE_PSInfo_Make` |
| 213 | 0xd5 | 12 | 8 | `CHEVCController::ConfigureMCPUs` |
| 214 | 0xd6 | 28 | 4–8 | `AVE_KeyFrame` |
| 226 | 0xe2 | 163 | 4,5,8 | `H264/H265VideoEncoderDPB::ManageDPBBuffer` |
| 227 | 0xe3 | 77 | 4,5,8 | `RateControl`, `printRateControlParams` |
| 253 | 0xfd | 7 | 4,6,8 | `HRD` |

Confirmed example of the field-naming output `docs/23` is after:

```
0x647a8   AVE_Log_Output(0x8C, 8, "sBufSet.saRecon[%d][0].iAddr:%016llx", 0, addr)
0x647b4   AVE_Log_CheckLevel(0x8C, 8)      ; the gate for the next one
```

So **`gs_sAVE_Log[140] = 8` and `gs_sAVE_Log[145] = 8` are the two bytes that
turn the encoder into a self-documenting oracle**, and `[130] = 8` adds the
command processor.

---

## 9. Crashlog

### 9.1 The crash path writes into the log ring

`RTK_platform_crashlog_complete` (`0xe0660`) is reached from
`RTK_crashlog_get_exception_info+0x26c` (`0xedad4`). It emits, all at
subsystem **2**, level **3 (CRIT)** — i.e. below the `< 5` cutoff, so
**unconditionally**:

| VA | format string | text |
|---|---|---|
| `0xe06b4` | `0x1313a0` | `Exception !! crash type %d` (argument from `crashlog_get_exception_type`, `0xedd40`) |
| `0xe0740` | `0x1313bb` | `[Call stack]` |
| `0xe07d0` | `0x1313c8` | `  0x%016llX` — one per frame, walking the callstack section built by `crashlog_create_callstack_section` (`0xeddbc`, section size `0x108`) |

**So a crash produces readable text in the `FwLog` ring, with no IPC channel
involved.** **[C]**

### 9.2 The RTKit crashlog buffer is firmware-local

`RTK_crashlog_init(uint)` (`0xee034`):

```
0xee050  buf  = *(0x2649a0)      ; __rtk_crashlog_local_buffer, 0 in the image
0xee054  size = *(0x134500)      ; __rtk_crashlog_requested_size = 0x4000 (16 KiB)
0xee074  if (!buf) buf = malloc(size)
0xee094  memset(buf, 0xEF, size)
0xee0a0  *(0x1347c0) = buf ; *(0x1347f0) = size
```

`__rtk_crashlog_is_local` (`0x134508`) is **1** in the image, so the buffer is
allocated from the firmware heap rather than supplied by the host. Whether that
heap lands inside the region we DMA-allocate depends on how
`__rtk_arm_start_bootstrap_area` derives `_sys_ram_base` (`0x194000`, zero in
the image) — **not established [?]**. So the crashlog *buffer* is not reliably
host-readable; the crashlog *text* in the FwLog ring is.

### 9.3 A liveness probe that works today, with no protocol at all

The firmware image is mapped at DART IOVA 0 out of a host buffer we still hold
a CPU pointer to. `__DATA` is at image VA `0x134000`. Therefore the following
firmware globals are directly readable from the host with a plain load:

| IOVA | reads as | meaning |
|---|---|---|
| `0x2649a0` | 0 | `RTK_crashlog_init` has not run |
| `0x2649a0` | non-zero | firmware reached `RTK_platform_init` — **the core is executing** |
| `0x1347c0` / `0x1347f0` | buf / `0x4000` | same, corroborating |
| `0x195090` | non-zero | `AVE_Log_Init` completed — the log context is live |
| `0x1950b0` | non-zero | logging is going to the **shared ring** |
| `0x1950b0` | 0 while `0x195090` non-zero | firmware took the **local fallback** — the boot config was rejected |
| `0x2649c8` | non-zero | the MMIO window base used by `CPlatformGPIOManager` |

That last pair is a direct test of the §6 hypothesis and costs one `readq` on
memory we already own.

---

## 10. What a Linux driver has to do

In order. Steps 1–4 must all happen **before** `AVE_IOP::Start`.

1. **Allocate the FwLog buffer.** `ringSize` a power of two (Apple's default
   `0x20000`); allocation `0x200 + ringSize`, rounded to 16 KiB; DART-mapped
   so the coprocessor can reach it. Its IOVA is `logIova`, its size `logSize`.
2. **Stamp the header** at the CPU mapping of that buffer:

   ```c
   memset(hdr, 0, 0x1DC);
   *(u32 *)(hdr + 0x00) = logSize;      /* informational           */
   *(u32 *)(hdr + 0x04) = 0x200;        /* ring offset             */
   *(u32 *)(hdr + 0x08) = ringSize;     /* ring size, must be != 0 */
   memset(hdr + 0x0C, 5, 0x100);        /* WARN everywhere         */
   hdr[0x0C + 2]   = 8;                 /* crash reports  (free anyway) */
   hdr[0x0C + 3]   = 8;                 /* asserts        (free anyway) */
   hdr[0x0C + 130] = 8;                 /* CmdProcessor            */
   hdr[0x0C + 140] = 8;                 /* AVC controller debug    */
   hdr[0x0C + 145] = 8;                 /* HEVC controller debug   */
   *(u32 *)(hdr + 0x10C) = 25;
   *(u32 *)(hdr + 0x110) = 20000;
   /* +0x154 (host read counter) and +0x198 (fw write counter) stay 0 */
   ```

3. **Build `_S_AVE_Fw_Cfg`** (56 bytes) in DART-mapped memory. Apple puts it in
   the `FwIPC` surface via `AVE_IPC::Alloc`; any DART-mapped address should do
   **[I]** — the firmware only does `MappedMemory(addr, 0x38, false)`.
   For `ave1` / t6001 (`docs/08` §7):

   | off | value |
   |---|---|
   | `+0x00` | `AVE_HwC+0x48` — **unknown [?]**; try the `sve-id` (0 for `ave0`, 1 for `ave1`) or `devNum` |
   | `+0x04` | devID = **12** (`t6000` → 11) |
   | `+0x08` | devNum = **2** (`t6000` → 1) |
   | `+0x0C` | devNumPerGroup — **[?]** |
   | `+0x10` | devSubIDFlag (u64) — **[?]** |
   | `+0x18` | devRevision — **[?]** |
   | `+0x20` | **`logIova`** (u64) |
   | `+0x28` | **`logSize`** (u32) |
   | `+0x30` | **[?]**, zero |

   The unknowns come from the 0x48-byte-stride device table at
   `0xfffffe0007edba00` and its sub-struct; `docs/08` §7 already decodes part of
   it. They are worth recovering before the first attempt, because
   `AVE_DevInfo::Init` consumes them.

4. **Point the firmware at it, then start:**

   ```c
   writel(cfg_iova & 0xffffffff, sve + 0x1C);   /* scratch[1] */
   writel(cfg_iova >> 32,        sve + 0x20);   /* scratch[2] */
   writel(0x08042006,            sve + 0x18);   /* scratch[0] — magic, last */
   /* ... AVE_IOP::Config, AVE_IOP::Start ... */
   ```

   (Bank 2 = `0x20D050000` for `ave0`, `0x307050000` for `ave1`; scratch `n` is
   at `+0x18 + 4n`. Apple writes the magic *first*, at `0xfffffe0008c1da60`,
   but the core is not running yet so order is immaterial; writing it last is
   strictly safer.)

5. **Drain the ring**, from a timer or a debugfs read:

   ```c
   wr = readl(hdr + 0x198);
   rd = readl(hdr + 0x154);
   /* 32-bit free-running byte counters; if wr < rd the writer wrapped */
   /* if (wr - rd) > ringSize you lost data: restart at wr - ringSize*4/5
      and skip forward to the first NUL */
   while (rd != wr) {
       const char *s = ring + (rd % ringSize);   /* NUL-terminated, may wrap */
       ... emit ...
       rd += strnlen_wrapped(s) + 1;
   }
   writel(rd, hdr + 0x154);
   ```

   With coherent DMA memory no cache maintenance is needed; with streaming
   mappings, invalidate `hdr+0x198` and the ring before each pass.

---

## 11. Proposed hardware experiments — **not performed**

Per `AGENTS.md`, these are written down only. All require an m1n1 hypervisor
serial console attached, per `docs/23`.

**P1 — zero-risk, no hardware change.** After the existing stage that starts
the ASC, `readq` the firmware DMA buffer at offsets `0x2649a0`, `0x1347c0`,
`0x195090`, `0x1950b0`, `0x2649c8` and print them. This touches only memory we
allocated. It distinguishes "the core never executed a firmware instruction"
from "the core booted RTKit but rejected our (absent) boot config", which is
the largest open question in `docs/31`. It should be run **before** any
register experiment, because it is free and it discriminates.

**P2 — the boot config.** Implement §10 steps 1–4 and re-run. Expected
outcomes, in order of informativeness:
* `0x195090` and `0x1950b0` both non-zero, and `hdr+0x198` advancing → we have
  firmware logging, and everything in `docs/23`'s "Oracle 1" becomes available.
* `0x195090` non-zero but `0x1950b0` zero → the config was read but
  `MappedMemory`/`HwToTarget` failed on our IOVA; suspect the log surface
  mapping.
* Both zero → the failure is earlier than `CPlatformEnvironment`, and P1's
  crashlog pointer says whether RTKit itself started.

Note the interlock from `docs/31`'s addendum: nothing clears `CPU_CONTROL`, so
**each attempt needs a reboot**, not a module reload.

**P3 — device identity.** If P2 shows the config being read but the firmware
still going quiet, the `[?]` fields of `_S_AVE_Fw_Cfg` are the next suspects:
`AVE_DevInfo::Init` is called with them immediately after `AVE_Log_Init`
(`0xe13a4`), and `AVE_DevInfo::Init` has its own asserts at subsystem 85.
Recover them statically from `0xfffffe0007edba00` first.

---

## 12. Confidence summary

| Claim | Status |
|---|---|
| Log output goes to the `FwLog` surface (index 29), not a channel or RTKit syslog | **[C]** |
| Header layout `+0x00/04/08`, level table at `+0x0C`, counters at `+0x154`/`+0x198`, total `0x1DC` | **[C]** — both sides read independently and agree |
| Entries are NUL-terminated strings in a byte ring, counters are free-running u32 | **[C]** |
| Reading the ring needs no IPC channel, doorbell or interrupt | **[C]** |
| Level semantics: emit iff `abs(level) & 0xf <= conf & 0xf`; subsystems 0–4 always emit | **[C]** |
| Level table is host-writable shared memory; firmware never writes it in the shared path | **[C]** |
| Crash reports and asserts go to the ring at subsystems 2 and 3, unconditionally | **[C]** |
| Buffer address is delivered in `_S_AVE_Fw_Cfg+0x20/+0x28`, pointed to by SVE scratch 1/2 with scratch 0 = `0x08042006` | **[C]** |
| The firmware's "GPIO" reads are the SVE scratch registers | **[C]** — `CPlatformGPIOManager::Read` `0xe1e54`, offset `0x01050018`, count 8 |
| Without the magic, the firmware runs with no logging **and** `AVE_DevInfo::Init(0,…)` | **[C]** |
| Subsystem ids 140/145/130 carry the `AVE_PICMGMT_PARAMS` field dumps | **[C]** |
| `AVE_FwLog::Init` runs before `StartUpIOP`, so the surface exists when `MakeFwCfg` runs | **[I]** — from the call sites in `AVE_HwC::Init` vs `StartUpIOP` |
| `_S_AVE_Fw_Cfg` need not live in the `FwIPC` surface | **[I]** — the firmware only maps 56 bytes at the given IOVA |
| Poll-interval units (µs) | **[?]** |
| Whether level changes made after firmware boot are observed (cacheability of the log mapping) | **[?]** |
| `_S_AVE_Fw_Cfg` fields `+0x00`, `+0x0C`, `+0x10`, `+0x18`, `+0x30` | **[?]** |
| Header bytes `+0x114`..`+0x153`, `+0x158`..`+0x197`, `+0x19C`..`+0x1DB` | **[?]** |
| `_S_AVE_Log` conf-byte bits 5–7 | **[?]** |
| Where the RTKit heap (and hence the 16 KiB crashlog buffer) physically lives | **[?]** |

## Verification (independent, not taken on the agent's word)

Every load-bearing claim was re-derived from the firmware image:

| Claim | Evidence | Verdict |
|---|---|---|
| Subsystems 0-4 bypass the level table | `0xa91c: cmp w8,#0x5 / b.cs 0xa92c / mov w0,#1 / ret` | confirmed |
| Level conf table is one byte per subsystem at header +0x0C | `0xa92c: add x8,x10,w8,uxtw` then `0xa934: ldrb w8,[x8,#12]` | confirmed |
| Emit iff `(level & 0xf) <= (conf & 0xf)` | `and w9,w9,#0xf` / `and w8,w8,#0xf` / `cmp w9,w8` / `cset w0,ls` | confirmed |
| 8 is the maximum meaningful level | `0xa9b8: sxtb w8,w21 / cneg (abs) / and #0xf / cmp #0x8 / b.hi` | confirmed |
| `gs_psCfg` lives at `0x195090` | `0xa994: adrp x8,0x195000` + `0xa998: ldr x8,[x8,#144]` | confirmed |
| Logging is dead while `gs_psCfg` is NULL | `0xa9a0: cbz x8, 0xab4c` - jumps past the whole emit path | confirmed |
| `gs_psCfg` points at the host log header, +0x04 = ring offset | `0xabec: ldr x8,[x8,#144]` then `0xabf0: ldrsw x9,[x8,#4]` | confirmed |
| Firmware write counter at +0x198 | `0xab38: add x0,x8,#0x198` and `0xab3c: str w9,[x8,#408]` | confirmed |

The last row is the one that makes this worth doing: the sink is plain shared
memory with a free-running counter, so the log is readable with no IPC channel,
no doorbell and no interrupt - i.e. it works in exactly the situation we are
in, where the core will not talk to us.
