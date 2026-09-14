// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE - CPUDART and DAPF: read-only dump (docs/44 E2) and filter
 * programming (docs/44 E3).
 *
 * UNTESTED. Compiled only; nothing here has run on hardware. Read
 * docs/44-reset-fetch-path.md and docs/48-e1-e3-procedure.md first.
 *
 * Why this exists: every AVE DART fault we logged is code 0x800, which is
 * NO_DAPF_MATCH (m1n1 proxyclient/m1n1/hw/dart8020.py R_ERROR bit 11). The
 * DART's address filter (DAPF) refused the fetch before any page-table walk.
 * Linux never programs a DAPF; m1n1 does so only for aop/mtp/pmp/isp
 * (m1n1 src/dapf.c:173-176). AVE's DAPF is dart-ave0 reg[3], AP 0x40d044000.
 *
 * Nothing here runs unless a module parameter asks for it:
 *
 *   dapf_dump=1          E2: map and dump, reads only.
 *   dapf_set=control     E3 negative control: 0x1f0 window + MMIO, NO TEXT.
 *   dapf_set=text        E3: 0x1f0 window + MMIO + iBoot TEXT (physical).
 *   dapf_mmio=ave0|adt|both|none   which MMIO entry goes with dapf_set.
 *
 * Register access discipline:
 *   - the blocks are mapped with devm_ioremap(), never requested: in the
 *     variant=3 overlay apple-dart owns 0x40d040000 and has requested it;
 *   - nothing is read or written unless the stage 6 runtime-PM reference is
 *     held (ave->powered and pm_runtime_active()), because a read of a
 *     power-gated block hangs the fabric;
 *   - the resource addresses are checked against the ADT-derived constants
 *     and against each other (DAPF = CPUDART + 0x4000) before any mapping, so
 *     a wrong overlay refuses instead of reading somewhere else (docs/00
 *     trap 7);
 *   - if the device has an "iommus" DART, it must be the same block as
 *     "cpudart", or we would be dumping a DART the core does not use.
 */

#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>

#include "ave.h"
#include "ave_dapf.h"

/*
 * DART register offsets, t8020/t6000 layout.
 *
 * Sources: Linux drivers/iommu/apple-dart.c (asahi branch, fetched
 * 2026-09-13) lines 45-89, and m1n1 proxyclient/m1n1/hw/dart8020.py
 * DART8020Regs, which agree on every offset both define. PARAMS1/2 are from
 * apple-dart.c only; REMAP, DAPF_LOCK and UNK1 are from dart8020.py only.
 */
#define DART_PARAMS1		0x00	/* apple-dart.c:45 */
#define DART_PARAMS2		0x04	/* apple-dart.c:48, bit 0 bypass support */
#define DART_ERROR		0x40	/* apple-dart.c:59, dart8020.py ERROR */
#define DART_ERROR_ADDR_LO	0x50	/* apple-dart.c:76 */
#define DART_ERROR_ADDR_HI	0x54	/* apple-dart.c:75 */
#define DART_CONFIG		0x60	/* apple-dart.c:70, bit 15 LOCK */
#define DART_REMAP(i)		(0x80 + 4 * (i))	/* dart8020.py REMAP */
#define DART_DAPF_LOCK		0xf0	/* dart8020.py, bit 0 LOCK */
#define DART_UNK1		0xf8	/* dart8020.py */
#define DART_ENABLED_STREAMS	0xfc	/* apple-dart.c:78 */
#define DART_TCR(sid)		(0x100 + 4 * (sid))		/* apple-dart.c:80 */
#define DART_TTBR(sid, i)	(0x200 + 0x10 * (sid) + 4 * (i))	/* :85, x4 */
#define DART_MAP_SIZE		0x400	/* covers every offset above */

#define DART_CONFIG_LOCK	BIT(15)
#define DART_DAPF_LOCK_BIT	BIT(0)
#define DART_TCR_TRANSLATE	BIT(7)	/* apple-dart.c:81 */
#define DART_TCR_BYPASS_DART	BIT(8)	/* apple-dart.c:82 */
#define DART_TCR_BYPASS_DAPF	BIT(12)	/* apple-dart.c:83 */
#define DART_TTBR_VALID		BIT(31)

/* ERROR bits, dart8020.py R_ERROR. apple-dart.c names only bits 0-4. */
#define DART_ERR_FLAG		BIT(31)
#define DART_ERR_STREAM(v)	(((v) >> 24) & 0xf)
#define DART_ERR_NO_DAPF_MATCH	BIT(11)
#define DART_ERR_WRITE		BIT(10)
#define DART_ERR_SUBPAGE_PROT	BIT(7)
#define DART_ERR_PTE_READ_FAULT	BIT(6)
#define DART_ERR_READ_FAULT	BIT(4)
#define DART_ERR_WRITE_FAULT	BIT(3)
#define DART_ERR_NO_PTE		BIT(2)
#define DART_ERR_NO_PMD		BIT(1)
#define DART_ERR_NO_TTBR	BIT(0)

/* DAPF entry layout, m1n1 src/dapf.c:35-41 (dapf_init_t8020). */
#define DAPF_ENTRY(i)		(0x40 * (i))
#define DAPF_R0			0x00	/* 32-bit, written last */
#define DAPF_R4			0x04	/* 32-bit */
#define DAPF_START		0x08	/* 64-bit */
#define DAPF_END		0x10	/* 64-bit, inclusive */
#define DAPF_MAP_SIZE		(0x40 * AVE_DAPF_MAX_ENTRIES)

