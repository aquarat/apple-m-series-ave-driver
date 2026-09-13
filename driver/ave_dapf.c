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
 */
static const struct ave_dapf_entry ave_dapf_window = {
	.start = 0x1f000000000ULL, .end = 0x1f0ffffffffULL,
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
 * Permissions copied from the 0x1f0 window entry - the ADT's only r0 = 0x33
 * entry and the one the firmware executes through for DATA; MMIO entries
 * are 0x31. The meaning of the r0 bits is unknown.
 */
static const struct ave_dapf_entry ave_dapf_text = {
	.start = AVE_IBOOT_TEXT_PHYS,
	.end = AVE_IBOOT_TEXT_PHYS + AVE_IBOOT_TEXT_SIZE - 1,	/* 0x10000c13fff */
	.r0 = 0x33, .r4 = 1,
	.what = "iBoot TEXT, physical",
};

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

static bool dapf_allow_stale;
module_param(dapf_allow_stale, bool, 0444);
MODULE_PARM_DESC(dapf_allow_stale,
		 "E3: program even if DAPF slots beyond the new set are non-zero (default: refuse)");

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
	return 0;
}

static void ave_dapf_dump_tcr(struct ave_device *ave, unsigned int sid)
{
	u32 tcr = readl(ave->cpudart + DART_TCR(sid));
	unsigned int i;

	dev_info(ave->dev, "dapf:   TCR[%2u]  = %#010x%s%s%s\n", sid, tcr,
		 tcr & DART_TCR_TRANSLATE ? " TRANSLATE" : "",
		 tcr & DART_TCR_BYPASS_DART ? " BYPASS_DART" : "",
		 tcr & DART_TCR_BYPASS_DAPF ? " BYPASS_DAPF" : "");
	for (i = 0; i < 4; i++) {
		u32 t = readl(ave->cpudart + DART_TTBR(sid, i));

		dev_info(ave->dev, "dapf:   TTBR[%2u][%u] = %#010x%s\n", sid, i, t,
			 t & DART_TTBR_VALID ? " VALID" : "");
	}
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
	return e->r0 && addr >= e->start && addr <= e->end;
}

static void ave_dapf_dump_entries(struct ave_device *ave, const char *tag)
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
		     const struct ave_dapf_entry *ent, unsigned int n)
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

	/* Exactly m1n1's dapf_init_t8020() order: r4, start, end, then r0. */
	for (i = 0; i < n; i++) {
		void __iomem *b = ave->dapf + DAPF_ENTRY(i);

		dev_info(ave->dev, "dapf: write [%2u] r0 %#06x r4 %#06x  %#013llx - %#013llx  %s\n",
			 i, ent[i].r0, ent[i].r4, ent[i].start, ent[i].end,
			 ent[i].what ?: "");
		writel(ent[i].r4, b + DAPF_R4);
		writeq(ent[i].start, b + DAPF_START);
		writeq(ent[i].end, b + DAPF_END);
		writel(ent[i].r0, b + DAPF_R0);
	}

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

int ave_dapf_program_selected(struct ave_device *ave)
{
	struct ave_dapf_entry set[AVE_DAPF_MAX_ENTRIES];
	struct iommu_domain *domain;
	bool want_text, mmio_ave0, mmio_adt;
	unsigned int n = 0, i, stale = 0;
	u32 tcr;
	int ret;

	if (!dapf_set || !*dapf_set || sysfs_streq(dapf_set, "off"))
		return 0;

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

	/* ADT order first (window, MMIO), firmware range appended. */
	set[n++] = ave_dapf_window;
	if (mmio_ave0)
		set[n++] = ave_dapf_mmio_ave0;
	if (mmio_adt)
		set[n++] = ave_dapf_mmio_adt;
	if (want_text)
		set[n++] = ave_dapf_text;

	/*
	 * A leftover entry beyond the new set - from an earlier dapf_set=text
	 * load in this boot, if DAPF state survives gating - would admit TEXT
	 * behind the negative control's back and turn "must still fault" into
	 * a false "the writes do nothing".
	 */
	for (i = n; i < AVE_DAPF_MAX_ENTRIES; i++) {
		struct ave_dapf_entry e;

		if (ave_dapf_slot_read(ave, i, &e)) {
			stale++;
			dev_warn(ave->dev,
				 "dapf: slot %u beyond the new set is non-empty: r0 %#x %#llx - %#llx\n",
				 i, e.r0, e.start, e.end);
		}
	}
	if (stale && !dapf_allow_stale) {
		dev_err(ave->dev,
			"dapf: REFUSING - %u stale slot(s) would contaminate the result; reboot, or pass dapf_allow_stale=1 knowingly\n",
			stale);
		return -EBUSY;
	}

	dev_info(ave->dev, "dapf: E3 dapf_set=%s dapf_mmio=%s: %u entries%s\n",
		 dapf_set, dapf_mmio, n,
		 want_text ? "" : "  (NEGATIVE CONTROL: no TEXT entry - must still fault NO_DAPF_MATCH at TEXT+0x200)");

	ret = ave_dapf_program(ave, set, n);
	if (ret)
		return ret;

	ave_dapf_dump_entries(ave, "E3 after");
	if (!ave->iboot_data_mapped)
		dev_warn(ave->dev,
			 "dapf: DATA is not DART-mapped (fw_map_data=0); expect faults at 0x1f0000ecxxx / DVA 0xecxxx once TEXT is admitted\n");
	return 0;
}
