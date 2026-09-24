// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace self-test for the opening command sequence that driver's
 * ave_session.c sends (Config -> Open -> Start_AVC).
 *
 * ave_session.c itself is driver-coupled (completions, DMA, the live IPC
 * transport), so it cannot be linked here. What IS testable in userspace, and
 * what actually matters for a first hardware run, is that the *parameter
 * choices* ave_session.c feeds to the pure builders in driver/ave_cmd.c are
 * accepted for BOTH firmware ABIs and produce commands of the exact size the
 * firmware asserts. This harness therefore rebuilds the same three commands
 * with the same values ave_session.c uses, against ave_cmd_abi_get() for each
 * ABI, and checks:
 *
 *   - each builder returns >= 0 (parameters accepted) and the right size;
 *   - the header carries the ids and client id we expect;
 *   - ave_cmd_check_reply() accepts a synthesised success reply and, as a
 *     negative control, rejects a failure status and a wrong-id reply.
 *
 * Pure computation. Touches no hardware. Mirrors tools/abi_selftest's shim
 * setup (-Ishim -include kshim.h).
 */
#include <stdio.h>
#include <stdlib.h>

#include "kshim.h"
#include "../../driver/ave_cmd.h"

/* Values duplicated from driver/ave_session.c (kept in sync by hand). */
#define SESS_CLIENT_ID		1u
#define SESS_SHMEM_SIZE		0x40000
#define SESS_FWCLIENT_SIZE	0xb4000
#define SESS_FWCLIENTMEM_SIZE	0x100000
#define SESS_CODED_SIZE		0x200000
#define SESS_WIDTH		1280
#define SESS_HEIGHT		720
#define SESS_QP			30

/* Fake but plausible IOVAs; the builders only require alignment / non-zero. */
#define IOVA_SHMEM		0x10000000ull
#define IOVA_FWCLIENT		0x20000000ull
#define IOVA_FWCLIENTMEM	0x30000000ull
#define IOVA_RECON		0x40000000ull	/* 128-aligned */
#define IOVA_CODED		0x50000000ull
#define IOVA_CODEDHDR		0x60000000ull
#define IOVA_LUMA		0x70000000ull	/* 64-aligned */
#define IOVA_CHROMA		0x70400000ull
#define IOVA_NBR		0x80000000ull	/* 64-aligned, +0x10000 per slot */
#define IOVA_LOWRES		0x90000000ull	/* 64-aligned LRME scaled-luma target */

/*
 * DPB slots the harness publishes. The firmware needs max_num_ref_frames+1 of
 * them (ProvideReferenceFrames copies slots 0..numRefs, fw 0x2b638-0x2b644,
 * with numRefs = SPS max_num_ref_frames, fw 0x5dd14) and the driver sends
 * max_num_ref_frames = 1, so 2.
 */
#define SESS_DPB		2
/* ALIGN(ALIGN(4*W,256) * ((H+63)>>4), 512) - AVE_CalcBufSizeOfLowResRef. */
#define SESS_LOWRES_SIZE	0x3c000ull
#define SESS_STRIDE		1280		/* % 64 == 0 */
#define SESS_PROCESS_SLOT	21
#define ALIGN_UP(x, a)		(((x) + (a) - 1) / (a) * (a))

static int failures, checks;
static const char *ctx;

#define CHECK(cond, ...) do {						\
		checks++;						\
		if (!(cond)) {						\
			failures++;					\
			printf("  FAIL [%s] ", ctx);			\
			printf(__VA_ARGS__);				\
			printf("\n");					\
		}							\
	} while (0)

static u8 cmdbuf[0x20000];

/* Synthesise the firmware's success reply for @op into @buf, per abi->reply. */
static void make_reply(const struct ave_cmd_abi *abi, enum ave_op op,
		       u64 client_id, u32 status, u16 id_override, u8 *buf)
{
	const struct ave_cmd_desc *d = &abi->cmd[op];
	const struct ave_reply_layout *r = &abi->reply;

	memset(buf, 0, d->reply_size);
	put_unaligned_le16(id_override ? id_override : d->reply_id, buf + r->id);
	if (r->client_id_bytes == 8)
		put_unaligned_le64(client_id, buf + r->client_id);
	else
		put_unaligned_le32((u32)client_id, buf + r->client_id);
	put_unaligned_le32(status, buf + r->status);
}

