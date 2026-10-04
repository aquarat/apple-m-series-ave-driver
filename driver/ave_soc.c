// SPDX-License-Identifier: GPL-2.0-only
/*
 * The per-SoC table (ave_soc.h). One row per SoC the driver has been
 * brought up on; docs/79 is the checklist for adding one.
 */
#include <linux/kernel.h>

#include "ave_soc.h"
#include "ave_dpe_tables.h"

/*
 * t6001, M1 Max (MacBookPro18,2/18,4), macOS 13.5 firmware. Every value
 * below was hard-coded in the driver before this table existed, and every
 * run up to f99/h3e used it.
 */
const struct ave_soc ave_soc_t6001 = {
	.name			= "t6001",
	.pipe_diag		= true,
	.dpe			= &ave_dpe_set_castor_6000,
	.dpe_phys		= 0x40d100000ULL,
	.inst			= 0,
	.fw_name		= "apple/ave_h13c.bin",
	.fw_pristine_name	= "apple/ave-13.5-data-pristine.bin",
	.fw_pristine_sha256	= {
		0xf1, 0xaf, 0x1e, 0xf4, 0x2b, 0xe0, 0x4a, 0x7d,
		0x60, 0x7b, 0x0b, 0xc1, 0xa2, 0x7d, 0xfd, 0xa5,
		0x72, 0x68, 0xb4, 0x76, 0xfe, 0xcf, 0x1d, 0x92,
		0xc8, 0xc1, 0x3c, 0x76, 0x0c, 0x71, 0xa1, 0x03,
	},
	.data_stkg_off		= 0x3a38,
	.text_win = {
		{ 0x0, {
			0x88, 0x5a, 0x78, 0x37, 0x38, 0xb3, 0xf7, 0xf8,
			0x91, 0xad, 0x16, 0xd3, 0xd5, 0x49, 0xb7, 0xd9,
			0x0d, 0x11, 0x4f, 0x67, 0x7b, 0xa1, 0x12, 0xc6,
			0xe3, 0xe5, 0x42, 0xb9, 0x5c, 0x72, 0xb9, 0xbe, } },
		{ 0x80000, {
			0x37, 0x54, 0x7c, 0x93, 0x89, 0xfb, 0x84, 0xc3,
			0x2b, 0x4f, 0x88, 0x8d, 0xde, 0x06, 0x1d, 0xd7,
			0xd4, 0xcc, 0xb7, 0xf0, 0x3c, 0x1e, 0x49, 0xee,
			0xd9, 0x65, 0xfd, 0x4e, 0x9b, 0xee, 0x7f, 0xbf, } },
		{ 0xe8000, {
			0x4d, 0x57, 0x01, 0x4b, 0xa7, 0x73, 0xd5, 0x76,
			0xd6, 0x5d, 0x1f, 0xdb, 0xcb, 0x51, 0xa6, 0x6d,
			0x6d, 0x8c, 0x59, 0x95, 0x9a, 0x48, 0x29, 0x2e,
			0xf2, 0xcb, 0xb5, 0xb8, 0xf4, 0x17, 0x0d, 0x09, } },
	},

	/*
	 * The kext's AVE_DevInfo table (13.5: 0xfffffe0007bc2a38, docs/09)
	 * has t6001 = DevID 15, DevType 12, ChipType 9. The driver has always
	 * sent t6000's row (14/11/8, "Castor") and the firmware accepts it;
	 * t6001's own row is untested. 26.6.2 renumbers (docs/45 row 3).
	 */
	.dev = {
		[AVE_ABI_MACOS_13_5] = { .dev_id = 14, .dev_type = 11, .chip_type = 8 },
		[AVE_ABI_MACOS_26_6] = { .dev_id = 11, .dev_type = 9, .chip_type = 6 },
	},

	/* ADT dart-ave0, bus + 0x2_0000_0000 (docs/44 §3, docs/56) */
	.cpudart_phys		= 0x40d040000ULL,
	.dapf_phys		= 0x40d044000ULL,
	.dart1_phys		= 0x40d030000ULL,
	.smmu_phys		= 0x40d020000ULL,

	/* Masked as programmed (docs/44 addendum, docs/49) */
	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x40d050000ULL, 0x40dc69000ULL },
	.dapf_mmio_adt		= { 0x506000000ULL, 0x507c6c000ULL },

	/* This machine's iBoot, 13.5 image (docs/43, docs/44 §2.4) */
	.iboot = {
		.text_phys	= 0x10000b28000ULL,
		.text_size	= 0xec000ULL,
		.text_dva	= 0xb28000ULL,	/* low 32 bits of the RVBAR base */
		.data_phys	= 0x10001a90000ULL,
		.data_size	= 0x134000ULL,
		.data_dva	= 0xec000ULL,
		.data_literal	= 0x1f0000ec000ULL,
	},

	.me1_node		= "/soc/power-management@28e580000/power-controller@8020",
	.me1_label		= "venc_me1",
	.pmp_report_node	= "/soc/pmp_report@28e3c0000/report@10",

	/* tools/pmp_ptd_map.py, controls C1-C4 (docs/75 §3, §11) */
	.pmp_ps_reg		= 0x28e0802d8ULL,	/* ADT ps-regs[5], psidx 27/28 */
	.pmp_report_base	= 0x28e3c0000ULL,	/* pmgr reg[41] */
	.pmp_dvfs_wr		= 0x28e3d0888ULL,	/* entry 273 = 264 + slot 9 */
	.pmp_dvfs_rd		= 0x28e3c1110ULL,
	.pmgr_perf_blk		= 0x28e580000ULL + 0x58000,	/* perf-regs[9] */
};

/*
 * t6001's second encoder, ave1 (docs/81 ave1 plan, host analysis of the
 * 13.5 ADT; run A0 checked the labels on the live DT). Every address is
 * ave0's + 0xFA000000 where the ADT says so. What is not known yet is 0 or
 * NULL, and the code that needs it refuses:
 *  - iboot.*: whether iBoot loads a copy for ave1 at all is open (run M0);
 *    the placement check refuses a zero text_phys;
 *  - fw_pristine_name: ave0's DATA carries ave0's addresses (IOBA, CpAd,
 *    WrAd), so restoring it into ave1 would drive ave0's hardware;
 *  - the PMP report and vote: Asahi's report@22 sets the wrong bit.
 * dev[] is ave0's row on purpose: ave1's own (15/12/9, instance 1) turns
 * on the firmware's mailbox route to ave0 (perGroup 2, fw 0xaa434).
 */
