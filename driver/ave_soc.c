// SPDX-License-Identifier: GPL-2.0-only
/*
 * The per-SoC table (ave_soc.h). One row per SoC the driver has been
 * brought up on; docs/79 is the checklist for adding one.
 */
#include <linux/kernel.h>

#include "ave_soc.h"

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

const struct ave_soc *ave_soc_pick(const struct ave_soc_set *set,
				   phys_addr_t dpe_phys)
{
	unsigned int i;

	for (i = 0; set && i < set->n; i++)
		if (set->rows[i]->dpe_phys == dpe_phys)
			return set->rows[i];
	return NULL;
}
