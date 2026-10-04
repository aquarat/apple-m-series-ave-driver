// SPDX-License-Identifier: GPL-2.0-only
/*
 * fake-ave-v4l2.c - an LD_PRELOAD emulation of the AVE V4L2 encoder as user
 * space sees it, to test ffmpeg's v4l2m2m encoder wrapper without the
 * hardware, and without touching any real /dev/video* node.
 *
 * What it emulates, after driver/ave_v4l2.c and the kernel's v4l2-mem2mem
 * and videobuf2 cores:
 *  - single-planar NV12/P010 OUTPUT, H.264/HEVC CAPTURE; width and height
 *    rounded up to 16, bytesperline to 64, sizeimage as ave_out_size();
 *  - the OUTPUT crop (right/bottom, less than one macroblock);
 *  - the controls ffmpeg sets: B_FRAMES 0..2 (clamped), HEADER_MODE (only
 *    JOINED_WITH_1ST_FRAME: SEPARATE is refused), BITRATE_MODE (CQ or VBR),
 *    FRAME_RC_ENABLE, the QPs, GOP_SIZE, FORCE_KEY_FRAME, ...;
 *  - the B-frame planner: driver/ave_gop.h, included as it is, with the
 *    driver's job_ready rule (a CAPTURE buffer for every frame of a group)
 *    and its B-frame latch (none when the GOP is too short);
 *  - completion in decode order (each group's anchor first, then its Bs),
 *    each CAPTURE buffer with its own source's timestamp and the
 *    KEYFRAME/PFRAME/BFRAME flag;
 *  - the drain: ENCODER_CMD STOP, the last packet flagged LAST plus the EOS
 *    event, EPIPE after it, an empty LAST buffer when nothing is pending;
 *  - poll() as v4l2_m2m_poll(): EPOLLERR, EPOLLOUT, EPOLLIN, EPOLLPRI;
 *  - STREAMOFF/STREAMON: a new session (frame numbering restarts, IDR first).
 * Coding takes FAKE_AVE_DELAY_US per job on a worker thread.
 *
 * The coded data is not real: every packet is an access unit of a template
 * H.264 stream (FAKE_AVE_TEMPLATE: Annex B; its first access unit an IDR
 * with SPS and PPS, then optionally a non-IDR one): SPS+PPS+IDR slice for
 * key frames, the non-IDR slice for the others. Timestamps, flags, packet
 * order and the containers can be checked; decoding is meaningful only for
 * the key frames.
 *
 * Environment:
 *   FAKE_AVE_PATH        the path to emulate (required); give ffmpeg the same
 *                        path with -device, so that it never scans /dev/video*
 *   FAKE_AVE_TEMPLATE    the template stream (required)
 *   FAKE_AVE_DUMP        append every source picture (its crop, NV12 or P010,
 *                        tightly packed, in presentation order) to this file
 *   FAKE_AVE_SHUFFLE=1   complete each group's frames in a random order
 *   FAKE_AVE_EOS_EARLY=1 queue the EOS event 5 ms before the LAST buffer
 *   FAKE_AVE_DELAY_US    coding time per job (default 2000)
 *   FAKE_AVE_LOG=1       log the ioctls to stderr
 *
 * Build: cc -O2 -Wall -shared -fPIC -I../../driver -o fake-ave-v4l2.so \
 *           fake-ave-v4l2.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

typedef uint32_t u32;
#include "ave_gop.h"

#define MAXBUF   32
#define MIN_W    192
#define MIN_H    96
#define MAX_W    4096
#define MAX_H    4096
#define ALIGNUP(x, a) (((x) + (a) - 1) / (a) * (a))
#define CODEC_H264 0
#define CODEC_HEVC 1
#define CAP_OFFSET 0x40000000u

enum bstate { B_DEQ, B_QUEUED, B_DONE };

struct fbuf {
	void *mem;
	size_t len;
	enum bstate st;
	struct timeval ts;
	u32 bytesused, flags, seq;
};

struct fq {
	struct fbuf b[MAXBUF];
	int n;
	bool streaming, last_dequeued;
	int rdy[MAXBUF], nrdy;		/* queued, not yet taken by a job */
	int done[MAXBUF], ndone;	/* done, not yet dequeued */
};

static struct {
	pthread_mutex_t mtx;
	pthread_cond_t cv;
	pthread_t worker;
	bool worker_up, running;
	int fd;
	/* formats */
	u32 w, h, bpl, out_fourcc, out_size, cap_size, codec;
	struct v4l2_rect crop;
	struct v4l2_fract tpf;
	/* controls */
	int b_frames, p_refs, gop, rc, bitrate, bitrate_mode, force_key;
	int qp, qp_p, qp_b, qp_min, qp_max, hevc_qp, hevc_qp_p, hevc_qp_b;
	int profile, level, entropy, hevc_profile, hevc_level;
	struct fq q[2];			/* 0: OUTPUT, 1: CAPTURE */
	/* v4l2-mem2mem drain state */
	bool is_draining, has_stopped, next_buf_last;
	int last_src;			/* OUTPUT buffer index, -1: none */
	int eos_events;
	/* session */
	bool session;
	u32 nb, frame_n, out_seq, cap_seq;
	struct ave_gop plan;
	/* template */
	uint8_t *key_au, *p_au;
	size_t key_len, p_len;
	/* options */
	const char *path, *dump;
	bool shuffle, eos_early, log;
	int delay_us;
	unsigned rnd;
	/* statistics */
	unsigned frames, max_held, sessions;
} F = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
	.fd = -1, .last_src = -1 };

