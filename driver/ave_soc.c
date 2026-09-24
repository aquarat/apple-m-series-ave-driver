// SPDX-License-Identifier: GPL-2.0-only
/*
 * The per-SoC table (ave_soc.h). One row per SoC the driver has been
 * brought up on; docs/79 is the checklist for adding one.
 */
#include "ave_soc.h"

/*
 * t6001, M1 Max (MacBookPro18,2/18,4), macOS 13.5 firmware. Every value
 * below was hard-coded in the driver before this table existed, and every
 * run up to f99/h3e used it.
 */
const struct ave_soc ave_soc_t6001 = {
	.name			= "t6001",
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
	},

	.me1_node		= "/soc/power-management@28e580000/power-controller@8020",
	.pmp_report_node	= "/soc/pmp_report@28e3c0000/report@10",

	/* tools/pmp_ptd_map.py, controls C1-C4 (docs/75 §3, §11) */
	.pmp_ps_reg		= 0x28e0802d8ULL,	/* ADT ps-regs[5], psidx 27/28 */
	.pmp_report_base	= 0x28e3c0000ULL,	/* pmgr reg[41] */
	.pmp_dvfs_wr		= 0x28e3d0888ULL,	/* entry 273 = 264 + slot 9 */
	.pmp_dvfs_rd		= 0x28e3c1110ULL,
	.pmgr_perf_blk		= 0x28e580000ULL + 0x58000,	/* perf-regs[9] */
};
