# 68 — The userspace interface

**Status: design only. Nothing here has been implemented, and nothing here has
run.** This document answers "what should userspace see, and what must the
driver become to show it?" It is not a reverse-engineering document: its
sources are the mainline kernel's own conventions, ffmpeg's `h264_v4l2m2m`
backend, and the hardware constraints established in
[53](53-first-frame.md)–[67](67-bitstream-output.md).

Everything about the hardware below is quoted from those documents with their
own confidence marks carried over: **C** read out of a disassembly, **I**
inferred, **U** unknown, and **HW** actually observed on this M1 Max. Design
proposals are marked **[proposal]**; they have no confidence mark at all,
because they are opinions about Linux, not findings about Apple.

## 0. The standing caveat

The encoder does not work yet.

F16/F17 ([53](53-first-frame.md) §28, **HW**) produced a syntactically valid,
`ffmpeg`-decodable 1280x720 Baseline I-frame — **whose every pixel is luma
130**. The source DMA never read the host's picture. Two Start_AVC scalars
(`0xFEC0` `src_mode`, `0xFCE8` `src_cfg_byte`) are **U** and currently zero,
and F18 — the one-variable discriminator that decides whether the source path
is a programming problem or an addressing problem — **has not been run**.

Every line of this document is therefore conditional on a fix that does not
exist. It is still worth writing now, because the answer to "which uapi"
changes what the *next* pieces of driver code should look like (a real client
object, a real completion path, a real teardown), and those are being written
anyway. But a milestone plan that opens with "ffmpeg encodes a file" would be
dishonest: the plan in §7 opens with "the encoder encodes the right picture",
which is not a uapi task at all.

Two further standing constraints from [63](63-teardown.md) that shape
everything here:

- **`rmmod` has twice been followed by a machine reset seconds later**, once
  after a *successful* encode with no faults logged (**HW**). Until that is
  understood, a device node that userspace can open and close repeatedly is a
  more dangerous object than the current module-parameter self-test, not a
  safer one.
- **A bad field wedges the coprocessor.** The firmware's failure mode for an
  out-of-range parameter is `_bsp_assert_fail` followed by `b .` — a dead core
  and, so far, a reboot — not an error reply (**C**,
  [64](64-multiframe.md) §1.4). Every validation a normal V4L2 driver can
  afford to leave to the hardware, this one must do in the host, and must do
  conservatively. **`-EINVAL` is always cheaper than finding out.**

---

## 1. What the hardware forces on any interface

### 1.1 The command lifecycle

| command | wire | scope | notes |
|---|---:|---|---|
| Config | 1 | **once per firmware boot** | publishes the FwIPC region, carved into rings that live there permanently (**C**, [63](63-teardown.md)) |
| Open | 2 | **per client** | registers a client id |
| Start_AVC | 4 | **per session** | carries *almost the entire* configuration, including every buffer table |
| Process | 7 | **per frame** | the only per-frame command |
| Stop | 6 | per client | cancels queued slots; `UNINIT_DONE` is **withheld until produced == consumed** (**C**) |
| Close | 12 | per client | `STOP_DONE` arrives only after the Close is *dequeued* behind the client's remaining work and `DestroyClient` runs (**C**) |
| Halt | 14 | **per device, terminal** | ends in `wfi` forever; recovery needs a full reload (**C**, [55](55-halt-command.md)) |
| Flush | 11 | per client | force-complete everything queued, then reply `0xE09` (**C**) |

Two properties of this list dominate the design:

1. **Config and Halt are device-scoped and Halt is terminal.** There is
   exactly one firmware boot per module load, and the only way to stop the
   firmware also makes it unusable until the driver is reloaded. So the device
   node's lifetime and the firmware's lifetime are not the same thing, and
   `close(fd)` must not be allowed to reach Halt.
2. **Start_AVC is a session, and Stop+Close destroy the client, not the
   session.** macOS has no observed path that reuses a client across a Stop
   ([63](63-teardown.md) from `AVE_Drv::StopClient`, **C**), and whether Stop
   alone leaves a restartable client is **U**. A second Start_AVC in one module
   load has never been sent by anything we have run.

### 1.2 What Start_AVC latches

From [64](64-multiframe.md) §2, [65](65-pframes.md) §2/§5,
[66](66-ratecontrol-sizing.md) §1–§5 (all **C** unless noted):

- **Resolution.** `H264VideoEncoderDPB`'s constructor computes the whole
  session's plane geometry once from `ctrl[2700]/[2704]`, and every chroma
  address in the session is derived from it.
- **The QP triple** `QP_I`/`QP_P`/`QP_B` (wire `0xFFB4`/`0xFFB8`/`0xFFBC`).
  Under fixed QP (`ui32RCFlag = 2`) there is **no per-frame QP field
  anywhere**; a whole-image scan found no per-frame QP writer
  ([66](66-ratecontrol-sizing.md) §2.4, **C** for the scan, **I** for the
  consequence). Changing QP means a new Start_AVC.
- **Rate-control mode, target bitrate, min/max QP, frame rate and frame-rate
  divisor** — all in the `AVEFWRCSettings` block at `0xFF30`.
- **The DPB**: `max_num_ref_frames` (`0x109DC`), and the recon, LowResRef,
  colocated, LowResResult and entropy tables, one entry per slot. These are
  copied **once** into the firmware's DPB context by `ProvideReferenceFrames`
  at Start; the firmware indexes its own copy per frame.
- **The coded-buffer pool**: `CodedData` address table (`0x4B8`, 20 entries),
  size table (`0x558`), `CodedHeader` table (`0x5C0`).
- **The slice map** (`0xFDAC`), entropy mode, and every SPS/PPS syntax flag —
  the parameter sets are generated exactly once, at Start, into a separate
  buffer, and there is **no per-IDR regeneration**
  ([67](67-bitstream-output.md) §3, **C**).

What is *not* latched — the complete per-frame host-owned set — is short:
source luma/chroma IOVA and stride, coded index + the three coded addresses,
`frameNumber`, frame type, context index, `forceKeyFrame`, `forceNonRefFrame`,
a timestamp/duration pair (**U**), and the slice-map entry count.
Everything else the host writes into `PICMGMT` is overwritten by
`setRefPointers` before `setPipe` reads it (**C**, [64](64-multiframe.md) §1.3).

### 1.3 The DPB is the firmware's, and it never tells us anything

The firmware chooses the recon slot, the reference list, the colocated buffer
and the LRME surface; the host chooses the coded index, the command slot, the
frame number, the frame type and the context index (**C**,
[64](64-multiframe.md) §2.1). **The completion carries no buffer identity** —
no DPB slot, no recon index, no byte count.

This is the single fact that decides the uapi (§2): a host that cannot see,
choose, pin or evict a reference picture cannot implement a *stateless* codec
interface, because a stateless interface is defined by userspace doing exactly
those things.

### 1.4 One resource, bounded at 20

`AVE_Client_AcquireOutputBuf` (kext `0xed5f00`) returns the lowest index `i`
whose **coded surface is idle _and_ whose slot `0x328 + 8*(i + 0x15)` has no
outstanding command**; `SendFwCmd_Process` then bounds-checks `i <= 19` and
computes `slot = i + 21` (kext `0xf01188` / `0xf0166c`). The coupling is
enforced in Apple's own allocator, not a naming coincidence (**C**,
[64](64-multiframe.md) §4).

So: **20 coded buffers, indices 0..19, command slots 21..40, and the index and
the slot are one resource.** Worse, the coupling is asserted on the wire:
`ProcessTranscodeStart` compares the per-frame coded address against the
Start-time table entry *for that index* and wedges the core if they differ
(fw `0x58404`, **C**). A coded buffer cannot be registered late, substituted,
or reallocated mid-session.

### 1.5 Ownership and quiesce

- A completed Process releases **that frame's coded output and nothing else**
  ([63](63-teardown.md), **C**).
- Everything published at Start_AVC stays owned by the firmware **until the
  client is destroyed** (Stop + Close both complete), and the firmware frees
  nothing — `DestroyClient` is pure bookkeeping (**C**).
- The FwIPC region, the log ring and the iBoot mapping stay owned **for the
  life of the firmware**: the heartbeat task writes the log ring with no
  command in flight (**C**). F16 freed them under a running core and the
  machine reset (**HW**).
- Source planes are released at `ENCODE_DONE` — **I**, never stated
  explicitly anywhere ([63](63-teardown.md) §5.3 flags this).
