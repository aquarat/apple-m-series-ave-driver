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
/*
 * Addresses below are CPU PHYSICAL. The ADT stores BUS addresses, and the
 * /arm-io "ranges" property maps bus 0x0 -> parent 0x2_00000000 for the first
 * 16 GB, so essentially every peripheral address needs +0x2_00000000.
 *
 * Eight bring-up attempts used the raw ADT values and therefore accessed an
 * undecoded hole, which hangs the fabric exactly like an unresponsive device.
 * Run tools/check_addrs.py before trusting any address in this file.
 */
#define AVE_BANK_DPE		0	/* 0x40D100000 + 0x45C000, AVE_DPE      */
#define AVE_BANK_ASC		1	/* 0x40D800000 + 0x800000, coprocessor  */
#define AVE_BANK_SVE		2	/* 0x40D050000 +   0x8000, doorbell etc */
#define AVE_BANK_PMGR_PS	3	/* 0x28E588000 +     0x24  - see below  */
#define AVE_BANK_FABRIC		4	/* 0x40C000000 + 0x1000000 - see below  */

/*
 * IMPORTANT: banks 3 and 4 are NOT AVE address space. Both are PMGR-owned
 * windows that the ADT hands to the AVE node, and both were misclassified in
 * earlier revisions of this header.
 *
 * Bank 3 is PMGR ps-regs[13] = reg window 2 (0x28E580000) + 0x8000, i.e. the
 * power-state registers for the five real VENC gates:
 *
 *   +0x00 VENC_DMA   +0x08 VENC_PIPE4  +0x10 VENC_PIPE5
 *   +0x18 VENC_ME0   +0x20 VENC_ME1
 *
 * The declared size of 0x24 is exactly ME1's register plus four bytes, which
 * is how the identification was confirmed. ave1 corroborates: its bank 3 is
 * 0x8E680260 + 0x7DC4, spanning VENC1_SYS to VENC1_ME1.
 *
 * Bank 4 is the PMGR fabric bridge window: bridge-reg-index is 48, VENC_SYS
 * owns bridge subdev 2, and pmgr reg[50] is exactly 0x40C000000 + 0x1000000.
 * AVE_AXI2AF operates on this window - so it configures a PMGR fabric bridge,
 * not an AVE register block.
 *
 * Consequence: only banks 0, 1 and 2 are AVE address space, and those are the
 * three that hang. Banks 3 and 4 are PMGR space, which is demonstrably
 * accessible - test/psdump reads bank 3 addresses successfully.
 */

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
 * CPU_STATUS bit names, from m1n1 proxyclient/m1n1/hw/asc.py. m1n1 itself
 * marks IRQ_NOT_PEND and FIQ_NOT_PEND as guesses.
 */
#define AVE_ASC_ST_RUNNING	BIT(0)
#define AVE_ASC_ST_STOPPED	BIT(1)
#define AVE_ASC_ST_IRQ_NOT_PEND	BIT(2)
#define AVE_ASC_ST_FIQ_NOT_PEND	BIT(3)
#define AVE_ASC_ST_IDLE		BIT(5)

/*
 * ASC timebase, from AVE_IOP_GetCurrTime64 (0xfffffe0008c400e8) for the
 * 0x400000 family: counter / (freq / 1e6) = microseconds. A timebase can tick
 * with the CPU held in reset, so it shows the block is clocked, not that the
 * core executes - see docs/42 §7.
 */
#define AVE_ASC_TIMER		0x178000	/* 64-bit */
#define AVE_ASC_TIMER_FREQ	0x160020	/* 32-bit */

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
 *
 * VERIFIED against AVE_IOP_Config_Nyx:
 *
 *   and  x8, x20, #0x3fffffff800          ; fw_base & AVE_ASC_FW_BASE_MASK
 *   mov  x9, #0x102000000000000           ; AVE_ASC_FW_BASE_TAG
 *   orr  x3, x8, x9
 *   mov  w1, #0x1                         ; bank 1
 *   mov  w2, #0x50000
 *   bl   AVE_Reg::Write64
 *
 * The mask requires a 2 KiB aligned base. The tag's meaning is unknown and is
 * reproduced verbatim. Config also Read64s the same offset first, to log it.
 *
 * Apple passes the address the firmware is mapped at; we map at DART IOVA 0,
 * so for us the value reduces to the tag alone.
 *
 * Note the mask is 0x3fffffff800 (11 hex digits), not the 0x3FFFFFFFF800 that
 * an earlier revision of this file recorded.
 */
