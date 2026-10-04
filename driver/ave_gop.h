/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - the B-frame mini-GOP planner of the V4L2 stream (docs/81 §3.3
 * and "V4L2 implementation").
 *
 * Pure: no I/O, no locking, no allocation, so tools/session_selftest runs it
 * as it is. The V4L2 layer feeds it one OUTPUT buffer at a time, in display
 * order, and it says whether to hold the buffer as a B or to send a batch:
 *
 *  - HOLD:   the source waits (in the m2m ready queue) for its anchor;
 *  - ANCHOR: the held Bs and this source as their P, one batch in display
 *            order (the firmware codes the P first and then releases the Bs,
 *            docs/81 §0 #1);
 *  - IDR:    the held sources close first (all B but the last, which is a
 *            P), then this source alone as an IDR. A B is never followed in
 *            display order by an IDR: an IDR flushes the DPB (closed GOP);
 *  - CLOSE:  a drain whose last source is already held: the same close.
 *
 * So the firmware never holds a B without the anchor in the same batch
 * (docs/81 R1), and every batch is complete before the next is built.
 *
 * frameNumber, which is also each picture's POC (fw 0x20ee8 AVC, 0x211b4
 * HEVC: frameNumber - frameNumber at the last IDR), is not the display
 * index. Every mini-GOP spans nb + 1 numbers whether or not it is full:
 * the Bs take anchor + 1.., the anchor takes anchor + nb + 1. A short
 * mini-GOP (drain, GOP boundary, forced key frame) therefore leaves a gap
 * in the POCs, which both standards allow, and every P sits at a multiple
 * of nb + 1 from its IDR. HEVC needs exactly that: the firmware picks a
 * frame's RPS set by its POC distance from the IDR (SetRpsVars, docs/81
 * Appendix B §2), and a P at a B's distance would get a B's set. Numbers
 * are unique and rise across batches; within a batch every B's is below
 * its anchor's, which is the firmware's release rule (ReorderFrames).
 */
#ifndef __AVE_GOP_H__
#define __AVE_GOP_H__

/* B frames between anchors this planner (and the firmware sets) handle. */
#define AVE_GOP_B_MAX		2
#define AVE_GOP_BATCH_MAX	(AVE_GOP_B_MAX + 1)

/* Frame types, the firmware's (AVE_FRAME_TYPE_*; ave_session.c checks). */
#define AVE_GOP_P		1
#define AVE_GOP_B		2
#define AVE_GOP_IDR		3

enum ave_gop_act {
	AVE_GOP_NONE,		/* nothing to do yet */
	AVE_GOP_HOLD,		/* hold the new source as a B */
	AVE_GOP_ANCHOR,		/* held Bs + the new source as their P */
	AVE_GOP_KEY,		/* close the held, then the new source as an IDR */
	AVE_GOP_CLOSE,		/* no new source: close the held (drain) */
};

struct ave_gop {
	u32	nb;		/* B frames per mini-GOP, 1..AVE_GOP_B_MAX */
	u32	gop;		/* IDR interval in display frames; 0 = first only */
	u32	n_held;		/* sources held as B, oldest first */
	u32	dist;		/* display distance of the next source from the IDR */
	u32	fn_anchor;	/* frameNumber of the last anchor or IDR */
	bool	started;	/* the first IDR has been planned */
};

struct ave_gop_frame {
	u32	src;		/* 0..n_held-1: held source i; n_held: the new one */
	u32	type;		/* AVE_GOP_{P,B,IDR} */
	u32	fn;		/* frameNumber */
};

struct ave_gop_batch {
	u32			n;
	struct ave_gop_frame	f[AVE_GOP_BATCH_MAX];
};

static inline void ave_gop_init(struct ave_gop *g, u32 nb, u32 gop)
{
	*g = (struct ave_gop){ .nb = nb, .gop = gop };
}

/*
 * What to do next. @has_new: a source not yet planned is queued. @force: a
 * key frame was asked for (FORCE_KEY_FRAME) for it. @last: with @has_new,
 * the new source is the last before a drain; without, the drain's last
 * source is the newest held one.
 */
static inline enum ave_gop_act ave_gop_decide(const struct ave_gop *g,
					      bool has_new, bool force, bool last)
{
	if (!has_new)
		return last && g->n_held ? AVE_GOP_CLOSE : AVE_GOP_NONE;
	if (!g->started || force || (g->gop && g->dist >= g->gop))
		return AVE_GOP_KEY;
	/* the frame before a GOP-boundary IDR is an anchor, never a B */
	if (last || g->n_held >= g->nb || (g->gop && g->dist + 1 >= g->gop))
		return AVE_GOP_ANCHOR;
	return AVE_GOP_HOLD;
}

/* CAPTURE buffers @a fills: one per coded frame. */
static inline u32 ave_gop_need(const struct ave_gop *g, enum ave_gop_act a)
{
	switch (a) {
	case AVE_GOP_ANCHOR:
	case AVE_GOP_KEY:
		return g->n_held + 1;
	case AVE_GOP_CLOSE:
		return g->n_held;
	default:
		return 0;
	}
}

/* Sources @a consumes from the head of the ready queue (held + new). */
static inline u32 ave_gop_srcs(const struct ave_gop *g, enum ave_gop_act a)
{
	return a == AVE_GOP_HOLD ? 0 : ave_gop_need(g, a);
}

/* The held sources as {B.., P} (@with_anchor: and the new source as P). */
static inline void ave_gop_minigop(struct ave_gop *g, struct ave_gop_batch *b,
				   bool with_anchor)
{
	u32 k = g->n_held, nbs = with_anchor ? k : k - 1, i;

	b->n = nbs + 1;
	for (i = 0; i < nbs; i++)
		b->f[i] = (struct ave_gop_frame){ i, AVE_GOP_B,
						  g->fn_anchor + 1 + i };
	g->fn_anchor += g->nb + 1;
	b->f[nbs] = (struct ave_gop_frame){ nbs, AVE_GOP_P, g->fn_anchor };
	g->n_held = 0;
}

/*
 * Apply @a (from ave_gop_decide) and fill @b with the batches to send, in
 * order: at most two (KEY with held sources). Returns how many.
 */
static inline u32 ave_gop_commit(struct ave_gop *g, enum ave_gop_act a,
				 struct ave_gop_batch b[2])
{
	u32 nbat = 0, k = g->n_held, fn;

	switch (a) {
	case AVE_GOP_HOLD:
		g->n_held++;
		g->dist++;
		return 0;
	case AVE_GOP_ANCHOR:
		ave_gop_minigop(g, &b[0], true);
		g->dist++;
		return 1;
	case AVE_GOP_CLOSE:
		if (!k)
			return 0;
		ave_gop_minigop(g, &b[0], false);
		return 1;
	case AVE_GOP_KEY:
		if (k)
			ave_gop_minigop(g, &b[nbat++], false);
		/* above every number used so far; POC restarts here */
		fn = g->started ? g->fn_anchor + 1 : 0;
		b[nbat].n = 1;
		b[nbat].f[0] = (struct ave_gop_frame){ k, AVE_GOP_IDR, fn };
		g->fn_anchor = fn;
		g->started = true;
		g->n_held = 0;
		g->dist = 1;
		return nbat + 1;
	default:
		return 0;
	}
}

#endif /* __AVE_GOP_H__ */