- The ordering rule, from macOS: Stop -> `UNINIT_DONE` -> Close -> `STOP_DONE`
  -> ungate the clock -> Halt -> verify -> **unmap and free while still
  powered** -> gate last. And the project's own rule: *never drop the power
  reference on a state we cannot prove is quiescent* ([63](63-teardown.md) §7,
  a proposal that **has never been run**).

### 1.6 Everything else worth knowing before reading §3

- Source format is **linear NV12**, two independent IOVAs, **no per-plane size
  fields on 13.5**, stride non-zero and a multiple of 64 on both planes, both
  addresses 64-byte aligned, and the hardware **fetches `16*ceil(H/16)` luma
  rows regardless of the declared height** (**C**, [53](53-first-frame.md)
  §2.1). Chroma is `stride * 8*ceil(H/16)`.
- Output is **Annex B with 4-byte start codes**, slice NALs only, starting at
  byte 0 of the coded buffer; SPS+PPS live in a separate Start-time buffer
  whose exact length is `coded_hdr[0x98] / 8` (**C**,
  [67](67-bitstream-output.md)).
- The length of a frame comes only from the coded header, as
  `sum(written) - sum(removed)`, plus `3 * numCABACzeroWordInserted` bytes the
  **host** must append (**C** for the arithmetic, **I** for the obligation).
- **The host must zero the coded header before every frame** — the slice
  records are stale otherwise and the walk over-reports (**C** mechanism,
  **U** whether the firmware ever clears it).
- `frameNumber` must be monotone; it is the firmware's queue key, and a
  violation is an assert-and-spin (**C**).
- Resolution limits are **192..4096 x 96..4096, even** (kext `0xea3d9c`, **C**),
  and the driver already enforces them (`ave_session.c`).
- Every IOVA must be **32-bit clean**: the firmware programs only the low 32
  bits of the coded and source addresses (**C**).

---

## 2. Which uapi

### 2.1 The candidates

**V4L2 stateless encoder.** Does not exist. There is a stateless *decoder*
uapi (and this very SoC has one: `drivers/media/platform/apple/avd`, which
selects `V4L2_H264` and `MEDIA_CONTROLLER` — a request-API stateless decoder).
There is no stateless *encoder* uapi in mainline, and if there were, it would
require userspace to supply the reference lists, the slice header and the DPB
management — precisely the three things this firmware does itself and never
reports (§1.3). Ruled out on both counts.

**A DRM-based interface.** DRM has no encode uapi. It would mean inventing
one, and no userspace speaks it. The only reason to consider DRM at all is
buffer sharing, and dma-buf gives us that without leaving V4L2. Ruled out.

**A bespoke chardev.** Tempting, because it is the only option that can
express the hardware exactly: "open a session with these immutable parameters,
get 20 slots, submit frames, get bitstreams". It is also what the current
self-test would most easily grow into. It is the wrong answer anyway: no
existing userspace speaks it, the stated goal is `ffmpeg -c:v h264_v4l2m2m`,
and a private ioctl interface has no path to mainline. It is worth keeping as
a **debug** interface (debugfs already is one) but not as *the* interface.

**V4L2 stateful M2M encoder.** The firmware owns rate control, the GOP model,
the DPB, reference management and bitstream header generation. That is the
definition of a stateful encoder: the client hands over raw frames and gets a
legal elementary stream back, and makes no encode decisions. Every constraint
in §1.2 is a constraint about *when* configuration can change, and V4L2's
stateful encoder spec already has a place for that — configuration happens
before `STREAMON` and the driver may refuse changes afterwards.

### 2.2 Recommendation

**A V4L2 stateful M2M H.264 encoder** — `v4l2_m2m_*`, `videobuf2` with
`vb2_dma_contig`, `V4L2_MEMORY_MMAP` first and `DMABUF` import on the OUTPUT
queue second, `V4L2_CID_MPEG_VIDEO_*` controls, no media controller, no
request API.

**Single-planar only: advertise `V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING`
and not the `_MPLANE` capabilities.** Two planes with two independent IOVAs
looks like the multi-planar model, but the hardware requires the two strides
to be *equal*, so `NV12M` buys nothing — and ffmpeg tests for mplane first
and uses it if it is advertised at all, which would put us on the
less-travelled path through its buffer code for no gain (§3.1). Offering
`NV12M` as a second format later is cheap and harmless.

(For contrast, the decoder on this same SoC — `drivers/media/platform/apple/avd`
— is a *stateless* driver with `MEDIA_CONTROLLER` and `V4L2_H264`. That is
the right answer for hardware that exposes slice-level control, and the wrong
one for this hardware, for the reason in §1.3. The two drivers will not look
alike.)

This is also what [22](22-driver-plan.md) and [04](04-roadmap.md) already
assume, and nothing found since contradicts it. What has changed since those
documents is that we now know *where it does not fit*, which is the rest of
this section.

### 2.3 Where our hardware does not fit the model

These are the mismatches. They are the part of this document worth keeping.

**M1. The capture-buffer pool is a session parameter, capped at 20, with
addresses the firmware asserts against.**
V4L2's model is that `REQBUFS`/`CREATE_BUFS` allocate buffers, and the driver
is told about them when they are queued. Ours must know the **entire set of
coded buffers, their addresses and their sizes, at Start_AVC**, and thereafter
the per-frame address must byte-match the table entry for its index or the
core wedges (§1.4). If the vb2 CAPTURE buffers *are* the firmware's coded
buffers, then: `REQBUFS(count > 20)` must be clamped (`q->max_num_buffers =
20` does this silently and is discoverable by userspace through
`V4L2_BUF_CAP_SUPPORTS_MAX_NUM_BUFFERS`); `CREATE_BUFS` must be refused once
the session is open, and `REMOVE_BUFS` must not be advertised at all; and the
firmware's 20-entry descriptor array has to be indexed by `vb2_buffer.index`
— which is *bounded* by `max_num_buffers` but **not dense**, since
`REMOVE_BUFS` + `CREATE_BUFS` can leave holes.

§5.4 argues that the way out is not to make the vb2 buffers be the coded
buffers at all. That is the single most useful design decision in this
document, because it makes most of this paragraph stop applying.

**M2. Everything V4L2 lets you change per-frame, we fix per-session.**
QP is the sharp case: `V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP` is an ordinary
writable control that userspace may set between frames, and under fixed QP
this hardware has no per-frame QP field at all. Same for bitrate, frame rate,
GOP-adjacent PPS flags, entropy mode, profile and level. §4 is the policy for
all of them.

**M3. There is no per-IDR SPS/PPS.** The firmware generates the parameter sets
exactly once, at Start_AVC, into a separate buffer that never reaches the
coded buffer ([67](67-bitstream-output.md) §3; "not also in the coded buffer"
is **I**, an absence argument). So
`V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR` and
`V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME` are both things the
*driver* must synthesise by memcpy, not things the hardware does. That is
cheap; the honest part is that it means the CAPTURE buffer the driver hands
back is not simply "the coded buffer", and the driver therefore cannot hand
userspace the DMA buffer the firmware wrote into. See M6.

**M4. The completion tells us almost nothing, and the length arrives out of
band.** `ENCODE_DONE` carries no byte count and no buffer identity beyond the
slot. The length is computed by walking slice records in a **`0x23000`-byte
side buffer that the host must zero before every frame**. A dropped frame is
reported as `FrameTypeReturned == 4` in that same side buffer, not as an error
status. Every V4L2 `bytesused`, `V4L2_BUF_FLAG_KEYFRAME` and
`V4L2_BUF_FLAG_ERROR` we set is derived from a parse, not from a register.

