# Building the driver

What a Linux AVE driver consists of, what this repository already supplies, and
what is still missing. The constants are in `driver/ave_hw.h` and
`driver/ave_abi.h`; the device tree is in `dts/`.

## Status, 2026-09-08: the hardware responds

Stages 1-10 pass on real hardware, at the corrected addresses
([30](30-address-translation-bug.md)):

| stage | result |
|---|---|
| 1-6 map banks, DMA mask, IRQ, power attach, resume | OK, repeatable |
| 7 write `SVE+0x38` (Apple's first access) | OK |
| 8 read `ASC+0x400048` | **`CPU_STATUS = 0x0000002a`** |
| 9 ASC start sequence + idle poll | OK - the poll succeeded |
| 10 read `SVE+0x10` | **`intr status = 0x00000000`** |

Stage 9 returning OK means the four-write start sequence ran and
`(CPU_STATUS & 3) == 0` was reached within the timeout, so the block accepts
the documented start sequence and transitions as expected.

**What this does not yet establish:** that the coprocessor is *executing*
anything. No firmware has been loaded, so the core has nothing to run. "Idle"
here means the control block reports idle, not that a running firmware is
quiescent.

Next blocker is firmware delivery - see below.

## Shape

A V4L2 **stateful** M2M encoder. That follows from the firmware owning rate
control, GOP structure, DPB and reference management, and bitstream header
generation ([03](03-protocol.md), [06](06-kext.md)) — the driver submits frames
and receives a legal bitstream, it does not make encode decisions. FFmpeg and
GStreamer already speak this interface, so no new userspace is needed.

## Components

| Component | What we have | Status |
|---|---|---|
| DT binding + nodes | `dts/apple,ave.yaml`, `dts/t6001-ave.dtsi` | draft; power-domain phandles unresolved |
| IOMMU | ordinary `dart,t6000` | **`apple-dart` as-is** ([12](12-dart-surfaces-mmio.md)) |
| Power | 11 gates by ADT index, no MMIO, ordering known | **`apple-pmgr-pwrstate` as-is** ([10](10-power.md)) |
| Firmware load | iBoot pre-loads; adopt via `segment-ranges`; IOVA 0 | contract known ([09](09-firmware-load.md)) |
| Coprocessor start | 4 register writes + idle poll, exact | `ave_hw.h` ([01](01-hardware.md)) |
| IPC transport | ring layout, doorbell, phase-bit sync | `ave_abi.h` ([08](08-ipc-transport.md)) |
| Interrupt | one AIC irq, status reg, W1C ack | `ave_hw.h` ([11](11-interrupts-bringup.md)) |
| Command layer | 11 ids, sizes, 64-byte header | `ave_abi.h` ([07](07-commands-abi.md)) |
| Buffer sizing | frame formulas, surface set, 64-byte stride rule | [14](14-frame-size-formulas.md)–[17](17-aux-engines-pools.md) |
| Command interiors | header only | **missing** |
| Coded-output size | not a closed form | **missing** |
| V4L2 glue | — | not started |

Three of the hardest pieces — IOMMU, power, and RTKit-style coprocessor
lifecycle — need no new Apple-specific code. That is the main reason this looks
tractable.

## Bring-up sequence

Assembled from `AVE_HwC::Init` / `StartUpIOP` and `AVE_IOP_Start_Nyx`. The state
machine at `AVE_HwC+192` is Create 0 → Init 1 → PowerOn 2 → StartUp 3, and
interrupts are only serviced in state 3.

```
probe:
    map reg[0..4]                        /* bank == ADT reg index */
    pmgr: SetPS(IOP, CLOCK_ON)           /* first hardware action, before DART */
    attach DART, request irq at ADT index 0
    adopt firmware via segment-ranges, map at IOVA 0
    snapshot the DATA segment            /* restored before every start */

power on:
    walk the domain tree from IOP:
        perf     IOP -> IOP_Mid -> IOP_Mid2 -> IOP_Max
        datapath IOP -> DMA/FE -> Pipe4/HME (AVC) -> ME0 -> ME1
        FAB independently

start coprocessor:                       /* all in bank 1 */
    write32(0x400808, 1)
    write32(0x400044, 0)
    write32(0x400400, 0x10000)
    write32(0x400044, 0x10)
    poll  read32(0x400048) & 3 == 0

handshake:                               /* bank 2 scratch registers */
    scratch[0] = 0x08042006
    scratch[1..2] = IPC surface IOVA
    write firmware base to bank1 + 0x50000
    read back the firmware base the firmware reports  /* arch 0x40 */
    allocate channel memory, second exchange yields the descriptor array
    create channels 1 ("IO") and 2 ("IO_T2H")

then:
    AVE_CMD_CONFIG                       /* 0x78 */
    AVE_CMD_OPEN                         /* 0x48, allocates a client_id */
    AVE_CMD_START  (enc_type = AVC)      /* 0x3180, publishes all buffers */
    AVE_CMD_PROCESS per frame            /* 0x63D8 */
    AVE_CMD_COMPLETE / AVE_CMD_CLOSE
```

The heartbeat can be omitted: it only *reads* a scratch counter, so skipping it
costs wedge detection and nothing else ([09](09-firmware-load.md)).

## Sending a command

1. Allocate the struct from the FwIPC region — 64-byte granule, 64-byte
   alignment.
2. Fill `struct ave_cmd_hdr`; `id`, `client_id`, `count`, `slot` (< 51),
   `enc_type`/`work_type`.
3. Write the slot: `{payload_fw_addr | phase, arg1, arg2}` where the address is
   in firmware space (`Kernel2FwAddr`).
4. Ring: `write32(bank2 + 0x0C, 1 << channel_doorbell_bit)`.
5. On interrupt: read `bank2 + 0x10`, write the same value back to clear,
   dispatch on the channel bit, then drain the T2H ring.

Sizes are firmware-enforced — a wrong length is rejected outright, which makes
early mistakes loud rather than silent. That is useful.

## Buffers

Input frames, per [14](14-frame-size-formulas.md)/[15](15-surface-layout.md):

- two planes only; a third would alias onto plane 1
- **`bytesPerRow` must be a non-zero multiple of 64 on both planes** — enforced,
  `-1015`. This is the one hard constraint to impose on userspace.
- no height or plane alignment found on any executed path
- `420v` at 1920x1080: 3,110,400 B, Y stride 1920 at 0, CbCr stride 1920 at
  `0x1FA400`

Internal surfaces the driver must allocate itself ([16](16-encode-surface-set.md)):
`Recon` (3,133,440 B at 1080p), `Colocated` (1,044,480), `MBStats` (3,526,656),
`MBInputCtrl` (130,560), `CodedHeader` (49,152), plus the LRME scratch surfaces,
which the encode path allocates without a separate LRME submission
([17](17-aux-engines-pools.md)). Kext-allocated surfaces are linear blobs
rounded to 16 KB.

## What still blocks first light

1. **Firmware delivery.** `ave_fw_adopt()` needs `segment-ranges` on the AVE
   node, and m1n1 does not emit an AVE node at all, so Linux has no firmware
   region. This is now the immediate blocker and it needs an m1n1 patch rather
   than a driver change: `dt_reserve_asc_firmware()` is generic and is already
   called for ISP. Doing so also answers the open question of whether iBoot
   pre-loads AVE firmware on a non-macOS boot, which
   [09-firmware-load.md](09-firmware-load.md) assumes but has never verified.
2. **Coded-output buffer size.** Not a closed form; rate-control dependent.
3. **Buffer publication.** `AVE_CHM_SetFwBuf` writes `{dartAddr, size}` entries
   into the Start command at `+0x4e8`; the slot-to-surface table is only
   partly mapped, and how `Process` references a buffer per frame is unknown.
4. **Power-domain phandles.** The ADT gives gate indices; they must be resolved
   against the t6001 PMGR nodes.

(1)–(3) are being worked; all three are static analysis, not hardware.

## Unknowns that are probably survivable

- `reg[3]` (36 bytes) — no call site found by anyone. Map it and ignore it.
- ADT interrupts 1024–1027 — unclaimed by Apple's own driver.
- `Reset`'s 81,672-byte payload — `Reset` may not be needed for a first encode.
- `AVE_SVE_REG_08`, the `0x400400`/`0x400808` writes and the `0x0102…` tag in
  the firmware-base write. Reproduce them verbatim; understanding can wait.

## Testing

The validation loop is much friendlier than a decoder's. A decoder must match a
reference bit-exactly; an encoder only has to emit a *legal* bitstream, and
`ffmpeg` gives ground truth on every attempt:

```sh
ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i in.yuv \
       -c:v h264_v4l2m2m out.h264
ffmpeg -v error -i out.h264 -f null -      # must be silent
```

First light is fixed-QP, I-frames only, one resolution. It does not need to look
good. `ConstantQpRateControl` exists in the firmware
([03](03-protocol.md)), so that path does not require defeating rate control.