const struct ave_soc ave_soc_t6001_ave1 = {
	.name			= "t6001-ave1",
	.pipe_diag		= true,
	.dpe			= &ave_dpe_set_castor_6000,
	.dpe_phys		= 0x507100000ULL,
	.inst			= 1,
	.fw_name		= "apple/ave_h13c.bin",
	/* the same blob; ave1's copy gets its own tags (iboot.tag_*) */
	.fw_pristine_name	= "apple/ave-13.5-data-pristine.bin",
	.fw_pristine_sha256	= {
		0xf1, 0xaf, 0x1e, 0xf4, 0x2b, 0xe0, 0x4a, 0x7d,
		0x60, 0x7b, 0x0b, 0xc1, 0xa2, 0x7d, 0xfd, 0xa5,
		0x72, 0x68, 0xb4, 0x76, 0xfe, 0xcf, 0x1d, 0x92,
		0xc8, 0xc1, 0x3c, 0x76, 0x0c, 0x71, 0xa1, 0x03,
	},
	.data_stkg_off		= 0x3a38,
	.text_win = {
		{ 0x0, {
			0x88, 0x5a, 0x78, 0x37, 0x38, 0xb3, 0xf7, 0xf8,
			0x91, 0xad, 0x16, 0xd3, 0xd5, 0x49, 0xb7, 0xd9,
			0x0d, 0x11, 0x4f, 0x67, 0x7b, 0xa1, 0x12, 0xc6,
			0xe3, 0xe5, 0x42, 0xb9, 0x5c, 0x72, 0xb9, 0xbe, } },
		{ 0x80000, {
			0x37, 0x54, 0x7c, 0x93, 0x89, 0xfb, 0x84, 0xc3,
			0x2b, 0x4f, 0x88, 0x8d, 0xde, 0x06, 0x1d, 0xd7,
			0xd4, 0xcc, 0xb7, 0xf0, 0x3c, 0x1e, 0x49, 0xee,
			0xd9, 0x65, 0xfd, 0x4e, 0x9b, 0xee, 0x7f, 0xbf, } },
		{ 0xe8000, {
			0x4d, 0x57, 0x01, 0x4b, 0xa7, 0x73, 0xd5, 0x76,
			0xd6, 0x5d, 0x1f, 0xdb, 0xcb, 0x51, 0xa6, 0x6d,
			0x6d, 0x8c, 0x59, 0x95, 0x9a, 0x48, 0x29, 0x2e,
			0xf2, 0xcb, 0xb5, 0xb8, 0xf4, 0x17, 0x0d, 0x09, } },
	},

	.dev = {
		[AVE_ABI_MACOS_13_5] = { .dev_id = 14, .dev_type = 11, .chip_type = 8 },
		[AVE_ABI_MACOS_26_6] = { .dev_id = 11, .dev_type = 9, .chip_type = 6 },
	},

	/* ADT dart-ave1 reg[0..3] */
	.cpudart_phys		= 0x507040000ULL,
	.dapf_phys		= 0x507044000ULL,
	.dart1_phys		= 0x507030000ULL,
	.smmu_phys		= 0x507020000ULL,

	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x507050000ULL, 0x507c69000ULL },
	/* dart-ave1's ADT entry 1 admits ave0's SVE..ASC (the peer window) */
	.dapf_mmio_adt		= { 0x40d050000ULL, 0x40dc69000ULL },
	.dapf_by_driver		= true,

	/*
	 * docs/82 A2: ave1's RVBAR is 0x102010000b28001 - iBoot points it at
	 * ave0's TEXT, which is read-only and shared. DATA is ours: the
	 * pristine blob with CpAd/WrAd/IOBA (DATA+0x3bbb/0x3bcb/0x3be4) set to
	 * ave1's ASC, ASC+0x400000 and axi2af.
	 */
	.iboot = {
		.text_phys	= 0x10000b28000ULL,
		.text_size	= 0xec000ULL,
		.text_dva	= 0xb28000ULL,
		.data_size	= 0x134000ULL,
		.data_dva	= 0xec000ULL,
		.data_literal	= 0x1f0000ec000ULL,
		.data_owned	= true,
		.tag_cpad	= 0x507800000ULL,
		.tag_wrad	= 0x507c00000ULL,
		.tag_ioba	= 0x506000000ULL,
	},

	.me1_node		= "/soc/power-management@28e680000/power-controller@8020",
	.me1_label		= "venc1_me1",
};

static const struct ave_soc *const ave_soc_t6001_rows[] = {
	&ave_soc_t6001, &ave_soc_t6001_ave1,
};

const struct ave_soc_set ave_soc_set_t6001 = {
	.rows	= ave_soc_t6001_rows,
	.n	= ARRAY_SIZE(ave_soc_t6001_rows),
};

/*
 * t8103, M1 (MacBook Air, apple,j313), macOS 13.5 firmware. Brought up by
 * the t8103-port fork (docs/89): H.264 through V4L2, reload in the same boot.
 * The iBoot placement below is that machine's; another t8103 machine may
 * differ (the driver checks RVBAR and refuses a mismatch, docs/79).
 */
