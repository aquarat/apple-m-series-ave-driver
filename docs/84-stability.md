# 84. Stability: the campaign, hang recovery, reload

2026-09-27. How stable the H.264 and HEVC paths are, and what is being done
about the two ways the driver still needs a reboot: a firmware hang and a
module reload.

## 1. The stability campaign (`tools/stress.sh`)

On the target, with the driver loaded as `OVERLAY_ARGS=pmp_venc=1
tools/ave-load.sh pmp_report=1 pmp_vote=0x2000000300000003` (the per-stream
vote soaks too). Everything goes through the V4L2 node with v4l2-ctl.
Fedora's ffmpeg has no HEVC decoder, so HEVC is checked by byte-exact
determinism on the target and graded on the host.

**Run of 2026-09-27 07:36-09:52: 0 FAIL.**

| part | what | result |
|---|---|---|
| references | 7 configurations: H.264 and HEVC 1080p, HEVC Main 10 from P010 720p, H.264 and HEVC 4K, rate control in both | H.264 decodes to the right frame count; HEVC parses |
| 1 soak | the 7 configurations repeated for 2 hours | **865 iterations x 7 = 6055 streams, every one byte-identical to its reference**; kernel log clean |
| 2 long streams | 60 000 frames H.264 1080p decoded live; 60 000 frames HEVC 720p to a file | H.264: decoder silent. HEVC (717 MB, decoded on the host): **0 decoder errors** |
| 3 cycles | 300 open/stream/close, alternating codecs | all byte-identical |
| 4 sweep | 150 random configurations: width 192-3840, height 96-2160, codec, QP 15-45 or 1-20 Mbit/s | all pass; the 72 HEVC outputs graded on the host, none below 25 dB |
| 5 kill | 40 clients SIGKILLed mid-stream at random | the next stream byte-identical every time |
| 6 contend | 20 rounds of two clients at once | one completes byte-identical, the other gets EBUSY, the device is fine after |

The script's first version had three bugs of its own, each caught before
the run and none a driver fault:
- `$0` resolved after `cd`;
- `pkill -P` hit `timeout(1)`, not the client, so the "killed" client kept
  the device and every later stream saw a correct EBUSY;
- ffmpeg's `psnr` filter repeats the last decoded frame against a longer
  source, so short-height streams graded at 20 dB (use `shortest=1`).

## 2. Hang recovery

**R1** (`results/r1-*`, bs1's reliable two-reference hang +
`session_recover=1`): after `PIPE HANG`, Stop is taken (IO ack) but never
completed, and a new Open is not even acknowledged. **The firmware is
wedged: recovery needs a core restart**, the same problem as a reload.

## 3. Linux can write the DAPF

docs/49 concluded that Linux cannot write AVE's DAPF (every attempt was an
SError). Every one of those writes went through a posted `devm_ioremap()`,
the mistake f93/f94 made on the PMP (docs/75). With non-posted mappings
(`ave_devm_ioremap_np`):
- **R2** (`dapf_set=same`, stage 8, core never started): all 16 slots
  written back with their own contents in m1n1's order, 0 differ, no SError.
- **R2b** (`dapf_set=probe`): slot 15 (uninitialised garbage outside
  m1n1's entries 0-2) cleared to zero reads back zero. **Writes take
  effect.**

Consequences: ave1's DAPF can be programmed by the driver (docs/82 M1 may
not need m1n1), and a reload is not blocked by the DAPF.

## 4. Reloading in the same boot

**R5** (load, encode, clean unload, then `insmod core_reset=2
fw_restore_data=1`): the halted core (0x2e) is pulsed, the DAPF fingerprint
**survives**, DATA is restored and the reload encodes at the **identical**
PSNR (44.308053). The one change from f56, which died here: the post-pulse
check reads only the DAPF, not the datapath DART's registers.

`reload` (default on) makes this automatic. At stage 7, when `core_reset`
is not given, `ave_fw_data_ran()` compares this boot's DATA with the
pristine blob, ignoring STKG (random per boot). Cold: 0 bytes, nothing
pulsed. After a run: ~291 600 bytes, so pulse + restore (`recover_halted`).
CPU_STATUS alone cannot tell cold (0x2a) from halted (0x2e) reliably, which
is how s2-9 pulsed a cold core.

**R6** (five `ave-load.sh` / encode / `ave-load.sh unload` cycles in one
boot, default parameters): load 1 "cold", loads 2-5 reset and restore.
**Every load: H.264 44.308053 dB, HEVC byte-identical.** `ave-load.sh` no
longer refuses a second load.
