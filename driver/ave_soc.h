/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Per-SoC (and per-machine) facts the driver cannot read from its DT node.
 *
 * The register banks come from the node's reg entries, so they already
 * follow the DT. Everything here is something the driver used to hard-code
 * for the t6001 (M1 Max) it was brought up on. The table is selected by the
 * node's first compatible ("apple,t6001-ave"); a node that matches only the
 * generic "apple,ave" is refused. docs/79 lists how to fill in a new SoC.
 */
#ifndef __AVE_SOC_H__
#define __AVE_SOC_H__

#include <linux/types.h>

#include "ave_version.h"

/* One row of the kext's AVE_DevInfo table, as sent in the boot handshake */
struct ave_soc_devrow {
	u32	dev_id;
	u32	dev_type;
	u32	chip_type;
};

/* A 16 KiB window of the firmware's TEXT and its sha256 (fw_restore_data) */
struct ave_soc_textwin {
	u32	off;
	u8	sha[32];
};

/* A DAPF entry's address range (the r0/r4 flags are not SoC-specific) */
struct ave_soc_range {
	u64	start;
	u64	end;		/* inclusive address of the last admitted word */
};

struct ave_soc {
	const char	*name;		/* "t6001" */
	/*
	 * Which encoder of the SoC this row is. The compatible names the SoC;
	 * the node's first reg (the DPE bank) picks the row (ave_soc_pick).
	 * inst 0 keeps the names every tool knows (apple_ave, apple-ave-enc).
	 */
	phys_addr_t	dpe_phys;
	u8		inst;

	/* Firmware images for request_firmware() (docs/09) */
	const char	*fw_name;
	const char	*fw_pristine_name;	/* 13.5 DATA, fw_restore_data */

	/* Boot handshake device row, per firmware ABI, indexed by enum ave_fw_abi */
	struct ave_soc_devrow	dev[AVE_ABI_MACOS_26_6 + 1];

	/* dart-ave0 instances, AP-physical (docs/44, docs/56) */
	phys_addr_t	cpudart_phys;	/* reg[0], the ASC's DART */
	phys_addr_t	dapf_phys;	/* reg[3], = cpudart + AVE_DAPF_OFFSET */
	phys_addr_t	dart1_phys;	/* reg[1], the datapath's DART */
	phys_addr_t	smmu_phys;	/* reg[2] */

	/* DAPF MMIO entries (docs/44 addendum, docs/49) */
	struct ave_soc_range	dapf_window;	/* the shared 0x1f0 DVA window */
	struct ave_soc_range	dapf_mmio_own;	/* this AVE's SVE..ASC span */
	struct ave_soc_range	dapf_mmio_adt;	/* what the ADT lists under dart-ave0 */

	/*
	 * Where iBoot placed the 13.5 firmware on this machine (docs/43,
	 * docs/44 §2.4). Injected by iBoot, so per machine and boot chain as
	 * much as per SoC; the driver checks the live RVBAR against text_phys
	 * before trusting any of it.
	 */
	struct {
		phys_addr_t	text_phys;
		u64		text_size;
		u64		text_dva;
		phys_addr_t	data_phys;
		u64		data_size;
		u64		data_dva;
		u64		data_literal;	/* DATA's DVA as the image spells it */
		/*
		 * docs/82 ave1: iBoot loads DATA for ave0 only. true = the
		 * driver builds this instance's DATA itself - the pristine
		 * blob with the tags below - in memory it owns (data_phys 0).
		 */
		bool		data_owned;
		u64		tag_cpad, tag_wrad, tag_ioba;
		/* where iBoot's per-boot stack guard (STKG) sits inside DATA */
		u64		data_stkg_off;
	} iboot;

	/*
	 * fw_restore_data: what the pristine DATA blob hashes to, and three
	 * TEXT windows that tell this image from every sibling variant
	 * (docs/43 §3.2). Per firmware variant, so per SoC.
	 */
	u8			pristine_sha256[32];
	struct ave_soc_textwin	text_win[3];
	/* docs/84 §3: the driver programs this DART's DAPF (m1n1 does not) */
	bool		dapf_by_driver;

	/* Power domains the DT cannot hand to the node (docs/57 #3, docs/78) */
	const char	*me1_node;		/* venc_me1 */
	const char	*me1_label;		/* checked before use */
	const char	*pmp_report_node;	/* pmp-venc-sys, report@10 */

	/* PMP and PMGR (docs/75); 0 = not known for this SoC, feature refused */
	phys_addr_t	pmp_ps_reg;		/* PMP / PMS_SRAM PS registers (R1a) */
	phys_addr_t	pmp_report_base;	/* PTD read side (R1b) */
	phys_addr_t	pmp_dvfs_wr;		/* AVE0 SOC-DEV-DVFS, write side */
	phys_addr_t	pmp_dvfs_rd;		/* the same entry, read side */
	phys_addr_t	pmgr_perf_blk;		/* PMGR perf block 9 (perf_dump) */
};

/* The rows of one SoC, one per encoder instance */
struct ave_soc_set {
	const struct ave_soc *const	*rows;
	unsigned int			n;
};

extern const struct ave_soc ave_soc_t6001;
extern const struct ave_soc ave_soc_t6001_ave1;
extern const struct ave_soc_set ave_soc_set_t6001;
extern const struct ave_soc ave_soc_t6000;
extern const struct ave_soc_set ave_soc_set_t6000;

const struct ave_soc *ave_soc_pick(const struct ave_soc_set *set,
				   phys_addr_t dpe_phys);

#endif /* __AVE_SOC_H__ */
