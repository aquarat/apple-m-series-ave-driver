// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bring-up aid: applies test/ave-overlay.dtbo to the live device tree so the
 * apple-ave driver has something to bind to.
 *
 * The base DT has no AVE node because m1n1 emits nodes only for blocks with an
 * existing binding, and there is no configfs overlay interface in this kernel
 * build. This is throwaway test scaffolding, not part of the driver.
 *
 * !! DANGER - READ docs/24-incident-2026-09-07.md BEFORE USING THIS !!
 *
 * Applying this overlay hard-hung an M1 Max on 2026-09-07: no panic, nothing
 * logged, PMU reset. The most likely cause is that the DART node it creates
 * gets programmed by apple-dart against a power domain chosen by inference,
 * and an access to an unpowered block on Apple silicon hangs the fabric.
 *
 * Do not run this without an m1n1 hypervisor serial console attached, and
 * consider dropping the DART node from the overlay entirely.
 */
#include <linux/module.h>
#include <linux/align.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "ave_overlay_dtbo.h"
#include "ave_overlay_noiommu_dtbo.h"
#include "ave_overlay_e2_dtbo.h"
#include "ave_overlay_e3_dtbo.h"
#include "ave_overlay_e4_dtbo.h"
#include "ave_overlay_e5_dtbo.h"
#include "ave_overlay_pmp_venc_dtbo.h"

static int ovcs_id, pmp_ovcs_id;

static bool pmp_venc;
module_param(pmp_venc, bool, 0444);
MODULE_PARM_DESC(pmp_venc,
		 "also enable the PMP report entry pmp-venc-sys (report@10) for apple-ave pmp_report=1 (docs/75 R3, docs/78); needs a DT with the PMP running");

/*
 * The dtbos cannot say &aic or &ps_venc_sys: the base tree has no
 * __symbols__. They used to carry the stock t6001-j314c DT's phandle
 * numbers, which a different base DT renumbers (the APPLE_USE_PMP build,
 * f89) and another SoC's DT never matches. So they carry sentinels,
 * 0xa7e000NN with NN the old stock number, and each one is replaced here
 * with the live phandle of the node it stands for, found by label or by
 * compatible. Every sentinel the dtbo holds must be known and resolved, and
 * none may be left over. On the stock t6001 DT the result is the old
 * literals (0x13, 0x1d, 0xc2, 0xc4, 0xc3, 0xc5), which f1-f99 ran with.
 *
 * The domains keep what the stock numbers pointed at, not what the dtbo
 * comments once called them: 0xc2 is venc_pipe5, 0xc5 afnc4_ioa.
 */
#define AVE_OV_SENTINEL(nn)	(0xa7e00000u | (nn))
#define AVE_OV_SENTINEL_MASK	0xffffff00u

static const struct {
	u32		sentinel;
	const char	*label;		/* a power domain's label, or NULL: the AIC */
} ave_ov_refs[] = {
	{ AVE_OV_SENTINEL(0x13), NULL },
	{ AVE_OV_SENTINEL(0x1d), "venc_sys" },
	{ AVE_OV_SENTINEL(0xc2), "venc_pipe5" },
	{ AVE_OV_SENTINEL(0xc4), "venc_me0" },
	{ AVE_OV_SENTINEL(0xc3), "venc_pipe4" },
	{ AVE_OV_SENTINEL(0xc5), "afnc4_ioa" },
};

static int ave_ov_find_pd(const char *label, u32 *phandle)
{
	struct device_node *np, *hit = NULL;
	const char *l;

	for_each_node_with_property(np, "#power-domain-cells") {
		if (of_property_read_string(np, "label", &l) || strcmp(l, label))
			continue;
		if (hit) {
			pr_err("ave-overlay: two power domains labelled %s: %pOF, %pOF\n",
			       label, hit, np);
			of_node_put(np);
			of_node_put(hit);
			return -EEXIST;
		}
		hit = of_node_get(np);
	}
	if (!hit || !hit->phandle) {
		pr_err("ave-overlay: no power domain labelled %s with a phandle\n", label);
		of_node_put(hit);
		return -ENODEV;
	}
	*phandle = hit->phandle;
	of_node_put(hit);
	return 0;
}

/* AIC2 on t600x, AIC on t8103/t8112 (Asahi's apple,aic bindings) */
static const struct of_device_id ave_ov_aic_match[] = {
	{ .compatible = "apple,aic2" },
	{ .compatible = "apple,aic" },
	{}
};

