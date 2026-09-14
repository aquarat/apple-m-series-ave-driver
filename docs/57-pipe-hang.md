# The F5 `PIPE HANG`: what the heartbeat says, what completes the pipe, and why it does not complete

Static analysis prompted by `results/f5-1789392104.kmsg` (2026-09-14 14:21,
commit `74ba0ff`): Config/Open/Start_AVC/Process accepted, datapath DMA mapped
on both DARTs, **no DART/SMMU fault, no AXI error**, then
`Uncompress Ref is not supported` (kmsg l.1000 area, right after Process) and,
every second from +2 s, `Controller Heart Beat ERROR: PIPE HANG: 1, 1`,
`ENC: StartCount 1-1-1-0, Idle 1-1-0-1`, `Pipe 1: ...`, `xcode 1: ...`.

All firmware VAs are **macOS 13.5** image VAs (`AppleAVE2FW-6070.11.1`, file
offset = VA + `0x4000`); kext VAs are 13.5 kernelcache VAs. Reproduce with

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw   --addr <VA> -n <LEN>
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr <VA> -n <LEN>
```

Labels per [00](00-methodology.md): **[C]** read from an instruction (VA cited),
**[I]** inferred (chain stated), **[U]** unknown. Nothing here was run on
hardware. Where a statement comes from the 26.6.2 blobs it says so.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| `ENC: StartCount a-b-c-d` | trigger counts of **LRMEFS, LRMERC, PIPE, XCODE** (`this+0xC9C/CA0/CA4/CA8`) | C |
| `Idle a-b-c-d` | `CFSM::Get(FSM) == 0` for the same four stages | C |
| `1-1-1-0 / 1-1-0-1` | LRMEFS and LRMERC were triggered once and are back in state 0 (finished); PIPE was triggered once and is **still in state 1 (started, no done event)**; XCODE never triggered | C |
| `PIPE HANG: 1, 1` | pipe queue non-empty, pipe FSM state 1, and the pipe StartCount (1, printed twice) did not change since the previous beat > 0.5 s ago | C |
| Which stages get a line | a stage prints iff its command queue is non-empty; the numbers are the per-client counters of the client at the queue head | C |
| What completes the pipe | ASC interrupt **source 1** → `CAVEPipeISRManager::AvePipeDoneHandler` → semaphore → `SignalProcessor` → FSM Pipe event 1 → `DoneEventPipe`. Nothing else posts that event | C |
| What xcode waits for | pipe FSM back to state 0 (unless `bTranscodeOverlap`) | C |
| Timeout path in firmware | none; the heartbeat only logs and sets bit 2 of scratch 7's top byte | C |
| `Uncompress Ref is not supported` | printed when `IdrPeriod==1 && (w>4096 \|\| h>4096)` is false **and** `NEED_LSB_PLANES` (wire `0xFD7D`) is 0. **In that case the AVC firmware never programs the reconstructed-picture DMA writer at all** | C |
| LogID 131 line 446 | `CAVEClient::CommandQueue::Dequeue` history entry: cmd type 7, pointer = firmware VA of the queued encode-command buffer | C / I |
| **Best candidate** | recon writer unprogrammed (flag `0xFD7D` = 0, recon table second word = 0); next, SVE `+0x38` left at 1 where macOS writes 0 before every command | I |

Two corrections to the existing record come out of this (§6).

---

## 1. Q1 — decoding the heartbeat

### 1.1 Where it is printed

| String (fw VA) | Sole reference | Function |
|---|---|---|
| `Controller Heart Beat ERROR: LRME/LRMERC/PIPE/XCODE HANG: %d, %d` (`0xbc46a/49a/4cc/4fc`) | `0xc6b0`, `0xc720`, `0xc790`, `0xc800` | `CFlowControllerBase::SignalProcessorSVEHeartBeat` `0xc590` |
| `ENC: StartCount %d-%d-%d-%d, Idle %d-%d-%d-%d` (`0xbc332`) | `0xc278` | `CFlowControllerBase::PrintHwClientStatus` `0xc200` |
| `LRMEFS/LRMERC/Pipe/xcode %d: StartCount ...` (`0xbc360/382/3a4/3c4`) | `0xc2f8`, `0xc3ac`, `0xc460`, `0xc514` | same |
| `AVE FW History Count: %lld` (`0xbe966`) | `0x22db0` | `AVE_History_Print` `0x22d8c` |

**[C]** (xref scan of every `adr`/`adrp+add` in `__text`).

### 1.2 The four StartCount and Idle fields

`PrintHwClientStatus` (`0xc200`):

- StartCount = `u32 [this+3228]`, `[this+3232]`, `[this+3236]`, `[this+3240]`
  (`0xc224..0xc230`). **[C]**
- Idle = `CFSM::Get(x) == 0` (`cset eq`, `0xc240/0xc254/0xc268/0xc27c`) for
  `x = [this+1392]`, `[this+1400]`, `[this+1408]`, `[this+1416]`
  (`0xc220..0xc270`). `0x96924` is `CFSM::Get` (symbol), returning the current
  state `[[fsm+56]+8]` (`0x96948..0x96954`). **[C]**

Which stage each slot is — every writer of the counters:

| counter | writer(s) | stage |
|---|---|---|
| `this+3228` | `str w10,[x19,#3228]` `0x6018`, `0x6418` | `AcquireAndTriggerLRMEFS` |
| `this+3232` | `0x6280` (LRMEFS), `0x68a8` (`AcquireAndTriggerLRMERC`), `0xa8a8` (`GetStatsEventLRMEFS`) | LRMERC |
| `this+3236` | `0x7bf8` only | `AcquireAndTriggerPIPE` |
| `this+3240` | `0x7f8c` only | `AcquireAndTriggerXCODE` |

(the HEVC multicore collector also writes all four, `0x8ede4..0x8edfc`, not on
our path). Each is `++` immediately before the stage's FSM start event
(`0x7be0..0x7bf8` then `CFSM::PostCallback(FSMPipe, 0, …)` `0x7c64`). **[C]**
They are never decremented; they count triggers, not completions. **[C]**

The FSMs are `this+1392` LRME, `+1400` LRMERC, `+1408` Pipe, `+1416` Xcode —
confirmed by the `PostCallback` sites (§2.3) and by the static tables
`CFlowControllerBase::FSMLrme/FSMLrmeRC/FSMPipe/FSMTranscode` at
`0xec1e0/0xec360/0xec510/0xec690`. Those tables are `0x30`-byte records
(`CFSM::Create` walks `0x30` strides, `0x96334..0x96344`); decoded as
{type, event, _, next, _, callback} (type 1 = state, 2 = transition, 3 = end),
`FSMPipe` is:

| state | event | next | callback |
|---|---|---|---|
| 0 | 0 | 1 | `StartEventPipe(void*)` `0xb4d4` → `b 0x9108` |
| 1 | 1 | 2 | `DoneEventPipe(void*)` `0x9e84` |
| 2 | 2 | 0 | `GetStatsEventPipe(void*)` `0xadf0` |

**[C]** for the table bytes and callback VAs, **[I]** for the column names
(consistent with every post site and with `Get()==1` being the heartbeat's
"running" test).

So **`StartCount 1-1-1-0, Idle 1-1-0-1`** = LRMEFS triggered once and back in
state 0; LRMERC triggered once and back in state 0; PIPE triggered once and
**not** in state 0 (the HANG test below proves it is exactly 1: started, no
done); XCODE never triggered and idle. **[C]**

F3/F4 (`1-0-1-0`, `0-1-0-1`) read: LRMEFS triggered and stuck (state ≠ 0),
LRMERC never triggered, PIPE triggered and stuck, XCODE never. Mapping the
datapath DMA fixed LRMEFS; LRMERC then ran; the pipe did not change. **[C]**
from the same decode applied to those kmsgs. Note that in F3 the pipe was
triggered *while LRMEFS was still running*: the pipe trigger does not wait for
LRME on an I-frame. **[I]**

### 1.3 "PIPE HANG: 1, 1"

`SignalProcessorSVEHeartBeat` (`0xc590`), x22 = `this+0x8060` (a heartbeat
record), for the pipe (`0xc744..0xc7b0`):

1. pipe command queue exists and is non-empty: `[this+2984] != 0` and head
   index `[this+2992] != [[this+2984]]` (`0xc744..0xc758`);
2. `CFSM::Get([this+1408]) == 1` (`0xc75c..0xc768`);
3. saved StartCount from the previous beat `[x22+16] == [this+3236]`
   (`0xc76c..0xc778`);
4. `[x22+32] < 500000` and `now - [x22+24] > 500000` (µs,
   `ClockToMicroSecondConvert` `0xc5c0`; `0xc77c..0xc78c`);

then `Print("PIPE HANG: %d, %d", saved, saved)` (`stp x8,x8,[sp]` `0xc798`),
`PrintHwClientStatus` (`0xc7a4`), and `[x22+4] |= 4` (`0xc7a8..0xc7b0`).
Bits for the other stages: LRME 1 (`0xc6cc`), LRMERC 2 (`0xc73c`), XCODE 8
(`0xc81c`). **[C]**

**Both numbers are the same value: the pipe's trigger count, unchanged.** The
first HANG at +2 s is the first beat whose *previous* beat already saw count 1.
**[I]**

At the end of every beat the four counters are snapshotted into `[x22+8..20]`
(`0xc858..0xc874`) and the firmware writes **scratch 7 =
`(hang_flags << 24) | (beat_count & 0xffffff)`** through
`CGPIOManager::instance` (`0xc878..0xc890`; instance symbol `0x154598`); at the
start of the beat it reads scratch 7 back and takes `>> 24` as the flag byte
(`0xc608..0xc634`). So **the host can see `PIPE HANG` as bit 26 of scratch 7**
and can clear it. **[C]** (the driver's "heartbeat scratch7 0x0" at l.904 is
the pre-Process value.)

If `[this+1704] == [this+1708]` it prints `Command Queue Empty!` and skips all
four checks (`0xc620..0xc650`). **[C]**

### 1.4 What decides whether a stage gets its own line

For each of LRMEFS (`this+2960`), LRMERC (`this+3176`), Pipe (`this+2984`),
xcode (`this+3008`): the line is printed **iff that stage's queue is non-empty**
(`cbz` on the queue pointer and `head == [queue]` compare, e.g. `0xc400..0xc414`
for Pipe). The entry at the head (`index mod 7`, `0x58`-byte entries, the magic
`0x24924925`) gives the client id `[entry+12]`; `CAVEPriorityQueue::
GetClientIndexFromID` (`0x175b4`) finds the client; the four numbers are the
**per-client** counters `client+0x44024/28/2C/30` (`add x9,x10,#0x44,lsl#12;
add x9,x9,#0x24`, `0xc48c..0xc4a8`). **[C]**

Per-client writers: `+0x44024` and `+0x44028` in `AcquireAndTriggerLRMEFS`
(`str [x23,#780]` `0x6008`/`0x6404`, `str [x23,#784]` `0x6248`, x23 =
`client+0x43D18` `0x5c98`); `+0x4402C` in `AcquireAndTriggerPIPE`
(`str w10,[x22,#788]` `0x7c44`). So `Pipe 1: 1-1-1-0` and `xcode 1: 1-1-1-0`
are client 1's own LRMEFS/LRMERC/PIPE/XCODE trigger counts. **[C]** (xcode's
`+0x44030` writer not individually located; **[I]** by the pattern.)

