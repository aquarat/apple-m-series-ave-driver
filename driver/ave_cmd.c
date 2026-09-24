// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE - pure command builders for every supported firmware ABI.
 *
 * UNTESTED ON HARDWARE. See ave_cmd.h. Every offset is read through
 * struct ave_cmd_abi; there are no wire offsets in this file.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define AVE_CMD_ABI_DEFINE_TABLES
#include "ave_cmd.h"

const struct ave_cmd_abi *ave_cmd_abi_get(enum ave_fw_abi abi)
{
	switch (abi) {
	case AVE_ABI_MACOS_13_5:
		return &ave_cmd_abi_13_5;
	case AVE_ABI_MACOS_26_6:
		return &ave_cmd_abi_26_6;
	default:
		return NULL;
	}
}

size_t ave_cmd_size(const struct ave_cmd_abi *abi, enum ave_op op)
{
	if (!abi || op >= AVE_OP_COUNT || !abi->cmd[op].id)
		return 0;
	return abi->cmd[op].size;
}

/*
 * Bounds-checked field writer. A table offset that runs past the command is a
 * table bug; it poisons the writer instead of scribbling past the buffer.
 */
struct ave_wr {
	u8	*buf;
	size_t	size;
	int	err;
};

static bool ave_wr_ok(struct ave_wr *w, u32 off, u32 width)
{
	if (w->err || off > w->size || width > w->size - off) {
		w->err = -EINVAL;
		return false;
	}
	return true;
}

static void wr8(struct ave_wr *w, u32 off, u8 v)
{
	if (ave_wr_ok(w, off, 1))
		w->buf[off] = v;
}

static void wr16(struct ave_wr *w, u32 off, u16 v)
{
	if (ave_wr_ok(w, off, 2))
		put_unaligned_le16(v, w->buf + off);
}

static void wr32(struct ave_wr *w, u32 off, u32 v)
{
	if (ave_wr_ok(w, off, 4))
		put_unaligned_le32(v, w->buf + off);
}

static void wr64(struct ave_wr *w, u32 off, u64 v)
{
	if (ave_wr_ok(w, off, 8))
		put_unaligned_le64(v, w->buf + off);
}

/* Write only if the field exists in this ABI. */
static void wr32_opt(struct ave_wr *w, u32 off, u32 v)
{
	if (off != AVE_OFF_NONE)
		wr32(w, off, v);
}

/* IEEE-754 binary64 bit pattern of an unsigned integer, without FP. */
static u64 ave_u32_to_f64_bits(u32 v)
{
	u64 mant;
	int e = 31;

	if (!v)
		return 0;
	while (!(v & (1u << e)))
		e--;
	mant = ((u64)v << (52 - e)) & ((1ull << 52) - 1);
	return ((u64)(1023 + e) << 52) | mant;
}

/* Zero-fill and header. Returns the command size or -EINVAL. */
static int ave_cmd_begin(const struct ave_cmd_abi *abi, enum ave_op op,
			 u8 *buf, size_t len, const struct ave_cmd_ctx *ctx,
			 u32 slot, struct ave_wr *w)
{
	const struct ave_cmd_desc *d;
	const struct ave_hdr_layout *h;
	bool global, hevc;

	if (!abi || !buf || !ctx || op >= AVE_OP_COUNT)
		return -EINVAL;
	d = &abi->cmd[op];
	h = &abi->hdr;
	if (!d->id || d->size < AVE_CMD_HDR_SIZE || len < d->size)
		return -EINVAL;

	if (d->slot != AVE_SLOT_CALLER)
		slot = d->slot;
	else if (slot >= h->max_slot)
		return -EINVAL;

	global = d->slot == AVE_SLOT_GLOBAL;
	/*
	 * The HEVC commands always carry the HEVC codec; the rest of an HEVC
	 * session's client commands (Open, Stop, Close, ...) carry it when
	 * the caller says the session is HEVC (docs/77 §1.2). An AVC command
	 * in an HEVC session is a caller bug.
	 */
	if (ctx->hevc && (op == AVE_OP_START_AVC || op == AVE_OP_PROCESS_AVC))
		return -EINVAL;
	hevc = op == AVE_OP_START_HEVC || op == AVE_OP_PROCESS_HEVC || ctx->hevc;
	if (!global && h->client_id_bytes == 4 && ctx->client_id > 0xffffffffull)
		return -EINVAL;

	memset(buf, 0, d->size);
	w->buf = buf;
	w->size = d->size;
	w->err = 0;

	wr16(w, 0, d->id);
	wr64(w, h->count, ctx->count);
	if (!global) {
		/* Config and Halt carry no client, type or codec. */
		if (h->client_id_bytes == 8)
			wr64(w, h->client_id, ctx->client_id);
		else
			wr32(w, h->client_id, (u32)ctx->client_id);
		wr32_opt(w, h->work_type, AVE_WORK_ENC);
		wr32(w, h->codec, hevc ? h->codec_hevc : h->codec_avc);
	}
	wr32(w, h->slot, slot);
	wr32(w, h->priority, d->priority);
	if (ave_wr_ok(w, h->timeout, sizeof(ctx->timeout)))
		memcpy(buf + h->timeout, ctx->timeout, sizeof(ctx->timeout));

	return w->err ? w->err : (int)d->size;
}

static int ave_cmd_end(struct ave_wr *w)
{
	if (w->err) {
		memset(w->buf, 0, w->size);
		return w->err;
	}
	return (int)w->size;
}

int ave_cmd_build_hdr(const struct ave_cmd_abi *abi, enum ave_op op,
		      u8 *buf, size_t len, const struct ave_cmd_ctx *ctx,
		      u32 slot)
{
	struct ave_wr w;
	int ret = ave_cmd_begin(abi, op, buf, len, ctx, slot, &w);

	return ret < 0 ? ret : ave_cmd_end(&w);
}

int ave_cmd_build_simple(const struct ave_cmd_abi *abi, enum ave_op op,
			 u8 *buf, size_t len, const struct ave_cmd_ctx *ctx)
{
	switch (op) {
	case AVE_OP_HALT:
	case AVE_OP_OPEN:
	case AVE_OP_CLOSE:	/* 13.5 +0x40 u8 (client[0xE0C95]) left 0 */
	case AVE_OP_STOP:
	case AVE_OP_COMPLETE:
	case AVE_OP_FLUSH:
		return ave_cmd_build_hdr(abi, op, buf, len, ctx, 0);
	default:
		return -EINVAL;
	}
}

int ave_cmd_build_config(const struct ave_cmd_abi *abi, u8 *buf, size_t len,
			 const struct ave_cmd_ctx *ctx,
			 const struct ave_config_params *p)
{
	const struct ave_config_layout *c;
	struct ave_wr w;
	int ret;

	if (!abi || !p)
		return -EINVAL;
	c = &abi->config;
	if (!p->shmem_addr || p->shmem_size < c->shmem_min)
		return -EINVAL;
	if (c->dsid_bytes == 1 && p->dsid > 0xff)
		return -EINVAL;

	ret = ave_cmd_begin(abi, AVE_OP_CONFIG, buf, len, ctx, 0, &w);
	if (ret < 0)
		return ret;

	wr8(&w, c->skip_mcpu, p->skip_mcpu);
	wr8(&w, c->create_mcpu, p->create_mcpu);
	if (c->reg_dart_addr != AVE_OFF_NONE)
		wr64(&w, c->reg_dart_addr, p->reg_dart_addr);
	wr32(&w, c->doorbell_cadence0, p->doorbell_cadence[0]);
	wr32(&w, c->doorbell_cadence1, p->doorbell_cadence[1]);
	if (c->dsid_bytes == 1) {
		wr8(&w, c->dsid, p->dsid);
		if (c->dsid2 != AVE_OFF_NONE)
			wr8(&w, c->dsid2, p->dsid);
	} else {
		wr32(&w, c->dsid, p->dsid);
	}
	wr64(&w, c->shmem_addr, p->shmem_addr);
	wr32(&w, c->shmem_size, p->shmem_size);

	return ave_cmd_end(&w);
}

/* level_idc -> _E_AVC_Level (26.6.2 AVC_FindLevelIdc table, docs/37 §3). */
static int ave_avc_level_enum(u8 level_idc)
{
	static const u8 idc[] = {
		10, 1, 11, 12, 13, 20, 21, 22, 30, 31,
		32, 40, 41, 42, 50, 51, 52, 60, 61, 62,
	};
	int i;

	for (i = 0; i < (int)sizeof(idc); i++)
		if (i != 1 && idc[i] == level_idc)	/* 1b (eLevel 2) not offered */
			return i + 1;
	return -EINVAL;
}