static int (*real_open)(const char *, int, ...);
static int (*real_close)(int);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);

static void flog(const char *fmt, ...)
{
	va_list ap;

	if (!F.log)
		return;
	va_start(ap, fmt);
	fputs("[fake-ave] ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void die(const char *msg)
{
	fprintf(stderr, "[fake-ave] FATAL: %s\n", msg);
	abort();
}

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);

	return v && *v ? atoi(v) : def;
}

static void resolve(void)
{
	if (real_open)
		return;
	real_close  = dlsym(RTLD_NEXT, "close");
	real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
	real_poll   = dlsym(RTLD_NEXT, "poll");
	real_mmap   = dlsym(RTLD_NEXT, "mmap");
	real_munmap = dlsym(RTLD_NEXT, "munmap");
	F.path      = getenv("FAKE_AVE_PATH");
	F.dump      = getenv("FAKE_AVE_DUMP");
	F.shuffle   = env_int("FAKE_AVE_SHUFFLE", 0);
	F.eos_early = env_int("FAKE_AVE_EOS_EARLY", 0);
	F.log       = env_int("FAKE_AVE_LOG", 0);
	F.delay_us  = env_int("FAKE_AVE_DELAY_US", 2000);
	F.rnd       = 2463534242u;
	real_open   = dlsym(RTLD_NEXT, "open");
}

/* ---- template ---------------------------------------------------------- */

static const uint8_t *next_sc(const uint8_t *p, const uint8_t *end)
{
	for (; end - p >= 3; p++)
		if (!p[0] && !p[1] && p[2] == 1)
			return p;
	return end;
}

static void au_append(uint8_t **au, size_t *len, const uint8_t *nal, size_t n)
{
	static const uint8_t sc[4] = { 0, 0, 0, 1 };

	*au = realloc(*au, *len + 4 + n);
	if (!*au)
		die("out of memory");
	memcpy(*au + *len, sc, 4);
	memcpy(*au + *len + 4, nal, n);
	*len += 4 + n;
}

static void load_template(void)
{
	const char *path = getenv("FAKE_AVE_TEMPLATE");
	const uint8_t *p, *end, *idr = NULL;
	size_t idr_len = 0;
	bool have_p = false;
	uint8_t *data;
	FILE *f;
	long size;

	if (F.key_au)
		return;
	if (!path || !(f = fopen(path, "rb")))
		die("set FAKE_AVE_TEMPLATE to an Annex B H.264 file");
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(size);
	if (!data || fread(data, 1, size, f) != (size_t)size)
		die("cannot read the template");
	fclose(f);

	end = data + size;
	p = next_sc(data, end);
	while (p < end) {
		const uint8_t *nal = p + 3, *nx = next_sc(nal, end), *ne = nx;
		int type;

		while (ne > nal && !ne[-1])
			ne--;
		type = nal[0] & 0x1f;
		if ((type == 7 || type == 8) && !idr) {
			au_append(&F.key_au, &F.key_len, nal, ne - nal);
		} else if (type == 5 && !have_p) {
			au_append(&F.key_au, &F.key_len, nal, ne - nal);
			idr = nal;
			idr_len = ne - nal;
		} else if (type == 1 && idr && !have_p) {
			au_append(&F.p_au, &F.p_len, nal, ne - nal);
			have_p = true;
		}
		p = nx;
	}
	if (!idr)
		die("the template has no IDR slice");
	if (!have_p) {		/* no P slice: the IDR slice, retyped */
		au_append(&F.p_au, &F.p_len, idr, idr_len);
		F.p_au[4] = (F.p_au[4] & 0xe0) | 1;
	}
	free(data);
}

/* ---- formats ----------------------------------------------------------- */

static u32 out_size(u32 codec, u32 bpl, u32 h)
{
	return codec == CODEC_HEVC ? bpl * h + bpl * (ALIGNUP(h, 64) / 2) : bpl * h * 3 / 2;
}

static u32 cap_size(u32 w, u32 h)
{
	return ALIGNUP(w * h * 3 / 2 + 65536, 4096);
}

static void clamp_size(u32 *w, u32 *h)
{
	*w = ALIGNUP(*w, 16);
	*h = ALIGNUP(*h, 16);
	*w = *w < MIN_W ? MIN_W : *w > MAX_W ? MAX_W : *w;
	*h = *h < MIN_H ? MIN_H : *h > MAX_H ? MAX_H : *h;
}

static void fill_out(struct v4l2_pix_format *p, u32 w, u32 h)
{
	const bool p010 = p->pixelformat == V4L2_PIX_FMT_P010;

	clamp_size(&w, &h);
	p->width = w;
	p->height = h;
	p->pixelformat = p010 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = ALIGNUP(w, 64) * (p010 ? 2 : 1);
	p->sizeimage = out_size(F.codec, p->bytesperline, h);
	if (p->colorspace == V4L2_COLORSPACE_DEFAULT)
		p->colorspace = V4L2_COLORSPACE_REC709;
}

static void fill_cap(struct v4l2_pix_format *p, u32 req)
{
	p->width = F.w;
	p->height = F.h;
	if (p->pixelformat != V4L2_PIX_FMT_H264 && p->pixelformat != V4L2_PIX_FMT_HEVC)
		p->pixelformat = F.codec == CODEC_HEVC ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = 0;
	p->sizeimage = req > cap_size(F.w, F.h) ? req : cap_size(F.w, F.h);
}

