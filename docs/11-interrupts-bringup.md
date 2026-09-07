# Interrupts and bring-up order

Everything below was read out of `AppleAVE2.kext` in `kc.macho` (macOS 26.6.2,
`kernelcache.release.mac13j`). Every offset and constant carries the VA of the
instruction it came from. Statements not backed by an instruction are marked
**inferred** or **unknown**.

Reproduce any line with:

```sh
python3 tools/disas.py --kext --addr <VA> -n <len>
```

## Headline result

The ten `AVE_HwC::ProcessIntr_*` methods are **not** ten interrupt sources, and
they do not map onto the five ADT interrupts. The real shape is:

```
one AIC interrupt  ->  one status register  ->  a bit per IPC channel
                   ->  a message ring in shared memory
                   ->  ProcessIntr_* selected by message opcode / client type
```

`ProcessIntr_Cmd`, `_CmdAck`, `_CmdErr`, `_OutputData` and the six
`_<engine>_OutputData` variants are **software dispatch on message content**,
one level below the hardware interrupt. Only `ProcessIntr_IPCCh` is selected by
a hardware status bit.

## 1. Interrupt registration — exactly one, at ADT index 0

`AVE_Drv::IO_start(IOService*, uint)` at `0xfffffe0008bdafc0` creates a single
event source per AVE instance:

| VA | instruction | argument |
|---|---|---|
| `0xfffffe0008bdb1e4` | `adrp/add x16, 0xfffffe0008bdb874` | `action` = `AVE_Drv::ISR` |
| `0xfffffe0008bdb1f8` | `adrp/add x16, 0xfffffe0008bdbb70` | `filter` = `AVE_Drv::FilterISR` |
| `0xfffffe0008bdb210` | `mov x3, x20` | `provider` = the `IOService` passed to `IO_start` |
| `0xfffffe0008bdb214` | `mov w4, #0x0` | **`index` = 0** |
| `0xfffffe0008bdb218` | `bl 0xfffffe000bf3eb80` | `IOFilterInterruptEventSource::filterInterruptEventSource` |

