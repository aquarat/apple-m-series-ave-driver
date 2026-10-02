# 88. HEVC compression efficiency against x265

2026-09-30. How good is AVE's HEVC, as the driver runs it today, next to a
software encoder? Measured on the t6000 (M1 Pro, docs/87) with the PMP
vote. Scripts, raw results and the reproduction recipe:
`bench/hevc-efficiency/`. Marks as in docs/00: [C] measured here, [I]
inferred.

## Summary

BD-rate is the extra bitrate AVE needs to match the other encoder's
quality (PCHIP BD-rate, see §4). Positive means AVE's files are bigger.

| AVE mode vs | public clips: PSNR-Y / VMAF | camera clips: PSNR-Y |
|---|---|---|
| **fixed QP vs x265 `medium` CRF** | **+20.9 % / +20.3 %** | +0.6 % |
| **fixed QP vs x265 `slow` CRF** | **+41.5 % / +73.8 %** | +11.3 % |
| fixed QP, Main 10 vs x265 `medium` CRF | +19.8 % / +15.8 % | −1.3 % |
| bitrate mode vs x265 `medium` (same targets) | +31.2 % / +22.4 % | +32.7 % |
| bitrate mode vs x265 `ultrafast` (same targets) | +1.8 % / +7.0 % | −21.2 % |

[C] AVE compresses like x265 between `ultrafast` and `fast`: about
`medium` + 20 % on clean sources, 40-75 % behind `slow`. On sources that
are already heavily compressed (the camera clips, H.264 at ~1.2 Mbit/s),
it is level with `medium`: the source's own artefacts cap what either
encoder can reach. Fixed QP is AVE's better mode; its bitrate mode (what
ffmpeg's `-b:v` uses) adds roughly 10 points. **Main 10 does not change
the picture**: −0.8 % (PSNR-Y) / −3.5 % (VMAF) against AVE's own Main,
at the same speed.

In return, per frame of 1080p50 [C]:

| encoder | fps | CPU s/frame | energy above idle |
|---|---|---|---|
| AVE, fixed QP (Main or Main 10) | ~115 | 0.0013 | 0.03 J |
| AVE, bitrate mode via ffmpeg | 117 | 0.0045 | 0.03 J |
| x265 `ultrafast` | 76 | 0.081 | 0.33 J |
| x265 `medium` CRF (10-bit) | 27 (21) | 0.245 (0.315) | 0.76 J (ABR run) |
| x265 `slow` CRF | 12 | 0.584 | not measured |

x265 ran at `nice 10` on all 8 cores with other work on the machine, so
its fps is somewhat pessimistic. Energy is the SMC "Total System Power"
above an idle of 6.1-7.4 W (`power.log`). That makes AVE about 10x (vs
`ultrafast`) to 25x (vs `medium`) less energy per frame.

So AVE is a fit where throughput or energy matters and a 15-25 % larger
file (vs `medium`) is acceptable. It is not one where size at equal
quality is the goal and a slow software encode is affordable.

## 1. Why AVE spends more bits [I]

What the driver asks the firmware for today: P frames only (B frames stall
the pipe, docs/81), one reference, no lookahead, one QP for I and P frames
in fixed-QP mode (`ave_session.c`, `qp_i = qp_p`), and no psychovisual
tuning. x265 `medium` uses B frames, 3 references, a 20-frame lookahead,
cutree and AQ; `slow` searches harder. In the bitrate runs, x265 `medium`
without B frames (`--bframes 0`, results.csv `x265-medium-noB`) closes
part of the gap. Driver-side candidates, cheapest first: a P-frame QP
offset in fixed-QP mode, two references, then B frames (docs/81).

## 2. Test material

