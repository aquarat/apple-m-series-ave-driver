// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE (Video Encoder) - platform driver, power and coprocessor bring-up
 *
 * UNTESTED. This has never run on hardware.
 *
 * Covers steps 1-4 of the bring-up sequence in docs/22-driver-plan.md:
 * probe and resource mapping, the power-domain tree, firmware adoption, and
 * starting the ASC. The IPC ring is in ave_ipc.c.
 *
 * The register writes here are transcribed from AVE_IOP_Start_Nyx and are
 * believed exact. Everything around them is new logic.
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "ave.h"

#define AVE_ASC_IDLE_TIMEOUT_US		100000

/*
 * Linux's DT already encodes the AVE power-domain tree (venc_sys -> venc_dma
 * -> venc_pipe4 / venc_pipe5 -> venc_me0 -> venc_me1), so genpd brings up the
 * whole chain from a leaf. The DT node lists the two leaves; we simply attach
 * to whatever it lists and let genpd order them.
 */

/*
 * Adopt the firmware iBoot already placed in memory.
 *
 * The kext does not load the image; iBoot does, and the OS picks it up through
 * the "segment-ranges" property - the same property and layout m1n1 emits for
 * DCP and ISP. Two segments: TEXT (read-only to the device) and DATA
 * (read-write). See docs/09-firmware-load.md.
 *
 * The DATA segment is snapshotted here and must be restored before every
 * start, because the firmware writes into it and a restart with dirty DATA
 * does not work on Apple's side either.
 */
static int ave_fw_adopt(struct ave_device *ave)
{
	struct device_node *np = ave->dev->of_node;
	u64 seg[4];
	void *src;
	int ret;

	/*
	 * TODO(unverified): the exact "segment-ranges" cell layout for AVE has
	 * not been confirmed against a live m1n1-generated FDT - only that the
	 * kext consumes the same property DCP/ISP use. Treat this parse as
	 * provisional.
	 */
	ret = of_property_read_u64_array(np, "apple,segment-ranges", seg,
					 ARRAY_SIZE(seg));
	if (ret) {
		dev_err(ave->dev, "no apple,segment-ranges: %d\n", ret);
		return ret;
	}

	ave->fw.text_pa   = seg[0];
	ave->fw.text_size = seg[1];
	ave->fw.data_pa   = seg[2];
	ave->fw.data_size = seg[3];

	src = memremap(ave->fw.data_pa, ave->fw.data_size, MEMREMAP_WB);
	if (!src)
		return -ENOMEM;

	ave->fw.data_snapshot = devm_kmemdup(ave->dev, src, ave->fw.data_size,
					     GFP_KERNEL);
	memunmap(src);
	if (!ave->fw.data_snapshot)
		return -ENOMEM;

	dev_info(ave->dev, "firmware: text %pa+%zx data %pa+%zx\n",
		 &ave->fw.text_pa, ave->fw.text_size,
		 &ave->fw.data_pa, ave->fw.data_size);
	return 0;
}

/* Restore the pristine DATA segment. Must precede every coprocessor start. */
static int ave_fw_restore_data(struct ave_device *ave)
{
	void *dst;

	dst = memremap(ave->fw.data_pa, ave->fw.data_size, MEMREMAP_WB);
	if (!dst)
		return -ENOMEM;

	memcpy(dst, ave->fw.data_snapshot, ave->fw.data_size);
	memunmap(dst);
	return 0;
}

/*
 * Start the coprocessor.
 *
 * Transcribed verbatim from AVE_IOP_Start_Nyx (0xfffffe0008c35628). The
 * meaning of the writes to AVE_ASC_AUX_808 and AVE_ASC_AUX_400 is unknown;
 * they are reproduced exactly because the sequence is what the hardware
 * expects, not because we understand it.
 *
 * Note nothing in Apple's driver ever clears CPU_CONTROL again - shutdown is
 * by sending Halt and polling scratch 0.
 */
static int ave_asc_start(struct ave_device *ave)
{
	u32 status;
	int ret;

	ave_write(ave, AVE_BANK_ASC, AVE_ASC_AUX_808, AVE_ASC_AUX_808_VAL);
	ave_write(ave, AVE_BANK_ASC, AVE_ASC_CPU_CONTROL, 0);
	ave_write(ave, AVE_BANK_ASC, AVE_ASC_AUX_400, AVE_ASC_AUX_400_VAL);
	ave_write(ave, AVE_BANK_ASC, AVE_ASC_CPU_CONTROL, AVE_ASC_CPU_RUN);

	ret = readl_relaxed_poll_timeout(
		ave->bank[AVE_BANK_ASC].base + AVE_ASC_CPU_STATUS,
		status, !(status & AVE_ASC_STATUS_BUSY),
		100, AVE_ASC_IDLE_TIMEOUT_US);
	if (ret) {
		dev_err(ave->dev, "ASC did not become idle (status %#x)\n",
			status);
		return ret;
	}

	dev_dbg(ave->dev, "ASC running, status %#x\n", status);
	return 0;
}

static void ave_asc_stop(struct ave_device *ave)
{
	/*
	 * Apple sends the Halt command and polls scratch 0 for
	 * AVE_SCRATCH0_STOPPED rather than touching CPU_CONTROL. Until the
	 * command layer exists we can only drop power, which the caller does.
	 *
	 * TODO: send AVE_CMD_HALT once ave_cmd.c exists.
	 */
	ave->running = false;
}