In F5 the LRMEFS/LRMERC queues are empty (their jobs moved on), the pipe queue
still holds the frame, and the xcode queue holds the frame waiting for the pipe.
**[C]** decode, **[I]** "waiting" (§2.4).

---

## 2. Q2 — what completes the pipe stage

### 2.1 Trigger: `AcquireAndTriggerPIPE` (`0x7624`)

Called from `ProcessQueue` (`0x126ac`), after `TriggerPrePIPE` (`0x1269c`) and
before `TriggerPostPIPE` (`0x126b4`). **[C]** Preconditions it checks: pipe queue
non-empty and pipe FSM state 0 (`0x7640..0x7664`); global
`CFlowControllerBase::bPipeResetDone` (`0x14e898`) non-zero (`0x7688..0x7694`);
client sync-state checks. Then:

- `vt+136` = `CFlowController::SetPipeClockGating(this, [this+1368], false)`
  (`0x7bc4..0x7bd8`; vtable `0xecfc0`). Returns at once because
  `this[1359]` is never written (docs/52 §4). **[C]**
- `this+3236++`, `client+0x4402C++` (`0x7be0..0x7c44`);
- `CFSM::PostCallback(FSMPipe, 0, CmdQuickCallback)` (`0x7c54..0x7c64`) →
  `StartEventPipe()` `0x9108`. **[C]**

