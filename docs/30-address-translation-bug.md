# The address-translation bug: eight experiments measured nothing

> ## CONFIRMED BY THE FIX, 2026-09-08
>
> With the corrected addresses, stages 1-7 all pass and the machine stays up:
>
> ```
> apple-ave 40d100000.video-encoder: stage 5 (power-attach): OK  (5 domains)
> apple-ave 40d100000.video-encoder: stage 6 (power-on): OK
> apple-ave 40d100000.video-encoder: stage 7 (write-sve-idle): starting
> apple-ave 40d100000.video-encoder:   writing 1 to SVE+0x38 ...
> apple-ave 40d100000.video-encoder:   write returned
> apple-ave 40d100000.video-encoder: stage 7 (write-sve-idle): OK
> ```
>
> That write is Apple's first register access, at `0x40d050038`. The identical
> write to `0x20d050038` hung the machine. **The address translation was the
> entire problem.** There was never anything wrong with the AVE block, the
> power domains, the fabric, the bridge, or iBoot.
>
> Stage 6 also now passes repeatably, confirming that its "non-determinism" was
> the overlay regression and nothing physical.

Found by an independent review, 2026-09-08. Verified before acting on it.

## What was wrong

Nodes under `/arm-io` in the ADT carry **bus** addresses. The `/arm-io` node's
`ranges` property maps them into parent (CPU physical) space:

```
bus 0x000000000000 -> parent 0x000200000000  size 0x400000000
bus 0x000580000000 -> parent 0x000580000000  size 0x180000000
bus 0x000700000000 -> parent 0x000700000000  size 0xf80000000
```

Essentially every peripheral address therefore needs **`+0x2_00000000`**.
Positive controls from the live device tree on this machine:

| ADT node | ADT `reg[0]` | Linux node |
|---|---|---|
| `avd0` | `0x86000000` | `avd@286000000` |
| `isp0` | `0x184000000` | `isp@384000000` |
| `dart-avd0` | `0x87010000` | `iommu@287010000` |

**AVE's addresses were used raw.** The overlay, the DTS and `ave_hw.h` all said
`0x20d100000` where the hardware is at `0x40d100000`. The stage-5 log confirms
what actually ran: `apple-ave 20d100000.video-encoder`.

There is no device at `0x20d1xxxxx`. Every AVE access in all eight bring-up
attempts went to an **undecoded hole**, which on Apple silicon hangs the fabric
with no fault and nothing logged — indistinguishable from a device that will
not answer.

The translation *was* applied correctly in one place: `test/psdump` used
`0x28e588000` for the PMGR registers and worked. The single address that was
right is the single one that responded. That inconsistency should have been the
tell.

## The second bug

`test/Makefile` built `ave_overlay_dtbo.h` from `ave-overlay.dts` — the
DART-inclusive overlay — not from `ave-overlay-nodart.dts`. Decoding the
committed header at each commit shows the DART node silently returning from
commit `c2a48a9` onward, so from the fourth attempt every run applied a DART at
a hole address, whose driver programs registers asynchronously after `insmod`
returns.

The tracked generated header is how it hid. It is now gitignored, the Makefile
builds the no-DART source, and the DART overlay is renamed `.disabled`.

## What this voids

Everything in [25-bringup-results.md](25-bringup-results.md) derived from
hardware, specifically:

- "reads hang" — read an undecoded hole
- "writes hang" ([29](29-first-access-hypothesis.md)) — wrote an undecoded hole
- "reset hangs" — the overlay had no `resets` property at that commit, so
  `devm_reset_control_get_optional` returned NULL and no reset was ever issued
- "stage 6 is non-deterministic" — deterministic after all: the two passes used
  the no-DART header, the failure used the DART header
- every "hypothesis eliminated" that rested on those observations

**Zero facts about AVE were established by the hardware work.** What was
established was two facts about the harness. The static analysis is unaffected;
none of it depends on these runs.

## Corrected addresses

From `tools/check_addrs.py`, which positive-controls the translation against
`avd0`, `isp0` and `dart-avd0` before printing:

| | bus (ADT) | physical |
|---|---|---|
| ave0 bank 0 (DPE) | `0x20d100000` | **`0x40d100000`** |
| ave0 bank 1 (ASC) | `0x20d800000` | **`0x40d800000`** |
| ave0 bank 2 (SVE) | `0x20d050000` | **`0x40d050000`** |
| ave0 bank 3 (PMGR PS) | `0x8e588000` | **`0x28e588000`** |
| ave0 bank 4 (bridge) | `0x20c000000` | **`0x40c000000`** |
| dart-ave0 | `0x20d040000`… | **`0x40d040000`**… |
| ave1 bank 0 | `0x307100000` | **`0x507100000`** |

So ASC `CPU_CONTROL` is `0x40dc00044`, the doorbell is `0x40d05000c`, and the
write that was tested as "Apple's first access" should have gone to
`0x40d050038`.

## Why it was not caught

The project verified every constant extracted from Apple's binaries against
those binaries, repeatedly and successfully. It never verified the apparatus.
[00-methodology.md](00-methodology.md)'s own Trap 2 — check that a test can
distinguish the case you care about — was applied to disassembly and never to
the experiment, even though a known-good control (AVD) was sitting on the same
SoC the whole time.

Recorded as Trap 7.