static void test_abi(enum ave_fw_abi which, const char *name)
{
	const struct ave_cmd_abi *abi = ave_cmd_abi_get(which);
	const struct ave_hdr_layout *h;
	u8 reply[0x80];
	int ret;

	printf("ABI %s:\n", name);
	ctx = name;
	CHECK(abi != NULL, "ave_cmd_abi_get returned NULL");
	if (!abi)
		return;
	h = &abi->hdr;

	/* ---- Config (global, no client) ---- */
	ctx = "Config";
	{
		struct ave_cmd_ctx c = { .count = 1, .client_id = 0 };
		struct ave_config_params p = {
			.skip_mcpu = false,
			.create_mcpu = true,
			.reg_dart_addr = 0,	/* session_reg_dart default */
			.dsid = 0,
			.shmem_addr = IOVA_SHMEM,
			.shmem_size = SESS_SHMEM_SIZE,
		};
		size_t want = ave_cmd_size(abi, AVE_OP_CONFIG);

		ret = ave_cmd_build_config(abi, cmdbuf, sizeof(cmdbuf), &c, &p);
		CHECK(ret == (int)want, "build_config ret %d want %zu", ret, want);
		CHECK(get_unaligned_le16(cmdbuf) == abi->cmd[AVE_OP_CONFIG].id,
		      "config id %#x", get_unaligned_le16(cmdbuf));

		make_reply(abi, AVE_OP_CONFIG, 0, abi->reply.status_ok, 0, reply);
		CHECK(ave_cmd_check_reply(abi, AVE_OP_CONFIG, reply,
					  abi->cmd[AVE_OP_CONFIG].reply_size,
					  0, NULL) == 0,
		      "check_reply rejected a success Config reply");
	}

	/* ---- Open ---- */
	ctx = "Open";
	{
		struct ave_cmd_ctx c = { .count = 2, .client_id = SESS_CLIENT_ID };
		size_t want = ave_cmd_size(abi, AVE_OP_OPEN);
		u64 got_cid;

		ret = ave_cmd_build_open(abi, cmdbuf, sizeof(cmdbuf), &c);
		CHECK(ret == (int)want, "build_open ret %d want %zu", ret, want);
		CHECK(get_unaligned_le16(cmdbuf) == abi->cmd[AVE_OP_OPEN].id,
		      "open id %#x", get_unaligned_le16(cmdbuf));
		got_cid = h->client_id_bytes == 8
			? get_unaligned_le64(cmdbuf + h->client_id)
			: get_unaligned_le32(cmdbuf + h->client_id);
		CHECK(got_cid == SESS_CLIENT_ID, "open client id %llu",
		      (unsigned long long)got_cid);

		make_reply(abi, AVE_OP_OPEN, SESS_CLIENT_ID,
			   abi->reply.status_ok, 0, reply);
		CHECK(ave_cmd_check_reply(abi, AVE_OP_OPEN, reply,
					  abi->cmd[AVE_OP_OPEN].reply_size,
					  SESS_CLIENT_ID, NULL) == 0,
		      "check_reply rejected a success Open reply");

		/* Negative control 1: failure status must be -EIO. */
		make_reply(abi, AVE_OP_OPEN, SESS_CLIENT_ID,
			   abi->reply.status_ok ^ 0xdead, 0, reply);
		CHECK(ave_cmd_check_reply(abi, AVE_OP_OPEN, reply,
					  abi->cmd[AVE_OP_OPEN].reply_size,
					  SESS_CLIENT_ID, NULL) == -EIO,
		      "check_reply accepted a FAILURE status (no negative control!)");

		/* Negative control 2: wrong reply id must be -EPROTO. */
		make_reply(abi, AVE_OP_OPEN, SESS_CLIENT_ID,
			   abi->reply.status_ok, 0x7fff, reply);
		CHECK(ave_cmd_check_reply(abi, AVE_OP_OPEN, reply,
					  abi->cmd[AVE_OP_OPEN].reply_size,
					  SESS_CLIENT_ID, NULL) == -EPROTO,
		      "check_reply accepted a WRONG-ID reply");
	}

	/* ---- Start_AVC ---- */
	ctx = "Start_AVC";
	{
		struct ave_cmd_ctx c = { .count = 3, .client_id = SESS_CLIENT_ID };
		struct ave_recon_buf recon[SESS_DPB] = {
			{ IOVA_RECON,		    SESS_WIDTH * SESS_HEIGHT, 0 },
			{ IOVA_RECON + 0x1000000ull, SESS_WIDTH * SESS_HEIGHT, 0 },
		};
		struct ave_buf coded = { .addr = IOVA_CODED, .size = SESS_CODED_SIZE };
		struct ave_buf coded_hdr = {
			.addr = IOVA_CODEDHDR,
			.size = abi->start_avc.coded_hdr_bytes,
		};
		struct ave_avc_session s = {
			.width = SESS_WIDTH, .height = SESS_HEIGHT,
			.frame_rate = 30, .bitrate = 0,
			.qp_i = SESS_QP, .qp_p = SESS_QP, .qp_b = SESS_QP,
			.key_interval = 1,
			.profile_idc = 66, .level_idc = 40, .cabac = false,
			.fw_client_addr = IOVA_FWCLIENT,
			.fw_client_size = SESS_FWCLIENT_SIZE,
			.fw_client_mem_addr = IOVA_FWCLIENTMEM,
			.fw_client_mem_size = SESS_FWCLIENTMEM_SIZE,
			.param_sets_addr = IOVA_FWCLIENTMEM + 0x100000,
			.param_sets_size = 0x1000,
			.recon = recon, .n_recon = SESS_DPB,
			.low_res_ref = { IOVA_LOWRES,
					 IOVA_LOWRES + SESS_LOWRES_SIZE },
			.n_low_res_ref = SESS_DPB,
			.coded = &coded, .coded_hdr = &coded_hdr, .n_coded = 1,
		};
		const struct ave_start_avc_layout *sl = &abi->start_avc;
		size_t want = ave_cmd_size(abi, AVE_OP_START_AVC);
		unsigned int k;

		/*
		 * 26.6.2 has no LowResRef table (not located); publishing one
		 * there must be refused, and the rest of the command must still
		 * build once it is dropped.
		 */
		if (sl->low_res_ref_set == AVE_OFF_NONE) {
			ret = ave_cmd_build_start_avc(abi, cmdbuf,
						      sizeof(cmdbuf), &c, &s);
			CHECK(ret == -EINVAL,
			      "builder accepted a LowResRef table for an ABI that has none (ret %d)",
			      ret);
			s.n_low_res_ref = 0;
		}

		ret = ave_cmd_build_start_avc(abi, cmdbuf, sizeof(cmdbuf), &c, &s);
		CHECK(ret == (int)want, "build_start_avc ret %d want %zu",
		      ret, want);
		CHECK(get_unaligned_le16(cmdbuf) == abi->cmd[AVE_OP_START_AVC].id,
		      "start id %#x", get_unaligned_le16(cmdbuf));

		make_reply(abi, AVE_OP_START_AVC, SESS_CLIENT_ID,
			   abi->reply.status_ok, 0, reply);
		CHECK(ave_cmd_check_reply(abi, AVE_OP_START_AVC, reply,
					  abi->cmd[AVE_OP_START_AVC].reply_size,
					  SESS_CLIENT_ID, NULL) == 0,
		      "check_reply rejected a success Start_AVC reply");

		/*
		 * The DPB tables. Slot i of the recon table and slot i of the
		 * LowResRef table describe the same DPB entry: the firmware
		 * copies wire recon_set[i] to DPB entry+48 and wire
		 * low_res_ref_set[i] to DPB entry+64 in the same pass
		 * (ProvideReferenceFrames, fw 0x2b75c / 0x2b780), and
		 * setRefPointers then publishes entry+64 as
		 * sLowResOutput.LowResSrcLumaScaled (fw 0x2c320).
		 */
		for (k = 0; k < SESS_DPB; k++) {
			u32 roff = sl->recon_set + k * sl->recon_stride +
				   sl->recon_addr;

			CHECK(get_unaligned_le64(cmdbuf + roff) == recon[k].addr,
			      "recon slot %u not at wire %#x", k, roff);
			CHECK(roff + 8 <= want,
			      "recon slot %u at wire %#x runs past the command",
			      k, roff);
		}
		if (sl->low_res_ref_set != AVE_OFF_NONE) {
			CHECK(sl->low_res_ref_stride == 8,
			      "LowResRef stride %u is not a u64",
			      sl->low_res_ref_stride);
			CHECK(sl->low_res_ref_max >= SESS_DPB,
			      "LowResRef table holds only %u slots",
			      sl->low_res_ref_max);
			for (k = 0; k < SESS_DPB; k++) {
				u32 off = sl->low_res_ref_set +
					  k * sl->low_res_ref_stride;

				CHECK(get_unaligned_le64(cmdbuf + off) ==
				      s.low_res_ref[k],
				      "LowResRef slot %u not at wire %#x", k, off);
				CHECK(off + 8 <= want,
				      "LowResRef slot %u at wire %#x runs past the command",
				      k, off);
				/* The two tables must not overlap each other. */
				CHECK(off >= sl->recon_set +
					     sl->low_res_ref_max * sl->recon_stride ||
				      off + 8 <= sl->recon_set,
				      "LowResRef slot %u at wire %#x lands inside the recon table at %#x",
				      k, off, sl->recon_set);
			}
			/* One past the last published slot must still be zero. */
			CHECK(get_unaligned_le64(cmdbuf + sl->low_res_ref_set +
						 SESS_DPB * sl->low_res_ref_stride) == 0,
			      "LowResRef slot %u was written but not published",
			      SESS_DPB);
		}

		/*
		 * Negative controls for the LowResRef table.
		 *
		 * Zero entries at all IS allowed: that is the session_lowres=0
		 * bisect, which must still build and must leave the table zero
		 * so the run reproduces ASSERT CAVCController_H13C.cpp:5782.
		 */
		if (sl->low_res_ref_set != AVE_OFF_NONE) {
			u64 keep = s.low_res_ref[1];

			s.low_res_ref[1] = IOVA_LOWRES + 32;	/* 32, not 64 */
			ret = ave_cmd_build_start_avc(abi, cmdbuf,
						      sizeof(cmdbuf), &c, &s);
			CHECK(ret == -EINVAL,
			      "builder accepted a LowResRef that is 32- but not 64-aligned (ret %d)",
			      ret);

			s.low_res_ref[1] = 0;
			ret = ave_cmd_build_start_avc(abi, cmdbuf,
						      sizeof(cmdbuf), &c, &s);
			CHECK(ret == -EINVAL,
			      "builder accepted a LowResRef table with a zero hole (ret %d)",
			      ret);

			s.low_res_ref[1] = keep;
			s.n_low_res_ref = SESS_DPB - 1;
			ret = ave_cmd_build_start_avc(abi, cmdbuf,
						      sizeof(cmdbuf), &c, &s);
			CHECK(ret == -EINVAL,
			      "builder accepted fewer LowResRef entries than DPB slots (ret %d)",
			      ret);

			s.n_low_res_ref = sl->low_res_ref_max + 1;
			ret = ave_cmd_build_start_avc(abi, cmdbuf,
						      sizeof(cmdbuf), &c, &s);
			CHECK(ret == -EINVAL,
			      "builder accepted more LowResRef entries than the table holds (ret %d)",
			      ret);

			/* The deliberate zero: no table, command still builds. */
			s.n_low_res_ref = 0;
			ret = ave_cmd_build_start_avc(abi, cmdbuf,
						      sizeof(cmdbuf), &c, &s);
			CHECK(ret == (int)want,
			      "session_lowres=0 control: builder refused to omit the LowResRef table (ret %d)",
			      ret);
			for (k = 0; k < sl->low_res_ref_max; k++)
				CHECK(get_unaligned_le64(cmdbuf + sl->low_res_ref_set +
							 k * sl->low_res_ref_stride) == 0,
				      "session_lowres=0 control: LowResRef slot %u is not zero",
				      k);
			s.n_low_res_ref = SESS_DPB;
		}

		/* The coded-header buffer must satisfy the builder's minimum. */
		coded_hdr.size = abi->start_avc.coded_hdr_bytes - 1;
		ret = ave_cmd_build_start_avc(abi, cmdbuf, sizeof(cmdbuf), &c, &s);
		CHECK(ret == -EINVAL,
		      "builder accepted an undersized coded-header buffer (ret %d)",
		      ret);
	}

	/* ---- Process (one I-frame) ---- */
	ctx = "Process";
	{
		const struct ave_process_avc_layout *l = &abi->process_avc;
		struct ave_cmd_ctx c = { .count = 4, .client_id = SESS_CLIENT_ID };
		struct ave_avc_frame f = {
			.frame_type	= AVE_FRAME_TYPE_IDR,
			.in_luma_addr	= IOVA_LUMA,
			.in_luma_stride	= SESS_STRIDE,
			.in_luma_size	= SESS_STRIDE * SESS_HEIGHT,
			.in_chroma_addr	= IOVA_CHROMA,
			.in_chroma_stride = SESS_STRIDE,
			.in_chroma_size	= SESS_STRIDE * SESS_HEIGHT / 2,
			.coded_index	= 0,
			.coded_addr	= IOVA_CODED,
			.coded_hdr_addr	= IOVA_CODEDHDR,
			.coded_size	= SESS_CODED_SIZE,
			.recon_luma_addr	= IOVA_RECON,
			.recon_chroma_addr	= IOVA_RECON + 0x100000,
			.recon_luma_lsb_addr	= IOVA_RECON + 0x200000,
			.recon_chroma_lsb_addr	= IOVA_RECON + 0x300000,
			.recon_mv_addr	= IOVA_RECON + 0x400000,
			.ctx_index	= 0,
			.force_key_frame = true,
			.low_res_src_addr = IOVA_LOWRES,
			.n_scratch	= 0,
		};
		size_t want = ave_cmd_size(abi, AVE_OP_PROCESS_AVC);
		u32 g, i, base = l->picmgmt;
		u64 saved;

		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
			for (i = 0; i < AVE_SRC_NBR_MAX; i++)
				f.src_nbr[g][i] = IOVA_NBR +
					((g * AVE_SRC_NBR_MAX + i) << 16);
		f.n_src_nbr = l->src_nbr_max;

		ret = ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f);
		CHECK(ret == (int)want, "build_process ret %d want %zu", ret, want);
		CHECK(get_unaligned_le16(cmdbuf) == abi->cmd[AVE_OP_PROCESS_AVC].id,
		      "process id %#x", get_unaligned_le16(cmdbuf));
		CHECK(get_unaligned_le32(cmdbuf + h->slot) == SESS_PROCESS_SLOT,
		      "process slot %u", get_unaligned_le32(cmdbuf + h->slot));
		CHECK(SESS_PROCESS_SLOT < h->max_slot,
		      "the slot the driver uses (%u) is >= max_slot %u",
		      SESS_PROCESS_SLOT, h->max_slot);
		CHECK(base + l->picmgmt_size <= want,
		      "PICMGMT %#x + %#x runs past the %#zx command",
		      base, l->picmgmt_size, want);
		if (l->picmgmt_size_word)
			CHECK(get_unaligned_le32(cmdbuf + base) == l->picmgmt_size,
			      "PICMGMT size word %#x want %#x",
			      get_unaligned_le32(cmdbuf + base), l->picmgmt_size);

		/* Every field the driver sets must land where the table says. */
		CHECK(get_unaligned_le64(cmdbuf + base + l->in_luma_addr) == IOVA_LUMA,
		      "luma addr not at PICMGMT+%#x", l->in_luma_addr);
		CHECK(get_unaligned_le32(cmdbuf + base + l->in_luma_stride) == SESS_STRIDE,
		      "luma stride not at PICMGMT+%#x", l->in_luma_stride);
		CHECK(get_unaligned_le64(cmdbuf + base + l->in_chroma_addr) == IOVA_CHROMA,
		      "chroma addr not at PICMGMT+%#x", l->in_chroma_addr);
		CHECK(get_unaligned_le32(cmdbuf + base + l->in_chroma_stride) == SESS_STRIDE,
		      "chroma stride not at PICMGMT+%#x", l->in_chroma_stride);
		CHECK(get_unaligned_le32(cmdbuf + base + l->frame_type) == AVE_FRAME_TYPE_IDR,
		      "frame type not at PICMGMT+%#x", l->frame_type);
		CHECK(cmdbuf[base + l->out_mode] == 0,
		      "out mode byte is not 0 (that arm needs size > 3*W*H/4)");
		CHECK(get_unaligned_le32(cmdbuf + base + l->out_index) == 0,
		      "out index not at PICMGMT+%#x", l->out_index);
		CHECK(get_unaligned_le64(cmdbuf + base + l->out_coded) == IOVA_CODED,
		      "coded addr not at PICMGMT+%#x", l->out_coded);
		CHECK(get_unaligned_le64(cmdbuf + base + l->out_coded_hdr) == IOVA_CODEDHDR,
		      "coded hdr addr not at PICMGMT+%#x", l->out_coded_hdr);
		CHECK(get_unaligned_le32(cmdbuf + base + l->out_coded_size) == SESS_CODED_SIZE,
		      "coded size not at PICMGMT+%#x", l->out_coded_size);
		CHECK(get_unaligned_le64(cmdbuf + base + l->recon_y) == IOVA_RECON,
		      "recon Y not at PICMGMT+%#x", l->recon_y);
		CHECK(get_unaligned_le64(cmdbuf + base + l->recon_uv) ==
		      IOVA_RECON + 0x100000,
		      "recon UV not at PICMGMT+%#x", l->recon_uv);
		CHECK(get_unaligned_le64(cmdbuf + base + l->recon_mv) ==
		      IOVA_RECON + 0x400000,
		      "recon MV not at PICMGMT+%#x", l->recon_mv);
		CHECK(cmdbuf[base + l->input_compressed] == 0,
		      "bInputCompressed is not 0");
		if (l->ctx_index != AVE_OFF_NONE)
			CHECK(get_unaligned_le32(cmdbuf + base + l->ctx_index) == 0,
			      "context index not written");
		if (l->recon_y_lsb != AVE_OFF_NONE)
			CHECK(get_unaligned_le64(cmdbuf + base + l->recon_y_lsb) ==
			      IOVA_RECON + 0x200000,
			      "recon Y_LSB not at PICMGMT+%#x", l->recon_y_lsb);
		/*
		 * sLowResOutput. The firmware calls setLRME for every frame and
		 * asserts LowResSrcLumaScaled != 0 (setLRME:5782, fw 0x52430), so
		 * the address has to land at the recorded offset.
		 */
		CHECK(l->low_res_src != AVE_OFF_NONE,
		      "no LowResSrcLumaScaled offset for this ABI");
		if (l->low_res_src != AVE_OFF_NONE) {
			CHECK(get_unaligned_le64(cmdbuf + base + l->low_res_src) ==
			      IOVA_LOWRES,
			      "LowResSrcLumaScaled not at PICMGMT+%#x", l->low_res_src);
			CHECK(base + l->low_res_src + 8 <= want,
			      "LowResSrcLumaScaled at PICMGMT+%#x runs past the command",
			      l->low_res_src);
			CHECK(l->low_res_src + 8 <= l->picmgmt_size,
			      "LowResSrcLumaScaled at +%#x runs past PICMGMT (%#x)",
			      l->low_res_src, l->picmgmt_size);
		}
		/*
		 * LowResResults[] must stay ZERO: the driver never publishes them
		 * and the firmware cbz-skips each one. Guard against a future
		 * builder writing there by accident.
		 */
		for (i = 0; i < l->low_res_results_max; i++) {
			u32 off = l->low_res_results + i * l->low_res_results_stride;

			CHECK(get_unaligned_le64(cmdbuf + base + off) == 0,
			      "LowResResults[%u] at PICMGMT+%#x is not zero", i, off);
			CHECK(off != l->low_res_src,
			      "LowResResults[%u] overlaps LowResSrcLumaScaled", i);
		}
		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++) {
			if (l->src_nbr_set[g] == AVE_OFF_NONE)
				continue;
			for (i = 0; i < f.n_src_nbr; i++)
				CHECK(get_unaligned_le64(cmdbuf + base +
							 l->src_nbr_set[g] + i * 8) ==
				      f.src_nbr[g][i],
				      "SrcNbr[%u][%u] not at PICMGMT+%#x",
				      g, i, l->src_nbr_set[g] + i * 8);
		}

		make_reply(abi, AVE_OP_PROCESS_AVC, SESS_CLIENT_ID,
			   abi->reply.status_ok, 0, reply);
		CHECK(ave_cmd_check_reply(abi, AVE_OP_PROCESS_AVC, reply,
					  abi->cmd[AVE_OP_PROCESS_AVC].reply_size,
					  SESS_CLIENT_ID, NULL) == 0,
		      "check_reply rejected a success Process reply");

		/* ---- negative controls ---- */
		saved = f.in_luma_addr;
		f.in_luma_addr = 0;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a ZERO input luma address");
		f.in_luma_addr = saved + 1;	/* not 64-aligned */
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a misaligned input luma address");
		f.in_luma_addr = saved;

		f.in_luma_stride = SESS_STRIDE + 1;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a stride that is not a multiple of 64");
		f.in_luma_stride = SESS_STRIDE;

		saved = f.in_chroma_addr;
		f.in_chroma_addr = 0;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a ZERO input chroma address");
		f.in_chroma_addr = saved;

		f.recon_luma_addr = IOVA_RECON + 64;	/* 64 but not 128 */
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a recon plane that is not 128-aligned");
		f.recon_luma_addr = IOVA_RECON;

		f.coded_addr = 0;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a ZERO coded address");
		f.coded_addr = IOVA_CODED;

		f.coded_hdr_addr = 0;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a ZERO coded-header address");
		f.coded_hdr_addr = IOVA_CODEDHDR;

		/* P (1) is legal now; B (2) still is not - no reference list
		 * is built for it - and 5 is not an IMG_FRAME_TYPE at all. */
		f.frame_type = 2;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a B frame with no reference list");
		f.frame_type = 5;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted frame type 5");
		f.frame_type = AVE_FRAME_TYPE_IDR;

		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						h->max_slot, &f) == -EINVAL,
		      "builder accepted slot == max_slot");

		/*
		 * LowResSrcLumaScaled negative controls. Zero is ALLOWED (it is the
		 * session_lowres=0 bisect that reproduces setLRME:5782), but a
		 * misaligned address must be refused here rather than by the
		 * firmware's line-5783 assert.
		 */
		f.low_res_src_addr = IOVA_LOWRES + 1;
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a misaligned LowResSrcLumaScaled address");
		f.low_res_src_addr = IOVA_LOWRES + 32;	/* 32 but not 64 */
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a 32-byte-aligned LowResSrcLumaScaled address");
		f.low_res_src_addr = 0;
		ret = ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f);
		CHECK(ret == (int)want,
		      "builder refused a ZERO LowResSrcLumaScaled (the bisect case): %d",
		      ret);
		if (ret == (int)want && l->low_res_src != AVE_OFF_NONE)
			CHECK(get_unaligned_le64(cmdbuf + base + l->low_res_src) == 0,
			      "a zero LowResSrcLumaScaled did not leave the field zero");
		f.low_res_src_addr = IOVA_LOWRES;

		if (l->src_nbr_max) {
			f.src_nbr[2][0] = 0;
			CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf),
							&c, SESS_PROCESS_SLOT,
							&f) == -EINVAL,
			      "builder accepted a ZERO SrcNbr entry");
			f.src_nbr[2][0] = IOVA_NBR + 0x80000;
			f.src_nbr[2][1] = IOVA_NBR + 1;	/* not 64-aligned */
			CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf),
							&c, SESS_PROCESS_SLOT,
							&f) == -EINVAL,
			      "builder accepted a misaligned SrcNbr entry");
			f.src_nbr[2][1] = IOVA_NBR + 0x90000;
		}
	}

	/* ---- coded-header decoding: the only source of the frame length ---- */
	ctx = "coded-length";
	{
		const struct ave_coded_hdr_layout *c = &abi->coded_hdr;
		static u8 hdr[0x30000];
		struct ave_coded_info info;

		if (!c->slice_stride) {
			printf("  (no coded-header layout for this ABI - skipped)\n");
		} else {
			CHECK(c->min_bytes <= abi->start_avc.coded_hdr_bytes,
			      "coded header needs %#x bytes but Start publishes %#x",
			      c->min_bytes, abi->start_avc.coded_hdr_bytes);
			CHECK(sizeof(hdr) >= c->min_bytes, "test buffer too small");

			memset(hdr, 0, sizeof(hdr));
			put_unaligned_le32(1000, hdr + 0 * c->slice_stride +
					   c->slice_bytes_written);
			hdr[0 * c->slice_stride + c->slice_bytes_removed] = 3;
			put_unaligned_le32(500, hdr + 1 * c->slice_stride +
					   c->slice_bytes_written);
			hdr[1 * c->slice_stride + c->slice_bytes_removed] = 0;
			/* slice 2 left zero: the terminator */
			put_unaligned_le32(3, hdr + c->frame_type);
			put_unaligned_le32(7, hdr + c->frame_num);
			put_unaligned_le32(376, hdr + c->sps_pps_bits);

			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), 0, &info) == 0,
			      "coded_length rejected a well-formed header");
			CHECK(info.bytes == 1497, "length %u want 1497", info.bytes);
			CHECK(info.slices == 2, "slices %u want 2", info.slices);
			CHECK(info.bytes_removed == 3, "trim %u want 3",
			      info.bytes_removed);
			CHECK(info.frame_type == 3, "frame type %u", info.frame_type);
			CHECK(info.frame_num == 7, "frame num %u", info.frame_num);
			CHECK(info.sps_pps_bits == 376, "sps/pps bits %u",
			      info.sps_pps_bits);

			/* Negative control 1: an all-zero header is zero bytes,
			 * not a crash and not a plausible length. */
			memset(hdr, 0, sizeof(hdr));
			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), 0, &info) == 0 &&
			      info.bytes == 0 && info.slices == 0,
			      "an empty header did not decode as zero slices");

			/* Negative control 2: a negative trim is corruption. */
			put_unaligned_le32(64, hdr + c->slice_bytes_written);
			hdr[c->slice_bytes_removed] = 0xff;	/* -1 as s8 */
			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), 0, &info) == -EPROTO,
			      "coded_length accepted a negative trim count");
			hdr[c->slice_bytes_removed] = 0;

			/* Negative control 3: a buffer smaller than the layout
			 * needs must be refused, not read out of bounds. */
			CHECK(ave_cmd_coded_length(abi, hdr, c->min_bytes - 1, 0,
						   &info) == -EINVAL,
			      "coded_length accepted an undersized header buffer");

			/* Negative control 4: a per-slice count that cannot fit
			 * in any buffer is refused. */
			put_unaligned_le32(0x7fffffffu, hdr + c->slice_bytes_written);
			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), 0, &info) == -EPROTO,
			      "coded_length accepted an absurd byte count");
		}
	}

	printf("  size table: Config %#zx Open %#zx Start_AVC %#zx Process %#zx\n",
	       ave_cmd_size(abi, AVE_OP_CONFIG),
	       ave_cmd_size(abi, AVE_OP_OPEN),
	       ave_cmd_size(abi, AVE_OP_START_AVC),
	       ave_cmd_size(abi, AVE_OP_PROCESS_AVC));
}


