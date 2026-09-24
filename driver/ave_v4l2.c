// SPDX-License-Identifier: GPL-2.0-only
/*
 * V4L2 stateful mem2mem H.264 encoder front end (docs/68).
 *
 * NV12 in on the OUTPUT queue, H.264 Annex-B out on the CAPTURE queue,
 * single-planar, MMAP (and DMABUF, which costs nothing here). One stream
 * owns the hardware at a time: a second context gets -EBUSY from
 * start_streaming, never from open() (docs/68 §7 step 5).
 *
 * The session layer (ave_session.c, ave_enc_*) does the firmware work and
 * is synchronous - one Process in flight - so device_run() hands the job to
 * a work item that may sleep on the reply.
 *
 * Format policy, from docs/68 §3.3: width a multiple of 64 and height a
 * multiple of 16. The hardware needs a 64-byte stride and fetches
 * 16*ceil(H/16) rows with chroma right after them; ffmpeg packs planes at
 * its own linesize and puts chroma at stride * the height we report, so
 * any other size gives a silently wrong picture with ffmpeg.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "ave.h"
#include "ave_session.h"
#include "ave_v4l2.h"

#define AVE_V4L2_NAME		"apple-ave"
#define AVE_MIN_W		192
#define AVE_MIN_H		96
#define AVE_MAX_W		4096
#define AVE_MAX_H		4096
#define AVE_DEF_W		1280
#define AVE_DEF_H		720
#define AVE_CODED_SLOTS		4
#define AVE_DEF_QP		30

struct ave_v4l2 {
	struct ave_device	*ave;
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct v4l2_m2m_dev	*m2m_dev;
	struct mutex		dev_mutex;	/* vfd and queue lock */
	struct mutex		hw_mutex;	/* ave_enc_* calls */
	struct workqueue_struct	*wq;
	struct ave_ctx		*owner;		/* the context holding a session */
};

struct ave_ctx {
	struct v4l2_fh		fh;
	struct ave_v4l2		*av;
	struct v4l2_ctrl_handler hdl;
	struct work_struct	run_work;
	u32			width, height;	/* OUTPUT, as negotiated */
	u32			bytesperline;
	u32			out_size, cap_size;
	struct v4l2_fract	timeperframe;
	u32			qp;
	u32			gop;
	bool			force_key;
	u32			frame_n;	/* frames sent this stream */
	u32			out_seq, cap_seq;
	bool			session;	/* Open + Start_AVC done */
};

static inline struct ave_ctx *fh_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct ave_ctx, fh);
}

/* ---------------------------------------------------------------------- */
/* Formats                                                                */
/* ---------------------------------------------------------------------- */

static void ave_clamp_size(u32 *w, u32 *h)
{
	*w = clamp_t(u32, ALIGN(*w, 64), AVE_MIN_W, AVE_MAX_W);
	*h = clamp_t(u32, ALIGN(*h, 16), AVE_MIN_H, AVE_MAX_H);
}

static u32 ave_out_size(u32 bpl, u32 h)
{
	return bpl * h * 3 / 2;
}

/* Worst case is I_PCM, 384 bytes per MB plus headers (f43); add slack. */
static u32 ave_cap_size(u32 w, u32 h)
{
	return ALIGN(w * h * 3 / 2 + SZ_64K, SZ_4K);
}

static void ave_fill_out_fmt(struct v4l2_pix_format *p, u32 w, u32 h)
{
	ave_clamp_size(&w, &h);
	p->width = w;
	p->height = h;
	p->pixelformat = V4L2_PIX_FMT_NV12;
	p->field = V4L2_FIELD_NONE;
	/* Recomputed every time: ffmpeg sends back our stale 0x0 answer. */
	p->bytesperline = ALIGN(w, 64);
	p->sizeimage = ave_out_size(p->bytesperline, h);
	p->colorspace = V4L2_COLORSPACE_REC709;
	p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	p->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static void ave_fill_cap_fmt(struct v4l2_pix_format *p, u32 w, u32 h,
			     u32 req_size)
{
	p->width = w;
	p->height = h;
	p->pixelformat = V4L2_PIX_FMT_H264;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = 0;
	/* ffmpeg's request is a floor, never a ceiling (docs/68 §3.4). */
	p->sizeimage = max(req_size, ave_cap_size(w, h));
	p->colorspace = V4L2_COLORSPACE_REC709;
}

static int ave_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	strscpy(cap->driver, AVE_V4L2_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple AVE H.264 encoder", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" AVE_V4L2_NAME, sizeof(cap->bus_info));
	return 0;
}