/*
 * AVE_DAPF_END_INCLUSIVE - why @end is the last admitted byte.
 *
 * m1n1 copies the ADT's end value into the register unmodified
 * (dapf.c:38), so whatever convention the ADT uses is the hardware's. The
 * ADT answers it: dart-isp0's filter-data-instance-0 (13.5 and restore ADTs,
 * identical) contains
 *
 *   0x28ec3c000 - 0x28ec3c003      one 32-bit register
 *   0x285460000 - 0x285460003      one 32-bit register
 *   0x28e584000 - 0x28e584043      a block ending in a 32-bit register at +0x40
 *   0x28e0b8000 - 0x28e0bffff      32 KiB
 *   0x1f000000000 - 0x1f0ffffffff  4 GiB
 *
 * An exclusive end would make the first two three bytes long. So end is
 * inclusive. Inferred from data, not from a disassembled comparison.
 *
 * The odd ones out are dart-ave0/1's MMIO entries ending in ...000
 * (0x507c6c000, 0x40dc69000): read inclusively they admit one byte of the
 * following page. Programmed verbatim, as m1n1 would.
 */

/*
 * The ADT entries. /arm-io/dart-ave{0,1} filter-data-instance-0 in
 * data/blobs/macos-13.5/adt.bin, decoded with m1n1's dapf_t8020_config
 * layout (dapf.c:12-20): {u64 start, u64 end, u8 unk1, u8 r0_hi, u8 r0_lo,
 * u8 unk2, u32 r4}, r0 = (r0_hi << 4) | r0_lo. Addresses are AP-physical.
 *
 * END ADDRESSES ARE STORED WITH THE LOW TWO BITS CLEAR. E1 (2026-09-13) read
 * ISP's live DAPF, which m1n1 programmed from the ADT: 0x1f0ffffffff reads
 * back 0x1f0fffffffc, 0x28e584043 reads 0x28e584040. So the register holds
 * the inclusive address of the last admitted 4-byte word. Programming the
 * ADT value verbatim would fail the readback check, so the values here are
 * already masked.
 */
static const struct ave_dapf_entry ave_dapf_window = {
	.start = 0x1f000000000ULL, .end = 0x1f0fffffffcULL,
	.r0 = 0x33, .r4 = 1,
	.what = "0x1f0 window (dart-ave0[0], dart-ave1[0], dart-isp0[0])",
};

/* ave0's own SVE..ASC span, listed under dart-ave1 (docs/44 addendum). */
static const struct ave_dapf_entry ave_dapf_mmio_ave0 = {
	.start = 0x40d050000ULL, .end = 0x40dc69000ULL,
	.r0 = 0x31, .r4 = 1,
	.what = "MMIO ave0-own (listed as dart-ave1[1])",
};

/* What the ADT lists under dart-ave0: ave1's fabric..ASC. */
static const struct ave_dapf_entry ave_dapf_mmio_adt = {
	.start = 0x506000000ULL, .end = 0x507c6c000ULL,
	.r0 = 0x31, .r4 = 1,
	.what = "MMIO as listed (dart-ave0[1], ave1's span)",
};

/*
 * iBoot's TEXT, physical, as the bootstrap addresses it (docs/44 §2.4).
 *
 * r0 = 0x11 is copied from ISP's live DAPF (E1, 2026-09-13): slot 0 there is
 * 0x10000c68000 - 0x100015e7ffc, r0 0x11, r4 1 - exactly ISP's TEXT
 * carve-out, which is what ISP's locked RVBAR points at. That entry is not in
 * the restore ADT; iBoot injects it. The meaning of the r0 bits is still
 * unknown, but 0x11 is what a working ASC on this machine uses for the same
 * job. (This entry first copied the window's 0x33.)
 */
static const struct ave_dapf_entry ave_dapf_text = {
	.start = AVE_IBOOT_TEXT_PHYS,
	.end = AVE_IBOOT_TEXT_PHYS + AVE_IBOOT_TEXT_SIZE - 4,	/* 0x10000c13ffc */
	.r0 = 0x11, .r4 = 1,
	.what = "iBoot TEXT, physical",
};

static const struct ave_dapf_entry ave_dapf_cleared = {
	.what = "cleared",
};

static char *dapf_order = "m1n1";
module_param(dapf_order, charp, 0444);
MODULE_PARM_DESC(dapf_order,
		 "E3: m1n1 (default; ADT order, only the needed slots, r4/start/end/r0, no pre-clear) | clear16-resets-machine (all 16 slots, r0=0 first - this reset the SoC on 2026-09-13)");

static bool dapf_quiesce = true;
module_param(dapf_quiesce, bool, 0444);
MODULE_PARM_DESC(dapf_quiesce,
		 "E3: zero every TCR and ENABLED_STREAMS around the DAPF writes, then restore (default 1; docs/49 N1)");

static int dapf_probe;
module_param(dapf_probe, int, 0444);
MODULE_PARM_DESC(dapf_probe,
		 "docs/49: 1 = N1b (30 s hold reading TCR every 2 s, then a same-value TCR write); 2 = N1e (30 s hold with NO register access at all, no write); 3 = N1f (120 s of N1b's reads, no write)");

static int dapf_early;
module_param(dapf_early, int, 0444);
MODULE_PARM_DESC(dapf_early,
		 "1 = program the DAPF at stage 8 (reset the SoC, N1j); 2 = at the end of stage 6, before the stage-7 SVE idle/clock-gating write (N1k)");

int ave_dapf_early(void)
{
	return dapf_early;
}

