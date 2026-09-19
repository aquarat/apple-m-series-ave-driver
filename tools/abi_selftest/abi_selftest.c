// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace self-test for driver/ave_cmd.c.
 *
 * Builds every command for both firmware ABIs and checks sizes, offsets and
 * values against the evidence tables in docs/46 and docs/47 (macOS 13.5) and
 * docs/07, 20, 21, 32, 35, 37, 38 (macOS 26.6.2).
 *
 * Every expected offset and value below is TYPED IN from those documents, with
 * the binary address that proves it. Nothing is computed from ave_abi.h -
 * that would only test that the tables agree with themselves.
 *
 * After the field checks, every byte of each command that no check covered
 * must be zero, so a field written at a wrong offset is caught even when no
 * check names that offset. That scan is itself validated against a negative
 * control (a planted stray byte must be reported).
 *
 * Pure computation. Touches no hardware.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kshim.h"
#include "../../driver/ave_cmd.h"

#define MAXCMD	0x20000

static int failures, checks;
static u8 covered[MAXCMD];
static const char *ctx_name;

#define FAIL(...) do { failures++; printf("  FAIL [%s] ", ctx_name); \
			printf(__VA_ARGS__); printf("\n"); } while (0)

static void begin(const char *name)
{
	ctx_name = name;
	memset(covered, 0, sizeof(covered));
}

static void expect_val(const u8 *buf, u32 off, int width, u64 want,
		       const char *what)
{
	u64 got = 0;
	int i;

	checks++;
	for (i = 0; i < width; i++) {
		got |= (u64)buf[off + i] << (8 * i);
		covered[off + i] = 1;
	}
	if (got != want)
		FAIL("%s @0x%x: got 0x%llx want 0x%llx", what, off,
		     (unsigned long long)got, (unsigned long long)want);
}

#define E8(b, o, v, w)	expect_val(b, o, 1, v, w)
#define E16(b, o, v, w)	expect_val(b, o, 2, v, w)
#define E32(b, o, v, w)	expect_val(b, o, 4, v, w)
#define E64(b, o, v, w)	expect_val(b, o, 8, v, w)

static void expect_int(long got, long want, const char *what)
{
	checks++;
	if (got != want)
		FAIL("%s: got %ld want %ld", what, got, want);
}

/* Returns the number of non-zero bytes nobody checked. */
static int stray_bytes(const u8 *buf, u32 size, bool report)
{
	int n = 0;
	u32 i;

	for (i = 0; i < size; i++) {
		if (buf[i] && !covered[i]) {
			if (report && n < 8)
				printf("    stray byte @0x%x = 0x%02x\n", i, buf[i]);
			n++;
		}
	}
	return n;
}

static void expect_rest_zero(const u8 *buf, u32 size)
{
	checks++;
	if (stray_bytes(buf, size, true))
		FAIL("unchecked non-zero bytes in a 0x%x command", size);
}

static u8 buf[MAXCMD];

static const struct ave_cmd_ctx CTX = {
	.count = 0x1122334455667788ull,
	.client_id = 0x0000000a,
	.timeout = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
		     0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf },
};

static void expect_timeout(u32 off)
{
	int i;

	for (i = 0; i < 16; i++)
		E8(buf, off + i, 0xa0 + i, "timeout");
}

/* ------------------------------------------------------------------------ */
/* Headers                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 13.5 header, docs/46 §2: u16 id +0 (fw ldrh 0xd65c); u64 CNT +8 (kext
 * 0xfffffe0008ea9168); u32 CID +0x10 (fw 0xe8e0); codec +0x18, 0 = AVC (fw
 * 0x13e60); slot +0x1C (kext 0xfffffe0008ea9188); priority +0x20 (fw 0xee7c);
 * timeout 16 B at +0x28 (kext 0xfffffe0008ea9190).
 */
static void expect_hdr_13_5(u16 id, u32 cid, u32 slot, u32 prio)
{
	E16(buf, 0x00, id, "13.5 id");
	E64(buf, 0x08, 0x1122334455667788ull, "13.5 CNT");
	E32(buf, 0x10, cid, "13.5 CID u32");
	E32(buf, 0x18, 0, "13.5 codec AVC=0");
	E32(buf, 0x1c, slot, "13.5 slot");
	E32(buf, 0x20, prio, "13.5 priority");
	expect_timeout(0x28);
}

/*
 * 26.6.2 header, docs/07 §4: u64 CID +0x10, client type +0x18 (Enc = 1),
 * enc type +0x1C (AVC = 1), slot +0x20, priority +0x24, timeout +0x30.
 */
static void expect_hdr_26_6(u16 id, u64 cid, u32 type, u32 enc, u32 slot,
			    u32 prio)
{
	E16(buf, 0x00, id, "26.6 id");
	E64(buf, 0x08, 0x1122334455667788ull, "26.6 CNT");
	E64(buf, 0x10, cid, "26.6 CID u64");
	E32(buf, 0x18, type, "26.6 client type");
	E32(buf, 0x1c, enc, "26.6 enc type");
	E32(buf, 0x20, slot, "26.6 slot");
	E32(buf, 0x24, prio, "26.6 priority");
	expect_timeout(0x30);
}

/* ------------------------------------------------------------------------ */