- **Public:** the Xiph "derf" SVT 1080p50 set
  (<https://media.xiph.org/video/derf/y4m/>): crowd_run, park_joy,
  ducks_take_off, in_to_tree, old_town_cross. **Frames 0-199** of each
  (4 s, 8-bit 4:2:0), fetched byte-exact with HTTP range requests
  (`prepare.sh`). Anyone can reproduce these inputs.
- **Private:** two surveillance-camera recordings (`cam-a` 2560x1440 at
  20 fps, 200 frames; `cam-b` 2304x1296 at 15.1 fps, 181 frames), H.264
  High at ~1.1-1.2 Mbit/s, decoded to raw. They stand for "re-encoding
  something that is already compressed". They cannot be distributed, so
  their rows are for context only. `cam-b`'s header claims 25 fps; the
  measured rate, 15.1, is what the bitrates use.

## 3. Encoders and settings

All encoders got the same raw input, keyint 250 (one IDR per clip), 1 pass.

| name in results.csv | what |
|---|---|
| `ave` | ffmpeg 8.1.2 `hevc_v4l2m2m -b:v {500k…16M}` (the driver's VBR controller) |
| `ave-cqp` | `v4l2-ctl`, rate control off, `hevc_i_frame_qp_value` 22-38 |
| `ave-cqp-main10` | as `ave-cqp` with `hevc_profile=2` (NV12 in, 10-bit coding), QP 34-50 (10-bit QP = 8-bit QP + 12) |
| `ave-cqp-p010` | P010 input (one clip, one QP): same result as NV12 → Main 10 (42.360 dB, 181.2 vs 181.3 Mbit/s) |
| `x265-{ultrafast,fast,medium}` | x265 4.1 (NEON, Neon_DotProd), `--bitrate` at AVE's targets |
| `x265-medium-noB` | `--preset medium --bframes 0` |
| `x265-{medium,slow}-crf` | `--crf 18-34` |
| `x265-medium-crf-10bit` | x265 4.1 HIGH_BIT_DEPTH, `--input-depth 8 --output-depth 10`, CRF 18-34 |

## 4. Scoring

- Decoded output against the source, frame by frame on raw YUV (not by
  container timestamps: ffmpeg would pair a 50 fps HEVC stream with a
  25 fps raw input wrongly).
- PSNR (Y and the 4:1:1 average) and SSIM from ffmpeg. VMAF from libvmaf
  3.0.0, model `vmaf_v0.6.1.json`.
- 10-bit outputs are scored at 8 bits against the 8-bit source with
  `-sws_dither none`. **swscale dithers by default** when reducing bit
  depth: on a random 10-bit ramp its default matched round(v/4) for 76 % of
  samples, `none` for 100 % [C]. Dithering would have handed Main 10 a
  noise penalty.
- **BD-rate** with PCHIP (monotone piecewise-cubic) interpolation of
  log-rate over quality, as in the JVET/AOM tools. VMAF points ≥ 99 are
  dropped (saturation). Means only include clips whose two quality ranges
  overlap by ≥ 30 %. The classic single-cubic BD gave +217 % where ranges
  barely overlapped, so it is not used for conclusions. Checks: known shifts
  come out exact (+10 %, −20 %), and the control (x265 `slow` vs `medium`
  CRF) comes out at −16.8 % PSNR-Y / −30.6 % VMAF on the public set, i.e.
  the method ranks the presets correctly.

## 5. Findings on the way

- **ffmpeg's V4L2 m2m wrapper segfaults with a 1080-line input.** The
  driver rounds the OUTPUT height to 1088 (1080 is not a multiple of 16) and
  the wrapper mishandles it. The driver itself survives: Stop/Close, buffers
  returned, the PMP vote released. `v4l2-ctl` with an OUTPUT crop works
  (the fixed-QP runs). For the ffmpeg runs the 1080p clips were fed as
  1088 lines with the last row repeated, and scored after cropping back
  (a <1 % handicap for `ave`).
- **The HEVC level control defaults to 4**, which is too low for 2560x1440
  (level 5). The stream decodes fine; the SPS just under-reports it.
- **x265's 1-pass ABR is erratic on 4-second clips** (`medium` scored below
  `fast` on in_to_tree and old_town_cross). The CRF rows are the ones to
  compare against.
- **Main 10 is free on AVE** (same fps, same CPU); on x265 it cost 28 %
  more CPU for about no gain on these clips.

## 6. Caveats

4-second excerpts with one IDR each: rate control and lookahead behave
differently over full-length material. Expect the same ranking, not the
same decimals. VMAF rewards x265's psychovisual defaults, and PSNR is
neutral; both are given. One machine, one driver state (docs/87, fixed QP
with I = P).

## 7. What bitrate for "good" 1080p and 4K? (guidance)

How the results above translate into bitrates for storing ordinary video.
The published service figures are approximate and from general knowledge,
not measured here [I]; check the services' current documentation before
relying on them.

| | 1080p | 4K |
|---|---|---|
| YouTube recommended *upload* bitrate (H.264, SDR; 24-30 / 50-60 fps) | 8 / 12 Mbit/s | 35-45 / 53-68 Mbit/s |
| YouTube *delivery* (VP9/AV1) | ~2.5-4 Mbit/s | ~12-20 Mbit/s |
| Netflix *delivery* (HEVC/AV1, per-title encoding) | ~3-6 Mbit/s (H.264 up to ~6-8) | ~8-16 Mbit/s (long a fixed ~15 Mbit/s) |
| Blu-ray / UHD Blu-ray | ~20-40 Mbit/s (H.264) | ~50-100 Mbit/s (HEVC) |

Streaming delivery is "good", not transparent: Netflix aims at roughly
VMAF 93-95 for its top rung.

Suggested HEVC targets for keeping video at that level (x265 `slow`/`medium`,
24-30 fps) [I]:

| content | 1080p | 4K |
|---|---|---|
| clean digital, animation, talking heads | 2-4 Mbit/s | 6-12 Mbit/s |
| typical film/TV, streaming-service quality | **4-8 Mbit/s** | **12-20 Mbit/s** |
| near-transparent, or grainy/noisy film | 8-15 Mbit/s | 20-40 Mbit/s |

- 4K needs roughly 2.5-4x the 1080p bitrate, not the 4x its pixel count
  suggests. 50/60 fps adds about 30-50 %.
- **AVE: add ~20 %** (§ Summary): about 5-10 Mbit/s for 1080p and
  15-25 Mbit/s for 4K at streaming-service quality.

Why the ranges are wide: the bitrate needed for **VMAF 93** at 1080p in
this benchmark, interpolated from `results.csv` [C]:

| clip | x265 slow CRF | x265 medium CRF | AVE fixed QP |
|---|---|---|---|
| cam-a, cam-b (static scenes, already compressed) | 0.4-1.7 Mbit/s | 0.4-2.1 Mbit/s | 0.5-2.0 Mbit/s |
| in_to_tree, old_town_cross (calmer) | 7-9 Mbit/s | 15-17 Mbit/s | 15-23 Mbit/s |
| crowd_run, park_joy, ducks_take_off (hardest) | 21-45 Mbit/s | 27-68 Mbit/s | 29-72 Mbit/s |

The derf clips are deliberate torture tests (50 fps, grain, water,
crowds): an upper bound, not typical content.

In practice:
- **Target quality, not bitrate.** Use x265 CRF ~20-22 (`slow`) or AVE
  fixed QP, and let each title find its own size. A fixed bitrate wastes
  bits on easy material and starves hard material.
- **The source caps the result.** Re-encoding a lossy file never improves
  it. HEVC typically keeps an H.264 source's quality at about 50-70 % of
  its bitrate, and going above the source's own bitrate buys nothing.
- **Calibrate on a few titles first:** encode two or three representative
  titles at a couple of settings and score them (`bench.py`'s `measure()`
  or the `vmaf` tool) before committing a whole collection.