/* ---- queues and the m2m job -------------------------------------------- */

static struct fq *qof(u32 type)
{
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return &F.q[0];
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return &F.q[1];
	return NULL;
}

static void rdy_remove(struct fq *q, int idx)
{
	for (int i = 0; i < q->nrdy; i++)
		if (q->rdy[i] == idx) {
			memmove(q->rdy + i, q->rdy + i + 1, (q->nrdy - i - 1) * sizeof(int));
			q->nrdy--;
			return;
		}
	die("rdy_remove: buffer not queued");
}

static void buf_done(struct fq *q, int idx)
{
	q->b[idx].st = B_DONE;
	q->done[q->ndone++] = idx;
	pthread_cond_broadcast(&F.cv);
}

static void mark_stopped(void)
{
	F.next_buf_last = false;
	F.last_src = -1;
	F.is_draining = false;
	F.has_stopped = true;
}

static bool is_last_src(int idx)
{
	return F.is_draining && idx >= 0 && idx == F.last_src;
}

/* v4l2_m2m_last_buffer_done(): an empty CAPTURE buffer flagged LAST */
static void last_buffer_done(int idx)
{
	struct fbuf *b = &F.q[1].b[idx];

	b->flags = V4L2_BUF_FLAG_LAST;
	b->bytesused = 0;
	b->seq = F.cap_seq++;
	buf_done(&F.q[1], idx);
	mark_stopped();
}

static void dump_source(int idx)
{
	const struct fbuf *b = &F.q[0].b[idx];
	const u32 bps = F.out_fourcc == V4L2_PIX_FMT_P010 ? 2 : 1;
	FILE *f;
	u32 y;

	F.frames++;
	if (!F.dump)
		return;
	f = fopen(F.dump, "ab");
	if (!f)
		die("cannot open FAKE_AVE_DUMP");
	for (y = 0; y < F.crop.height; y++)
		fwrite((uint8_t *)b->mem + (size_t)y * F.bpl, 1, F.crop.width * bps, f);
	for (y = 0; y < (F.crop.height + 1) / 2; y++)
		fwrite((uint8_t *)b->mem + (size_t)F.bpl * F.h + (size_t)y * F.bpl, 1,
		       ALIGNUP(F.crop.width, 2) * bps, f);
	fclose(f);
}

/*
 * HEVC: Annex B with HEVC NAL unit headers and filler payloads - VPS, SPS,
 * PPS and IDR_W_RADL for key frames, TRAIL_R for P, TRAIL_N for B. Nothing
 * can parse or decode it; it is for raw (-f hevc) output only.
 */
static const uint8_t hevc_key[] = {
	0, 0, 0, 1, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60,
	0, 0, 0, 1, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03,
	0, 0, 0, 1, 0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40,
	0, 0, 0, 1, 0x26, 0x01, 0xaf, 0x1d, 0x80, 0xaa, 0x55, 0xaa, 0x55,
};
static const uint8_t hevc_p[] = { 0, 0, 0, 1, 0x02, 0x01, 0xd0, 0x2d, 0xaa, 0x55, 0xaa };
static const uint8_t hevc_b[] = { 0, 0, 0, 1, 0x00, 0x01, 0xe0, 0x2d, 0x55, 0xaa, 0x55 };

/* code one frame of the given type from source s into CAPTURE buffer d */
static void code_frame(int s, int d, u32 type, bool last)
{
	struct fbuf *src = &F.q[0].b[s], *dst = &F.q[1].b[d];
	const uint8_t *au = type == AVE_GOP_IDR ? F.key_au : F.p_au;
	size_t len = type == AVE_GOP_IDR ? F.key_len : F.p_len;

	if (F.codec == CODEC_HEVC) {
		au  = type == AVE_GOP_IDR ? hevc_key : type == AVE_GOP_B ? hevc_b : hevc_p;
		len = type == AVE_GOP_IDR ? sizeof(hevc_key) :
		      type == AVE_GOP_B ? sizeof(hevc_b) : sizeof(hevc_p);
	}

	if (len > dst->len)
		die("CAPTURE buffer too small");
	memcpy(dst->mem, au, len);
	dst->bytesused = len;
	dst->ts = src->ts;
	dst->seq = F.cap_seq++;
	dst->flags = type == AVE_GOP_IDR ? V4L2_BUF_FLAG_KEYFRAME :
		     type == AVE_GOP_B ? V4L2_BUF_FLAG_BFRAME : V4L2_BUF_FLAG_PFRAME;
	flog("coded src %d (ts %ld.%06ld) as %s into cap %d%s\n", s,
	     (long)src->ts.tv_sec, (long)src->ts.tv_usec,
	     type == AVE_GOP_IDR ? "IDR" : type == AVE_GOP_B ? "B" : "P", d,
	     last ? " LAST" : "");
	if (last) {
		/* the driver queues the event before the buffer is done */
		dst->flags |= V4L2_BUF_FLAG_LAST;
		F.eos_events++;
		pthread_cond_broadcast(&F.cv);
		if (F.eos_early) {
			pthread_mutex_unlock(&F.mtx);
			usleep(5000);
			pthread_mutex_lock(&F.mtx);
		}
		mark_stopped();
	}
	rdy_remove(&F.q[1], d);
	buf_done(&F.q[1], d);
}

/* the driver's ave_b_next(): what the planner would do now */
static enum ave_gop_act b_next(int *src)
{
	const struct ave_gop *g = &F.plan;
	const struct fq *o = &F.q[0];
	u32 i;