/* profile_idc -> _E_AVC_Profile (26.6.2 AVC_FindProfileIdc, docs/37 §3). */
static int ave_avc_profile_enum(u8 profile_idc)
{
	switch (profile_idc) {
	case 66:	return 2;	/* Baseline */
	case 77:	return 4;	/* Main */
	case 100:	return 6;	/* High */
	default:	return -EINVAL;
	}
}

#define AVE_AVC_MIN_W	192	/* 13.5 kext 0xfffffe0008ea3d9c; same numbers */
#define AVE_AVC_MIN_H	96	/* table-driven on 26.6.2 (docs/47 §5)       */
#define AVE_AVC_MAX_WH	4096

/*
 * The half of AVC_INIT and HEVC_INIT that is the same on the wire: the
 * AVE_VIDEO_PARAMS scalars, AVEFWRCSettings and the buffer tables at
 * .start_avc's offsets (docs/77 §2.2, §2.7). Checked and written by the same
 * code for both codecs, so what AVC has proven on hardware is exactly what
 * HEVC sends. @align is the address alignment the controller asserts on the
 * SrcNbr and entropy buffers: AVE_STRIDE_ALIGN (64) for AVC - the only value
 * before HEVC - and 128 for HEVC (:14062, :7447).
 *
 * Moved verbatim out of ave_cmd_build_start_avc(); the AVC-only checks
 * (profile, level, CABAC, scaling lists) stayed there.
 */
static int ave_vp_check(const struct ave_start_avc_layout *l,
			const struct ave_avc_session *s, u32 align)
{
	u32 i;

	/* ---- parameter validation, before touching the buffer ---- */
	if (s->width < AVE_AVC_MIN_W || s->width > AVE_AVC_MAX_WH ||
	    s->height < AVE_AVC_MIN_H || s->height > AVE_AVC_MAX_WH ||
	    (s->width & 1) || (s->height & 1))
		return -EINVAL;
	if (!s->frame_rate || !s->key_interval)
		return -EINVAL;
	if (s->qp_i > 51 || s->qp_p > 51 || s->qp_b > 51)
		return -EINVAL;
	if (s->qp_min > 51 || s->qp_max > 51 ||
	    (s->qp_max && s->qp_min > s->qp_max))
		return -EINVAL;
	/* The firmware's controller needs a target and an ABI that has it. */
	if (s->rc_enable && (!s->bitrate || l->rc_mode_on == AVE_OFF_NONE))
		return -EINVAL;
	if (!s->fw_client_addr || !s->fw_client_size ||
	    !s->fw_client_mem_addr || !s->fw_client_mem_size)
		return -EINVAL;
	/* Zero here is what tripped the firmware assert; refuse it here. */
	if (l->param_sets_addr != AVE_OFF_NONE &&
	    (!s->param_sets_addr || !s->param_sets_size))
		return -EINVAL;
	if (!s->recon || !s->n_recon || s->n_recon > l->recon_max)
		return -EINVAL;
	if (!s->coded || !s->coded_hdr || !s->n_coded ||
	    s->n_coded > l->coded_max)
		return -EINVAL;
	for (i = 0; i < s->n_recon; i++)
		if (!s->recon[i].addr || (s->recon[i].addr & 127))
			return -EINVAL;
	/*
	 * NEED_LSB_PLANES: the layout must have it, and every recon entry then
	 * needs a 128-aligned LSB plane - the firmware asserts & 127 on all
	 * four recon planes (fw 0x55310 / 0x5541c / 0x58068 / 0x54f94).
	 */
	if (s->need_lsb_planes) {
		if (l->need_lsb_planes == AVE_OFF_NONE ||
		    l->recon_lsb_addr == AVE_OFF_NONE)
			return -EINVAL;
		for (i = 0; i < s->n_recon; i++)
			if (!s->recon[i].lsb_addr || (s->recon[i].lsb_addr & 127))
				return -EINVAL;
	}
	/*
	 * LowResResult: all or nothing, non-zero and 64-byte aligned, since
	 * setPipe asserts both for every entry the reference loop reaches
	 * (CAVCController_H13C.cpp:6184/6185).
	 */
	if (s->n_low_res_result) {
		if (l->low_res_result_set == AVE_OFF_NONE ||
		    !l->low_res_result_max ||
		    s->n_low_res_result > l->low_res_result_max ||
		    s->n_low_res_result > AVE_LOW_RES_RESULT_MAX)
			return -EINVAL;
		for (i = 0; i < s->n_low_res_result; i++)
			if (!s->low_res_result[i] ||
			    (s->low_res_result[i] & (AVE_STRIDE_ALIGN - 1)))
				return -EINVAL;
	}
	/* A source-path sweep value is meaningless on an ABI without the field. */
	if ((s->src_mode && l->src_mode == AVE_OFF_NONE) ||
	    (s->src_cfg_byte && l->src_cfg_byte == AVE_OFF_NONE) ||
	    (s->src_go_bit3 && l->src_go_bit3 == AVE_OFF_NONE) ||
	    (s->src_go_bits && l->src_go_bits == AVE_OFF_NONE) ||
	    (s->dbg_bits && l->dbg_bits == AVE_OFF_NONE) ||
	    (s->ipcm_islice && l->ipcm_islice == AVE_OFF_NONE) ||
	    (s->lambda_block && l->lambda_scales == AVE_OFF_NONE) ||
	    (s->skip_mode && l->skip_mode == AVE_OFF_NONE))
		return -EINVAL;
	if (s->n_entropy) {
		u32 j, cols = s->n_entropy_cols ? s->n_entropy_cols : 1;
		u32 cols_max = l->entropy_cols_max ? l->entropy_cols_max : 1;

		if (l->entropy_set == AVE_OFF_NONE || !l->entropy_max ||
		    s->n_entropy > l->entropy_max ||
		    s->n_entropy > AVE_ENTROPY_MAX ||
		    cols > AVE_ENTROPY_COLS || cols > cols_max)
			return -EINVAL;
		for (i = 0; i < s->n_entropy; i++)
			for (j = 0; j < cols; j++)
				if (!s->entropy[i][j] ||
				    (s->entropy[i][j] & (align - 1)))
					return -EINVAL;
	}
	if (s->n_colocated) {
		if (s->n_colocated != s->n_recon || s->n_colocated > AVE_DPB_MAX ||
		    l->colocated_set == AVE_OFF_NONE ||
		    s->n_colocated > l->colocated_max)
			return -EINVAL;
		for (i = 0; i < s->n_colocated; i++)
			if (!s->colocated[i] ||
			    (s->colocated[i] & (AVE_STRIDE_ALIGN - 1)))
				return -EINVAL;
	}
	if (s->n_low_res_ref) {
		/*
		 * One LowResRef per DPB slot, in slot order, or none at all.
		 * Publishing them for a layout that has no such table, or an
		 * address the setLRME alignment assert (fw 0x523dc, line 5783)
		 * would reject, is a silent wrong answer - refuse it here.
		 */
		if (s->n_low_res_ref != s->n_recon ||
		    s->n_low_res_ref > AVE_DPB_MAX ||
		    l->low_res_ref_set == AVE_OFF_NONE ||
		    s->n_low_res_ref > l->low_res_ref_max)
			return -EINVAL;
		for (i = 0; i < s->n_low_res_ref; i++)
			if (!s->low_res_ref[i] ||
			    (s->low_res_ref[i] & (AVE_STRIDE_ALIGN - 1)))
				return -EINVAL;
	}
	for (i = 0; i < s->n_coded; i++)
		if (!s->coded[i].addr || !s->coded[i].size ||
		    !s->coded_hdr[i].addr ||
		    s->coded_hdr[i].size < l->coded_hdr_bytes)
			return -EINVAL;
	if (s->n_src_nbr) {
		u32 g;

		if (!l->src_nbr_max || s->n_src_nbr > l->src_nbr_max)
			return -EINVAL;
		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++) {
			if (l->src_nbr_set[g] == AVE_OFF_NONE)
				continue;
			for (i = 0; i < s->n_src_nbr; i++)
				if (!s->src_nbr[g][i] ||
				    (s->src_nbr[g][i] & (align - 1)))
					return -EINVAL;
		}
	}
	return 0;
}