const struct ave_soc ave_soc_t8103 = {
	.name			= "t8103",
	.dpe			= &ave_dpe_set_acis_8103,
	.dpe_phys		= 0x267100000ULL,
	.inst			= 0,
	/* AppleAVE2FW_H13G.im4p, 13.5 (22G74), 1327864 bytes unwrapped */
	.fw_name		= "apple/ave_h13g.bin",
	/*
	 * DATA as iBoot left it on this machine, copied out of DRAM before the
	 * first start (t8103/dump-fw.sh on the t8103-port branch, 2026-10-01): 0x128000 bytes, 146 of
	 * them filled in by iBoot (STKG, SOC_ 0x8103, SOCR 0x11, CpAd
	 * 0x267800000, WrAd 0x267c00000, tunables); IOBA stays 0 here.
	 */
	.fw_pristine_name	= "apple/ave-13.5-h13g-data-pristine.bin",
	.fw_pristine_sha256	= {
		0xee, 0x25, 0x94, 0x4a, 0x90, 0xf4, 0x71, 0x44,
		0x91, 0xfd, 0x4e, 0x98, 0xa0, 0x81, 0x69, 0x50,
		0x0c, 0x39, 0xdd, 0xfd, 0x7e, 0xee, 0xe0, 0x18,
		0xf8, 0x5a, 0x94, 0x05, 0xa5, 0xc7, 0x3e, 0x57,
	},
	.data_stkg_off		= 0x3380,
	.text_win = {
		{ 0x0, {
			0x73, 0xd0, 0xa1, 0xbd, 0xd0, 0x70, 0x19, 0xc1,
			0x9f, 0x72, 0x93, 0xab, 0x9e, 0x99, 0xd2, 0xfc,
			0x44, 0xa9, 0xfe, 0xa9, 0x85, 0x8a, 0x61, 0x66,
			0x99, 0x78, 0xe6, 0xc3, 0xe0, 0xf3, 0x0b, 0x71, } },
		{ 0x80000, {
			0x73, 0xd2, 0xc4, 0xe6, 0xbd, 0xcd, 0x94, 0xc1,
			0xc4, 0xf3, 0x74, 0x0a, 0xfc, 0x15, 0x4b, 0x3f,
			0x90, 0x85, 0x1c, 0x80, 0xc6, 0x7d, 0x0d, 0x91,
			0x7a, 0xfa, 0x86, 0xca, 0xd3, 0x80, 0xd7, 0x0c, } },
		{ 0xc8000, {
			0xf8, 0x41, 0x53, 0x9f, 0x10, 0xa0, 0x98, 0xed,
			0xbf, 0x10, 0x78, 0xa2, 0x2e, 0x99, 0xee, 0xaf,
			0x71, 0x85, 0x5a, 0x37, 0x59, 0x51, 0xe4, 0x6c,
			0x8c, 0xb6, 0x26, 0xee, 0xc1, 0x31, 0xd0, 0x68, } },
	},

	/* docs/79 §2: t8103 on 13.5 is 13/10/7 (Acis). 26.6: docs/45 row 5. */
	.dev = {
		[AVE_ABI_MACOS_13_5] = { .dev_id = 13, .dev_type = 10, .chip_type = 7 },
	},

	/* ADT dart-ave reg[0..3], bus + 0x2_0000_0000 */
	.cpudart_phys		= 0x267040000ULL,
	.dapf_phys		= 0x267044000ULL,
	.dart1_phys		= 0x267030000ULL,
	.smmu_phys		= 0x267020000ULL,

	/*
	 * ADT dart-ave filter-data-instance-0 has one entry, the DVA window
	 * 0xf00000000-0xfffffffff (r0 0x33, r4 1); there is no MMIO entry on
	 * this SoC, so dapf_mmio_adt stays 0.
	 */
	.dapf_window		= { 0xf00000000ULL, 0xffffffffcULL },
	.dapf_mmio_own		= { 0x267050000ULL, 0x267c69000ULL },
	/*
	 * The driver programs the DAPF (TEXT + window, ave_dapf_program_instance),
	 * as on t6000, so stock m1n1 suffices. UNTESTED on t8103: the port was
	 * brought up with m1n1 doing it (t8103-port's m1n1 0002). With that
	 * m1n1 the driver rewrites the same two entries; m1n1 sets no lock.
	 */
	.dapf_by_driver		= true,

	/*
	 * This machine's iBoot, 13.5 H13G image: /arm-io/ave segment-ranges
	 * read from the live ADT on 2026-10-01 (m1n1 probe, pre-loaded = 1):
	 *   TEXT phys 0x8009f4000 iova 0       remap 0x8009f4000 size 0xcc000
	 *   DATA phys 0x8019b0000 iova 0xcc000 remap 0xf000cc000 size 0x128000
	 * The DATA literal sits at TEXT+0x423c as in H13C (same RTKit start).
	 *
	 * docs/89 §6: a Mac mini (j274) on stock m1n1 reads the same RVBAR
	 * and literal, but there iBoot's DATA lies inside Linux's System RAM
	 * (0x801224000-0x802c27fff on that boot) and is overwritten in use.
	 * Then the driver builds DATA itself, as for t6001's ave1: the
	 * pristine blob in its own pages, mapped at the same DVA 0xcc000 the
	 * literal 0xf000cc000 reaches through the DAPF window. TEXT stays
	 * iBoot's (outside RAM, fetched physically). Where DATA is outside
	 * RAM (the j313 with the fork's m1n1) nothing changes. The blob may be
	 * the pinned j313 dump or any blob that verifies against ave_h13g.bin
	 * with these tags (tools/data_blob_from_image.py build --soc t8103).
	 */
	.iboot = {
		.text_phys	= 0x8009f4000ULL,
		.text_size	= 0xcc000ULL,
		.text_dva	= 0x9f4000ULL,
		.data_phys	= 0x8019b0000ULL,
		.data_size	= 0x128000ULL,
		.data_dva	= 0xcc000ULL,
		.data_literal	= 0xf000cc000ULL,
		.data_owned	= AVE_DATA_OWNED_IF_RAM,
		.tag_cpad	= 0x267800000ULL,	/* the ASC bank */
		.tag_wrad	= 0x267c00000ULL,
		.tag_ioba	= 0,			/* iBoot leaves IOBA 0 on t8103 */
		.tag_soc	= 0x8103,
		.blob_by_image	= true,
	},

	/*
	 * Neither ME domain has a phandle in Asahi's t8103 DT, and they are
	 * siblings under pipe4 + pipe5 (on t6001 me1 hangs off me0).
	 */
	.me0_node		= "/soc/power-management@23b700000/power-controller@8018",
	.me0_label		= "venc_me0",
	.me1_node		= "/soc/power-management@23b700000/power-controller@8020",
	.me1_label		= "venc_me1",

	/*
	 * docs/89 §8: an H.264 B frame hangs the encoder here with both ME
	 * units programmed (wire 0xFCEA = 1), the fix that works on t6000 and
	 * t8112 (docs/94). Until a configuration is shown to work, streams get
	 * one reference per frame; enc_two_refs=1 lets the lab try.
	 */
	.two_refs_hang		= true,

	/* No PMP report on t8103 in Asahi: leave the pmp_* fields 0 (docs/79 §4) */
};

