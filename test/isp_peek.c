// SPDX-License-Identifier: GPL-2.0-only
/*
 * docs/44 experiment E1: read ISP's RVBAR, CPU_CONTROL, CPU_STATUS and DAPF
 * while ISP is running. READ-ONLY. Touches nothing of AVE's.
 *
 * UNTESTED - compiled only. The operator runs it; see
 * docs/48-e1-e3-procedure.md. Agents: do not load this (AGENTS.md).
 *
 * OPERATOR PRECONDITION: start the camera first and keep it streaming for the
 * whole load, e.g. in another terminal
 *
 *     ffmpeg -f v4l2 -i /dev/video0 -f null -
 *
 * then check /sys/bus/platform/devices/384000000.isp/power/runtime_status
 * reads "active". The module refuses on its own if it is not, but the check
 * above is what tells you whether to bother.
 *
 * What it reads (AP-physical, all derived from the live DT and then checked
 * against these constants; any mismatch refuses before mapping anything):
 *
 *   ISP reg[0] 0x384000000 (apple,isp "coproc")
 *     +0x1050000  RVBAR, 64-bit   m1n1 proxyclient/m1n1/hw/isp.py ISP_ASC_RVBAR;
 *                                 Linux apple/isp/isp-regs.h:14 ISP_COPROC_RVBAR
 *     +0x1400044  CPU_CONTROL     isp.py ISP_ASC_CONTROL; isp-regs.h:16
 *     +0x1400048  CPU_STATUS      isp.py ISP_ASC_STATUS;  isp-regs.h:17
 *                 (= ASC base 0x385400000 + 0x44/0x48, m1n1 hw/asc.py ASCRegs;
 *                  STATUS bit 0 RUNNING, bit 1 STOPPED, bit 5 IDLE)
 *
 *   ISP DAPF 0x3860ec000 = first "iommus" DART 0x3860e8000 + 0x4000
 *                 = ADT dart-isp0 reg[5], bus 0x1860ec000 (+0x200000000)
 *     16 entries at +0x40*i: +0x00 r0, +0x04 r4, +0x08 start, +0x10 end
 *     (m1n1 src/dapf.c:35-41 dapf_init_t8020, which m1n1 runs for
 *      /arm-io/dart-isp0 index 5, dapf.c:175)
 *
 *   dart_regs=1 (default off) additionally reads the DART's DAPF_LOCK (+0xf0)
 *   and TCR[0..15] (+0x100) - to tell whether ISP's streams enforce the DAPF
 *   at all (TCR bit 12 BYPASS_DAPF).
 *
 * Power safety. A read of a gated block hangs the fabric. So, holding
 * device_lock(ISP) so it cannot unbind, and refusing if the lock is busy:
 *   1. the ISP device must be bound and pm_runtime_active();
 *   2. pm_runtime_get_if_active() on the ISP device;
 *   3. pm_runtime_get_if_active() on EVERY genpd supplier of the ISP device
 *      and on its first DART. This matters: apple-isp holds only power
 *      domain 0 through runtime PM of the ISP device itself; domains 1..n are
 *      separate pm_runtime_get_sync() calls on virtual genpd devices, and
 *      isp_firmware_shutdown_stage1() drops them BEFORE it puts the ISP
 *      device. A reference on the ISP device alone would not stop the ASC's
 *      domains gating mid-read if the camera stopped. The number of genpd
 *      suppliers pinned must equal the node's power-domains count.
 *   Only then ioremap and read, each register once, then drop every reference.
 *   Residual: the device-link list is walked without the (unexported) SRCU
 *   read lock; device_lock prevents apple-isp's own link removal, not a
 *   concurrent fw_devlink change elsewhere.
 *
 * Reading the result (docs/44 §6 E1):
 *
 *   Negative control first: CPU_STATUS must have RUNNING (bit 0). If not, ISP
 *   was not up and nothing below is evidence. The module says so.
 *
 *   RVBAR base (& 0x3fffffff800, AVE's field mask; ISP's is assumed equal):
 *   - in 0x1f000000000..0x1f0ffffffff: ISP boots TEXT through the DART window.
 *     AVE's physical TEXT fetch is then a deliberate difference, and the
 *     E3 TEXT DAPF entry is AVE-specific. Does not refute H1 for AVE.
 *   - equal to ISP's TEXT physical address (the /reserved-memory apple,asc-mem
 *     node whose iommu-addresses give ISP IOVA 0; 0x10000c68000 on this boot):
 *     ISP also fetches TEXT physically, and something on Linux admits it. The
 *     DAPF dump should then show an entry covering it - supports H1's
 *     mechanism and tells E3 which r0/r4 a TEXT entry needs.
 *     If NO DAPF entry covers it, either the DAPF is bypassed for that stream
 *     (check with dart_regs=1) or H1's "DAPF must admit TEXT" is refuted.
 *   - below 4 GiB (e.g. 0 or ~0): an IOVA-looking base. Neither window; the
 *     §0 model is incomplete for ISP.
 *   - anything else: unexplained; record it.
 *
 *   DAPF slots vs the 13.5 ADT's dart-isp0 filter-data-instance-0 (15
 *   entries, compiled in below). Every slot MATCH => the t8020 register
 *   layout used by ave_dapf.c is right, m1n1's programming survived ISP's
 *   power cycling (evidence against H3's "gating loses DAPF state" for
 *   ISP). Slot 15 has no ADT entry; it must NOT read as a match - that is
 *   the discriminator's own negative control. All-DIFF or all-zero means
 *   the layout, the address, or the survival assumption is wrong - do not
 *   run E3 on the strength of the layout until that is explained.
 */
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/string.h>