/*
 * The single interrupt.
 *
 * The ADT declares five but Apple registers a handler only for index 0; that
 * one carries everything, demultiplexed by the SVE status register. The other
 * four are unclaimed and we do not request them.
 */
static irqreturn_t ave_irq_handler(int irq, void *data)
{
	struct ave_device *ave = data;
	u32 status;

	status = ave_read(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS);
	if (!status)
		return IRQ_NONE;

	/* Write-1-to-clear: the value read is written straight back. */
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS, status);

	/*
	 * One bit per IPC channel. Everything below this - command ack,
	 * command error, per-engine completion - is software dispatch on the
	 * message content, not on hardware bits.
	 */
	dev_dbg(ave->dev, "irq status %#x\n", status);

	/* TODO: drain the T2H ring and dispatch. Needs the command layer. */
	return IRQ_HANDLED;
}

static int ave_power_up(struct ave_device *ave)
{
	int ret;

	/*
	 * Apple's AVE_HwC::Init sets the IOP domain to CLOCK_ON as its very
	 * first hardware action, before the DART is attached and before any
	 * MMIO is mapped. genpd brings the whole list up together, which is
	 * close enough given the domains are dependency-ordered in the DT.
	 */
	ret = pm_runtime_resume_and_get(ave->dev);
	if (ret < 0) {
		dev_err(ave->dev, "failed to power up: %d\n", ret);
		return ret;
	}
	return 0;
}

static int ave_start(struct ave_device *ave)
{
	int ret;

	ret = ave_power_up(ave);
	if (ret)
		return ret;

	ret = ave_fw_restore_data(ave);
	if (ret)
		goto err_power;

	ret = ave_asc_start(ave);
	if (ret)
		goto err_power;

	ret = ave_ipc_handshake(ave);
	if (ret) {
		dev_err(ave->dev, "IPC handshake failed: %d\n", ret);
		goto err_asc;
	}

	ave->running = true;
	dev_info(ave->dev, "coprocessor up, %u IPC channel(s)\n",
		 ave->nchannels);
	return 0;

err_asc:
	ave_asc_stop(ave);
err_power:
	pm_runtime_put(ave->dev);
	return ret;
}

static void ave_stop(struct ave_device *ave)
{
	if (!ave->running)
		return;
	ave_asc_stop(ave);
	pm_runtime_put(ave->dev);
}

static int ave_probe(struct platform_device *pdev)
{
	static const char * const bank_names[AVE_NUM_BANKS] = {
		"dpe", "asc", "sve", "unk3", "axi2af",
	};
	struct device *dev = &pdev->dev;
	struct ave_device *ave;
	unsigned int i;
	int ret;

	ave = devm_kzalloc(dev, sizeof(*ave), GFP_KERNEL);
	if (!ave)
		return -ENOMEM;

	ave->dev = dev;
	platform_set_drvdata(pdev, ave);

	for (i = 0; i < AVE_NUM_BANKS; i++) {
		struct resource *res;

		ave->bank[i].base = devm_platform_get_and_ioremap_resource(
			pdev, i, &res);
		if (IS_ERR(ave->bank[i].base))
			return dev_err_probe(dev, PTR_ERR(ave->bank[i].base),
					     "failed to map bank %u (%s)\n",
					     i, bank_names[i]);
		ave->bank[i].size = resource_size(res);
	}

	/*
	 * The DART is an ordinary apple-dart, so the plain DMA API is enough -
	 * Apple's kext does not program DART registers either, it goes through
	 * an IOKit mapper. 42 bits is a guess sized to the observed IOVAs.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return dev_err_probe(dev, ret, "no suitable DMA mask\n");

	ave->irq = platform_get_irq(pdev, AVE_IRQ_INDEX);
	if (ave->irq < 0)
		return ave->irq;

	ret = devm_request_irq(dev, ave->irq, ave_irq_handler, 0,
			       dev_name(dev), ave);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	ret = devm_pm_domain_attach_list(dev, NULL, &ave->pd_list);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to attach power domains\n");
	if (ret < AVE_PD_LEAVES)
		dev_warn(dev, "only %d power domain(s), expected %d\n",
			 ret, AVE_PD_LEAVES);

	ret = ave_fw_adopt(ave);
	if (ret)
		return dev_err_probe(dev, ret, "failed to adopt firmware\n");

	ret = ave_ipc_init(ave);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set up IPC\n");

	devm_pm_runtime_enable(dev);

	/*
	 * Bring the coprocessor up once at probe so a failure is visible
	 * immediately rather than at first open. A real driver would defer
	 * this to the first V4L2 open.
	 */
	ret = ave_start(ave);
	if (ret) {
		ave_ipc_fini(ave);
		return dev_err_probe(dev, ret, "coprocessor did not start\n");
	}

	dev_info(dev, "Apple AVE video encoder ready\n");
	return 0;
}

static void ave_remove(struct platform_device *pdev)
{
	struct ave_device *ave = platform_get_drvdata(pdev);

	ave_stop(ave);
	ave_ipc_fini(ave);
}

static const struct of_device_id ave_of_match[] = {
	{ .compatible = "apple,ave" },
	{}
};
MODULE_DEVICE_TABLE(of, ave_of_match);

static struct platform_driver ave_driver = {
	.probe	= ave_probe,
	.remove	= ave_remove,
	.driver	= {
		.name		= "apple-ave",
		.of_match_table	= ave_of_match,
	},
};
module_platform_driver(ave_driver);

MODULE_DESCRIPTION("Apple AVE video encoder");
MODULE_LICENSE("GPL");