**M5. Teardown is a firmware transaction that can fail, and failing means
refusing to free.** V4L2's `STREAMOFF` must return quickly and must leave the
queues reusable; `close()` must always succeed. Ours needs Stop and Close
round trips whose completion latency is **unbounded by anything the host can
see** (Close sits behind the client's remaining queued frames, **C**), and the
project's rule is to leak and stay powered rather than gate on an unproven
state. A `STREAMOFF` that can take seconds, and a driver that can decide to
keep a power reference forever, are both unusual. They are also
non-negotiable.

**M6. We cannot (yet) hand userspace the buffer the firmware wrote.** Three
reasons stack up: the coded buffer's address is pinned by the Start-time table
(M1), the frame may need SPS+PPS prepended (M3), and a multi-slice frame is
not necessarily contiguous — `bytesToRemove` leaves a hole of junk at the end
of a slice that the host must skip ([67](67-bitstream-output.md) §3.6, **C**;
impossible in a single-slice session, which is why the current code gets away
with a straight `memcpy`). So the first driver copies: firmware buffer ->
vb2 CAPTURE buffer. That costs a memcpy per frame of a few hundred KB at
most, and it buys the freedom to make the CAPTURE queue behave like V4L2 says
it should. Zero-copy on the CAPTURE side is a later optimisation and may never
be possible.

**M7. One session, one client, one device — and probably one *ever* per module
load.** The firmware supports up to 0x80 clients, but nothing about
multi-client has ever been analysed, let alone run (**U**), and a second
Start_AVC after a Stop has never been sent by anything (§1.1).

The obvious implementation — `-EBUSY` from the second `open()` — is
**wrong and will fail review**. `v4l2-compliance`'s `testUnlimitedOpens()`
opens the node 100 times and its comment is explicit: *"There should not be
an artificial limit to the number of `open()`s you can do on a V4L2 device
node... please don't start rejecting opens in your driver at 101! There
really shouldn't be a limit in the driver. If there are resource limits, then
check against those limits where they are actually needed."* So: `open()`
always succeeds and always gets its own `v4l2_fh` + `v4l2_m2m_ctx`, and the
single-session limit is enforced at **`start_streaming()`**, which returns
`-EBUSY` if another context already owns the hardware. That is where the
resource actually is.

Whether a *sequential* second session (first context closes, second one
streams) works at all is the open hardware question, and until it is answered
the driver should fail the second `start_streaming()` too — loudly, with a
`dev_err` naming the reason, not silently.

**M8. A bad parameter is a dead machine.** Normal V4L2 drivers clamp controls
and let the hardware produce a bad picture. Here an out-of-range value reaches
`b .`. Control ranges must be the *intersection* of what V4L2 permits and what
the firmware's asserts allow, and where the firmware's limit is unknown the
driver must pick the narrow end.

**M9. ffmpeg's own contract is narrower than V4L2's, and it is the contract
that matters.** Three of its rules bite us specifically: it asks for
`V4L2_PIX_FMT_YUV420` and refuses to proceed if the driver negotiates NV12
instead (so `-pix_fmt nv12` becomes mandatory); it fills the OUTPUT buffer
using *its own* `AVFrame->linesize` while taking the plane count and the
height from *our* reported format, which restricts us to `width % 64 == 0`
and `height % 16 == 0`; and it supports `V4L2_MEMORY_MMAP` only, so DMABUF
import buys us nothing with ffmpeg specifically. §3.2 and §3.3 have the
detail. None of this is a hardware problem and none of it can be fixed in
the driver — it has to be designed around.

---

## 3. The mapping, field by field

### 3.1 What `h264_v4l2m2m` actually does

Read out of FFmpeg `release/8.1` (`libavcodec/v4l2_m2m_enc.c`, `v4l2_m2m.c`,
`v4l2_context.c`, `v4l2_buffers.c`, `v4l2_fmt.c`), which is byte-identical to
`master` for these files and matches the 8.1.2 binary installed here. This is
worth reading before designing anything, because ffmpeg's encoder wrapper is
much less flexible than the V4L2 spec.

- **Device discovery**: `opendir("/dev")`, take every entry starting with
  `video`, in **readdir order**, and use the first that probes. There is no
  `device` option in 8.1 — the ffmpeg binary here confirms it, offering only
  `num_output_buffers` and `num_capture_buffers`. Probing opens the node,
  `QUERYCAP`s it, and requires `VIDIOC_ENUM_FMT` on the CAPTURE queue to
  enumerate `V4L2_PIX_FMT_H264`. (On this machine `/dev/video0` is
  `apple-isp`, which will fail that test, so we are fine — but the ordering
  is luck, not design.)
- **QUERYCAP**: `V4L2_CAP_VIDEO_M2M` alone is enough. **If both `M2M` and
  `M2M_MPLANE` are advertised, mplane wins**, so a driver that wants the
  single-planar path must not advertise the mplane caps.
- **Controls**: exactly eight, all via single-control `S_EXT_CTRLS` with
  `which = V4L2_CTRL_CLASS_MPEG` (not `V4L2_CTRL_WHICH_CUR_VAL`), and **every
  one of them is non-fatal** — an `EINVAL` from an unimplemented control is
  silent at default verbosity. ffmpeg never calls `QUERYCTRL` or `QUERYMENU`;
  it sets controls blind.
- **Memory**: `V4L2_MEMORY_MMAP` only. There is **no DMABUF and no USERPTR
  path** in the ffmpeg encoder. `REQBUFS` only, never `CREATE_BUFS`.
- **The one fatal control path** is `V4L2_CID_MPEG_VIDEO_B_FRAMES`: ffmpeg
  sets it to 0, reads it back, and if the readback is non-zero it fails the
  encoder open with `AVERROR_PATCHWELCOME`. Either do not implement the
  control at all, or make it settable to 0 and read back 0.

### 3.2 Raw format: the mismatch that decides the command line

**ffmpeg will ask for `V4L2_PIX_FMT_YUV420` (planar I420), and we can only do
NV12.**

`-i in.y4m` gives `avctx->pix_fmt = AV_PIX_FMT_YUV420P`. The `M2MENC` codec
definition declares no `pix_fmts` list, so the filter graph inserts no
conversion. ffmpeg maps `YUV420P -> V4L2_PIX_FMT_YUV420` and `TRY_FMT`s it;
if that fails it walks `ENUM_FMT`, finds our `NV12`, maps it to
`AV_PIX_FMT_NV12`, tries it, succeeds — and then compares the negotiated
format against `avctx->pix_fmt`, finds `nv12 != yuv420p`, and **fails the
encoder open** with `"Encoder requires nv12 pixel format."`.

So the literal command `ffmpeg -i in.y4m -c:v h264_v4l2m2m out.mp4` **cannot
work against an NV12-only driver**. The working command is

```
ffmpeg -i in.y4m -pix_fmt nv12 -c:v h264_v4l2m2m out.mp4
```

Three responses, in order of how much I like them:

1. **Accept the flag** and document it. The hardware is NV12; ffmpeg's
   wrapper is unusually rigid here (GStreamer's `v4l2h264enc` negotiates
   properly); every other V4L2 NV12-only encoder has the same problem.
   **[proposal — this is the recommendation.]**
2. **Find out whether the hardware can read I420 at all.** The source-path
   scalar `src_mode` (`0xFEC0`, **U**) reaches the source-read block's format
   registers, and `0x40D12000C` is described as the format word
   ([53](53-first-frame.md) §28). If one of its values selects three-plane
   4:2:0 there is a third plane pointer somewhere — but the ABI has exactly
   two plane pointers and two strides, so this is unlikely. Cheap to look at
   while sweeping `src_mode` for F18 anyway.
3. **Interleave chroma in the driver.** A CPU pass over the U and V planes
   per frame. Technically possible, ~0.35 MB/frame at 720p, and **wrong**:
   mainline V4L2 drivers do not do format conversion, and a reviewer would
   say so. Rejected.

### 3.3 Geometry: two more ffmpeg constraints that bound the format

These are not V4L2 rules; they are consequences of how
`v4l2_buffer_swframe_to_buf()` fills the OUTPUT buffer, and they are sharp.

**(a) ffmpeg packs planes at `AVFrame->linesize[]`, not at our
`bytesperline`.** The copy is `size = frame->linesize[i] * h` per plane,
concatenated. `linesize` comes from libavfilter's pool (typically
`FFALIGN(width, 32 or 64)` on aarch64), so our stride and ffmpeg's agree only
when `linesize == bytesperline`. Since we require `bytesperline % 64 == 0`,
**only widths that are already multiples of 64 are safe with ffmpeg**. 1280
and 1920 are. 1278 would produce a sheared picture with no error anywhere.

**(b) ffmpeg computes the chroma offset from the height _we report_, and our
hardware wants `ALIGN(height,16)` rows.** The copy uses our reported height
for the luma plane and half of it for chroma. Our hardware fetches
`16*ceil(H/16)` luma rows and expects chroma at `stride * 16*ceil(H/16)`.
These agree **only when `height % 16 == 0`**. If we report 1080, ffmpeg puts
chroma at `stride*1080` and the hardware reads it from `stride*1088`; if we
report 1088 instead, ffmpeg reads 8 rows past the end of the source AVFrame.
There is no correct answer for 1080 — only a choice of which thing breaks.

