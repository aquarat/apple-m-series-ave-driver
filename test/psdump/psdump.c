// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only dump of PMGR power-state registers for the VENC domains, with the
 * AVD domain as a working control.
 *
 * SAFE: it only reads PMGR registers, which Linux's own pmgr driver accesses
 * routinely. It does not touch the AVE address space at all.
 *
 * Purpose: Asahi's pmgr-pwrstate writes PS_TARGET and polls PS_ACTUAL, and
 * touches nothing else. If iBoot left DEV_DISABLE (bit 10) or RESET (bit 31)
 * set on the VENC domains, genpd would report "on" while the block stays dead
 * - which matches what we see. AVD works, so any bit that differs between
 * avd_sys and venc_sys is a prime suspect.
 */
#include <linux/io.h>
#include <linux/module.h>

#define PMGR0	0x28e080000ULL		/* holds avd_sys @0x270 */
#define PMGR2	0x28e580000ULL		/* holds venc_* */

static const struct { const char *name; u64 pa; } regs[] = {
	{ "avd_sys   (WORKS)", PMGR0 + 0x270 },
	{ "venc_sys",          PMGR2 + 0x3b0 },
	{ "venc_dma",          PMGR2 + 0x8000 },
	{ "venc_pipe4",        PMGR2 + 0x8008 },
	{ "venc_pipe5",        PMGR2 + 0x8010 },
	{ "venc_me0",          PMGR2 + 0x8018 },
	{ "venc_me1",          PMGR2 + 0x8020 },
};

static void decode(const char *name, u32 v)
{
	pr_info("psdump: %-18s = 0x%08x  target=%x actual=%x%s%s%s%s ps_min=%x ps_auto=%x\n",
		name, v, v & 0xf, (v >> 4) & 0xf,
		(v & BIT(8))  ? " WAS_PWRGATED" : "",
		(v & BIT(9))  ? " WAS_CLKGATED" : "",
		(v & BIT(10)) ? " DEV_DISABLE"  : "",
		(v & BIT(31)) ? " RESET"        : "",
		(v >> 16) & 0xf, (v >> 24) & 0xf);
}

static int __init psdump_init(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		void __iomem *p = ioremap(regs[i].pa, 4);

		if (!p) {
			pr_err("psdump: ioremap %llx failed\n", regs[i].pa);
			continue;
		}
		decode(regs[i].name, readl_relaxed(p));
		iounmap(p);
	}
	return -EAGAIN;	/* nothing to keep loaded */
}
module_init(psdump_init);
MODULE_DESCRIPTION("Dump Apple PMGR power-state registers (read-only)");
MODULE_LICENSE("GPL");