static void test_simple(void)
{
	const struct ave_cmd_abi *a13 = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	const struct ave_cmd_abi *a26 = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	/*
	 * { op, 13.5 id, 13.5 size, 13.5 slot, 26.6 id, 26.6 size, 26.6 slot }
	 * 13.5: docs/46 §1.1 size checks (fw VA), §1.2 slot literals.
	 * 26.6.2: docs/07 §2-§4.
	 */
	static const struct {
		enum ave_op op; const char *name;
		u16 id13; u32 sz13; u32 slot13; u32 prio13;
		u16 id26; u32 sz26; u32 slot26; u32 prio26;
	} t[] = {
		/* Open: 13.5 START id 2 0x40 fw 0xd794, {3,200} 0xfffffe000722f210 */
		{ AVE_OP_OPEN, "open", 2, 0x40, 3, 200, 3, 0x48, 3, 200 },
		/* Close: 13.5 STOP id 12 0x48 fw 0xdce8, {4,200} */
		{ AVE_OP_CLOSE, "close", 12, 0x48, 4, 200, 4, 0x48, 4, 200 },
		/* Stop: 13.5 UNINIT id 6 0x40 fw 0xda0c, {7,200} */
		{ AVE_OP_STOP, "stop", 6, 0x40, 7, 200, 7, 0x48, 7, 200 },
		/* Complete: 13.5 id 13 0x40 fw 0xdd78, slot 9 0xfffffe0008ead1e0 */
		{ AVE_OP_COMPLETE, "complete", 13, 0x40, 9, 200, 9, 0x48, 9, 200 },
		/* Flush: 13.5 id 11 0x40 fw 0xdc58, slot 10 0xfffffe0008ead5cc */
		{ AVE_OP_FLUSH, "flush", 11, 0x40, 10, 200, 11, 0x48, 11, 200 },
		/*
		 * Halt: 13.5 POWERDOWN id 14 0x40 fw 0xde08, slot ~0 prio 0.
		 * Independently confirmed from the kext side in docs/55: the
		 * 16 bytes MakeFwCmd_Halt stores at +0x10 are the literal at
		 * 0xfffffe000723ccc0, twelve zeros then ff ff ff ff - client
		 * 0, codec 0, slot ~0 - which is exactly this row.
		 */
		{ AVE_OP_HALT, "halt", 14, 0x40, 0xffffffff, 0, 2, 0x48, 0xffffffff, 0 },
	};
	char name[64];
	size_t i;
	int ret;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		bool global = t[i].slot13 == 0xffffffff;

		snprintf(name, sizeof(name), "13.5 %s", t[i].name);
		begin(name);
		memset(buf, 0x5a, sizeof(buf));
		ret = ave_cmd_build_simple(a13, t[i].op, buf, sizeof(buf), &CTX);
		expect_int(ret, t[i].sz13, "size");
		if (global) {
			E16(buf, 0, t[i].id13, "id");
			E64(buf, 8, CTX.count, "CNT");
			E32(buf, 0x1c, t[i].slot13, "slot");
			E32(buf, 0x20, 0, "prio");
			expect_timeout(0x28);
		} else {
			expect_hdr_13_5(t[i].id13, 0xa, t[i].slot13, t[i].prio13);
		}
		expect_rest_zero(buf, t[i].sz13);
		E8(buf, t[i].sz13, 0x5a, "no write past the command");

		snprintf(name, sizeof(name), "26.6 %s", t[i].name);
		begin(name);
		memset(buf, 0x5a, sizeof(buf));
		ret = ave_cmd_build_simple(a26, t[i].op, buf, sizeof(buf), &CTX);
		expect_int(ret, t[i].sz26, "size");
		if (global) {
			E16(buf, 0, t[i].id26, "id");
			E64(buf, 8, CTX.count, "CNT");
			E32(buf, 0x20, t[i].slot26, "slot");
			E32(buf, 0x24, 0, "prio");
			expect_timeout(0x30);
		} else {
			expect_hdr_26_6(t[i].id26, 0xa, 1, 1, t[i].slot26, t[i].prio26);
		}
		expect_rest_zero(buf, t[i].sz26);
		E8(buf, t[i].sz26, 0x5a, "no write past the command");
	}

	begin("simple: size and op checks");
	expect_int(ave_cmd_build_open(a13, buf, 0x3f, &CTX), -EINVAL, "13.5 open len 0x3f");
	expect_int(ave_cmd_build_open(a26, buf, 0x47, &CTX), -EINVAL, "26.6 open len 0x47");
	expect_int(ave_cmd_build_close(a13, buf, 0x47, &CTX), -EINVAL, "13.5 close len 0x47");
	expect_int(ave_cmd_build_simple(a13, AVE_OP_PRIORITY, buf, sizeof(buf), &CTX),
		   -EINVAL, "13.5 has no Priority (docs/46 §1.2)");
	expect_int(ave_cmd_build_simple(a26, AVE_OP_START_AVC, buf, sizeof(buf), &CTX),
		   -EINVAL, "Start is not a simple command");
	{
		struct ave_cmd_ctx big = CTX;

		big.client_id = 0x100000000ull;
		expect_int(ave_cmd_build_open(a13, buf, sizeof(buf), &big), -EINVAL,
			   "13.5 CID is u32 (RegisterClient(unsigned int) 0x17738)");
		expect_int(ave_cmd_build_open(a26, buf, sizeof(buf), &big), 0x48,
			   "26.6 CID is u64");
	}
	expect_int(ave_cmd_abi_get(AVE_ABI_UNKNOWN) == NULL, 1, "unknown ABI -> NULL");
	expect_int(ave_cmd_size(a13, AVE_OP_START_HEVC), 0x32dc8, "13.5 HEVC_INIT fw 0xd970");
	expect_int(ave_cmd_size(a26, AVE_OP_START_HEVC), 0x13f28, "26.6 HEVC start");
	expect_int(ave_cmd_size(a13, AVE_OP_RESET), 0x32db0, "13.5 RESET fw 0xd824");
	expect_int(ave_cmd_size(a26, AVE_OP_RESET), 0x13f08, "26.6 Reset");
	expect_int(ave_cmd_size(a13, AVE_OP_PROCESS_HEVC), 0x6838, "13.5 HEVC_ENCODE fw 0xdb30");
	expect_int(ave_cmd_size(a26, AVE_OP_PROCESS_HEVC), 0xb1c0, "26.6 HEVC process");
}

/* ------------------------------------------------------------------------ */

static void test_config(void)
{
	const struct ave_cmd_abi *a13 = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	const struct ave_cmd_abi *a26 = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	struct ave_config_params p = {
		.skip_mcpu = false,
		.create_mcpu = true,
		.reg_dart_addr = 0x0000000123456000ull,
		.doorbell_cadence = { 0x11, 0x22 },
		.dsid = 0x5c,
		.shmem_addr = 0x0000000fedc00000ull,
		.shmem_size = 0x10000,
	};
	int ret;

	/* 13.5 ProcessConfig 0xe4d4, docs/46 §3 */
	begin("13.5 config");
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_config(a13, buf, sizeof(buf), &CTX, &p);
	expect_int(ret, 0x70, "size (fw assert 0xd704)");
	E16(buf, 0x00, 1, "id CONFIG");
	E64(buf, 0x08, CTX.count, "CNT");
	E32(buf, 0x10, 0, "CID 0 (q literal 0xfffffe000723ccc0)");
	E32(buf, 0x14, 0, "+0x14 0");
	E32(buf, 0x18, 0, "codec 0");
	E32(buf, 0x1c, 0xffffffff, "slot ~0");
	E32(buf, 0x20, 0, "prio 0 (str wzr 0xfffffe0008efadc4)");
	expect_timeout(0x28);
	E8(buf, 0x40, 0, "bSkipMcpu (fw ldrb [x23,#64] 0xe550)");
	E8(buf, 0x41, 1, "bCreateMcpu (fw ldrb [x23,#65] 0xe54c)");
	E64(buf, 0x48, 0x0000000123456000ull, "reg DART addr (fw 0xe554)");
	E32(buf, 0x50, 0x11, "cadence0 (fw 0xe574)");
	E32(buf, 0x54, 0x22, "cadence1 (fw 0xe584)");
	E8(buf, 0x58, 0x5c, "DSID u8 (fw 0xe560)");
	E8(buf, 0x59, 0x5c, "DSID u8 (fw 0xe564)");
	E64(buf, 0x60, 0x0000000fedc00000ull, "shmem IOVA (fw 0xe5dc)");
	E32(buf, 0x68, 0x10000, "shmem size (fw 0xe5e0)");
	expect_rest_zero(buf, 0x70);

	/* 26.6.2 docs/20 §2 */
	begin("26.6 config");
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_config(a26, buf, sizeof(buf), &CTX, &p);
	expect_int(ret, 0x78, "size");
	E16(buf, 0x00, 1, "id");
	E64(buf, 0x08, CTX.count, "CNT");
	E32(buf, 0x18, 0, "client type 0");
	E32(buf, 0x20, 0xffffffff, "slot ~0");
	E32(buf, 0x24, 0, "prio 0");
	expect_timeout(0x30);
	E8(buf, 0x48, 0, "bSkipMcpu (fw 0x29118)");
	E8(buf, 0x49, 1, "bCreateMcpu (fw 0x2910c)");
	E32(buf, 0x58, 0x11, "cadence0 (fw 0x29128)");
	E32(buf, 0x5c, 0x22, "cadence1 (fw 0x29134)");
	E32(buf, 0x60, 0x5c, "MCC DSID u32 (fw 0x29120)");
	E64(buf, 0x68, 0x0000000fedc00000ull, "shmem IOVA (fw 0x291d8)");
	E32(buf, 0x70, 0x10000, "shmem size (fw 0x291dc)");
	expect_rest_zero(buf, 0x78);

	begin("config negatives");
	p.dsid = 0x100;
	expect_int(ave_cmd_build_config(a13, buf, sizeof(buf), &CTX, &p), -EINVAL,
		   "13.5 DSID is u8");
	expect_int(ave_cmd_build_config(a26, buf, sizeof(buf), &CTX, &p), 0x78,
		   "26.6 DSID is u32");
	p.dsid = 1;
	p.shmem_size = 19183;
	expect_int(ave_cmd_build_config(a13, buf, sizeof(buf), &CTX, &p), -EINVAL,
		   "13.5 shmem < 4*(2744+2048+4) (fw 0xe5f0)");
	expect_int(ave_cmd_build_config(a26, buf, sizeof(buf), &CTX, &p), 0x78,
		   "26.6 shmem >= 4*(1176+2048+4)");
	p.shmem_size = 0x10000;
	expect_int(ave_cmd_build_config(a13, buf, 0x6f, &CTX, &p), -EINVAL, "13.5 len 0x6f");
	expect_int(ave_cmd_build_config(a26, buf, 0x77, &CTX, &p), -EINVAL, "26.6 len 0x77");
}

