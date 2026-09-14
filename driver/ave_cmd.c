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
	hevc = op == AVE_OP_START_HEVC || op == AVE_OP_PROCESS_HEVC;
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

int ave_cmd_build_start_avc(const struct ave_cmd_abi *abi, u8 *buf, size_t len,
			    const struct ave_cmd_ctx *ctx,
			    const struct ave_avc_session *s)
{
	const struct ave_start_avc_layout *l;
	const struct ave_sps_layout *sps;
	const struct ave_pps_layout *pps;
	u32 cw, ch, i;
	int prof, lvl, ret;
	struct ave_wr w;

	if (!abi || !s)
		return -EINVAL;
	l = &abi->start_avc;
	sps = &abi->sps;
	pps = &abi->pps;

	/* ---- parameter validation, before touching the buffer ---- */
	if (s->width < AVE_AVC_MIN_W || s->width > AVE_AVC_MAX_WH ||
	    s->height < AVE_AVC_MIN_H || s->height > AVE_AVC_MAX_WH ||
	    (s->width & 1) || (s->height & 1))
		return -EINVAL;
	if (!s->frame_rate || !s->key_interval)
		return -EINVAL;
	if (s->qp_i > 51 || s->qp_p > 51 || s->qp_b > 51)
		return -EINVAL;
	prof = ave_avc_profile_enum(s->profile_idc);
	lvl = ave_avc_level_enum(s->level_idc);
	if (prof < 0 || lvl < 0 || (s->profile_idc == 66 && s->cabac))
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
				    (s->src_nbr[g][i] & (AVE_STRIDE_ALIGN - 1)))
					return -EINVAL;
		}
	}

	ret = ave_cmd_begin(abi, AVE_OP_START_AVC, buf, len, ctx, 0, &w);
	if (ret < 0)
		return ret;

	cw = ave_mb_align(s->width);
	ch = ave_mb_align(s->height);

	/* ---- per-client firmware buffers ---- */
	wr64(&w, l->fw_client_addr, s->fw_client_addr);
	wr32(&w, l->fw_client_size, s->fw_client_size);
	wr64(&w, l->fw_client_mem_addr, s->fw_client_mem_addr);
	/* 13.5 only: 26.6.2's counterpart has not been located (docs/52). */
	/*
	 * sSVEMap.iNum: we always run a single SVE core. Writing it also keeps
	 * the firmware off iFwClientMemAddr, whose carve is behind iNum > 1
	 * (fw 0x5cb50). docs/54.
	 */
	wr32_opt(&w, l->sve_num, 1);
	if (l->param_sets_addr != AVE_OFF_NONE) {
		wr64(&w, l->param_sets_addr, s->param_sets_addr);
		wr32_opt(&w, l->param_sets_size, s->param_sets_size);
	}
	wr32(&w, l->fw_client_mem_size, s->fw_client_mem_size);

	/* ---- geometry: MB-aligned, display size via SPS cropping (docs/38) */
	wr32(&w, l->width, cw);
	wr32(&w, l->height, ch);

	/* ---- fixed-QP rate control ---- */
	wr32(&w, l->frame_rate, s->frame_rate);
	wr32(&w, l->bitrate, s->bitrate);
	wr32(&w, l->rc_mode, l->rc_mode_fixed_qp);
	if (l->rc_feature != AVE_OFF_NONE)
		wr64(&w, l->rc_feature, l->rc_feature_fixed_qp);
	wr32(&w, l->qp_i, s->qp_i);
	wr32(&w, l->qp_p, s->qp_p);
	wr32(&w, l->qp_b, s->qp_b);
	wr32(&w, l->qp_min, 0);
	wr32(&w, l->qp_max, 51);
	wr32(&w, l->key_interval, s->key_interval);
	wr32_opt(&w, l->key_interval_strict, s->key_interval);
	wr32(&w, l->slice_num, 1);

	/* ---- buffer tables ---- */
	for (i = 0; i < s->n_recon; i++) {
		u32 e = l->recon_set + i * l->recon_stride;

		wr64(&w, e + l->recon_addr, s->recon[i].addr);
		if (l->recon_size != AVE_OFF_NONE)
			wr32(&w, e + l->recon_size, s->recon[i].luma_size);
		/* 26.6.2 uncompressed entry is {base, luma, base, 0}, docs/21 §3.2 */
		if (l->recon_meta_addr != AVE_OFF_NONE)
			wr64(&w, e + l->recon_meta_addr, s->recon[i].addr);
		if (s->need_lsb_planes)
			wr64(&w, e + l->recon_lsb_addr, s->recon[i].lsb_addr);
	}
	if (s->need_lsb_planes)
		wr8(&w, l->need_lsb_planes, 1);
	for (i = 0; i < s->n_low_res_ref; i++)
		wr64(&w, l->low_res_ref_set + i * l->low_res_ref_stride,
		     s->low_res_ref[i]);
	for (i = 0; i < s->n_coded; i++) {
		wr64(&w, l->coded_addr + i * l->coded_addr_stride, s->coded[i].addr);
		wr32(&w, l->coded_size + i * l->coded_size_stride, s->coded[i].size);
		wr64(&w, l->coded_hdr_addr + i * l->coded_hdr_addr_stride,
		     s->coded_hdr[i].addr);
		wr32(&w, l->coded_hdr_size + i * l->coded_hdr_size_stride,
		     s->coded_hdr[i].size);
	}
	for (i = 0; i < s->n_src_nbr; i++) {
		u32 g;

		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
			if (l->src_nbr_set[g] != AVE_OFF_NONE)
				wr64(&w, l->src_nbr_set[g] + i * 8,
				     s->src_nbr[g][i]);
	}

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
	wr8(&w, sps->frame_cropping_flag, cw != s->width || ch != s->height);
	wr32(&w, sps->crop_left, 0);
	wr32(&w, sps->crop_right, (cw - s->width) / 2);
	wr32(&w, sps->crop_top, 0);
	wr32(&w, sps->crop_bottom, (ch - s->height) / 2);
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