	for (i = 0; i <= g->n_held; i++)
		src[i] = (int)i < o->nrdy ? o->rdy[i] : -1;
	if (src[g->n_held] < 0)
		return ave_gop_decide(g, false, false,
				      g->n_held && is_last_src(src[g->n_held - 1]));
	return ave_gop_decide(g, true, F.force_key, is_last_src(src[g->n_held]));
}

static bool job_ready(void)
{
	int src[AVE_GOP_BATCH_MAX + 1];
	enum ave_gop_act a;
	u32 need;

	if (!F.session || F.running || !F.q[0].streaming || !F.q[1].streaming)
		return false;
	if (!F.nb)
		return F.q[0].nrdy && F.q[1].nrdy;
	a = b_next(src);
	if (a == AVE_GOP_NONE)
		return false;
	need = ave_gop_need(&F.plan, a);
	return (u32)F.q[1].nrdy >= (need ? need : 1);
}

static void b_batch(const struct ave_gop_batch *b, const int *src)
{
	int order[AVE_GOP_BATCH_MAX], dst[AVE_GOP_BATCH_MAX];
	bool last = false;
	u32 i, k, n = b->n;

	/* the anchor (the batch's last, its P or IDR) first, then the Bs */
	order[0] = n - 1;
	for (i = 0; i + 1 < n; i++)
		order[i + 1] = i;
	if (F.shuffle)
		for (i = n - 1; i > 0; i--) {
			int j, t;

			F.rnd = F.rnd * 1664525u + 1013904223u;
			j = (F.rnd >> 8) % (i + 1);
			t = order[i];
			order[i] = order[j];
			order[j] = t;
		}
	for (i = 0; i < n; i++)
		last |= is_last_src(src[b->f[i].src]);
	for (k = 0; k < n; k++)
		dst[k] = F.q[1].rdy[k];
	for (k = 0; k < n; k++) {
		i = order[k];
		code_frame(src[b->f[i].src], dst[k], b->f[i].type, last && k == n - 1);
	}
	for (i = 0; i < n; i++) {
		int s = src[b->f[i].src];

		rdy_remove(&F.q[0], s);
		buf_done(&F.q[0], s);
	}
}

static void do_job(void)
{
	int src[AVE_GOP_BATCH_MAX + 1];
	struct ave_gop_batch b[2];
	enum ave_gop_act a;
	u32 i, nbat, need;

	if (!F.nb) {
		int s, d;
		bool idr;

		if (!F.q[0].nrdy || !F.q[1].nrdy)
			return;
		s = F.q[0].rdy[0];
		d = F.q[1].rdy[0];
		idr = !F.frame_n || F.force_key || (F.gop && !(F.frame_n % F.gop));
		F.force_key = 0;
		F.frame_n++;
		F.q[0].b[s].seq = F.out_seq++;
		dump_source(s);
		code_frame(s, d, idr ? AVE_GOP_IDR : AVE_GOP_P, is_last_src(s));
		rdy_remove(&F.q[0], s);
		buf_done(&F.q[0], s);
		return;
	}

	a = b_next(src);
	need = ave_gop_need(&F.plan, a);
	if (a == AVE_GOP_NONE || (u32)F.q[1].nrdy < (need ? need : 1))
		return;
	if (a != AVE_GOP_CLOSE) {
		F.q[0].b[src[F.plan.n_held]].seq = F.out_seq++;
		F.frame_n++;
		dump_source(src[F.plan.n_held]);
	}
	if (a == AVE_GOP_KEY)
		F.force_key = 0;
	nbat = ave_gop_commit(&F.plan, a, b);
	if (F.plan.n_held > F.max_held)
		F.max_held = F.plan.n_held;
	for (i = 0; i < nbat; i++)
		b_batch(&b[i], src);
}

static void *worker_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&F.mtx);
	for (;;) {
		while (!job_ready())
			pthread_cond_wait(&F.cv, &F.mtx);
		/* coding time; STREAMOFF waits for a running job */
		F.running = true;
		pthread_mutex_unlock(&F.mtx);
		usleep(F.delay_us);
		pthread_mutex_lock(&F.mtx);
		if (F.session && F.q[0].streaming && F.q[1].streaming)
			do_job();
		F.running = false;
		pthread_cond_broadcast(&F.cv);
	}
	return NULL;
}

/* ---- ioctls ------------------------------------------------------------ */

static void free_bufs(struct fq *q)
{
	for (int i = 0; i < q->n; i++)
		if (q->b[i].mem)
			real_munmap(q->b[i].mem, q->b[i].len);
	memset(q, 0, sizeof(*q));
}

static void end_session(void)
{
	while (F.running)
		pthread_cond_wait(&F.cv, &F.mtx);
	F.session = false;
}

static void start_session(void)
{
	u32 nb = F.b_frames;

	if (F.session || !F.q[0].streaming || !F.q[1].streaming)
		return;
	if (nb && F.gop && (u32)F.gop < nb + 2)
		nb = F.gop >= 3 ? F.gop - 2 : 0;
	F.nb = nb;
	F.frame_n = 0;
	ave_gop_init(&F.plan, nb, F.gop);
	F.session = true;
	F.sessions++;
	flog("session %u: %ux%u (crop %ux%u) bpl %u, %s, B %u, GOP %d, RC %d mode %d, QP %d/%d/%d\n",
	     F.sessions, F.w, F.h, F.crop.width, F.crop.height, F.bpl,
	     F.codec == CODEC_HEVC ? "HEVC" : "H.264", nb, F.gop, F.rc, F.bitrate_mode,
	     F.codec == CODEC_HEVC ? F.hevc_qp : F.qp,
	     F.codec == CODEC_HEVC ? F.hevc_qp_p : F.qp_p,
	     F.codec == CODEC_HEVC ? F.hevc_qp_b : F.qp_b);
	pthread_cond_broadcast(&F.cv);
}

