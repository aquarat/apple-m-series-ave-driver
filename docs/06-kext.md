# AppleAVE2.kext — the host side

The firmware image shows what the coprocessor does. `AppleAVE2.kext` shows what
the *host* does, which is what a Linux driver has to replicate. It is
extractable from any IPSW with **no hardware and no hypervisor**.

## Getting it

The kernelcache is a member of the same IPSW, per SoC family. M1 Pro/Max is
`kernelcache.release.mac13j` (31.5 MB compressed):

```sh
./.venv/bin/python tools/fetch_firmware.py --url <ipsw-url>   # or fetch manually
./.venv/bin/pyimg4 im4p extract -i data/blobs/kernelcache.release.mac13j \
                                -o data/blobs/kc.macho
python3 tools/kext_extract.py data/blobs/kc.macho --list --grep ave
python3 tools/kext_classmap.py data/derived/kext-symbols.txt
```

The unwrapped kernelcache is a 118 MB `MH_FILESET` (filetype 12) with 349
embedded kexts. Note that Apple sets `LC_REQ_DYLD` on `LC_FILESET_ENTRY`, so
the command constant to match is `0x80000035`, not `0x35`.

`com.apple.driver.AppleAVE2` contributes **10,844 symbols**, of which 1198 are
AVE class methods across 91 classes. Symbols live in the kernelcache's shared
`__LINKEDIT`, so they must be resolved via the kext's own `LC_SYMTAB`, not by
slicing the fileset region.

## Architecture

| Class | Methods | Role |
|---|---|---|
| `AppleAVE2Driver` / `AppleAVE2UserClient` | 42 / 52 | IOKit entry points |
| `AVE_Drv` | 74 | top-level driver |
| `AVE_HwC` | 104 | hardware controller — command send + interrupt handling |
| `AVE_MD` | 70 | message dispatch |
| `AVE_SwC` | 83 | software controller |
| `AVE_IPC` | 19 | **host↔firmware transport** |
| `AVE_IOP` | 8 | RTKit coprocessor lifecycle |
| `AVE_FwImg` | 14 | firmware image load (incl. CTRR) |
| `AVE_DART` | 21 | IOMMU |
| `AVE_PMGR` | 23 | power/clock sequencing |
| `AVE_Surface` / `AVE_SurfaceMgr` | 47 / 19 | buffers |
| `AVE_DPB` / `AVE_GOPMgr` / `AVE_FPS` | 27 / 24 / 21 | host-side codec state |

## The transport is shared memory, not endpoint messaging

This is the most useful correction the kext provides. `AVE_IPC`:

```
Init / Uninit / GetInfo / Print
CreateChannel / DestroyChannel / AllocChannelMem / FreeChannelMem
Send / Recv / SetChIntr / CheckChIntr
Alloc / Free
Kernel2FwAddr / Fw2KernelAddr / Kernel2DARTAddr / DART2KernelAddr
UpdateFwBaseAddr
```

Channels with shared memory, address translation, and a channel interrupt —
matching the firmware's `CChannelManager`, `CRealChannel`,
`ffwIOPChannelDescriptor`, `CSharedMemoryHost` and `DoorBellRing`.

`AVE_IOP` is by contrast tiny: `Init`, `Config`, `Start`, `Stop`, `CheckIdle`.

**So RTKit boots and supervises the coprocessor, but encode traffic flows
through a shared-memory ring with a doorbell.** The RTKit mailbox endpoint
numbers therefore gate *bring-up*, not the data path, and matter considerably
less than first assumed.

## Command interface

`AVE_HwC::SendFwCmd_*` — the wire command set, 11 entries:

```
Reset  Config  Open  Start  Stop  Process  Complete  Flush  Close  Priority  Halt
```

This refines the firmware-side reading. The firmware has 16 handlers because
`Start` and `Process` split by codec and engine (`Start_AVC`/`Start_HEVC`,
`Process_AVC`/`HEVC`/`DMV`/`LRME`/`MCTF`); on the wire they are single commands
with the codec selected by a parameter. Confirmed by the builders:

```
AVE_CHM_MakeFwCmd_Open (_S_AVE_CHM*, uint64_t, uint32_t, _S_AVE_TimeOut*, sCAveCmdOpen*)
AVE_CHM_MakeFwCmd_Start_AVC  (..., sCAveCmdAvcStart*)
AVE_CHM_MakeFwCmd_Start_HEVC (..., sCAveCmdHevcStart*)
AVE_CHM_MakeFwCmd_Process_AVC(..., sCAveCmdAvcProcess*, int, AVE_PICMGMT_PARAMS*, _S_AVE_FrameInfo*)
```

> **Corrected.** An earlier revision of this document claimed `sCAveCmdOpen`
> is 120 bytes, reading the `mov w0, #0x78` in `AVE_CHM_MakeFwCmd_Open` as an
> allocation size. It is not — it is the `AVE_Log` subsystem id passed to
> `AVE_Log_CheckLevel`, the same pattern the firmware uses with subsystem
> `0x80` in `CmdProcessor`. `sCAveCmdOpen` is **72 bytes** (`0x48`), enforced
> by the firmware itself. See [07-commands-abi.md](07-commands-abi.md) for the
> confirmed sizes.

### Numeric command ids: recovered

**Resolved** — see [07-commands-abi.md](07-commands-abi.md). The firmware's
`CmdProcessor` reads a u16 id at offset 0 of the command struct and branches
through a 12-entry jump table, which gives the ids directly and independently
of the host side.

## Interrupt sources

`AVE_HwC::ProcessIntr_*` enumerates them, which should map onto the five
interrupts in the ADT node:

```
ProcessIntr_Cmd  ProcessIntr_CmdAck  ProcessIntr_CmdErr  ProcessIntr_IPCCh
ProcessIntr_Enc_OutputData   ProcessIntr_LRME_OutputData
ProcessIntr_MCTF_OutputData  ProcessIntr_DMV_OutputData
ProcessIntr_GGM_OutputData   ProcessIntr_MSC_OutputData
```

Note `GGM` and `MSC` — two engines not visible in the firmware strings.

## Other useful surface

- `AVE_HwC::StartUpIOP` / `ShutDownIOP`, `CreateFwHeap`, `AllocIPCMem`,
  `MakeFwCfg` — the full bring-up sequence, in order, in one class.
- `FwHeartBeatTimer*` — the firmware expects a periodic heartbeat
  (`CControllerHeartBeat` on the far side). A driver that omits it will
  probably see the firmware fault.
- `AVE_PMGR` has explicit dependency ordering (`SetPSDependencyUp/Down`,
  `CheckPeerUp/Down`) — the eleven power gates are sequenced, not parallel.
- `AVE_MD_Crypto` / `AVE_Crypto` — there is a crypto path (likely protected
  content); ignorable for initial bring-up.
- `AVE_MD_SVE` / `AVE_SVEDPB` (58 + 22 methods) — a second scalable/multi-layer
  encode path.

Full map: `data/derived/kext-classmap.txt`, raw symbols
`data/derived/kext-symbols.txt`.
