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
#define SESS_STRIDE		1280		/* % 64 == 0 */
#define SESS_PROCESS_SLOT	21

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
		struct ave_recon_buf recon = {
			.addr = IOVA_RECON,
			.luma_size = SESS_WIDTH * SESS_HEIGHT,
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
			.recon = &recon, .n_recon = 1,
			.coded = &coded, .coded_hdr = &coded_hdr, .n_coded = 1,
		};
		size_t want = ave_cmd_size(abi, AVE_OP_START_AVC);

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

		f.frame_type = 1;	/* P: not a legal first frame here */
		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						SESS_PROCESS_SLOT, &f) == -EINVAL,
		      "builder accepted a non-I frame type");
		f.frame_type = AVE_FRAME_TYPE_IDR;

		CHECK(ave_cmd_build_process_avc(abi, cmdbuf, sizeof(cmdbuf), &c,
						h->max_slot, &f) == -EINVAL,
		      "builder accepted slot == max_slot");

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

			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), &info) == 0,
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
			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), &info) == 0 &&
			      info.bytes == 0 && info.slices == 0,
			      "an empty header did not decode as zero slices");

			/* Negative control 2: a negative trim is corruption. */
			put_unaligned_le32(64, hdr + c->slice_bytes_written);
			hdr[c->slice_bytes_removed] = 0xff;	/* -1 as s8 */
			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), &info) == -EPROTO,
			      "coded_length accepted a negative trim count");
			hdr[c->slice_bytes_removed] = 0;

			/* Negative control 3: a buffer smaller than the layout
			 * needs must be refused, not read out of bounds. */
			CHECK(ave_cmd_coded_length(abi, hdr, c->min_bytes - 1,
						   &info) == -EINVAL,
			      "coded_length accepted an undersized header buffer");

			/* Negative control 4: a per-slice count that cannot fit
			 * in any buffer is refused. */
			put_unaligned_le32(0x7fffffffu, hdr + c->slice_bytes_written);
			CHECK(ave_cmd_coded_length(abi, hdr, sizeof(hdr), &info) == -EPROTO,
			      "coded_length accepted an absurd byte count");
		}
	}

	printf("  size table: Config %#zx Open %#zx Start_AVC %#zx Process %#zx\n",
	       ave_cmd_size(abi, AVE_OP_CONFIG),
	       ave_cmd_size(abi, AVE_OP_OPEN),
	       ave_cmd_size(abi, AVE_OP_START_AVC),
	       ave_cmd_size(abi, AVE_OP_PROCESS_AVC));
}

int main(void)
{
	/* NULL ABI must be handled, not crash (mirrors the driver's guard). */
	ctx = "null-abi";
	CHECK(ave_cmd_abi_get(AVE_ABI_UNKNOWN) == NULL,
	      "ave_cmd_abi_get(UNKNOWN) should be NULL");

	test_abi(AVE_ABI_MACOS_13_5, "macOS 13.5");
	test_abi(AVE_ABI_MACOS_26_6, "macOS 26.6.2");

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