`DoneEventPipe` clears `bPipeResetDone` (`strb wzr,[x8,#2200]` `0xa1b0`), so the
next pipe needs `TriggerPrePIPE`'s reset again. **[C]**

### 2.2 Start: `StartEventPipe` → `ProcessPipeStart` → hardware

`StartEventPipe()` (`0x9108`) calls the controller's `vt+288`
(`CAVECommonController::ResetDMANeighborRegs` `0x27c88`, `0x9318..0x9324`), then
`vt+304` `CAVCController::StartPipe(cmd, -1)` (`0x94f8..0x950c`), asserting the
result is 0 (`rt == 0`, line 1878). **[C]** With `-1` `StartPipe` (`0x49214`)
takes the direct arm (`tbnz w21,#31` `0x49278`) to `vt+504` =
**`CAVCController::ProcessPipeStart`** `0x52d50`. **[C]**

`ProcessPipeStart`: `vt+472` `ProcessPipeReset` (`0x52e08`),
`PipePrepareParam` (`0x52e28`), `SetQP` (`0x52e34`), **`setPipe`** (`0x52e40`),
then, if `[this+0x23FE4] == 0 && [this+0x23FE5] != 0`,
`McpuController::Start(1, 0, [this+0x24019], 0, 0)` via `[this+1040]`
(`0x52ebc..0x52ee8`). **[C]** Those two bytes are Config `+0x40`/`+0x41`
(`bSkipMcpu`/`bCreateMcpu`; chain in §3.4). The MCPU units (`MCPU_MbInput`,
`IntraEst`, `MotionEst`, `ModeDecision`, `ReconLuma`, `ReconChroma`, `CAVLC`,
names at `0xbfe26..0xbfebc`) are created by `McpuInit(1)` (`0x39498..0x395b4`)
and started per pipe by `StartAvePipe` (`0x3974c`) → `CAvePipeMcpu::StartUnit`
(`0x91d14`), which loads each unit's firmware from the ASC image itself
(`LoadFirmwareImageDynamic` `0x91b54`; image pointers such as `0xdc6b0`,
`0xe3350` in `__TEXT.__const`, `0x91348`, `0x9143c`). No host buffer is
involved. **[C]**

`setPipe` ends in `setPipeGo` (`vt+224`, `0x61fec`): sets bit 0 of
`base+0x1110128` (`0x62034..0x62044`) and writes 3 to `base+0x11E0100/4100/8100`
(`0x62064..0x62084`). `base` = `[0x21a7b8]` = AP `0x40C000000` (docs/56 §5.1),
so these are `0x40D110128` and `0x40D1E0100..`. **[C]** register writes, **[I]**
that `0x40D110128` bit 0 is "go".

### 2.3 Done: interrupt source 1 is the only way

`CAVEPipeISRManager` ctor (`0x37dec`) registers, through
`CISRManager::instance` (`0x1545b8`) `vt+56 = Register(src, fn, ctx)`:

| src | handler | ack write to `base+0x1110140` | signals |
|---|---|---|---|
| 15 | `LRMEDoneHandler` `0x3839c` | `2` | `[isr+416]` |
| 32 | `LRMERCDoneHandler` `0x383d4` | `0x800` | `[isr+424]` |
| **1** | **`AvePipeDoneHandler` `0x3840c`** | **`4`** | **`[isr+432]`** |
| 3 | `TranscodeDoneHandler` `0x38444` | `8` | `[isr+440]` |
| 16 / 17 | HEVC pipe / xcode done | `0x20` / `0x40` | `+448` / `+456` |
| 14 | `TranscodeHandler` `0x38504` | reads `base+0x12EA08C` | `+464` on bit 24 |
| 2 | `AxiErrorHandler` `0x38b04` | bit 0 | prints |
| 33 / 34 | `CveSeb5WrBfr` / `AvcXcWrBfr` | `0x1000` / `0x2000` | print `... buffer write full!` |
| 29 | `decompressionErrorHandler` `0x38aa0` | — | **returns 1 silently** |
| 37 | `PiodmaHandler` `0x38aa8` | `base+0x1060004` | sets stitching flags silently |

(registration `0x37eb4..0x3819c`; handler bodies `0x3839c..0x38cdc`; every
source is unmasked at the end of the ctor, `UnmaskAll` `0x381a0` → `vt+48`
`CISRManager::Unmask`). `0xb1ad8` is `_RTK_semaphore_signal`. **[C]**

