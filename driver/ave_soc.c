// SPDX-License-Identifier: GPL-2.0-only
/*
 * The per-SoC table (ave_soc.h). One row per SoC the driver has been
 * brought up on; docs/79 is the checklist for adding one.
 */
#include <linux/kernel.h>

#include "ave_soc.h"

/*
 * The 13.5 H13C image (M1 Max): sha256 of data/blobs/ave-13.5-data-pristine.bin,
 * 0x134000 bytes, as produced by tools/make_ave_data_blob.py (byte-identical
 * to what tools/extract_pristine_data.py emits), and three 16 KiB windows of
 * its __TEXT.
 */
#define AVE_H13C_PRISTINE_SHA256 {					\
	0xf1, 0xaf, 0x1e, 0xf4, 0x2b, 0xe0, 0x4a, 0x7d,			\
	0x60, 0x7b, 0x0b, 0xc1, 0xa2, 0x7d, 0xfd, 0xa5,			\
	0x72, 0x68, 0xb4, 0x76, 0xfe, 0xcf, 0x1d, 0x92,			\
	0xc8, 0xc1, 0x3c, 0x76, 0x0c, 0x71, 0xa1, 0x03, }

#define AVE_H13C_TEXT_WIN {						\
	{ 0x0, {							\
		0x88, 0x5a, 0x78, 0x37, 0x38, 0xb3, 0xf7, 0xf8,		\
		0x91, 0xad, 0x16, 0xd3, 0xd5, 0x49, 0xb7, 0xd9,		\
		0x0d, 0x11, 0x4f, 0x67, 0x7b, 0xa1, 0x12, 0xc6,		\
		0xe3, 0xe5, 0x42, 0xb9, 0x5c, 0x72, 0xb9, 0xbe, } },	\
	{ 0x80000, {							\
		0x37, 0x54, 0x7c, 0x93, 0x89, 0xfb, 0x84, 0xc3,		\
		0x2b, 0x4f, 0x88, 0x8d, 0xde, 0x06, 0x1d, 0xd7,		\
		0xd4, 0xcc, 0xb7, 0xf0, 0x3c, 0x1e, 0x49, 0xee,		\
		0xd9, 0x65, 0xfd, 0x4e, 0x9b, 0xee, 0x7f, 0xbf, } },	\
	{ 0xe8000, {							\
		0x4d, 0x57, 0x01, 0x4b, 0xa7, 0x73, 0xd5, 0x76,		\
		0xd6, 0x5d, 0x1f, 0xdb, 0xcb, 0x51, 0xa6, 0x6d,		\
		0x6d, 0x8c, 0x59, 0x95, 0x9a, 0x48, 0x29, 0x2e,		\
		0xf2, 0xcb, 0xb5, 0xb8, 0xf4, 0x17, 0x0d, 0x09, } },	\
}

/*
 * t6001, M1 Max (MacBookPro18,2/18,4), macOS 13.5 firmware. Every value
 * below was hard-coded in the driver before this table existed, and every
 * run up to f99/h3e used it.
 */
const struct ave_soc ave_soc_t6001 = {
	.name			= "t6001",
	.dpe_phys		= 0x40d100000ULL,
	.inst			= 0,
	.fw_name		= "apple/ave_h13c.bin",
	.fw_pristine_name	= "apple/ave-13.5-data-pristine.bin",

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
		.data_stkg_off	= 0x3a38,	/* docs/43 §3.4 */
	},
	.pristine_sha256	= AVE_H13C_PRISTINE_SHA256,
	.text_win		= AVE_H13C_TEXT_WIN,

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
	.dpe_phys		= 0x507100000ULL,
	.inst			= 1,
	.fw_name		= "apple/ave_h13c.bin",
	/* the same blob; ave1's copy gets its own tags (iboot.tag_*) */
	.fw_pristine_name	= "apple/ave-13.5-data-pristine.bin",

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
		.data_stkg_off	= 0x3a38,
	},
	.pristine_sha256	= AVE_H13C_PRISTINE_SHA256,
	.text_win		= AVE_H13C_TEXT_WIN,

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
	.dpe_phys		= 0x40d100000ULL,
	.inst			= 0,
	.fw_name		= "apple/ave_h13s.bin",
	.fw_pristine_name	= "apple/ave-13.5-h13s-data-pristine.bin",

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
		.data_stkg_off	= 0x3380,
	},
	.pristine_sha256	= {
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

const struct ave_soc *ave_soc_pick(const struct ave_soc_set *set,
				   phys_addr_t dpe_phys)
{
	unsigned int i;

	for (i = 0; set && i < set->n; i++)
		if (set->rows[i]->dpe_phys == dpe_phys)
			return set->rows[i];
	return NULL;
}
