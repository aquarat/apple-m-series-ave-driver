# 92. Trading encode time for compression: what the hardware offers

*2026-10-03, on the M2 (t8112, docs/90). Can AVE spend more time per frame
for fewer bits at the same quality? The options, cheapest first.*

## Summary

| lever | status | effect |
|---|---|---|
| A speed/quality preset | **does not exist**: the motion search is fixed-function and already at its widest window (±192x96, wire 0xFCE0 = 0) | — |
| Separate P-frame QP | **done** (V4L2 `*_P_FRAME_QP`, `*_B_FRAME_QP`) | −1.5 to −2 % bitrate at +2/+3 (§1) |
| HEVC in-loop/prediction tools | already at macOS's settings (SAO, TMVP, WPP on) | — |
| Two or more references | **works since docs/94** (both ME units); was blocked: any frame with two active references stalls the pipe after two macroblocks, on the M2 as on the M1 Max (§2) | not measured separately |
| B-frames | **works since docs/94** in the self-test; V4L2 support in progress (docs/81) | worth ~9 %: x265 `medium` without B frames needs 9.3 % more bitrate (VMAF, these clips) |
| Multi-pass / lookahead | firmware code present (CMultiPassControl, H14G's LRME-RC); host interface not mapped | mainly a rate-control (bitrate-mode) improvement; fixed QP is already AVE's better mode |

## 1. P-frame QP offset

The Start command has separate I/P/B QP fields (wire 0xFFB4/0xFFB8/0xFFBC,
`ave_abi.h`); the driver wrote the same value to all three. It now takes
the standard V4L2 controls `h264_p_frame_qp_value`, `h264_b_frame_qp_value`,
`hevc_p_frame_qp_value`, `hevc_b_frame_qp_value` (0, the default, = the
I-frame QP, so nothing changes unless asked). The firmware honours it: at
I-QP 30 on `crowd_run` the stream shrinks from 14.4 MB (+0) to 10.7 MB (+2)
and 8.0 MB (+4).

Bench (`bench/hevc-efficiency`, encoders `ave-cqp-poffN`, M2,
`results-m2-poff.csv`), PCHIP BD-rate against plain fixed QP on the same
machine, negative = fewer bits for the same quality:

| P offset | crowd | park | ducks | tree | town | mean VMAF | mean PSNR-Y |
|---|---|---|---|---|---|---|---|
| +1 | −0.1 | +0.9 | +0.7 | +1.0 | −1.9 | +0.1 % | −0.1 % |
| +2 | −1.2 | +0.8 | +0.0 | −2.2 | −3.1 | −1.1 % | −1.4 % |
| +3 | −1.4 | +1.7 | +0.4 | −5.3 | −3.2 | −1.6 % | −2.1 % |
| +4 | −1.9 | −0.1 | +0.2 | −4.3 | −3.4 | −1.9 % | −1.3 % |
| +6 | −2.2 | +0.9 | +1.2 | −3.3 | −3.1 | −1.3 % | −0.1 % |

A small, consistent gain on the slow clips (`in_to_tree`, `old_town_cross`,
3-6 %), none on the fast ones. With P-only coding, every P frame is a
reference for the next, so a large offset degrades the whole chain; +2 to
+3 is the useful range. Against x265 `medium` the gap stays ~20 %.

## 2. Two references and B-frames on the M2

The M1 Max's runs (docs/53 bs1, hb3, b4, mq1; docs/81) were repeated on
the M2 with the same parameters (probe-time self-test, 1280x720):

| run | what | M2 result |
|---|---|---|
| bs1 | H.264 IPP, RefSpacingP 2: frame 2 has two L0 references | frame 1 (one reference) 2524 bytes, **as on the M1 Max**; frame 2 `PIPE HANG: 3, 3` |
| hb3 | HEVC, two references | frames 0-1 complete, frame 2 hangs |
| b4 | H.264 IDR, {B1, P2}: the firmware reorders | P2 completes (2232 bytes), B1 hangs |
| mq1 | bs1 with every macOS Start/Process group (`session_macos=0x3ff`) | frame 1 2637 bytes (as on the M1 Max), frame 2 hangs |

Every run matches the M1 Max's byte for byte up to the hang, and the hang is
the same. A hang only wedges the firmware: unloading and reloading the
driver recovers it (docs/84 R7), and each time a normal encode afterwards
gave the usual 44.308053 dB.

Also ruled out on the M2: the SMMU. The pixel path translates through it
(docs/90 §5.2) and Linux has no handler for its faults, so a stalled
translation would be silent; its error registers read clean at the hang
(`regdump=1`: only the constant 0x800 at +0). The MCPU images come from the
firmware image itself (docs/57 §2.2), so no host-supplied microcode is
missing either.

With the commands, the firmware build, the SoC, the DPE tunables, the
clocks (docs/53 bs5) and the DART streams (bs6) all varied without effect,
what remains is state outside the commands that macOS sets up and we do not.
The way to find it is to watch macOS do a two-reference encode, with m1n1's
hypervisor tracing the encoder's MMIO and the DARTs (§3).

## 3. Proposal: trace macOS

m1n1 can boot macOS as a guest and log every MMIO access to chosen ranges
(`proxyclient/m1n1/trace/`). Recording macOS's own HEVC encode with B frames
(VideoToolbox, e.g. `ffmpeg -c:v hevc_videotoolbox -bf 2`), then diffing
its register writes and DART/SMMU set-up against ours, is how the M1 GPU,
DCP and AVD drivers were brought up. It needs:

- a machine with macOS 13.5 (or the stub's version) installed beside Fedora;
- a second machine connected by USB-C running the m1n1 proxyclient, as the
  hypervisor's console and trace sink;
- booting that macOS through m1n1 in hypervisor mode for the session (a
  one-off boot; the normal boot chain is not changed).

docs/93 then ran the firmware's per-frame code under emulation from live
snapshots: it programs the second reference completely and the hardware
accepts every write, so the trace should look outside the per-frame
registers. Until then, B-frames and multiple references stay parked; the driver's
experiment switches (`session_bframes`, `session_ref_spacing_p`,
`session_hevc_refs`) remain for the trace's follow-up.