The semaphore array is `this+0xD0` of the flow controller: `ProcessConfig`
passes `add x3,x19,#0xd0` to `PipeISRManagerCreate` (`0xe5a0..0xe5b0`), and the
ctor stores `[x5+40]` → `isr+432` (`0x381a4..0x381b4`) etc. So source 1 signals
`this+248` = index 0. **[C]**

`CFlowControllerBase::SignalProcessor` (`0x12c94`) matches the fired semaphore
against `this+248+8i` and jumps through the table at `0x12f94`: i=0
"pipe done!" and i=3 "hevc pipe done!" → `PostCallback(FSMPipe, 1)`
(`0x12ec4`, `0x12f1c..0x12f28`); i=1/4 → Xcode event 1; i=2 →
`ProcessTranscodeError`; i=5 → LRME 1; i=6 → LRMERC 1. **[C]**

There are exactly 8 `CFSM::PostCallback` call sites in the image; the only one
that posts event 1 to `FSMPipe` is `0x12f28`. **[C]** Therefore the F5 state
(pipe FSM stuck at 1 while LRME and LRMERC completed through the same ISR
manager and the same ack register) means **source 1 never reached
`AvePipeDoneHandler`**. **[I]**, strong: the other possibility is a handler that
ran and a semaphore that was lost, for which there is no mechanism in the code
read.

`DoneEventPipe` (`0x9e84`) → controller `vt+352` `CAVCController::PipeDone`
(`0x9fec..0x9ff0`, asserting `rt == 0`, line 2266) → state 2 →
`GetStatsEventPipe` → state 0. **[C]**

### 2.4 What xcode is waiting for

`AcquireAndTriggerXCODE` (`0x7c7c`), single-core arm (`[client+480] < 2`,
`0x7d98`):

- client pipe count `[client+0x4402C]` must differ from xcode count
  `[client+0x44030]` (`0x7e20..0x7e24`) — xcode cannot run ahead of the pipe;
- `w9 = ([client+480] > 1) || CFSM::Get(FSMPipe) == 0` (`0x7e64..0x7e7c`);
- if `[client+8] == 0`, `w9 |= ([client+6] != 0)` (`0x7ebc..0x7ecc`);
  `client+6` is `bTranscodeOverlap` (wire `0x10DE4`, docs/46).

So on our path **xcode starts only when the pipe FSM returns to 0**, i.e.
after pipe-done and stats. **[C]**

### 2.5 Timeout

The firmware has none for the pipe: the heartbeat logs, prints the status and
sets the scratch-7 flag (§1.3), and nothing reads `[x22+4]` to reset or abort.
**[C]** for the heartbeat body; **[I]** for "nothing else" (no other reader of
`this+0x8064` was looked for). The 13.5 kext has `FwHeartBeatTimerHandler`
with `Panic for Firmware Heart Beat Timer: %d` / `Log for …` strings; what it
does on macOS is **[U]**.

---

## 3. Q3 — what can stall the pipe with no DMA fault

### 3.1 The recon writer is never programmed (see §4) — top candidate

With `NEED_LSB_PLANES = 0` the AVC `setPipe` skips the only code that writes the
reconstructed-picture DMA registers. **[C]** (§4.3). A pipe whose recon output
stage has no destination and no enable word issues no AXI transaction — so no
DART fault — and never finishes. **[I]**

### 3.2 SVE `+0x38` (`SetIdle`/clock gating) is 1 during the encode; macOS writes 0 before every command

- 13.5 `AVE_HwC::SendFwCmd(CHM*, …)` calls `AVE_DPM_TuneUpPipe(dpm, codec==0,
  codec==1, byte[client+0xE0AC2])` before sending (`0xfffffe0008efb7b4..
  0xefb7d0`, gated on `[x19+160] == 3` `0xefb784`). **[C]**
- `AVE_DPM_TuneUpPipe` (`0xfffffe0008ee7ea8`): `AVE_PMGR::SetPS(3, ClockOn,
  1)` if AVC (`0xee7f4c..f5c`), `SetPS(4, …)` if HEVC (`0xee7f60..f74`),
  `SetPS(5, …)` always (`0xee7f78..f88`), `SetPS(6, …)` iff that client byte
  (`0xee7f8c..fa0`), then **`AVE_PMGR::SetClockGating(false)`**
  (`mov w1,#0` `0xee7fa8`, `bl` `0xee7fac`). **[C]** On 13.5 PD 3/4/5/6 =
  Pipe4/HME, Pipe5/MDINTRA, ME0, ME1/MDINTER (docs/10 table). **[I]** (table
  from docs/10, not re-derived here.)
- `SetClockGating(b)` → `AVE_SVECtrl::SetIdle(b)` (`0xfffffe0008f2cdf8`) →
  `AVE_Reg::Write32(bank 2, [table+44], b)` (`0xfffffe0008f419a0..9ac`), and
  `[table+44]` is `0x38` (docs/45 row 21). **[C]**
- The driver writes `1` to SVE `+0x38` at stage 7 (`driver/ave_drv.c:798`,
  `:815`) and never `0`. **[C]** (grep of `AVE_SVE_IDLE`.)

So on macOS the block is running with SVE `+0x38 = 0` whenever a command is
sent; ours stays 1. What the bit gates in hardware is **[U]**; LRME ran with it
at 1, so if it matters it matters selectively. The firmware itself does not
touch `+0x38` (the only `+0x105xxxx` accesses are `+0x04`, `+0x3C`, `+0x40`,
scratch and doorbell: `0x4edc4`, `0x27cfc`, `0x27e64`, `0xa68e8`, `0xa6a9c`).
**[C]** for the scan (it finds the known scratch/doorbell uses, so it is not
blind), **[I]** that it is complete.

### 3.3 Power: `venc_me1` is not attached, and the overlay attaches an unrelated domain

The live device tree of the F5 boot (boot 14:09:19, F5 at 14:21:45; the AVE
node is still present) was read from `/proc/device-tree`:

| phandle | node | label |
|---|---|---|
| `0x1d` | `power-controller@3b0` | `venc_sys` |
| `0xc1` | `power-controller@8000` | `venc_dma` |
| `0xc3` | `power-controller@8008` | `venc_pipe4` |
| `0xc2` | `power-controller@8010` | `venc_pipe5` |
| `0xc4` | `power-controller@8018` | `venc_me0` (parents `0xc2`, `0xc3`) |
| — (no phandle) | `power-controller@8020` | `venc_me1` (parent `0xc4`) |
| `0xc5` | `power-management@28e680000/power-controller@1a0` | **`afnc4_ioa`** |

and `video-encoder@40d100000/power-domains = <0x1d 0xc2 0xc4 0xc3 0xc5>`.
**[C]** (read-only DT read). Consequences:

- The overlay comment in `test/ave-overlay-e4.dts` (`0xc2 ps_venc_dma`,
  `0xc5 ps_venc_me0`) is wrong: `0xc2` is `venc_pipe5`, `0xc5` is `afnc4_ioa`.
  `venc_dma` is powered only as a genpd parent. **[C]**
- `venc_me1` has no phandle, so nothing references it, and genpd's
  "Disabling unused power domains" (journal, this boot) will have turned it off
  unless it was already off. **[I]**, high.
- macOS powers ME1 for a pipe only when `client+0xE0AC2` is set (§3.2), and the
  26.6.2 `AVC_Panda` DPM map lists ME1 only in its third mask (docs/10). So an
  I-frame may not need it. **[I]**, medium-low.

### 3.4 MCPUs: our Config matches macOS's default

- `ProcessInitStage2` builds the init-param record with `[sp+64] = this[1357]`
  (Config `+0x40`) and `[sp+65] = this[1358]` (Config `+0x41`)
  (`0x14438`, `0x1449c`, `0x144bc..0x144c0`); `CAVCController::Init` copies that
  `u16` to `params+32` (`ldrh w11,[x21,#16]` `0x463d0`, `strh [sp,#48]`
  `0x463dc`, `x1 = sp+16`); `InitEncodingParameters` stores it at
  `this+0x23FE4` (`ldrh w10,[x27,#32]` `0x5ce70`, `strh w10,[x22,#32]`
  `0x5ce84`, x22 = `this+0x23FC4`). **[C]** This resolves docs/54 #32's
  **[U]**: with `skip=0, create=1` `setPipe` calls `ConfigureMCPUs`
  (`0x57e3c..0x57eac`); with `skip=1, create=1` it writes 1 to
  `base+0x1410008`, `+0x1430008` … `+0x14D0008` (`0x57e50..0x57e90`); with
  `create=0`, `ConfigureNoMCPUs` (a log only, `0x60074`). **[C]**
- Kext: `bSkipMcpu = (_S_AVE_Cfg[0] == 3)` (`0xfffffe0008efadd0..ade0`), and
  `_S_AVE_Cfg[0]` is the `ave-platform` boot-arg (`AVE_Cfg_RetrieveBootArgs`
  `0xfffffe0008ea6080..6144`, string `0xfffffe00071e83ce`), zero by default
  (`AVE_Cfg_Default` memset `0xfffffe0008ea6008`). **[C]** So macOS normally
  sends `skip=0, create=1` — the same as `driver/ave_session.c:649-650`. **[I]**
  (no `ave-platform` boot-arg assumed).

### 3.5 Interrupts the host could be responsible for

None on the path read. The pipe-done interrupt is an **ASC-local** source
(src 1) acknowledged by the firmware at `0x40D110140` (§2.3); the LRME and
LRMERC done interrupts use the same manager and register and worked in F5.
**[C]/[I]**. The kext consumes only ADT interrupt index 0 (docs/11, **26.6.2
[C]**, not re-checked on 13.5). The remaining four ADT interrupts are **[U]**,
with no evidence that the host must service them.

### 3.6 Buffers the pipe writes

- No `buffer write full!` line was printed, and handlers 33/34 print
  unconditionally (`0x38bcc`, `0x38c1c`): the CAVLC/SEB intermediate buffers did
  not overflow. **[C]** handler, **[I]** conclusion.
- `LowResResults`, colocated, `fw_data`, `stats_DMA_addr`, `mbAddressCPUFWData`
  are all gated off or `cbz`-skipped when zero (docs/54 §3.4, not re-read).
  "Skipped" means the firmware leaves the corresponding register unprogrammed;
  whether the hardware then waits on it is **[U]** — the same shape as §3.1, but
  for outputs a single I-frame does not obviously need.
- SrcNeighbor/entropy sizes: an overrun inside a mapped arena would not fault
  and would not obviously stop the pipe. **[I]**

### 3.7 Silent error sources

`decompressionErrorHandler` (src 29) returns 1 without logging (`0x38aa0`), and
`PiodmaHandler` (src 37) records bits of `base+0x1060004` into the stitching
flags without logging (`0x38aa8..0x38afc`). A pipe error reported only through
these would look exactly like F5. **[C]** handlers, **[U]** whether they fired.

---

## 4. Q4 — `Uncompress Ref is not supported`

### 4.1 The gate

In `CAVCController::setPipe` (`0x52f38`):

```
53218  add x17, x19, #0x13, lsl #12 ; 53220 add x17,x17,#0xa3c ; str x17,[sp,#112]
54ce4  ldr x8,[sp,#112] ; ldr w8,[x8] ; cbz w8, 0x54f7c   ; A = this+0x13A3C
54cf0  (default path: "encoder_addr_src_colo", colocated, ...)
54f7c  ldrb w8,[x24,#1312] ; cbz w8, 0x54ffc              ; B = this+0x24058 (x24 = this+0x23B38, 0x52f60)
54f84  ldr x9,[x27,#2208] ; cbz x9 -> assert 6777         ; sRecon.Y_LSB
54f8c  tst x9,#0x7f ; b.eq 0x552a8                        ; recon writer block
54ffc  adr x0,"Uncompress Ref is not supported" ; bl CLogger::Print ; b 0x54cf0
```