static bool dapf_dump;
module_param(dapf_dump, bool, 0444);
MODULE_PARM_DESC(dapf_dump,
		 "E2: dump CPUDART config and all 16 DAPF entries, read-only (needs overlay variant=2 or 3)");

static char *dapf_set = "off";
module_param(dapf_set, charp, 0444);
MODULE_PARM_DESC(dapf_set,
		 "E3: off (default) | control = 0x1f0 window + MMIO, no TEXT (negative control) | text = window + MMIO + iBoot TEXT");

static char *dapf_mmio = "ave0";
module_param(dapf_mmio, charp, 0444);
MODULE_PARM_DESC(dapf_mmio,
		 "E3 MMIO entry: ave0 (default; 0x40d050000-0x40dc69000, dart-ave1's entry) | adt (0x506000000-0x507c6c000, as dart-ave0 lists it) | both | none");


static int ave_dapf_check_power(struct ave_device *ave)
{
	if (!ave->powered || !pm_runtime_active(ave->dev)) {
		dev_err(ave->dev,
			"dapf: REFUSING - no runtime-PM reference held (need stage 6); a read of a gated DART hangs the fabric\n");
		return -EPERM;
	}
	return 0;
}

/*
 * Map "cpudart" and "dapf". Idempotent. Validates the apparatus before the
 * first mapping; it performs no register access itself.
 */
static int ave_dapf_map(struct ave_device *ave)
{
	struct platform_device *pdev = to_platform_device(ave->dev);
	struct device_node *iommu_np;
	struct resource *rd, *rf, ri = {};
	int ret;

	if (ave->cpudart && ave->dapf)
		return 0;

	rd = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cpudart");
	rf = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dapf");
	if (!rd || !rf) {
		dev_err(ave->dev,
			"dapf: node has no \"cpudart\"/\"dapf\" reg entries - load test/ave-overlay.ko variant=2 or variant=3\n");
		return -ENODEV;
	}

	if (rd->start != AVE_CPUDART_PHYS || rf->start != AVE_DAPF_PHYS ||
	    rf->start != rd->start + AVE_DAPF_OFFSET) {
		dev_err(ave->dev,
			"dapf: REFUSING - cpudart %pR / dapf %pR, expected %#llx / %#llx (DAPF = CPUDART + %#x)\n",
			rd, rf, AVE_CPUDART_PHYS, AVE_DAPF_PHYS, AVE_DAPF_OFFSET);
		return -EINVAL;
	}
	if (resource_size(rd) < DART_MAP_SIZE || resource_size(rf) < DAPF_MAP_SIZE) {
		dev_err(ave->dev, "dapf: REFUSING - cpudart %pR or dapf %pR too small\n",
			rd, rf);
		return -EINVAL;
	}

	/*
	 * With a DART bound (variant=3) the dump must be of *that* DART. The
	 * fault reports have always come from 40d040000; make sure the overlay
	 * still says so.
	 */
	iommu_np = of_parse_phandle(ave->dev->of_node, "iommus", 0);
	if (iommu_np) {
		ret = of_address_to_resource(iommu_np, 0, &ri);
		of_node_put(iommu_np);
		if (ret || ri.start != rd->start) {
			dev_err(ave->dev,
				"dapf: REFUSING - iommus DART is %pR (ret %d), \"cpudart\" is %pR\n",
				&ri, ret, rd);
			return -EINVAL;
		}
	}

	ave->cpudart = devm_ioremap(ave->dev, rd->start, DART_MAP_SIZE);
	ave->dapf = devm_ioremap(ave->dev, rf->start, DAPF_MAP_SIZE);
	if (!ave->cpudart || !ave->dapf) {
		dev_err(ave->dev, "dapf: ioremap failed\n");
		ave->cpudart = NULL;
		ave->dapf = NULL;
		return -ENOMEM;
	}
	ave->cpudart_phys = rd->start;
	ave->dapf_phys = rf->start;
	dev_info(ave->dev, "dapf: mapped CPUDART %pa and DAPF %pa (not requested)\n",
		 &ave->cpudart_phys, &ave->dapf_phys);

	/*
	 * The datapath DART (0x40d030000), if the overlay attaches it
	 * (variant=4). F4 showed the encoder's DMA translating through it -
	 * and faulting there with NO_TTBR while the CPUDART was fine - so it
	 * is read alongside the CPUDART from now on. Found through the
	 * iommus phandles, never by address alone.
	 */
	ave->dart1 = NULL;
	for (int i = 1; ; i++) {
		struct of_phandle_args args;

		if (of_parse_phandle_with_args(ave->dev->of_node, "iommus",
					       "#iommu-cells", i, &args))
			break;
		ret = of_address_to_resource(args.np, 0, &ri);
		of_node_put(args.np);
		if (!ret && ri.start == AVE_DART1_PHYS) {
			ave->dart1 = devm_ioremap(ave->dev, ri.start, DART_MAP_SIZE);
			if (ave->dart1)
				dev_info(ave->dev, "dapf: mapped datapath DART %#llx (not requested)\n",
					 AVE_DART1_PHYS);
			break;
		}
	}
	return 0;
}

