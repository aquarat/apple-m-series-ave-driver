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

/* AVE_DPE tunables, one kext CfgSet (ave_dpe_tables.h, docs/58 5.1) */
struct ave_dpe_tunable {
	u16	off;
	u32	clear;
	u32	set;
};

/* Inside AVE_DPE (bank 0); the same on every CfgSet seen so far */
#define AVE_DPE_CAT_BASE	0xdc000
#define AVE_DPE_CAC_BASE	0xdc400

struct ave_dpe_set {
	const char			*name;	/* the kext's, "Castor_6000" */
	const struct ave_dpe_tunable	*cat_default;
	unsigned int			n_cat_default;
	const struct ave_dpe_tunable	*cac_default;
	unsigned int			n_cac_default;
	const struct ave_dpe_tunable	*cac_8bit;
	unsigned int			n_cac_8bit;
};

extern const struct ave_dpe_set ave_dpe_set_castor_6000;
extern const struct ave_dpe_set ave_dpe_set_acis_8103;
extern const struct ave_dpe_set ave_dpe_set_atlas_8112;

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
	/*
	 * Which encoder of the SoC this row is. The compatible names the SoC;
	 * the node's first reg (the DPE bank) picks the row (ave_soc_pick).
	 * inst 0 keeps the names every tool knows (apple_ave, apple-ave-enc).
	 */
	phys_addr_t	dpe_phys;
	u8		inst;

	/* AVE_DPE tunables for this SoC; NULL = unknown, dpe_tunables is refused */
	const struct ave_dpe_set *dpe;

	/*
	 * The session diagnostics read pipe registers (AVE_DPE +0x10140,
	 * +0x20010, the MCPU blocks, ...) at offsets taken from the H13C
	 * firmware. true = that map holds on this SoC. A read outside a real
	 * block is an SError and a reset (f38), so it is off until shown.
	 */
	bool		pipe_diag;

	/* Firmware images for request_firmware() (docs/09) */
	const char	*fw_name;
	const char	*fw_pristine_name;	/* 13.5 DATA, fw_restore_data */
	/*
	 * What makes that blob this image's (docs/51): the blob's sha256,
	 * where iBoot's per-boot stack guard sits in DATA, and the sha256 of
	 * three 16 KiB windows of TEXT as it is in DRAM on this machine.
	 */
	u8		fw_pristine_sha256[32];
	u32		data_stkg_off;
	struct {
		u32	off;
		u8	sha[32];
	} text_win[3];

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
		 * Where this instance's DATA comes from (AVE_DATA_*).
		 * docs/82 ave1: iBoot loads DATA for ave0 only, so ave1's is
		 * AVE_DATA_OWNED (= true): the driver builds it itself - the
		 * pristine blob with the tags below - in memory it owns
		 * (data_phys 0). docs/89 §6: on a stock-m1n1 Mac mini iBoot's
		 * DATA lies in Linux's System RAM, so t8103 is
		 * AVE_DATA_OWNED_IF_RAM, decided at probe.
		 */
		u8		data_owned;
		/*
		 * docs/90 (t8112): RVBAR holds TEXT's DVA (text_dva, full width),
		 * not its physical address, and the core fetches TEXT and DATA
		 * through the DART; TEXT is always mapped at text_dva.
		 */
		bool		translated;
		/*
		 * Owned DATA: the tag values this instance's DATA must carry
		 * (CpAd = the ASC bank, WrAd = CpAd + 0x400000, IOBA = the
		 * fabric bank or 0), and the SoC id (SOC_, 0 = not checked).
		 */
		u64		tag_cpad, tag_wrad, tag_ioba;
		u32		tag_soc;
		/*
		 * Owned DATA only: also accept a pristine blob that is not the
		 * pinned fw_pristine_sha256 if it verifies against fw_name's
		 * __DATA byte for byte outside iBoot's fill set (the tag
		 * payloads and the _rtk_tunables region) and carries the tags
		 * above (tools/data_blob_from_image.py, docs/89 §6).
		 */
		bool		blob_by_image;
	} iboot;
	/* docs/84 §3: the driver programs this DART's DAPF (m1n1 does not) */
	bool		dapf_by_driver;
	/*
	 * dart-ave is a "dart,t8110" (t8112 and later): the newer register
	 * layout (ave_dapf.c ave_dart_t8110) and a DAPF entry with r20 at +0x20.
	 * false = the t8020/t6000 layout every earlier row uses.
	 */
	bool		dart_t8110;
	/*
	 * docs/90: the firmware's source reader needs the picture size from
	 * PICMGMT +0x964 for linear input too (H14G); the H13x builds read it
	 * only for compressed input, so it is not sent there.
	 */
	bool		src_dims;

	/* Power domains the DT cannot hand to the node (docs/57 #3, docs/78) */
	const char	*me1_node;		/* venc_me1 */
	const char	*me1_label;		/* checked before use */
	/* venc_me0, where it has no phandle either (t8103); NULL = in the DT node */
	const char	*me0_node;
	const char	*me0_label;
	const char	*pmp_report_node;	/* pmp-venc-sys, report@10 */

	/* PMP and PMGR (docs/75); 0 = not known for this SoC, feature refused */
	phys_addr_t	pmp_ps_reg;		/* PMP / PMS_SRAM PS registers (R1a) */
	phys_addr_t	pmp_report_base;	/* PTD read side (R1b) */
	phys_addr_t	pmp_dvfs_wr;		/* AVE0 SOC-DEV-DVFS, write side */
	phys_addr_t	pmp_dvfs_rd;		/* the same entry, read side */
	phys_addr_t	pmgr_perf_blk;		/* PMGR perf block 9 (perf_dump) */
};

/* ave_soc.iboot.data_owned */
#define AVE_DATA_IBOOT		0	/* iBoot's DATA at data_phys, mapped in place */
#define AVE_DATA_OWNED		1	/* always the driver's own copy (= true) */
#define AVE_DATA_OWNED_IF_RAM	2	/* own copy iff iBoot's DATA is in System RAM */

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
extern const struct ave_soc ave_soc_t8103;
extern const struct ave_soc_set ave_soc_set_t8103;
extern const struct ave_soc ave_soc_t8112;
extern const struct ave_soc_set ave_soc_set_t8112;

const struct ave_soc *ave_soc_pick(const struct ave_soc_set *set,
				   phys_addr_t dpe_phys);

#endif /* __AVE_SOC_H__ */