**[C]**. Log, not assert (as docs/53 §11.5 / docs/56 §5.2 said).

### 4.2 Where A and B come from

- **A = `this+0x13A3C`**, written once, in `InitEncodingParameters`
  (`str w9,[x21,#2588]` `0x5d434`, x21 = `this+0x13020` `0x5d270`):
  `w9 = ([VP+0xFED4] == 1) && ([this+0xA8C] > 4096 || [this+0xA90] > 4096)`
  (`0x5d3f8..0x5d430`), with `[sp,#104]` = `VP+0xFED0` (`0x5ce00`), `this+0xA8C/
  A90` = `VP+0/+4` (`0x5ced0..0x5ced8`). VP = `cmd+0x60`, so A = **`IdrPeriod`
  (wire `0xFF34`) `== 1` and (width `0x60` or height `0x64`) `> 4096`**. The
  other references (`0x460ec` ctor zero, `0x4f2d4`, `0x51280`) only read or
  clear it. **[C]** The same expression is computed in `ProcessInitStage2`
  (`0x1444c..0x14484`). Reading: "intra-only and too large → no reference is
  kept". **[I]**
- **B = `this+0x24058`** = `u8 [VP+0xF760+1469]` (`ldrb w8,[x23,#1469]`
  `0x5d08c`, `strb w8,[x22,#148]` `0x5d098`, x23 = `VP+0xF760` `0x5cdd4`) =
  **wire `0xFD7D`**, the 26.6.2-named `NEED_LSB_PLANES` (docs/46). **[C]**

Ours: `key_interval = 1` → `0xFF34 = 1`, but 1280x720 → A = 0; `0xFD7D` left
0 → B = 0 → the log prints. **[C]** (driver `ave_session.c:912`; no writer of
`AVE_START_NEED_LSB_PLANES` for 13.5.)

### 4.3 What the skipped block does — the recon writer

`0x552a8` is entered **only** from `0x54f90` (a scan of every direct
`b`/`b.cond`/`cbz`/`cbnz`/`tbz`/`tbnz` in the image finds no other edge, and
`0x552a4` is a spin loop, so no fall-through). **[C]** for direct edges;
`br`-through-table targets were not enumerated, so "only" is **[I]** for those
— the same scan does find the known `0x54f90` and `0x55418` edges. It
and its continuations `0x553e8` (from `0x5530c`) and `0x58010` (from `0x55418`,
preceded by a `ret` at `0x5800c`) write registers relative to `x23 + base`.
On every route to this point `x23 = 0x1130700` (`mov w23,#0x700; movk
#0x113,lsl#16` at `0x54a58`, the only assignment to x23 between `0x54a00` and
`0x552b4`; reached via `0x54a20`/`0x54a50` from `0x54a1c`, `0x54d90`,
`0x54e38`, `0x551e0`). **[C]** for the scan, **[I]** that no path avoids
`0x54a58`. Independent check: HEVC `setPipe` writes the same constant to the
same address from a different base (`w12 = 0x1130600`, `sub x11,x12,#0x3c0`,
`0x732d0..0x732f0`), i.e. `+0x1130240` both times. **[C]**

| AP address | value | VA |
|---|---|---|
| `0x40D13025C` | `sRecon.Y_LSB` (low 32) | `0x552b4..0x552bc` |
| `0x40D130250` | `(((w << 5) + 0x3E0) & ~0x3FF) * bits / 8` — one row of 32-px tiles at 1024 samples/tile | `0x552c0..0x552e4` |
| `0x40D130254` | `0x700D1 \| (this[0x24059] << 2)` | `0x552e8..0x552fc` |
| — | asserts `sRecon.Y_MSB != 0`, `& 127 == 0` (6791/6792) | `0x55300..0x55310`, `0x55358` |
| `0x40D13024C` | `sRecon.Y_MSB` (low 32) | `0x553e8..0x553f8` |
| `0x40D130240` | **`0x800314B1`** | `0x553ec..0x55400` |
| `0x40D13031C/310/314` | chroma LSB, size, … (if `this[2692]`) | `0x58010..0x58050` |
| — | asserts `UV_LSB`, `UV_MSB` (6805/6806, 6818/6819) | `0x5540c..0x5541c`, `0x58054..0x58068` |

then `b 0x54cf0` (`0x58120`). `0x40D130000` is the block whose `+0` the AXI
error handler dumps (docs/56 §5.1). The constant `0x800314B1` occurs in the
AVC controller only at `0x553ec` (the other two hits, `0x6fe88`, `0x732e4`, are
`CHEVCController::setPipe4PFrame`/`setPipe` — which also shows the search
works). **[C]**

**So with B = 0 the AVC firmware writes neither the recon addresses nor the
recon enable word, for any frame.** **[C]** Whether a pipe with an
unconfigured recon writer stalls is **[I]**, but it is the one hardware
configuration difference on the pipe path that the firmware itself calls out.

### 4.4 What the "LSB" planes are on 13.5, and what macOS supplies

- `CAVECommonDPB::setRefPointers` (`0x2c314..0x2c33c`):
  `Y_MSB = entry+72`, `Y_LSB = entry+216`, `UV_MSB = Y_MSB + [dpb+20]`,
  `UV_LSB = Y_LSB + [dpb+24]`. **[C]** entry+72/+216 come from the Start_AVC
  recon table's first and second `u64` (docs/53 §13.1).