static void ave_dart_dump_tcr(struct ave_device *ave, void __iomem *base,
			      const char *name, unsigned int sid)
{
	u32 tcr = readl(base + DART_TCR(sid));
	unsigned int i;

	dev_info(ave->dev, "dapf:   %s TCR[%2u]  = %#010x%s%s%s\n", name, sid, tcr,
		 tcr & DART_TCR_TRANSLATE ? " TRANSLATE" : "",
		 tcr & DART_TCR_BYPASS_DART ? " BYPASS_DART" : "",
		 tcr & DART_TCR_BYPASS_DAPF ? " BYPASS_DAPF" : "");
	for (i = 0; i < 4; i++) {
		u32 t = readl(base + DART_TTBR(sid, i));

		dev_info(ave->dev, "dapf:   %s TTBR[%2u][%u] = %#010x%s\n", name,
			 sid, i, t, t & DART_TTBR_VALID ? " VALID" : "");
	}
}

static void ave_dapf_dump_tcr(struct ave_device *ave, unsigned int sid)
{
	ave_dart_dump_tcr(ave, ave->cpudart, "CPUDART", sid);
}

static void ave_dapf_dump_dart(struct ave_device *ave, const char *tag)
{
	u32 err = readl(ave->cpudart + DART_ERROR);
	u64 eaddr = ((u64)readl(ave->cpudart + DART_ERROR_ADDR_HI) << 32) |
		    readl(ave->cpudart + DART_ERROR_ADDR_LO);
	u32 cfg = readl(ave->cpudart + DART_CONFIG);
	u32 dlock;
	unsigned int i;

	dev_info(ave->dev, "dapf: [%s] CPUDART %pa\n", tag, &ave->cpudart_phys);
	dev_info(ave->dev, "dapf:   PARAMS1 = %#010x  PARAMS2 = %#010x\n",
		 readl(ave->cpudart + DART_PARAMS1),
		 readl(ave->cpudart + DART_PARAMS2));
	dev_info(ave->dev,
		 "dapf:   ERROR   = %#010x%s stream %u code %#x%s%s%s%s%s%s  addr %#llx\n",
		 err, err & DART_ERR_FLAG ? " FLAG" : "", DART_ERR_STREAM(err),
		 err & 0xfff,
		 err & DART_ERR_NO_DAPF_MATCH ? " NO_DAPF_MATCH" : "",
		 err & DART_ERR_WRITE ? " WRITE" : "",
		 err & (DART_ERR_READ_FAULT | DART_ERR_WRITE_FAULT |
			DART_ERR_PTE_READ_FAULT | DART_ERR_SUBPAGE_PROT) ? " PROT" : "",
		 err & DART_ERR_NO_PTE ? " NO_PTE" : "",
		 err & DART_ERR_NO_PMD ? " NO_PMD" : "",
		 err & DART_ERR_NO_TTBR ? " NO_TTBR" : "",
		 eaddr);
	dev_info(ave->dev, "dapf:   CONFIG  = %#010x%s\n", cfg,
		 cfg & DART_CONFIG_LOCK ? " LOCKED" : "");
	for (i = 0; i < 4; i++)
		dev_info(ave->dev, "dapf:   REMAP[%u] = %#010x\n", i,
			 readl(ave->cpudart + DART_REMAP(i)));
	dlock = readl(ave->cpudart + DART_DAPF_LOCK);
	dev_info(ave->dev, "dapf:   DAPF_LOCK = %#010x%s  UNK1(0xf8) = %#010x  ENABLED_STREAMS = %#010x\n",
		 dlock, dlock & DART_DAPF_LOCK_BIT ? " LOCKED" : "",
		 readl(ave->cpudart + DART_UNK1),
		 readl(ave->cpudart + DART_ENABLED_STREAMS));
	ave_dapf_dump_tcr(ave, 0);
	ave_dapf_dump_tcr(ave, 1);
	ave_dapf_dump_tcr(ave, 15);
	if (ave->dart1) {
		u32 e1 = readl(ave->dart1 + DART_ERROR);

		dev_info(ave->dev, "dapf: [%s] datapath DART %#llx: ERROR %#010x ENABLED_STREAMS %#010x REMAP[0] %#010x\n",
			 tag, AVE_DART1_PHYS, e1,
			 readl(ave->dart1 + DART_ENABLED_STREAMS),
			 readl(ave->dart1 + DART_REMAP(0)));
		ave_dart_dump_tcr(ave, ave->dart1, "DART1", 0);
		ave_dart_dump_tcr(ave, ave->dart1, "DART1", 1);
	}
}

/*
 * Does the datapath DART translate with the same tables as the CPUDART?
 *
 * macOS programs every dart-ave0 instance with one translation (docs/56), and
 * apple-dart does the same for every DART in iommus - the CPUDART's SIDs 0 and
 * 1 both read TTBR 0x901c0584 in F4. The encoder's DMA goes through
 * 0x40d030000 (F4: NO_TTBR faults there at the input frame's IOVA), so if its
 * stream-0 translation does not match the CPUDART's, starting the hardware
 * only buys an SMMU fault storm - and F4 ended in a machine reset shortly
 * after. Returns 0 when they match (or when there is no second DART to
 * compare), -EIO when they do not. Reads only.
 */
int ave_dart_datapath_check(struct ave_device *ave, const char *tag)
{
	u32 c_tcr, c_ttbr, d_tcr, d_ttbr;
	int ret;

	ret = ave_dapf_check_power(ave);
	if (ret)
		return ret;
	ret = ave_dapf_map(ave);
	if (ret)
		return ret;
	if (!ave->dart1) {
		dev_warn(ave->dev, "dart: [%s] no datapath DART in iommus (overlay variant=4?); cannot check\n",
			 tag);
		return -ENODEV;
	}

	c_tcr = readl(ave->cpudart + DART_TCR(0));
	c_ttbr = readl(ave->cpudart + DART_TTBR(0, 0));
	d_tcr = readl(ave->dart1 + DART_TCR(0));
	d_ttbr = readl(ave->dart1 + DART_TTBR(0, 0));
	dev_info(ave->dev, "dart: [%s] SID0 CPUDART TCR %#x TTBR %#010x | DART1 TCR %#x TTBR %#010x -> %s\n",
		 tag, c_tcr, c_ttbr, d_tcr, d_ttbr,
		 (d_ttbr == c_ttbr && d_tcr == c_tcr && (d_ttbr & DART_TTBR_VALID))
		 ? "MATCH" : "MISMATCH");
	if (d_ttbr != c_ttbr || d_tcr != c_tcr || !(d_ttbr & DART_TTBR_VALID))
		return -EIO;
	return 0;
}