/*
 * Write the shared half. @sve_num is where this command keeps sSVEMap.iNum:
 * AVC_INIT's tail (0x10DE8) lies inside HEVC_INIT's VPS block, so the
 * caller names it. Moved verbatim out of ave_cmd_build_start_avc(), less the
 * profile-dependent mode_8x8 write, which stayed there.
 */
static void ave_vp_fill(struct ave_wr *w, const struct ave_start_avc_layout *l,
			const struct ave_avc_session *s, u32 sve_num)
{
	u32 cw = ave_mb_align(s->width);
	u32 ch = ave_mb_align(s->height);
	u32 i;

	/* ---- per-client firmware buffers ---- */
	wr64(w, l->fw_client_addr, s->fw_client_addr);
	wr32(w, l->fw_client_size, s->fw_client_size);
	wr64(w, l->fw_client_mem_addr, s->fw_client_mem_addr);
	/* 13.5 only: 26.6.2's counterpart has not been located (docs/52). */
	/*
	 * sSVEMap.iNum: we always run a single SVE core. Writing it also keeps
	 * the firmware off iFwClientMemAddr, whose carve is behind iNum > 1
	 * (fw 0x5cb50). docs/54.
	 */
	wr32_opt(w, sve_num, 1);
	if (l->param_sets_addr != AVE_OFF_NONE) {
		wr64(w, l->param_sets_addr, s->param_sets_addr);
		wr32_opt(w, l->param_sets_size, s->param_sets_size);
	}
	wr32(w, l->fw_client_mem_size, s->fw_client_mem_size);

	/* ---- geometry: MB-aligned, display size via SPS cropping (docs/38) */
	wr32(w, l->width, cw);
	wr32(w, l->height, ch);

	/* ---- fixed-QP rate control ---- */
	wr32(w, l->frame_rate, s->frame_rate);
	wr32(w, l->bitrate, s->bitrate);
	if (s->rc_enable) {
		wr32(w, l->rc_mode, l->rc_mode_on);
		/*
		 * bitrate_sel picks which bitrate field the controller reads;
		 * 0 keeps the plain ui32BitRate above. The DRL block's layout
		 * is unknown, so it stays disabled.
		 */
		wr32_opt(w, l->bitrate_sel, 0);
		wr32_opt(w, l->frame_rate_div, s->frame_rate_div ?
						s->frame_rate_div : 1);
	} else {
		wr32(w, l->rc_mode, l->rc_mode_fixed_qp);
		if (l->rc_feature != AVE_OFF_NONE)
			wr64(w, l->rc_feature, l->rc_feature_fixed_qp);
	}
	wr32(w, l->qp_i, s->qp_i);
	wr32(w, l->qp_p, s->qp_p);
	wr32(w, l->qp_b, s->qp_b);
	/*
	 * Only under rate control. Writing these unconditionally changed the
	 * default fixed-QP image (0xFF88 went from 0 to session_qp_min) while
	 * two comments still claimed the default was byte-identical to what
	 * F17 sent - which would quietly confuse any bisect against the older
	 * runs.
	 */
	if (s->rc_enable) {
		wr32(w, l->qp_min, s->qp_min);
		wr32(w, l->qp_max, s->qp_max ? s->qp_max : 51);
	} else {
		wr32(w, l->qp_min, 0);
		wr32(w, l->qp_max, 51);
	}
	wr32(w, l->key_interval, s->key_interval);
	wr32_opt(w, l->key_interval_strict, s->key_interval);
	wr32(w, l->slice_num, 1);
	/*
	 * iNumViews: Apple's kext refuses to send the command with these zero
	 * (0 < iNumViews <= 2, kext 0xec9078), and we have been sending zero.
	 * The firmware only branches on == 2, so this closes a validator we
	 * were failing rather than changing what the hardware does. docs/62 §6.
	 */
	for (i = 0; i < ARRAY_SIZE(l->num_views); i++)
		wr32_opt(w, l->num_views[i], 1);
	/*
	 * The source-read scalars. Both default to zero, which is exactly what
	 * every run up to F17 sent; a non-zero value here is an experiment.
	 */
	if (s->src_mode)
		wr16(w, l->src_mode, s->src_mode);
	if (s->src_cfg_byte)
		wr8(w, l->src_cfg_byte, s->src_cfg_byte);
	if (s->src_go_bit3)
		wr8(w, l->src_go_bit3, s->src_go_bit3);
	if (s->src_go_bits)
		wr8(w, l->src_go_bits, s->src_go_bits);
	if (s->dbg_bits)
		wr32(w, l->dbg_bits, s->dbg_bits);
	if (s->ipcm_islice)
		wr8(w, l->ipcm_islice, s->ipcm_islice);
	if (s->skip_mode)
		wr16(w, l->skip_mode, s->skip_mode);
	if (s->lambda_block) {
		/*
		 * max(1, round(2^((QP - 12) / 6))) for QP 0..51: the sqrt-lambda
		 * curve, and exactly macOS's table (docs/72 §5.2). macOS's HEVC
		 * tables are byte-identical (docs/77 §5).
		 */
		static const u8 lam[52] = {
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
			2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9,
			10, 11, 13, 14, 16, 18, 20, 23, 25, 29, 32, 36, 40, 45, 51, 57,
			64, 72, 81, 91,
		};
		u32 q, k, j, idx = 0;

		for (k = 0; k < 5; k++)
			wr32(w, l->lambda_scales + 4 * k, 0x400);
		for (q = 0; q < 52; q++) {
			if (q && lam[q] != lam[q - 1])
				idx++;
			wr32(w, l->lambda_qp_tab + 4 * q, lam[q]);
			/* index of lam[q] among the distinct values */
			wr32(w, l->lambda_idx_tab + 4 * q, idx);
			if (!q || lam[q] != lam[q - 1]) {
				/* record idx: { lambda, 8 x 16 * lambda } */
				wr32(w, l->lambda_rec_tab + 36 * idx, lam[q]);
				for (j = 1; j < 9; j++)
					wr32(w, l->lambda_rec_tab + 36 * idx + 4 * j,
					     16 * lam[q]);
			}
		}
	}

	/* ---- buffer tables ---- */
	for (i = 0; i < s->n_recon; i++) {
		u32 e = l->recon_set + i * l->recon_stride;

		wr64(w, e + l->recon_addr, s->recon[i].addr);
		if (l->recon_size != AVE_OFF_NONE)
			wr32(w, e + l->recon_size, s->recon[i].luma_size);
		/* 26.6.2 uncompressed entry is {base, luma, base, 0}, docs/21 §3.2 */
		if (l->recon_meta_addr != AVE_OFF_NONE)
			wr64(w, e + l->recon_meta_addr, s->recon[i].addr);
		if (s->need_lsb_planes)
			wr64(w, e + l->recon_lsb_addr, s->recon[i].lsb_addr);
	}
	if (s->need_lsb_planes)
		wr8(w, l->need_lsb_planes, 1);
	for (i = 0; i < s->n_low_res_ref; i++)
		wr64(w, l->low_res_ref_set + i * l->low_res_ref_stride,
		     s->low_res_ref[i]);
	/* The low-res search's output surfaces; read from the first P frame. */
	for (i = 0; i < s->n_low_res_result; i++)
		wr64(w, l->low_res_result_set + i * l->low_res_result_stride,
		     s->low_res_result[i]);
	for (i = 0; i < s->n_colocated; i++)
		wr64(w, l->colocated_set + i * l->colocated_stride,
		     s->colocated[i]);
	/* The SEB write buffers the pipe's drain channels are programmed from. */
	for (i = 0; i < s->n_entropy; i++) {
		u32 j, cols = s->n_entropy_cols ? s->n_entropy_cols : 1;

		for (j = 0; j < cols; j++) {
			wr64(w, l->entropy_set + i * l->entropy_stride_i +
			     j * l->entropy_stride_j, s->entropy[i][j]);
			/* Matching size, at an inferred offset (see ave_abi.h). */
			if (s->entropy_size && l->entropy_size_set != AVE_OFF_NONE)
				wr32(w, l->entropy_size_set +
				     i * l->entropy_size_stride_i +
				     j * l->entropy_size_stride_j,
				     s->entropy_size);
		}
	}
	for (i = 0; i < s->n_coded; i++) {
		wr64(w, l->coded_addr + i * l->coded_addr_stride, s->coded[i].addr);
		wr32(w, l->coded_size + i * l->coded_size_stride, s->coded[i].size);
		wr64(w, l->coded_hdr_addr + i * l->coded_hdr_addr_stride,
		     s->coded_hdr[i].addr);
		wr32(w, l->coded_hdr_size + i * l->coded_hdr_size_stride,
		     s->coded_hdr[i].size);
	}
	for (i = 0; i < s->n_src_nbr; i++) {
		u32 g;

		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
			if (l->src_nbr_set[g] != AVE_OFF_NONE)
				wr64(w, l->src_nbr_set[g] + i * 8,
				     s->src_nbr[g][i]);
	}
}

