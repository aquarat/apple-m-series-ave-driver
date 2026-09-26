# 80. The PMP clock vote, held per stream

2026-09-26. Until now `pmp_vote` (docs/75 R4, docs/78) was written once at
probe and held until unload. The encoder's clock, the SoC level and the
fabric level all stayed at the voted level while nothing was being encoded.
Now the vote is held only while a stream is open.

## 1. Behaviour

| load options | when the vote is written | when it is released |
|---|---|---|
| `pmp_vote=0` (default) | never | - |
| `pmp_vote=V` (streaming, the new default) | `ave_enc_start()`, before Open | `ave_enc_stop()`, after a clean Close |
| `pmp_vote=V pmp_vote_always=1` | probe, as f95-f99 did | power-off |
| `pmp_vote=V` with `session_selftest=1`/`session_frame=1` | probe (the self-test never streams) | power-off |

The rules from docs/75 §10 still hold:
- **No vote without the report.** It needs `pmp_report=1`, a running PMP
  and `ave-overlay pmp_venc=1`. Without the report, probe warns once and
  never writes the entry.
- **Released before the report.** At power-off the vote goes first, then
  the report, then VENC_SYS. That covers a stream that was still open.
- **Non-posted writes.** The entry is written with `ioremap_np`
  (f93/f94).

`pmp_vote` is validated at probe, so a malformed value still fails the
load rather than the first STREAMON.

## 2. Differences from the probe-time vote

- **No 200 ms holds.** The probe-time path waits 200 ms around each
  access, so each netconsole marker leaves the machine before the next
  access (f93-f99). The streaming path skips them, since they would add
  800 ms to every STREAMON and STREAMOFF. It also skips the read-back (§3): each
  vote logs `pmp: AVE0 DVFS vote V written`, and stream end logs
  `pmp: at stream end, AVE0 DVFS V (+8 S)`, with `MISMATCH` appended if
  the entry is not the vote.
- **A failed vote does not fail the stream.** It only costs speed, and the
  stream runs at whatever clock the PMP chooses.
- **A stream that did not close keeps its vote.** If Close fails, the
  firmware may still be encoding into the stream's buffers, so the clock
  is left alone. Power-off releases it.

## 3. Results (docs/53 v1-v9)

**Held per stream, VMax + FAB0 VMax runs at 5.81 ms per 1080p frame and
21.0 ms per 4K frame**, the same as the probe-time vote (f99). One finding
changed the design on the way:

**Never read the entry back while the PMP has the write pending.** The
read side's status word (`+8`) has bit 1 set from the write until the PMP
takes the value up. Reading the entry in that window makes the PMP treat
the request as handled without applying it: the status clears, the value
reads back correctly, and the clock stays at the no-vote level (v1, v3,
v7: 17.4 ms/64 ms). The same vote with no read runs at full speed (v8). The
probe-time path always read 400 ms after its write, which is why f95-f99
never saw it. macOS never reads the entry after `_writePTD` (docs/75 §1.4).

So a streaming vote is a write only. `ave_pmp_stream_off()` reads the
entry at stream end, when bit 1 has long cleared, and logs `MISMATCH` if it
is not the vote. The release (`1 << 61`) is written and not read.

Ruled out on the way:
- a PS-REQ transition (v4: pulsing the report after the vote changes
  nothing)
- the vote having to precede the firmware boot or Config (v5, v6: both
  run at full speed)
- a slow PMP ramp (v7: 10 s streams stay slow)

**Not measured:** that the release lowers the clock between streams. It is
the same write-only path as the vote, which v8/v9 show the PMP applies.
Reading the PMP's DVFS-STATE while idle (`perf_dump`) would show it.