static bool ave_dapf_slot_read(struct ave_device *ave, unsigned int i,
			       struct ave_dapf_entry *e)
{
	void __iomem *b = ave->dapf + DAPF_ENTRY(i);

	e->r0 = readl(b + DAPF_R0);
	e->r4 = readl(b + DAPF_R4);
	e->start = readq(b + DAPF_START);
	e->end = readq(b + DAPF_END);
	return e->r0 || e->r4 || e->start || e->end;
}

static bool ave_dapf_covers(const struct ave_dapf_entry *e, u64 addr)
{
	/* start > end is uninitialised garbage (E2), not an entry. */
	return e->r0 && e->start <= e->end && addr >= e->start && addr <= e->end;
}

static unsigned int ave_dapf_dump_entries(struct ave_device *ave, const char *tag)
{
	/* The addresses the questions in docs/44 E2 are about. */
	static const u64 probe_addr[] = {
		AVE_IBOOT_TEXT_PHYS + 0x200,	/* the faulting fetch */
		0x1f0000ec000ULL,		/* DATA through the window */
		0x40d050000ULL,			/* ave0 SVE */
		0x506000000ULL,			/* ave1 fabric */
	};
	unsigned int i, j, used = 0;

	dev_info(ave->dev, "dapf: [%s] DAPF %pa, %u entries x {r0 +0, r4 +4, start +8, end +0x10 (incl)}\n",
		 tag, &ave->dapf_phys, AVE_DAPF_MAX_ENTRIES);
	for (i = 0; i < AVE_DAPF_MAX_ENTRIES; i++) {
		struct ave_dapf_entry e;
		bool nz = ave_dapf_slot_read(ave, i, &e);
		char hits[64] = "";

		if (!nz) {
			dev_info(ave->dev, "dapf:   [%2u] empty\n", i);
			continue;
		}
		used++;
		for (j = 0; j < ARRAY_SIZE(probe_addr); j++) {
			if (ave_dapf_covers(&e, probe_addr[j])) {
				size_t l = strlen(hits);

				snprintf(hits + l, sizeof(hits) - l, " admits:%#llx",
					 probe_addr[j]);
			}
		}
		dev_info(ave->dev, "dapf:   [%2u] r0 %#06x r4 %#06x  %#013llx - %#013llx%s\n",
			 i, e.r0, e.r4, e.start, e.end, hits);
	}
	dev_info(ave->dev, "dapf: [%s] %u non-empty slot(s)\n", tag, used);
	return used;
}

/*
 * Ungated dump, for callers that have their own reason to look. The block
 * reset path (ave_drv.c, core_reset) uses it to answer the question that
 * decides whether resetting the core is viable at all: the DAPF entries are
 * written once by m1n1 at boot and Linux cannot rewrite them - a write is a
 * fatal SError (docs/49) - so if a reset clears them, nothing after it can
 * fetch and the only way back is a reboot.
 */
/*
 * FNV-1a over every field of all 16 slots, empty ones included, plus whether
 * any slot still admits the core's first fetch. A slot count is not enough:
 * the unused slots on this machine hold uninitialised junk, so the count reads
 * 16 before and after anything (r1, 2026-09-14) and could never say "no".
 */
static u64 ave_dapf_fingerprint(struct ave_device *ave, bool *admits_fetch)
{
	u64 h = 0xcbf29ce484222325ULL;
	unsigned int i;

	*admits_fetch = false;
	for (i = 0; i < AVE_DAPF_MAX_ENTRIES; i++) {
		struct ave_dapf_entry e = {};
		u64 f[4];
		unsigned int k, b;

		if (ave_dapf_slot_read(ave, i, &e) &&
		    ave_dapf_covers(&e, AVE_IBOOT_TEXT_PHYS + 0x200))
			*admits_fetch = true;
		f[0] = e.r0; f[1] = e.r4; f[2] = e.start; f[3] = e.end;
		for (k = 0; k < ARRAY_SIZE(f); k++)
			for (b = 0; b < 64; b += 8) {
				h ^= (f[k] >> b) & 0xff;
				h *= 0x100000001b3ULL;
			}
	}
	return h;
}

int ave_dapf_dump_now(struct ave_device *ave, const char *tag,
		      u64 *fingerprint, bool *admits_fetch)
{
	bool admits;
	u64 fp;
	int ret;

	ret = ave_dapf_check_power(ave);
	if (ret)
		return ret;
	ret = ave_dapf_map(ave);
	if (ret)
		return ret;

	ave_dapf_dump_dart(ave, tag);
	ave_dapf_dump_entries(ave, tag);
	fp = ave_dapf_fingerprint(ave, &admits);
	dev_info(ave->dev, "dapf: [%s] fingerprint %#018llx, TEXT fetch %s\n",
		 tag, fp, admits ? "admitted" : "NOT ADMITTED");
	if (fingerprint)
		*fingerprint = fp;
	if (admits_fetch)
		*admits_fetch = admits;
	return 0;
}