static int set_ctrl(struct v4l2_ext_control *c)
{
	int v = c->value;
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

	switch (c->id) {
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:          F.b_frames = c->value = CLAMP(v, 0, AVE_GOP_B_MAX); break;
	case V4L2_CID_MPEG_VIDEO_REF_NUMBER_FOR_PFRAMES: F.p_refs = c->value = CLAMP(v, 1, 2); break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:          F.gop = c->value = CLAMP(v, 0, 65535); break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:   F.force_key = 1; break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:   F.qp = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP:   F.qp_p = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP:   F.qp_b = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:       F.qp_min = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:       F.qp_max = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP:   F.hevc_qp = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP:   F.hevc_qp_p = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP:   F.hevc_qp_b = c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP:
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP:       c->value = CLAMP(v, 0, 51); break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:           F.bitrate = c->value = CLAMP(v, 1, 400000000); break;
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:   F.rc = c->value = CLAMP(v, 0, 1); break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		if (v != V4L2_MPEG_VIDEO_BITRATE_MODE_CQ && v != V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
			return -EINVAL;
		F.bitrate_mode = v;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		if (v != V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE &&
		    v != V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE &&
		    v != V4L2_MPEG_VIDEO_H264_PROFILE_MAIN && v != V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)
			return -EINVAL;
		F.profile = v;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE:
		if (v < 0 || v > 1)
			return -EINVAL;
		F.entropy = v;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		if (v < 0 || v > V4L2_MPEG_VIDEO_H264_LEVEL_5_2)
			return -EINVAL;
		F.level = v;
		break;
	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
		if (v != V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME)
			return -EINVAL;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		if (v != V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN && v != V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10)
			return -EINVAL;
		F.hevc_profile = v;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_TIER:
		if (v != V4L2_MPEG_VIDEO_HEVC_TIER_MAIN)
			return -EINVAL;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		if (v < 0 || v > V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2)
			return -EINVAL;
		F.hevc_level = v;
		break;
	default:
		return -EINVAL;
	}
	return 0;
#undef CLAMP
}

static int get_ctrl(struct v4l2_ext_control *c)
{
	switch (c->id) {
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:          c->value = F.b_frames; break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:          c->value = F.gop; break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:   c->value = F.qp; break;
	case V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP:   c->value = F.qp_p; break;
	case V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP:   c->value = F.qp_b; break;
	case V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP:   c->value = F.hevc_qp; break;
	case V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP:   c->value = F.hevc_qp_p; break;
	case V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP:   c->value = F.hevc_qp_b; break;
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:   c->value = F.rc; break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:      c->value = F.bitrate_mode; break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:           c->value = F.bitrate; break;
	case V4L2_CID_MIN_BUFFERS_FOR_OUTPUT:       c->value = F.b_frames ? F.b_frames + 2 : 1; break;
	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
		c->value = V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void reset_state(void)
{
	free_bufs(&F.q[0]);
	free_bufs(&F.q[1]);
	F.w = 1280;
	F.h = 720;
	F.bpl = 1280;
	F.codec = CODEC_H264;
	F.out_fourcc = V4L2_PIX_FMT_NV12;
	F.out_size = out_size(F.codec, F.bpl, F.h);
	F.cap_size = cap_size(F.w, F.h);
	F.crop = (struct v4l2_rect){ 0, 0, F.w, F.h };
	F.tpf = (struct v4l2_fract){ 1, 30 };
	F.b_frames = F.gop = F.rc = F.force_key = 0;
	F.p_refs = 1;
	F.bitrate = 10000000;
	F.bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
	F.qp = F.hevc_qp = 30;
	F.qp_p = F.qp_b = F.hevc_qp_p = F.hevc_qp_b = 0;
	F.is_draining = F.has_stopped = F.next_buf_last = false;
	F.last_src = -1;
	F.eos_events = 0;
	F.session = false;
	F.out_seq = F.cap_seq = 0;
}

static int do_ioctl(unsigned long req, void *arg)
{
	switch (req) {
	case VIDIOC_QUERYCAP: {
		struct v4l2_capability *cap = arg;

		memset(cap, 0, sizeof(*cap));
		strcpy((char *)cap->driver, "apple-ave");
		strcpy((char *)cap->card, "Apple AVE H.264/HEVC encoder");
		strcpy((char *)cap->bus_info, "platform:apple-ave");
		cap->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
		cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
		return 0;
	}
	case VIDIOC_ENUM_FMT: {
		struct v4l2_fmtdesc *f = arg;

		if (f->index > 1 || !qof(f->type))
			return -EINVAL;
		if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
			f->pixelformat = f->index ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
			f->flags = 0;
		} else {
			f->pixelformat = f->index ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
			f->flags = V4L2_FMT_FLAG_COMPRESSED;
		}
		return 0;
	}
	case VIDIOC_G_FMT: {
		struct v4l2_format *f = arg;

		if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
			f->fmt.pix.pixelformat = F.out_fourcc;
			fill_out(&f->fmt.pix, F.w, F.h);
		} else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			f->fmt.pix.pixelformat = F.codec == CODEC_HEVC ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
			fill_cap(&f->fmt.pix, F.cap_size);
		} else {
			return -EINVAL;
		}
		return 0;
	}
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		struct fq *q = qof(f->type);

		if (!q)
			return -EINVAL;
		if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
			fill_out(&f->fmt.pix, f->fmt.pix.width, f->fmt.pix.height);
		else
			fill_cap(&f->fmt.pix, f->fmt.pix.sizeimage);
		if (req == VIDIOC_TRY_FMT)
			return 0;
		if (q->n)
			return -EBUSY;
		if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
			F.w = f->fmt.pix.width;
			F.h = f->fmt.pix.height;
			F.bpl = f->fmt.pix.bytesperline;
			F.out_fourcc = f->fmt.pix.pixelformat;
			F.out_size = f->fmt.pix.sizeimage;
			F.crop = (struct v4l2_rect){ 0, 0, F.w, F.h };
			if (F.cap_size < cap_size(F.w, F.h))
				F.cap_size = cap_size(F.w, F.h);
		} else {
			u32 codec = f->fmt.pix.pixelformat == V4L2_PIX_FMT_HEVC ? CODEC_HEVC : CODEC_H264;

			if (codec != F.codec) {
				if (F.q[0].n)
					return -EBUSY;
				F.codec = codec;
				if (codec != CODEC_HEVC && F.out_fourcc == V4L2_PIX_FMT_P010) {
					F.out_fourcc = V4L2_PIX_FMT_NV12;
					F.bpl = ALIGNUP(F.w, 64);
				}
				F.out_size = out_size(codec, F.bpl, F.h);
			}
			F.cap_size = f->fmt.pix.sizeimage;
		}
		flog("S_FMT %s: %ux%u bpl %u size %u\n",
		     f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT ? "OUTPUT" : "CAPTURE",
		     f->fmt.pix.width, f->fmt.pix.height, f->fmt.pix.bytesperline,
		     f->fmt.pix.sizeimage);
		return 0;
	}
	case VIDIOC_G_SELECTION: {
		struct v4l2_selection *s = arg;

		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		if (s->target == V4L2_SEL_TGT_CROP)
			s->r = F.crop;
		else if (s->target == V4L2_SEL_TGT_CROP_DEFAULT || s->target == V4L2_SEL_TGT_CROP_BOUNDS)
			s->r = (struct v4l2_rect){ 0, 0, F.w, F.h };
		else
			return -EINVAL;
		return 0;
	}
	case VIDIOC_S_SELECTION: {
		struct v4l2_selection *s = arg;
		u32 minw, minh, w, h;

		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT || s->target != V4L2_SEL_TGT_CROP)
			return -EINVAL;
		if (F.q[0].streaming)
			return -EBUSY;
		minw = ALIGNUP(F.w, 16) - 14;
		minh = ALIGNUP(F.h, 16) - 14;
		minw = minw < MIN_W ? MIN_W : minw;
		minh = minh < MIN_H ? MIN_H : minh;
		w = ALIGNUP(s->r.width, 2);
		h = ALIGNUP(s->r.height, 2);
		s->r.left = s->r.top = 0;
		s->r.width = w < minw ? minw : w > F.w ? F.w : w;
		s->r.height = h < minh ? minh : h > F.h ? F.h : h;
		F.crop = s->r;
		flog("crop %ux%u of %ux%u\n", F.crop.width, F.crop.height, F.w, F.h);
		return 0;
	}
	case VIDIOC_G_PARM:
	case VIDIOC_S_PARM: {
		struct v4l2_streamparm *a = arg;
		struct v4l2_fract *t = &a->parm.output.timeperframe;

		if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		if (req == VIDIOC_S_PARM && t->numerator && t->denominator)
			F.tpf = *t;
		a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
		*t = F.tpf;
		return 0;
	}
	case VIDIOC_S_EXT_CTRLS:
	case VIDIOC_G_EXT_CTRLS:
	case VIDIOC_TRY_EXT_CTRLS: {
		struct v4l2_ext_controls *cs = arg;

		for (u32 i = 0; i < cs->count; i++) {
			struct v4l2_ext_control c = cs->controls[i];
			int ret = req == VIDIOC_G_EXT_CTRLS ? get_ctrl(&c) : set_ctrl(&c);

			if (ret) {
				cs->error_idx = i;
				flog("%s ctrl 0x%x = %d refused\n",
				     req == VIDIOC_G_EXT_CTRLS ? "get" : "set", c.id, c.value);
				return ret;
			}
			if (req != VIDIOC_TRY_EXT_CTRLS)
				cs->controls[i] = c;
			flog("%s ctrl 0x%x = %d\n", req == VIDIOC_G_EXT_CTRLS ? "get" : "set",
			     c.id, c.value);
		}
		return 0;
	}
	case VIDIOC_SUBSCRIBE_EVENT:
	case VIDIOC_UNSUBSCRIBE_EVENT:
		return 0;
	case VIDIOC_DQEVENT: {
		struct v4l2_event *ev = arg;

		if (!F.eos_events)
			return -ENOENT;
		memset(ev, 0, sizeof(*ev));
		ev->type = V4L2_EVENT_EOS;
		ev->pending = --F.eos_events;
		return 0;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *r = arg;
		struct fq *q = qof(r->type);
		bool out = r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
		u32 count = r->count;

		if (!q || r->memory != V4L2_MEMORY_MMAP)
			return -EINVAL;
		if (q->streaming)
			return -EBUSY;
		free_bufs(q);
		if (!count)
			return 0;
		if (F.b_frames && count < (u32)F.b_frames + (out ? 2 : 1))
			count = F.b_frames + (out ? 2 : 1);
		if (count > MAXBUF)
			count = MAXBUF;
		for (u32 i = 0; i < count; i++) {
			size_t len = out ? F.out_size : F.cap_size;
			void *m = real_mmap(NULL, len, PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			if (m == MAP_FAILED)
				return -ENOMEM;
			q->b[i].mem = m;
			q->b[i].len = len;
		}
		q->n = count;
		r->count = count;
		r->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
		flog("REQBUFS %s: %u x %zu\n", out ? "OUTPUT" : "CAPTURE", count, q->b[0].len);
		return 0;
	}
	case VIDIOC_QUERYBUF:
	case VIDIOC_QBUF:
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *vb = arg;
		struct fq *q = qof(vb->type);
		bool out = vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
		struct fbuf *b;
		int idx;

		if (!q || vb->memory != V4L2_MEMORY_MMAP)
			return -EINVAL;
		if (req == VIDIOC_DQBUF) {
			if (!q->streaming)
				return -EINVAL;
			if (!out && q->last_dequeued)
				return -EPIPE;
			if (!q->ndone)
				return -EAGAIN;
			idx = q->done[0];
			memmove(q->done, q->done + 1, (q->ndone - 1) * sizeof(int));
			q->ndone--;
			b = &q->b[idx];
			b->st = B_DEQ;
			vb->index = idx;
			vb->bytesused = b->bytesused;
			vb->flags = out ? 0 : b->flags;
			vb->field = V4L2_FIELD_NONE;
			vb->timestamp = b->ts;
			vb->sequence = b->seq;
			vb->length = b->len;
			vb->m.offset = (out ? 0 : CAP_OFFSET) + idx * 0x1000000u;
			if (!out && (b->flags & V4L2_BUF_FLAG_LAST))
				q->last_dequeued = true;
			return 0;
		}
		if (vb->index >= (u32)q->n)
			return -EINVAL;
		idx = vb->index;
		b = &q->b[idx];
		if (req == VIDIOC_QUERYBUF) {
			vb->length = b->len;
			vb->m.offset = (out ? 0 : CAP_OFFSET) + idx * 0x1000000u;
			vb->bytesused = b->st == B_DEQ ? 0 : b->bytesused;
			vb->flags = b->st == B_QUEUED ? V4L2_BUF_FLAG_QUEUED :
				    b->st == B_DONE ? V4L2_BUF_FLAG_DONE : 0;
			return 0;
		}
		/* QBUF */
		if (b->st != B_DEQ)
			return -EINVAL;
		b->st = B_QUEUED;
		b->flags = 0;
		if (out) {
			b->ts = vb->timestamp;
			b->bytesused = vb->bytesused ? vb->bytesused : b->len;
		} else {
			b->bytesused = 0;
			if (q->streaming && F.is_draining && F.next_buf_last) {
				last_buffer_done(idx);
				return 0;
			}
		}
		q->rdy[q->nrdy++] = idx;
		pthread_cond_broadcast(&F.cv);
		return 0;
	}
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF: {
		int *type = arg;
		struct fq *q = qof(*type);
		bool out = *type == V4L2_BUF_TYPE_VIDEO_OUTPUT;

		if (!q)
			return -EINVAL;
		if (req == VIDIOC_STREAMON) {
			if (q->streaming)
				return 0;
			if (out)
				F.last_src = -1;
			q->streaming = true;
			if (F.q[0].streaming && F.q[1].streaming &&
			    F.codec != CODEC_HEVC && F.out_fourcc == V4L2_PIX_FMT_P010) {
				q->streaming = false;
				return -EINVAL;
			}
			flog("STREAMON %s\n", out ? "OUTPUT" : "CAPTURE");
			start_session();
			return 0;
		}
		end_session();
		q->streaming = false;
		q->last_dequeued = false;
		for (int i = 0; i < q->n; i++)
			q->b[i].st = B_DEQ;
		q->nrdy = q->ndone = 0;
		if (out) {
			if (F.is_draining) {
				F.last_src = -1;
				if (F.q[1].nrdy) {
					int d = F.q[1].rdy[0];

					rdy_remove(&F.q[1], d);
					last_buffer_done(d);
				} else {
					F.next_buf_last = true;
				}
			}
			if (F.has_stopped)
				F.eos_events++;
		} else {
			F.next_buf_last = F.is_draining = F.has_stopped = false;
		}
		flog("STREAMOFF %s\n", out ? "OUTPUT" : "CAPTURE");
		pthread_cond_broadcast(&F.cv);
		return 0;
	}
	case VIDIOC_ENCODER_CMD:
	case VIDIOC_TRY_ENCODER_CMD: {
		struct v4l2_encoder_cmd *ec = arg;

		if (ec->cmd != V4L2_ENC_CMD_STOP && ec->cmd != V4L2_ENC_CMD_START)
			return -EINVAL;
		ec->flags = 0;
		if (req == VIDIOC_TRY_ENCODER_CMD)
			return 0;
		if (ec->cmd == V4L2_ENC_CMD_START) {
			if (F.is_draining)
				return -EBUSY;
			F.has_stopped = false;
			F.q[1].last_dequeued = false;
			return 0;
		}
		if (F.is_draining)
			return -EBUSY;
		if (F.has_stopped)
			return 0;
		F.last_src = F.q[0].nrdy ? F.q[0].rdy[F.q[0].nrdy - 1] : -1;
		F.is_draining = true;
		flog("STOP: last source %d\n", F.last_src);
		if (F.last_src < 0) {
			if (F.q[1].nrdy) {
				int d = F.q[1].rdy[0];

				rdy_remove(&F.q[1], d);
				last_buffer_done(d);
			} else {
				F.next_buf_last = true;
			}
		}
		pthread_cond_broadcast(&F.cv);
		return 0;
	}
	default:
		flog("ioctl 0x%lx not emulated\n", req);
		return -ENOTTY;
	}
}