/* ------------------------------------------------------------------------ */

static const struct ave_recon_buf RECON[2] = {
	{ 0x0000000200000000ull, 0x001fe000, 0 },
	{ 0x0000000200400000ull, 0x001fe000, 0 },
};
static const struct ave_buf CODED[2] = {
	{ 0x0000000300000000ull, 0x2f8000 },
	{ 0x0000000300400000ull, 0x2f8000 },
};
static const struct ave_buf CODED_HDR_13[2] = {
	{ 0x0000000310000000ull, 0x23000 },
	{ 0x0000000310100000ull, 0x23000 },
};
static const struct ave_buf CODED_HDR_26[2] = {
	{ 0x0000000310000000ull, 0xc000 },
	{ 0x0000000310100000ull, 0xc000 },
};

static struct ave_avc_session session_1080p(bool v13)
{
	struct ave_avc_session s = {
		.width = 1920, .height = 1080,
		.frame_rate = 30,
		.bitrate = 0,
		.qp_i = 26, .qp_p = 27, .qp_b = 28,
		.key_interval = 1,
		.profile_idc = 100, .level_idc = 40,
		.cabac = true,
		.fw_client_addr = 0x0000000400000000ull, .fw_client_size = 0x100000,
		.fw_client_mem_addr = 0x0000000400200000ull, .fw_client_mem_size = 0x10000,
		/* SPS/PPS output buffer; required wherever the ABI has the field */
		.param_sets_addr = 0x0000000400300000ull, .param_sets_size = 0x1000,
		.recon = RECON, .n_recon = 2,
		.coded = CODED, .coded_hdr = v13 ? CODED_HDR_13 : CODED_HDR_26,
		.n_coded = 2,
	};
	return s;
}