/*
 * HEVC (docs/77): the opening sequence ave_session.c sends with
 * session_codec=1 - Open and Stop/Close carrying codec 1, HEVC_INIT with the
 * session's HEVC sizes, then IDR + three P HEVC_ENCODEs - rebuilt with the
 * same values, 13.5 only. The 26.6.2 builders must refuse.
 */
/* ave_session.c HEVC sizes at 1280x720 (docs/77 §9 formulas, by hand):
 * entropy 2304 * 40 * max(4, 12); SrcNbr Pixel 768 * 40 * 11 -> 16K;
 * colocated 128 * 40 * 12 -> 4K; SliceHeader 256 * 0x400. */
#define H_ENTROPY_SIZE		1105920u
#define H_NBR_SLOT		344064u
#define H_COLOC_SIZE		61440u
#define H_SLICEHDR_SIZE		0x40000u
#define IOVA_SLICEHDR		0xa0000000ull

static u8 hbuf[0x40000];

static void test_hevc(void)
{
	const struct ave_cmd_abi *abi = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	const struct ave_cmd_abi *a26 = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	struct ave_cmd_ctx c = { .count = 2, .client_id = SESS_CLIENT_ID,
				 .hevc = true };
	struct ave_recon_buf recon[SESS_DPB] = {
		{ IOVA_RECON, 0, IOVA_RECON + 0x300000 },
		{ IOVA_RECON + 0x1000000ull, 0, IOVA_RECON + 0x1300000ull },
	};
	struct ave_buf coded = { .addr = IOVA_CODED, .size = 0x152000 };
	struct ave_buf coded_hdr = { .addr = IOVA_CODEDHDR, .size = 0x23000 };
	static struct ave_hevc_session h;
	static struct ave_hevc_frame f;
	u8 reply[0x80];
	u32 i, j, n;
	int ret;

	printf("HEVC (macOS 13.5):\n");
	ctx = "HEVC Open";
	ret = ave_cmd_build_open(abi, hbuf, sizeof(hbuf), &c);
	CHECK(ret == 0x40, "open ret %d", ret);
	CHECK(get_unaligned_le32(hbuf + abi->hdr.codec) == 1,
	      "Open codec %u, want 1 (docs/77 §1.2)",
	      get_unaligned_le32(hbuf + abi->hdr.codec));

	ctx = "HEVC_INIT";
	memset(&h, 0, sizeof(h));
	h.vp.width = SESS_WIDTH; h.vp.height = SESS_HEIGHT;
	h.vp.frame_rate = 30;
	h.vp.qp_i = h.vp.qp_p = h.vp.qp_b = SESS_QP;
	h.vp.qp_min = 10; h.vp.qp_max = 51;
	h.vp.key_interval = 1;
	h.vp.lambda_block = true;			/* session_lambda default */
	h.vp.fw_client_addr = IOVA_FWCLIENT;
	h.vp.fw_client_size = SESS_FWCLIENT_SIZE;
	h.vp.fw_client_mem_addr = IOVA_FWCLIENTMEM;
	h.vp.fw_client_mem_size = SESS_FWCLIENTMEM_SIZE;
	h.vp.param_sets_addr = IOVA_FWCLIENTMEM + 0x100000;
	h.vp.param_sets_size = 0x1000;
	h.vp.need_lsb_planes = true;
	h.vp.recon = recon; h.vp.n_recon = SESS_DPB;
	h.vp.low_res_ref[0] = IOVA_LOWRES;
	h.vp.low_res_ref[1] = IOVA_LOWRES + SESS_LOWRES_SIZE;
	h.vp.n_low_res_ref = SESS_DPB;
	for (i = 0; i < 4; i++)
		h.vp.low_res_result[i] = IOVA_LOWRES + 0x100000 + 0x10000 * i;
	h.vp.n_low_res_result = 4;
	h.vp.colocated[0] = IOVA_LOWRES + 0x200000;
	h.vp.colocated[1] = IOVA_LOWRES + 0x200000 + H_COLOC_SIZE;
	h.vp.n_colocated = SESS_DPB;
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			h.vp.entropy[i][j] = 0xb0000000ull + (u64)ALIGN_UP(H_ENTROPY_SIZE, 128) * (4 * i + j);
	h.vp.n_entropy = 2; h.vp.n_entropy_cols = 4;
	h.vp.entropy_size = H_ENTROPY_SIZE;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			h.vp.src_nbr[i][j] = IOVA_NBR + (u64)H_NBR_SLOT * (4 * i + j);
	h.vp.n_src_nbr = 4;
	h.vp.coded = &coded; h.vp.coded_hdr = &coded_hdr; h.vp.n_coded = 1;
	h.level_idc = 120;
	h.input_format_word = 16;
	h.max_num_ref_frames = 1;
	h.log2_max_poc_lsb_minus4 = 4;
	h.sao = h.wpp = h.sps_tmvp = true;
	h.n_st_rps = 1;
	/* session_hevc_xc=2 (default): two TranscodedData surfaces of
	 * align4K(coded / 2), docs/77 §14 */
	h.transcoded[0] = 0xc0000000ull;
	h.transcoded[1] = 0xc0200000ull;
	h.n_transcoded = 2;
	h.transcoded_size = 0xa9000;	/* 0x152000 / 2 */
	c.count = 3;
	ret = ave_cmd_build_start_hevc(abi, hbuf, sizeof(hbuf), &c, &h);
	CHECK(ret == (int)ave_cmd_size(abi, AVE_OP_START_HEVC),
	      "start_hevc ret %d", ret);
	CHECK(get_unaligned_le16(hbuf) == 5, "HEVC_INIT id %u", get_unaligned_le16(hbuf));
	CHECK(get_unaligned_le32(hbuf + abi->hdr.slot) == 6, "HEVC_INIT slot");
	CHECK(get_unaligned_le64(hbuf + abi->start_hevc.transcoded_set) == 0xc0000000ull &&
	      get_unaligned_le64(hbuf + abi->start_hevc.transcoded_set +
				 abi->start_hevc.transcoded_stride) == 0xc0200000ull &&
	      get_unaligned_le32(hbuf + abi->start_hevc.transcoded_size) == 0xa9000,
	      "TranscodedData pair and size not published");
	make_reply(abi, AVE_OP_START_HEVC, SESS_CLIENT_ID, abi->reply.status_ok, 0, reply);
	CHECK(ave_cmd_check_reply(abi, AVE_OP_START_HEVC, reply,
				  abi->cmd[AVE_OP_START_HEVC].reply_size,
				  SESS_CLIENT_ID, NULL) == 0,
	      "INIT_DONE (0xE04) refused for HEVC_INIT");
	CHECK(abi->cmd[AVE_OP_START_HEVC].reply_id == 0xe04, "HEVC_INIT reply id");
	CHECK(ave_cmd_build_start_hevc(a26, hbuf, sizeof(hbuf), &c, &h) == -EINVAL,
	      "26.6.2 must refuse HEVC_INIT");

	ctx = "HEVC_ENCODE";
	for (n = 0; n < 4; n++) {
		memset(&f, 0, sizeof(f));
		f.pic.frame_type = n ? AVE_FRAME_TYPE_P : AVE_FRAME_TYPE_IDR;
		f.pic.frame_num = n;
		f.pic.in_luma_addr = IOVA_LUMA;
		f.pic.in_luma_stride = SESS_STRIDE;
		f.pic.in_chroma_addr = IOVA_CHROMA;
		f.pic.in_chroma_stride = SESS_STRIDE;
		f.pic.coded_index = 0;
		f.pic.coded_addr = IOVA_CODED;
		f.pic.coded_hdr_addr = IOVA_CODEDHDR;
		f.pic.coded_size = coded.size;
		f.pic.recon_luma_addr = IOVA_RECON;
		f.pic.recon_chroma_addr = IOVA_RECON + 0x100000;
		f.pic.recon_luma_lsb_addr = IOVA_RECON + 0x200000;
		f.pic.recon_chroma_lsb_addr = IOVA_RECON + 0x300000;
		f.pic.recon_mv_addr = IOVA_RECON + 0x400000;
		f.pic.force_key_frame = !n;
		f.pic.low_res_src_addr = IOVA_LOWRES;
		memcpy(f.pic.entropy, h.vp.entropy, sizeof(f.pic.entropy));
		f.pic.n_entropy = 2; f.pic.n_entropy_cols = 4;
		memcpy(f.pic.src_nbr, h.vp.src_nbr, sizeof(f.pic.src_nbr));
		f.pic.n_src_nbr = 4;
		f.poc_lsb = n & 255;			/* frames since the IDR */
		f.sao = true;
		f.hdr_slot_base = IOVA_SLICEHDR;
		f.hdr_slot_size = H_SLICEHDR_SIZE;
		c.count = 4 + n;
		ret = ave_cmd_build_process_hevc(abi, hbuf, sizeof(hbuf), &c,
						 SESS_PROCESS_SLOT, &f);
		CHECK(ret == 0x6838, "frame %u: process_hevc ret %d", n, ret);
		CHECK(get_unaligned_le16(hbuf) == 8, "HEVC_ENCODE id");
		CHECK(get_unaligned_le32(hbuf + abi->hdr.codec) == 1, "HEVC_ENCODE codec");
		CHECK(get_unaligned_le32(hbuf + abi->process_hevc.slice +
					 abi->process_hevc.sh_poc_lsb) == n,
		      "frame %u: POC lsb", n);
		CHECK(hbuf[abi->process_hevc.st_rps] == (n ? 1 : 0),
		      "frame %u: slice RPS flag", n);
		CHECK(hbuf[abi->process_hevc.picmgmt + abi->process_hevc.pic_single_xc] == 0,
		      "frame %u: two transcoders, PICMGMT+0xF65 must stay 0", n);
		CHECK(get_unaligned_le32(hbuf + abi->process_hevc.slice +
					 abi->process_hevc.sh_seg_limit) == 0xffffffffu &&
		      get_unaligned_le32(hbuf + abi->process_hevc.slice +
					 abi->process_hevc.sh_seg_limit + 4) == 0xffffffffu,
		      "frame %u: S+0x54C/0x550 must be -1 (h2b abort, docs/77 §15)", n);
	}
	/* session_hevc_xc=1: PICMGMT+0xF65 = 1, and HEVC_INIT without the pair. */
	f.single_xc = true;
	ret = ave_cmd_build_process_hevc(abi, hbuf, sizeof(hbuf), &c,
					 SESS_PROCESS_SLOT, &f);
	CHECK(ret == 0x6838 &&
	      hbuf[abi->process_hevc.picmgmt + abi->process_hevc.pic_single_xc] == 1,
	      "single transcoder: PICMGMT+0xF65 = 1 (ret %d)", ret);
	h.n_transcoded = 0;
	ret = ave_cmd_build_start_hevc(abi, hbuf, sizeof(hbuf), &c, &h);
	CHECK(ret == 0x32dc8, "HEVC_INIT with no TranscodedData (ret %d)", ret);
	make_reply(abi, AVE_OP_PROCESS_HEVC, SESS_CLIENT_ID, abi->reply.status_ok, 0, reply);
	CHECK(ave_cmd_check_reply(abi, AVE_OP_PROCESS_HEVC, reply,
				  abi->cmd[AVE_OP_PROCESS_HEVC].reply_size,
				  SESS_CLIENT_ID, NULL) == 0,
	      "ENCODE_DONE refused for HEVC_ENCODE");

	ctx = "HEVC Stop/Close";
	ret = ave_cmd_build_simple(abi, AVE_OP_STOP, hbuf, sizeof(hbuf), &c);
	CHECK(ret == 0x40 && get_unaligned_le32(hbuf + abi->hdr.codec) == 1,
	      "Stop codec 1 (ret %d)", ret);
	ret = ave_cmd_build_close(abi, hbuf, sizeof(hbuf), &c);
	CHECK(ret == 0x48 && get_unaligned_le32(hbuf + abi->hdr.codec) == 1,
	      "Close codec 1 (ret %d)", ret);
}

int main(void)
{
	/* NULL ABI must be handled, not crash (mirrors the driver's guard). */
	ctx = "null-abi";
	CHECK(ave_cmd_abi_get(AVE_ABI_UNKNOWN) == NULL,
	      "ave_cmd_abi_get(UNKNOWN) should be NULL");

	test_abi(AVE_ABI_MACOS_13_5, "macOS 13.5");
	test_abi(AVE_ABI_MACOS_26_6, "macOS 26.6.2");
	test_hevc();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