static short poll_events(short events)
{
	const struct fq *s = &F.q[0], *d = &F.q[1];
	bool s_queued = false, d_queued = false;
	short rc = 0;

	if ((events & POLLPRI) && F.eos_events)
		rc |= POLLPRI;
	if (!(events & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM)))
		return rc;
	for (int i = 0; i < s->n; i++)
		s_queued |= s->b[i].st != B_DEQ;
	for (int i = 0; i < d->n; i++)
		d_queued |= d->b[i].st != B_DEQ;
	if ((!s->streaming || !s_queued) &&
	    (!d->streaming || (!d_queued && !d->last_dequeued)))
		return rc | POLLERR;
	if (s->ndone)
		rc |= POLLOUT | POLLWRNORM;
	if (d->ndone || d->last_dequeued)
		rc |= POLLIN | POLLRDNORM;
	return rc;
}

/* ---- the interposed calls ---------------------------------------------- */

static int fake_open(const char *path)
{
	int fd;

	pthread_mutex_lock(&F.mtx);
	if (F.fd >= 0) {
		pthread_mutex_unlock(&F.mtx);
		errno = EBUSY;
		return -1;
	}
	load_template();
	fd = real_open("/dev/null", O_RDWR);
	if (fd >= 0) {
		F.fd = fd;
		reset_state();
		if (!F.worker_up) {
			pthread_create(&F.worker, NULL, worker_main, NULL);
			F.worker_up = true;
		}
	}
	flog("open %s -> %d\n", path, fd);
	pthread_mutex_unlock(&F.mtx);
	return fd;
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;

	resolve();
	if (F.path && !strcmp(path, F.path))
		return fake_open(path);
	va_start(ap, flags);
	if (flags & (O_CREAT | O_TMPFILE))
		mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) __attribute__((alias("open")));