static void test_start_13_5(void)
{
	const struct ave_cmd_abi *a = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	struct ave_avc_session s = session_1080p(true);
	int ret;

	begin("13.5 start_avc");
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s);
	expect_int(ret, 0x10e10, "size (fw 0xd8bc)");
	/* id 4 AVC_INIT, {6,200} literal 0xfffffe000722f220 */
	expect_hdr_13_5(4, 0xa, 6, 200);

	/* docs/46 §9.1 */
	E64(buf, 0x40, 0x0000000400000000ull, "FwClient IOVA (fw 0xee2c)");
	E32(buf, 0x48, 0x100000, "FwClient size (fw 0xee40)");
	E64(buf, 0x50, 0x0000000400200000ull, "FwClientMem (fw 0x463f0)");
	E32(buf, 0x58, 0x10000, "FwClientMem size");
	/* docs/52: fw ldr x20,[x23,#880] 0x5df28 / ldr w2,[x23,#888] 0x5de44 */
	E64(buf, 0xfb30, 0x0000000400300000ull, "ParameterSetsBuffer (fw 0x5df28)");
	E32(buf, 0xfb38, 0x1000, "ParameterSetsBufferSize (fw 0x5de44)");
	/* sSVEMap.iNum: single core, closes CollectDataFromCpus:8657, docs/54 */
	E32(buf, 0x10de8, 1, "sSVEMap.iNum = 1");
	/* docs/46 §9.2, docs/38 §7: MB-aligned in VideoParams */
	E32(buf, 0x60, 1920, "width (fw 0x5ced0)");
	E32(buf, 0x64, 1088, "height (fw 0x5ced0)");
	/* DPB: 16-byte entries at 0x88 (fw 0x5d6ac, stride 0x10) */
	E64(buf, 0x88, 0x0000000200000000ull, "recon[0] (fw [x20,#40] 0x5d6ac)");
	E64(buf, 0x98, 0x0000000200400000ull, "recon[1] (fw [x20,#56] 0x5d6b4)");
	/* coded data: addr[20] 0x4b8, size[20] 0x558, hdr addr 0x5c0 */
	E64(buf, 0x4b8, 0x0000000300000000ull, "coded addr[0] (fw 0x5d4c8)");
	E64(buf, 0x4c0, 0x0000000300400000ull, "coded addr[1] (fw 0x5d4e4)");
	E32(buf, 0x558, 0x2f8000, "coded size[0] (fw 0x5d4d4)");
	E32(buf, 0x55c, 0x2f8000, "coded size[1] (fw 0x5d4ec)");
	E64(buf, 0x5c0, 0x0000000310000000ull, "coded hdr[0] (fw 0x5d4dc)");
	E64(buf, 0x5c8, 0x0000000310100000ull, "coded hdr[1] (fw 0x5d4f4)");
	E32(buf, 0x660, 0x23000, "coded hdr size[0] (kext 0xfffffe0008eaeed4)");
	E32(buf, 0x664, 0x23000, "coded hdr size[1]");
	E32(buf, 0xfdac, 1, "sSliceMap.iNum (fw 0x14414, inferred)");
	/* AVE_FW_RC_PARAMS, docs/47 §2 */
	E32(buf, 0xff34, 1, "ui32IdrPeriod (fw 0x5d9e8)");
	E32(buf, 0xff4c, 30, "ExpectedFrameRate (fw 0x5d9c4)");
	E32(buf, 0xff24, 1, "iNumViews (kext 0xec9078: 0 < n <= 2)");
	E32(buf, 0xff28, 1, "iNumViews second range check");
	E32(buf, 0xff50, 2, "ui32RCFlag = AVE_RC_FIXQP (fw 0x5ceb4, 0x41158)");
	E32(buf, 0xff88, 0, "QP min (fw 0x5d9f0)");
	E32(buf, 0xff8c, 51, "QP max (fw 0x5da00)");
	E32(buf, 0xffb4, 26, "QP I (fw 0x5cebc)");
	E32(buf, 0xffb8, 27, "QP P (fw 0x5cebc)");
	E32(buf, 0xffbc, 28, "QP B (fw 0x5cec8)");
	/* SPS params, docs/46 §9.2 and the SPS writer 0x194e4.. */
	E32(buf, 0x105b4, 100, "profile_idc raw (u(8) 0x194e4)");
	E32(buf, 0x105d0, 40, "level_idc raw (u(8) 0x19564)");
	E32(buf, 0x105d4, 0, "seq_parameter_set_id (0x19570)");
	E32(buf, 0x105d8, 1, "chroma_format_idc (0x195ac)");
	E32(buf, 0x105e0, 0, "bit_depth_luma_minus8 (0x195dc)");
	E32(buf, 0x105e4, 0, "bit_depth_chroma_minus8 (0x195e8)");
	E32(buf, 0x109cc, 0, "log2_max_frame_num_minus4 (0x197c4)");
	E32(buf, 0x109d0, 2, "pic_order_cnt_type (0x197d0)");
	E32(buf, 0x109dc, 1, "max_num_ref_frames (0x19860)");
	E8(buf, 0x109e0, 0, "gaps_in_frame_num (0x19870)");
	E32(buf, 0x109e4, 119, "pic_width_in_mbs_minus1 (0x1987c)");
	E32(buf, 0x109e8, 67, "pic_height_in_map_units_minus1 (0x19888)");
	E32(buf, 0x109ec, 1, "frame_mbs_only_flag (0x19898)");
	E32(buf, 0x109f0, 1, "direct_8x8_inference_flag (0x198c4)");
	E8(buf, 0x109f4, 0, "vui_parameters_present_flag (0x19924)");
	E8(buf, 0x10a40, 1, "frame_cropping_flag (0x198d8)");
	E32(buf, 0x10a44, 0, "crop left (0x198f0)");
	E32(buf, 0x10a48, 0, "crop right (0x198fc)");
	E32(buf, 0x10a4c, 0, "crop top (0x19908)");
	E32(buf, 0x10a50, 4, "crop bottom (0x19914)");
	E8(buf, 0x10a54, 1, "SPS bFWCreatesHeader (fw 0x5dd64)");
	E32(buf, 0x10a58, 0, "SPS header_len (0x19974)");
	/* PPS params, PPS writer 0x199c4.. */
	E32(buf, 0x10c60, 0, "pic_parameter_set_id (0x199c4)");
	E32(buf, 0x10c64, 0, "pps seq_parameter_set_id (0x199d0)");
	E32(buf, 0x10c68, 1, "entropy_coding_mode_flag (0x199e0)");
	E32(buf, 0x10c70, 0, "num_slice_groups_minus1 (0x199fc)");
	E32(buf, 0x10c88, 0, "pic_init_qp_minus26 (0x19b10)");
	E8(buf, 0x10cb0, 1, "deblocking_filter_control_present (0x19b38)");
	E8(buf, 0x10cb1, 0, "constrained_intra_pred (0x19b48)");
	E8(buf, 0x10cb3, 1, "transform_8x8_mode_flag (0x19b64)");
	E8(buf, 0x10cd8, 1, "PPS bFWCreatesHeader (fw 0x5dd68)");
	expect_rest_zero(buf, 0x10e10);

	/*
	 * NEED_LSB_PLANES (docs/57): u8 1 at wire 0xFD7D (fw 0x5d08c ->
	 * this+0x24058) and each recon entry's second u64 = the LSB plane.
	 */
	begin("13.5 start_avc need_lsb_planes");
	{
		static struct ave_recon_buf rl[2];

		s = session_1080p(true);
		rl[0] = RECON[0]; rl[0].lsb_addr = 0x0000000200800000ull;
		rl[1] = RECON[1]; rl[1].lsb_addr = 0x0000000200c00000ull;
		s.recon = rl;
		s.need_lsb_planes = true;
		memset(buf, 0, sizeof(buf));
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   0x10e10, "size unchanged");
		E8(buf, 0xfd7d, 1, "NEED_LSB_PLANES (fw 0x5d08c)");
		E64(buf, 0x88, RECON[0].addr, "recon[0] MSB unchanged");
		E64(buf, 0x90, rl[0].lsb_addr, "recon[0] LSB at entry+8");
		E64(buf, 0x98, RECON[1].addr, "recon[1] MSB unchanged");
		E64(buf, 0xa0, rl[1].lsb_addr, "recon[1] LSB at entry+8");

		rl[1].lsb_addr = 0;
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   -EINVAL, "an LSB plane missing");
		rl[1].lsb_addr = 0x0000000200c00040ull;
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   -EINVAL, "LSB plane & 127 (fw 0x54f94)");
		s.need_lsb_planes = false;
		memset(buf, 0, sizeof(buf));
		ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s);
		E8(buf, 0xfd7d, 0, "flag off: 0xFD7D stays 0");
		E64(buf, 0x90, 0, "flag off: no LSB written");
		s.recon = RECON;
	}

	/*
	 * The source-path scalars and iNumViews (docs/62 §6). iNumViews is
	 * unconditional - Apple's own validator rejects zero - while the two
	 * source scalars are written only when a sweep asks for them, so the
	 * default image must be byte-identical to what F17 sent.
	 */
	begin("13.5 start_avc source path");
	s = session_1080p(true);
	memset(buf, 0, sizeof(buf));
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
		   0x10e10, "size unchanged");
	E32(buf, 0xff24, 1, "iNumViews (kext 0xec9078 rejects 0)");
	E32(buf, 0xff28, 1, "iNumViews second range check");
	E16(buf, 0xfec0, 0, "src_mode unset stays 0 (as every run to F17)");
	E8(buf, 0xfce8, 0, "src_cfg_byte unset stays 0");

	s.src_mode = 0x15;
	s.src_cfg_byte = 0x03;
	memset(buf, 0, sizeof(buf));
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
		   0x10e10, "size unchanged with a sweep value");
	E16(buf, 0xfec0, 0x15, "src_mode (fw 0x5d018: &3 -> 0x...050, >>2 -> 0x...0D0)");
	E8(buf, 0xfce8, 0x03, "src_cfg_byte (fw 0x5d118 -> 0x40D12000C bits 16+)");
	E8(buf, 0xfec2, 0, "src_mode is a u16, not wider");
	s.src_mode = 0;
	s.src_cfg_byte = 0;

	/* Colocated MV table at wire 0xF6B0, stride 8 (docs/53 13.2, docs/60). */
	begin("13.5 start_avc colocated");
	s = session_1080p(true);
	s.n_colocated = 2;
	s.colocated[0] = 0x0000000210000000ull;
	s.colocated[1] = 0x0000000210080000ull;
	memset(buf, 0, sizeof(buf));
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), 0x10e10, "size unchanged");
	E64(buf, 0xf6b0, s.colocated[0], "colocated[0] (VideoParams+0xF650)");
	E64(buf, 0xf6b8, s.colocated[1], "colocated[1]");
	E64(buf, 0xf6c0, 0, "slot 2 left zero");
	s.colocated[1] = 0;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL, "a zero colocated entry");
	s.colocated[1] = 0x0000000210080020ull;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL, "colocated & 63");
	s.colocated[1] = 0x0000000210080000ull;
	s.n_colocated = 1;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL, "count != n_recon");

	/*
	 * encoder_addr_entropy as a START-time table at wire 0xF830 + 32i + 8j
	 * (docs/61 10: ProvideReferenceFrames copies VP+0xF770+96+8n, fw
	 * 0x2ba98..0x2bc8c; setRefPointers then rebuilds the per-frame copy from
	 * it every frame). Rows 0..3, all four columns.
	 */
	begin("13.5 start_avc entropy table");
	s = session_1080p(true);
	{
		u32 i3, j3;

		for (i3 = 0; i3 < 4; i3++)
			for (j3 = 0; j3 < 4; j3++)
				s.entropy[i3][j3] = 0x0000000800000000ull +
						    0x100000ull * (4 * i3 + j3);
		s.n_entropy = 4;
		s.n_entropy_cols = 4;
		memset(buf, 0, sizeof(buf));
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   0x10e10, "size unchanged");
		for (i3 = 0; i3 < 4; i3++)
			for (j3 = 0; j3 < 4; j3++)
				E64(buf, 0xf830 + 32 * i3 + 8 * j3,
				    s.entropy[i3][j3], "entropy[i][j] at 0xF830");
		E64(buf, 0xf830 + 32 * 4, 0, "row 4 left zero");
		/*
		 * The size table sits immediately after the address table, at
		 * 0xFA30 + 16i + 4j (INFERRED: the firmware's own ctrl+0xEC0 and
		 * ctrl+0x10C0 are 0x200 apart, and 0xFA30 + 0x100 = 0xFB30 =
		 * param_sets_addr, so the gap is exactly one u32[16][4]).
		 */
		s.entropy_size = 0xf0000;
		memset(buf, 0, sizeof(buf));
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   0x10e10, "size unchanged with the size table");
		for (i3 = 0; i3 < 4; i3++)
			for (j3 = 0; j3 < 4; j3++) {
				E64(buf, 0xf830 + 32 * i3 + 8 * j3,
				    s.entropy[i3][j3], "entropy addr");
				E32(buf, 0xfa30 + 16 * i3 + 4 * j3, 0xf0000,
				    "entropy size at 0xFA30 + 16i + 4j");
			}
		E32(buf, 0xfa30 + 16 * 4, 0, "size row 4 left zero");
		E64(buf, 0xfb30, s.param_sets_addr, "param_sets still at 0xFB30");
		s.entropy_size = 0;
		s.entropy[1][2] = 0;
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   -EINVAL, "a hole in the start-time matrix");
		s.entropy[1][2] = 0x0000000800600020ull;
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   -EINVAL, "entropy & 63 (assert :8021)");
		s.entropy[1][2] = 0x0000000800600000ull;
		s.n_entropy = 5;
		expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s),
			   -EINVAL, "more rows than setPipe reads");
	}

	/* SrcNeighbor group 3 (FwData) is at wire 0xFED0, not beside the rest. */
	begin("13.5 start_avc src_nbr group 3");
	s = session_1080p(true);
	memset(buf, 0, sizeof(buf));
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), 0x10e10, "size");
	E64(buf, 0xf7d0, s.src_nbr[0][0], "SrcNbr Info[0] at 0xF7D0");
	E64(buf, 0xf7f0, s.src_nbr[1][0], "SrcNbr Pixel[0] at 0xF7F0");
	E64(buf, 0xf810, s.src_nbr[2][0], "SrcNbr Data[0] at 0xF810");
	E64(buf, 0xfed0, s.src_nbr[3][0], "SrcNbr FwData[0] at 0xFED0 (docs/61 10)");
	E64(buf, 0xf830, 0, "0xF830 is entropy row 0, not FwData");

	begin("13.5 start_avc negatives");
	s = session_1080p(true);
	expect_int(ave_cmd_build_start_avc(a, buf, 0x10e0f, &CTX, &s), -EINVAL, "len short by 1");
	s.n_recon = 17;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "17 recon: fw loop reads 16 (0x5d6ac-0x5d728)");
	s = session_1080p(true);
	s.n_coded = 21;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "21 coded: cap 20 (kext 0xfffffe0008ea4ce8)");
	s = session_1080p(false);	/* 0xc000 coded headers */
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "coded header 0xc000 < 13.5 0x23000 (kext 0xfffffe0008ea4fb8)");
	s = session_1080p(true);
	s.fw_client_mem_addr = 0;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "FwClientMem 0 (assert 0x5cb68)");
	s = session_1080p(true);
	s.frame_rate = 0;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "frame rate 0");
}

