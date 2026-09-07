# AVE hardware, as described by the Apple Device Tree

Source: `DeviceTree.j314cap.im4p` from `UniversalMac_26.6.2_25G83_Restore.ipsw`
(macOS 26.6.2), parsed with m1n1's `adt.py`. Machine: MacBook Pro 14" M1 Max,
`apple,j314c` / `apple,t6001`.

## Why the ADT and not `/proc/device-tree`

A booted Asahi system does **not** expose the ADT. `/proc/device-tree` is the
hand-written Linux DTS from `arch/arm64/boot/dts/apple/`, which m1n1 patches
with runtime values at boot. It therefore only contains nodes somebody has
already written a binding for. On this machine that includes `avd@286000000`
(`apple,t6000-avd`) but **no AVE node at all** — not because the hardware is
absent, but because no driver exists.

The ADT must therefore come from Apple's own DeviceTree image. See
[05-reproducing.md](05-reproducing.md).

## Instances

M1 Max exposes **two independent encoders**, `/arm-io/ave0` and `/arm-io/ave1`,
each with its own DART. This is consistent with Apple's claim of dual video
encode engines on M1 Max; M1 Pro (`t6000`) is expected to have one, and the base
M1 (`t8103`) one. This has not been verified against a Pro or base-M1 ADT.

Both nodes carry `compatible = "ave2"` and `device_type = "ave"`, matching the
`AppleAVE2` kext and the `AppleAVE2FW` firmware name.

Note `ave0` reports `soc-id = t6000` even on this `t6001` part.

## MMIO ranges

`ave0` has five `reg` entries. `AVE_Reg::Init` (`0xfffffe0008c532d0`) maps them
with `mapDeviceMemoryWithIndex` in a loop, caching each result in an array
indexed by position — so throughout the kext, **"bank" N means ADT `reg` entry
N**. Roles below are from call sites (see
[12-dart-surfaces-mmio.md](12-dart-surfaces-mmio.md)):

| # | Base | Size | Role |
|---|---|---|---|
| 0 | `0x20D100000` | `0x45C000` | `AVE_DPE` — confirmed |
| 1 | `0x20D800000` | `0x800000` | **ASC coprocessor block at `+0x400000`** — confirmed |
| 2 | `0x20D050000` | `0x8000`   | no call site found — unknown |
| 3 | `0x8E588000`  | `0x24`     | no call site found — unknown; **not** the `mcc_dataset` window |
| 4 | `0x20C000000` | `0x1000000`| `AVE_AXI2AF` — confirmed |

> **Corrected.** An earlier revision of this document guessed the ASC mailbox
> was `reg[2]` on the strength of its 32 KB size. It is not. The ASC is in
> `reg[1]`, and `reg[2]`'s purpose remains unknown.

### ASC start sequence (verified)

`AVE_IOP_Start_Acis(AVE_Reg*)` at `0xfffffe0008c2ab34` — `_Acis` is the variant
selected for `t6000`/`t6001`. Register helpers are
`Write32(AVE_Reg*, bank, offset, value)` at `0xfffffe0008c53e58` and
`Read32(AVE_Reg*, bank, offset)` at `0xfffffe0008c53df0`. Every access below is
bank 1; absolute addresses assume `ave0`'s base of `0x20D800000`.

| VA | access | offset | absolute | value |
|---|---|---|---|---|
| `0xc2abf8` | write | `0x400808` | `0x20DC00808` | `1` |
| `0xc2ac10` | write | `0x400044` | `0x20DC00044` | `0` |
| `0xc2ac24` | write | `0x400400` | `0x20DC00400` | `0x10000` |
| `0xc2ac3c` | write | `0x400044` | `0x20DC00044` | `0x10` |

`AVE_IOP_CheckIdle_Acis` reads `0x400048` (`0x20DC00048`) and treats the core as
idle when `value & 0x3 == 0` (`tst w0, #0x3` at `0xfffffe0008c2adbc`).

Sibling routines exist for other SoCs — `_Atlas`, `_Castor`, `_Hera` — at the
same offsets, so this sequence is not specific to `t6001`.

`ave1` mirrors this at `0x307100000`, `0x307800000`, `0x307050000`,
`0x8E680260`, `0x306000000`.

Range 2 is the first thing to probe: if the ASC mailbox is there, m1n1's
existing RTKit tracer can be pointed at it immediately.

## Interrupts, power, clocks

`ave0`: interrupts `1031, 1025, 1024, 1027, 1026` (note the first is out of
sequence — plausibly the ASC/mailbox interrupt, with the remaining four being
per-engine completion IRQs).

`ave0` has **eleven** power-gates and clock-gates: `456, 457, 458, 301, 302,
303, 304, 300, 459, 600, 602`. `ave1` uses `549, 550, 551, 368, 369, 370, 371,
367, 552, 601, 603`. A block with eleven separately gated domains is a strong
hint that the fixed-function engines (LRME, MCTF, DMV, transcode, pipe) power
up independently — power sequencing is likely to be one of the fiddlier parts
of bring-up.

`ave0` also has `function-mcc_dataset` (memory controller tuning) and
`coprovider-group = "ave"`.

## DART

`/arm-io/dart-ave0`: `compatible = "dart,t6000"`, `page-size = 16384`,
four 16 KB register banks from `0x20D040000`, single interrupt `1028`,
`sids = 32769`, with a `mapper-ave0` child.

This is an entirely ordinary Apple DART. **Linux's existing `apple-dart` driver
should handle it with no changes** — one less thing to build.

## Full dumps

- `data/derived/adt-ave-nodes.txt` — property-by-property
- `data/derived/adt-ave-full.txt` — m1n1's full node representation