int ave_dapf_dump(struct ave_device *ave)
{
	int ret;

	if (!dapf_dump)
		return 0;

	ret = ave_dapf_check_power(ave);
	if (ret)
		return ret;
	ret = ave_dapf_map(ave);
	if (ret)
		return ret;

	dev_info(ave->dev, "dapf: E2 read-only dump (domain %s)\n",
		 iommu_get_domain_for_dev(ave->dev) ? "attached" : "none - no DART bound");
	ave_dapf_dump_dart(ave, "E2");
	ave_dapf_dump_entries(ave, "E2");
	return 0;
}

int ave_dapf_program(struct ave_device *ave,
		     const struct ave_dapf_entry *ent, unsigned int n,
		     bool preclear)
{
	unsigned int i;
	int ret, bad = 0;

	if (!n || n > AVE_DAPF_MAX_ENTRIES)
		return -EINVAL;
	ret = ave_dapf_check_power(ave);
	if (ret)
		return ret;
	ret = ave_dapf_map(ave);
	if (ret)
		return ret;

	for (i = 0; i < n; i++) {
		if (ent[i].start > ent[i].end) {
			dev_err(ave->dev, "dapf: entry %u start %#llx > end %#llx\n",
				i, ent[i].start, ent[i].end);
			return -EINVAL;
		}
	}

	if (readl(ave->cpudart + DART_DAPF_LOCK) & DART_DAPF_LOCK_BIT) {
		dev_err(ave->dev, "dapf: REFUSING - DAPF_LOCK is set; writes would be ignored\n");
		return -EPERM;
	}

	/*
	 * m1n1's dapf_init_t8020() order: r4, start, end, r0 last. With
	 * @preclear (dapf_order=clear16 only) every slot is first disabled with
	 * r0 = 0 - which is the write that reset the machine on 2026-09-13
	 * (docs/48, docs/49), so it is no longer the default.
	 *
	 * Original rationale for the pre-clear:
	 * E2 found non-zero garbage r0 in the slots E3 enables, and m1n1's
	 * order alone would leave that garbage enable in place while the range
	 * is half-written. The core is halted during this, but the r0 bits are
	 * not understood, so no slot passes through an unintended state.
	 */
	for (i = 0; i < n; i++) {
		void __iomem *b = ave->dapf + DAPF_ENTRY(i);

		if (i <= 1)
			ave_step(ave, "next: first write to DAPF slot %u (%s)", i,
				 preclear ? "r0 = 0 pre-clear" : "r4, m1n1 order");

		dev_info(ave->dev, "dapf: write [%2u] r0 %#06x r4 %#06x  %#013llx - %#013llx  %s\n",
			 i, ent[i].r0, ent[i].r4, ent[i].start, ent[i].end,
			 ent[i].what ?: "");
		if (preclear)
			writel(0, b + DAPF_R0);
		if (ent[i].end & 3)
			dev_warn(ave->dev, "dapf: [%2u] end %#llx has low bits set; the register will drop them\n",
				 i, ent[i].end);
		writel(ent[i].r4, b + DAPF_R4);
		writeq(ent[i].start, b + DAPF_START);
		writeq(ent[i].end, b + DAPF_END);
		writel(ent[i].r0, b + DAPF_R0);
	}

	ave_step(ave, "all %u DAPF slots written; next: readback", n);
	for (i = 0; i < n; i++) {
		struct ave_dapf_entry back;
		bool ok;

		ave_dapf_slot_read(ave, i, &back);
		ok = back.r0 == ent[i].r0 && back.r4 == ent[i].r4 &&
		     back.start == ent[i].start && back.end == ent[i].end;
		if (!ok) {
			bad++;
			dev_err(ave->dev,
				"dapf: readback [%2u] MISMATCH: r0 %#x r4 %#x %#llx - %#llx\n",
				i, back.r0, back.r4, back.start, back.end);
		}
	}
	if (bad) {
		dev_err(ave->dev, "dapf: %d of %u entries did not read back\n", bad, n);
		return -EIO;
	}
	ave->dapf_programmed = true;
	dev_info(ave->dev, "dapf: %u entries programmed and verified by readback\n", n);
	return 0;
}

bool ave_dapf_program_requested(void)
{
	return dapf_set && *dapf_set && !sysfs_streq(dapf_set, "off");
}

/*
 * N1b (docs/49): does the reset follow our write, or something earlier?
 *
 * Two resets each came ~3 s after the pre-start scratch writes and right after
 * a marker hold, so the write and the delay are confounded. This holds with no
 * writes at all, logging every 2 s, then performs the most innocuous write
 * possible - TCR[0] rewritten with the value it already has - and holds again.
 * Every line is picked up by the fsync-per-line capture.
 */
