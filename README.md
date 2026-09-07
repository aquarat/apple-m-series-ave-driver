# apple-ave-driver

Reverse-engineering notes and tooling for **AVE**, the Apple Video Encoder block
on Apple Silicon, with the goal of a Linux V4L2 encoder driver.

AVE is the counterpart to AVD (the decoder, which Asahi already supports). As of
this writing there is no public AVE work in Asahi — the encoder is untouched.
This repository is the starting point for that work.

**Target hardware for the initial effort:** Apple M1 Max (`t6001`, board `j314c`),
which exposes two independent encoder instances (`ave0`, `ave1`). The approach
should generalise across the M1 family and forward.

## Status

Static reconnaissance only. **No code has been run on the hardware, and no
driver exists yet.** Everything here was derived from Apple's own shipped
firmware and device tree.

| Question | Answer | Confidence |
|---|---|---|
| Is AVE present and enumerable? | Yes — `/arm-io/ave0`, `/arm-io/ave1` | Confirmed |
| IOMMU | `dart,t6000`, 16 KB pages — Linux `apple-dart` already handles it | Confirmed |
| Firmware format | arm64e Mach-O, `MH_PRELOAD`, 2.6 MB | Confirmed |
| Firmware runtime | **RTKit** (`RTKit-3255.160.4.release`), ASC 128-bit mailbox | Confirmed |
| Where is rate control? | In firmware (`CRateControl`, `CAVEMultiPass`) | Confirmed |
| Driver model | V4L2 **stateful** M2M encoder | Follows from the above |
| Firmware command handlers | 16 recovered (`ProcessCmd_*`) | Names confirmed |
| Host↔fw data path | Shared-memory channels (`AVE_IPC`), not endpoint messaging | Confirmed |
| Wire command set | 11 commands (`AVE_HwC::SendFwCmd_*`) | Confirmed |
| Numeric command ids | 1–12, from the firmware jump table (5 unused) | Confirmed |
| Command struct sizes | `0x48` for most, `0x78` Config, `0x13F08` Reset | Confirmed |
| Userspace ABI | 10 IOKit selectors, exact struct sizes | Confirmed |
| SoC identification | ADT `soc-id` -> DevID -> ChipType -> variant (`t6001`=`_Nyx`) | Confirmed |
| Firmware variant | `H13C` for M1 Max, from `BuildManifest.plist` | Confirmed |
| Firmware load | iBoot pre-loads; kext adopts via `segment-ranges`, IOVA 0 | Confirmed |
| Interrupts | **one** AIC irq (ADT index 0); the rest is software dispatch | Confirmed |
| Heartbeat | 3 s poll of a scratch counter; writes nothing — omittable | Confirmed |
| MMIO bank mapping | bank N = ADT `reg` index N; ASC is bank 1 `+0x400000` | Confirmed |
| Doorbell | bank 2 `+0x0C`, write `1 << chan_bit`; status W1C at `+0x10` | Confirmed |
| IPC ring | 20 MiB `FwIPC` surface, 3 channels, `0x40` slots, phase-bit sync | Confirmed |
| ASC start sequence | 4 register writes + idle poll, exact values | Confirmed |
| Input pixel formats | NV12-style `420v`/`420f` **and** Interchange (lossless) | Confirmed |
| Frame size formulas | all four layout primitives transcribed | Confirmed |
| `420v` @ 1920x1080 | 3,110,400 B; Y stride 1920 @ 0; CbCr stride 1920 @ `0x1FA400` | Confirmed |
| **Input stride constraint** | **non-zero multiple of 64 on both planes** (enforced, `-1015`) | Confirmed |
| Plane count | 2 planes only; a third would alias onto plane 1 | Confirmed |
| Kext-allocated surfaces | linear blobs, `align_up(size, 16 KB)`, no geometry keys | Confirmed |
| FwIPC allocator | ChkPool buddy allocator, 64-byte granule and alignment | Confirmed |
| Encode surface set | 35-slot InfoSet; 19 internal + 7 out allocations | Confirmed |
| `Recon` @ 1920x1080 | 3,133,440 B; DPB hard max 17 frames | Confirmed |
| Coded-output size | **not a closed form** — rate-control dependent | Open |
| DART | kext uses IOKit mapper only; `apple-dart` + DMA API suffices | Confirmed |
| Power | no MMIO; `apple-pmgr-pwrstate` suffices. 11 domains, order known | Confirmed |
| `ave0` / `ave1` coupling | none via power management — independent | Confirmed |
| Mailbox endpoint ids | **Unknown** — gates bring-up only, not the data path | Open |