int ave_cmd_build_start_avc(const struct ave_cmd_abi *abi, u8 *buf, size_t len,
			    const struct ave_cmd_ctx *ctx,
			    const struct ave_avc_session *s)
{
	const struct ave_start_avc_layout *l;
	const struct ave_sps_layout *sps;
	const struct ave_pps_layout *pps;
	u32 cw, ch, dw, dh;
	int prof, lvl, ret;
	struct ave_wr w;

	if (!abi || !s)
		return -EINVAL;
	l = &abi->start_avc;
	sps = &abi->sps;
	pps = &abi->pps;

	/* ---- parameter validation, before touching the buffer ---- */
	ret = ave_vp_check(l, s, AVE_STRIDE_ALIGN);
	if (ret)
		return ret;
	prof = ave_avc_profile_enum(s->profile_idc);
	lvl = ave_avc_level_enum(s->level_idc);
	if (prof < 0 || lvl < 0 || (s->profile_idc == 66 && s->cabac))
		return -EINVAL;
	if (s->scaling_flat && abi->sps.scaling_4x4 == AVE_OFF_NONE)
		return -EINVAL;

	ret = ave_cmd_begin(abi, AVE_OP_START_AVC, buf, len, ctx, 0, &w);
	if (ret < 0)
		return ret;

	cw = ave_mb_align(s->width);
	ch = ave_mb_align(s->height);

	ave_vp_fill(&w, l, s, l->sve_num);
	/* High: the 8x8 transform, matching PPS transform_8x8_mode_flag. */
	if (s->profile_idc >= 100)
		wr32_opt(&w, l->mode_8x8, 2);

	/* ---- SPS ---- */
	wr32(&w, sps->profile, sps->enum_profile_level ? (u32)prof : s->profile_idc);
	wr32(&w, sps->level, sps->enum_profile_level ? (u32)lvl : s->level_idc);
	wr32(&w, sps->seq_parameter_set_id, 0);		/* must be 0 (docs/37 §5.5) */
	wr32(&w, sps->chroma_format_idc, 1);		/* 4:2:0 */
	wr32(&w, sps->bit_depth_luma_minus8, 0);	/* 8-bit */
	wr32(&w, sps->bit_depth_chroma_minus8, 0);
	wr32(&w, sps->log2_max_frame_num_minus4, 0);
	wr32(&w, sps->pic_order_cnt_type, 2);		/* 1 unusable (docs/37 §5.6) */
	wr32(&w, sps->max_num_ref_frames, 1);
	wr8(&w, sps->gaps_in_frame_num_allowed, 0);
	wr32(&w, sps->pic_width_in_mbs_minus1, cw / AVE_MB_SIZE - 1);
	wr32(&w, sps->pic_height_in_map_units_minus1, ch / AVE_MB_SIZE - 1);
	wr32(&w, sps->frame_mbs_only_flag, 1);
	wr32(&w, sps->direct_8x8_inference_flag, 1);
	wr8(&w, sps->vui_parameters_present_flag, 0);	/* 13.5 VUI has no timing */
	/* CropUnitX = CropUnitY = 2 for 4:2:0 progressive */
	dw = s->crop_width ? s->crop_width : s->width;
	dh = s->crop_height ? s->crop_height : s->height;
	wr8(&w, sps->frame_cropping_flag, cw != dw || ch != dh);
	wr32(&w, sps->crop_left, 0);
	if (s->scaling_flat) {
		/*
		 * macOS fills every list with 16 when no matrix is in use
		 * (AVE_PrepareSequenceHeader, docs/74). The present flag stays
		 * 0, so the coded SPS does not change.
		 */
		u32 k;

		for (k = 0; k < 6 * 16; k++)
			wr16(&w, sps->scaling_4x4 + 2 * k, s->scaling_flat);
		for (k = 0; k < 6 * 64; k++)
			wr16(&w, sps->scaling_8x8 + 2 * k, s->scaling_flat);
	}
	wr32(&w, sps->crop_right, (cw - dw) / 2);
	wr32(&w, sps->crop_top, 0);
	wr32(&w, sps->crop_bottom, (ch - dh) / 2);
	wr8(&w, sps->fw_creates_header, 1);		/* == PPS's (docs/37 §5.1) */
	wr32(&w, sps->header_len, 0);			/* 26.6.2 checks == 0 */

	/* ---- PPS ---- */
	wr32(&w, pps->pic_parameter_set_id, 0);
	wr32(&w, pps->seq_parameter_set_id, 0);
	wr32(&w, pps->entropy_coding_mode_flag, s->cabac);
	wr32(&w, pps->num_slice_groups_minus1, 0);	/* must be 0 (docs/37 §5.7) */
	wr32(&w, pps->pic_init_qp_minus26, 0);
	wr8(&w, pps->deblocking_filter_control, 1);
	wr8(&w, pps->constrained_intra_pred, 0);
	wr8(&w, pps->transform_8x8_mode, s->profile_idc >= 100);
	wr8(&w, pps->fw_creates_header, 1);

	return ave_cmd_end(&w);
}

/* ------------------------------------------------------------------------ */
/* HEVC (docs/77)                                                           */
/* ------------------------------------------------------------------------ */

#define AVE_HEVC_PROFILE_MAIN	1	/* general_profile_idc */
/* Short-term sets the builder writes at most; the IPPP selector needs 4. */
#define AVE_HEVC_ST_RPS_MAX	4

/* general_level_idc values of Table A.8 (30 x level). */
static bool ave_hevc_level_ok(u8 idc)
{
	static const u8 ok[] = {
		30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186,
	};
	u32 i;

	for (i = 0; i < sizeof(ok); i++)
		if (ok[i] == idc)
			return true;
	return false;
}

/* profile_tier_level() for Main, general_tier 0 (docs/77 §2.3, §5). */
static void ave_hevc_ptl(struct ave_wr *w, const struct ave_hevc_ps_layout *p,
			 u32 ptl, u8 level_idc)
{
	wr32(w, ptl + p->ptl_profile_idc, AVE_HEVC_PROFILE_MAIN);
	/* Main is also Main 10 compatible: compat[1] and compat[2]. */
	wr8(w, ptl + p->ptl_compat + 1, 1);
	wr8(w, ptl + p->ptl_compat + 2, 1);
	wr8(w, ptl + p->ptl_progressive, 1);
	wr8(w, ptl + p->ptl_non_packed, 1);
	wr8(w, ptl + p->ptl_frame_only, 1);
	wr32(w, ptl + p->ptl_level_idc, level_idc);
}

/*
 * Re-read what was written at the fields whose wrong value the firmware
 * answers with an assert that spins (docs/77 §7), so a layout whose writes
 * overlap - a later block clobbering an earlier field - is refused on the
 * host instead of hanging the firmware.
 */