int ave_dapf_write_probe(struct ave_device *ave)
{
	unsigned int t;
	u32 v, back;
	int ret;

	if (!dapf_probe)
		return 0;
	ret = ave_dapf_check_power(ave);
	if (ret)
		return ret;
	ret = ave_dapf_map(ave);
	if (ret)
		return ret;

	if (dapf_probe == 3) {
		/* N1f: N1b's read loop without the write, for 120 s. */
		for (t = 0; t <= 120; t += 2) {
			dev_info(ave->dev, "PROBE N1f read loop: t=%us TCR[0]=%#x ENABLED_STREAMS=%#x\n",
				 t, readl(ave->cpudart + DART_TCR(0)),
				 readl(ave->cpudart + DART_ENABLED_STREAMS));
			msleep(2000);
		}
		dev_info(ave->dev, "PROBE N1f survived 120 s of late DART reads\n");
		return 0;
	}

	if (dapf_probe == 2) {
		/* N1e: same hold inside probe, but no register access at all. */
		for (t = 0; t <= 30; t += 2) {
			dev_info(ave->dev, "PROBE hold, NO register access: t=%us\n", t);
			msleep(2000);
		}
		dev_info(ave->dev, "PROBE N1e hold complete; returning to probe\n");
		return 0;
	}

	for (t = 0; t <= 30; t += 2) {
		dev_info(ave->dev, "PROBE hold, no DART writes: t=%us TCR[0]=%#x ENABLED_STREAMS=%#x\n",
			 t, readl(ave->cpudart + DART_TCR(0)),
			 readl(ave->cpudart + DART_ENABLED_STREAMS));
		msleep(2000);
	}

	v = readl(ave->cpudart + DART_TCR(0));
	if (dapf_probe == 4) {
		/*
		 * N1i: 60 s between the announcement and the write, printing
		 * every 2 s with no register access (N1e: such holds are
		 * safe), so there is no doubt the lines reach disk before the
		 * write happens.
		 */
		dev_info(ave->dev, "PROBE N1i next: same-value write TCR[0] <- %#x after a 60 s no-access wait\n", v);
		for (t = 0; t <= 60; t += 2) {
			dev_info(ave->dev, "PROBE N1i pre-write wait, no register access: t=%us\n", t);
			msleep(2000);
		}
		dev_info(ave->dev, "PROBE N1i writing TCR[0] now\n");
		msleep(3000);
		writel(v, ave->cpudart + DART_TCR(0));
		dev_info(ave->dev, "PROBE N1i wrote TCR[0]; readback %#x\n",
			 readl(ave->cpudart + DART_TCR(0)));
		for (t = 0; t <= 20; t += 2) {
			dev_info(ave->dev, "PROBE N1i hold after write: t=%us\n", t);
			msleep(2000);
		}
		dev_info(ave->dev, "PROBE N1i survived the write\n");
		return 0;
	}
	dev_info(ave->dev, "PROBE next: same-value write TCR[0] <- %#x (in 5 s)\n", v);
	/*
	 * 5 s, not 2: a fabric hang blocks NVMe MMIO too, so the capture's
	 * fsync of the line printed just before a hang never lands. Give this
	 * line ample time to reach disk before the write (N1h, docs/49).
	 */
	msleep(5000);
	writel(v, ave->cpudart + DART_TCR(0));
	back = readl(ave->cpudart + DART_TCR(0));
	dev_info(ave->dev, "PROBE wrote TCR[0] same value; readback %#x\n", back);

	for (t = 0; t <= 10; t += 2) {
		dev_info(ave->dev, "PROBE hold after write: t=%us\n", t);
		msleep(2000);
	}
	dev_info(ave->dev, "PROBE survived: a same-value TCR write is harmless; the value matters\n");
	return 0;
}

