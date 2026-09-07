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

static int ovcs_id;

static int __init ave_ov_init(void)
{
	int ret;

	ret = of_overlay_fdt_apply(ave_overlay_dtbo, ave_overlay_dtbo_len,
				   &ovcs_id, NULL);
	if (ret) {
		pr_err("ave-overlay: apply failed: %d\n", ret);
		return ret;
	}
	pr_info("ave-overlay: applied, ovcs_id=%d\n", ovcs_id);
	return 0;
}

static void __exit ave_ov_exit(void)
{
	int ret = of_overlay_remove(&ovcs_id);

	pr_info("ave-overlay: removed (%d)\n", ret);
}

module_init(ave_ov_init);
module_exit(ave_ov_exit);
MODULE_DESCRIPTION("Apply the AVE test device-tree overlay");
MODULE_LICENSE("GPL");