static bool ave_hevc_verify(const u8 *b, const struct ave_cmd_abi *abi,
			    const struct ave_hevc_session *h)
{
	const struct ave_start_avc_layout *l = &abi->start_avc;
	const struct ave_start_hevc_layout *hl = &abi->start_hevc;
	const struct ave_hevc_ps_layout *p = &abi->hps;
	u32 vps = hl->vps_block, sps = hl->sps_block[0], pps = hl->pps_block[0];
	u32 fmt, i;

	/* Hevc_headers.cpp:756: nesting must be 1 with no sub-layers. */
	if (!get_unaligned_le32(b + sps + p->sps_max_sub_layers_m1) &&
	    b[sps + p->sps_temporal_nesting] != 1)
		return false;
	if (!get_unaligned_le32(b + vps + p->vps_max_sub_layers_m1) &&
	    b[vps + p->vps_temporal_nesting] != 1)
		return false;
	/* Hevc_headers.cpp:945 */
	if (get_unaligned_le32(b + pps + p->extra_sh_bits))
		return false;
	/* The PPS writer ADDS to this (0x1fa50). */
	if (get_unaligned_le32(b + pps + p->pps_header_len) ||
	    get_unaligned_le32(b + sps + p->sps_header_len))
		return false;
	/* 0xEE0005 on a mismatch (0x84f8c); we want both set. */
	if (b[sps + p->sps_fw_creates_header] != 1 ||
	    b[pps + p->pps_fw_creates_header] != 1)
		return false;
	/* CHEVCController_H13C.cpp:5118 (0x83658-0x83680). */
	fmt = (get_unaligned_le32(b + hl->input_format_word) >> 2) & 7;
	if (fmt != 4 &&
	    fmt < get_unaligned_le32(b + sps + p->chroma_format_idc))
		return false;
	/* iNumViews 0 silently builds no recon, SPS/PPS or RC (docs/77 §2.2). */
	if (!get_unaligned_le32(b + l->num_views[0]))
		return false;
	if (get_unaligned_le32(b + hl->sve_num) != 1)
		return false;
	/* ui32RCFlag 0 skips DPB creation (0x852e0). */
	if (!get_unaligned_le32(b + l->rc_mode))
		return false;
	/* setPipe :13921..:13969: every recon plane pair, incl. the LSB. */
	if (b[l->need_lsb_planes] != 1)
		return false;
	for (i = 0; i < h->vp.n_recon; i++) {
		u32 e = l->recon_set + i * l->recon_stride;
		u64 msb = get_unaligned_le64(b + e + l->recon_addr);
		u64 lsb = get_unaligned_le64(b + e + l->recon_lsb_addr);

		if (!msb || !lsb || ((msb | lsb) & 127))
			return false;
	}
	return true;
}

int ave_cmd_build_start_hevc(const struct ave_cmd_abi *abi, u8 *buf,
			     size_t len, const struct ave_cmd_ctx *ctx,
			     const struct ave_hevc_session *h)
{
	const struct ave_start_avc_layout *l;
	const struct ave_start_hevc_layout *hl;
	const struct ave_hevc_ps_layout *p;
	const struct ave_avc_session *s;
	u32 vps, sps, pps, rps, cw, ch, dw, dh, align, fmt, i;
	struct ave_wr w;
	int ret;

	if (!abi || !h)
		return -EINVAL;
	l = &abi->start_avc;
	hl = &abi->start_hevc;
	p = &abi->hps;
	s = &h->vp;

	/* An ABI whose HEVC layout was never read (26.6.2): refuse. */
	if (hl->vps_block == AVE_OFF_NONE || !hl->shares_avc_vp ||
	    !hl->sps_block[0] || !hl->pps_block[0] || !hl->rps_block ||
	    !hl->sve_num || !hl->input_format_word || !hl->pps_count ||
	    !hl->sao_enb_config || !hl->sao_eo_bo || !hl->input_bitdepth ||
	    !hl->max_num_ref_frames || !hl->slice_map_height ||
	    !hl->entropy_rows)
		return -EINVAL;
	/* The shared fields HEVC cannot do without (docs/77 §2.2, §7). */
	if (l->num_views[0] == AVE_OFF_NONE || l->need_lsb_planes == AVE_OFF_NONE ||
	    l->entropy_size_set == AVE_OFF_NONE || l->param_sets_addr == AVE_OFF_NONE)
		return -EINVAL;
	align = hl->buf_align ? hl->buf_align : AVE_STRIDE_ALIGN;

	ret = ave_vp_check(l, s, align);
	if (ret)
		return ret;
	/* AVC-only members: meaningless here, so a set one is a caller bug. */
	if (s->profile_idc || s->level_idc || s->cabac || s->scaling_flat)
		return -EINVAL;
	if (!ave_hevc_level_ok(h->level_idc))
		return -EINVAL;
	/* :5118 - bits [4:2] 0 fails it for 4:2:0; >4 means nothing known. */
	fmt = (h->input_format_word >> 2) & 7;
	if (!fmt || fmt > 4)
		return -EINVAL;
	/* setPipe asserts all four recon planes (:13921..:13969). */
	if (!s->need_lsb_planes)
		return -EINVAL;
	/* QP modulation is a rate-control setting (macOS clears it for FIXQP). */
	if (h->qp_mod && (!s->rc_enable || hl->qp_mod == AVE_OFF_NONE ||
			  !hl->qp_mod))
		return -EINVAL;
	if (h->log2_max_poc_lsb_minus4 > 12 || h->n_st_rps > AVE_HEVC_ST_RPS_MAX ||
	    (h->n_st_rps && !h->max_num_ref_frames) ||
	    h->max_num_ref_frames > 15 ||
	    s->n_recon < h->max_num_ref_frames + 1)
		return -EINVAL;
	/* setPipe :14199/:14200 and SetTranscodeCommon :7447/:7448. */
	if (s->n_entropy < hl->entropy_rows || !s->entropy_size)
		return -EINVAL;
	/* SetTranscode :7597/:7598: the coded buffer, 128-aligned. */
	for (i = 0; i < s->n_coded; i++)
		if (s->coded[i].addr & (align - 1))
			return -EINVAL;
	/*
	 * TranscodedData: all or none (none only for single-transcoder frames),
	 * each non-zero and 128-aligned (:7605/:7606), with a size.
	 */
	if (h->n_transcoded) {
		if (h->n_transcoded != hl->transcoded_max ||
		    h->n_transcoded > ARRAY_SIZE(h->transcoded) ||
		    !hl->transcoded_set || !hl->transcoded_size ||
		    !h->transcoded_size)
			return -EINVAL;
		for (i = 0; i < h->n_transcoded; i++)
			if (!h->transcoded[i] || (h->transcoded[i] & (align - 1)))
				return -EINVAL;
	}

	ret = ave_cmd_begin(abi, AVE_OP_START_HEVC, buf, len, ctx, 0, &w);
	if (ret < 0)
		return ret;

	cw = ave_mb_align(s->width);
	ch = ave_mb_align(s->height);

	/* ---- AVE_VIDEO_PARAMS / RC / buffer tables, as AVC_INIT ---- */
	ave_vp_fill(&w, l, s, hl->sve_num);

	/* ---- HEVC-only AVE_VIDEO_PARAMS (docs/77 §2.2, §6) ---- */
	wr32(&w, hl->input_format_word, h->input_format_word);
	wr32(&w, hl->pps_count, 1);
	/* SAO: hardware and syntax together (docs/77 §2.6). */
	wr32(&w, hl->sao_enb_config, h->sao ? 0xffff : 0);
	wr32(&w, hl->sao_eo_bo, 0xffffffff);		/* firmware default */
	wr32(&w, hl->input_bitdepth, 8);
	wr32(&w, hl->max_num_ref_frames, h->max_num_ref_frames);
	wr32(&w, hl->slice_map_height, ch);
	for (i = 0; i < h->n_transcoded; i++)
		wr64(&w, hl->transcoded_set + i * hl->transcoded_stride,
		     h->transcoded[i]);
	if (h->n_transcoded)
		wr32(&w, hl->transcoded_size, h->transcoded_size);

	/* ---- VPS (video_header_parameter_set_rbsp 0x1d294) ---- */
	vps = hl->vps_block;
	wr8(&w, vps + p->vps_base_internal, 1);
	wr8(&w, vps + p->vps_base_available, 1);
	wr8(&w, vps + p->vps_temporal_nesting, 1);
	ave_hevc_ptl(&w, p, vps + p->ptl, h->level_idc);
	wr8(&w, vps + p->vps_sublayer_info, 1);
	wr32(&w, vps + p->vps_max_dec_pic_buf_m1, h->max_num_ref_frames);
	/* num_hrd_parameters, timing, extension: all 0 (0x1d50c) */

	/* ---- SPS[0] (seq_parameter_set_rbsp 0x1e380) ---- */
	sps = hl->sps_block[0];
	wr8(&w, sps + p->sps_temporal_nesting, 1);	/* :756 */
	ave_hevc_ptl(&w, p, sps + p->ptl, h->level_idc);
	wr32(&w, sps + p->chroma_format_idc, 1);
	wr32(&w, sps + p->pic_width, cw);
	wr32(&w, sps + p->pic_height, ch);
	wr32(&w, sps + p->ctb_cols, (cw + 31) >> 5);
	wr32(&w, sps + p->ctb_rows, (ch + 31) >> 5);
	/* Conformance window in chroma units (SubWidthC = SubHeightC = 2). */
	dw = s->crop_width ? s->crop_width : s->width;
	dh = s->crop_height ? s->crop_height : s->height;
	wr8(&w, sps + p->conf_win_flag, cw != dw || ch != dh);
	wr32(&w, sps + p->conf_win_right, (cw - dw) / 2);
	wr32(&w, sps + p->conf_win_bottom, (ch - dh) / 2);
	wr32(&w, sps + p->log2_max_poc_lsb_m4, h->log2_max_poc_lsb_minus4);
	wr8(&w, sps + p->sps_sublayer_info, 1);
	wr32(&w, sps + p->sps_max_dec_pic_buf_m1, h->max_num_ref_frames);
	/* CTB 32, min CB 8, TB 4..32, depth 1/0: what the pipe does (§0 #6). */
	wr32(&w, sps + p->log2_min_cb_m3, 0);
	wr32(&w, sps + p->log2_diff_cb, 2);
	wr32(&w, sps + p->log2_min_tb_m2, 0);
	wr32(&w, sps + p->log2_diff_tb, 3);
	wr32(&w, sps + p->tb_depth_inter, 1);
	wr32(&w, sps + p->tb_depth_intra, 0);
	/* scaling_list_enabled 0 = flat, matching PICMGMT+0x6F4 = 0 (§2.5). */
	wr8(&w, sps + p->scaling_enabled, 0);
	wr8(&w, sps + p->sao, h->sao);
	wr8(&w, sps + p->sps_tmvp, h->sps_tmvp);
	wr8(&w, sps + p->sps_fw_creates_header, 1);	/* == PPS's */
	wr32(&w, sps + p->sps_header_len, 0);

	/* ---- RPS (docs/77 §2.4) ---- */
	rps = hl->rps_block;
	wr32(&w, rps + p->rps_num_st, h->n_st_rps);
	/*
	 * Every set is the same one-reference set: the firmware, not the
	 * slice RPS we send, picks the set per frame - for IPPP it takes set
	 * "frames since the IDR" while that is <= 3, else set 0 (fw
	 * 0x6c7d4 -> 0x6c974) - so sets 0..3 must all exist (docs/77 §18).
	 */
	for (i = 0; i < h->n_st_rps; i++) {
		u32 e = rps + p->rps_entry0 + i * p->rps_entry_stride;

		wr8(&w, e + p->rps_inter_pred, 0);
		wr32(&w, e + p->rps_num_neg, 1);
		wr32(&w, e + p->rps_num_pos, 0);
		wr16(&w, e + p->rps_dpoc_s0_m1, 0);	/* delta POC -1 */
		wr8(&w, e + p->rps_used_s0, 1);
		wr32(&w, e + p->rps_num_delta_pocs, 1);
		/* The derived fields the firmware's ref lists read (docs/77 §18). */
		wr32(&w, e + p->rps_d_num_neg, 1);
		wr32(&w, e + p->rps_d_num_pos, 0);
		wr8(&w, e + p->rps_d_used_s0, 1);
		wr32(&w, e + p->rps_d_delta_poc_s0, (u32)-1);	/* DeltaPocS0[0] */
	}

	/* ---- PPS[0] (pic_parameter_set_rbsp 0x1f50c) ---- */
	pps = hl->pps_block[0];
	/* ids: the firmware overwrites both (0x850c8-0x850e8) */
	wr32(&w, pps + p->extra_sh_bits, 0);		/* :945 */
	wr32(&w, pps + p->init_qp_m26, 0);
	/*
	 * cu_qp_delta goes with QP modulation (docs/77 §20). The firmware's
	 * transcoder context sets it iff bEnableQPMod || bEnableMBInputCtrl
	 * (0x856ac-0x856bc) while the live XC+0x214 takes this PPS flag
	 * (0x75b54); macOS sets and clears the two together (user space
	 * 0x6cf4c / 0x86a2c-0x86a30). h4a sent 1 here with QPMod 0 and the
	 * transcoder hung on its first frame.
	 */
	if (h->qp_mod)
		wr8(&w, hl->qp_mod, 1);
	wr8(&w, pps + p->cu_qp_delta, h->qp_mod);
	wr32(&w, pps + p->cu_qp_delta_depth, h->qp_mod ? 2 : 0);
	wr8(&w, pps + p->wpp, h->wpp);
	wr8(&w, pps + p->deblock_ctrl_present, 1);
	wr8(&w, pps + p->pps_fw_creates_header, 1);
	wr32(&w, pps + p->pps_header_len, 0);		/* accumulated: 0 in */

	if (!w.err && !ave_hevc_verify(buf, abi, h))
		w.err = -EINVAL;
	return ave_cmd_end(&w);
}

