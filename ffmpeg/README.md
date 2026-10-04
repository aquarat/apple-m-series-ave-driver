# ffmpeg for archival transcoding on the AVE

A patch series for FFmpeg's V4L2 mem2mem encoder wrapper (`h264_v4l2m2m`,
`hevc_v4l2m2m`) that makes a plain

```
ffmpeg -i in.mkv -c:v hevc_v4l2m2m -qp 24 -bf 1 -g 250 -c:a copy out.mkv
```

work on this driver: B frames, constant QP, Matroska/MP4 output, any width
(720), 1080-line pictures, and a choice of encoder node. Before it, ffmpeg
forced B frames off, had rate control only, could not write Matroska or MP4
("Could not write header"), sheared 720-wide pictures (PSNR ~10 dB),
crashed on 1080 lines and took whichever `/dev/video*` came first, which is
why `tools/ave-transcode.sh` goes through `v4l2-ctl` and `mkvmerge`.

- `patches/`: the series, `git format-patch` output.
- `build-ffmpeg-ave.sh`: clones FFmpeg, applies the series, builds a lean
  static ffmpeg/ffprobe into `$HOME/ffmpeg-ave` and runs the unit test.
- `test/fake-ave-v4l2.c`, `test/fake-encode-test.py`: an emulation of this
  driver's V4L2 interface (LD_PRELOAD, no hardware) and the end-to-end checks
  run against it. See "Testing without the hardware".

## Base

Made on FFmpeg 8.1 plus the v4l2-request series (Kwiboo's
`v4l2-request-n8.1` branch, as used for the AVD decoder) and two local
v4l2-request commits; that tree's `v4l2_m2m*.c`/`v4l2_buffers.c`/
`v4l2_context.c` are identical to FFmpeg 8.1's. The series applies as is
(`git am`, no fuzz) to **plain FFmpeg n8.1** too, which is what
`build-ffmpeg-ave.sh` uses by default (`FFMPEG_REF`, `FFMPEG_REPO` to
change it): the archival build needs no v4l2-request decoding. Checked both
ways: applied to the 8.1 release tree and to the v4l2-request tree, built
on aarch64 (Fedora Asahi 44, gcc), unit test passing.

## The patches

| # | patch | what it does |
|---|---|---|
| 1 | `avcodec/v4l2_buffers: honour the OUTPUT format's bytesperline and height` | Copies frames into OUTPUT buffers at the driver's `bytesperline`, with the chroma plane at `bytesperline * height` (single-planar) or in its own plane (NV12M ...), reading only the frame's own lines: fixes the sheared 720-wide picture and the 1080 -> 1088 crash (it read 8 lines past the frame). Repeats the last column/line into the padding. Maps `V4L2_PIX_FMT_P010`. New `libavcodec/v4l2_enc_utils.c` (device-free helpers) and its unit test `fate-v4l2-enc-utils`. |
| 2 | `avcodec/v4l2_m2m_enc: crop the OUTPUT to the picture size` | When the driver rounds the OUTPUT format up, sets `VIDIOC_S_SELECTION` (CROP) to the picture size, so the SPS carries 1920x1080, not 1088. |
| 3 | `avcodec/v4l2_m2m_enc: take yuv420p and yuv420p10 frames for NV12/P010 devices` | Interleaves the chroma while copying, so the usual yuv420p decoder output needs no `-pix_fmt nv12`; yuv420p10le goes to P010 (HEVC Main 10). Other formats still need converting (the error says how). |
| 4 | `avcodec/v4l2_m2m: add a "device" option to pick the V4L2 node` | `-device /dev/videoN`, or a name: node (`video3`), V4L2 name (`apple-ave1-enc`), driver, card or bus_info. Without it, nodes are probed in numeric order. |
| 5 | `avcodec/v4l2_m2m_enc: add constant-QP coding` | `-qp N` (or `-q:v N`): rate control off, `BITRATE_MODE=CQ`, `{H264,HEVC}_{I,P,B}_FRAME_QP` = N, N+`qp_p_offset` (0), N+`qp_b_offset` (3). Without it nothing changes: rate control at `-b:v`. |
| 6 | `avcodec/v4l2_m2m_enc: support B frames` | `-bf N` to `V4L2_CID_MPEG_VIDEO_B_FRAMES`, read back; DTS derived (below); `has_b_frames`; picture type and QP in the packets' stats; a receive loop that does not deadlock on held B frames; packets copied out and CAPTURE buffers requeued at once; header-only packets merged; the EOS event no longer drops the last packets. |
| 7 | `avcodec/v4l2_m2m_enc: provide extradata for global-header muxers` | For Matroska/MP4 (`AV_CODEC_FLAG_GLOBAL_HEADER`): codes one black frame at init, keeps its VPS/SPS/PPS (SPS/PPS) as extradata, then STREAMOFF/STREAMON so the real stream starts afresh. |