int ave_dapf_program_selected(struct ave_device *ave)
{
	struct ave_dapf_entry set[AVE_DAPF_MAX_ENTRIES];
	struct iommu_domain *domain;
	bool want_text, mmio_ave0, mmio_adt;
	unsigned int n = 0, nwrite, i, stale = 0;
	bool clear16 = false;
	u32 saved_tcr[16], saved_en = 0;
	u32 tcr;
	int ret;

	if (!dapf_set || !*dapf_set || sysfs_streq(dapf_set, "off"))
		return 0;
	if (ave->dapf_programmed) {
		dev_info(ave->dev, "dapf: already programmed this probe; skipping\n");
		return 0;
	}

	if (sysfs_streq(dapf_set, "control"))
		want_text = false;
	else if (sysfs_streq(dapf_set, "text"))
		want_text = true;
	else {
		dev_err(ave->dev, "dapf: unknown dapf_set=\"%s\" (off|control|text)\n",
			dapf_set);
		return -EINVAL;
	}

	if (sysfs_streq(dapf_mmio, "ave0")) {
		mmio_ave0 = true;  mmio_adt = false;
	} else if (sysfs_streq(dapf_mmio, "adt")) {
		mmio_ave0 = false; mmio_adt = true;
	} else if (sysfs_streq(dapf_mmio, "both")) {
		mmio_ave0 = true;  mmio_adt = true;
	} else if (sysfs_streq(dapf_mmio, "none")) {
		mmio_ave0 = false; mmio_adt = false;
	} else {
		dev_err(ave->dev, "dapf: unknown dapf_mmio=\"%s\" (ave0|adt|both|none)\n",
			dapf_mmio);
		return -EINVAL;
	}

	/*
	 * E3 is defined as "keep translation, let the DAPF be the backstop".
	 * Without a paging domain either no DART is bound (variant=2: TCR is
	 * whatever gating left, nobody handles faults) or the DART is in
	 * bypass, which skips the DAPF entirely.
	 */
	domain = iommu_get_domain_for_dev(ave->dev);
	if (!domain || !(domain->type & __IOMMU_DOMAIN_PAGING)) {
		dev_err(ave->dev,
			"dapf: REFUSING dapf_set=%s - needs a translating DART domain (overlay variant=3, group type DMA)\n",
			dapf_set);
		return -EINVAL;
	}

	ret = ave_dapf_check_power(ave);
	if (ret)
		return ret;
	ret = ave_dapf_map(ave);
	if (ret)
		return ret;

	tcr = readl(ave->cpudart + DART_TCR(0));
	if ((tcr & (DART_TCR_BYPASS_DAPF | DART_TCR_BYPASS_DART)) ||
	    !(tcr & DART_TCR_TRANSLATE)) {
		dev_err(ave->dev,
			"dapf: REFUSING - TCR[0] = %#x; stream 0 is not translating with the DAPF enforced\n",
			tcr);
		return -EINVAL;
	}

	ave_dapf_dump_dart(ave, "E3 before");
	ave_dapf_dump_entries(ave, "E3 before");

	/*
	 * The spelling is the interlock: the old sequence reset the machine,
	 * so it can only be selected by a name that says so.
	 */
	if (sysfs_streq(dapf_order, "clear16-resets-machine")) {
		clear16 = true;
	} else if (!sysfs_streq(dapf_order, "m1n1")) {
		dev_err(ave->dev, "dapf: unknown dapf_order=\"%s\" (m1n1|clear16-resets-machine)\n", dapf_order);
		return -EINVAL;
	}

	if (clear16) {
		/*
		 * All 16 slots, ISP-like layout: slot 0 TEXT or cleared, slot 1
		 * window, then MMIO, rest cleared. A clean negative control, but
		 * this sequence reset the machine on its first write.
		 */
		for (i = 0; i < AVE_DAPF_MAX_ENTRIES; i++)
			set[i] = ave_dapf_cleared;
		set[0] = want_text ? ave_dapf_text : ave_dapf_cleared;
		n = 1;
		set[n++] = ave_dapf_window;
		if (mmio_ave0)
			set[n++] = ave_dapf_mmio_ave0;
		if (mmio_adt)
			set[n++] = ave_dapf_mmio_adt;
		nwrite = AVE_DAPF_MAX_ENTRIES;
	} else {
		/*
		 * docs/49 N1: exactly what m1n1's dapf_init_t8020() would write
		 * from an ADT listing these entries - ADT order from slot 0, only
		 * as many slots as entries, nothing else touched. TEXT goes last.
		 *
		 * CAVEAT: slots beyond n keep E2's uninitialised contents, so a
		 * dapf_set=control run in this mode is NOT a clean negative
		 * control. Use it to learn whether the writes are survivable.
		 */
		set[n++] = ave_dapf_window;
		if (mmio_ave0)
			set[n++] = ave_dapf_mmio_ave0;
		if (mmio_adt)
			set[n++] = ave_dapf_mmio_adt;
		if (want_text)
			set[n++] = ave_dapf_text;
		nwrite = n;
		for (i = n; i < AVE_DAPF_MAX_ENTRIES; i++) {
			struct ave_dapf_entry e;

			if (ave_dapf_slot_read(ave, i, &e))
				stale++;
		}
		if (stale)
			dev_warn(ave->dev, "dapf: m1n1 order leaves %u non-empty slot(s) beyond %u untouched\n",
				 stale, n);
	}

	dev_info(ave->dev, "dapf: E3 dapf_set=%s dapf_mmio=%s dapf_order=%s quiesce=%d: %u entries%s\n",
		 dapf_set, dapf_mmio, dapf_order, dapf_quiesce, n,
		 want_text ? "" : clear16 ?
		 "  (NEGATIVE CONTROL: no TEXT entry - must still fault NO_DAPF_MATCH at TEXT+0x200)" :
		 "  (no TEXT entry; NOT a clean control in m1n1 order - garbage slots remain)");

	/*
	 * docs/49 N1: every known-good DAPF write (m1n1 at boot, m1n1's AOP
	 * experiment) happens before the DART is configured. apple-dart has
	 * enabled translation and all streams by now, so make the DART look
	 * like that around the writes, and put it back afterwards. The core is
	 * halted, so nothing is using the DART meanwhile; TTBRs are untouched,
	 * so no TLB invalidation is needed on restore.
	 */
	if (dapf_quiesce) {
		for (i = 0; i < 16; i++)
			saved_tcr[i] = readl(ave->cpudart + DART_TCR(i));
		saved_en = readl(ave->cpudart + DART_ENABLED_STREAMS);
		ave_step(ave, "next: quiesce DART - all TCRs 0 (saved TCR[0] %#x)", saved_tcr[0]);
		for (i = 0; i < 16; i++)
			writel(0, ave->cpudart + DART_TCR(i));
		ave_step(ave, "next: ENABLED_STREAMS 0 (saved %#x)", saved_en);
		writel(0, ave->cpudart + DART_ENABLED_STREAMS);
		ave_step(ave, "DART quiesced");
	}

	ret = ave_dapf_program(ave, set, nwrite, clear16);

	if (dapf_quiesce) {
		ave_step(ave, "next: restore ENABLED_STREAMS %#x and TCRs", saved_en);
		writel(saved_en, ave->cpudart + DART_ENABLED_STREAMS);
		for (i = 0; i < 16; i++)
			writel(saved_tcr[i], ave->cpudart + DART_TCR(i));
		dev_info(ave->dev, "dapf: DART restored: TCR[0] %#x ENABLED_STREAMS %#x\n",
			 readl(ave->cpudart + DART_TCR(0)),
			 readl(ave->cpudart + DART_ENABLED_STREAMS));
	}
	if (ret)
		return ret;

	ave_dapf_dump_entries(ave, "E3 after");
	if (!ave->iboot_data_mapped)
		dev_warn(ave->dev,
			 "dapf: DATA is not DART-mapped (fw_map_data=0); expect faults at 0x1f0000ecxxx / DVA 0xecxxx once TEXT is admitted\n");
	return 0;
}