int __open_2(const char *path, int flags)
{
	return open(path, flags);
}

int close(int fd)
{
	resolve();
	if (fd >= 0 && fd == F.fd) {
		pthread_mutex_lock(&F.mtx);
		end_session();
		F.q[0].streaming = F.q[1].streaming = false;
		reset_state();
		F.fd = -1;
		fprintf(stderr, "[fake-ave] close: %u source frames coded in %u session(s), "
			"at most %u held as B\n", F.frames, F.sessions, F.max_held);
		pthread_mutex_unlock(&F.mtx);
	}
	return real_close(fd);
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	int ret;

	resolve();
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd < 0 || fd != F.fd)
		return real_ioctl(fd, req, arg);
	pthread_mutex_lock(&F.mtx);
	ret = do_ioctl(req, arg);
	pthread_mutex_unlock(&F.mtx);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int poll(struct pollfd *fds, nfds_t n, int timeout)
{
	struct timespec deadline;

	resolve();
	if (n != 1 || fds[0].fd < 0 || fds[0].fd != F.fd)
		return real_poll(fds, n, timeout);
	clock_gettime(CLOCK_REALTIME, &deadline);
	if (timeout > 0) {
		deadline.tv_sec += timeout / 1000;
		deadline.tv_nsec += (long)(timeout % 1000) * 1000000;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
	}
	pthread_mutex_lock(&F.mtx);
	for (;;) {
		short rc = poll_events(fds[0].events);

		fds[0].revents = rc & (fds[0].events | POLLERR | POLLHUP);
		if (fds[0].revents || !timeout)
			break;
		if (timeout < 0) {
			pthread_cond_wait(&F.cv, &F.mtx);
		} else if (pthread_cond_timedwait(&F.cv, &F.mtx, &deadline) == ETIMEDOUT) {
			fds[0].revents = 0;
			break;
		}
	}
	pthread_mutex_unlock(&F.mtx);
	return fds[0].revents ? 1 : 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	resolve();
	if (fd >= 0 && fd == F.fd) {
		void *ret = MAP_FAILED;
		struct fq *q;
		u32 idx;

		pthread_mutex_lock(&F.mtx);
		q = (u32)off >= CAP_OFFSET ? &F.q[1] : &F.q[0];
		idx = ((u32)off & ~CAP_OFFSET) / 0x1000000u;
		if (idx < (u32)q->n && len <= q->b[idx].len)
			ret = q->b[idx].mem;
		pthread_mutex_unlock(&F.mtx);
		if (ret == MAP_FAILED)
			errno = EINVAL;
		return ret;
	}
	return real_mmap(addr, len, prot, flags, fd, off);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off)
	__attribute__((alias("mmap")));

int munmap(void *addr, size_t len)
{
	resolve();
	pthread_mutex_lock(&F.mtx);
	for (int t = 0; t < 2; t++)
		for (int i = 0; i < F.q[t].n; i++)
			if (F.q[t].b[i].mem == addr) {
				pthread_mutex_unlock(&F.mtx);
				return 0;	/* freed at REQBUFS 0 or close */
			}
	pthread_mutex_unlock(&F.mtx);
	return real_munmap(addr, len);
}
