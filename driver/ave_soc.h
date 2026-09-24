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

/* A DAPF entry's address range (the r0/r4 flags are not SoC-specific) */
struct ave_soc_range {
	u64	start;
	u64	end;		/* inclusive address of the last admitted word */
};

struct ave_soc {
	const char	*name;		/* "t6001" */

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
	} iboot;

	/* Power domains the DT cannot hand to the node (docs/57 #3, docs/78) */
	const char	*me1_node;		/* venc_me1 */
	const char	*pmp_report_node;	/* pmp-venc-sys, report@10 */

	/* PMP and PMGR (docs/75); 0 = not known for this SoC, feature refused */
	phys_addr_t	pmp_ps_reg;		/* PMP / PMS_SRAM PS registers (R1a) */
	phys_addr_t	pmp_report_base;	/* PTD read side (R1b) */
	phys_addr_t	pmp_dvfs_wr;		/* AVE0 SOC-DEV-DVFS, write side */
	phys_addr_t	pmp_dvfs_rd;		/* the same entry, read side */
	phys_addr_t	pmgr_perf_blk;		/* PMGR perf block 9 (perf_dump) */
};

extern const struct ave_soc ave_soc_t6001;

#endif /* __AVE_SOC_H__ */