static int ave_ov_find_aic(u32 *phandle)
{
	struct device_node *np = of_find_matching_node(NULL, ave_ov_aic_match);
	int ret = 0;

	if (!np || !of_property_read_bool(np, "interrupt-controller") ||
	    !np->phandle) {
		pr_err("ave-overlay: no apple,aic/aic2 interrupt controller with a phandle\n");
		ret = -ENODEV;
	} else {
		*phandle = np->phandle;
	}
	of_node_put(np);
	return ret;
}

/*
 * Returns a fixed-up copy of fdt (kfree it), or an ERR_PTR. The sentinels
 * are matched as whole big-endian cells on 4-byte boundaries: the structure
 * block is cell-aligned, and 0xa7 0xe0 is not text, so the strings block
 * cannot produce one.
 */
static void *ave_ov_fixup(const void *fdt, unsigned int len)
{
	unsigned int n[ARRAY_SIZE(ave_ov_refs)] = {};
	__be32 live[ARRAY_SIZE(ave_ov_refs)];
	unsigned int off;
	bool changed = false;
	u8 *copy;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ave_ov_refs); i++) {
		u32 ph;

		ret = ave_ov_refs[i].label ?
		      ave_ov_find_pd(ave_ov_refs[i].label, &ph) :
		      ave_ov_find_aic(&ph);
		if (ret)
			return ERR_PTR(ret);
		live[i] = cpu_to_be32(ph);
		if (ph != (ave_ov_refs[i].sentinel & ~AVE_OV_SENTINEL_MASK))
			changed = true;
	}

	copy = kmemdup(fdt, len, GFP_KERNEL);
	if (!copy)
		return ERR_PTR(-ENOMEM);
	for (off = 0; off + 4 <= len; off += 4) {
		u32 v = be32_to_cpup((const __be32 *)(copy + off));

		if ((v & AVE_OV_SENTINEL_MASK) != AVE_OV_SENTINEL(0))
			continue;
		for (i = 0; i < ARRAY_SIZE(ave_ov_refs); i++)
			if (v == ave_ov_refs[i].sentinel)
				break;
		if (i == ARRAY_SIZE(ave_ov_refs)) {
			pr_err("ave-overlay: unknown sentinel %#x at dtbo offset %#x; refusing\n",
			       v, off);
			kfree(copy);
			return ERR_PTR(-EINVAL);
		}
		memcpy(copy + off, &live[i], 4);
		n[i]++;
	}
	/* The AIC and venc_sys are in every variant; a dtbo without them is stale. */
	if (!n[0] || !n[1]) {
		pr_err("ave-overlay: the dtbo has no AIC/venc_sys sentinels (built before docs/79 §3?); refusing\n");
		kfree(copy);
		return ERR_PTR(-EINVAL);
	}
	for (i = 0; i < ARRAY_SIZE(ave_ov_refs); i++)
		if (n[i])
			pr_info("ave-overlay: %s -> phandle %#x (x%u)\n",
				ave_ov_refs[i].label ?: "aic",
				be32_to_cpu(live[i]), n[i]);
	if (!changed)
		pr_info("ave-overlay: every phandle is the stock t6001 one\n");
	return copy;
}

/*
 * variant=0 (default): AVE node plus the real DART, iommus = <&dart 0>.
 *                      apple-dart binds and resets it, and the IOMMU catches
 *                      stray coprocessor accesses as loud translation faults.
 *
 * variant=1:           no DART node and no iommus, so apple-dart never binds
 *                      and iBoot's DART configuration survives - which is the
 *                      thing we want to test. It also removes the backstop:
 *                      a stray write from the coprocessor then lands in
 *                      physical RAM rather than raising a fault, and we would
 *                      be running iBoot's firmware, whose addressing
 *                      assumptions we have not read.
 *
 * variant=2 (docs/44 E2): variant=1's node plus "cpudart" and "dapf" reg
 *                      entries appended, for a read-only CPUDART/DAPF dump
 *                      that apple-dart has never touched. Stop at stage 8.
 *
 * variant=3 (docs/44 E2 control, E3): variant=0 plus the same two appended
 *                      reg entries. The DART is bound and translating; the
 *                      driver may program the DAPF (dapf_set=).
 *
 * variant=5 (docs/69): variant=4 plus stream 15 on both DARTs. The ADT
 *                      declares sids = 0x8001 - streams 0 and 15 - and
 *                      nothing has ever attached 15.
 *
 * variant=4 (docs/56): variant=3 with both DART nodes in iommus, so the
 *                      encoder datapath's DART carries the same mappings as
 *                      the CPUDART - the way Linux attaches ISP's DARTs.
 *
 * Selected here rather than at build time so the risk is chosen when the
 * module is loaded, with the consequence in front of whoever types it.
 * Any other value is refused; before variants 2 and 3 existed every non-zero
 * value meant variant=1.
 */
