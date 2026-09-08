# Bring-up state and the remaining gap

Current as of 2026-09-08. This is the "where are we actually" document; the
per-topic docs stay as they are.

## Apple's start sequence, verified

`AVE_HwC::StartUpIOP` (`0xfffffe0008c1d354`), in address order — every entry
read from the disassembly:

| VA | call | do we? |
|---|---|---|
| `0xc1d468` | `AVE_DPM::SetIOP` | no |
| `0xc1d474` | `AVE_DPM::SetHw` | no |
| `0xc1d47c` | `AVE_FwImg::UpdateImage` | effectively yes |
| `0xc1d50c` | `AVE_IPC` constructor | n/a |
| `0xc1d528` | `AVE_IPC::Init` | partly |
| `0xc1d690` | `AVE_IPC::Alloc` | yes (`dma_alloc`) |
| `0xc1ddfc` | `AVE_IOP::Config` | **yes** |
| `0xc1dee8` | `AVE_IOP::Start` | **yes** |
| `0xc1dfe4` | `AVE_SVECtrl::RecvIOPMsg` | not yet |

Two things this settles:

- **`AVE_IPC::Init` runs before Config and Start**, and it does *not* perform a
  handshake. Its calls are `AVE_GetSurfaceCfg(FwIPC)`,
  `AVE_SurfaceMgr::CreateSurface`, `GetDARTAddr`/`GetSize`, and finally
  `AVE_FwImg::GetBaseAddr`. It allocates the 20 MiB shared region and works out
  the firmware base. Nothing is written to scratch registers.
- **Apple waits for the firmware to speak first.** The only call after
  `AVE_IOP::Start` is `RecvIOPMsg`. So the boot handshake is firmware-initiated;
  the host does not poke it awake.

`AVE_FwImg::UpdateImage` calls only `RestoreCTRRData` (restore the pristine
DATA segment) and `UpdateBufImage`, which `docs/09` established is dead code
because `AVE_FwImg_FindMap` is a stub. We copy the whole image fresh on every
load, so this is covered.

`AVE_DPM::SetIOP`/`SetHw` set performance levels. `docs/28` found
`function-set_perf_state_floor` is absent from the j314c ADT and
`AVE_PMGR::SetPerfState` bails on the null check, so the DPM ladder is inert on
M1 Max. Believed not required; not proven.

## What the hardware does

All of this is reproducible, on a machine that no longer crashes:

| step | result |
|---|---|
| map five banks | OK |
| DMA mask, IRQ, power attach, resume | OK |
| write `SVE+0x38` (Apple's first access) | OK |
| read `ASC+0x400048` | `CPU_STATUS = 0x2a` |
| DART bind | both instances init; `iommu group 16` |
| 20 MiB `dma_alloc_coherent` | `iova 0xfe000000` |
| firmware load, contiguous, `iommu_map` at IOVA 0 | OK, `0x268000` bytes |
| `Write64(bank1, 0x50000, tag)` | readback `0x0102010000b28001` |
| ASC start sequence | OK |
| **after start** | `CPU_STATUS 0x2a -> 0x2c`, scratch[0..7] all zero and static |

So: the coprocessor's state register **changes** when we start it, but nothing
else moves. Apple's own next step is to wait for a message that never arrives.

The `0x50000` readback is worth noting — we wrote `0x0102000000000000` and read
back `0x0102010000b28001`. The tag survives; the hardware populates the low
bits. That offset is a live status register, not storage.

## Why the firmware may not be running

Ranked by how well each fits the evidence. None is established.

1. **Stream ID / bypass.** The ADT gives `dart-ave0` `sids = 0x8001` (SIDs 0 and
   15) and `bypass = 0x8000` (SID 15 in bypass). We map firmware at IOVA 0 on
   SID 0. If the coprocessor fetches instructions on SID 15, then under Apple it
   is in bypass — physical addressing — and `apple_dart_hw_reset` strips bypass
   on probe, so the fetch would translate through an empty page table instead.
   This is the best fit: the core starts, faults on its first fetch, and goes
   quiet. Testable by mapping SID 15 as well, or by checking for DART faults.
2. **`AVE_DPM::SetIOP`/`SetHw`.** Believed inert on this SoC, but unproven, and
   they are the only calls in `StartUpIOP` we skip outright.
3. **Memory attributes.** `dma_alloc_attrs` gives non-cacheable coherent memory.
   An RTKit image may expect normal cacheable memory, and the DATA segment
   certainly wants to be writable-back.
4. **Something in `AVE_IOP::Config` beyond the `0x50000` write.** We implement
   the one write we found; `AVE_IOP_Config_Nyx` is larger than that.

## What is left to do generally

- Resolve the above and get the firmware to send its first message.
- Implement `RecvIOPMsg` and the channel setup that follows it.
- The command layer: `docs/07`/`20` have the ids, sizes and header; the
  per-frame `AVE_PICMGMT_PARAMS` fields are still unmapped.
- V4L2 M2M glue.