static const struct ave_soc *const ave_soc_t8103_rows[] = {
	&ave_soc_t8103,
};

const struct ave_soc_set ave_soc_set_t8103 = {
	.rows	= ave_soc_t8103_rows,
	.n	= ARRAY_SIZE(ave_soc_t8103_rows),
};

/*
 * t6000, M1 Pro (MacBookPro18,1/18,3, j314s/j316s), macOS 13.5 firmware.
 * docs/87. The encoder is t6001's ave0 (t6000 is t6001 cut in half): the
 * j314s and j314c ADTs agree on /arm-io/ave0 (reg, interrupts, power-gates)
 * and /arm-io/dart-ave0 (reg, sids, filter-data-instance-0). There is no
 * ave1. What differs is the firmware variant, H13S rather than H13C, so its
 * layout, placement and identity are this row's own:
 *  - __TEXT 0xd0000 (not 0xec000), __DATA vm 0xd0000 +0x128000 (not
 *    0xec000 +0x134000); TEXT+0x423c holds 0x1f0000d0000;
 *  - iBoot places TEXT at the same 0x10000b28000 and DATA at 0x10001a74000
 *    (a cold 16 MiB dump from 0x10000b28000, tools/fw_dump_compare.py
 *    --data-off 0xf4c000: TEXT 100 %, DATA non-zero 99.98 %);
 *  - the pristine DATA blob is that dump's DATA, zero-padded (the bss);
 *  - STKG is at DATA+0x3380 (the patchbay moved).
 * m1n1 v1.6.1 does not program dart-ave0's DAPF (no tools/m1n1 patch on
 * this machine), so the driver does, as for t6001's ave1 (docs/84 §3).
 * The device row is t6000's own, the one t6001 has always sent. The PMP
 * fields are t6001's: tools/pmp_ptd_map.py on the j314s ADT passes C1-C4
 * and gives the same PS registers, report base, DVFS slot 9 and perf block
 * (docs/87 §6). They are only used with pmp_report=1/pmp_vote=, which need
 * the PMP running (an APPLE_USE_PMP DTB, docs/78) - stock Fedora has it off.
 */
const struct ave_soc ave_soc_t6000 = {
	.name			= "t6000",
	.pipe_diag		= true,		/* t6001's ave0 hardware */
	.dpe			= &ave_dpe_set_castor_6000,	/* DevID 14, as t6001 sends */
	.dpe_phys		= 0x40d100000ULL,
	.inst			= 0,
	.fw_name		= "apple/ave_h13s.bin",
	.fw_pristine_name	= "apple/ave-13.5-h13s-data-pristine.bin",
	.data_stkg_off		= 0x3380,

	.dev = {
		[AVE_ABI_MACOS_13_5] = { .dev_id = 14, .dev_type = 11, .chip_type = 8 },
		[AVE_ABI_MACOS_26_6] = { .dev_id = 11, .dev_type = 9, .chip_type = 6 },
	},

	.cpudart_phys		= 0x40d040000ULL,
	.dapf_phys		= 0x40d044000ULL,
	.dart1_phys		= 0x40d030000ULL,
	.smmu_phys		= 0x40d020000ULL,

	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x40d050000ULL, 0x40dc69000ULL },
	/* j314s's dart-ave0 entry 1 is the same 0x506... span as j314c's */
	.dapf_mmio_adt		= { 0x506000000ULL, 0x507c6c000ULL },
	.dapf_by_driver		= true,

	.iboot = {
		.text_phys	= 0x10000b28000ULL,
		.text_size	= 0xd0000ULL,
		.text_dva	= 0xb28000ULL,
		.data_phys	= 0x10001a74000ULL,
		.data_size	= 0x128000ULL,
		.data_dva	= 0xd0000ULL,
		.data_literal	= 0x1f0000d0000ULL,
	},
	.fw_pristine_sha256	= {
		0x36, 0xd8, 0x28, 0x53, 0xea, 0x46, 0x48, 0xab,
		0xe1, 0x1a, 0x0a, 0x2b, 0x62, 0x49, 0x9d, 0xb5,
		0x15, 0x2a, 0xf5, 0x4a, 0x38, 0xd0, 0x56, 0xe9,
		0x55, 0xeb, 0x1c, 0x49, 0x69, 0xf8, 0xce, 0xc7,
	},
	.text_win = {
		{ 0x0, {
			0xd4, 0x0a, 0xc3, 0xcf, 0x8b, 0x07, 0xce, 0xe9,
			0x0f, 0x89, 0xc2, 0xfe, 0x41, 0x6c, 0x2d, 0xf6,
			0x28, 0x63, 0x23, 0x9c, 0x02, 0xe0, 0x7c, 0xc4,
			0x69, 0x85, 0xb9, 0x80, 0x26, 0x1d, 0x7f, 0x31, } },
		{ 0x80000, {
			0x7c, 0x28, 0x97, 0xfb, 0xf8, 0x88, 0xb7, 0xd4,
			0xc3, 0xbd, 0x69, 0xc6, 0x98, 0xe9, 0xda, 0x94,
			0x26, 0xb4, 0xb6, 0xd5, 0x7f, 0x1d, 0x58, 0x86,
			0xb4, 0x0f, 0xfe, 0x45, 0xc0, 0x90, 0x9e, 0xef, } },
		{ 0xcc000, {
			0xd7, 0x5c, 0x19, 0x18, 0xe1, 0x41, 0x72, 0xd2,
			0x80, 0xa2, 0x42, 0x79, 0xdf, 0xd1, 0x66, 0x8f,
			0xd4, 0x54, 0xaf, 0xd3, 0x66, 0x4e, 0x9d, 0xb1,
			0xbb, 0x76, 0x9f, 0x28, 0xc5, 0xba, 0xae, 0xc8, } },
	},

	.me1_node		= "/soc/power-management@28e580000/power-controller@8020",
	.me1_label		= "venc_me1",
	.pmp_report_node	= "/soc/pmp_report@28e3c0000/report@10",

	/* tools/pmp_ptd_map.py on the j314s ADT: identical to t6001's */
	.pmp_ps_reg		= 0x28e0802d8ULL,	/* ADT ps-regs[5], psidx 27/28 */
	.pmp_report_base	= 0x28e3c0000ULL,	/* pmgr reg[41] */
	.pmp_dvfs_wr		= 0x28e3d0888ULL,	/* entry 273 = 264 + slot 9 */
	.pmp_dvfs_rd		= 0x28e3c1110ULL,
	.pmgr_perf_blk		= 0x28e580000ULL + 0x58000,	/* perf-regs[9] */
};