static int ave_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		f->pixelformat = V4L2_PIX_FMT_NV12;
	} else {
		f->pixelformat = V4L2_PIX_FMT_H264;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
	}
	return 0;
}

static int ave_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fs)
{
	if (fs->index || (fs->pixel_format != V4L2_PIX_FMT_NV12 &&
			  fs->pixel_format != V4L2_PIX_FMT_H264))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise.min_width = AVE_MIN_W;
	fs->stepwise.max_width = AVE_MAX_W;
	fs->stepwise.step_width = 64;
	fs->stepwise.min_height = AVE_MIN_H;
	fs->stepwise.max_height = AVE_MAX_H;
	fs->stepwise.step_height = 16;
	return 0;
}

static int ave_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = fh_to_ctx(file);

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		ave_fill_out_fmt(&f->fmt.pix, ctx->width, ctx->height);
	else
		ave_fill_cap_fmt(&f->fmt.pix, ctx->width, ctx->height,
				 ctx->cap_size);
	return 0;
}

static int ave_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = fh_to_ctx(file);

	/* 0x0 is ffmpeg's first question; clamp, never refuse (docs/68 §3.3). */
	if (V4L2_TYPE_IS_OUTPUT(f->type))
		ave_fill_out_fmt(&f->fmt.pix, f->fmt.pix.width,
				 f->fmt.pix.height);
	else
		ave_fill_cap_fmt(&f->fmt.pix, ctx->width, ctx->height,
				 f->fmt.pix.sizeimage);
	return 0;
}

static int ave_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = fh_to_ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);

	if (vb2_is_busy(vq))
		return -EBUSY;
	ave_try_fmt(file, priv, f);
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		ctx->width = f->fmt.pix.width;
		ctx->height = f->fmt.pix.height;
		ctx->bytesperline = f->fmt.pix.bytesperline;
		ctx->out_size = f->fmt.pix.sizeimage;
		ctx->cap_size = max(ctx->cap_size,
				    ave_cap_size(ctx->width, ctx->height));
	} else {
		ctx->cap_size = f->fmt.pix.sizeimage;
	}
	return 0;
}

static int ave_g_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	struct ave_ctx *ctx = fh_to_ctx(file);

	if (!V4L2_TYPE_IS_OUTPUT(a->type))
		return -EINVAL;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe = ctx->timeperframe;
	return 0;
}

static int ave_s_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	struct ave_ctx *ctx = fh_to_ctx(file);
	struct v4l2_fract *t = &a->parm.output.timeperframe;

	if (!V4L2_TYPE_IS_OUTPUT(a->type))
		return -EINVAL;
	/*
	 * Recorded, not sent: the fixed-QP session does not use the frame
	 * rate. It goes on the wire with rate control (docs/66).
	 */
	if (t->numerator && t->denominator)
		ctx->timeperframe = *t;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	*t = ctx->timeperframe;
	return 0;
}

