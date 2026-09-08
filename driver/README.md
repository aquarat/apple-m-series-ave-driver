# Driver

**UNTESTED. None of this has run on hardware, and it has not been compiled.**

The `build` symlink on the development machine points at an uninstalled
`kernel-devel`, so not even a compile check has been done. Treat every file
here as a first draft.

| File | Contents |
|---|---|
| `ave_hw.h` | MMIO banks, ASC start sequence, doorbell, power domains, SoC ids |
| `ave_abi.h` | command ids and sizes, headers, IPC ring, pixel formats, size helpers |
| `ave.h` | driver private structures |
| `ave_drv.c` | probe, power, firmware adoption, ASC start, interrupt |
| `ave_ipc.c` | shared-memory ring, doorbell, scratch handshake |

Covers steps 1–5 of [../docs/22-driver-plan.md](../docs/22-driver-plan.md).
There is no command layer and no V4L2 layer yet.

## Firmware

The driver loads the firmware itself rather than adopting an iBoot pre-load, so
no bootloader patch is needed. It requests `apple/ave_h13c.bin`: the **unwrapped
Mach-O**, not the `.im4p`.

Apple firmware is not redistributable and is not in this repository. Extract
your own from an IPSW for hardware you own:

```sh
./.venv/bin/python tools/fetch_firmware.py --board j314c --variant H13C
./.venv/bin/pyimg4 im4p extract \
    -i data/blobs/Firmware/ave/AppleAVE2FW_H13C.im4p -o data/blobs/ave_h13c.bin
sudo install -Dm644 data/blobs/ave_h13c.bin /lib/firmware/apple/ave_h13c.bin
```

Use `data/derived/board-to-ave-firmware.txt` to pick the right variant for a
different machine.

## Building

```sh
sudo dnf install kernel-devel-$(uname -r)
make -C driver
```

## Confidence

The two headers are transcriptions, each constant carrying the instruction
address it came from; they are the most trustworthy part. The `.c` files are
new logic written around them and have never been executed.

Specifically flagged as likely to be wrong on first boot:

- `ave_ipc_handshake()` — the `0x08042006` magic and the scratch register
  assignment are marked UNVERIFIED in `ave_abi.h`. This is the most probable
  first failure.
- `ave_fw_adopt()` — the `segment-ranges` cell layout is assumed to match
  DCP/ISP and has not been checked against a live FDT.
- The DMA mask width (42 bits) is a guess sized to the observed IOVAs.
- `ave_ipc_handshake()` leaves `nchannels = 0`: the second exchange that
  returns the channel descriptor array is not implemented, so no channel is
  usable yet.

## What a first boot would prove

Reaching "coprocessor up" validates, in one go: the power-domain ordering, the
four-write ASC start sequence and its idle poll, the bank-to-ADT-reg mapping,
the DART attachment, the interrupt index, and the firmware adoption contract.
That is a large fraction of the static findings tested at once, which is why
[../docs/23-empirical-bringup.md](../docs/23-empirical-bringup.md) puts it
first.