static const struct ave_soc *const ave_soc_t6000_rows[] = {
	&ave_soc_t6000,
};

const struct ave_soc_set ave_soc_set_t6000 = {
	.rows	= ave_soc_t6000_rows,
	.n	= ARRAY_SIZE(ave_soc_t6000_rows),
};

/*
 * t8112, M2 (Mac mini j473), macOS 13.5 firmware (docs/90).
 *
 * AppleAVE2FW_H14G (same build tag as H13x), TEXT 0xd0000 / DATA vm 0xd0000
 * +0x128000 like H13S, STKG at DATA+0x35d8; device row 18/15/10 from the
 * 13.5 kext table; AVE_DPE tunables Atlas_8112 from the M2's own
 * kernelcache; the same MMIO, DART and power-domain addresses as t8103.
 *
 * What differs (docs/90 §3): dart-ave is a "dart,t8110" (dart_t8110), and
 * the core fetches its firmware through it. RVBAR holds 0x800000000, the
 * dart-ave vm-base, not a physical address; the DATA literal is
 * 0x8000d0000, vm-base + 0xd0000. iBoot put TEXT at 0x8008e0000 and DATA at
 * 0x800e88000, both below Linux's RAM, and left the DAPF empty (the ADT has
 * no dapf-instance for dart-ave). Placement read on one j473 (docs/90 §3).
 */
const struct ave_soc ave_soc_t8112 = {
	.name			= "t8112",
	.dpe			= &ave_dpe_set_atlas_8112,
	.pipe_diag		= false,	/* pipe register map not checked on t8112 */
	.dpe_phys		= 0x267100000ULL,
	.inst			= 0,
	.fw_name		= "apple/ave_h14g.bin",
	.fw_pristine_name	= "apple/ave-13.5-h14g-data-pristine.bin",
	.fw_pristine_sha256	= {
		0xfe, 0x2d, 0x5e, 0xc1, 0xb2, 0xf9, 0x27, 0x98,
		0xe5, 0x84, 0x7e, 0x0f, 0xc0, 0x66, 0xfc, 0x95,
		0xdb, 0xbd, 0xd5, 0xe3, 0x52, 0x66, 0x51, 0xba,
		0x00, 0x21, 0x6b, 0x45, 0xb7, 0x83, 0x9f, 0x3d,
	},
	.data_stkg_off		= 0x35d8,
	.text_win = {
		{ 0x0, {
			0x86, 0xc3, 0x55, 0xf3, 0x40, 0x47, 0x6c, 0xc3,
			0xac, 0x9b, 0x6a, 0x57, 0x79, 0x28, 0xb5, 0x10,
			0x9e, 0xb9, 0x5b, 0xb4, 0x49, 0x72, 0x3d, 0x34,
			0x1b, 0x2c, 0xc6, 0xc9, 0x1d, 0x32, 0x13, 0xef, } },
		{ 0x80000, {
			0x7a, 0x5d, 0x76, 0x5e, 0x2a, 0x20, 0xc1, 0xc6,
			0x83, 0xcc, 0xc5, 0x8d, 0x5f, 0x5e, 0x71, 0x15,
			0x77, 0xb3, 0xea, 0x00, 0xaa, 0x2f, 0x73, 0x53,
			0x66, 0x41, 0x0e, 0xc0, 0x18, 0x1d, 0x31, 0xbb, } },
		{ 0xcc000, {
			0x34, 0xbb, 0x1e, 0x48, 0x8f, 0x4c, 0x27, 0x09,
			0x19, 0x9c, 0x54, 0x0c, 0x02, 0x0c, 0x48, 0x66,
			0xe8, 0xf8, 0x62, 0xc7, 0x9e, 0xfd, 0xf7, 0x6f,
			0xb3, 0xff, 0x8a, 0xb6, 0x8b, 0x69, 0x70, 0xa1, } },
	},

	/* 13.5 kext AVE_DevInfo (docs/09): t8112 = chipType 10, devType 15, DevID 18 */
	.dev = {
		[AVE_ABI_MACOS_13_5] = { .dev_id = 18, .dev_type = 15, .chip_type = 10 },
	},

	/* ADT dart-ave: reg[0] DART, [1] CPUDART, [2] SMMU, [3] CPU_DAPF; bus + 0x2_0000_0000 */
	.cpudart_phys		= 0x267040000ULL,
	.dapf_phys		= 0x267044000ULL,
	.dart1_phys		= 0x267030000ULL,
	.smmu_phys		= 0x267020000ULL,
	.dart_t8110		= true,
	.src_dims		= true,

	.dapf_mmio_own		= { 0x267050000ULL, 0x267c69000ULL },

	.iboot = {
		.text_phys	= 0x8008e0000ULL,
		.text_size	= 0xd0000ULL,
		.text_dva	= 0x800000000ULL,
		.data_phys	= 0x800e88000ULL,
		.data_size	= 0x128000ULL,
		.data_dva	= 0x8000d0000ULL,
		.data_literal	= 0x8000d0000ULL,
		.translated	= true,
	},

	/* As on t8103: neither ME domain has a phandle; siblings under pipe4 + pipe5 */
	.me0_node		= "/soc/power-management@23b700000/power-controller@8018",
	.me0_label		= "venc_me0",
	.me1_node		= "/soc/power-management@23b700000/power-controller@8020",
	.me1_label		= "venc_me1",

	/*
	 * PMP (docs/90 §9): tools/pmp_ptd_map.py on the j473 ADT, controls
	 * C1-C4 against Asahi's t8112 pmp-report offsets. VENC votes in its
	 * own perf domain (1), levels VNOM 1 / VMAX 2, no FAB0 pseudo-device:
	 * macOS's VMax message is 0x2000000000000002. Measured, nothing
	 * should use it: the encoder is fastest with the PMP off (docs/90 §9).
	 */
	.pmp_report_node	= "/soc/pmp_report@23b3c0000/report@9",
	.pmp_ps_reg		= 0x23b700410ULL,	/* ADT ps-regs[9], psidx 2/3 */
	.pmp_report_base	= 0x23b3c0000ULL,	/* pmgr reg[39] */
	.pmp_dvfs_wr		= 0x23b3d0578ULL,	/* entry 175 = 168 + slot 7 */
	.pmp_dvfs_rd		= 0x23b3c0af0ULL,
	.pmgr_perf_blk		= 0x23b758000ULL,	/* perf-regs[1] */
};