- `H264VideoEncoderDPB` ctor (`0x2d14c..0x2d1a4`), `w1 = width`, `w2 = height`,
  `w8` = its last argument: `tX = (w+31)>>5`, `tY = (h+35)>>5`;
  `[dpb+20] = tX * 1024 * w8 / 8 * tY`; `[dpb+24] = align_up(32 <<
  (ceil_log2 tX + ceil_log2 tY), 128)`. **[C]** That is the 32x32-tile frame
  format of docs/14 §4.3 (Lossless 8-bit luma tile = `0x400`; metadata =
  aligned power-of-two header). So on 13.5 **MSB = tile data, LSB = tile
  metadata**, and "uncompressed ref" is the lossless-tile reference. **[I]**
  For 1280x720 with `w8 = 8` **[I]**: `tX = 40`, `tY = 23`, luma data
  `0xE6000`, luma metadata `0x10000`.
- 13.5 kext `AVE_CHM_GetFwDPBBuf` (`0xfffffe0008eae4d8`) fills the four recon
  addresses three ways: compressed (`GetYUVCompData`, both words non-zero,
  `0xeae6b0..6e8`); linear `{Y, 0, UV, 0}` (`0xeae6f0..714`); and the default
  (`0xeae788..7b4`) `{base + h4 + h12, base, base + h4 + h12 + h0, base + h4}` —
  metadata at the base, data after it. **[C]** The arm is chosen by client
  fields `[client+0xD0708]`, `[client+0xD0708+1609]`, bit depth and
  `[client+0xE0B55]` (`0xeae574..0xeae6ac`); which one macOS takes for our
  session is **[U]**. Two of the three supply a non-zero LSB word, which only
  makes sense if the firmware reads it, i.e. with `0xFD7D != 0`. **[I]**
- Where macOS's `0xFD7D` comes from: it is inside the `AVE_VIDEO_PARAMS` block
  the kext stages from its client (docs/46 §9); no kext writer of that offset
  was found. **[U]** Whether macOS encodes AVC with this log line printing:
  **[U]**.

---

## 5. Q5 — history entry `LogID 131 line 446 "7 0xffffffff807fd2a8 0 0 0 0"`

- `AVE_History_Add(gpHistory, time, client=[q+0], LogID=0x83, line=0x1BE,
  frame=[e+88], type=[e+92], "%d %p %d %d %d %d", [e+56], [e+64], …)` at
  `0x16bb0..0x16bcc`, in **`CAVEClient::CommandQueue::Dequeue(sCmdInformation*)`**
  `0x16af4`. **[C]** 17 `AVE_History_Add` calls exist; this is the only one with
  `#0x1be`. **[C]**
- `CommandQueue::Enqueue` (`0x1664c`) stores `w1` at `[e+56]` (`0x166ac`) and
  `x4` at `[e+64]` (`0x166a4`, `stp x2,x3,[x22,#40]!` then `str x4,[x22,#24]`);
  for types 7 and 8 it also copies frame info and reads the frame number/type
  from `buf+0x1670/0x1674` (AVC) or `+0x6258/0x625c` (HEVC) into `[e+88..92]`
  (`0x166a8..0x16704`). **[C]**
- So entry 7 = "frame 0, type 3 dequeued for processing": `7` is the
  queue-entry command type (the AVC encode type — it is the type that carries
  FrameInfo, and history entry 6 is `AVC frame 0 type 3`) **[I]**, and
  `0xffffffff807fd2a8` is the command buffer pointer passed to `Enqueue`.
  `0xffffffff80000000` is the firmware's `fw_base` for the FwIPC surface (kmsg
  `msg3: fw_base 0xffffffff80000000 (FwIPC iova 0xff800000)`), so it is offset
  `0x7FD2A8` into FwIPC — a firmware-side copy from the encode-command pool
  (`aquireEncCmdBufFromPool` `0xc8c4`), not our Process message at FwIPC
  `+0x3AB40`. **[I]**
- It is the last history entry because nothing after dequeue completes. The
  same pointer in F3, F4 and F5 fits a deterministic pool slot. **[I]**

---

## 6. Corrections to existing documents (not edited here)

1. **docs/54 §3.4 rows #23 and #25** ("`sRecon.Y_MSB`/`UV_MSB` … reached,
   satisfied"): both are behind the same `0xFD7D` gate as #22/#24 — `0x55310`/
   `0x55358` sit in the block entered only from `0x54f90`, `0x58068`/`0x580b0`
   in the one entered only from `0x55418`. With our configuration they are
   **not reached**. **[C]** (§4.3)
2. **docs/53 §13.1**, "That recon half is already confirmed on hardware … got
   past `setPipe`'s `sRecon.Y_MSB` asserts": those asserts were never evaluated
   (same reason). `setRefPointers` running is still established by the
   LowResSrcLumaScaled chain; the recon half is **not** hardware-confirmed.
   **[C]**
