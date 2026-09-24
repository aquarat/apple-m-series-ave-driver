// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE - observe the dart-ave0 SMMU's fault status. Reads only.
 *
 * Why this exists: the first Process that ran the encoder hardware
 * (results/f3-1789384805.kmsg) ended with AIC 1028 - the one line shared by
 * the CPUDART, the DART and the SMMU of dart-ave0 - firing ~29 000 times with
 * both apple_dart_irq handlers returning IRQ_NONE, until the kernel disabled
 * it. docs/56: macOS's pending check looks at DART +0x40 bit 31, DART +0x100c,
 * SMMU +0x40 bit 31 and SMMU +0x1008 bit 20 (kext 0xfffffe0009b162c8), and
 * apple-dart only tests the first of those. No translation fault was printed,
 * so the SMMU is the likely source - inferred, not shown.
 *
 * Linux has no driver for the SMMU and must not get one: macOS never gives it
 * page tables, and apple-dart's reset would write registers macOS never
 * touches on that block (docs/56 Q2). So the AVE driver maps it itself,
 * devm_ioremap() without requesting it, and hangs a SHARED handler on the same
 * line that only reads.
 *
 * Deliberately observe-only:
 *   - the handler never claims the interrupt, so the kernel's storm detection
 *     behaves exactly as before - this adds evidence, not a new failure mode;
 *   - it never writes. macOS clears status with (status & 0x27c) at +0x40, but
 *     the last unexplored block we wrote (the DAPF, docs/49) took the machine
 *     down with an SError, so a clear waits until a read has shown what is
 *     there.
 *
 * Gated by smmu_watch=1 (needs overlay variant=4, which adds the "smmu" reg
 * entry and the second interrupt). Nothing here runs without it.
 */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>

#include "ave.h"
#include "ave_smmu.h"

#define AVE_SMMU_SIZE		0x4000
#define SMMU_ERROR		0x40		/* bit 31 = fault pending, docs/56 */
#define SMMU_ERROR_FLAG		BIT(31)
#define SMMU_ERROR_REGS		10		/* +0x40 .. +0x64 */
#define SMMU_PERF_STATUS	0x1008		/* bit 20, the other pending source */

static bool smmu_watch;
module_param(smmu_watch, bool, 0444);
MODULE_PARM_DESC(smmu_watch,
		 "log dart-ave0 SMMU fault status on the shared DART interrupt, read-only (needs overlay variant=4)");

static void ave_smmu_log(struct ave_device *ave, const char *tag)
{
	u32 r[SMMU_ERROR_REGS];
	unsigned int i;

	for (i = 0; i < SMMU_ERROR_REGS; i++)
		r[i] = readl(ave->smmu + SMMU_ERROR + 4 * i);
	dev_info(ave->dev,
		 "smmu: [%s] +0x40.. %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x; +0x1008 %08x\n",
		 tag, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9],
		 readl(ave->smmu + SMMU_PERF_STATUS));
}

static irqreturn_t ave_smmu_irq(int irq, void *data)
{
	struct ave_device *ave = data;
	u32 st;

	/* Cleared, and synchronised, before the power reference is dropped. */
	if (!READ_ONCE(ave->smmu_live))
		return IRQ_NONE;

	st = readl(ave->smmu + SMMU_ERROR);
	if (!(st & SMMU_ERROR_FLAG))
		return IRQ_NONE;

	ave->smmu_faults++;
	if (__ratelimit(&ave->smmu_rs))
		ave_smmu_log(ave, "fault");
	return IRQ_NONE;	/* observe only - see the file comment */
}

int ave_smmu_init(struct ave_device *ave)
{
	struct platform_device *pdev = to_platform_device(ave->dev);
	struct resource *res;
	int irq, ret;

	if (!smmu_watch)
		return 0;
	if (!ave->powered) {
		dev_warn(ave->dev, "smmu: not powered; not watching\n");
		return -ENODEV;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "smmu");
	if (!res || res->start != ave->soc->smmu_phys || resource_size(res) != AVE_SMMU_SIZE) {
		dev_err(ave->dev, "smmu: no \"smmu\" reg at %#llx +%#x (overlay variant=4?); refusing\n",
			(u64)ave->soc->smmu_phys, AVE_SMMU_SIZE);
		return -ENODEV;
	}
	irq = platform_get_irq_optional(pdev, 1);
	if (irq < 0) {
		dev_err(ave->dev, "smmu: no second interrupt (overlay variant=4?): %d\n", irq);
		return irq;
	}

	ave->smmu = devm_ioremap(ave->dev, res->start, resource_size(res));
	if (!ave->smmu)
		return -ENOMEM;
	ratelimit_state_init(&ave->smmu_rs, HZ, 10);

	/* E-S1 baseline (docs/56): what the block reads before anything runs. */
	ave_smmu_log(ave, "baseline");

	WRITE_ONCE(ave->smmu_live, true);
	ret = devm_request_irq(ave->dev, irq, ave_smmu_irq, IRQF_SHARED,
			       "apple-ave-smmu", ave);
	if (ret) {
		WRITE_ONCE(ave->smmu_live, false);
		dev_err(ave->dev, "smmu: request shared irq %d: %d\n", irq, ret);
		return ret;
	}
	ave->smmu_irq = irq;
	dev_info(ave->dev, "smmu: watching %#llx on shared irq %d (read-only)\n",
		 (u64)ave->soc->smmu_phys, irq);
	return 0;
}

void ave_smmu_quiesce(struct ave_device *ave)
{
	if (!ave->smmu || !READ_ONCE(ave->smmu_live))
		return;
	WRITE_ONCE(ave->smmu_live, false);
	if (ave->smmu_irq > 0)
		synchronize_irq(ave->smmu_irq);
	ave_smmu_log(ave, "at power-off");
	dev_info(ave->dev, "smmu: %lu fault interrupt(s) seen\n", ave->smmu_faults);
}