static const struct ave_soc *const ave_soc_t8112_rows[] = {
	&ave_soc_t8112,
};

const struct ave_soc_set ave_soc_set_t8112 = {
	.rows	= ave_soc_t8112_rows,
	.n	= ARRAY_SIZE(ave_soc_t8112_rows),
};

/*
 * t6002, M1 Ultra (Mac Studio j375d), macOS 13.5 firmware (docs/98).
 *
 * Two t6001 dies. The j375d restore ADT has four encoders: ave0/ave1 are
 * t6001's ave0/ave1 byte for byte (reg, interrupts, power-gates, dart-ave*
 * reg, sids, filter-data-instance-0), and ave2/ave3 are die 1's copies:
 * every bus address + 0x2000000000, AIC + 4096 (die 1), PMGR ids with the
 * die bit. CPU physical = ADT bus + 0x200000000 on both dies (/arm-io
 * ranges), so die 1's encoders are at 0x240d100000 and 0x2507100000.
 *
 * Firmware: the BuildManifest gives j375dap AppleAVE2FW_H13D (docs/43
 * §1.2). H13D is H13C rebuilt: the same segment layout (TEXT 0xec000, DATA
 * vm 0xec000 +0x134000, file 0xf0000 +0x64000), the same patchbay and
 * tunables offsets (STKG DATA+0x3a38, CpAd/WrAd/IOBA at the docs/82
 * offsets), and the same 1390 symbols; the only function that changed size
 * is CAVECommonController::SetTunable (672 -> 588 bytes), whose table gains
 * three AVE_DPE pipe tunables (+0x30740/44/48). Everything after it moves
 * by -0x54, so the TEXT identity windows below are H13D's own.
 *
 * What is NOT known until the machine has been read (docs/98 U0): where
 * iBoot put TEXT and DATA, whether it preloaded ave1-ave3, and the
 * pristine DATA blob. Those come from the block below, which
 * tools/t6002_placement.py rewrites from the live ADT and cold dumps. While
 * a row's TEXT is 0 the driver refuses at probe, before any register access
 * (ave_drv.c), so an unfilled build is harmless.
 *
 * Device row: t6000's 14/11/8 for every instance (perGroup 1, instance 0),
 * as t6001 sends for both encoders: no firmware-to-firmware routing between
 * the four (docs/82 §0.2). DAPF: the driver programs it (stock m1n1).
 * PMP: ave0's addresses are t6001's (tools/pmp_ptd_map.py on the j375d ADT
 * passes C1-C4 with identical values), but the stock j375d DT has the PMP
 * disabled; die 1 has no VENC report entries in Asahi's DT at all.
 *
 * BEGIN t6002-placement (generated: tools/t6002_placement.py; do not edit by hand)
 */
#define T6002_AVE0_TEXT_PHYS		0x10000bfc000ULL
#define T6002_AVE0_DATA_PHYS		0x10001640000ULL
#define T6002_AVE0_PRISTINE_NAME	"apple/ave-13.5-h13d-data-pristine.bin"
#define T6002_AVE0_PRISTINE_SHA256	{ 0x5a, 0x00, 0x28, 0xb7, 0x7d, 0xc4, 0x31, 0xb1, 0xb5, 0x96, 0xe5, 0xe4, 0x98, 0x2a, 0x96, 0xb8, 0x9a, 0x1f, 0xff, 0xa3, 0xe9, 0x71, 0x45, 0xf2, 0xe2, 0xc1, 0x48, 0x47, 0x89, 0x56, 0xab, 0x55 }
#define T6002_AVE1_TEXT_PHYS		0x10000bfc000ULL
#define T6002_AVE1_DATA_PHYS		0x10001774000ULL
#define T6002_AVE1_PRISTINE_NAME	"apple/ave-13.5-h13d-ave1-data-pristine.bin"
#define T6002_AVE1_PRISTINE_SHA256	{ 0x64, 0x83, 0x32, 0xa9, 0xa2, 0x14, 0xea, 0x23, 0xd7, 0x7c, 0xd9, 0x0a, 0xca, 0xc5, 0xd0, 0xa5, 0xf2, 0x03, 0x5d, 0x95, 0x90, 0x2d, 0xa6, 0xdd, 0xe9, 0x7f, 0x70, 0x89, 0x66, 0x45, 0x0f, 0x73 }
#define T6002_AVE2_TEXT_PHYS		0x10000bfc000ULL
#define T6002_AVE2_DATA_PHYS		0x100018a8000ULL
#define T6002_AVE2_PRISTINE_NAME	"apple/ave-13.5-h13d-ave2-data-pristine.bin"
#define T6002_AVE2_PRISTINE_SHA256	{ 0xf0, 0xdd, 0xcd, 0xd9, 0x4d, 0xa6, 0x8b, 0xef, 0xfb, 0x67, 0x14, 0xff, 0x95, 0xc6, 0x4b, 0x58, 0x6e, 0x96, 0x70, 0x7b, 0xa3, 0x70, 0x27, 0x9e, 0xc2, 0x41, 0x92, 0xf7, 0x1d, 0xf6, 0x33, 0xd0 }
#define T6002_AVE3_TEXT_PHYS		0x10000bfc000ULL
#define T6002_AVE3_DATA_PHYS		0x100019dc000ULL
#define T6002_AVE3_PRISTINE_NAME	"apple/ave-13.5-h13d-ave3-data-pristine.bin"
#define T6002_AVE3_PRISTINE_SHA256	{ 0x3f, 0x93, 0x8f, 0xb9, 0x56, 0x03, 0x62, 0xbb, 0xf6, 0x50, 0xdf, 0x9b, 0xca, 0xec, 0xa0, 0xcb, 0x2a, 0xa1, 0x0a, 0xc4, 0xf9, 0x58, 0x30, 0x42, 0x23, 0xc7, 0x8c, 0x93, 0x97, 0x9d, 0xd5, 0x16 }
/* END t6002-placement */