static int ave_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static const struct v4l2_ioctl_ops ave_ioctl_ops = {
	.vidioc_querycap		= ave_querycap,
	.vidioc_enum_fmt_vid_out	= ave_enum_fmt,
	.vidioc_enum_fmt_vid_cap	= ave_enum_fmt,
	.vidioc_enum_framesizes		= ave_enum_framesizes,
	.vidioc_g_fmt_vid_out		= ave_g_fmt,
	.vidioc_g_fmt_vid_cap		= ave_g_fmt,
	.vidioc_try_fmt_vid_out		= ave_try_fmt,
	.vidioc_try_fmt_vid_cap		= ave_try_fmt,
	.vidioc_s_fmt_vid_out		= ave_s_fmt,
	.vidioc_s_fmt_vid_cap		= ave_s_fmt,
	.vidioc_g_parm			= ave_g_parm,
	.vidioc_s_parm			= ave_s_parm,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_encoder_cmd		= v4l2_m2m_ioctl_encoder_cmd,
	.vidioc_try_encoder_cmd		= v4l2_m2m_ioctl_try_encoder_cmd,

	.vidioc_subscribe_event		= ave_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/* ---------------------------------------------------------------------- */
/* Controls                                                               */
/* ---------------------------------------------------------------------- */

static int ave_s_ctrl(struct v4l2_ctrl *c)
{
	struct ave_ctx *ctx = container_of(c->handler, struct ave_ctx, hdl);

	switch (c->id) {
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:
		ctx->qp = c->val;	/* one QP for I and P until RC (docs/68 §3.5) */
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctx->gop = c->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		ctx->force_key = true;
		break;
	default:
		/*
		 * Accepted and stored so that clients which set them blind -
		 * ffmpeg sets BITRATE and FRAME_RC_ENABLE on every open -
		 * succeed; the session is fixed-QP (docs/68 §3.5).
		 */
		break;
	}
	return 0;
}

static const struct v4l2_ctrl_ops ave_ctrl_ops = {
	.s_ctrl = ave_s_ctrl,
};

static int ave_init_ctrls(struct ave_ctx *ctx)
{
	struct v4l2_ctrl_handler *h = &ctx->hdl;
	const struct v4l2_ctrl_ops *o = &ave_ctrl_ops;

	v4l2_ctrl_handler_init(h, 12);
	/* ffmpeg sets 0 and reads it back; non-zero fails its open (§3.1). */
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 0, 1, 0);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, 65535, 1, 0);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, 0, 51, 1,
			  AVE_DEF_QP);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 0, 51, 1, 10);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 0, 51, 1, 51);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_BITRATE, 1, 400000000, 1,
			  10000000);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0, 1, 1, 0);
	v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ,
			       ~BIT(V4L2_MPEG_VIDEO_BITRATE_MODE_CQ),
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ);
	v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
			       ~(BIT(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
				 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE)),
			       V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
	v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_0,
			       ~BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_0),
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
	v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
			       ~BIT(V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME),
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 1, 1, 1);
	if (h->error) {
		int err = h->error;

		v4l2_ctrl_handler_free(h);
		return err;
	}
	ctx->fh.ctrl_handler = h;
	return v4l2_ctrl_handler_setup(h);
}

/* ---------------------------------------------------------------------- */
/* Queues                                                                 */
/* ---------------------------------------------------------------------- */

static int ave_queue_setup(struct vb2_queue *vq, unsigned int *nbuf,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vq);
	u32 size = V4L2_TYPE_IS_OUTPUT(vq->type) ? ctx->out_size : ctx->cap_size;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int ave_buf_prepare(struct vb2_buffer *vb)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = V4L2_TYPE_IS_OUTPUT(vb->type) ? ctx->out_size : ctx->cap_size;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	/*
	 * The hardware reads each plane from its IOVA, which must be 64-byte
	 * aligned (kext 0xeb0768); dma-contig gives page alignment, and the
	 * chroma offset bytesperline * height is a multiple of 64 * 16.
	 */
	if (V4L2_TYPE_IS_OUTPUT(vb->type) &&
	    (vb2_dma_contig_plane_dma_addr(vb, 0) & 63))
		return -EINVAL;
	if (!V4L2_TYPE_IS_OUTPUT(vb->type))
		vb2_set_plane_payload(vb, 0, 0);
	return 0;
}

static void ave_buf_queue(struct vb2_buffer *vb)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (!V4L2_TYPE_IS_OUTPUT(vb->type) &&
	    vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		/* Drained: this buffer carries the LAST flag and nothing else. */
		vbuf->sequence = ctx->cap_seq++;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void ave_return_bufs(struct ave_ctx *ctx, struct vb2_queue *q,
			    enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *vb;

	for (;;) {
		vb = V4L2_TYPE_IS_OUTPUT(q->type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vb)
			break;
		v4l2_m2m_buf_done(vb, state);
	}
}

static int ave_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(q);
	struct ave_v4l2 *av = ctx->av;
	struct vb2_queue *other = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
		V4L2_TYPE_IS_OUTPUT(q->type) ? V4L2_BUF_TYPE_VIDEO_CAPTURE :
					       V4L2_BUF_TYPE_VIDEO_OUTPUT);
	int ret = 0;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->out_seq = 0;
	else
		ctx->cap_seq = 0;

	/* Open + Start_AVC when the second queue starts (docs/68 §7 step 5). */
	if (!vb2_is_streaming(other) || ctx->session)
		return 0;

	mutex_lock(&av->hw_mutex);
	if (av->owner && av->owner != ctx) {
		ret = -EBUSY;
	} else {
		ret = ave_enc_start(av->ave, ctx->width, ctx->height, ctx->qp,
				    AVE_CODED_SLOTS);
		if (!ret) {
			av->owner = ctx;
			ctx->session = true;
			ctx->frame_n = 0;
		}
	}
	mutex_unlock(&av->hw_mutex);
	if (ret) {
		dev_err(av->ave->dev, "v4l2: start %ux%u QP %u failed: %d\n",
			ctx->width, ctx->height, ctx->qp, ret);
		ave_return_bufs(ctx, q, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static void ave_end_session(struct ave_ctx *ctx)
{
	struct ave_v4l2 *av = ctx->av;

	if (!ctx->session)
		return;
	/* The run work may be mid-Process; the hw mutex waits it out. */
	mutex_lock(&av->hw_mutex);
	if (ave_enc_stop(av->ave))
		dev_err(av->ave->dev,
			"v4l2: Stop/Close failed; the firmware may still hold this stream's buffers\n");
	if (av->owner == ctx)
		av->owner = NULL;
	ctx->session = false;
	mutex_unlock(&av->hw_mutex);
}

static void ave_stop_streaming(struct vb2_queue *q)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(q);

	flush_work(&ctx->run_work);	/* m2m already waited for a running job */
	ave_end_session(ctx);
	ave_return_bufs(ctx, q, VB2_BUF_STATE_ERROR);
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type) &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx)) {
		static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };

		v4l2_event_queue_fh(&ctx->fh, &eos);
	}
}