/*
 * Per-frame checks shared by AVC_ENCODE and HEVC_ENCODE: PICMGMT is one
 * struct (docs/77 §3.3). @align as for ave_vp_check(). Moved verbatim out
 * of ave_cmd_build_process_avc().
 */
static int ave_pic_check(const struct ave_cmd_abi *abi,
			 const struct ave_avc_frame *f, u32 align)
{
	const struct ave_process_avc_layout *l = &abi->process_avc;

	/*
	 * I, P and IDR. B is left out deliberately: it needs a reference list
	 * the session does not build yet, and the firmware's enum accepts it
	 * (2 and 7) without that being a reason to send it. docs/64 §1.3.
	 */
	if (f->frame_type != AVE_FRAME_TYPE_I &&
	    f->frame_type != AVE_FRAME_TYPE_P &&
	    f->frame_type != AVE_FRAME_TYPE_IDR)
		return -EINVAL;
	if (!f->in_luma_addr || (f->in_luma_addr & (AVE_STRIDE_ALIGN - 1)) ||
	    !f->in_luma_stride || (f->in_luma_stride % AVE_STRIDE_ALIGN) ||
	    !f->in_chroma_addr || (f->in_chroma_addr & (AVE_STRIDE_ALIGN - 1)) ||
	    !f->in_chroma_stride || (f->in_chroma_stride % AVE_STRIDE_ALIGN))
		return -EINVAL;
	if (l->in_luma_size != AVE_OFF_NONE &&
	    (!f->in_luma_size || !f->in_chroma_size))
		return -EINVAL;
	if (f->coded_index >= abi->start_avc.coded_max || !f->coded_addr ||
	    !f->coded_hdr_addr || !f->coded_size)
		return -EINVAL;
	if ((f->recon_luma_addr & 127) || (f->recon_chroma_addr & 127) ||
	    (f->recon_luma_lsb_addr & 127) || (f->recon_chroma_lsb_addr & 127))
		return -EINVAL;
	/*
	 * LowResSrcLumaScaled: optional (0 = leave the field zero, which makes
	 * the firmware assert at setLRME:5782 - a deliberate bisect), but if
	 * given it must be 64-aligned, and the ABI must have the field.
	 */
	if (f->low_res_src_addr &&
	    ((f->low_res_src_addr & (AVE_STRIDE_ALIGN - 1)) ||
	     l->low_res_src == AVE_OFF_NONE))
		return -EINVAL;
	/*
	 * Entropy-coding buffers: optional in the same sense as low_res_src
	 * above (0 = leave the table zero and reproduce the :8020 assert on
	 * purpose), but a table we do write must be complete and aligned, and
	 * the ABI must have the field.
	 */
	if (f->n_entropy) {
		u32 i, j, cols = f->n_entropy_cols ? f->n_entropy_cols : 1;
		u32 cols_max = l->entropy_cols_max ? l->entropy_cols_max : 1;

		if (l->entropy_set == AVE_OFF_NONE || !l->entropy_max ||
		    f->n_entropy > l->entropy_max ||
		    f->n_entropy > AVE_ENTROPY_MAX ||
		    cols > AVE_ENTROPY_COLS || cols > cols_max)
			return -EINVAL;
		for (i = 0; i < f->n_entropy; i++)
			for (j = 0; j < cols; j++)
				if (!f->entropy[i][j] ||
				    (f->entropy[i][j] & (align - 1)))
					return -EINVAL;
	}
	if (f->n_src_nbr) {
		u32 g, i;

		if (!l->src_nbr_max || f->n_src_nbr > l->src_nbr_max)
			return -EINVAL;
		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++) {
			if (l->src_nbr_set[g] == AVE_OFF_NONE)
				continue;
			for (i = 0; i < f->n_src_nbr; i++)
				if (!f->src_nbr[g][i] ||
				    (f->src_nbr[g][i] & (align - 1)))
					return -EINVAL;
		}
	}
	if (f->n_scratch > l->scratch_n)
		return -EINVAL;
	return 0;
}

