# 96. B frames and two references: hardware results and defaults

*2026-10-04, M1 Pro (t6000, H13S) and M2 (t8112, H14G). The V4L2 B-frame
and two-reference support of docs/81 §8, run on hardware, benchmarked, and
the defaults that follow from the numbers.*

## Summary

- **B frames work through V4L2 on both SoCs**, H.264 and HEVC, B = 1 and 2,
  with every docs/81 §8.6 test passing (§1). The M1 Pro's streams are
  byte-identical to the M2's.
- **One B frame per P with the B QP 3 above the I QP is the best measured
  setting: −7.6 % bitrate at equal VMAF, −9.7 % at equal PSNR-Y**, better
  on every clip (§2). It halves the gap to x265 `--preset medium`: from
  +20 % to +11 % (VMAF), from +21 % to +7.5 % (PSNR-Y).
- B frames cost no hardware time per frame (§3). Each B adds one frame of
  latency.
- Two-reference P frames give −0.1 % / −0.6 % and cost the M1 Pro twice the
  time per frame (docs/94).
- **Defaults** (§5): a B QP of 0 with B frames on now means I QP + 3; the
  B_FRAMES control stays at 0, because no measured client would gain from
  a default and one would break.

![compression settings](img/levers.svg)

## 1. Hardware tests (docs/81 §8.6)

`tools/v4l2-test.sh`, 1280x720 `testsrc2`, fixed QP 30, frame types from
`ffprobe` in display order:

| run | what | M2 | M1 Pro |
|---|---|---|---|
| v0 | defaults, v4l2-ctl and ffmpeg | 44.255294 / 52.793971 dB, as before | same |
| v1 | H.264, 2 references | 60 frames, 44.35 dB | (module-parameter run: same) |
| v2, v2r | H.264 B=1, 61 frames | `IBPBP…BP`, all decode; the repeat is byte-identical | byte-identical to the M2 |
| v3 | H.264 B=2 | `IBBPBBP…BBP` | same |
| v4 | drain: B=2 60 / 59 frames, B=1 60 | `…BBPBP`, `…BBPP`, `…BPP`; every frame decodes | B=2/59 same |
| v5 | B=1, GOP 30, 61 frames | I at 0, 30, 60; frame 29 P | same |
| v6 | B=2, forced key frame every 6 | `IBBPBPIBBPBPI…`: never a B before an IDR | same |
| v8 | HEVC, 2 references | 58 of 59 P frames use two (`refs l0 2`) | - |
| v9 | HEVC B self-test (hb1) | completion order 0, 2, 1, 4, 3; `IBPBP` decodes, 50.9 dB | - |
| v10 | HEVC B=1, 61 and 60 frames | `IBPB…`, decode, 44.20 / 44.21 dB | byte-identical to the M2 |
| v11 | HEVC B=2 | `IBBP…`, 44.18 dB | byte-identical to the M2 |
| v12 | ffmpeg H.264 and HEVC | as v0 (ffmpeg sets B_FRAMES 0) | H.264 as v0 |