3. **docs/53 §11.5** ("we do not need a compression flag … for the first
   frame"): the flag decides whether the recon writer is programmed at all
   (§4.3), not merely the format of a buffer a later P-frame reads. **[C]**
4. **`test/ave-overlay-e4.dts`** phandle comment: `0xc2` = `venc_pipe5`,
   `0xc4` = `venc_me0`, `0xc3` = `venc_pipe4`, `0xc5` = `afnc4_ioa` (not a VENC
   domain); `venc_dma` = `0xc1`; `venc_me1` has no phandle. **[C]** (§3.3)
5. **docs/54 #32** `ConfigureMCPUs` vs `ConfigureNoMCPUs` **[U]** → Config
   `+0x40/+0x41`, we take `ConfigureMCPUs`. **[C]** (§3.4)

---

## 7. Ranked causes, each with a discriminating change

Same-boot restarts work, so these are meant as separate, one-variable runs.
All are proposals for the operator (AGENTS.md).

### 1. Recon writer unprogrammed — `0xFD7D = 0` and recon entry `+8 = 0` (confidence: medium-high)

**Change.** In the 13.5 `start_avc` layout add `need_lsb_planes = 0xFD7D` (u8)
and write 1 (module parameter, default off, so the old behaviour is one flag
away). Set `recon_meta_addr = 0x08` and lay each DPB slot out like the kext's
default arm and the firmware's derivations:

```
Y_LSB  = slot + 0x00000          (entry +8)   luma metadata   0x10000 at 1280x720
UV_LSB = Y_LSB + [dpb+24]        (firmware)   chroma metadata, keep >= 0x10000 free
Y_MSB  = slot + 0x20000          (entry +0)   luma tile data  0xE6000
UV_MSB = Y_MSB + [dpb+20]        (firmware)   chroma tile data
```

All four are 128-aligned; the end (`slot + 0x106000` + chroma data, roughly
`+0x179000`) fits the current `0x1c2000` slot. Keep the per-frame PICMGMT recon
fields as they are (they are overwritten).

**Expect if this is the cause:** `Uncompress Ref is not supported` is gone
(deterministic — if it still prints, the byte did not land: apparatus check);
no assert 6777/6791/6805/6818; then pipe done — heartbeat
`StartCount 1-1-1-1` or no heartbeat error, xcode runs, and ideally `0x0E06`
arrives. A DART fault at a recon IOVA instead would mean the tile sizes are
wrong but the writer is now live — also progress, and it names the plane.
**Expect if not:** the log line is gone, and the heartbeat still shows
`PIPE HANG: 1, 1` / `1-1-1-0 / 1-1-0-1`.

### 2. SVE `+0x38` left at 1 during the encode (confidence: medium)

**Change.** Immediately before sending Process, `ave_write(ave, AVE_BANK_SVE,
AVE_SVE_IDLE, 0)` (what `AVE_DPM_TuneUpPipe` → `SetClockGating(false)` does on
13.5 before every command), and write 1 back after the completion or timeout.
This register is already written by stage 7 on every run, so the access itself
is proven.

**Expect if cause:** the pipe completes (as in #1). **If not:** identical
heartbeat. Run it on its own first, then combined with #1 if #1 alone moves
the failure but does not finish the frame.

### 3. Power: `venc_me1` off (and `afnc4_ioa` attached by mistake) (confidence: low-medium for an I-frame)

**Change, diagnostic first (read-only):** at the hang, log bank 3 (`unk3`,
AP `0x28E588000`, already mapped) offsets `0x00, 0x08, 0x10, 0x18, 0x20` =
PS registers of VENC_DMA, PIPE4, PIPE5, ME0, ME1 (docs/27 §7). **Then:** fix
the overlay list to `<0x1d 0xc1 0xc3 0xc2 0xc4>` and attach `venc_me1`
(it has no phandle, so either give it one in the DT build or attach its genpd
from the driver through its node). macOS brings ME1 to ClockOn only when
`client+0xE0AC2` is set.

**Expect if cause:** ME1's PS reads off; with ME1 on, the pipe completes.
**If not:** ME1 reads off and powering it changes nothing, or it already reads
on.

### 4. The done interrupt is raised but not delivered (confidence: low)

**Change (read-only, run last in a session because the frame is already
lost):** when the timeout fires, log `0x40D110140` (ack/status; pipe bit 2),
`0x40D110128` (bit 0 set by `setPipeGo`), `0x40D120000/4`, `0x40D130240`
(recon enable word) via bank 0, and scratch 7 (expect bit 26 set by the heartbeat).
The firmware's AXI handler reads `0x40D110140` before writing it, so the read
may have a side effect: **[I]** risk to the diagnosis, not to the machine.

**Discriminates:** bit 2 set with no FSM progress → delivery/ack problem, not
the hardware; bit 2 clear → the hardware really did not finish (then #1–#3).
`0x40D130240 != 0x800314B1` at the hang would independently confirm §4.3 on
hardware (the value after reset is **[U]**).

### 5. MCPU start path (confidence: low; our Config equals macOS's default)

**Change:** Config `+0x40 = 1` (`skip_mcpu=1`, keep `+0x41 = 1`). The firmware
then writes 1 to the seven MCPU `+0x8` registers and skips `ConfigureMCPUs` /
`McpuController::Start`. **This departs from what macOS does**; use only as a
discriminator if #1–#3 are exhausted. **Expect:** a change in behaviour
(completion or a different hang/log) implicates the MCPU units; no change
exonerates them.

### 6. Silent pipe error (src 29 decompression, src 37 PIODMA) (confidence: low)

No driver change can see the firmware flags; read `0x40D060004` alongside #4.
Non-zero bits 0–4 at the hang would point here.

---

## 8. Reproduce

```sh
# heartbeat, status print, counters
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0xc590 -n 0x334
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0xc200 -n 0x390
# trigger / start / done chain
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x7624 -n 0x658
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x9108 -n 0x490
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x12c94 -n 0x31c
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x37dec -n 0x440
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x3839c -n 0x170
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x7c7c -n 0x300
# Uncompress Ref gate, its inputs, and the recon writer it skips
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54ce4 -n 0x10
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54f7c -n 0x90
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x552a8 -n 0x160
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x58010 -n 0x118
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d3f8 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d08c -n 0x10
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2d14c -n 0x60
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2c314 -n 0x30
# history entry
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x16af4 -n 0xfc
# MCPU selection
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x14438 -n 0x90
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57ddc -n 0xd4
# kext: TuneUpPipe before every command, SetClockGating -> SetIdle, DPB arms
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008efb780 -n 0x54
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee7f30 -n 0x88
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008f41974 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eae574 -n 0x250
# live DT of the F5 boot (read-only)
od -An -tx4 --endian=big /proc/device-tree/soc/video-encoder@40d100000/power-domains
```