`0xfffffe000bf3eb80` is
`IOFilterInterruptEventSource::filterInterruptEventSource(OSObject*, Action, Filter, IOService*, int)`
(resolved from the kernelcache's own symtab).

A scan of the whole AppleAVE2 text range (`0xfffffe0008b30000`–`0xfffffe0008d40000`)
for calls to `IOService::registerInterrupt` (`0xfffffe000bee8de4`),
`IOService::enableInterrupt` (`0xfffffe000bee8b2c`) and
`IOInterruptEventSource::interruptEventSource` (`0xfffffe000bf39da8`) finds
**no other call sites** inside the kext.

**Conclusion (confirmed):** `AppleAVE2.kext` consumes one interrupt per AVE
instance — the one at index 0 of the ADT `interrupts` property.

**Inferred** (from IOKit's normal `AppleARMIODevice` behaviour, that
`IODeviceMemory`/interrupt specifiers are built in ADT property order):

| ADT index | `ave0` | `ave1` | used by kext |
|---|---|---|---|
| 0 | **1031** | **1041** | yes — the only one |
| 1 | 1025 | 1035 | no |
| 2 | 1024 | 1034 | no |
| 3 | 1027 | 1037 | no |
| 4 | 1026 | 1036 | no |

The out-of-sequence `1031` therefore *is* the one that matters, but there is
no evidence in the kext for what 1024–1027 are. **Unknown.** They are most
likely wired to the AVE-internal coprocessor and serviced by the firmware
(`CAVEPipeISRManager`, `CPlatformISRManager` exist on the firmware side), but
nothing here proves that.

`dart-ave0`'s own interrupt (1028) belongs to the DART driver, not to AVE.

### Filter and dispatch

`AVE_Drv::FilterISR` (`0xfffffe0008bdbb70`) is trivial — it bumps a counter at
`AVE_Drv+368` and returns 1, i.e. it always schedules the workloop handler
(`0xfffffe0008bdbb74`–`0xfffffe0008bdbb84`). There is no hardware read in the
primary interrupt context.

`AVE_Drv::ISR` (`0xfffffe0008bdb874`) walks four slots (`0xfffffe0008bdb980`–
`0xfffffe0008bdb9a8`, `x22` stepping 0→0x20 by 8):

- `AVE_Drv+320 + i*8` — the `IOFilterInterruptEventSource*` for instance `i`
- `AVE_Drv+256 + i*8` — the `AVE_HwC*` for instance `i`

and calls `AVE_HwC::ISR` (`0xfffffe0008c26210`) on the slot whose event source
matches. Four slots because `AVE_HwC::Init` rejects an instance index ≥ 4
(`0xfffffe0008c19f60: cmp w27, #0x4 / b.cs`).

## 2. What `AVE_HwC::ISR` reads

`AVE_HwC::ISR(IOInterruptEventSource*, int)` at `0xfffffe0008c26210`:

| VA | what |
|---|---|
| `0xfffffe0008c263f0` | `ldr x8, [x19, #184]` — this instance's event source |
| `0xfffffe0008c263f4` | `cmp x20, x8` — mismatch counts as spurious and returns |
| `0xfffffe0008c26498` | `ldr w8, [x19, #192]` — driver state |
| `0xfffffe0008c2649c` | `cmp w8, #0x2` — state 2 logs "not started" and returns |
| `0xfffffe0008c264a8` | `cmp w8, #0x3` — anything else returns |
| `0xfffffe0008c264ac` | `ldr x0, [x19, #120]` — the `AVE_SVECtrl` |
| `0xfffffe0008c264b4` | `bl 0xfffffe0008c918f0` — `AVE_SVECtrl::GetIntr(uint*)` |
| `0xfffffe0008c264bc` | `bl 0xfffffe0008c1982c` — `AVE_HwC::ProcessIntr()` |

The value fetched by `GetIntr` here is only used for the trace line at
`0xfffffe0008c26570`; `ProcessIntr` re-reads it.

### `AVE_SVECtrl::GetIntr` — the status register

`AVE_SVECtrl::GetIntr(uint*)` at `0xfffffe0008c918f0`:

```
fffffe0008c91904:  ldp  x8, x9, [x0, #24]   ; x8 = AVE_Reg*, x9 = register map
fffffe0008c91908:  ldr  w2, [x9, #4]        ; map[1] = status register offset
fffffe0008c91910:  mov  w1, #0x2            ; _E_AVE_RegType = 2
fffffe0008c91914:  bl   0xfffffe0008c53df0  ; AVE_Reg::Read32
fffffe0008c91918:  str  w0, [x19]
```

`AVE_Reg::Read32(_E_AVE_RegType, int)` at `0xfffffe0008c53df0` is a plain
indexed load:

```
fffffe0008c53df4:  cmp  w1, #0x5            ; bank index must be <= 5
fffffe0008c53e00:  add  x8, x0, #0x40
fffffe0008c53e04:  ldr  x8, [x8, w1, uxtw #3]  ; base = AVE_Reg+0x40 + bank*8
fffffe0008c53e08:  ldr  w8, [x8, w2, sxtw]     ; *(u32*)(base + offset)
```

`AVE_Reg::Init(IOService*, AVE_DevInfo*, uint, AVE_DART*, int)` at
`0xfffffe0008c532d0` fills that base array from IOKit:

- `0xfffffe0008c5336c`: `blraa` through `IOService` vtable `+0x710`. Resolved
  via `__ZTV9IOService` (`0xfffffe0007e31b78`) `+0x10+0x710` →
  `IOService::mapDeviceMemoryWithIndex(uint, uint)` (`0xfffffe000bee8fc4`),
  called with `(i, 0)`.
- `0xfffffe0008c5339c`: `blraa` through the returned object's vtable `+0x138` →
  `IOMemoryMap::getVirtualAddress()` (`0xfffffe000bf4d174`).
- `0xfffffe0008c533a0`: `str x0, [x26, #64]` — stored at `AVE_Reg+0x40+i*8`.
- `0xfffffe0008c53338`: the loop count is forced to **5** unless
  `AVE_DevInfo::GetDevType()` returns `0x1e` (30).

So **bank index == device-memory index**, and (**inferred**, from
`AppleARMIODevice` building `IODeviceMemory` in ADT `reg` order)
**bank index == ADT `reg` entry index**.

For `ave0`: bank 2 = `0x20D050000` (size `0x8000`); for `ave1`,
`0x307050000`.

### The register map is per chip generation

`AVE_SVECtrl::Init(const _S_AVE_Cfg*, AVE_DevInfo*, uint, AVE_Reg*)`
(`0xfffffe0008c90fbc`) calls `AVE_DevInfo::GetChipType()` (`0xfffffe0008bab798`)
at `0xfffffe0008c91008` and passes the result to `0xfffffe0008c90e44`, which
indexes a 21-entry pointer table at **`0xfffffe0007ee0f80`** by `chipType - 1`
(`0xfffffe0008c90e5c`–`0xfffffe0008c90e88`) and stores the result at
`AVE_SVECtrl+0x20` (`0xfffffe0008c91010`).

Three distinct maps are referenced:

| chipType | map symbol |
|---|---|
| 1–2 | `gsc_sAVE_SVECtrl_Reg_Hypnos` (`0xfffffe00072747a4`) |
| 3–12 | `gsc_sAVE_SVECtrl_Reg_Rhea` (`0xfffffe00072748b4`) |
| 13–21 | `gsc_sAVE_SVECtrl_Reg_Themis` (`0xfffffe0007274694`) |

Field meanings, from the accessors:

| map word | meaning | call site proving it |
|---|---|---|
| `[0]` | interrupt **set** register offset | `SetIntr` `0xfffffe0008c91538` (`ldr w2, [x8]`) |
| `[1]` | interrupt **status / clear** offset | `GetIntr` `0xfffffe0008c91908`, `ClearIntr` `0xfffffe0008c91954` |
| `[2]` | **count** of scratch registers (bounds check) | `WriteScratch` `0xfffffe0008c91284`–`0xfffffe0008c9128c` |
| `[3+n]` | scratch register `n` offset | `WriteScratch` `0xfffffe0008c912a4` (`add x8, x8, #0xc`) |
| `[67]` (`+0x10c`) | "idle" register offset | `SetIdle` `0xfffffe0008c91cb0` (`ldr w2, [x8, #268]`) |

Values (read directly out of `__PRELINK_TEXT`):

| map | set | status | scratch count | scratch offsets | idle |
|---|---|---|---|---|---|
| Hypnos | `0x10` | `0x14` | 8 | `0x1c`…`0x38` | `0x3c` |
| **Rhea** | **`0x0c`** | **`0x10`** | **8** | **`0x18,0x1c,0x20,0x24,0x28,0x2c,0x30,0x34`** | **`0x38`** |
| Themis | `0x0c` | `0x10` | 64 | `0x18`…`0x114` | `0x118` |

### Which map applies to M1 Pro / Max

`AVE_DevInfo::Init(_E_AVE_DevID)` (`0xfffffe0008baa724`) looks the device up in a
35-entry × `0x48`-byte table at **`0xfffffe0007edba00`** (indexing code:
`0xfffffe0008ba9eb0`–`0xfffffe0008ba9ed4`), then copies
`cap[0]`→`DevInfo+8` (DevID), `cap[4]`→`DevInfo+12` (DevType),
`cap[8]`→`DevInfo+16` (ChipType) at `0xfffffe0008baa774`–`0xfffffe0008baa780`.
Accessors: `GetDevID` `[+8]`, `GetDevType` `[+12]`, `GetChipType` `[+16]`,
`GetDevRevision` `[+20]`, `GetDevArch` `[+24]`, `GetDevNum` `[+28]`.

Relevant rows (each entry's `+16` pointer names the SoC):

| DevID | DevType | ChipType | capability entry |
|---|---|---|---|
| 10 | 8 | 5 | `gsc_sAVE_DevCap_CEntry_8103` (M1) |
| 11 | 9 | 6 | `gsc_sAVE_DevCap_CEntry_6000` (M1 Pro) |
| **12** | **10** | **7** | **`gsc_sAVE_DevCap_CEntry_6001` (M1 Max)** |
| 13 | 11 | 7 | `gsc_sAVE_DevCap_CEntry_6002` (M1 Ultra) |

ChipType 5, 6 and 7 all land in the Rhea band, so on the whole M1 family the
status register is:

> **bank 2 + `0x10`** — `ave0`: `0x20D050010`, `ave1`: `0x307050010`

and the interrupt-set (host→coprocessor doorbell) register is
**bank 2 + `0x0c`** (`0x20D05000C` / `0x30705000C`).

> **Note for [01-hardware.md](01-hardware.md).** That document currently says
> `reg[2]`'s purpose is unknown, and that `_Acis` is the IOP variant selected
> for `t6000`/`t6001`. Both need adjusting. `reg[2]` is this SVE control block.
> And the per-chip IOP function table at `0xfffffe0007ee07f0` (stride 40,
> indexed by ChipType, used at `0xfffffe0008c40840` and `0xfffffe0008c40bd8`)
> gives ChipType 5 = `_Acis` (t8101/t8103), ChipType 6 = `_Castor` (t6000),
> **ChipType 7 = `_Nyx` (t6001/t6002)**. `AVE_IOP_Start_Nyx` is at
> `0xfffffe0008c35628` and `AVE_IOP_Config_Nyx` at `0xfffffe0008c353f0`. The
> register offsets and values in `01-hardware.md`'s table are nonetheless
> correct for Nyx and Castor too — I diffed all three; they are identical
> (`0xfffffe0008c356dc`–`0xfffffe0008c35730` for Nyx's `Start`,
> `0xfffffe0008c35558`–`0xfffffe0008c35570` for its `Config`).

## 3. Acknowledge / clear

Write-1-to-clear, on the same register that is read.

`AVE_SVECtrl::ClearIntr(uint mask)` at `0xfffffe0008c9192c`:

```
fffffe0008c91954:  ldr  w2, [x8, #4]        ; map[1] -- SAME offset as GetIntr
fffffe0008c91958:  mov  w1, #0x2            ; bank 2
fffffe0008c9195c:  mov  x3, x21             ; the mask
fffffe0008c91960:  bl   0xfffffe0008c53e58  ; AVE_Reg::Write32
```

`AVE_HwC::ProcessIntr` reads the status and immediately writes the *whole read
value* back:

```
fffffe0008c1986c:  bl   GetIntr        ; status -> [x29-0x54]
fffffe0008c19920:  ldr  x0, [x19, #120]
fffffe0008c19924:  ldur w1, [x29, #-84]
fffffe0008c19928:  bl   0xfffffe0008c9192c   ; ClearIntr(status)
```

The complementary `SetIntr` (`0xfffffe0008c91510`) writes to `map[0]`, and
`SetIPCIntr(int bit)` / `ClearIPCIntr(int bit)` (`0xfffffe0008c91c38` /
`0xfffffe0008c91c5c`) are thin wrappers that do `1 << bit` first
(`0xfffffe0008c91c48`, `0xfffffe0008c91c6c`). So the set register is the host's
doorbell to the coprocessor and the status register is the coprocessor's
doorbell to the host, cleared W1C.

## 4. The demux: status bits → IPC channels

`AVE_HwC::ProcessIntr()` at `0xfffffe0008c1982c` is the whole of it:

```
fffffe0008c19854:  ldr  w8, [x0, #192] ; state
fffffe0008c19858:  cmp  w8, #0x3       ; only state 3 does anything
fffffe0008c1986c:  bl   GetIntr
fffffe0008c19928:  bl   ClearIntr(status)
fffffe0008c1992c:  mov  w20, #0x1                    ; ch = 1
fffffe0008c19948:  ldr  x0, [x19, #224]              ; AVE_IPC*
fffffe0008c19954:  bl   0xfffffe0008c43fac           ; AVE_IPC::CheckChIntr(ch, status)
fffffe0008c19958:  cbnz w0, skip
fffffe0008c199f0:  bl   0xfffffe0008c196a0           ; ProcessIntr_IPCCh(ch)
fffffe0008c199f4:  add  w20, w20, #0x1
fffffe0008c199f8:  cmp  w20, #0x3                    ; loop ch = 1, 2
```

`AVE_IPC::CheckChIntr(_E_AVE_IPC_Ch, uint)` at `0xfffffe0008c43fac`:

```
fffffe0008c43fe0:  add  x8, x20, #0xd4       ; per-channel bit-number array
fffffe0008c43fe4:  ubfiz x9, x19, #2, #32    ; ch * 4
fffffe0008c43ffc:  ldr  w8, [x10]            ; bit = AVE_IPC[0xd4 + ch*4]
fffffe0008c44000:  lsr  w8, w2, w8
fffffe0008c44004:  tst  w8, #0x1             ; status >> bit & 1
```

### The bit numbers are not constants in the kext

`AVE_IPC::CreateChannel(int, int, unsigned long)` at `0xfffffe0008c440a0` fills
that array from a descriptor supplied by the firmware:

```
fffffe0008c441d4:  add  x21, x19, #0xd4
fffffe0008c441fc:  bl   0xfffffe0008c4208c   ; AVE_IPC_ChName2ID(char*, int*)
fffffe0008c44224:  ldr  w9, [x24, #68]       ; descriptor field +0x44
fffffe0008c4423c:  str  w9, [x10]            ; -> AVE_IPC[0xd4 + id*4]
```

`x24` walks the descriptor array passed in as argument 3; `+0x4c` of the same
descriptor is a firmware address handed to `AVE_IPC::Fw2KernelAddr`
(`0xfffffe0008c441e0`). `AVE_IPC::SetChIntr` (`0xfffffe0008c43bc8`) uses the same
number cached in the channel object at `+0x10`, and rings the doorbell with it:

```
fffffe0008c43df8:  ldr  x0, [x22, #24]      ; AVE_SVECtrl*
fffffe0008c43dfc:  ldr  w1, [x19, #16]      ; the bit number
fffffe0008c43e00:  bl   0xfffffe0008c91c38  ; SetIPCIntr(bit)
```

**So the mapping "status bit N → channel" is discovered at runtime from the
firmware's channel descriptor table, not baked into the host.** A Linux driver
must read it the same way. Recovering fixed values would mean digging into the
firmware's `RealChannelCreate(CObject*, ulong, ffwIOPChannelDescriptor*, bool, int)`
(firmware `0x273026` in the symbol list) — that was not done here.

`AVE_IPC_ChName2ID` at `0xfffffe0008c4208c` matches the descriptor's name
against a 3-entry string table at `0xfffffe0007ee0b38` (`strcmp` at
`0xfffffe0008c420cc`, loop bound `#0x3` at `0xfffffe0008c420d8`):

| id | name |
|---|---|
| 0 | `""` (empty — unused) |
| 1 | `"IO"` |
| 2 | `"IO_T2H"` |

which is exactly why `ProcessIntr` loops `ch = 1, 2`.

## 5. `ProcessIntr_*` are message handlers

`AVE_HwC::ProcessIntr_IPCCh(_E_AVE_IPC_Ch)` at `0xfffffe0008c196a0` drains the
channel:

```
fffffe0008c196d4:  bl   0xfffffe0008c45550   ; AVE_IPC::Recv(ch, &msg, &a, &b)
fffffe0008c196ec:  cmp  w20, #0x1
fffffe0008c196f0:  b.ne ...
fffffe0008c19700:  bl   0xfffffe0008c15928   ; ch 1 -> ProcessIntr_CmdAck(msg, a)
   ; ch 2:
fffffe0008c19714:  bl   0xfffffe0008c44e08   ; AVE_IPC::Send(2, msg)  (return the buffer)
fffffe0008c19724:  bl   0xfffffe0008c17c34   ; ProcessIntr_Cmd(msg, a, b)
fffffe0008c1973c:  bl   Recv again -- loop until empty
```

| channel | name | handler |
|---|---|---|
| 1 | `IO` | `AVE_HwC::ProcessIntr_CmdAck` (`0xfffffe0008c15928`) |
| 2 | `IO_T2H` | `AVE_HwC::ProcessIntr_Cmd` (`0xfffffe0008c17c34`) |

`ProcessIntr_Cmd` requires a message of at least `0x48` bytes
(`0xfffffe0008c17cdc: cmp w21, #0x47 / b.ls` → reject) and reads the opcode as a
**u16 at offset 0**:

```
fffffe0008c17efc:  ldrh w23, [x20]
fffffe0008c17f00:  sub  w8, w23, #0x1
fffffe0008c17f04:  cmp  w8, #0x14           ; valid ids are 1 .. 0x14
```

(consistent with the command-id work in
[07-commands-abi.md](07-commands-abi.md), which recovered the ids from the
firmware side.) Other header fields it touches: `+0x10` (client handle, used
with `AVE_Pipeline::FindMD` at `0xfffffe0008c18040`), `+0x20`
(`0xfffffe0008c1805c`), `+0x48` (`0xfffffe0008c18368`). Errors go to
`ProcessIntr_CmdErr` (`0xfffffe0008c187ec`); completions reach
`ProcessIntr_OutputData` (`0xfffffe0008c19430`).

### The per-engine split

`AVE_HwC::ProcessIntr_OutputData(_S_AVE_CHM*, _S_AVE_Cmd*)` at
`0xfffffe0008c179a8` switches on a field of the **channel/client context**
(`_S_AVE_CHM + 0x34`, loaded at `0xfffffe0008c179d8`), not on anything from the
interrupt:

| `CHM+0x34` | tail branch | handler |
|---|---|---|
| 2 | `0xfffffe0008c17bfc` | `ProcessIntr_LRME_OutputData` (`0xfffffe0008c16090`) |
| 3 | `0xfffffe0008c17aec` | `ProcessIntr_MCTF_OutputData` (`0xfffffe0008c16518`) |
| 4 | `0xfffffe0008c17b94` | `ProcessIntr_MSC_OutputData` (`0xfffffe0008c1675c`) |
| 5 | `0xfffffe0008c17bc8` | `ProcessIntr_GGM_OutputData` (`0xfffffe0008c16a9c`) |
| 6 | `0xfffffe0008c17a2c` | `ProcessIntr_DMV_OutputData` (`0xfffffe0008c162d4`) |
| 0, 1, ≥7 | `0xfffffe0008c17c30` | `ProcessIntr_Enc_OutputData` (`0xfffffe0008c16cdc`) |

Comparison chain at `0xfffffe0008c179dc`–`0xfffffe0008c179f8` and
`0xfffffe0008c17aac`–`0xfffffe0008c17ab8`.

Where `CHM+0x34` is set was **not** traced. It is presumably filled from the
`Open` command's parameters; that is bounded work but was not done here.

## 6. GGM and MSC

Both *do* appear in the H13C firmware, so [06-kext.md](06-kext.md)'s remark
that they are "not visible in the firmware strings" is wrong:

- `data/derived/symbols.txt` (firmware) contains
  `_gc_sAVE_DevCap_DPMMap_GGM_Erebus` at firmware `0x10719c`.
- `strings data/blobs/ave_h13c.bin` contains
  `"%s::%s:%d msc %d enc %d | %d %d | %d %d %d | %d"`.

They are two of nine work-engine families in the kext, enumerated by the
`AVE_Work_<X>_CalcEUNum` symbols: `Crypto, DMV, Enc, GGM, LACost, LAGOP, LRME,
MCTF, MSC`. Each has a full parallel set (`CalcRefNum`, `CalcSurfaceInfo`,
`ConfigEUMap`, `CalcPerfNumPerEU`, `CalcDPMThreshold`, `PreUpdate`,
`PostUpdate`), i.e. they are scheduling peers of the encoder, not sub-modes.

**GGM — what the strings actually say** (all from
`data/blobs/AppleAVE2.macho`, assertion text compiled into the kext):

```
psGGMInfo->eGGMMode == AVE_GGM_Mode_Stats || psGGMInfo->eGGMMode == AVE_GGM_Mode_Repair
psGGMInfo->iGGNum > 0 && psGGMInfo->iGGNum <= 32 && psGGMInfo->iValidGGNum > 0 ...
(psGGMInfo->iPFHWFlag & ((1 << AVE_HwType_GGM_RS) | (1 << AVE_HwType_GGM_FS) |
                         (1 << AVE_HwType_GGM_SC))) || (... AVE_HwType_GGM_Pipe)
psGGMInfo->iaBaseColorWeight[i] >= 0 && ... <= 1024 && psGGMInfo->iaSpatialWeight[i] ... <= 1024
psGGMInfo->saGGMRefInfo[i].iGGMRefIdx >= 0 && ... <= 8
```

plus "GGM Refs buffer", "GGM stats buffer", "GGM output buffer",
"GGM multipass index out of bounds", and `AVE_HwType_GGM_{RS,FS,SC,Pipe}` — the
only four `AVE_HwType_*` names present in the kext.

Facts, then: GGM is a sub-block with four hardware units (RS / FS / SC / Pipe),
two modes (**Stats** and **Repair**), up to 32 "GG"s per frame, up to 9
reference indices, per-item **base-colour** and **spatial** weights in `[0,1024]`,
and three buffers (refs, stats, output). A base-colour weight paired with a
spatial weight is the parameterisation of an edge-preserving / bilateral-style
filter, and "Stats then Repair" is an analyse-then-correct two-pass structure.
The expansion of the acronym is **unknown** and I am not going to invent one.

MSC has essentially no strings — only `AVE_Work_MSC_PostUpdate` and
`ProcessIntr_MSC_OutputData` survive as symbol names. Its expansion is
**unknown**.

**Neither is reachable on M1.** The per-SoC capability tables
`gsc_sAVE_DevCap_SEntry_<kind>_<soc>` exist as:

- `AVC_*` and `HEVC_*` for every SoC from 6000/8010 up to 8160
- `GGM_*` and `DMV_*` for **only** `8150`, `8152`, `8160`
- no `SEntry` at all for MSC, LRME or MCTF

So GGM (and a separately-advertised DMV) are capabilities of the newest silicon
only; on `t6001` no client can select them and the corresponding
`ProcessIntr_*` handlers are dead code. **Inferred** from the table names, but
the naming is unambiguous.

## 7. `AVE_HwC` state machine

Field `AVE_HwC+192`. Every store to it in the class:

| VA | value | method |
|---|---|---|
| `0xfffffe0008c19d48` | 0 | `Create` |
| `0xfffffe0008c1bf8c` | 1 | `Init` |
| `0xfffffe0008c210d8` | 2 | `PowerOn` |
| `0xfffffe0008c1cfac` | 3 | `StartUp` |
| `0xfffffe0008c208c0` | 2 | `ShutDown` |
| `0xfffffe0008c213b0` | 1 | `PowerOff` |
| `0xfffffe0008c1c674` | 0 | `Uninit` |

Interrupts are only serviced in state 3 (`0xfffffe0008c19858`).

`AVE_HwC` sub-object pointers, all stored by `Init`:

| offset | object | store VA |
|---|---|---|
| `+72` | instance index | `0xfffffe0008c1bf80` |
| `+96` | `AVE_PMGR` | `0xfffffe0008c1a0d4` |
| `+104` | `AVE_DART` | `0xfffffe0008c1a920` |
| `+112` | `AVE_Reg` | `0xfffffe0008c1abf0` |
| `+120` | `AVE_SVECtrl` | `0xfffffe0008c1a0a4` |
| `+128` | `AVE_AXI2AF` | `0xfffffe0008c1a0bc` |
| `+136` | `AVE_DPM` | `0xfffffe0008c1afa8` |
| `+144` | `AVE_DPE` | `0xfffffe0008c1b124` |
| `+152` | `AVE_MCC` | `0xfffffe0008c1b42c` |
| `+160` | `AVE_FwImg` | `0xfffffe0008c1b5a4` |
| `+176` | `AVE_FwLog` | `0xfffffe0008c1b778` |
| `+184` | `IOFilterInterruptEventSource` | `0xfffffe0008c1bf6c` |
| `+192` | state | above |
| `+200` | `AVE_IOP` | `0xfffffe0008c1bbb8` |
| `+224` | `AVE_IPC` | `0xfffffe0008c1d510` (in `StartUpIOP`) |

## 8. Bring-up order

Derived from the call order inside each method. Every step cites the `bl` it
came from.

### 8.1 `AppleAVE2Driver::start` (`0xfffffe0008cc3958`)

1. `AVE_Cfg_Get()` — `0xfffffe0008cc3a3c`
2. `AVE_Drv::Create()` — `0xfffffe0008cc3d6c`
3. `AVE_DevInfo::RetrieveSVEID(IORegistryEntry*, int*)` — `0xfffffe0008cc3ea0`
   (name and signature only; that it reads the instance id from the ADT node is
   **inferred**, not read out of the disassembly)
4. `AVE_Drv::Init(void*, const _S_AVE_Cfg*)` — `0xfffffe0008cc4038`
5. `AVE_Drv::IO_start(IOService*, uint)` — `0xfffffe0008cc427c`
6. `AVE_Drv::IO_init()` — `0xfffffe0008cc43cc`
7. `AVE_AnalyticsService_Create/Start` — `0xfffffe0008cc4ae0`, `0xfffffe0008cc4aec`
8. `AppleAVE2Driver::SetAbility(_E_AVE_DevID)` — `0xfffffe0008cc4f80`

Unwind on failure runs `IO_stop`, `IO_free`, `Uninit`, `Destroy`
(`0xfffffe0008cc4470`–`0xfffffe0008cc4490`).

### 8.2 `AVE_Drv::IO_start` (`0xfffffe0008bdafc0`), per instance

1. reject index ≥ 4 — `0xfffffe0008bdb058`
2. `AVE_HwC::Create()` — `0xfffffe0008bdb1c4`
3. create the filter interrupt event source, **index 0** — `0xfffffe0008bdb218`
4. `AVE_HwC::Init(...)` — `0xfffffe0008bdb260`

### 8.3 `AVE_Drv::IO_init` (`0xfffffe0008bd9428`)

1. `AVE_Mutex_Create` — `0xfffffe0008bd9688`
2. `AVE_DevInfo::Init(_E_AVE_DevID)` — `0xfffffe0008bd96b0`
3. `AVE_SurfaceMgr::Init(AVE_DevInfo*)` — `0xfffffe0008bd9918`
4. `AVE_GetSurfaceCfg` / `AVE_DevCap_Find` / `AVE_SurfaceMgr::CreateSurface` —
   `0xfffffe0008bd9b0c`, `0xfffffe0008bd9b1c`, `0xfffffe0008bd9b5c`
5. `AVE_Crypto::Init` — `0xfffffe0008bd9c08`
6. three `AVE_Timer_Create(IOWorkLoop*, IOTimerEventSource*)` —
   `0xfffffe0008bd9dc8`, `…dfc`, `…e30`
7. `AVE_DLList_Init` — `0xfffffe0008bd9e40`
8. `AVE_DLB::Init(cfg, AVE_DevInfo*)` — `0xfffffe0008bd9e64`

### 8.4 `AVE_HwC::Init` (`0xfffffe0008c19de8`) — state 0 → 1

Ordered `bl` sequence, dropping logging:

1. `AVE_DevInfo::GetDevType` / `GetChipType` — `0xfffffe0008c1a084`, `…090`
2. construct `AVE_SVECtrl`, `AVE_AXI2AF`, `AVE_PMGR` — `0xfffffe0008c1a0a0`,
   `…0b8`, `…0d0`
3. **`AVE_PMGR::Init(IOService*, cfg, AVE_DevInfo*, uint, AVE_SVECtrl*)`** —
   `0xfffffe0008c1a0f0`
4. **`AVE_PMGR::SetPS(PD=0, PS=2, wait=true)`** — `0xfffffe0008c1a3dc`
   (args at `0xfffffe0008c1a3d0`–`…3d8`) — power the block up before touching it
5. `AVE_DART::Init(IOService*, cfg, AVE_DevInfo*, uint, int)` —
   `0xfffffe0008c1a93c`; then `AVE_SurfaceMgr::SetDART` — `0xfffffe0008c1aa98`
6. `AVE_SurfaceMgr::EnableOp` — `0xfffffe0008c1abdc`
7. **`AVE_Reg::Init(IOService*, AVE_DevInfo*, uint, AVE_DART*, int)`** —
   `0xfffffe0008c1ac10` (maps the MMIO banks)
8. **`AVE_SVECtrl::Init(cfg, AVE_DevInfo*, uint, AVE_Reg*)`** — `0xfffffe0008c1ad74`
9. `AVE_PMGR::SetClockGating(bool)` — `0xfffffe0008c1aeb0`
10. `AVE_DPM::Init(AVE_DevInfo*, cfg, uint, AVE_PMGR*)` — `0xfffffe0008c1afc0`
11. `AVE_DPE::Init(cfg, AVE_DevInfo*, uint, AVE_Reg*)` — `0xfffffe0008c1b13c`,
    then `AVE_DPE::Reset()` — `0xfffffe0008c1b2e8`
12. `AVE_MCC::Init(IOService*, cfg, AVE_DevInfo*, uint)` — `0xfffffe0008c1b444`
13. `AVE_FwImg::Init(IOService*, cfg, AVE_DevInfo*, AVE_SurfaceMgr*, uint, AVE_DART*, int)`
    — `0xfffffe0008c1b5c8`
14. `AVE_FwLog::Init(cfg, AVE_SurfaceMgr*, uint)` — `0xfffffe0008c1b78c`
15. `AVE_AXI2AF::Init(AVE_DevInfo*, uint, AVE_Reg*)` — `0xfffffe0008c1b960`
16. `AVE_IOP::Init(cfg, AVE_DevInfo*, uint, AVE_Reg*, AVE_SVECtrl*, AVE_AXI2AF*, AVE_FwImg*)`
    — `0xfffffe0008c1bbd8`
17. `AVE_HwC::CreateFwHeartBeatTimer(IOWorkLoop*, cfg, uint)` — `0xfffffe0008c1bd38`
18. `AVE_SurfaceMgr::DARTMapSurface` — `0xfffffe0008c1bd4c`
19. `AVE_PMGR::SetPS(PD=4, PS=2, true)` — `0xfffffe0008c1bde4` — guarded by
    `cmp w26, #0x1e / b.ne` at `0xfffffe0008c1bdcc`. `w26` is set from
    `AVE_DevInfo::GetDevType()` at `0xfffffe0008c1a088`; I did not trace its
    liveness across every intervening logging block, but the identification is
    corroborated by what the guard protects: `AVE_Reg::Read32(bank=5, 0x9C000)`
    → `SetDevRevision` (`0xfffffe0008c1bf0c`, `0xfffffe0008c1bf18`), and bank 5
    only exists when `AVE_Reg::Init` mapped 6 banks, which happens only for
    `DevType == 0x1e` (`0xfffffe0008c53338`). So: skipped on M1 (DevType 10),
    which is also why only 5 banks are mapped there.
20. power back down and idle: `SurfaceMgr::DisableOp` `0xfffffe0008c1bf38`,
    `AVE_DART::SetActive(false)` `0xfffffe0008c1bf44`,
    `AVE_PMGR::SetPS(PD=0, PS=0, true)` `0xfffffe0008c1bf5c`
21. store the event source and set **state = 1** — `0xfffffe0008c1bf6c`,
    `0xfffffe0008c1bf8c`

Note step 20: `Init` leaves the hardware **powered off**.

### 8.5 `AVE_HwC::PowerOn` (`0xfffffe0008c209ac`) — state 1 → 2

1. `AVE_DPM::PowerOn()` — `0xfffffe0008c20b60`
2. `AVE_DPE::Reset()` — `0xfffffe0008c20bec`
3. `AVE_AXI2AF::ApplyTunables()` — `0xfffffe0008c20d20`
4. `AVE_AXI2AF::SetParity()` — `0xfffffe0008c20e08`
5. `AVE_DART::SetActive(true)` — `0xfffffe0008c20e90`
6. `AVE_SurfaceMgr::EnableOp()` — `0xfffffe0008c21094`
7. **state = 2** — `0xfffffe0008c210d8`

Reached from `AVE_Drv::PowerOn` (`0xfffffe0008bdd080`), itself called from
`IO_setPowerState` (`0xfffffe0008bdcd3c`), `SetPowerState` (`0xfffffe0008bde8f8`),
`ForcePowerOn` (`0xfffffe0008bdec10`) and `TurnPowerOn` (`0xfffffe0008bdf1b0`);
`TurnPowerOn` is reached from `TryPowerOn` (`0xfffffe0008bdefd0`) and
`AcquireEUC` (`0xfffffe0008be0368`) — i.e. power comes up on demand when a
client is admitted.

### 8.6 `AVE_HwC::StartUpIOP` (`0xfffffe0008c1d354`) — boot the coprocessor

1. `AVE_FwLog::Print` — `0xfffffe0008c1d458`
2. `AVE_DPM::SetIOP(_E_AVE_DPM_PL, uint)` — `0xfffffe0008c1d468`
3. `AVE_DPM::SetHw(_E_AVE_DPM_PL)` — `0xfffffe0008c1d474`
4. `AVE_FwImg::UpdateImage()` — `0xfffffe0008c1d47c`
5. construct `AVE_IPC` and
   `AVE_IPC::Init(AVE_DevInfo*, AVE_SurfaceMgr*, uint, AVE_SVECtrl*, AVE_FwImg*)`
   — `0xfffffe0008c1d50c`, `0xfffffe0008c1d528`; stored at `AVE_HwC+224`
   (`0xfffffe0008c1d510`)
6. `AVE_IPC::Alloc(0x38, &addr)` — `0xfffffe0008c1d690` (size at
   `0xfffffe0008c1d68c: mov w1, #0x38`)
7. `AVE_HwC::MakeFwCfg(_S_AVE_Fw_Cfg*)` — `0xfffffe0008c1d978`
8. **`AVE_SVECtrl::SetIOPFlag(0)`** — `0xfffffe0008c1da64`. `SetIOPFlag(i)`
   (`0xfffffe0008c91210`) writes the constant **`0x08042006`**
   (`0xfffffe0008c91258`–`0xfffffe0008c9125c`) into scratch register `i`, i.e.
   for Rhea into bank 2 + `0x18`.
9. `AVE_IPC::Kernel2DARTAddr(kaddr)` — `0xfffffe0008c1db50`
10. **`WriteScratch(1, addr_lo)`** — `0xfffffe0008c1dc7c` (bank 2 + `0x1c`)
11. **`WriteScratch(2, addr_hi)`** — `0xfffffe0008c1dd10`
    (`0xfffffe0008c1dd08: lsr x2, x21, #32`) (bank 2 + `0x20`)
12. `AVE_IOP::Config()` — `0xfffffe0008c1ddfc`
13. `AVE_IOP::Start()` — `0xfffffe0008c1dee8`

`AVE_IOP::Config` (`0xfffffe0008c4073c`) and `Start` (`0xfffffe0008c40af0`)
dispatch through the per-ChipType table at `0xfffffe0007ee07f0`. For
`t6001` (ChipType 7) that is `_Nyx`:

`AVE_IOP_Config_Nyx(AVE_Reg*, uint64 fwBase)` — `0xfffffe0008c353f0`:

| VA | access | bank | offset | value |
|---|---|---|---|---|
| `0xfffffe0008c354e0` | `Read64` | 1 | `0x50000` | (read-back for the log) |
| `0xfffffe0008c35570` | `Write64` | 1 | `0x50000` | `(fwBase & 0x3FFFFFFFF800) \| 0x0102000000000000` |

(mask at `0xfffffe0008c35558`, constant at `0xfffffe0008c3555c`.)

`AVE_IOP_Start_Nyx(AVE_Reg*)` — `0xfffffe0008c35628`, all bank 1:

| VA | offset | absolute (`ave0`) | value |
|---|---|---|---|
| `0xfffffe0008c356ec` | `0x400808` | `0x20DC00808` | `1` |
| `0xfffffe0008c35704` | `0x400044` | `0x20DC00044` | `0` |
| `0xfffffe0008c35718` | `0x400400` | `0x20DC00400` | `0x10000` |
| `0xfffffe0008c35730` | `0x400044` | `0x20DC00044` | `0x10` |

`AVE_IOP_CheckIdle_Nyx` (`0xfffffe0008c357dc`) reads bank 1 `0x400048`
(`0xfffffe0008c3589c`) and treats `value & 3 == 0` as idle
(`0xfffffe0008c358a4`). `0x0044`/`0x0048`/bit 4 match Apple's usual ASC
`CPU_CONTROL` / `CPU_STATUS` / `RUN` layout.

### 8.7 `AVE_HwC::StartUp` (`0xfffffe0008c1cd8c`) — state 2 → 3

Guard: state must be 2; state 3 is a no-op, anything else is an error
(`0xfffffe0008c1ce80`–`0xfffffe0008c1ce90`).

1. `AVE_HwC::StartUpIOP()` — `0xfffffe0008c1cea0`
2. **state = 3** — `0xfffffe0008c1cfac` (before the heartbeat starts)
3. `AVE_HwC::StartFwHeartBeatTimer()` — `0xfffffe0008c1cfb4`
4. `AVE_MCC::Enable()` — `0xfffffe0008c1d0ec`
5. `AVE_HwC::SendFwCmd_Config()` — `0xfffffe0008c1d0f4`

Failure unwind: `StopFwHeartBeatTimer` (`0xfffffe0008c1d17c`), `AVE_MCC::Disable`
(`0xfffffe0008c1d184`).

`StartUp` is entered from `AVE_Drv::StartUp` (`0xfffffe0008be38b8`) and from
`AVE_HwC::ProcessReadyCmd_StartIOP` (`0xfffffe0008c0ed60`) — so in normal
operation the coprocessor is booted lazily, by a queued `StartIOP` work item,
not by `start()`.

### 8.8 What `Prepare` and `ProcessReady` actually are

- `AVE_HwC::Prepare` (`0xfffffe0008c21ef0`) is **not** part of bring-up. Its
  only caller is `AVE_Drv::PowerOff` (`0xfffffe0008bdd340`); it stops the
  heartbeat timer (`0xfffffe0008c21fcc`) and skips queued commands
  (`AVE_CHMList_SkipAllCmds`, `0xfffffe0008c221a0`).
- `AVE_HwC::ProcessReady` (`0xfffffe0008c14104`) is the ready-queue pump:
  `AVE_DLList_Front` (`0xfffffe0008c14120`) then `ProcessReadyCmd`
  (`0xfffffe0008c14134`). `ProcessReadyCmd_Open` (`0xfffffe0008c0f06c`) is where
  a client's `Open` is finally issued.

### 8.9 Minimum sequence for a Linux driver

Confirmed-only, in order:

1. Map the ADT `reg` banks in order; bank index is used verbatim throughout.
2. Enable power/clock gates (`AVE_PMGR::SetPS(0, 2)`), then DART, then read the
   register banks.
3. Request AIC interrupt `interrupts[0]` (1031 for `ave0`) with a threaded
   handler; ignore the other four.
4. Power on: DPM on → DPE reset → AXI2AF tunables + parity → DART active.
5. Load firmware, allocate the IPC shared memory, get its DART (IOVA) address.
6. Write `0x08042006` to SVE scratch[0], the IOVA low word to scratch[1], high
   word to scratch[2] (bank 2 + `0x18`/`0x1c`/`0x20` on M1).
7. Write `(fwBase & 0x3FFFFFFFF800) | 0x0102000000000000` as a 64-bit store to
   bank 1 + `0x50000`.
8. ASC start: bank 1 `0x400808`←1, `0x400044`←0, `0x400400`←0x10000,
   `0x400044`←0x10.
9. Start the firmware heartbeat timer, then send the `Config` command.
10. In the IRQ handler: read bank 2 + `0x10`, write the same value back to clear,
    then for each IPC channel test its bit (bit numbers come from the firmware's
    channel descriptors) and drain the channel's message ring.

Only then can `Open` be accepted.

## 9. Not determined

- **Which peripheral 1024–1027 belong to.** The kext never registers them.
- **Fixed values for the IPC channel interrupt bits.** They come from the
  firmware's channel descriptor (`+0x44`) at runtime; extracting constants means
  reading `RealChannelCreate` in the firmware image, which was not attempted.
- **Where `_S_AVE_CHM + 0x34`** (the engine selector for `ProcessIntr_OutputData`)
  is written. Not traced.
- **What bank 3 (`0x8E588000`, 36 bytes) is.** No call site found here either.
- **The purpose of bank 1 `0x400808` and `0x400400`.** Values written are known;
  meaning is not.
- **The `0x0102000000000000` tag** in the `Config` write. Constant confirmed,
  meaning unknown.
- **Expansions of GGM and MSC.** Evidence in §6; no acronym expansion appears
  anywhere in either binary.
- Whether `mapDeviceMemoryWithIndex(i)` really equals ADT `reg[i]` — this is the
  one load-bearing inference in the register addresses above. It could be
  confirmed on hardware by reading bank 1 + `0x400048` (`0x20DC00048`) and
  checking it behaves like an ASC `CPU_STATUS`.