/*
 * Fill PICMGMT at @base. Moved verbatim out of ave_cmd_build_process_avc(),
 * which passes l->picmgmt; HEVC passes process_hevc.picmgmt.
 */
static int ave_pic_fill(struct ave_wr *w, const struct ave_process_avc_layout *l,
			u32 base, const struct ave_avc_frame *f)
{
	u32 i;

	if (!ave_wr_ok(w, base, l->picmgmt_size))
		return w->err;
	if (l->picmgmt_size_word)
		wr32(w, base, l->picmgmt_size);

	wr32(w, base + l->frame_type, f->frame_type);
	if (l->frame_num != AVE_OFF_NONE) {
		if (l->frame_num_u32)
			wr32(w, base + l->frame_num, (u32)f->frame_num);
		else
			wr64(w, base + l->frame_num, f->frame_num);
	}
	if (l->poc != AVE_OFF_NONE)
		wr32(w, base + l->poc, f->poc);
	if (l->frame_rate_f64 != AVE_OFF_NONE && f->frame_rate)
		wr64(w, base + l->frame_rate_f64,
		     ave_u32_to_f64_bits(f->frame_rate));
	wr8(w, base + l->input_compressed, 0);

	wr64(w, base + l->in_luma_addr, f->in_luma_addr);
	wr32(w, base + l->in_luma_stride, f->in_luma_stride);
	wr64(w, base + l->in_chroma_addr, f->in_chroma_addr);
	wr32(w, base + l->in_chroma_stride, f->in_chroma_stride);
	if (l->in_luma_size != AVE_OFF_NONE)
		wr32(w, base + l->in_luma_size, f->in_luma_size);
	if (l->in_chroma_size != AVE_OFF_NONE)
		wr32(w, base + l->in_chroma_size, f->in_chroma_size);

	wr8(w, base + l->out_mode, 0);		/* Coded == CodedData[index] arm */
	wr32(w, base + l->out_index, f->coded_index);
	wr64(w, base + l->out_coded, f->coded_addr);
	wr64(w, base + l->out_coded_hdr, f->coded_hdr_addr);
	wr32(w, base + l->out_coded_size, f->coded_size);

	if (f->recon_luma_addr)
		wr64(w, base + l->recon_y, f->recon_luma_addr);
	if (f->recon_chroma_addr)
		wr64(w, base + l->recon_uv, f->recon_chroma_addr);
	if (f->recon_mv_addr)
		wr64(w, base + l->recon_mv, f->recon_mv_addr);
	if (f->recon_luma_lsb_addr && l->recon_y_lsb != AVE_OFF_NONE)
		wr64(w, base + l->recon_y_lsb, f->recon_luma_lsb_addr);
	if (f->recon_chroma_lsb_addr && l->recon_uv_lsb != AVE_OFF_NONE)
		wr64(w, base + l->recon_uv_lsb, f->recon_chroma_lsb_addr);

	wr32_opt(w, base + l->ctx_index, f->ctx_index);
	/*
	 * forceKeyFrame is an int the firmware reads in GetFrameType; the host
	 * writes 0 for its own "3" sentinel (kext 0xfffffe0008eab8d8-8e4), so
	 * only 0 and 1 are ever sent. It has no effect when the frame type is
	 * given explicitly (anything but 5 skips GetFrameType, fw 0x145d4).
	 */
	wr32_opt(w, base + l->force_key_frame, f->force_key_frame);
	/* Feeds nal_ref_idc through the rate controller (docs/64 §1.3). */
	if (l->force_non_ref != AVE_OFF_NONE)
		wr8(w, base + l->force_non_ref, f->force_non_ref);
	if (l->update_param_sets != AVE_OFF_NONE)
		wr8(w, base + l->update_param_sets, f->update_param_sets);
	/* HEVC: mode 0 = flat 16 in every quantiser scale register (docs/77 §2.5). */
	wr32_opt(w, base + l->scaling_matrix_mode, 0);

	if (f->low_res_src_addr && l->low_res_src != AVE_OFF_NONE)
		wr64(w, base + l->low_res_src, f->low_res_src_addr);
	/*
	 * encoder_addr_entropy[i][j] in the per-frame block: column 0 is what
	 * SetTranscode asserts (docs/54). The pipe's drain channels do NOT come
	 * from here - setRefPointers overwrites this whole region each frame from
	 * the Start_AVC table (docs/61 10) - but it is kept: harmless, and right
	 * again if a firmware ever stops overwriting it.
	 */
	for (i = 0; i < f->n_entropy; i++) {
		u32 j, cols = f->n_entropy_cols ? f->n_entropy_cols : 1;

		for (j = 0; j < cols; j++)
			wr64(w, base + l->entropy_set +
			     l->entropy_stride_i * i + l->entropy_stride_j * j,
			     f->entropy[i][j]);
	}

	for (i = 0; i < f->n_src_nbr; i++) {
		u32 g;

		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
			if (l->src_nbr_set[g] != AVE_OFF_NONE)
				wr64(w, base + l->src_nbr_set[g] + i * 8,
				     f->src_nbr[g][i]);
	}
	for (i = 0; i < f->n_scratch; i++)
		if (f->scratch[i] && l->scratch[i] != AVE_OFF_NONE)
			wr64(w, base + l->scratch[i], f->scratch[i]);
	return w->err;
}

int ave_cmd_build_process_avc(const struct ave_cmd_abi *abi, u8 *buf,
			      size_t len, const struct ave_cmd_ctx *ctx,
			      u32 slot, const struct ave_avc_frame *f)
{
	struct ave_wr w;
	int ret;

	if (!abi || !f)
		return -EINVAL;
	ret = ave_pic_check(abi, f, AVE_STRIDE_ALIGN);
	if (ret)
		return ret;

	ret = ave_cmd_begin(abi, AVE_OP_PROCESS_AVC, buf, len, ctx, slot, &w);
	if (ret < 0)
		return ret;
	ave_pic_fill(&w, &abi->process_avc, abi->process_avc.picmgmt, f);
	return ave_cmd_end(&w);
}