static void test_start_26_6(void)
{
	const struct ave_cmd_abi *a = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	struct ave_avc_session s = session_1080p(false);
	int ret;

	begin("26.6 start_avc");
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s);
	expect_int(ret, 0x3180, "size (docs/07 §3)");
	expect_hdr_26_6(6, 0xa, 1, 1, 6, 200);	/* docs/37 §1 */
	/* docs/21 §4 */
	E64(buf, 0x48, 0x0000000400000000ull, "FwClient IOVA (fw 0x297d8)");
	E32(buf, 0x50, 0x100000, "FwClient size (fw 0x297dc)");
	E64(buf, 0x58, 0x0000000400200000ull, "FwClientMem");
	E32(buf, 0x60, 0x10000, "FwClientMem size");
	/* docs/20 §3.3, docs/35 §9 */
	E32(buf, 0x220, 30, "sComm.FrameRate");
	E64(buf, 0x228, 0x80000000ull, "sRC.Feature bit 31 (fw 0x6d36c)");
	E32(buf, 0x234, 3, "sRC.RCMode = const QP");
	E32(buf, 0x238, 0, "Bitrate");
	E32(buf, 0x240, 26, "QP I");
	E32(buf, 0x244, 27, "QP P");
	E32(buf, 0x248, 28, "QP B");
	E32(buf, 0x298, 0, "RCQPRange min");
	E32(buf, 0x29c, 51, "RCQPRange max");
	E32(buf, 0x2b8, 1, "MaxKeyFrameInterval");
	E32(buf, 0x2bc, 1, "StrictKeyFrameInterval");
	E32(buf, 0x368, 1920, "VideoParams.ui32Width");
	E32(buf, 0x36c, 1088, "VideoParams.ui32Height (docs/38 §7)");
	/* docs/21 §3.2 _S_AVE_DPBBuf {dataAddr, dataSize, metaAddr, metaSize} */
	E64(buf, 0x390, 0x0000000200000000ull, "saRecon[0] dataAddr");
	E32(buf, 0x398, 0x1fe000, "saRecon[0] dataSize");
	E64(buf, 0x3a0, 0x0000000200000000ull, "saRecon[0] metaAddr = base");
	E64(buf, 0x3b0, 0x0000000200400000ull, "saRecon[1] dataAddr");
	E32(buf, 0x3b8, 0x1fe000, "saRecon[1] dataSize");
	E64(buf, 0x3c0, 0x0000000200400000ull, "saRecon[1] metaAddr");
	/* docs/21 §2.3: CodedData Buf_Set+0x9C0, CodedHeader +0xBC0 */
	E64(buf, 0xd50, 0x0000000300000000ull, "saCodedData[0].iAddr");
	E32(buf, 0xd58, 0x2f8000, "saCodedData[0].iSize");
	E64(buf, 0xd60, 0x0000000300400000ull, "saCodedData[1].iAddr");
	E32(buf, 0xd68, 0x2f8000, "saCodedData[1].iSize");
	E64(buf, 0xf50, 0x0000000310000000ull, "CodedHeader[0].iAddr");
	E32(buf, 0xf58, 0xc000, "CodedHeader[0].iSize");
	E64(buf, 0xf60, 0x0000000310100000ull, "CodedHeader[1].iAddr");
	E32(buf, 0xf68, 0xc000, "CodedHeader[1].iSize");
	E32(buf, 0x25d4, 1, "sSliceMap.iNum");
	/* docs/37 §1, §3 */
	E32(buf, 0x291c, 6, "eProfile = High enum");
	E32(buf, 0x2938, 12, "eLevel = 4.0 enum");
	E32(buf, 0x293c, 0, "seq_parameter_set_id");
	E32(buf, 0x2940, 1, "chroma_format_idc");
	E32(buf, 0x2948, 0, "bit_depth_luma_minus8");
	E32(buf, 0x294c, 0, "bit_depth_chroma_minus8");
	E32(buf, 0x2d34, 0, "log2_max_frame_num_minus4");
	E32(buf, 0x2d38, 2, "pic_order_cnt_type");
	E32(buf, 0x2d44, 1, "max_num_ref_frames");
	E8(buf, 0x2d48, 0, "gaps_in_frame_num");
	E32(buf, 0x2d4c, 119, "pic_width_in_mbs_minus1");
	E32(buf, 0x2d50, 67, "pic_height_in_map_units_minus1");
	E32(buf, 0x2d54, 1, "frame_mbs_only_flag");
	E32(buf, 0x2d58, 1, "direct_8x8_inference_flag");
	E8(buf, 0x2d5c, 0, "vui_parameters_present_flag");
	E8(buf, 0x2db4, 1, "frame_cropping_flag");
	E32(buf, 0x2db8, 0, "crop left");
	E32(buf, 0x2dbc, 0, "crop right");
	E32(buf, 0x2dc0, 0, "crop top");
	E32(buf, 0x2dc4, 4, "crop bottom");
	E8(buf, 0x2dc8, 1, "SPS bFWCreatesHeader");
	E32(buf, 0x2dcc, 0, "SPS header_len (must be 0, fw 0x41b68)");
	/* docs/37 §4 */
	E32(buf, 0x2fd0, 0, "pic_parameter_set_id");
	E32(buf, 0x2fd4, 0, "pps seq_parameter_set_id");
	E32(buf, 0x2fd8, 1, "entropy_coding_mode_flag");
	E32(buf, 0x2fe0, 0, "num_slice_groups_minus1");
	E32(buf, 0x2ff8, 0, "pic_init_qp_minus26");
	E8(buf, 0x3020, 1, "deblocking_filter_control_present");
	E8(buf, 0x3021, 0, "constrained_intra_pred");
	E8(buf, 0x3023, 1, "transform_8x8_mode_flag");
	E8(buf, 0x3048, 1, "PPS bFWCreatesHeader");
	expect_rest_zero(buf, 0x3180);

	begin("26.6 start_avc negatives");
	expect_int(ave_cmd_build_start_avc(a, buf, 0x317f, &CTX, &s), -EINVAL, "len short by 1");
	s.n_coded = 31;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "31 coded: cap 30");
	s = session_1080p(false);
	s.level_idc = 1;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL, "level 1b");
	s = session_1080p(false);
	s.profile_idc = 66;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL,
		   "Baseline with CABAC");
	s = session_1080p(false);
	s.qp_i = 52;
	expect_int(ave_cmd_build_start_avc(a, buf, sizeof(buf), &CTX, &s), -EINVAL, "QP 52");
}