static int variant;
module_param(variant, int, 0444);
MODULE_PARM_DESC(variant,
		 "0 = with DART (default), 1 = no IOMMU: preserves iBoot's DART config, NO backstop, 2 = 1 + cpudart/dapf regs (E2), 3 = 0 + cpudart/dapf regs (E3), 4 = 3 with both DARTs in iommus (docs/56)");

static int __init ave_ov_init(void)
{
	const void *fdt;
	void *fixed;
	unsigned int len;
	int ret;

	switch (variant) {
	case 0:
		fdt = ave_overlay_dtbo;
		len = ave_overlay_dtbo_len;
		break;
	case 1:
		fdt = ave_overlay_noiommu_dtbo;
		len = ave_overlay_noiommu_dtbo_len;
		pr_warn("ave-overlay: variant=1 - NO IOMMU, the coprocessor is unconstrained\n");
		break;
	case 2:
		fdt = ave_overlay_e2_dtbo;
		len = ave_overlay_e2_dtbo_len;
		pr_warn("ave-overlay: variant=2 - NO IOMMU, plus cpudart/dapf regs for the E2 dump; stop_after<=8\n");
		break;
	case 3:
		fdt = ave_overlay_e3_dtbo;
		len = ave_overlay_e3_dtbo_len;
		pr_warn("ave-overlay: variant=3 - with DART, plus cpudart/dapf regs (E2 control / E3)\n");
		break;
	case 4:
		fdt = ave_overlay_e4_dtbo;
		len = ave_overlay_e4_dtbo_len;
		pr_warn("ave-overlay: variant=4 - variant=3 with both DARTs in iommus (docs/56)\n");
		break;
	case 5:
		fdt = ave_overlay_e5_dtbo;
		len = ave_overlay_e5_dtbo_len;
		pr_warn("ave-overlay: variant=5 - variant=4 plus stream 15, which the ADT declares (sids 0x8001) and nothing has attached (docs/69)\n");
		break;
	default:
		pr_err("ave-overlay: variant=%d is not 0-5; refusing\n", variant);
		return -EINVAL;
	}

	if (pmp_venc) {
		struct device_node *soc = of_find_node_by_path("/soc");
		struct device_node *np;
		bool pmp_up = false;

		/* The PMP node, pmp@<unit>, whatever the SoC puts it at */
		for_each_child_of_node(soc, np)
			if (of_node_name_eq(np, "pmp") && of_device_is_available(np))
				pmp_up = true;
		of_node_put(soc);
		if (!pmp_up) {
			pr_err("ave-overlay: pmp_venc=1 but the PMP node is not enabled (docs/78); refusing\n");
			return -ENODEV;
		}
		ret = of_overlay_fdt_apply((void *)ave_overlay_pmp_venc_dtbo,
					   ave_overlay_pmp_venc_dtbo_len,
					   &pmp_ovcs_id, NULL);
		if (ret) {
			pr_err("ave-overlay: pmp-venc-sys apply failed: %d\n", ret);
			return ret;
		}
		pr_info("ave-overlay: pmp-venc-sys (report@10) enabled, ovcs_id=%d\n",
			pmp_ovcs_id);
	}

	fixed = ave_ov_fixup(fdt, len);
	if (IS_ERR(fixed))
		return PTR_ERR(fixed);
	/* of_overlay_fdt_apply() keeps its own copy */
	ret = of_overlay_fdt_apply(fixed, len, &ovcs_id, NULL);
	kfree(fixed);
	if (ret) {
		pr_err("ave-overlay: apply failed: %d\n", ret);
		return ret;
	}
	pr_info("ave-overlay: applied, ovcs_id=%d\n", ovcs_id);
	return 0;
}

/*
 * There is deliberately NO module_exit.
 *
 * Removing the overlay unbinds the DARTs, and apple_dart_remove() calls
 * apple_dart_hw_reset() -- sixteen TCR writes plus a TLB invalidate -- without
 * any runtime resume. By then __device_release_driver() has already called
 * pm_runtime_put_sync(), and pmgr-pwrstate is GENPD_FLAG_IRQ_SAFE, so venc_sys
 * is genuinely powered off. Writing DART registers into a power-gated block
 * hangs the fabric.
 *
 * That is what killed the first DART attempt: the log showed the DART
 * initialise and stage 1 pass, then the machine died in cleanup's rmmod.
 *
 * Apply once per boot; iterate by rmmod/insmod of apple_ave only. The AVE
 * device, its IOMMU group and its default domain all persist across driver
 * unbinds.
 */
module_init(ave_ov_init);
MODULE_DESCRIPTION("Apply the AVE test device-tree overlay");
MODULE_LICENSE("GPL");