#define ISP_EXPECT_COPROC	0x384000000ULL
#define ISP_EXPECT_DART		0x3860e8000ULL
#define ISP_DAPF_OFFSET		0x4000

#define ISP_ASC_RVBAR		0x1050000	/* 64-bit */
#define ISP_ASC_CPU		0x1400000
#define ISP_ASC_CPU_CONTROL	0x44
#define ISP_ASC_CPU_STATUS	0x48
#define ASC_ST_RUNNING		BIT(0)
#define ASC_ST_STOPPED		BIT(1)
#define ASC_ST_IDLE		BIT(5)
#define ASC_RVBAR_MASK		0x3fffffff800ULL	/* AVE_ASC_FW_BASE_MASK */

#define DAPF_ENTRIES		16
#define DAPF_R0			0x00
#define DAPF_R4			0x04
#define DAPF_START		0x08
#define DAPF_END		0x10

#define DART_DAPF_LOCK		0xf0
#define DART_TCR(sid)		(0x100 + 4 * (sid))
#define DART_TCR_TRANSLATE	BIT(7)
#define DART_TCR_BYPASS_DART	BIT(8)
#define DART_TCR_BYPASS_DAPF	BIT(12)

#define MAX_PINNED		32

struct dapf_ent {
	u64 start, end;
	u32 r0, r4;
};

/*
 * /arm-io/dart-isp0 filter-data-instance-0, data/blobs/macos-13.5/adt.bin
 * (identical in data/blobs/adt.bin), decoded as m1n1 dapf_t8020_config.
 * The live ADT m1n1 used this boot was not available to check against.
 */
static const struct dapf_ent isp_adt[] = {
	{ 0x1f000000000ULL, 0x1f0ffffffffULL, 0x33, 1 },
	{ 0x28e584000ULL, 0x28e584043ULL, 0x31, 1 },
	{ 0x28e0b8000ULL, 0x28e0bffffULL, 0x31, 1 },
	{ 0x39a000000ULL, 0x39a00004bULL, 0x31, 1 },
	{ 0x39b010000ULL, 0x39b0130b3ULL, 0x31, 1 },
	{ 0x39b014000ULL, 0x39b0170b3ULL, 0x31, 1 },
	{ 0x39b018000ULL, 0x39b01b0b3ULL, 0x31, 1 },
	{ 0x28ec3c000ULL, 0x28ec3c003ULL, 0x31, 1 },
	{ 0x29342c000ULL, 0x293448003ULL, 0x31, 1 },
	{ 0x285460000ULL, 0x285460003ULL, 0x31, 1 },
	{ 0x200004000ULL, 0x2000d63f8ULL, 0x31, 1 },
	{ 0x201004000ULL, 0x2010d63f8ULL, 0x31, 1 },
	{ 0x202004000ULL, 0x2020d63f8ULL, 0x31, 1 },
	{ 0x203004000ULL, 0x2030d63f8ULL, 0x31, 1 },
	{ 0x28e3d0000ULL, 0x28e3d0bfcULL, 0x31, 1 },
};

static bool dart_regs;
module_param(dart_regs, bool, 0444);
MODULE_PARM_DESC(dart_regs, "also read the ISP DART's DAPF_LOCK and TCR[0..15] (default off)");

static struct device *pinned[MAX_PINNED];
static unsigned int npinned;

static void unpin_all(void)
{
	while (npinned) {
		struct device *d = pinned[--npinned];

		pm_runtime_put(d);
		put_device(d);
	}
}

static int pin(struct device *d, const char *why)
{
	int r;

	if (npinned >= MAX_PINNED)
		return -ENOSPC;
	r = pm_runtime_get_if_active(d);
	if (r <= 0) {
		pr_err("isp_peek: REFUSING - %s %s not runtime-active (get_if_active %d)\n",
		       why, dev_name(d), r);
		return r ? r : -EAGAIN;
	}
	pinned[npinned++] = get_device(d);
	return 0;
}

