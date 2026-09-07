/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE (Video Encoder) - hardware definitions
 *
 * Every constant here was read out of AppleAVE2.kext or the AVE firmware and
 * re-verified against the binary. The comment on each gives the instruction
 * address it came from, so it can be re-checked with:
 *
 *     python3 tools/disas.py --kext --addr <VA> -n 0x40
 *
 * Anything not verified is marked UNVERIFIED and must not be relied on.
 * See docs/00-methodology.md before adding to this file.
 */
#ifndef __AVE_HW_H__
#define __AVE_HW_H__

/*
 * MMIO banks.
 *
 * AVE_Reg::Init (0xfffffe0008c532d0) maps the ADT "reg" entries by index with
 * mapDeviceMemoryWithIndex and caches them in an array, so a "bank" in the kext
 * is exactly an ADT reg index. ave0 on t6001 has five.
 */
#define AVE_BANK_DPE		0	/* 0x20D100000 + 0x45C000, AVE_DPE      */
#define AVE_BANK_ASC		1	/* 0x20D800000 + 0x800000, coprocessor  */
#define AVE_BANK_SVE		2	/* 0x20D050000 +   0x8000, doorbell etc */
#define AVE_BANK_UNKNOWN3	3	/* 0x8E588000  +     0x24, purpose unknown */
#define AVE_BANK_AXI2AF		4	/* 0x20C000000 + 0x1000000, AVE_AXI2AF */

#define AVE_NUM_BANKS		5

/*
 * ASC (coprocessor) block, within AVE_BANK_ASC.
 * From AVE_IOP_Start_Nyx (0xfffffe0008c35628) and AVE_IOP_CheckIdle_Nyx.
 * _Nyx is the variant t6001 selects; _Acis/_Castor/_Atlas/_Hera use identical
 * offsets, so these are not SoC specific.
 */
#define AVE_ASC_BASE		0x400000

#define AVE_ASC_CPU_CONTROL	(AVE_ASC_BASE + 0x044)	/* 0xfffffe0008c356d4 */
#define AVE_ASC_CPU_STATUS	(AVE_ASC_BASE + 0x048)	/* CheckIdle_Nyx      */
#define AVE_ASC_AUX_400		(AVE_ASC_BASE + 0x400)
#define AVE_ASC_AUX_808		(AVE_ASC_BASE + 0x808)

#define AVE_ASC_CPU_RUN		0x10	/* written to CPU_CONTROL to start */
#define AVE_ASC_AUX_400_VAL	0x10000	/* meaning unknown */
#define AVE_ASC_AUX_808_VAL	0x1	/* meaning unknown */
#define AVE_ASC_STATUS_BUSY	0x3	/* idle when (status & 0x3) == 0 */

/*
 * Start sequence, in order (AVE_IOP_Start_Nyx). All writes are to AVE_BANK_ASC:
 *
 *	write32(AVE_ASC_AUX_808,     AVE_ASC_AUX_808_VAL);
 *	write32(AVE_ASC_CPU_CONTROL, 0);
 *	write32(AVE_ASC_AUX_400,     AVE_ASC_AUX_400_VAL);
 *	write32(AVE_ASC_CPU_CONTROL, AVE_ASC_CPU_RUN);
 *
 * Nothing in the kext ever clears CPU_CONTROL again; shutdown is by sending the
 * Halt command and polling scratch register 0.
 */

/*
 * Firmware base-address handoff, within AVE_BANK_ASC.
 * UNVERIFIED: reported as a 64-bit write of
 * (fw_base & 0x3FFFFFFFF800) | 0x0102000000000000. The high tag is unexplained.
 */
#define AVE_ASC_FW_BASE		0x50000

/*
 * SVE control block, within AVE_BANK_SVE. Offsets come from a per-SoC table;
 * the t6000/t6001 table is at 0xfffffe00072748b4 and reads
 * 0x0c, 0x10, 0x08, 0x18, 0x1c, 0x20, 0x24, 0x28.
 * Proof that these are bank 2: AVE_SVECtrl::SetIntr (0xfffffe0008c91510) does
 * ldr w2,[x8] (table[0]) then mov w1,#0x2 then AVE_Reg::Write32.
 */
#define AVE_SVE_DOORBELL	0x0c	/* write (1 << channel_bit)          */
#define AVE_SVE_INTR_STATUS	0x10	/* write-1-to-clear the value read    */
#define AVE_SVE_REG_08		0x08	/* table[2], purpose unknown          */
#define AVE_SVE_SCRATCH(n)	(0x18 + 4 * (n))	/* n = 0..7          */
#define AVE_SVE_IDLE		0x38	/* AVE_SVECtrl::SetIdle              */

#define AVE_SVE_NUM_SCRATCH	8

/* Handshake value polled in scratch 0 after Halt. UNVERIFIED. */
#define AVE_SCRATCH0_STOPPED	0x08042006

/*
 * Interrupts. AVE_Drv::IO_start registers exactly ONE handler, at ADT
 * interrupts index 0 (mov w4,#0x0 at 0xfffffe0008bdb214). No other interrupt
 * registration exists anywhere in the kext. For ave0 that is AIC 1031; the
 * remaining four ADT interrupts (1024..1027) are unclaimed by the host and
 * their purpose is unknown.
 */
#define AVE_IRQ_INDEX		0

/*
 * Power domains, in ADT power-gates order.
 * Names from the kext table at 0xfffffe0007ee0e18.
 * AVE_PMGR performs no MMIO at all - it drives AppleARMIODevice by gate index -
 * so Linux's apple-pmgr-pwrstate covers this and only the ordering is
 * AVE specific.
 *
 * Power-up is a two-branch tree rooted at IOP:
 *   perf ladder : IOP -> IOP_MID -> IOP_MID2 -> IOP_MAX
 *   datapath    : IOP -> DMA_FE -> {PIPE4_HME | PIPE5_MDINTRA} -> ME0 -> ME1
 * FAB is independent. DCS is absent on t6001.
 * AVC uses the PIPE4_HME branch; HEVC and LRME use PIPE5_MDINTRA.
 */
enum ave_power_domain {
	AVE_PD_IOP		= 0,
	AVE_PD_IOP_MID		= 1,
	AVE_PD_IOP_MID2		= 2,
	AVE_PD_IOP_MAX		= 3,
	AVE_PD_DMA_FE		= 4,
	AVE_PD_PIPE4_HME	= 5,
	AVE_PD_PIPE5_MDINTRA	= 6,
	AVE_PD_ME0		= 7,
	AVE_PD_ME1		= 8,
	AVE_PD_FAB		= 9,
	AVE_PD_DCS		= 10,	/* absent on t6001 */
	AVE_PD_COUNT		= 11,
};

/* Power states (AVE_PMGR). */
enum ave_power_state {
	AVE_PS_POWER_OFF	= 0,
	AVE_PS_CLOCK_OFF	= 1,
	AVE_PS_CLOCK_ON		= 2,
};

/*
 * SoC identification. AVE_DevInfo::RetrieveDevID reads the ADT "soc-id"
 * property and looks it up in a 34-entry table at 0xfffffe0007edba00.
 */
#define AVE_DEVID_T8103		10
#define AVE_DEVID_T6000		11
#define AVE_DEVID_T6001		12

#define AVE_DEVTYPE_T6000	9
#define AVE_DEVTYPE_T6001	10

#define AVE_CHIPTYPE_ACIS	5	/* t8103 */
#define AVE_CHIPTYPE_CASTOR	6	/* t6000 */
#define AVE_CHIPTYPE_NYX	7	/* t6001 */

#endif /* __AVE_HW_H__ */