/* H13D's TEXT, as the image has it (tools/t6002_placement.py checks DRAM) */
#define T6002_TEXT_WIN { \
	{ 0x0, { \
		0x99, 0xae, 0x83, 0x15, 0xc6, 0x0d, 0xf9, 0x35, \
		0x94, 0xcd, 0xd0, 0x0c, 0xdf, 0x7b, 0x72, 0x92, \
		0x89, 0xce, 0xeb, 0xca, 0x54, 0xd3, 0xe2, 0x0c, \
		0xd9, 0x57, 0x2d, 0xcd, 0xa7, 0xfd, 0x5c, 0x49, } }, \
	{ 0x80000, { \
		0xfe, 0xb9, 0xd4, 0x3d, 0x9b, 0x85, 0xa3, 0x08, \
		0xad, 0xdc, 0x09, 0xbd, 0xb4, 0x3a, 0xe5, 0xe3, \
		0xa6, 0x8d, 0xdd, 0x2f, 0x58, 0xc6, 0xf5, 0xe4, \
		0xce, 0xc2, 0xfe, 0x70, 0x20, 0x53, 0x3c, 0xc2, } }, \
	{ 0xe8000, { \
		0x34, 0x4a, 0x25, 0x19, 0x17, 0x06, 0xf2, 0x22, \
		0x8d, 0x53, 0x71, 0x1e, 0xee, 0xe2, 0xf7, 0xec, \
		0x82, 0xea, 0xde, 0x47, 0x35, 0x7b, 0x9f, 0xfd, \
		0x52, 0x4e, 0xcc, 0xfa, 0x3f, 0x4a, 0x4b, 0x38, } }, \
}

/*
 * One encoder's iBoot block. RVBAR's low 32 bits are TEXT's DVA (t6001:
 * 0x10000b28000 -> 0xb28000). DATA 0 = iBoot did not load one for this
 * encoder: the driver builds it from ave0's pristine blob with this
 * encoder's three tags (docs/82, as t6001's ave1).
 */
#define T6002_IBOOT(text, data, cpad, wrad, ioba) {			\
	.text_phys	= (text),					\
	.text_size	= 0xec000ULL,					\
	.text_dva	= (text) & 0xffffffffULL,			\
	.data_phys	= (data),					\
	.data_size	= 0x134000ULL,					\
	.data_dva	= 0xec000ULL,					\
	.data_literal	= 0x1f0000ec000ULL,				\
	.data_owned	= !(data),					\
	.tag_cpad	= (cpad),					\
	.tag_wrad	= (wrad),					\
	.tag_ioba	= (ioba),					\
}

#define T6002_DEV_ROWS {						\
	[AVE_ABI_MACOS_13_5] = { .dev_id = 14, .dev_type = 11, .chip_type = 8 }, \
	[AVE_ABI_MACOS_26_6] = { .dev_id = 11, .dev_type = 9, .chip_type = 6 }, \
}

static const struct ave_soc ave_soc_t6002_ave0 = {
	.name			= "t6002",
	.pipe_diag		= true,		/* t6001's ave0 hardware */
	.dpe			= &ave_dpe_set_castor_6000,	/* DevID 14-16 share it */
	.dpe_phys		= 0x40d100000ULL,
	.inst			= 0,
	.fw_name		= "apple/ave_h13d.bin",
	.fw_pristine_name	= T6002_AVE0_PRISTINE_NAME,
	.fw_pristine_sha256	= T6002_AVE0_PRISTINE_SHA256,
	.data_stkg_off		= 0x3a38,
	.text_win		= T6002_TEXT_WIN,
	.dev			= T6002_DEV_ROWS,

	/* ADT dart-ave0, as t6001 */
	.cpudart_phys		= 0x40d040000ULL,
	.dapf_phys		= 0x40d044000ULL,
	.dart1_phys		= 0x40d030000ULL,
	.smmu_phys		= 0x40d020000ULL,
	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x40d050000ULL, 0x40dc69000ULL },
	.dapf_mmio_adt		= { 0x506000000ULL, 0x507c6c000ULL },	/* ave1's span */
	.dapf_by_driver		= true,

	/* iBoot's own DATA; the tags are only used for owned DATA */
	.iboot = T6002_IBOOT(T6002_AVE0_TEXT_PHYS, T6002_AVE0_DATA_PHYS, 0, 0, 0),

	.me1_node		= "/soc@200000000/power-management@28e580000/power-controller@8020",
	.me1_label		= "venc_me1",
	.pmp_report_node	= "/soc@200000000/pmp_report@28e3c0000/report@10",

	/* tools/pmp_ptd_map.py on the j375d ADT: t6001's values, C1-C4 ok */
	.pmp_ps_reg		= 0x28e0802d8ULL,
	.pmp_report_base	= 0x28e3c0000ULL,
	.pmp_dvfs_wr		= 0x28e3d0888ULL,
	.pmp_dvfs_rd		= 0x28e3c1110ULL,
	.pmgr_perf_blk		= 0x28e580000ULL + 0x58000,
};