int ave_cmd_build_process_hevc(const struct ave_cmd_abi *abi, u8 *buf,
			       size_t len, const struct ave_cmd_ctx *ctx,
			       u32 slot, const struct ave_hevc_frame *hf)
{
	const struct ave_process_hevc_layout *hp;
	const struct ave_start_hevc_layout *hl;
	u32 sh, align, i;
	struct ave_wr w;
	int ret;

	if (!abi || !hf)
		return -EINVAL;
	hp = &abi->process_hevc;
	hl = &abi->start_hevc;
	if (hl->vps_block == AVE_OFF_NONE || !hp->slice || !hp->picmgmt ||
	    !hp->st_rps || !hp->hdr_slots || !hp->hdr_slot_bytes ||
	    !hp->pic_single_xc || !hp->sh_seg_limit ||
	    hp->pic_single_xc >= abi->process_avc.picmgmt_size ||
	    hp->sh_hdr_slots + 8 * hp->hdr_slots > hp->slice_fw_copy ||
	    hp->slice_fw_copy > hp->slice_size)
		return -EINVAL;
	align = hl->buf_align ? hl->buf_align : AVE_STRIDE_ALIGN;

	ret = ave_pic_check(abi, &hf->pic, align);
	if (ret)
		return ret;
	/* SetTranscode :7597/:7598 */
	if (hf->pic.coded_addr & (align - 1))
		return -EINVAL;
	if (!hf->hdr_slot_base ||
	    hf->hdr_slot_size < hp->hdr_slots * hp->hdr_slot_bytes)
		return -EINVAL;
	/* The firmware sets POC 0 on an IDR itself (0x215bc); say the same. */
	if (hf->pic.frame_type == AVE_FRAME_TYPE_IDR && hf->poc_lsb)
		return -EINVAL;

	ret = ave_cmd_begin(abi, AVE_OP_PROCESS_HEVC, buf, len, ctx, slot, &w);
	if (ret < 0)
		return ret;

	/* PICMGMT: the AVC fill at HEVC's base (docs/77 §3.3). */
	ave_pic_fill(&w, &abi->process_avc, hp->picmgmt, &hf->pic);

	/*
	 * S, the slice-header parameters (docs/77 §3.2). The firmware fills
	 * nal_unit_type, slice_type, the reference counts and slice_qp_delta
	 * itself (AVE_HEVC_PrepareSliceHeader 0x21278); these it does not.
	 * macOS's defaults, user space 0x6d360-0x6d3b8 (delegated).
	 */
	sh = hp->slice;
	wr32(&w, sh + hp->sh_size_word, hp->slice_size);
	wr8(&w, sh + hp->sh_first_slice, 1);
	wr32(&w, sh + hp->sh_pps_id, 0);	/* among wire 0xFD80.. (all 0) */
	wr32(&w, sh + hp->sh_poc_lsb, hf->poc_lsb);
	wr8(&w, sh + hp->sh_tmvp, 0);
	wr8(&w, sh + hp->sh_sao_luma, hf->sao);
	wr8(&w, sh + hp->sh_sao_chroma, hf->sao);
	wr8(&w, sh + hp->sh_col_from_l0, 1);
	wr32(&w, sh + hp->sh_five_minus_merge, 3);
	wr8(&w, sh + hp->sh_lf_across, 0);
	/* sh_map stays zero: one slice (kext GenerateMap, docs/77 §3.2). */
	/* macOS's all-ones pair after the map; 0 crashes the fw (§15). */
	wr32(&w, sh + hp->sh_seg_limit, 0xffffffff);
	wr32(&w, sh + hp->sh_seg_limit + 4, 0xffffffff);
	for (i = 0; i < hp->hdr_slots; i++)
		wr64(&w, sh + hp->sh_hdr_slots + 8 * i,
		     hf->hdr_slot_base + (u64)i * hp->hdr_slot_bytes);

	/* One transcoder into the coded buffer, or two into TranscodedData. */
	if (hf->single_xc)
		wr8(&w, hp->picmgmt + hp->pic_single_xc, 1);

	/* P: the SPS short-term set 0 (docs/77 §3.2); I/IDR carry none. */
	if (hf->pic.frame_type == AVE_FRAME_TYPE_P) {
		wr8(&w, hp->st_rps + hp->st_rps_sps_flag, 1);
		wr32(&w, hp->st_rps + hp->st_rps_idx, 0);
	}
	return ave_cmd_end(&w);
}

int ave_cmd_coded_length_codec(const struct ave_cmd_abi *abi, const void *hdr,
			       size_t hdr_len, u32 coded_size, bool hevc,
			       u32 hdr_slot_max, struct ave_coded_info *out)
{
	const struct ave_coded_hdr_layout *c;
	const u8 *h = hdr;
	u32 i, written = 0, removed = 0, hdr_total = 0;

	if (!abi || !hdr || !out)
		return -EINVAL;
	c = &abi->coded_hdr;
	if (!c->slice_stride)
		return -EINVAL;		/* layout not read for this ABI */
	if (hdr_len < c->min_bytes)
		return -EINVAL;
	if (hevc && (c->slice_hdr_len == AVE_OFF_NONE ||
		     c->slice_hdr_iova == AVE_OFF_NONE))
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->frame_type = get_unaligned_le32(h + c->frame_type);
	out->frame_num = get_unaligned_le32(h + c->frame_num);
	out->sps_pps_bits = get_unaligned_le32(h + c->sps_pps_bits);
	/* Never written for HEVC (docs/77 §4): whatever is there is not ours. */
	if (!hevc && c->cabac_zero_words != AVE_OFF_NONE &&
	    c->cabac_zero_words + 4u <= hdr_len)
		out->cabac_zero_words =
			get_unaligned_le32(h + c->cabac_zero_words);

	for (i = 0; i < c->slice_max; i++) {
		u32 rec = i * c->slice_stride;
		u32 n, hl = 0;
		u64 hiova = 0;
		int trim;

		if ((u64)rec + c->slice_bytes_removed + 1 > hdr_len)
			break;
		if (hevc && ((u64)rec + c->slice_hdr_len + 4 > hdr_len ||
			     (u64)rec + c->slice_hdr_iova + 8 > hdr_len))
			break;
		n = get_unaligned_le32(h + rec + c->slice_bytes_written);
		if (!n)
			break;
		trim = (signed char)h[rec + c->slice_bytes_removed];
		if (trim < 0)
			return -EPROTO;	/* Apple bails here too (0xec5078) */
		/*
		 * Per record, not just in aggregate. A single record with
		 * trim > written underflows this slice's length to nearly 4
		 * GiB while the total still balances against another record,
		 * and that length goes straight into a memcpy in
		 * ave_session_publish().
		 */
		if ((u32)trim > n)
			return -EPROTO;
		/* A byte count larger than the whole buffer is nonsense. */
		if (n > 0x40000000u || written > 0x40000000u - n)
			return -EPROTO;
		if (hevc) {
			/*
			 * The header lives in the slot the host gave for this
			 * slice; a length past the slot, or a length with no
			 * slot, is a header we cannot have read correctly.
			 */
			hl = get_unaligned_le32(h + rec + c->slice_hdr_len);
			hiova = get_unaligned_le64(h + rec + c->slice_hdr_iova);
			if ((hdr_slot_max && hl > hdr_slot_max) ||
			    (hl && !hiova) || hl > 0x100000u)
				return -EPROTO;
		}
		/*
		 * Refuse rather than record a prefix: the caller assembles the
		 * stream from slice[], so silently dropping records past the
		 * cap would publish a frame that is short by however many were
		 * left out, padded with whatever vmalloc handed us. We send
		 * one slice, so this is a guard, not a limit we expect to hit.
		 */
		if (out->n_slice >= AVE_CODED_SLICE_MAX)
			return -E2BIG;
		out->slice[out->n_slice].off = written;
		out->slice[out->n_slice].len = n - (u32)trim;
		out->slice[out->n_slice].hdr_iova = hiova;
		out->slice[out->n_slice].hdr_len = hl;
		out->n_slice++;
		written += n;
		removed += (u32)trim;
		hdr_total += hl;
		out->slices++;
	}
	if (removed > written)
		return -EPROTO;
	/*
	 * The host mirror of the firmware's own overflow test
	 * ([8312]+[8316]+[8356] <= [8360], fw 0x5bf88). It is also the cheap
	 * detector for having read a previous frame's stale records: the
	 * record array is only walked until a zero byte count, which is sound
	 * only because the buffer was cleared first.
	 */
	if (coded_size && written > coded_size)
		return -EPROTO;

	out->span = written;
	out->bytes_removed = removed;
	out->hdr_bytes = hdr_total;
	/* HEVC: Σ(written + hdrlen − removed), kext 0xfffffe0008ec4ed8. */
	out->bytes = written - removed + hdr_total;
	return 0;
}

int ave_cmd_coded_length(const struct ave_cmd_abi *abi, const void *hdr,
			 size_t hdr_len, u32 coded_size,
			 struct ave_coded_info *out)
{
	return ave_cmd_coded_length_codec(abi, hdr, hdr_len, coded_size, false,
					  0, out);
}

int ave_cmd_check_reply(const struct ave_cmd_abi *abi, enum ave_op op,
			const u8 *msg, size_t len, u64 client_id, u32 *status)
{
	const struct ave_cmd_desc *d;
	const struct ave_reply_layout *r;
	u64 cid;
	u32 st;

	if (!abi || !msg || op >= AVE_OP_COUNT)
		return -EINVAL;
	d = &abi->cmd[op];
	r = &abi->reply;
	if (!d->id || !d->reply_id || !d->reply_size)
		return -EINVAL;
	if (len < d->reply_size || r->status + 4u > d->reply_size ||
	    r->client_id + (u32)r->client_id_bytes > d->reply_size)
		return -EPROTO;
	if (get_unaligned_le16(msg + r->id) != d->reply_id)
		return -EPROTO;

	st = get_unaligned_le32(msg + r->status);
	if (status)
		*status = st;

	cid = r->client_id_bytes == 8 ? get_unaligned_le64(msg + r->client_id)
				      : get_unaligned_le32(msg + r->client_id);
	if (cid != (r->client_id_bytes == 8 ? client_id : (u32)client_id))
		return -EPROTO;

	return st == r->status_ok ? 0 : -EIO;
}