/* ISP TEXT phys: the apple,asc-mem node mapping ISP IOVA 0. 0 if none. */
static u64 isp_text_phys(struct device_node *isp_np)
{
	struct device_node *rm, *c;
	u64 found = 0;

	rm = of_find_node_by_path("/reserved-memory");
	if (!rm)
		return 0;
	for_each_child_of_node(rm, c) {
		struct resource r;
		u32 ph, a_hi, a_lo;

		if (!of_device_is_compatible(c, "apple,asc-mem") ||
		    of_property_read_u32_index(c, "iommu-addresses", 0, &ph) ||
		    of_property_read_u32_index(c, "iommu-addresses", 1, &a_hi) ||
		    of_property_read_u32_index(c, "iommu-addresses", 2, &a_lo))
			continue;
		if (ph != isp_np->phandle || a_hi || a_lo)
			continue;
		if (!of_address_to_resource(c, 0, &r)) {
			found = r.start;
			of_node_put(c);
			break;
		}
	}
	of_node_put(rm);
	return found;
}

static void peek(struct device_node *isp_np, phys_addr_t coproc,
		 phys_addr_t dart)
{
	void __iomem *rv, *cpu, *dapf, *dr = NULL;
	u64 rvbar, base, text;
	u32 ctl, st;
	unsigned int i, j, matched = 0, used = 0;

	rv = ioremap(coproc + ISP_ASC_RVBAR, 8);
	cpu = ioremap(coproc + ISP_ASC_CPU, 0x100);
	dapf = ioremap(dart + ISP_DAPF_OFFSET, 0x40 * DAPF_ENTRIES);
	if (dart_regs)
		dr = ioremap(dart, 0x200);
	if (!rv || !cpu || !dapf || (dart_regs && !dr)) {
		pr_err("isp_peek: ioremap failed\n");
		goto out;
	}

	/* CPU_STATUS first: it decides whether the rest means anything. */
	st = readl(cpu + ISP_ASC_CPU_STATUS);
	ctl = readl(cpu + ISP_ASC_CPU_CONTROL);
	rvbar = readq(rv);

	pr_info("isp_peek: CPU_STATUS  %#010x%s%s%s\n", st,
		st & ASC_ST_RUNNING ? " RUNNING" : "",
		st & ASC_ST_STOPPED ? " STOPPED" : "",
		st & ASC_ST_IDLE ? " IDLE" : "");
	pr_info("isp_peek: CPU_CONTROL %#010x%s\n", ctl,
		ctl & 0x10 ? " RUN" : "");
	if (!(st & ASC_ST_RUNNING))
		pr_warn("isp_peek: NEGATIVE CONTROL FAILED - ISP core not RUNNING; RVBAR below is NOT evidence\n");

	base = rvbar & ASC_RVBAR_MASK;
	text = isp_text_phys(isp_np);
	pr_info("isp_peek: RVBAR       %#018llx  lock(bit0)=%llu  base %#llx\n",
		rvbar, rvbar & 1, base);
	pr_info("isp_peek: ISP TEXT phys from /reserved-memory (IOVA 0): %#llx\n",
		text);
	if (base >= 0x1f000000000ULL && base <= 0x1f0ffffffffULL)
		pr_info("isp_peek: verdict: RVBAR in the 0x1f0 window (DART-translated TEXT); AVE's physical fetch is AVE-specific\n");
	else if (text && base == text)
		pr_info("isp_peek: verdict: RVBAR = ISP TEXT PHYSICAL; check below for the admitting DAPF entry\n");
	else if (base < SZ_4G)
		pr_info("isp_peek: verdict: RVBAR below 4 GiB (IOVA-looking); neither window\n");
	else
		pr_info("isp_peek: verdict: RVBAR base unexplained; record it\n");

	for (i = 0; i < DAPF_ENTRIES; i++) {
		void __iomem *b = dapf + 0x40 * i;
		struct dapf_ent e = {
			.r0 = readl(b + DAPF_R0),
			.r4 = readl(b + DAPF_R4),
			.start = readq(b + DAPF_START),
			.end = readq(b + DAPF_END),
		};
		const char *m = "no ADT entry";
		bool covers_rv = e.r0 && base >= e.start && base <= e.end;

		if (e.r0 || e.r4 || e.start || e.end)
			used++;
		if (i < ARRAY_SIZE(isp_adt)) {
			const struct dapf_ent *a = &isp_adt[i];

			if (a->start == e.start && a->end == e.end &&
			    a->r0 == e.r0 && a->r4 == e.r4) {
				m = "MATCH";
				matched++;
			} else {
				m = "DIFF";
				for (j = 0; j < ARRAY_SIZE(isp_adt); j++)
					if (isp_adt[j].start == e.start &&
					    isp_adt[j].end == e.end)
						m = "DIFF (range is another ADT slot)";
			}
		}
		pr_info("isp_peek: DAPF[%2u] r0 %#06x r4 %#06x  %#013llx - %#013llx  %s%s\n",
			i, e.r0, e.r4, e.start, e.end, m,
			covers_rv ? "  <- admits RVBAR base" : "");
	}
	pr_info("isp_peek: DAPF: %u non-empty, %u/%zu match the 13.5 ADT slot-for-slot\n",
		used, matched, ARRAY_SIZE(isp_adt));

	if (dr) {
		pr_info("isp_peek: DART %pa DAPF_LOCK %#010x\n", &dart,
			readl(dr + DART_DAPF_LOCK));
		for (i = 0; i < 16; i++) {
			u32 t = readl(dr + DART_TCR(i));

			pr_info("isp_peek: DART TCR[%2u] %#010x%s%s%s\n", i, t,
				t & DART_TCR_TRANSLATE ? " TRANSLATE" : "",
				t & DART_TCR_BYPASS_DART ? " BYPASS_DART" : "",
				t & DART_TCR_BYPASS_DAPF ? " BYPASS_DAPF" : "");
		}
	}
out:
	if (dr)
		iounmap(dr);
	if (dapf)
		iounmap(dapf);
	if (cpu)
		iounmap(cpu);
	if (rv)
		iounmap(rv);
}