Two findings matter most. **AVE runs RTKit**, so m1n1's tracer and Linux's
`apple-rtkit` apply directly and the coprocessor lifecycle is existing
infrastructure. And **`AppleAVE2.kext` is statically analysable from the same
IPSW**, giving the host half of the protocol — 1198 named methods — without
hardware. Together these mean the project is not blocked on hypervisor
tracing, which was the original assumption.

**Before adding findings, read [docs/00-methodology.md](docs/00-methodology.md)** —
it records the standing evidence rule and five traps in these binaries, each of
which has already produced a wrong result here that a later commit had to
correct.

Both sides of the protocol are available for static analysis: the firmware
image ([docs/02-firmware.md](docs/02-firmware.md),
[docs/03-protocol.md](docs/03-protocol.md)) and `AppleAVE2.kext`
([docs/06-kext.md](docs/06-kext.md)), which contributes 1198 named AVE methods.
Hardware and a hypervisor trace are **not** required to make further progress.

See also [docs/01-hardware.md](docs/01-hardware.md) and
[docs/04-roadmap.md](docs/04-roadmap.md).

## Licensing and blobs

**No Apple firmware is committed to this repository, and none may be.**
`data/blobs/` is gitignored. `tools/fetch_firmware.py` extracts the blobs
locally from Apple's own distribution; each user fetches their own copy for
hardware they own. Only *derived facts* — symbol names, register addresses,
node properties — live in `data/derived/`, since those are interoperability
information rather than redistributed code.

## Quick start

```sh
python3 -m venv .venv && ./.venv/bin/pip install -r requirements.txt
git clone --depth 1 https://github.com/AsahiLinux/m1n1 m1n1-src   # ADT parser

./.venv/bin/python tools/fetch_firmware.py --board j314c --variant H13C
./.venv/bin/pyimg4 im4p extract -i data/blobs/Firmware/ave/AppleAVE2FW_H13C.im4p \
                                -o data/blobs/ave_h13c.bin
./.venv/bin/pyimg4 im4p extract -i data/blobs/Firmware/all_flash/DeviceTree.j314cap.im4p \
                                -o data/blobs/adt.bin

python3 tools/extract_protocol.py data/blobs/ave_h13c.bin
./.venv/bin/python tools/adt_dump.py data/blobs/adt.bin --grep ave

# host side: pull AppleAVE2.kext out of the kernelcache
python3 tools/kext_extract.py data/blobs/kc.macho --list --grep ave
python3 tools/kext_classmap.py data/derived/kext-symbols.txt
```

Full reproduction steps, including why the ADT cannot simply be read from a
booted Linux system, are in [docs/05-reproducing.md](docs/05-reproducing.md).

## Building a driver

[docs/22-driver-plan.md](docs/22-driver-plan.md) maps the findings onto the
components a Linux driver needs, with the bring-up sequence and what still
blocks first light.

The confirmed constants are available as compilable headers:

- `driver/ave_hw.h` — MMIO banks, ASC start sequence, doorbell, power domains
- `driver/ave_abi.h` — command ids and sizes, the 64-byte header, IPC ring,
  pixel-format enums, the 64-byte stride rule
- `dts/apple,ave.yaml`, `dts/t6001-ave.dtsi` — device tree binding and nodes

Every constant carries the instruction address it was read from, so any of them
can be re-checked in one command. Three of the hardest pieces need no new
Apple-specific code: `apple-dart` covers the IOMMU, `apple-pmgr-pwrstate` covers
power, and the coprocessor is ordinary RTKit.

## Layout

```
docs/            findings, methodology and the driver plan
driver/          C headers of confirmed constants
dts/             device tree binding and node fragments
tools/           extraction and analysis scripts
data/derived/    committed: symbols, command tables, ADT dumps (facts)
data/blobs/      gitignored: Apple proprietary firmware and device tree
```
