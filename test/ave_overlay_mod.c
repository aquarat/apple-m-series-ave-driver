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

#include "ave_overlay_dtbo.h"
#include "ave_overlay_noiommu_dtbo.h"
#include "ave_overlay_e2_dtbo.h"
#include "ave_overlay_e3_dtbo.h"

static int ovcs_id;

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
 * Selected here rather than at build time so the risk is chosen when the
 * module is loaded, with the consequence in front of whoever types it.
 * Any other value is refused; before variants 2 and 3 existed every non-zero
 * value meant variant=1.
 */
static int variant;
module_param(variant, int, 0444);
MODULE_PARM_DESC(variant,
		 "0 = with DART (default), 1 = no IOMMU: preserves iBoot's DART config, NO backstop, 2 = 1 + cpudart/dapf regs (E2), 3 = 0 + cpudart/dapf regs (E3)");

static int __init ave_ov_init(void)
{
	const void *fdt;
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
	default:
		pr_err("ave-overlay: variant=%d is not 0, 1, 2 or 3; refusing\n", variant);
		return -EINVAL;
	}

	ret = of_overlay_fdt_apply((void *)fdt, len,
				   &ovcs_id, NULL);
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
