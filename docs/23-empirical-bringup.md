# Closing the last gaps empirically

Static analysis has taken this about as far as it efficiently goes. The
remaining unknowns — the per-frame `AVE_PICMGMT_PARAMS` fields, the `RCMode`
enumerators, the `iAddr` alignment question — are all things the hardware can
answer directly, and faster than more disassembly can.

But "fuzzing" undersells what is available. Blind random input would be a poor
tool here. There are **three good oracles**, and with them this becomes
measurement rather than search.

## Oracle 1 — the firmware's own logging (highest value, underused)

The firmware is built with its instrumentation intact. It carries `AVE_Log`
with per-subsystem levels (`AVE_Log_CheckLevel(subsystem, level)`), full RTKit
crashlog and tracekit infrastructure (`RTK_crashlog_init`,
`_RTK_tracekit_tracelist`), and format strings that **name fields verbatim**:

```
CMD %d %d | %d 0x%x | CID %llu CNT %llu
sBufSet.saRecon[%d][0].iAddr:%016llx
sBufSet.saEntropyCoding[%d][%d].saIBuf[AVE_BufIdx_Data].iAddr:%016llx
pPicParams->sRecon.{Y_LSB,Y_MSB,UV_LSB,UV_MSB}
pPicParams->sRef.Y_L0_MSB[me_ref_index]
```

If log output can be made to flow, **the firmware describes its own ABI**. That
is not inference from a disassembler; it is the vendor's instrumentation
telling us what it received and what the fields are called. It is strictly
better evidence than anything perturbation can produce.

Priority order therefore starts here: after the transport works, get logging
out before designing any experiment. Two things to find:

1. Where log output lands — a ring in the shared region, a dedicated channel,
   or the `FwLog` surface (surface index 29, which has no InfoSet slot and is
   allocated directly).
2. Whether the host can raise the per-subsystem log level. `AVE_Log_CheckLevel`
   takes a subsystem id and a level, so the levels live somewhere writable.
   Raising them is likely worth more than any other single experiment.

## Oracle 2 — the output bitstream

For the per-frame fields this is nearly perfect, because **H.264 is
self-describing**. Everything a minimal encode needs to set is readable back
out of the encoder's own output:

| Field being hunted | Read back from |
|---|---|
| QP | slice header `slice_qp_delta` + PPS `pic_init_qp_minus26` |
| Frame type (I/P/B) | slice header `slice_type` |
| Resolution | SPS `pic_width_in_mbs_minus1`, `pic_height_in_map_units_minus1` |
| Reference count | SPS `max_num_ref_frames` |
| GOP / IDR period | IDR NAL spacing |
| Entropy mode | PPS `entropy_coding_mode_flag` |
| Profile / level | SPS `profile_idc`, `level_idc` |

So the loop is not "guess and hope":

```
for candidate_offset in region:
    for value in {a, b}:
        set field, encode one frame, parse the output
    if the parsed parameter changed with the value -> that offset is that field
```

This *observes* semantics rather than inferring them. It is exhaustive for the
dozen fields a first encode needs.

## Oracle 3 — the firmware as a validator

The firmware rejects malformed input loudly, which makes it a classifier for
valid versus invalid values:

- `CmdProcessor` rejects any command whose length is not the exact expected
  size — so structural mistakes fail immediately rather than corrupting state.
- `CRateControl::Check_RCMode` rejects out-of-range modes (note its `csel`
  polarity: it rejects 20 and 100, which reads backwards at first glance).
- `AVE_DMV_CheckResolution` returns `-1002` on an unsupported configuration.

This makes the `RCMode` enum an ideal perturbation target: sweep a small
integer domain, record accept/reject, and the valid set falls out. Constant-QP
is then identifiable by which accepted mode makes the output QP track the QP
field. Domain is small, signal is clean, and no addresses are involved.

## What not to do

- **Do not blind-fuzz command payloads.** Before the structure is known you
  mostly hit the size check and learn nothing. Perturb specific offsets with
  a hypothesis attached.
- **Do not perturb the ASC start sequence or the power-domain order.** Both are
  already known exactly. Randomising them only costs reboots.
- **Be careful with anything that carries an address.** Wrong values produce
  DART faults or a wedged coprocessor.

On which: the **DART is the safety net** — for the coprocessor. AVE cannot
reach memory outside its mappings, so a bad *address* is contained by the IOMMU,
and the firmware is loaded and signed by iBoot so it cannot be persistently
damaged.

> **Corrected 2026-09-07.** That containment argument is sound for the
> coprocessor and was then wrongly extended to the whole experiment. It assumes
> a working, correctly powered DART; **standing up a new DART node is not itself
> a contained act.** An access to an unpowered or unclocked register block on
> Apple silicon hangs the fabric with no fault, no panic and nothing logged.
> That is what happened on the first probe attempt — see
> [24-incident-2026-09-07.md](24-incident-2026-09-07.md).
>
> Two further gaps in that assessment: device probe is **asynchronous**, so a
> clean `insmod` return is not evidence the step succeeded; and an overlay that
> binds two drivers needs both risk-assessed, not just the one being developed.
>
> **Do not attempt a probe on this machine without an m1n1 hypervisor serial
> console attached.** A hang with no console destroys the information needed to
> diagnose it.

## Harness

The single most valuable piece of infrastructure is **a debugfs interface for
raw command submission**, so each experiment is a userspace script rather than
a module rebuild and reboot. Roughly:

```
/sys/kernel/debug/apple-ave/
    cmd            write: raw command bytes -> submitted verbatim
    log            read:  firmware log output
    last_status    read:  return code and any error payload
    regs           read:  the SVE block, for interrupt/status observation
```

With that in place a field hunt is a shell loop, and the whole session is
recordable. Every experiment and its observation should be committed as data
under `data/experiments/` so the field map is reproducible rather than
remembered — the same standard the static findings are held to.

## Methodological warning

Perturb-and-observe produces confident conclusions from small samples, which is
[trap 2](00-methodology.md) wearing different clothes. "Offset X is QP because
changing it changed the QP" is wrong if two fields feed QP, or if the value was
clamped, or if the effect came from something else that moved at the same time.

For every positive result:

- **test a negative** — does changing an unrelated nearby offset produce the
  same effect? If so, the experiment is not isolating anything.
- **test the boundary** — does the effect saturate or wrap where the field's
  width says it should? A u8 field should misbehave at 256.
- **prefer the oracle that names things.** A firmware log line that prints the
  field beats ten differential experiments that infer it.

Differential testing tells you what a field *does*. It does not tell you what
it is called, what its valid domain is, or what else it affects. Use it
surgically on the dozen fields that matter, not to map 11.5 KB of structure.

## Sequence

1. Bring up transport (`ave_drv.c`, `ave_ipc.c`) and confirm the ASC starts and
   the firmware answers the handshake. **This alone validates a large fraction
   of the recovered constants** — power order, ASC sequence, doorbell, IPC
   region, interrupt path — and is the single highest-value next step.
2. Get firmware logging out. Raise log levels if reachable.
3. Sweep `RCMode` against the validator.
4. Differential-hunt the per-frame fields against the bitstream oracle.
5. First light: fixed QP, I-frames only, one resolution. Success is
   `ffmpeg -v error -i out.h264 -f null -` staying silent.