/* ------------------------------------------------------------------------ */

static struct ave_avc_frame frame_idr(void)
{
	struct ave_avc_frame f = {
		.frame_type = 3,			/* IDR on both, docs/47 §1.3 */
		.frame_num = 7, .poc = 14, .frame_rate = 30,
		.in_luma_addr = 0x0000000500000000ull, .in_luma_stride = 1920,
		.in_luma_size = 1920 * 1088,
		.in_chroma_addr = 0x0000000500200000ull, .in_chroma_stride = 1920,
		.in_chroma_size = 1920 * 544,
		.coded_index = 1,
		.coded_addr = 0x0000000300400000ull,
		.coded_hdr_addr = 0x0000000310100000ull,
		.coded_size = 0x2f8000,
		.recon_luma_addr = 0x0000000200000000ull,
		.recon_chroma_addr = 0x00000002001fe000ull,
		.recon_mv_addr = 0x0000000600000000ull,
	};
	return f;
}

static void test_process_13_5(void)
{
	const struct ave_cmd_abi *a = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	struct ave_avc_frame f = frame_idr();
	const u32 P = 0x9c8;	/* PICMGMT, kext 0xfffffe0008eac9bc; fw 0x145c8 */
	int ret;

	begin("13.5 process_avc");
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f);
	expect_int(ret, 0x1940, "size (fw 0xda9c)");
	/* id 7, slot = caller's (stp w8,w24,[x23,#24] 0xfffffe0008eac898) */
	expect_hdr_13_5(7, 0xa, 40, 200);
	E32(buf, P + 0x000, 0xf68, "PICMGMT[0] = 0xF68 (kext 0xfffffe0008eac9a0)");
	E32(buf, P + 0xcac, 3, "FrameType (kext 0xfffffe0008eaaa50)");
	E8(buf, P + 0x6f3, 0, "bInputCompressed (kext 0xfffffe0008eabb98)");
	E64(buf, P + 0x8c0, 0x0000000500000000ull, "sInput.Y (kext 0xfffffe0008eb0904)");
	E32(buf, P + 0x8c8, 1920, "luma stride (kext 0xfffffe0008eb0908)");
	E64(buf, P + 0x8d0, 0x0000000500200000ull, "sInput.UV (kext 0xfffffe0008eb0910)");
	E32(buf, P + 0x8d8, 1920, "chroma stride (kext 0xfffffe0008eb0914)");
	E8(buf, P + 0xc00, 0, "sOutput mode (kext 0xfffffe0008eb051c)");
	E32(buf, P + 0xc04, 1, "sOutput index (kext 0xfffffe0008eb0520)");
	E64(buf, P + 0xc08, 0x0000000300400000ull, "sOutput.Coded (kext 0xfffffe0008eb0548)");
	E64(buf, P + 0xc10, 0x0000000310100000ull, "coded_dataHeader (kext 0xfffffe0008eb05a0)");
	E32(buf, P + 0xc18, 0x2f8000, "CodedBufSize (kext 0xfffffe0008eb0554)");
	E64(buf, P + 0x898, 0x0000000200000000ull, "sRecon.Y_MSB (fw 0x3c0c4)");
	E64(buf, P + 0x8a8, 0x00000002001fe000ull, "sRecon.UV_MSB (fw 0x3c0d0)");
	E64(buf, P + 0x8b8, 0x0000000600000000ull, "recon MV (fw 0x2c4b0)");
	expect_rest_zero(buf, 0x1940);

	/*
	 * encoder_addr_entropy[i][0] - docs/54. The table is [16][4] at
	 * PICMGMT +0xA00, stride 0x20 between rows; SetTranscode asserts the
	 * first four are non-zero and 64-aligned (fw 0x59558 / 0x595a0).
	 */
	begin("13.5 process_avc entropy table");
	f = frame_idr();
	f.n_entropy = 4;
	f.n_entropy_cols = 1;
	f.entropy[0][0] = 0x0000000700000000ull;
	f.entropy[1][0] = 0x00000007000f0000ull;
	f.entropy[2][0] = 0x00000007001e0000ull;
	f.entropy[3][0] = 0x00000007002d0000ull;
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f);
	expect_int(ret, 0x1940, "size unchanged");
	E64(buf, P + 0xa00, f.entropy[0][0], "entropy[0][0] (wire 0x13C8)");
	E64(buf, P + 0xa20, f.entropy[1][0], "entropy[1][0] (wire 0x13E8)");
	E64(buf, P + 0xa40, f.entropy[2][0], "entropy[2][0] (wire 0x1408)");
	E64(buf, P + 0xa60, f.entropy[3][0], "entropy[3][0] (wire 0x1428)");
	E64(buf, P + 0xa08, 0, "[0][1] left zero with one column");
	E64(buf, P + 0xa80, 0, "row 4 left zero: only ctrl+3768 = 4 are read");

	/*
	 * All four columns: the kext writes entry [i][j] at +0xA00 + 32i + 8j
	 * (columns outside, rows inside, kext 0xfffffe0008eb0cb8..0d0c).
	 */
	begin("13.5 process_avc entropy matrix");
	{
		u32 i2, j2;

		for (i2 = 0; i2 < 4; i2++)
			for (j2 = 0; j2 < 4; j2++)
				f.entropy[i2][j2] = 0x0000000700000000ull +
						    0x100000ull * (4 * i2 + j2);
		f.n_entropy_cols = 4;
		memset(buf, 0, sizeof(buf));
		expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
			   0x1940, "size unchanged");
		for (i2 = 0; i2 < 4; i2++)
			for (j2 = 0; j2 < 4; j2++)
				E64(buf, P + 0xa00 + 32 * i2 + 8 * j2,
				    f.entropy[i2][j2], "entropy[i][j]");
		E64(buf, P + 0xa18 + 32 * 4, 0, "row 4 still zero");
		f.entropy[2][3] = 0;
		expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
			   -EINVAL, "a hole in the matrix");
		f.entropy[2][3] = 0x0000000700b00000ull;
		f.n_entropy_cols = 5;
		expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
			   -EINVAL, "more columns than the wire table has");
		f.n_entropy_cols = 1;
	}

	begin("13.5 process_avc entropy negatives (one column)");

	f.entropy[2][0] = 0;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
		   -EINVAL, "a zero entry in a table we claim to fill (:8020)");
	f.entropy[2][0] = 0x0000000700000020ull;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
		   -EINVAL, "entry % 64 (:8021)");
	f.entropy[2][0] = 0x00000007001e0000ull;
	f.n_entropy = 17;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
		   -EINVAL, "more rows than the wire table holds");
	f = frame_idr();
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 40, &f),
		   0x1940, "n_entropy = 0 still builds: the assert is the control");

	begin("13.5 process_avc negatives");
	f = frame_idr();
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 41, &f), -EINVAL,
		   "slot 41 (cmp w24,#0x28 0xfffffe0008eac860)");
	expect_int(ave_cmd_build_process_avc(a, buf, 0x193f, &CTX, 21, &f), -EINVAL, "len short");
	f.in_luma_size = 0;
	f.in_chroma_size = 0;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 21, &f), 0x1940,
		   "13.5 has no plane size (docs/47 §1.2)");
	f = frame_idr();
	f.coded_index = 20;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 21, &f), -EINVAL,
		   "coded index 20 >= cap 20");
	f = frame_idr();
	f.in_chroma_stride = 1921;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 21, &f), -EINVAL,
		   "stride % 64 (kext 0xfffffe0008eb0774)");
	f = frame_idr();
	f.frame_type = 1;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 21, &f), -EINVAL,
		   "P frame refused (I-only builder)");
}