static int __init isp_peek_init(void)
{
	struct device_node *np, *dart_np;
	struct platform_device *pdev;
	struct device_link *link;
	struct resource rc = {}, rd = {};
	struct device *dev;
	int ret, npd, ngenpd = 0;
	bool dart_pinned = false;

	np = of_find_compatible_node(NULL, NULL, "apple,isp");
	if (!np) {
		pr_err("isp_peek: no apple,isp node\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(np, 0, &rc);
	dart_np = of_parse_phandle(np, "iommus", 0);
	if (ret || !dart_np || of_address_to_resource(dart_np, 0, &rd) ||
	    rc.start != ISP_EXPECT_COPROC ||
	    resource_size(&rc) < ISP_ASC_CPU + 0x100 ||
	    rd.start != ISP_EXPECT_DART) {
		pr_err("isp_peek: REFUSING - ISP reg %pR / DART %pR, expected %#llx / %#llx\n",
		       &rc, &rd, ISP_EXPECT_COPROC, ISP_EXPECT_DART);
		ret = -EINVAL;
		goto out_np;
	}

	npd = of_count_phandle_with_args(np, "power-domains",
					 "#power-domain-cells");

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("isp_peek: no platform device for %pOF\n", np);
		ret = -ENODEV;
		goto out_np;
	}
	dev = &pdev->dev;

	if (!device_trylock(dev)) {
		pr_err("isp_peek: REFUSING - ISP device lock busy (probe/remove in progress?)\n");
		ret = -EBUSY;
		goto out_put;
	}
	if (!dev->driver) {
		pr_err("isp_peek: REFUSING - ISP device not bound to a driver\n");
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!pm_runtime_active(dev)) {
		pr_err("isp_peek: REFUSING - ISP not runtime-active; start the camera stream first\n");
		ret = -EAGAIN;
		goto out_unlock;
	}

	ret = pin(dev, "ISP");
	if (ret)
		goto out_unlock;

	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		struct device *sup = link->supplier;

		if (!strncmp(dev_name(sup), "genpd:", 6)) {
			ret = pin(sup, "power domain");
			if (ret)
				goto out_unpin;
			ngenpd++;
		} else if (sup->of_node == dart_np) {
			ret = pin(sup, "DART");
			if (ret)
				goto out_unpin;
			dart_pinned = true;
		}
	}
	if (npd <= 0 || ngenpd != npd || !dart_pinned) {
		pr_err("isp_peek: REFUSING - pinned %d of %d power domains, DART %s\n",
		       ngenpd, npd, dart_pinned ? "pinned" : "NOT pinned");
		ret = -EINVAL;
		goto out_unpin;
	}
	pr_info("isp_peek: ISP bound and active; pinned ISP + %d power domains + DART %pa\n",
		ngenpd, &rd.start);

	peek(np, rc.start, rd.start);
	ret = 0;

out_unpin:
	unpin_all();
out_unlock:
	device_unlock(dev);
out_put:
	put_device(dev);
out_np:
	of_node_put(dart_np);
	of_node_put(np);
	return ret;
}

/* Holds nothing after init; unloading is safe. */
static void __exit isp_peek_exit(void)
{
}

module_init(isp_peek_init);
module_exit(isp_peek_exit);
MODULE_DESCRIPTION("docs/44 E1: read-only ISP RVBAR / CPU status / DAPF peek");
MODULE_LICENSE("GPL");