No hang, timeout or DPB error in any run. Grading note: the M2's ffmpeg 8.1
pairs frames differently when it grades a raw stream with B frames (the
same file graded 44.09 dB there, 44.30 dB with the M1 Pro's ffmpeg); the
bench below decodes to raw frames first and is not affected.

## 2. Compression

`bench/hevc-efficiency` on the M2 (five Xiph 1080p50 clips, 200 frames, QP
22-38; `results-m2-bframes.csv`, `results-m2-bframes2.csv`). Encoder names:
`-bN` sets `video_b_frames=N`, `-boffN` the B QP to the I QP + N,
`-poffN` the P QP likewise. PCHIP BD-rate against AVE's own P-only fixed QP
(`ave-cqp`), negative = fewer bits for the same quality:

| setting | crowd | park | ducks | tree | town | **mean VMAF** | **mean PSNR-Y** | vs x265 medium (VMAF / PSNR-Y) |
|---|---|---|---|---|---|---|---|---|
| P only (`ave-cqp`) | | | | | | 0 | 0 | +20.3 % / +20.9 % |
| P QP +3 (docs/92) | −1.4 | +1.7 | +0.4 | −5.3 | −3.2 | −1.6 % | −2.1 % | +21.5 / +21.6 |
| 2 references (docs/94) | | | | | | −0.1 % | −0.6 % | |
| B=1 | −0.7 | −4.5 | −5.5 | +4.0 | +4.2 | −0.5 % | −4.2 % | +19.6 / +15.3 |
| B=2 | +8.2 | +3.2 | +0.4 | +9.0 | +9.8 | +6.1 % | +0.3 % | +27.5 / +20.9 |
| B=1, B QP +1 | −3.5 | −7.7 | −5.6 | −2.5 | −2.1 | −4.3 % | −7.2 % | +15.1 / +10.7 |
| B=1, B QP +2 | −5.9 | −10.5 | −5.6 | −7.3 | −5.7 | −7.0 % | −9.1 % | +11.9 / +8.0 |
| **B=1, B QP +3** | −6.7 | −11.5 | −3.7 | −9.3 | −7.0 | **−7.6 %** | **−9.7 %** | **+11.3 / +7.5** |
| B=1, B QP +4 | −7.3 | −12.3 | −1.6 | −10.6 | −8.2 | −8.0 % | −9.8 % | +11.0 / +7.7 |
| B=1, P QP +1, B QP +3 | −5.7 | −9.8 | −4.7 | −7.4 | −7.2 | −7.0 % | −9.9 % | +13.2 / +8.9 |
| B=2, B QP +2 | −0.8 | −7.2 | −0.1 | −9.4 | −6.7 | −4.8 % | −8.1 % | +14.5 / +9.0 |
| B=2, B QP +4 | −4.3 | −12.3 | +4.4 | −15.5 | −11.7 | −7.9 % | −10.7 % | +11.2 / +7.5 |

(VMAF per clip; the PSNR-Y per-clip values are in the CSVs.)

- B frames at the P QP save little: they cost about as many bits as the P
  frames they replace. The gain comes from coding the B frames, which
  nothing references, more coarsely.
- B QP +3 and +4 are within 0.4 %; +3 is the steadier one (`ducks_take_off`
  −3.7 % against −1.6 %).
- B=2 is no better than B=1 at any offset tried: the P frames move twice as
  far from their references.
- A P QP offset on top adds nothing.

![BD-rate vs x265 medium](img/bdrate.svg)

## 3. Speed and latency

Hardware time per frame (the driver's `enc:` line), 61 frames, fixed QP, M1
Pro with the PMP vote:

| | H.264 B=0 | H.264 B=1 | HEVC B=0 | HEVC B=1 |
|---|---|---|---|---|
| 720p | 2.74 ms | 2.77 ms | 3.58 ms | 3.47 ms |
| 1080p | 5.24 ms | 5.15 ms | 6.69 ms | 6.76 ms |

No difference beyond run-to-run noise. In the bench (whole ffmpeg/V4L2
loop, M2), B=1 ran at 83 fps against 76 for P only: fewer bits to copy.
Each B frame is held until its P has been encoded, so B=1 adds one frame of
latency and B=2 two.

## 4. Clients

| client | B frames |
|---|---|
| v4l2-ctl, raw streams | work (all of §1) |
| ffmpeg `h264_v4l2m2m` / `hevc_v4l2m2m` | never: the wrapper sets B_FRAMES 0 whatever `-bf` says (docs/81 §3.2) |
| GStreamer 1.28 `v4l2h264enc`, Baseline (its default when downstream does not ask for a profile) | none: the driver refuses B for Baseline |
| GStreamer 1.28 `v4l2h264enc`, `profile=high`, `extra-controls="controls,video_b_frames=1"` | the elementary stream is correct (90 of 90 frames, `IBPBP…`), but the element gives each output buffer the next input timestamp as its DTS, without the reorder delay: every B frame gets DTS > PTS (B at PTS 33 ms, DTS 67 ms). Into `mp4mux` this loses a frame and leaves non-monotonic timestamps |

The GStreamer problem is in the element: V4L2 has no decode timestamp, so
the encoder element has to derive the DTS, and with reordering it needs a
one-frame offset (`gst_video_encoder_set_min_pts()` exists for this). The
driver gives each CAPTURE buffer its own source's timestamp, as the V4L2
encoder interface specifies.

## 5. Defaults

| what | default | why |
|---|---|---|
| B QP (`h264_b_frame_qp_value`, `hevc_b_frame_qp_value`) | 0 = **I QP + 3 when B frames are on** (was: the P QP) | §2: the best measured offset; an explicitly set B QP is used as given. With B frames off nothing changes |
| `video_b_frames` | **0** | no measured client gains from a default: ffmpeg overrides it, GStreamer's default Baseline refuses it, and GStreamer with Main/High muxes broken timestamps (§4). It also adds latency, which live and real-time use does not want. Clients that handle reordering set it |
| `reference_frames_for_a_p_frame` | 1 | −0.1 % / −0.6 % for 2.2x the M1 Pro's time per frame (docs/94) |
| MultiME (wire 0xFCEA) | set automatically with B frames or two references | the one-reference stream gains nothing from it (identical output and speed on the M1 Pro, docs/94), so it stays off there: one less difference from the tested default path |

The recommended setting for a client that handles B frames: `video_b_frames=1`
with Main or High (H.264) or Main (HEVC), and no B QP.

## 6. Reproduce

```sh
# hardware tests: docs/81 §8.6
# compression (target; bench/hevc-efficiency/README.md for the set-up)
RESULTS=results-m2-bframes2.csv \
ENCODERS=ave-cqp-x-b1-boff1,ave-cqp-x-b1-boff3,ave-cqp-x-b1-boff4,ave-cqp-x-b2-boff4,ave-cqp-poff1-x-b1-boff3 \
    python3 bench.py run crowd_run park_joy ducks_take_off in_to_tree old_town_cross
# charts (host)
.venv/bin/python bench/plots.py
# GStreamer
gst-launch-1.0 videotestsrc num-buffers=90 ! video/x-raw,width=1280,height=720,framerate=30/1,format=NV12 \
    ! v4l2h264enc extra-controls="controls,video_b_frames=1" ! video/x-h264,profile=high \
    ! h264parse ! mp4mux ! filesink location=b1.mp4
```