/* Die 0's second encoder: t6001's ave1 */
static const struct ave_soc ave_soc_t6002_ave1 = {
	.name			= "t6002-ave1",
	.pipe_diag		= true,
	.dpe			= &ave_dpe_set_castor_6000,
	.dpe_phys		= 0x507100000ULL,
	.inst			= 1,
	.fw_name		= "apple/ave_h13d.bin",
	.fw_pristine_name	= T6002_AVE1_PRISTINE_NAME,
	.fw_pristine_sha256	= T6002_AVE1_PRISTINE_SHA256,
	.data_stkg_off		= 0x3a38,
	.text_win		= T6002_TEXT_WIN,
	.dev			= T6002_DEV_ROWS,

	.cpudart_phys		= 0x507040000ULL,
	.dapf_phys		= 0x507044000ULL,
	.dart1_phys		= 0x507030000ULL,
	.smmu_phys		= 0x507020000ULL,
	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x507050000ULL, 0x507c69000ULL },
	.dapf_mmio_adt		= { 0x40d050000ULL, 0x40dc69000ULL },	/* ave0's span */
	.dapf_by_driver		= true,

	.iboot = T6002_IBOOT(T6002_AVE1_TEXT_PHYS, T6002_AVE1_DATA_PHYS,
			     0x507800000ULL, 0x507c00000ULL, 0x506000000ULL),

	.me1_node		= "/soc@200000000/power-management@28e680000/power-controller@8020",
	.me1_label		= "venc1_me1",
};

/*
 * Die 1. CPU physical = die 0's + 0x2000000000 for the encoder and its
 * DARTs; its PMGR is die 1's (soc@2200000000, power-management@28e580000 =
 * 0x228e580000). The owned-DATA tags assume the firmware addresses its
 * own block by these global addresses, as ave0/ave1 do; docs/98 U0 checks
 * that against iBoot's own tags where iBoot loaded a die-1 DATA.
 */
static const struct ave_soc ave_soc_t6002_ave2 = {
	.name			= "t6002-ave2",
	.pipe_diag		= true,
	.dpe			= &ave_dpe_set_castor_6000,
	.dpe_phys		= 0x240d100000ULL,
	.inst			= 2,
	.fw_name		= "apple/ave_h13d.bin",
	.fw_pristine_name	= T6002_AVE2_PRISTINE_NAME,
	.fw_pristine_sha256	= T6002_AVE2_PRISTINE_SHA256,
	.data_stkg_off		= 0x3a38,
	.text_win		= T6002_TEXT_WIN,
	.dev			= T6002_DEV_ROWS,

	/* ADT dart-ave2 reg[0..3], bus 0x220d040000.. + 0x200000000 */
	.cpudart_phys		= 0x240d040000ULL,
	.dapf_phys		= 0x240d044000ULL,
	.dart1_phys		= 0x240d030000ULL,
	.smmu_phys		= 0x240d020000ULL,
	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x240d050000ULL, 0x240dc69000ULL },
	/* dart-ave2 filter entry 1: ave3's SVE..ASC (not its axi2af, unlike ave0's) */
	.dapf_mmio_adt		= { 0x2507050000ULL, 0x2507c6c000ULL },
	.dapf_by_driver		= true,

	.iboot = T6002_IBOOT(T6002_AVE2_TEXT_PHYS, T6002_AVE2_DATA_PHYS,
			     0x240d800000ULL, 0x240dc00000ULL, 0x240c000000ULL),

	.me1_node		= "/soc@2200000000/power-management@28e580000/power-controller@8020",
	.me1_label		= "venc_me1_die1",
};

static const struct ave_soc ave_soc_t6002_ave3 = {
	.name			= "t6002-ave3",
	.pipe_diag		= true,
	.dpe			= &ave_dpe_set_castor_6000,
	.dpe_phys		= 0x2507100000ULL,
	.inst			= 3,
	.fw_name		= "apple/ave_h13d.bin",
	.fw_pristine_name	= T6002_AVE3_PRISTINE_NAME,
	.fw_pristine_sha256	= T6002_AVE3_PRISTINE_SHA256,
	.data_stkg_off		= 0x3a38,
	.text_win		= T6002_TEXT_WIN,
	.dev			= T6002_DEV_ROWS,

	.cpudart_phys		= 0x2507040000ULL,
	.dapf_phys		= 0x2507044000ULL,
	.dart1_phys		= 0x2507030000ULL,
	.smmu_phys		= 0x2507020000ULL,
	.dapf_window		= { 0x1f000000000ULL, 0x1f0fffffffcULL },
	.dapf_mmio_own		= { 0x2507050000ULL, 0x2507c69000ULL },
	.dapf_mmio_adt		= { 0x240d050000ULL, 0x240dc69000ULL },	/* ave2's span */
	.dapf_by_driver		= true,

	.iboot = T6002_IBOOT(T6002_AVE3_TEXT_PHYS, T6002_AVE3_DATA_PHYS,
			     0x2507800000ULL, 0x2507c00000ULL, 0x2506000000ULL),

	.me1_node		= "/soc@2200000000/power-management@28e680000/power-controller@8020",
	.me1_label		= "venc1_me1_die1",
};

static const struct ave_soc *const ave_soc_t6002_rows[] = {
	&ave_soc_t6002_ave0, &ave_soc_t6002_ave1,
	&ave_soc_t6002_ave2, &ave_soc_t6002_ave3,
};

const struct ave_soc_set ave_soc_set_t6002 = {
	.rows	= ave_soc_t6002_rows,
	.n	= ARRAY_SIZE(ave_soc_t6002_rows),
};

const struct ave_soc *ave_soc_pick(const struct ave_soc_set *set,
				   phys_addr_t dpe_phys)
{
	unsigned int i;

	for (i = 0; set && i < set->n; i++)
		if (set->rows[i]->dpe_phys == dpe_phys)
			return set->rows[i];
	return NULL;
}
