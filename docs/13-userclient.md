# AppleAVE2UserClient — the userspace ABI

This is the VideoToolbox↔kernel boundary. It does not constrain a Linux driver
directly (Linux will expose V4L2 instead), but it is the clearest statement of
the session lifecycle Apple's stack actually uses, and it fixes the sizes of the
configuration structures. It is also the surface iOS security researchers have
published on, so it is the easiest place to cross-check outside sources.

Everything below is **confirmed** — read out of the dispatch table and the
disassembly — unless marked otherwise.

## Dispatch

`AppleAVE2UserClient::externalMethod` at `0xfffffe0008cb4b78`:

```
fffffe0008cb4bb4:  sub  w8, w22, #0x1        ; selector - 1
fffffe0008cb4bb8:  cmp  w8, #0x9             ; reject unless <= 9
fffffe0008cb4bbc:  b.hi 0xfffffe0008cb4c20   ; -> return 0xe00002c7
fffffe0008cb4bc0:  mov  w8, #0x18            ; 24 = sizeof(IOExternalMethodDispatch)
fffffe0008cb4bc4:  umull x8, w22, w8         ; selector * 24
fffffe0008cb4bc8:  adrp x9, 0xfffffe0007ee1000
fffffe0008cb4bcc:  add  x9, x9, #0xda0       ; table base 0xfffffe0007ee1da0
```

So **valid selectors are 1–10**. Note the index is `selector * 24`, not
`(selector - 1) * 24`, so entry 0 exists in the table but is unreachable; it is
all zeroes. Out-of-range returns `0xe00002c7` (`kIOReturnUnsupported`), set at
`0xfffffe0008cb4c20`.

The dispatch table lives in `__DATA_CONST` at VA `0xfffffe0007ee1da0`
(file offset `0xeddda0`). Its function pointers are chained-fixup encoded; the
low 32 bits hold the image-relative **file offset**, not a VA — verified by
matching each against the kext's symbol addresses.

## The ten methods

`IOExternalMethodDispatch` is `{function, checkScalarInputCount,
checkStructureInputSize, checkScalarOutputCount, checkStructureOutputSize}`.
**Every method takes zero scalars in and zero scalars out** — all traffic is
structure input/output.

| Sel | Method | struct in | struct out |
|---:|---|---:|---:|
| 1 | `IO_Open` | 2,336 (`0x920`) | 40 (`0x28`) |
| 2 | `IO_Close` | 40 (`0x28`) | 32 (`0x20`) |
| 3 | `IO_Config` | 56 (`0x38`) | 32 (`0x20`) |
| 4 | `IO_Prepare` | **106,656** (`0x1a0a0`) | 32 (`0x20`) |
| 5 | `IO_Start` | **106,672** (`0x1a0b0`) | 336 (`0x150`) |
| 6 | `IO_Stop` | 48 (`0x30`) | 32 (`0x20`) |
| 7 | `IO_Process` | 48 (`0x30`) | 32 (`0x20`) |
| 8 | `IO_Complete` | 48 (`0x30`) | 32 (`0x20`) |
| 9 | `IO_Flush` | 48 (`0x30`) | 32 (`0x20`) |
| 10 | `IO_Reset` | 48 (`0x30`) | 32 (`0x20`) |

Observations:

- The steady-state commands (`Stop`, `Process`, `Complete`, `Flush`, `Reset`)
  share an identical 48-in / 32-out shape, which is almost certainly a common
  command header. *Inferred* — the field layout has not been read.
- `Prepare` and `Start` carry a ~104 KB structure, differing by exactly 16
  bytes. That is the encoder/session configuration blob. Its size alone tells
  you this is not a handful of parameters; it is a large descriptor, plausibly
  including per-frame or per-slice tables.
- `Start` is the only method returning more than 32 bytes (336). *Inferred:*
  capability or handle information returned to userspace at session start.
- `Open` at 2,336 bytes in / 40 out.

## Not exposed to userspace

`AppleAVE2UserClient` also implements `IO_Print`, `IO_PowerOn`, `IO_PowerOff`,
`IO_StartUp` and `IO_ShutDown`, but **none of them appear in the dispatch
table**. They are internal entry points driven by the driver and power
management, not callable from userspace.

## Relationship to the firmware command set

The userspace lifecycle lines up with the wire command set in
[06-kext.md](06-kext.md) but is not identical:

```
userspace :  Open Config Prepare Start Process Complete Flush Stop Close Reset
wire      :  Open Config         Start Process Complete Flush Stop Close Reset Priority Halt
```

`Prepare` exists only on the userspace side; `Priority` and `Halt` only on the
wire side. Consistent with `AVE_HwC` doing host-side buffer preparation before
anything is sent to the firmware.

As with the wire protocol, **there is no separate AVC/HEVC path here** — codec
selection is a parameter inside the configuration structure, not a distinct
entry point.

## What is still unknown

- The field layout of every one of these structures. Sizes are confirmed;
  contents are not. Recovering them means disassembling the `IO_*` handlers,
  which unpack the input struct into `_S_AVE_Cmd` and friends.
- Which fields of the ~104 KB `Prepare`/`Start` blob are the encoder parameters
  a V4L2 driver would need to synthesise.

Cross-referencing published `AppleAVE2Driver` exploitation write-ups from the
iOS security community may shortcut the struct layouts, since that work
necessarily documented these same input structures. Not yet attempted.
