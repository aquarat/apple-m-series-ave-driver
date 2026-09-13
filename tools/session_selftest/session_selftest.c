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

	printf("  size table: Config %#zx Open %#zx Start_AVC %#zx\n",
	       ave_cmd_size(abi, AVE_OP_CONFIG),
	       ave_cmd_size(abi, AVE_OP_OPEN),
	       ave_cmd_size(abi, AVE_OP_START_AVC));
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
