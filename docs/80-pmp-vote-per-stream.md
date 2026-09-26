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
  800 ms to every STREAMON and STREAMOFF. Each vote still logs one line,
  `pmp: AVE0 DVFS vote V, read back R`, with `MISMATCH` appended when the
  read-back differs.
- **A failed vote does not fail the stream.** It only costs speed, and the
  stream runs at whatever clock the PMP chooses.
- **A stream that did not close keeps its vote.** If Close fails, the
  firmware may still be encoding into the stream's buffers, so the clock
  is left alone. Power-off releases it.

## 3. What a hardware run has to show (v1, not yet run)

Load with `pmp_report=1 pmp_vote=0x2000000300000003` and
`OVERLAY_ARGS=pmp_venc=1`, then run `tools/v4l2-test.sh` at 1080p and 4K, twice
each.

- **Yes:**
  - one vote line and one release line per stream, both read back
    without MISMATCH
  - per-frame times at the f99 figures, 1080p 5.8 ms and 4K 21.0 ms,
    from the first stream on
- **No:**
  - frame times at the no-vote figures (17.4 ms / 63.9 ms): the PMP needs
    time to act on the vote, or the release of the previous stream wins
  - an SError, or a reset at STREAMON: the missing holds mattered

If the first frames of each stream are slow and the rest fast, the PMP
takes time to raise the clock. The fix would be to vote at `open()` rather
than at STREAMON, not to bring back the holds.