## Options

| option | default | |
|---|---|---|
| `-qp N` | -1 (rate control) | constant QP (I frames); also `-q:v N` |
| `-qp_p_offset N` | 0 | P QP = I QP + N |
| `-qp_b_offset N` | 3 | B QP = I QP + N (the driver's own default, docs/96) |
| `-bf N` | 0 | B frames between references; the driver keeps 0..2 |
| `-g N` | 12 (ffmpeg's) | IDR interval. **Set it** (e.g. 250): every key frame is an IDR (closed GOP), and 12 costs bits |
| `-device D` | first that works | `/dev/video3`, `video3`, `apple-ave1-enc`, `apple-ave`, ... |
| `-b:v R` | 200k (ffmpeg's) | rate control target when there is no `-qp` |

Examples:

```
# HEVC at fixed QP 24, one B frame, 10 s GOPs, audio and subtitles copied
ffmpeg -i in.mkv -map 0 -c:v hevc_v4l2m2m -qp 24 -bf 1 -g 250 -c:a copy -c:s copy out.mkv

# interlaced SD (DV, DVD, broadcast): deinterlace first; one frame per frame
ffmpeg -i tape.dv -vf bwdif=mode=send_frame -c:v hevc_v4l2m2m -qp 22 -bf 1 -g 250 \
       -c:a flac out.mkv

# 10-bit HEVC (Main 10) from a 10-bit source
ffmpeg -i in.mov -pix_fmt yuv420p10le -c:v hevc_v4l2m2m -qp 24 -bf 1 -g 250 -c:a copy out.mkv

# pick the second encoder (M1 Ultra), e.g. to run two transcodes at once
ffmpeg -i a.mkv -c:v hevc_v4l2m2m -device apple-ave-enc  -qp 24 -bf 1 -g 250 -c:a copy a.out.mkv &
ffmpeg -i b.mkv -c:v hevc_v4l2m2m -device apple-ave1-enc -qp 24 -bf 1 -g 250 -c:a copy b.out.mkv

# rate control, as before
ffmpeg -i in.mkv -c:v hevc_v4l2m2m -b:v 4M -g 250 -c:a copy out.mp4
```

Sources other than yuv420p/yuv420p10le (DV NTSC's yuv411p, ProRes' yuv422p10,
MJPEG's yuvj422p) need `-pix_fmt nv12` (or `-vf ...,format=nv12`). The
encoder codes progressive frames only: deinterlace (bwdif/yadif) or IVTC
(fieldmatch,decimate) first.

## Design notes

**DTS.** The driver takes frames in presentation order and returns packets
in decode order (a group's anchor first, then its B frames), each with its
own source's timestamp, so every PTS is right and the DTS must be made.
Packet *n* gets the PTS of the *n - B*'th frame in input order (a FIFO of
input PTS), where B is the number of B frames the driver accepted; the first
B packets get the first PTS minus B frame durations (from `-r`/the frame
rate, else 1 tick). Within a group of B+1 frames no frame moves more than B
places, so this DTS never exceeds the packet's PTS whatever order the group
is coded in, and it rises as the input PTS rise (variable frame rate
included). B is an upper bound: anchor-first groups need only 1 (x264's
`has_b_frames` without pyramid), and when the driver latches fewer B frames
than the control says (GOP shorter than B+2, H.264 Baseline, H.264 on t8103)
the DTS are still valid, just later. `has_b_frames` is set to B. This is
the approach of nvenc/VideoToolbox (a timestamp queue shifted by the
reorder delay), with the shift taken from earlier input PTS instead of
subtracting a fixed duration, so VFR input stays right.

**Not blocking on held frames.** The old receive loop queued a frame and
then blocked for a packet, which deadlocks the moment the driver holds a B
frame for its anchor. It now waits for a packet only when the device has
more frames in flight than it may hold back (the B frames plus one being
coded, which also overlaps the copy of the next frame with the coding of
the last); otherwise it returns EAGAIN for more input. Draining waits for
the LAST buffer; the EOS event (which this driver queues just before the
LAST buffer is done) no longer ends the drain early, and after it the wait
is bounded (2 s).

**Extradata.** Matroska and MP4 need the parameter sets at header time, and
the Matroska muxer ignores `AV_PKT_DATA_NEW_EXTRADATA` for H.264/HEVC, so
side data on the first packet does not help. The driver offers only
`HEADER_MODE_JOINED_WITH_1ST_FRAME`. So, when the muxer wants a global
header, the encoder codes one black frame during init, takes the parameter
sets in front of its first slice, then STREAMOFF both queues (the driver
ends that session) and requeues the CAPTURE buffers; the real stream starts
a new session at the next STREAMON with the same controls, hence the same
parameter sets, which stay in-band on every IDR as well. Cost: one extra
session start/stop per run (MPEG-TS and raw output skip it).

**Copying packets out.** CAPTURE buffers are copied into ordinary packets
and requeued at once: a B group needs B+1 free CAPTURE buffers, and the
muxer's interleaving queue may hold packets for a long time.

## Testing without the hardware

The unit test (`libavcodec/tests/v4l2_enc_utils`, also `make
fate-v4l2-enc-utils`) covers the copy (720x576 at 768 bytes per line, 1080 in
1088 lines, P010, I420, NV12M, yuv420p->NV12, yuv420p10->P010), the DTS
derivation against an emulation of `ave_gop.h` (0-2 B frames, GOP 0-250,
drains, shuffled groups, VFR, plus a control that must fail without the
delay), the header-only test and the parameter-set extraction.
`build-ffmpeg-ave.sh` runs it.

`test/fake-ave-v4l2.c` emulates this driver's V4L2 interface for user space
(formats and rounding, crop, controls, `ave_gop.h` itself for the B-frame
plan, completion order, flags, the drain, poll, STREAMOFF/ON) behind
LD_PRELOAD at a path that does not exist, and copies its "coded" packets
from a template H.264 stream. `test/fake-encode-test.py` runs ffmpeg
through it and checks packet counts, the PTS set, DTS monotonic and <= PTS,
key frames, extradata, B-frame reordering, the session restart, the controls
(QP, rate control, B-frame clamp) and, byte for byte, the picture the device
received against ffmpeg's own conversion (stride, crop, chroma placement,
yuv420p->NV12, yuv420p10->P010):

```
cc -O2 -Wall -shared -fPIC -Idriver -o /tmp/fake-ave-v4l2.so ffmpeg/test/fake-ave-v4l2.c -ldl -lpthread
python3 ffmpeg/test/fake-encode-test.py --ffmpeg ~/ffmpeg-ave/bin/ffmpeg \
    --ffprobe ~/ffmpeg-ave/bin/ffprobe --fake /tmp/fake-ave-v4l2.so \
    --template-ffmpeg /usr/bin/ffmpeg     # any ffmpeg with libopenh264 or libx264
```

It always passes `-device` with the fake path, so ffmpeg never opens a real
node. It cannot tell whether the real firmware does what the emulation does;
that is the hardware test plan's job.

## Hardware results (2026-10-04, M1 Ultra, built with build-ffmpeg-ave.sh)

| test | command (abridged) | result |
|---|---|---|
| H1 | 720x576 25p + FLAC -> `.mkv`, `hevc_v4l2m2m -qp 24 -bf 1 -g 250` | 200/200 frames, `IBPBP…`, extradata 108 bytes, DTS <= PTS and rising on every packet, PSNR-Y 48.246593 (the same as `tools/ave-transcode.sh` with v4l2-ctl at the same QP) |
| H2 | 1920x1080 25p -> `.mkv`, same | 250/250, reported as 1920x1080 (crop, not 1088), PSNR-Y 48.89 |
| H3 | 720x576 -> `.mkv`, `h264_v4l2m2m -qp 24 -bf 1` | 200/200, `IBPBP…`, extradata 29 bytes |
| H4 | 1080p -> `.mp4`, `-bf 2` | 250/250, `IBBPBBP…` |
| H6 | `-device apple-ave2-enc` | runs on that encoder |
| H7 | `-b:v 8M` (no `-qp`) | rate control as before, IPPP |
| H9 | 1, 2, 3 and 7-frame inputs with `-bf 2` | every frame out |
| H11 | interlaced 720x576 with `-vf bwdif=mode=send_frame:parity=auto:deint=all` | 150/150 |
| parallel | four of H2 at once, no `-device` | all four complete, identical output; the driver's `open_balance` (docs/98 §12) put them on the four encoders |
| metadata | `-map_metadata 0` | `creation_time` carried into Matroska (without it ffmpeg drops it, as it always has) |

Not run: H5 (implicitly fine: every output decodes from its header), H8 (MPEG-TS), H10
(Main 10), H12 (throughput comparison).

## Hardware test plan (for the lead, on the M1 Ultra)

Fresh boot, driver loaded as usual (`tools/ave-load.sh`), node names from
`/sys/class/video4linux/video*/name`. After each step: `dmesg | tail`, no
`v4l2:` errors. Shell setup:

```
FF=$HOME/ffmpeg-ave/bin/ffmpeg FP=$HOME/ffmpeg-ave/bin/ffprobe
ffmpeg/build-ffmpeg-ave.sh                      # ends with "v4l2_enc_utils: all tests passed"
$FF -f lavfi -i testsrc2=s=720x576:r=25   -f lavfi -i sine=r=48000 -t 20 -pix_fmt yuv420p -c:v ffv1 -c:a flac src576.mkv
$FF -f lavfi -i testsrc2=s=1920x1080:r=25 -f lavfi -i sine=r=48000 -t 20 -pix_fmt yuv420p -c:v ffv1 -c:a flac src1080.mkv
# checks, as functions
dts_ok()  { $FP -v error -select_streams v -show_entries packet=pts,dts -of csv=p=0 "$1" |
            awk -F, 'NR>1 && $2<=d {b++} $2>$1 {b++} {d=$2} END {print (b ? "BAD " b : "ok")}'; }
count()   { $FP -v error -select_streams v -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$1"; }
types()   { $FP -v error -select_streams v -show_frames -show_entries frame=pict_type -of csv=p=0 "$1" | sort | uniq -c; }
psnr()    { $FF -hide_banner -i "$1" -i "$2" -lavfi '[0:v][1:v]psnr' -f null - 2>&1 | grep -o 'average:[0-9.inf]*'; }
decodes() { $FF -v error -xerror -i "$1" -f null - && echo decodes; }
params()  { $FP -v error -select_streams v -show_entries stream=codec_name,profile,width,height,coded_height,has_b_frames,extradata_size,pix_fmt -of default=nw=1 "$1"; }
```

| # | what | command | pass |
|---|---|---|---|
| H1 | fixed QP + B to Matroska, SD | `$FF -i src576.mkv -c:v hevc_v4l2m2m -qp 24 -bf 1 -g 250 -c:a copy h1.mkv` | `count h1.mkv` = 500; `decodes h1.mkv`; `types` shows B (~half) and I/P; `psnr h1.mkv src576.mkv` > 35 dB (the old stride bug gave ~10); `params`: hevc, 720x576, extradata_size > 0 |
| H2 | the same to MP4 | `... -bf 1 ... h2.mp4` | `dts_ok h2.mp4` = ok; `count` 500; decodes; PSNR as H1 |
| H3 | 1080 lines, 2 B frames | `$FF -i src1080.mkv -c:v hevc_v4l2m2m -qp 24 -bf 2 -g 250 -c:a copy h3.mkv` (and `.mp4`) | no crash; `params`: 1920x1080 (not 1088); `count` 500; `dts_ok` on the mp4; PSNR > 35 dB; log has no "cannot crop" |
| H4 | H.264 | H1-H3 with `-c:v h264_v4l2m2m` | as above (H.264 B frames only where the driver allows them; it says so in dmesg) |
| H5 | parameter sets: header = stream | `$FF -i h1.mkv -c copy -bsf:v trace_headers -f null - 2>&1 \| grep -A3 'Sequence Parameter Set' \| head -40` and `tools/hevc_parse.py` on the first IDR | extradata's VPS/SPS/PPS equal the in-band ones of the first IDR (same sizes, same fields) - the dummy-frame session and the real one must agree |
| H6 | device selection | `-device /dev/videoN`, `-device apple-ave1-enc`, then two transcodes at once on the two nodes | log "Using device ..." is the one asked for; both finish; outputs as H1 |
| H7 | rate control unchanged | `$FF -i src1080.mkv -c:v hevc_v4l2m2m -b:v 4M -g 250 h7.mkv` | no "Constant QP" in `-v verbose` log; bitrate near 4 Mbit/s (`$FP -show_entries format=bit_rate`), as before the series |
| H8 | MPEG-TS (no global header) | `... -qp 24 -bf 2 -g 250 h8.ts` | `dts_ok` ok; count; decodes; only one session in dmesg |
| H9 | short streams, the drain | `-frames:v N` for N = 1, 2, 3, 7 with `-bf 2`, to `.mp4` | `count` = N each time; `dts_ok` ok; no hang (the EOS/LAST ordering) |
| H10 | Main 10 | `$FF -i src576.mkv -pix_fmt yuv420p10le -c:v hevc_v4l2m2m -qp 24 -bf 1 -g 250 h10.mkv` | `params`: profile Main 10, pix_fmt yuv420p10le; PSNR vs source > 35 |
| H11 | a real archival job | an interlaced DV or MPEG-2 file: `-vf bwdif=mode=send_frame -c:v hevc_v4l2m2m -qp 22 -bf 1 -g 250 -c:a flac` | plays (mpv), A/V in sync over the whole file, duration equals the source's |
| H12 | throughput | `time` H3 with `-bf 0`, `-bf 1`, `-bf 2`; compare with `tools/ave-transcode.sh` on the same file | fps at least the v4l2-ctl pipeline's (the copy is the only extra work) |

## Not verified without hardware

- **The second session.** The extradata step stops and restarts the driver's
  session on one file handle (STREAMOFF both queues, STREAMON again). The
  driver supports it on paper (`stop_streaming` ends the session, the next
  `start_streaming` opens one), but nothing exercised it until now. If it
  misbehaves: write `.ts` (no global header, no restart) and remux with
  `ffmpeg -i x.ts -c copy x.mkv`. H5 checks that both sessions produce the
  same parameter sets; if they differ (say the PPS's init QP followed rate
  control state), the in-band ones still win in every decoder, but the
  CodecPrivate would be stale.
- **Timing.** The emulation completes a group's CAPTURE buffers together, as
  `ave_b_batch()` does, and queues EOS before LAST; the real interleaving of
  completions and `poll()` is untested. The receive loop also gives up after
  100 wake-ups without a packet ("The device stopped returning packets")
  instead of spinning on a queue in error.
- **`has_b_frames` vs. the latched B count.** `G_CTRL(B_FRAMES)` returns the
  value set, not what STREAMON latched (fewer for GOP < B+2, H.264 Baseline,
  H.264 on t8103), so the reorder delay can be larger than needed; harmless.
- **Colour.** ffmpeg passes no colorimetry in the OUTPUT format, so the
  driver signals its default (Rec. 709) in the stream whatever the source
  (SD is Rec. 601); the containers do carry the source's tags. A follow-up
  patch could pass `avctx->colorspace`/`color_range`/... through.
- `-g` default: ffmpeg's 12 means an IDR every 12 frames; always pass `-g`.
- The defaults that are ffmpeg's, not the driver's: `-b:v` 200k in rate
  control mode, as before this series.