static void test_process_26_6(void)
{
	const struct ave_cmd_abi *a = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	struct ave_avc_frame f = frame_idr();
	const u32 P = 0x12c0;	/* docs/32 */
	int ret;

	begin("26.6 process_avc");
	memset(buf, 0, sizeof(buf));
	ret = ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 50, &f);
	expect_int(ret, 0x63d8, "size");
	expect_hdr_26_6(8, 0xa, 1, 1, 50, 200);
	/* docs/32 §1, §3, §6; docs/39 §1.2 */
	E64(buf, P + 0x4f68, 7, "sFrameInfo.FrameNum");
	E32(buf, P + 0x4f78, 3, "sFrameInfo.FrameType");
	E32(buf, P + 0x4f80, 14, "PicOrderCntVal");
	E64(buf, P + 0x4fa8, 0x403e000000000000ull, "frame rate double 30.0");
	E8(buf, P + 0x174e, 0, "bInputCompressed");
	E64(buf, P + 0x4570, 0x0000000500000000ull, "luma iAddr");
	E32(buf, P + 0x4578, 1920 * 1088, "luma iSize");
	E32(buf, P + 0x457c, 1920, "luma iStride");
	E64(buf, P + 0x4590, 0x0000000500200000ull, "chroma iAddr");
	E32(buf, P + 0x4598, 1920 * 544, "chroma iSize");
	E32(buf, P + 0x459c, 1920, "chroma iStride");
	E8(buf, P + 0x4ef0, 0, "sOutput mode");
	E32(buf, P + 0x4ef4, 1, "sOutput index");
	E64(buf, P + 0x4ef8, 0x0000000300400000ull, "sOutput.Coded");
	E64(buf, P + 0x4f00, 0x0000000310100000ull, "coded_dataHeader");
	E32(buf, P + 0x4f08, 0x2f8000, "CodedBufSize");
	E64(buf, P + 0x4548, 0x0000000200000000ull, "sRecon.Y_MSB");
	E64(buf, P + 0x4558, 0x00000002001fe000ull, "sRecon.UV_MSB");
	E64(buf, P + 0x4568, 0x0000000600000000ull, "recon MV");
	expect_rest_zero(buf, 0x63d8);

	begin("26.6 process_avc negatives");
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 51, &f), -EINVAL,
		   "slot 51 (cmp w26,#0x33)");
	f.in_luma_size = 0;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 1, &f), -EINVAL,
		   "26.6 luma size required (docs/39 §1.2)");
	f = frame_idr();
	f.coded_index = 29;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 1, &f), 0x63d8,
		   "coded index 29 < cap 30");
	f.recon_luma_addr += 64;
	expect_int(ave_cmd_build_process_avc(a, buf, sizeof(buf), &CTX, 1, &f), -EINVAL,
		   "recon not 128-aligned (docs/21 §2.2.1)");
}

