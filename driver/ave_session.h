/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - first-command session self-test.
 *
 * UNTESTED ON HARDWARE. This sends the opening command sequence (Config ->
 * Open -> Start_AVC) for a fixed-QP, I-frame-only, 8-bit 4:2:0 AVC session
 * over the IO channel and checks each firmware reply. It is a diagnostic
 * probe, not the encode path: it and, with session_frame=1, encodes one I-frame (docs/53) and is gated
 * behind the module parameter ave_session.session_selftest (off by default).
 *
 * The handshake must already be complete (ave->boot_phase == READY, the IO
 * channel bound). Call it once, after ave_start() prints
 * "Apple AVE video encoder ready".
 */
#ifndef __AVE_SESSION_H__
#define __AVE_SESSION_H__

struct ave_device;

/*
 * Run the opening command sequence. Returns 0 if every step was accepted by
 * the firmware, a negative errno otherwise (or 0 immediately when the gate
 * parameter is off, so the caller can invoke it unconditionally). Never
 * touches hardware beyond the already-live IPC transport, and frees every
 * buffer it allocates on all paths.
 */
int ave_session_selftest(struct ave_device *ave);
/* session_selftest=1 or session_frame=1: the probe-time self-test, not V4L2. */
bool ave_session_selftest_requested(void);

/*
 * Free the buffers the self-test handed to the firmware. Only safe once the
 * core cannot reach them any more: call it from ave_remove() after the power
 * has been dropped.
 */
void ave_session_release(struct ave_device *ave);

/*
 * Remove the debugfs view but keep the buffers. For the unload path that
 * leaks them on purpose: the entries must not outlive the module, or the
 * next run's capture silently picks up the previous load's frame.
 */
void ave_session_hide(struct ave_device *ave);

/*
 * Give the firmware its client back - Stop (id 6) then Close (id 12), waiting
 * for UNINIT_DONE and STOP_DONE - before anything is unmapped or gated. This
 * is what macOS does and we never did (docs/63); both replies are withheld
 * until the client's outstanding work has drained, so they are the signal
 * that the session buffers are safe to free. Needs the IPC transport, so call
 * it from ave_remove() first of all. 0 when the client was returned (or none
 * was open).
 */
int ave_session_close_client(struct ave_device *ave);

/* The encoder API the V4L2 layer drives (ave_v4l2.c, docs/68). */
int ave_enc_init(struct ave_device *ave);
/*
 * ave_enc_cfg.codec: the codec word of the Open/Stop/Close headers
 * (docs/77 §1.2). HEVC needs the 13.5 firmware ABI (docs/77 §8.1 item 4).
 */
#define AVE_ENC_CODEC_H264	0
#define AVE_ENC_CODEC_HEVC	1
/* True when this firmware ABI (and the module's settings) can run HEVC. */
bool ave_enc_hevc_supported(struct ave_device *ave);
/* One stream's parameters (docs/68 step 4). Zero means "the default". */
struct ave_enc_cfg {
	u32	codec;			/* AVE_ENC_CODEC_*; 0 = H.264 */
	u32	width, height;		/* the buffer, MB-aligned */
	u32	crop_w, crop_h;		/* SPS crop; 0 = none */
	u32	qp;			/* fixed QP, or the RC's starting QP */
	u32	qp_p, qp_b;		/* fixed QP of P / B frames; 0 = qp (docs/92) */
	u32	qp_min, qp_max;		/* RC clamp; 0,0 = 10..51 */
	u32	bitrate;		/* bit/s; 0 = fixed QP */
	u32	fps_num;		/* integer Hz (wire 0xFF4C); 0 = 30 */
	u32	fps_den;		/* wire 0xFF48, the non-droppable rate, NOT a divisor (docs/76); 0 = 1 */
	u32	slots;			/* coded slots */
	u32	profile_idc;		/* H.264 66, 77, 100; HEVC 0/1 Main, 2 Main 10 */
	u32	src_bitdepth;		/* 10 = P010 (HEVC only; docs/83); else NV12 */
	/*
	 * A floor; the size may need more. 0 = none. H.264 level_idc (10..52);
	 * HEVC general_level_idc, 30 x level (Table A.8 values only).
	 */
	u32	level_idc;
	bool	cabac;			/* H.264 only; HEVC is always CABAC */
	/*
	 * docs/81 "V4L2 implementation". Both 0 = the stream every release so
	 * far has sent. Either above its default turns on what the hardware
	 * needs for two references (3 DPB slots, both ME units: Start wire
	 * 0xFCEA, docs/94); B frames also POC type 0 (H.264) or the B RPS
	 * sets (HEVC). B frames then go through ave_enc_encode_batch();
	 * two-reference P frames still go through ave_enc_encode().
	 */
	u32	bframes;		/* B frames between anchors, 0..2 */
	u32	p_refs;			/* references per P frame, 1..2; 0 = 1 */
};
int ave_enc_start(struct ave_device *ave, const struct ave_enc_cfg *cfg);
int ave_enc_encode(struct ave_device *ave, u32 n, bool idr,
		   dma_addr_t luma, dma_addr_t chroma, u32 stride,
		   void *out, size_t out_size, size_t *out_len, bool *keyframe);
int ave_enc_stop(struct ave_device *ave);

/*
 * One frame of a batch (ave_gop.h plans them): frameNumber, which is also
 * its POC; AVE_GOP_{P,B,IDR}; the source planes, as ave_enc_encode().
 */
struct ave_enc_frame {
	u32		fn;
	u32		type;
	dma_addr_t	luma, chroma;
	u32		stride;
};

/*
 * One completion. @buf/@size: the caller's k-th output buffer, filled by
 * the k-th frame to complete (completion order = decode order). Returned:
 * @len, @frame (its index in the frames[] array, found from the reply's
 * slot and checked against the frameNumber the firmware echoes in the coded
 * header) and @type as coded (an HEVC frame the firmware made an IDR says so).
 */
struct ave_enc_out {
	void		*buf;
	size_t		size;
	size_t		len;
	u32		frame;
	u32		type;
};

/*
 * Send @count frames back to back in the order given - display order: the
 * firmware reorders a B behind its anchor (docs/81 §0 #1), so a batch must
 * end with that anchor - and collect @count completions into out[0..].
 * *@n_out: completions delivered, also on failure. 0 when all completed.
 */
int ave_enc_encode_batch(struct ave_device *ave, const struct ave_enc_frame *f,
			 u32 count, struct ave_enc_out *out, u32 *n_out);

/*
 * Ask the firmware to halt itself (command 14) so that the next load can
 * start it again without rebooting the machine. Sends on IO and waits for SVE
 * scratch 0, because this command never replies (docs/55). Returns 0 when the
 * firmware reached its wfi, a negative errno otherwise, or 0 immediately when
 * the gate parameter is off. Call it from ave_remove() while the IPC
 * transport is still live and BEFORE dropping power.
 */
int ave_session_halt(struct ave_device *ave);
bool ave_session_halt_requested(void);

#endif /* __AVE_SESSION_H__ */