int ave_cmd_build_process_avc(const struct ave_cmd_abi *abi, u8 *buf,
			      size_t len, const struct ave_cmd_ctx *ctx,
			      u32 slot, const struct ave_avc_frame *f)
{
	const struct ave_process_avc_layout *l;
	struct ave_wr w;
	u32 base, i;
	int ret;

	if (!abi || !f)
		return -EINVAL;
	l = &abi->process_avc;

	if (f->frame_type != AVE_FRAME_TYPE_I &&
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
		u32 i;

		if (l->entropy_set == AVE_OFF_NONE || !l->entropy_max ||
		    f->n_entropy > l->entropy_max ||
		    f->n_entropy > AVE_ENTROPY_MAX)
			return -EINVAL;
		for (i = 0; i < f->n_entropy; i++)
			if (!f->entropy[i] ||
			    (f->entropy[i] & (AVE_STRIDE_ALIGN - 1)))
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
				    (f->src_nbr[g][i] & (AVE_STRIDE_ALIGN - 1)))
					return -EINVAL;
		}
	}
	if (f->n_scratch > l->scratch_n)
		return -EINVAL;

	ret = ave_cmd_begin(abi, AVE_OP_PROCESS_AVC, buf, len, ctx, slot, &w);
	if (ret < 0)
		return ret;

	base = l->picmgmt;
	if (!ave_wr_ok(&w, base, l->picmgmt_size))
		return ave_cmd_end(&w);
	if (l->picmgmt_size_word)
		wr32(&w, base, l->picmgmt_size);

	wr32(&w, base + l->frame_type, f->frame_type);
	if (l->frame_num != AVE_OFF_NONE)
		wr64(&w, base + l->frame_num, f->frame_num);
	if (l->poc != AVE_OFF_NONE)
		wr32(&w, base + l->poc, f->poc);
	if (l->frame_rate_f64 != AVE_OFF_NONE && f->frame_rate)
		wr64(&w, base + l->frame_rate_f64,
		     ave_u32_to_f64_bits(f->frame_rate));
	wr8(&w, base + l->input_compressed, 0);

	wr64(&w, base + l->in_luma_addr, f->in_luma_addr);
	wr32(&w, base + l->in_luma_stride, f->in_luma_stride);
	wr64(&w, base + l->in_chroma_addr, f->in_chroma_addr);
	wr32(&w, base + l->in_chroma_stride, f->in_chroma_stride);
	if (l->in_luma_size != AVE_OFF_NONE)
		wr32(&w, base + l->in_luma_size, f->in_luma_size);
	if (l->in_chroma_size != AVE_OFF_NONE)
		wr32(&w, base + l->in_chroma_size, f->in_chroma_size);

	wr8(&w, base + l->out_mode, 0);		/* Coded == CodedData[index] arm */
	wr32(&w, base + l->out_index, f->coded_index);
	wr64(&w, base + l->out_coded, f->coded_addr);
	wr64(&w, base + l->out_coded_hdr, f->coded_hdr_addr);
	wr32(&w, base + l->out_coded_size, f->coded_size);

	if (f->recon_luma_addr)
		wr64(&w, base + l->recon_y, f->recon_luma_addr);
	if (f->recon_chroma_addr)
		wr64(&w, base + l->recon_uv, f->recon_chroma_addr);
	if (f->recon_mv_addr)
		wr64(&w, base + l->recon_mv, f->recon_mv_addr);
	if (f->recon_luma_lsb_addr && l->recon_y_lsb != AVE_OFF_NONE)
		wr64(&w, base + l->recon_y_lsb, f->recon_luma_lsb_addr);
	if (f->recon_chroma_lsb_addr && l->recon_uv_lsb != AVE_OFF_NONE)
		wr64(&w, base + l->recon_uv_lsb, f->recon_chroma_lsb_addr);

	wr32_opt(&w, base + l->ctx_index, f->ctx_index);
	/*
	 * forceKeyFrame is an int the firmware reads in GetFrameType; the host
	 * writes 0 for its own "3" sentinel (kext 0xfffffe0008eab8d8-8e4), so
	 * only 0 and 1 are ever sent. It has no effect when the frame type is
	 * given explicitly (anything but 5 skips GetFrameType, fw 0x145d4).
	 */
	wr32_opt(&w, base + l->force_key_frame, f->force_key_frame);
	if (l->force_non_ref != AVE_OFF_NONE)
		wr8(&w, base + l->force_non_ref, 0);
	if (l->update_param_sets != AVE_OFF_NONE)
		wr8(&w, base + l->update_param_sets, f->update_param_sets);
	wr32_opt(&w, base + l->scaling_matrix_mode, 0);

	if (f->low_res_src_addr && l->low_res_src != AVE_OFF_NONE)
		wr64(&w, base + l->low_res_src, f->low_res_src_addr);
	/* encoder_addr_entropy[i][0]; j = transcode_buffer_id = 0 (docs/54). */
	for (i = 0; i < f->n_entropy; i++)
		wr64(&w, base + l->entropy_set + l->entropy_stride_i * i,
		     f->entropy[i]);

	for (i = 0; i < f->n_src_nbr; i++) {
		u32 g;

		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
			if (l->src_nbr_set[g] != AVE_OFF_NONE)
				wr64(&w, base + l->src_nbr_set[g] + i * 8,
				     f->src_nbr[g][i]);
	}
	for (i = 0; i < f->n_scratch; i++)
		if (f->scratch[i] && l->scratch[i] != AVE_OFF_NONE)
			wr64(&w, base + l->scratch[i], f->scratch[i]);

	return ave_cmd_end(&w);
}