/*
 * The /arm-io bus->AP-physical translation. ADT reg entries are bus
 * addresses; Linux nodes carry the translated ones. Subtracting this is how
 * we recover what the coprocessor itself must be told - see
 * docs/30-address-translation-bug.md for what happens when the two are
 * confused in the other direction.
 */
#define AVE_ARM_IO_BUS_OFFSET	0x200000000ULL

/*
 * The firmware's own I/O window, as it must appear in the image's IOBA tag:
 * the 32 MiB region based at bus 0x20C000000 (bank 4's AP-physical
 * 0x40C000000 less the translation). The firmware forms every register
 * address as base + a fixed offset, e.g. base + 0x1800000 = the ASC bank and
 * base + 0x1050000 = the SVE bank.
 */
#define AVE_IOBA_SIZE		0x2000000

#define AVE_ASC_FW_BASE		0x50000
#define AVE_ASC_FW_BASE_MASK	0x3fffffff800ULL
#define AVE_ASC_FW_BASE_TAG	0x0102000000000000ULL


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
 * Power domains.
 *
 * IMPORTANT: the ADT power-gates list is NOT the AVE_PMGR PD enum order, and
 * it is not all power gates. Resolving the eleven ADT gate ids against the
 * ADT's own PMGR device table (/arm-io/pmgr "devices", matched on id2) gives:
 *
 *   456 VENC-SYS-V      457 VENC-SOC-VNOM   458 VENC-MEM-FAST
 *   300 VENC_DMA        301 VENC_PIPE4      302 VENC_PIPE5
 *   303 VENC_ME0        304 VENC_ME1
 *   459 VENC-SOC-VMAX   600 VENC-SOC-VMID2  602 VENC-FAB0-VMAX
 *
 * Only the five VENC_* entries have a real ps register (psreg 13, psidx 0..4).
 * The six hyphenated ones are voltage/performance states with psreg 0, which
 * is consistent with docs/10 finding that AVE's "PStates" are a clock ladder
 * with no DVFS values anywhere in the kext.
 *
 * Linux models six per instance and ALREADY encodes the dependency tree, so
 * genpd brings up the whole chain from a leaf and the driver does not have to
 * walk it:
 *
 *   ... -> venc_sys -> venc_dma -> venc_pipe4
 *                              -> venc_pipe5 -> venc_me0 -> venc_me1
 *
 * That matches the tree recovered from AVE_PMGR in docs/10 (AVC uses the
 * pipe4 branch, HEVC and LRME the pipe5 branch), independently confirmed here
 * by Asahi's own device tree.
 *
 * Referencing venc_pipe4 and venc_me1 is therefore sufficient to power
 * everything.
 */
enum ave_power_domain {
	AVE_PD_SYS	= 0,	/* venc_sys   - root                        */
	AVE_PD_DMA	= 1,	/* venc_dma   - datapath front end          */
	AVE_PD_PIPE4	= 2,	/* venc_pipe4 - AVC branch (HME)            */
	AVE_PD_PIPE5	= 3,	/* venc_pipe5 - HEVC/LRME branch (MDINTRA)  */
	AVE_PD_ME0	= 4,	/* venc_me0                                 */
	AVE_PD_ME1	= 5,	/* venc_me1   - deepest leaf                */
	AVE_PD_COUNT	= 6,
};

/* The DT node lists the two leaves; genpd pulls their ancestors up. */
#define AVE_PD_LEAVES	2

/* Power states (AVE_PMGR). */
enum ave_power_state {
	AVE_PS_POWER_OFF	= 0,
	AVE_PS_CLOCK_OFF	= 1,
	AVE_PS_CLOCK_ON		= 2,
};

/*
 * SoC identification. AVE_DevInfo::RetrieveDevID reads the ADT "soc-id"
 * property and looks it up in a 34-entry table at 0xfffffe0007edba00.
 *
 * The DevID is per ENCODER INSTANCE, not per machine: on this t6001 board
 * /arm-io/ave0 carries soc-id t6000 and /arm-io/ave1 carries t6001, so ave0
 * is DevID 11 (_Castor) and ave1 is 12 (_Nyx). We drive ave0, so we send 11.
 *
 * Getting this wrong is quiet rather than loud. The firmware indexes a table
 * at __DATA 0x135c60 by DevID with stride 0x48 and no bounds check
 * (fw 0x21d6c); rows 11 and 12 are both valid but carry different device and
 * capability descriptors, so a wrong value is accepted and mis-configures the
 * engine instead of being rejected.
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