/* ------------------------------------------------------------------------ */

static void put32(u8 *b, u32 off, u32 v) { put_unaligned_le32(v, b + off); }

static void test_replies(void)
{
	const struct ave_cmd_abi *a13 = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	const struct ave_cmd_abi *a26 = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	u8 m13[0x48], m26[0x50];
	u32 st;

	begin("replies");
	/* 13.5 CONFIG_DONE: NotificationToHost(0xE01, 0, 0xEE0000) fw 0xe724-0xe768;
	 * 0x40 message, id +0, cid +0x10, status +0x38 (0x136f8-0x13708) */
	memset(m13, 0, sizeof(m13));
	put_unaligned_le16(0xe01, m13);
	put32(m13, 0x38, 0x00ee0000);
	expect_int(ave_cmd_check_reply(a13, AVE_OP_CONFIG, m13, 0x40, 0, &st), 0, "13.5 CONFIG_DONE ok");
	expect_int(st, 0xee0000, "13.5 status word");
	put32(m13, 0x38, 0x00ee0002);
	expect_int(ave_cmd_check_reply(a13, AVE_OP_CONFIG, m13, 0x40, 0, &st), -EIO,
		   "13.5 failure status 0xEE0002");
	put32(m13, 0x38, 0);
	expect_int(ave_cmd_check_reply(a13, AVE_OP_CONFIG, m13, 0x40, 0, &st), -EIO,
		   "13.5 status 0 is NOT success");
	put32(m13, 0x38, 0x00ee0000);
	expect_int(ave_cmd_check_reply(a13, AVE_OP_OPEN, m13, 0x40, 0, &st), -EPROTO,
		   "13.5 CONFIG_DONE is not START_DONE");
	expect_int(ave_cmd_check_reply(a13, AVE_OP_CONFIG, m13, 0x3f, 0, &st), -EPROTO,
		   "13.5 short reply");
	/* ENCODE_DONE 0xE06 is the 0x48 form (tst w8,#0x60 0x13784) */
	put_unaligned_le16(0xe06, m13);
	put32(m13, 0x10, 0xa);
	expect_int(ave_cmd_check_reply(a13, AVE_OP_PROCESS_AVC, m13, 0x48, 0xa, &st), 0,
		   "13.5 ENCODE_DONE ok");
	expect_int(ave_cmd_check_reply(a13, AVE_OP_PROCESS_AVC, m13, 0x40, 0xa, &st), -EPROTO,
		   "13.5 ENCODE_DONE needs 0x48");
	expect_int(ave_cmd_check_reply(a13, AVE_OP_PROCESS_AVC, m13, 0x48, 0xb, &st), -EPROTO,
		   "13.5 wrong client");

	/* 26.6.2: id = command id, cid u64 +0x10, status +0x40 (fw 0x312c8-0x312d4) */
	memset(m26, 0, sizeof(m26));
	put_unaligned_le16(1, m26);
	expect_int(ave_cmd_check_reply(a26, AVE_OP_CONFIG, m26, 0x48, 0, &st), 0, "26.6 Config ok");
	put32(m26, 0x40, (u32)-1004);
	expect_int(ave_cmd_check_reply(a26, AVE_OP_CONFIG, m26, 0x48, 0, &st), -EIO,
		   "26.6 status -1004");
	put32(m26, 0x40, 0);

	/* Negative controls: the other version's success must not pass. */
	expect_int(ave_cmd_check_reply(a13, AVE_OP_CONFIG, m26, 0x48, 0, &st), -EPROTO,
		   "26.6 Config reply fed to 13.5 checker");
	memset(m13, 0, sizeof(m13));
	put_unaligned_le16(0xe01, m13);
	put32(m13, 0x38, 0x00ee0000);
	expect_int(ave_cmd_check_reply(a26, AVE_OP_CONFIG, m13, 0x48, 0, &st), -EPROTO,
		   "13.5 CONFIG_DONE fed to 26.6 checker");
	expect_int(ave_cmd_check_reply(a13, AVE_OP_HALT, m13, 0x40, 0, &st), -EINVAL,
		   "13.5 Halt has no known reply");
}

/* The stray-byte scan must actually catch a stray byte. */
static void test_negative_control(void)
{
	const struct ave_cmd_abi *a13 = ave_cmd_abi_get(AVE_ABI_MACOS_13_5);
	const struct ave_cmd_abi *a26 = ave_cmd_abi_get(AVE_ABI_MACOS_26_6);
	struct ave_avc_session s = session_1080p(true);
	int n;

	begin("negative control");
	memset(buf, 0, sizeof(buf));
	ave_cmd_build_start_avc(a13, buf, sizeof(buf), &CTX, &s);
	/* check only the header; everything else must show up as stray */
	expect_hdr_13_5(4, 0xa, 6, 200);
	n = stray_bytes(buf, 0x10e10, false);
	checks++;
	if (n == 0)
		FAIL("scan found nothing in an unchecked Start command");
	else
		printf("  ok   negative control: %d unchecked non-zero bytes detected\n", n);

	/* A 26.6.2-shaped Start must fail the 13.5 table (sizes differ). */
	expect_int(ave_cmd_build_start_avc(a26, buf, 0x3180, &CTX,
					   &(struct ave_avc_session){0}), -EINVAL,
		   "empty session refused");
	s = session_1080p(false);
	expect_int(ave_cmd_build_start_avc(a13, buf, 0x3180, &CTX, &s), -EINVAL,
		   "13.5 Start into a 26.6.2-sized buffer refused");
}

int main(void)
{
	test_simple();
	test_config();
	test_start_13_5();
	test_start_26_6();
	test_process_13_5();
	test_process_26_6();
	test_replies();
	test_negative_control();

	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS",
	       checks, failures);
	return failures ? 1 : 0;
}
