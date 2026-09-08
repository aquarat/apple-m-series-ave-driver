# The first access is a WRITE, not a read

> ## THE REFUTATION BELOW IS ITSELF VOID (2026-09-08)
>
> The write was issued to `0x20d050038`, an undecoded hole; the correct address
> is `0x40d050038`. The posted-write hypothesis is **untested**, not refuted.
> See [30-address-translation-bug.md](30-address-translation-bug.md).
>
> ## RESULT: hypothesis refuted (2026-09-07) — WITHDRAWN
>
> Stage 7 was rebuilt to issue exactly Apple's first access — a single
> `Write32(bank 2, +0x38, 1)` and nothing else — and **it hung the machine**,
> same signature as every read: PMU `1 boot error, 0 panics`, nothing logged,
> marker correctly attributing it to `write-sve-idle`.
>
> So the read/write distinction is **not** the explanation. The AVE address
> space is dead to all traffic, in both directions. That is outcome 2 of the
> three predicted below, and it is a clean elimination: posted writes do not
> get through either.
>
> What this leaves, from the analysis in this document and
> [27-platform-enablers.md](27-platform-enablers.md):
>
> - **The fabric bridge.** `VENC_SYS` owns PMGR bridge 2, which is
>   `pmgr reg[50]` = `0x20C000000` = ave0's bank 4. If that bridge is not
>   configured, nothing behind it responds — and nothing in Linux configures
>   it, because nothing in Linux knows AVE exists.
> - **Something iBoot does only when macOS is the target OS.** Asahi boots
>   through the same iBoot, but iBoot is told which OS it is starting, and the
>   VENC fabric agent may simply be left unrouted for a non-macOS boot.
>
> Both are consistent with every observation to date, and neither can be
> distinguished without either reading the bridge window or tracing macOS.


This is the most actionable result to come out of the static analysis, and it
explains why every hardware attempt so far has failed in the same way.

## What Apple actually does first

Tracing `AppleAVE2Driver::probe`/`init`/`start` through 140 AVE functions to
the first register access gives:

```
AVE_HwC::Init                       0xfffffe0008c1aeb0
  -> AVE_PMGR::SetClockGating(true)
     -> AVE_SVECtrl::SetIdle(1)
        -> AVE_Reg::Write32(bank 2, +0x38, 1)      = 0x20D050038 on ave0
```

The offset comes from `gsc_sAVE_SVECtrl_Reg_Rhea + 0x10c` (ChipType 7 =
t6001). The same table's `+0x00` and `+0x04` entries decode to `0x0c` and
`0x10`, which match the doorbell and W1C status offsets established
independently in [08-ipc-transport.md](08-ipc-transport.md) — so the table
decode is cross-checked.

**Apple's first hardware access is a 32-bit WRITE.** Not a read.

## What we did

| Attempt | Access | Result |
|---|---|---|
| 7a | **read** bank 2 `+0x10` | hang |
| 7b | **read** bank 1 `+0x400048` | hang |
| 7c | `reset_control_reset()` (PMGR) | hang |

Every one of our attempts was a read, or a PMGR reset. We have never issued the
write Apple issues.

## Why this may be the whole answer

On an AXI-style fabric a **posted write** is fire-and-forget: the CPU does not
wait for a response. A **read** must wait for data to come back. If the VENC
fabric agent is not responding, a read stalls the core until the PMU watchdog
resets the machine — exactly the signature we see, six times: no panic, no
oops, nothing logged, `1 boot error, 0 panics`.

A write to a non-responding agent may simply be swallowed. So "reads hang" and
"writes are safe" are entirely consistent, and would mean we have been probing
the block in the one way that cannot work.

**This is a hypothesis, not a conclusion.** It is consistent with all six
failures and with Apple's own ordering, but it has not been tested.

## Corroborating findings

- **There is no hidden enable call.** The trace found 24 distinct external
  targets in the whole window; every one is allocation, locking, ktrace,
  IORegistry lookup, IOSurface plumbing, DART setup, interrupt-source creation,
  MMIO mapping, or a platform-function lookup. Nothing enables the block.
- **Apple has *fewer* power domains up than we did.** At that first write only
  `SetPS(PD_IOP, ClockOn)` has run — ADT gate 456. Our experiments had five
  domains on, a strict superset, and still hung. So the failure is not
  insufficient power.
- **macOS writes the same PS value Asahi does.** `ApplePMGR::_enableDeviceGated`
  at `0xfffffe00097f853c` selects `0xF` for on, `0x0` for off, `0x4` for the
  intermediate clock-off state. Identical to `apple-pmgr-pwrstate`.
- **`enablePsdService` (vtable +0x8c8) is not taken on t6001** — gated on a
  HwFeature bit that is 0 for devIDs 11/12/13 and nonzero for others, so the
  test discriminates.
- **`function-set_perf_state_floor` does not exist in the j314c ADT** (0
  occurrences against 7 for `function-mcc_dataset` as a positive control), so
  the whole `AVE_DPM` performance ladder is inert on M1 Max.
- **Nothing in the kext ever touches bank 3** — 0 of 169 register sites,
  consistent with it being PMGR space rather than AVE space
  ([ave_hw.h](../driver/ave_hw.h)).

## Proposed experiment

Not run. Requires operator go-ahead and a reboot into the 7.1.6 kernel the
modules are built against.

Replace stage 7 with Apple's actual first access, and nothing else:

```c
/* AVE_SVECtrl::SetIdle(1) - the first thing Apple's driver touches */
ave_write(ave, AVE_BANK_SVE, AVE_SVE_IDLE, 1);
```

Three outcomes, all informative:

1. **It survives.** Writes are safe and reads are not, which confirms the
   posted-write reading and means the whole bring-up must be re-planned around
   write-then-verify rather than probe-by-reading. Next step would be to follow
   Apple's order to the ASC start and only then attempt a read.
2. **It hangs.** The write/read distinction is not the issue and the block is
   genuinely dead to all traffic. That is a strong result too — it would point
   squarely at the fabric bridge (bank 4, PMGR-owned) or at something iBoot
   does only when macOS is the target OS.
3. It survives and a *subsequent* read still hangs — which localises the
   problem precisely to read transactions.

To reduce this to one variable, the stage should do the single write and
nothing else, with `stop_after` stopping immediately after it.