int ave_cmd_coded_length(const struct ave_cmd_abi *abi, const void *hdr,
			 size_t hdr_len, struct ave_coded_info *out)
{
	const struct ave_coded_hdr_layout *c;
	const u8 *h = hdr;
	u32 i, written = 0, removed = 0;

	if (!abi || !hdr || !out)
		return -EINVAL;
	c = &abi->coded_hdr;
	if (!c->slice_stride)
		return -EINVAL;		/* layout not read for this ABI */
	if (hdr_len < c->min_bytes)
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->frame_type = get_unaligned_le32(h + c->frame_type);
	out->frame_num = get_unaligned_le32(h + c->frame_num);
	out->sps_pps_bits = get_unaligned_le32(h + c->sps_pps_bits);

	for (i = 0; i < c->slice_max; i++) {
		u32 rec = i * c->slice_stride;
		u32 n;
		int trim;

		if ((u64)rec + c->slice_bytes_removed + 1 > hdr_len)
			break;
		n = get_unaligned_le32(h + rec + c->slice_bytes_written);
		if (!n)
			break;
		trim = (signed char)h[rec + c->slice_bytes_removed];
		if (trim < 0)
			return -EPROTO;	/* Apple bails here too (0xec5078) */
		/* A byte count larger than the whole buffer is nonsense. */
		if (n > 0x40000000u || written > 0x40000000u - n)
			return -EPROTO;
		written += n;
		removed += (u32)trim;
		out->slices++;
	}
	if (removed > written)
		return -EPROTO;

	out->bytes_removed = removed;
	out->bytes = written - removed;
	return 0;
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