static const struct vb2_ops ave_qops = {
	.queue_setup		= ave_queue_setup,
	.buf_prepare		= ave_buf_prepare,
	.buf_queue		= ave_buf_queue,
	.start_streaming	= ave_start_streaming,
	.stop_streaming		= ave_stop_streaming,
};

static int ave_queue_init(void *priv, struct vb2_queue *src,
			  struct vb2_queue *dst)
{
	struct ave_ctx *ctx = priv;
	struct vb2_queue *q[] = { src, dst };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(q); i++) {
		q[i]->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE :
				 V4L2_BUF_TYPE_VIDEO_OUTPUT;
		q[i]->io_modes = VB2_MMAP | VB2_DMABUF;
		q[i]->drv_priv = ctx;
		q[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q[i]->ops = &ave_qops;
		q[i]->mem_ops = &vb2_dma_contig_memops;
		q[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q[i]->lock = &ctx->av->dev_mutex;
		/* Allocated through AVE's own DMA ops: the IOVA is a DART one. */
		q[i]->dev = ctx->av->ave->dev;
		ret = vb2_queue_init(q[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Running a job                                                          */
/* ---------------------------------------------------------------------- */

static void ave_run_work(struct work_struct *work)
{
	struct ave_ctx *ctx = container_of(work, struct ave_ctx, run_work);
	struct ave_v4l2 *av = ctx->av;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
	dma_addr_t luma;
	size_t len = 0;
	bool key = false, idr;
	int ret;

	/*
	 * Peek, and take the buffers off the ready queues only when the frame
	 * is done. Removing the source up front let an ENCODER_CMD(STOP) that
	 * arrived mid-frame see no pending source and hand back an empty LAST
	 * buffer ahead of the real last frame, which ffmpeg then never read
	 * (f66: 59 of 60 frames).
	 */
	src = v4l2_m2m_next_src_buf(m2m);
	dst = v4l2_m2m_next_dst_buf(m2m);
	if (!src || !dst)
		goto finish;

	idr = !ctx->frame_n || ctx->force_key ||
	      (ctx->gop && !(ctx->frame_n % ctx->gop));
	luma = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);

	mutex_lock(&av->hw_mutex);
	ret = ctx->session ?
		ave_enc_encode(av->ave, ctx->frame_n, idr, luma,
			       luma + (dma_addr_t)ctx->bytesperline * ctx->height,
			       ctx->bytesperline,
			       vb2_plane_vaddr(&dst->vb2_buf, 0),
			       vb2_plane_size(&dst->vb2_buf, 0), &len, &key) :
		-EIO;
	mutex_unlock(&av->hw_mutex);
	if (ret) {
		dev_err(av->ave->dev, "v4l2: frame %u failed: %d\n",
			ctx->frame_n, ret);
		state = VB2_BUF_STATE_ERROR;
		len = 0;
	} else {
		ctx->force_key = false;
		ctx->frame_n++;
	}

	src->sequence = ctx->out_seq++;
	dst->sequence = ctx->cap_seq++;
	v4l2_m2m_buf_copy_metadata(src, dst);
	vb2_set_plane_payload(&dst->vb2_buf, 0, len);
	dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME);
	dst->flags |= key ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
	if (v4l2_m2m_is_last_draining_src_buf(m2m, src)) {
		static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };

		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &eos);
		v4l2_m2m_mark_stopped(m2m);
	}
	v4l2_m2m_src_buf_remove(m2m);
	v4l2_m2m_dst_buf_remove(m2m);
	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
finish:
	v4l2_m2m_job_finish(av->m2m_dev, m2m);
}

static void ave_device_run(void *priv)
{
	struct ave_ctx *ctx = priv;

	queue_work(ctx->av->wq, &ctx->run_work);
}

static const struct v4l2_m2m_ops ave_m2m_ops = {
	.device_run = ave_device_run,
};

/* ---------------------------------------------------------------------- */
/* File operations                                                        */
/* ---------------------------------------------------------------------- */

static int ave_open(struct file *file)
{
	struct ave_v4l2 *av = video_drvdata(file);
	struct ave_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->av = av;
	INIT_WORK(&ctx->run_work, ave_run_work);
	ctx->width = AVE_DEF_W;
	ctx->height = AVE_DEF_H;
	ctx->bytesperline = ALIGN(AVE_DEF_W, 64);
	ctx->out_size = ave_out_size(ctx->bytesperline, AVE_DEF_H);
	ctx->cap_size = ave_cap_size(AVE_DEF_W, AVE_DEF_H);
	ctx->timeperframe = (struct v4l2_fract){ 1, 30 };
	ctx->qp = AVE_DEF_QP;

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ret = ave_init_ctrls(ctx);
	if (ret)
		goto err_fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(av->m2m_dev, ctx, ave_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrl;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int ave_release(struct file *file)
{
	struct ave_ctx *ctx = fh_to_ctx(file);
	struct ave_v4l2 *av = ctx->av;

	mutex_lock(&av->dev_mutex);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);	/* stops both queues */
	mutex_unlock(&av->dev_mutex);
	ave_end_session(ctx);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations ave_fops = {
	.owner		= THIS_MODULE,
	.open		= ave_open,
	.release	= ave_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

/* ---------------------------------------------------------------------- */
/* Registration                                                           */
/* ---------------------------------------------------------------------- */

int ave_v4l2_register(struct ave_device *ave)
{
	struct ave_v4l2 *av;
	int ret;

	av = kzalloc(sizeof(*av), GFP_KERNEL);
	if (!av)
		return -ENOMEM;
	av->ave = ave;
	mutex_init(&av->dev_mutex);
	mutex_init(&av->hw_mutex);
	av->wq = alloc_ordered_workqueue("apple-ave", 0);
	if (!av->wq) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = v4l2_device_register(ave->dev, &av->v4l2_dev);
	if (ret)
		goto err_wq;
	av->m2m_dev = v4l2_m2m_init(&ave_m2m_ops);
	if (IS_ERR(av->m2m_dev)) {
		ret = PTR_ERR(av->m2m_dev);
		goto err_v4l2;
	}

	av->vfd = (struct video_device) {
		.fops		= &ave_fops,
		.ioctl_ops	= &ave_ioctl_ops,
		.release	= video_device_release_empty,
		.lock		= &av->dev_mutex,
		.v4l2_dev	= &av->v4l2_dev,
		.vfl_dir	= VFL_DIR_M2M,
		.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
	};
	strscpy(av->vfd.name, "apple-ave-enc", sizeof(av->vfd.name));
	video_set_drvdata(&av->vfd, av);
	ret = video_register_device(&av->vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;

	ave->v4l2 = av;
	dev_info(ave->dev, "v4l2: H.264 encoder at /dev/video%d\n",
		 av->vfd.num);
	return 0;

err_m2m:
	v4l2_m2m_release(av->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&av->v4l2_dev);
err_wq:
	destroy_workqueue(av->wq);
err_free:
	kfree(av);
	return ret;
}

void ave_v4l2_unregister(struct ave_device *ave)
{
	struct ave_v4l2 *av = ave->v4l2;

	if (!av)
		return;
	video_unregister_device(&av->vfd);
	v4l2_m2m_release(av->m2m_dev);
	v4l2_device_unregister(&av->v4l2_dev);
	destroy_workqueue(av->wq);
	ave->v4l2 = NULL;
	kfree(av);
}