**Conclusion [proposal]: the first driver supports `width % 64 == 0` and
`height % 16 == 0` only, and says so in `TRY_FMT` by rounding to those.**
720p and 1920x1088 qualify; 1080p does not, and pretending otherwise is how
you get a silently wrong picture. This restriction is an ffmpeg artefact, not
a hardware one — a client that respects `bytesperline` and `sizeimage` (v4l2
test apps, GStreamer) can use any even size the firmware accepts.

Two more format rules from the same code:

- **`TRY_FMT` is first called with `width = height = 0`.** It must clamp to
  the minimum and return success, not `-EINVAL`, or the node is skipped
  during discovery and ffmpeg reports `"Could not find a valid device"`.
- **The `bytesperline`/`sizeimage` ffmpeg passes to `S_FMT` on OUTPUT are
  the stale values our own 0x0 `TRY_FMT` returned.** The driver must
  recompute both from width/height and ignore what it was given. Trusting
  them means allocating minimum-size buffers and silently truncating every
  frame.

### 3.4 The table

`S_ctl` = set at `Start_AVC`; `per-frame` = in the `Process` command;
`driver` = the driver implements it in software with no hardware field.

| V4L2 | ffmpeg sets it? | our field | where | policy / what we reject |
|---|---|---|---|---|
| `S_FMT(OUTPUT)` width/height | yes | `width`/`height` | `Start_AVC` | MB-aligned on the wire; reject outside 192..4096 x 96..4096; round to `%64`/`%16` (§3.3) |
| `S_FMT(OUTPUT)` pixelformat | yes (`YUV420`, falls back to enumerating) | NV12 two-plane source | per-frame | `V4L2_PIX_FMT_NV12` only; see §3.2 |
| `S_FMT(OUTPUT)` bytesperline | ignored on the write path | `in_luma_stride` / `in_chroma_stride` (equal) | per-frame | must be `!= 0`, `% 64`; recompute, never trust |
| `S_FMT(OUTPUT)` sizeimage | ignored | — | — | `bytesperline * ALIGN(h,16) * 3/2`; recompute |
| `S_FMT(CAPTURE)` pixelformat | yes | — | — | `V4L2_PIX_FMT_H264` only (not `H264_NO_SC`, not `H264_SLICE`) |
| `S_FMT(CAPTURE)` sizeimage | yes, `FFALIGN(h,32)*FFALIGN(w,32)*3/4` rounded to 4 KiB | coded buffer size | `Start_AVC` (table at `0x558`) | treat ffmpeg's value as a **floor**; use `AVE_CalcBufSizeOfCodedData` (1 384 448 at 720p), which is larger |
| `S_PARM(OUTPUT)` timeperframe | yes, if `-r`/input fps known | `frame_rate` `0xFF4C`, `frame_rate_div` `0xFF48` | `Start_AVC` | `0 < fps < 100000`; reject 0; also sets the CAPTURE interval per the spec |
| `V4L2_CID_MPEG_VIDEO_B_FRAMES` | **yes, and reads it back** | — | — | range 0..0. Non-zero readback **fails the encoder open** |
| `V4L2_CID_MPEG_VIDEO_GOP_SIZE` | yes (default 12) | *none* — the driver emits frame type 3 every N frames | per-frame (`0xCAC`) | free to change at any time (§4.2) |
| `V4L2_CID_MPEG_VIDEO_H264_I_PERIOD` | no | same as GOP_SIZE | per-frame | implement as an alias; note `ui32IdrPeriod` (`0xFF34`) is an **RC input, not the GOP driver** ([66](66-ratecontrol-sizing.md) §2.2) |
| `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` | yes, but only when the frame is already tagged I | frame type 3 / `forceKeyFrame` `0xA00` | per-frame | free; applies to the next queued OUTPUT buffer |
| `V4L2_CID_MPEG_VIDEO_BITRATE` | **yes, always, warns on failure** | `0xFF30`, bits/s | `Start_AVC` | accept and store; **only has an effect with the firmware RC on**, which is not safe yet (§3.5) |
| `V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE` | yes, sets 1 | `ui32RCFlag` `0xFF50`: 2 = `AVE_RC_FIXQP`, 1 = `AVE_RC_ON` | `Start_AVC` | **default 0 (fixed QP)**; accepting 1 is the CBR question in §3.5 |
| `V4L2_CID_MPEG_VIDEO_BITRATE_MODE` | no | `ui32RCFlag` | `Start_AVC` | expose `CQ` (fixed QP) and, later, `VBR`. **Reject `CBR`** — there is no VBV, no peak-rate and no CPB field anywhere in 13.5 (**C**, a symbol-count discriminator with a positive control), so "constant bitrate" would be a lie |
| `V4L2_CID_MPEG_VIDEO_H264_{I,P,B}_FRAME_QP` | no | `0xFFB4`/`0xFFB8`/`0xFFBC` | `Start_AVC` | 0..51; **grabbed while streaming** — no per-frame QP field exists |
| `V4L2_CID_MPEG_VIDEO_H264_MIN_QP` | yes (silent on EINVAL) | `0xFF88` | `Start_AVC` | 0..51; the firmware forces it to 0 if `>= 51` |
| `V4L2_CID_MPEG_VIDEO_H264_MAX_QP` | yes | `0xFF8C` | `Start_AVC` | 1..51; the firmware forces it to 51 if outside that |
| `V4L2_CID_MPEG_VIDEO_H264_PROFILE` | only with `-profile:v` | `profile_idc` | `Start_AVC` (SPS) | `BASELINE`/`CONSTRAINED_BASELINE` for now; `MAIN`/`HIGH` need CABAC or `transform_8x8`, both untested |
| `V4L2_CID_MPEG_VIDEO_H264_LEVEL` | **no** | `level_idc` | `Start_AVC` (SPS) | expose it anyway; it is not cosmetic — the firmware derives the DPB size from the level (`min(16, MaxDpbMbs(level)/(mbW*mbH))`), and an out-of-table level silently gives DPB = 2 (**C**) |
| `V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE` | no | `entropy_coding_mode_flag` `0x10C68` | `Start_AVC` | CAVLC only until CABAC is run; CABAC also turns on the `cabac_zero_word` obligation ([67](67-bitstream-output.md)) |
| `V4L2_CID_MPEG_VIDEO_HEADER_MODE` | yes, asks for `SEPARATE`, ignores failure | driver-side | driver | see §3.6 |
| `V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR` | no | driver-side memcpy | driver | default 1 |
| `V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE` | no | `sSliceMap` `0xFDAC` | `Start_AVC` | `SINGLE` only. Multi-slice is untested and would activate the `bytesToRemove` discontiguity ([67](67-bitstream-output.md) §3.6) |
| `V4L2_CID_MPEG_VIDEO_MAX_REF_PIC` | no | `max_num_ref_frames` `0x109DC` | `Start_AVC` | 1 for IPPP; clamp to the level's DPB limit, reject `>= 17` |
| `V4L2_CID_MIN_BUFFERS_FOR_OUTPUT` | no | — | — | publish it anyway (2); other clients read it |
| `VIDIOC_ENCODER_CMD(STOP)` | yes, at EOF | drain | driver | implement it, or return **`ENOTTY`** — see §3.6 |
| buffer timestamp | yes, in and out | `PICMGMT+0xCB8` is *probably* PTS+duration (**U**) | driver | copy OUTPUT -> CAPTURE host-side with `v4l2_m2m_buf_copy_metadata()`; do not trust the hardware field |
| `V4L2_BUF_FLAG_KEYFRAME` | yes, reads it | `FrameTypeReturned` in the coded header | driver | set it, or the mp4 has an empty `stss` and cannot be seeked |

### 3.5 Rate control: what to expose, and what to refuse

The firmware has three RC modes (`ui32RCFlag`: 0 off, 1 on, 2 fixed QP) and a
target bitrate in bits per second — proven by the firmware's own
`bpp = bitrate / framerate / (W*H)` computation (**C**,
[66](66-ratecontrol-sizing.md) §2.2). But it has **no VBV, no peak bitrate,
no CPB size and no HRD parameters at all** — established with a symbol-count
discriminator against the 26.6.2 kext, which has all of them (**C**). And
there are two gates inside `ProcessInit` (`sCRCInitParams+177` and `+180`)
that must both be non-zero before the bitrate arm runs *at all*, and neither
has been traced to a host field (**U**).

So:

- `FRAME_RC_ENABLE = 0` (fixed QP, `ui32RCFlag = 2`) is the default and the
  only mode that has ever run. `dev-encoder.rst` says this is exactly what
  the control means: *"If this control is disabled then the quantization
  parameter for each frame type is constant and set with appropriate
  controls."*
- `BITRATE_MODE_CQ` maps onto the same thing, with `CONSTANT_QUALITY`
  (1..100) as an inverse QP if anyone wants it.
- `BITRATE_MODE_VBR` maps to `ui32RCFlag = 1` and is the mode to enable once
  the two gates are understood.
- **`BITRATE_MODE_CBR` should be rejected**, because there is nothing behind
  it. A driver that accepts CBR and delivers an average-bitrate controller
  with a QP clamp is lying to every client that asks for CBR because it has a
  network budget.

ffmpeg sets `BITRATE` and `FRAME_RC_ENABLE = 1` unconditionally and warns
(bitrate) or stays silent (rc_enable) if either fails. With the first driver
answering `-EINVAL` to `FRAME_RC_ENABLE = 1`, ffmpeg carries on at fixed QP
and the user sees nothing at default verbosity — acceptable, and better than
pretending.

### 3.6 Output contract, drain and the mp4 muxer

ffmpeg does **no** parsing of the CAPTURE data: the buffer becomes the
`AVPacket` payload verbatim. So:

- **Annex B, one complete access unit per CAPTURE buffer.** Our coded buffer
  already holds exactly this ([67](67-bitstream-output.md), **HW** via F16).
- **The first CAPTURE buffer must contain an SPS and a PPS in Annex B.**
  The mp4 muxer has no extradata at `write_header` time (ffmpeg never sets
  `avctx->extradata` on this path and ignores `AV_CODEC_FLAG_GLOBAL_HEADER`),
  so `movenc` adopts the *first packet* as extradata and scans it for
  `nal_unit_type` 7 and 8 to build the `avcC`. If it finds neither, it writes
  an **empty `avcC` and no error** — an unplayable mp4 with nothing in the
  log. This is the failure mode most likely to waste an afternoon.
- ffmpeg asks for `HEADER_MODE_SEPARATE` and does not care if we refuse.
  **Refuse it and prepend SPS+PPS to every IDR instead [proposal]**: it
  satisfies the muxer, it also makes `-f h264` and `-f mpegts` work, and it
  avoids a header-only buffer with no source frame whose timestamp collides
  with the first real frame's.
- The mov muxer converts Annex B to length-prefixed AVCC itself. We emit
  Annex B and touch nothing.
- **`VIDIOC_ENCODER_CMD` must either work or return `ENOTTY`.** Only `ENOTTY`
  triggers ffmpeg's `STREAMOFF` fallback; any other errno leaves it blocked
  in `poll(..., -1)` forever at end of input. The same hang happens if the
  command succeeds but no CAPTURE buffer ever comes back with
  `V4L2_BUF_FLAG_LAST` or `bytesused == 0`.
- **Timestamps must be copied from the OUTPUT buffer to the CAPTURE buffer**
  by the driver; vb2 does not do it. Without it every packet gets
  `pts = dts = 0` and the muxer complains about non-monotonic DTS.
- One CAPTURE buffer per OUTPUT buffer, **in order, no reordering** — ffmpeg
  has no DTS reordering logic at all, which is also why it refuses B-frames.
  We have no B-frames, so this costs nothing.

---

## 4. The session-restart problem

This is the crux. V4L2 lets a client call `VIDIOC_S_CTRL` at any time; we
latch most of the encoder's configuration into one command that cannot be
re-sent.

### 4.1 The three available policies, and why one of them is a trap

- **Transparently tear down and re-Start.** On a control change, drain the
  in-flight frame, send Stop + Close, re-Open, re-Start_AVC with the new
  parameters, carry on. Userspace notices nothing but a latency spike.
- **Reject** with `-EBUSY` and make the client drain, `STREAMOFF` the CAPTURE
  queue, reconfigure and `STREAMON` again.
- **Defer** to the next keyframe, applying the change at an IDR boundary.

The first one is a trap, for four independent reasons:

1. **It may not be possible at all.** A second Start_AVC in one module load
   has never been sent. `AVC_INIT` runs `CreateClient`, and macOS has no
   observed path that reuses a client across a Stop (**C**/**I**,
   [63](63-teardown.md), [64](64-multiframe.md)). If the honest answer turns
   out to be "you must reload the firmware", a transparent restart is a lie.
2. **Its latency is unbounded by anything the host can see.** `STOP_DONE` is
   emitted only when the Close is *dequeued* behind the client's remaining
   queued frames (**C**).
3. **Its failure mode is a machine reset.** A Stop that times out means work
   really is in flight, which is the one state in which tearing anything down
   is worst ([63](63-teardown.md)).
4. **V4L2 does not ask for it.** `dev-encoder.rst` says, verbatim: *"The
   client may attempt to set a control during encoding and if the operation
   fails with the `-EBUSY` error code, the CAPTURE queue needs to be stopped
   for the configuration change to be allowed. To do this, it may follow the
   Drain sequence to avoid losing the already queued/encoded frames."*
   Rejecting is **documented, expected behaviour**, and the spec tells
   userspace how to recover.

**Policy [proposal]: reject, with `v4l2_ctrl_grab()`, and never restart
transparently.** Grab at `start_streaming()`, ungrab at `stop_streaming()`.
`__v4l2_ctrl_grab()` sets `V4L2_CTRL_FLAG_GRABBED`, which makes
`try_or_set_cluster()` return `-EBUSY` on `S_CTRL`/`S_EXT_CTRLS`, leaves
`TRY_EXT_CTRLS` working so a client can still validate values, and emits a
`V4L2_EVENT_CTRL` with `V4L2_EVENT_CTRL_CH_FLAGS` so a subscribed client
learns live. `VIDIOC_QUERYCTRL` reports the flag. That is the whole
mechanism, and it is three lines per control.

Note that not every mainline encoder does this — wave5's `s_ctrl` has no
`-EBUSY` at all and simply ignores late changes, and Venus snapshots its
control state at `start_streaming()`. Silently ignoring is worse for us than
for them: a client that sets QP mid-stream and gets no error has no way to
learn that its bitrate is wrong. **Grab.**

The mid-stream *resolution* change needs no policy at all: `dev-encoder.rst`
has no dynamic-resolution-change section for encoders (that is the decoder's
`V4L2_EVENT_SOURCE_CHANGE`), CAPTURE width/height are read-only and derived,
and `S_FMT` must return `-EBUSY` *while buffers are allocated* — the trigger
is `vb2_is_busy()`, not `vb2_is_streaming()`. The ecosystem already expects a
full teardown, so our hardware's hardest constraint costs us nothing here.

### 4.2 The policy, item by item

"Grab" = `v4l2_ctrl_grab()` at STREAMON, `-EBUSY` until STREAMOFF.
"Free" = changeable at any time, because the *driver* owns the behaviour.

| what | mechanism | our constraint | policy |
|---|---|---|---|
| CAPTURE pixelformat | `S_FMT` | `V4L2_PIX_FMT_H264` only | reject others; `-EBUSY` if `vb2_is_busy()` |
| resolution | `S_FMT(OUTPUT)` | Start-latched; 192..4096 x 96..4096, even | `-EBUSY` if `vb2_is_busy()`; adjust out-of-range in `TRY_FMT`; recompute the CAPTURE format |
| OUTPUT pixelformat | `S_FMT(OUTPUT)` | NV12 only | adjust to NV12 (`TRY_FMT` convention), `-EBUSY` if busy |
| stride | `S_FMT(OUTPUT).bytesperline` | non-zero, `% 64`, `>= ALIGN(W,16)` | reject a bad one outright — do not round; the firmware checks `stride & 0x3f` |
| frame interval | `S_PARM(OUTPUT)` | Start-latched (`0xFF4C`/`0xFF48`); `0 < fps < 100000` | `-EBUSY` while streaming **[proposal]** — not a documented requirement, but consistent, and `S_PARM` is a *mandatory* pre-STREAMON step in the spec |
| `H264_I/P/B_FRAME_QP` | `S_CTRL` | Start-latched; **no per-frame QP field exists** | **grab** — this is the headline mismatch |
| `H264_MIN_QP` / `MAX_QP` | `S_CTRL` | Start-latched (`0xFF88`/`0xFF8C`) | grab |
| `BITRATE` | `S_CTRL` | Start-latched (`0xFF30`, bits/s) | grab (see §4.3 for the one candidate exception) |
| `BITRATE_MODE` | `S_CTRL` | Start-latched (`ui32RCFlag`) | grab; menu restricted (§3) |
| `FRAME_RC_ENABLE` | `S_CTRL` | Start-latched | grab |
| `H264_PROFILE`, `H264_LEVEL` | `S_CTRL` | SPS fields, generated once at Start | grab |
| `H264_ENTROPY_MODE` | `S_CTRL` | Start-latched; CABAC needs `profile_idc >= 77` and is untested | grab, and expose CAVLC only until someone runs it |
| `B_FRAMES` | `S_CTRL` | B-frames never attempted | range 0..0 |
| `MULTI_SLICE_MODE` | `S_CTRL` | slice map is Start-latched; multi-slice untested and would expose the `bytesToRemove` hole problem | `SINGLE` only |
| `MAX_REF_PIC` | `S_CTRL` | `max_num_ref_frames`, Start-latched, bounded by the level's `MaxDpbMbs` | grab; clamp to what the level allows |
| `GOP_SIZE`, `H264_I_PERIOD` | `S_CTRL` | **the driver owns the GOP** — it picks the per-frame type | **free** |
| `FORCE_KEY_FRAME` | `S_CTRL` (button) | per-frame frame type 3 | **free**; applies to the next queued OUTPUT buffer |
| `HEADER_MODE` | `S_CTRL` | the driver synthesises both modes by memcpy | grab (it changes what the first CAPTURE buffer means), but cheap either way |
| `PREPEND_SPSPPS_TO_IDR` | `S_CTRL` | driver-side memcpy | **free** |
| CAPTURE buffer count | `REQBUFS` | 20 cap — *only under design A* (§5.4) | design B: free. Design A: set `q->max_num_buffers = 20` and let `REQBUFS` clamp silently, which is the supported mechanism and is discoverable via `V4L2_BUF_CAP_SUPPORTS_MAX_NUM_BUFFERS` |
| `CREATE_BUFS` / `REMOVE_BUFS` on CAPTURE | ioctl | design A: the buffer set is frozen at Start | design B: allowed. Design A: `-EBUSY` from `queue_setup` while the session lives, and do not advertise `REMOVE_BUFS` |

**GOP_SIZE being free is worth dwelling on**, because it is the one place the
hardware is *more* flexible than it looks. Frame type is a per-frame host
field, and macOS's own client normally writes 5 ("firmware decides") — but a
host that writes 3 and 1 explicitly owns the GOP entirely
([64](64-multiframe.md) §4.1, **C** for the enum, **I** that nothing
overwrites a host-supplied type). So `GOP_SIZE` needs no firmware field at
all: the driver counts frames and emits an IDR when the count says so. The
same is true of `FORCE_KEY_FRAME`.

### 4.3 The one candidate for "defer to the next keyframe"

`PICMGMT + 0x6EB`, `bChangeBitrateAtNextIDR`
([66](66-ratecontrol-sizing.md) §2.4), is a real per-frame field whose name
says exactly what a deferred-bitrate policy would need. It is **U**: nobody
has traced what the firmware does with it, and it is meaningless in fixed-QP
mode anyway. If rate control is ever turned on — which needs the two
undiscovered gates `sCRCInitParams+177/+180` first
([66](66-ratecontrol-sizing.md) §2.1) — this is the field to investigate
before deciding that `BITRATE` must be grabbed forever.

Everything else has no deferred path, because there is no per-frame field to
defer *into*. That is the honest reason the "defer" column of the table above
is empty.

---

## 5. Buffer plumbing

### 5.1 The two queues

| | OUTPUT (raw) | CAPTURE (coded) |
|---|---|---|
| format | `V4L2_PIX_FMT_NV12`, single-planar | `V4L2_PIX_FMT_H264` |
| memory | `MMAP`, plus `DMABUF` import later | `MMAP` |
| allocator | `vb2_dma_contig` | `vb2_dma_contig` |
| count | 2..N, no hardware limit | see §5.4 |

The source is one buffer, not two: derive chroma as
`luma_dma + bytesperline * ALIGN(height, 16)` and hand the firmware the two
addresses it wants. §2.2 has the reasoning for single-planar.

ffmpeg uses `MMAP` for both queues and has no DMABUF path at all (§3.1), so
`DMABUF` import is for GStreamer, a camera pipeline or a compositor — not for
the milestone in §7.

### 5.2 The source plane rules a `TRY_FMT` must enforce

All **C** from [53](53-first-frame.md) §2.1 / [38](38-dimension-convention.md):

- `width`, `height`: 192..4096 x 96..4096, even. The **hardware** gets the
  MB-aligned coded size; **userspace** sees the size it asked for, with
  `V4L2_SEL_TGT_CROP` describing the visible rectangle. (ffmpeg reads no
  selection rectangle at all — for it, the `S_FMT` size *is* the visible
  size — so the crop target only matters to better clients.)
- `bytesperline = ALIGN(ALIGN(width,16), 64)`, and any client-supplied stride
  must also be non-zero and a multiple of 64 — reject otherwise, do not
  silently round, because the firmware checks `stride & 0x3f` on both planes.
- `sizeimage = bytesperline * ALIGN(height,16) * 3 / 2`. **This is the
  mismatch that bites**: the hardware fetches `16*ceil(H/16)` luma rows
  regardless of the declared height, so a client that sizes a 1080-row buffer
  at `stride*1080*3/2` hands the DMA 8 rows of somebody else's memory. A
  driver that returns the honest `sizeimage` and refuses anything smaller is
  the whole defence — and with ffmpeg specifically it is not enough, because
  ffmpeg positions the chroma plane itself, at `linesize * our_height`
  (§3.3b). That is why the first driver accepts only `height % 16 == 0`.
- **Report the requested height, never the MB-aligned one.** The alignment
  is the driver's business; a client that is told 1088 when it asked for 1080
  will read 8 rows past the end of its own frame.
- Both plane addresses 64-byte aligned (page alignment gives this for free)
  and **below 4 GiB**.

### 5.3 DMABUF import on the OUTPUT queue

Verdict: **it should work, and it is cheap — but it is not on the critical
path, because ffmpeg cannot use it. Three caveats.**

Why it should work: the AVE device sits behind `apple-dart`, and the working
overlay lists *both* AVE DARTs in one `iommus` property
(`test/ave-overlay-e4.dts`: `<&dart_ave0_0 0>, <&dart_ave0_0 1>,
<&dart_ave0_1 0>, <&dart_ave0_1 1>`). `apple-dart.c` has
`MAX_DARTS_PER_DEVICE 3`; `apple_dart_of_xlate()` runs once per `iommus`
entry and extends a single `apple_dart_master_cfg` with one `stream_maps[]`
slot per DART; `struct apple_dart_domain` holds **one** `pgtbl_ops` shared by
all of them; and `apple_dart_attach_dev_paging()` ends with
`for_each_stream_map(...) apple_dart_setup_translation(...)`, writing the
*same* TTBR values into every DART. One domain, one page table, both DARTs.

So anything mapped into the device's DMA domain — including a dmabuf mapped
by `dma_buf_map_attachment()` on `ave->dev`, which `vb2_dma_contig` attaches
to `q->dev` = the platform device — is reachable by the coprocessor *and* by
the encoder datapath, and the driver must **not** try to map it twice or
invent a second `struct device`. This is also the mechanism
`ave_dart_datapath_check()` depends on when it asserts that the datapath
DART's TTBR equals the CPUDART's and treats a mismatch as fatal.

The caveats:

1. **Contiguity.** The firmware takes one address and one stride per plane, so
   each plane must be a single contiguous IOVA range. `vb2_dc_map_dmabuf()`
   computes `vb2_dc_get_contiguous_size(sgt)` and fails with `-EFAULT` and
   `"contiguous chunk is too small"` if it is shorter than the plane, so the
   failure is a clean error at `QBUF`, not a fault. Behind an IOMMU the DMA
   layer normally coalesces the scatterlist into one IOVA range anyway, and
   with 16 KiB pages and a 16 KiB DART granule the odds are good.
2. **The 4 GiB ceiling is not currently structural.** `ave_probe_stages()`
   calls `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42))`, and
   `ave_sess_dma_alloc()` compensates with a runtime check that refuses any
   buffer crossing 4 GiB. An imported dmabuf goes through neither.
   **Suggested change: set the mask to `DMA_BIT_MASK(32)`** so the IOVA
   allocator cannot produce an address the firmware will truncate, and keep
   the runtime check as a belt-and-braces assertion.
3. **Coherency is an open question.** `dts/t6001-ave.dtsi` has no
   `dma-coherent` property, so the DMA API will treat AVE as non-coherent and
   `vb2` will do cache maintenance on every imported (cached) source buffer.
   Whether the AVE datapath is actually coherent on this SoC is **U**; if it
   is, adding `dma-coherent` removes a per-frame cache clean of a whole
   frame. Worth answering before anyone benchmarks throughput. Note the
   firmware-shared structures stay `dma_alloc_coherent` either way.

`DMABUF` on the **CAPTURE** queue is a different matter and the answer is
**no, not in any near-term driver**: an imported buffer's address is not known
until `QBUF`, and the coded buffer's address must be in the Start_AVC table
(M1). Export (`EXPBUF`) of MMAP capture buffers is fine and free.

### 5.4 The coded pool, the 20-slot cap, and what `dma_alloc_coherent` becomes

Two designs are possible.

**Design A — the vb2 CAPTURE buffers _are_ the firmware's coded buffers.**
`vb2_dma_contig` allocates at `REQBUFS`, so at `STREAMON` the driver can walk
the queue, read each buffer's `vb2_dma_contig_plane_dma_addr()`, and build the
Start_AVC `CodedData` table from them. Zero copy. Costs: `REQBUFS` count is
capped at 20 and becomes a session parameter; `CREATE_BUFS` must be refused
while streaming; the vb2 buffer index *is* the coded index and therefore the
command slot; and the SPS/PPS prepend (M3) has no natural home, because the
firmware writes at byte 0 of the buffer.

**Design B — a driver-private coded pool, copied into vb2 buffers
[proposal, recommended first].** Keep `dma_alloc_coherent` for `N = min(20,
something small — 4 is plenty for a synchronous driver)` coded buffers and
their `0x23000`-byte headers, exactly as `ave_session_start_avc()` does today.
On `ENCODE_DONE`, parse the header, assemble the frame into the vb2 CAPTURE
buffer (prepending SPS+PPS on the first frame or on every IDR, skipping
`bytesToRemove` holes, appending `3 * numCABACzeroWordInserted` bytes), set
`bytesused`, and release the coded slot back to the free list.

Design B is the recommendation, and the reason is that **it makes M1 mostly
go away**: the 20-entry pool becomes a bound on *frames in flight*, which a
synchronous driver never approaches, instead of a bound on `REQBUFS` count.
`CREATE_BUFS` works. Buffer indices stop mattering. Userspace can ask for 4 or
64 capture buffers and the driver does not care. The price is one memcpy of a
few hundred KB per frame, which is nothing next to the encode, and roughly
3 MB of permanently allocated coherent memory per coded slot at 720p
(1 384 448 + 143 360 bytes, [66](66-ratecontrol-sizing.md) §2.7).

Design A is the optimisation to reach for *after* single-slice output is
proven and after the header-delivery question (M3) has an answer. It is not
the first driver.

Everything else `ave_session.c` allocates with `dma_alloc_coherent` — recon,
LowResRef, LowResResult, colocated, entropy, SrcNeighbor, `fw_client`,
`fw_client_mem`, parameter sets — is **firmware-private and stays exactly as
it is**. It is never mapped to userspace, its lifetime is the session's, and
it is freed only under the teardown rules of §1.5. The only current allocation
that *leaves* is the per-frame source plane pair, which becomes vb2 OUTPUT
buffers.

### 5.5 Completion matching

`ENCODE_DONE` carries the command slot and nothing else useful, and the
firmware may in principle reorder between clients (**C** that the machinery
exists; **I** that a single-client IPPP stream is FIFO,
[64](64-multiframe.md) §5). The driver must therefore keep a
`slot -> {coded index, vb2 OUTPUT buf, vb2 CAPTURE buf, frameNumber}` table
and match on the slot, then cross-check `FrameNumberFromDriverReturned` in the
coded header against the `frameNumber` it sent — the only per-frame identity
the hardware offers ([67](67-bitstream-output.md) §3.5, **C**). A mismatch is
a bug in our slot bookkeeping and should be loud.

The first driver should submit **strictly one frame at a time** — this is
[64](64-multiframe.md)'s own recommendation, and the reasoning is that on this
machine a completion-matching bug costs a boot to diagnose. `v4l2_m2m`'s job
queue gives that for free: one `device_run()` at a time, `v4l2_m2m_job_finish()`
from the completion.

---

## 6. What is missing in the driver before any of this is possible

Ordered. Each item is a thing the bring-up harness does not have a concept of.

1. **A correct picture.** F18's discriminator, then whatever it points at
   (sweep `src_mode` `0xFEC0` / `src_cfg` `0xFCE8`, or chase the addressing).
   Operator-run. Until this is done, everything below is building a road to a
   place with nothing in it.
2. **A second frame, and a P frame.** `session_frames=2` (F20) needs the
   `LowResResult` buffers that an I-frame never reads and a P-frame asserts on
   ([65](65-pframes.md) §Q4) — already allocated in `ave_session.c`, never
   exercised. Also proves the coded-header memset requirement and that
   `frameNumber` reaches the firmware.
3. **A teardown that is provably safe** ([63](63-teardown.md) §7, F19).
   `ave_session_close_client()` exists and has never run. Until Stop and Close
   are known to work, `STREAMOFF` and `close()` have no implementation and the
   module cannot be unloaded safely — which means no iterative development.
   This is the real blocker on the uapi, not the uapi.
4. **A client object.** Everything today is file-static: one
   `AVE_SESS_CLIENT_ID`, one `ave_sess_rx` capture struct, one
   `ave->session_bufs`. A V4L2 driver needs `struct ave_ctx` (per `open()`),
   even if the driver then refuses the second one.
5. **An asynchronous completion path.** `ave_session_cmd()` sends, waits on a
   completion, and decodes inline, with a global rx hook that is installed and
   removed around the run. The real driver needs the IRQ path to look the slot
   up in a table, complete *that* frame, and hand it to `v4l2_m2m_job_finish()`
   — with the hook permanently installed, which also fixes the trap that a
   teardown Stop/Close today would time out on success because no hook is
   installed ([63](63-teardown.md), **C** from the driver source).
6. **Session parameters as data, not module parameters.** ~40 `module_param`s
   drive Start_AVC today. They need to become a `struct ave_session_params`
   filled from the V4L2 format and control state, with the bisect knobs kept
   as *overrides* — they are the only reason the hardware work is tractable
   and must not be deleted.
7. **Buffer sizing driven by the format, not by `session_width`.** Already
   mostly done ([66](66-ratecontrol-sizing.md) §5 fixed the recon and coded
   formulas); the remaining work is that allocation happens inside
   `ave_session_selftest()`'s linear flow rather than in a
   `start_streaming()` that can fail cleanly.
8. **The vb2/m2m layer itself.** This is the *smallest* item on the list and
   the only conventional one: `v4l2_device`, `video_device`,
   `v4l2_m2m_dev_init`, two queues, `device_run`, the ctrl handler. A few
   hundred lines, none of it novel.
9. **A second session in one module load.** Needed before the device node is
   honest (a client that closes and reopens must work). Untested, and §1.1
   says it may not be possible without a firmware reload.

---

## 7. The smallest credible milestone

**Target:** `ffmpeg -i in.y4m -pix_fmt nv12 -c:v h264_v4l2m2m out.mp4`
produces a file that `ffmpeg -v error -i out.mp4 -f null -` accepts and whose
pictures are the input pictures. The `-pix_fmt nv12` is not optional and is
not a bug we can fix (§3.2); pretending the bare command will work is the
kind of promise this plan is trying to avoid. Nothing else. No resolution changes, no bitrate control, no
second session, no concurrency, one fixed QP, IPPP, one slice, 1280x720.

The order matters more than the content. Steps 1–3 are hardware work that has
nothing to do with V4L2 and cannot be skipped; steps 4–9 are the driver.

**Step 1 — the picture. (operator, hardware)**
F18's flat-luma discriminator, then the `src_mode`/`src_cfg` sweep if it says
the DMA never read our bytes. Done when `tools/check_frame.py` grades a run
`OK` instead of `BLANK`. *Everything after this is blocked on it.*

**Step 2 — two frames, then four. (operator, hardware)**
`session_frames=2` (IDR + P) with the `LowResResult` table published, then 4.
Done when frame 1 decodes as a P frame that predicts from frame 0, and
`FrameNumberFromDriverReturned` reads 0,1,2,3. This also proves the per-frame
coded-header memset and the coded-slot rotation, both of which the V4L2 layer
depends on.

**Step 3 — teardown. (operator, hardware; F19)**
Stop -> `UNINIT_DONE` -> Close -> `STOP_DONE`, then unmap while powered, then
gate. Done when a load/encode/unload cycle completes and the machine is still
up sixty seconds later, twice. **Without this there is no iterative
development**: every subsequent step costs a reboot, and a V4L2 driver whose
`close()` can reset the machine cannot be offered to userspace at all.

**Step 4 — restructure, no new behaviour. (static, no hardware)**
Split `ave_session.c` into a session layer that takes a
`struct ave_session_params` and a harness that fills it from the module
parameters. Introduce `struct ave_ctx`. Make the IPC rx hook permanent and
slot-keyed, with the self-test as its first client. Verify by re-running the
existing self-test unchanged — byte-identical commands on the wire.

**Step 5 — the m2m skeleton. (static, then one hardware run)**
`v4l2_device`, `video_device` (`VFL_TYPE_VIDEO`, `V4L2_CAP_VIDEO_M2M |
V4L2_CAP_STREAMING`), `v4l2_m2m_dev_init`, two `vb2_dma_contig` queues, one
m2m context per `open()`, and `-EBUSY` from `start_streaming()` — never from
`open()` (M7) — if another context already owns the hardware. Formats fixed: `NV12` in, `H264` out, one
resolution, `TRY_FMT` enforcing §5.2. `start_streaming()` on **whichever queue
streams second** does Open + Start_AVC (ffmpeg streams OUTPUT first, so it
cannot be hung off the CAPTURE queue; wave5 does the same thing);
`device_run()` does one Process; the completion does
`v4l2_m2m_job_finish()`. `stop_streaming()` does Stop + Close. No controls
yet beyond the ones with no choice in them.

**Step 6 — the output contract.**
Assemble the CAPTURE buffer per §5.4 design B: SPS+PPS (length
`hdr[0x98]/8`) prepended on every IDR, slices concatenated skipping
`bytesToRemove`, `3*numCABACzeroWordInserted` bytes appended,
`V4L2_BUF_FLAG_KEYFRAME` set from `FrameTypeReturned`, `bytesused` set,
timestamp copied from the OUTPUT buffer. Done when `dd`-ing the frames out of
a v4l2 test app gives a file `ffmpeg` decodes — the same bar the debugfs
`frame.h264` already meets, through a different pipe.

**Step 7 — the controls ffmpeg sets.**
Only the ones `h264_v4l2m2m` actually touches (§3), with the values it will
send accepted and everything else rejected rather than silently ignored. The
GOP/keyframe path is the only one with real behaviour behind it: the driver
owns the GOP, emitting frame type 3 on frame 0 and every `gop_size`, 1
otherwise ([64](64-multiframe.md) recommends host-driven types over the
firmware's `type = 5` model for bring-up).

**Step 8 — the drain.**
`VIDIOC_ENCODER_CMD(V4L2_ENC_CMD_STOP)`, the last-buffer flag, and the
`V4L2_EVENT_EOS`/zero-`bytesused` convention. Synchronous submission makes
this nearly trivial: there is never more than one frame in the firmware, so
"drain" is "finish the one in flight, then flag the next dequeued CAPTURE
buffer". Done when ffmpeg's own drain at end of input does not hang.

**Step 9 — ffmpeg end to end.**
`ffmpeg -i in.y4m -pix_fmt nv12 -c:v h264_v4l2m2m out.mp4` at 1280x720. Done
when the output decodes and PSNR against the input is sane. Expect the first
failures in device probing and format negotiation, not in the encode: the
0x0 `TRY_FMT`, the stale `sizeimage`, the `B_FRAMES` readback and the
`ENCODER_CMD` errno are each capable of failing the whole thing on their own,
and three of the four fail *silently* (§3.1, §3.3, §3.6).

**Explicitly not in this milestone:** DMABUF import (§5.3 — do it in step 10,
it is small and self-contained), bitrate/CBR (the two undiscovered RC gates,
[66](66-ratecontrol-sizing.md) §2.1, make it a research task not a driver
task), multi-slice, CABAC, resolutions other than the one compiled in,
`CREATE_BUFS`, a second session, `v4l2-compliance` cleanliness, and
HEVC.

---

## 8. What in this document is speculative

Listed so that nobody mistakes the confident tone for evidence.

**Design opinions, not findings.** Everything marked **[proposal]**: NV12
single-planar first, design B for the coded pool, host-driven GOP, `-EBUSY`
at `start_streaming()`, the whole of §4's policy table and §7's ordering.
None of it has been tried; all of it can be argued with.

**Load-bearing facts that are `C` but have never been executed.** The
20-buffer cap, the index/slot coupling, the address-match assert, the
Start-time latching of QP, the level->DPB derivation, the coded-header
layout, `psets_len = sps_pps_bits/8`, the status codes. These come from
Apple's own binaries and both sides agree, which is the second-strongest tier
in [00](00-methodology.md) — but no Linux code has depended on any of them
yet.

**Facts that are `I` and that the design leans on anyway.**
- Source planes are released at `ENCODE_DONE`. Nowhere stated
  ([63](63-teardown.md) §5.3 flags the gap). If it is wrong, the OUTPUT queue
  hands buffers back to userspace while the firmware is still reading them.
- SPS/PPS are *not* also written into the coded buffer — an absence argument
  ([67](67-bitstream-output.md) §3.3). The driver keeps a
  `nal_unit_type == 7` guard precisely because of this.
- The host must append `3 * numCABACzeroWordInserted` bytes. Zero for CAVLC,
  so unreachable in the first driver; it becomes real with CABAC.
- `apple-dart` programming one domain into every DART in `iommus` — quoted
  here from the driver's own `ave_dart_datapath_check()` and the overlay, not
  re-read from `drivers/iommu/apple-dart.c`.

**Genuinely unknown and potentially blocking.**
- The two source-path scalars (§0). Everything is blocked on these.
- The two hidden rate-control gates `sCRCInitParams+177/+180`
  ([66](66-ratecontrol-sizing.md) §2.1, **U**): until they are found, turning
  CBR on is a guess, which is why §4 rejects `BITRATE_MODE_CBR`.
- Whether a second `Start_AVC` (with or without an intervening Stop) works at
  all. This decides whether §4's "restart" column is implementable or whether
  every policy in it collapses to "reject".
- Whether `close()`/`STREAMOFF` can be made safe enough to be called
  repeatedly. Step 3 of §7.
- Whether the AVE datapath is cache-coherent (§5.3 caveat 3).
- `PICMGMT+0xCB8` — "almost certainly PTS and duration, not read"
  ([64](64-multiframe.md)). V4L2 timestamps are copied host-side anyway, so
  nothing depends on it; it is listed because it is the one place the
  hardware might carry a timestamp for us.

## 9. Suggested changes to other documents

Not applied — this document owns no file but itself.

- **[22](22-driver-plan.md) and [04](04-roadmap.md)** describe phase 7 as
  "conventional work once the hardware path exists". That is right about the
  vb2/m2m glue and wrong about the rest: §4 (the restart policy) and §5.4
  (the coded pool) are not conventional, and teardown (§7 step 3) is a
  hardware milestone that the roadmap files under phase 8 ("power
  management"). Suggest moving teardown ahead of the V4L2 work in both.
- **[65](65-pframes.md) §6.29** gives the frame-type field as wire `0xD74`
  while [64](64-multiframe.md) §2.2 derives `0x1674` twice. `ave_abi.h` is
  the tiebreaker and agrees with 64: `process_avc.picmgmt = 0x9c8`,
  `.frame_type = 0xcac`, so the wire offset is `0x1674` and 65's table has a
  different base. Worth a one-line correction in 65.
- **[63](63-teardown.md) §5.3** flags that source-plane release timing is
  never stated. For a V4L2 driver that is not a footnote — it is the rule
  that decides when an OUTPUT buffer goes back to userspace. Suggest raising
  it to an open question with a proposed experiment (submit frame N+1 with
  frame N's source buffer overwritten immediately after `ENCODE_DONE`, and
  see whether frame N's picture is affected).
- **`driver/ave_drv.c`**: `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42))`
  should be 32, per §5.3 caveat 2. That is a real (if latent) bug today, not
  only a uapi concern: nothing but a runtime check in one allocator stops the
  IOVA allocator handing out an address the firmware truncates.
