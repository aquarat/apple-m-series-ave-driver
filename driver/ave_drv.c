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
#include <linux/iopoll.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "ave.h"

#define AVE_ASC_IDLE_TIMEOUT_US		100000

/*
 * Bring-up staging.
 *
 * There is no serial console on this machine, so a hang is silent. probe()
 * is therefore split into numbered stages and stops after `stop_after`. Each
 * stage logs before and after itself, so:
 *
 *   - if the machine survives, dmesg names every stage that completed;
 *   - if it hangs, the marker file written by tools/bringup.sh before insmod
 *     names the stage that did it.
 *
 * Stages that survive can be retested with rmmod/insmod in the same boot, so
 * only a stage that actually hangs costs a reboot.
 *
 * Default is 0: map nothing, touch nothing. Raise it deliberately.
 */
static int stop_after;
module_param(stop_after, int, 0444);
MODULE_PARM_DESC(stop_after, "stop probe after this stage (0 = do nothing)");

enum ave_stage {
	AVE_STAGE_NONE		= 0,
	AVE_STAGE_MAP_BANKS	= 1,
	AVE_STAGE_DMA_MASK	= 2,
	AVE_STAGE_GET_IRQ	= 3,
	AVE_STAGE_REQUEST_IRQ	= 4,
	AVE_STAGE_POWER_ATTACH	= 5,
	AVE_STAGE_POWER_ON	= 6,	/* resume only - NO register access */
	AVE_STAGE_WRITE_IDLE	= 7,	/* THE write Apple issues first */
	AVE_STAGE_READ_ASC	= 8,	/* read bank 1 */
	AVE_STAGE_IPC_ALLOC	= 9,	/* exercises the DART */
	AVE_STAGE_FW_LOAD	= 10,	/* must precede ASC start */
	AVE_STAGE_BOOT_CFG	= 11,	/* hand the firmware its arguments */
	AVE_STAGE_IOP_CONFIG	= 12,	/* tell the ASC where firmware is */
	AVE_STAGE_ASC_START	= 13,	/* the four-write start sequence */
	AVE_STAGE_RECV_MSG	= 14,	/* wait for the firmware to speak */
	AVE_STAGE_PROBE_STATE	= 15,	/* is the core actually executing? */
	AVE_STAGE_START		= 16,
	AVE_STAGE_MAX		= 16,
};

static const char * const ave_stage_name[] = {
	"none", "map-banks", "dma-mask", "get-irq", "request-irq",
	"power-attach", "power-on", "write-sve-idle", "read-asc-status",
	"ipc-alloc", "fw-load", "boot-cfg", "iop-config", "asc-start",
	"recv-msg", "probe-state", "start",
};

/* Returns true if this stage should run. Logs the decision either way. */
static bool ave_stage(struct device *dev, enum ave_stage n)
{
	if (n > stop_after) {
		dev_info(dev, "stage %d (%s): SKIPPED (stop_after=%d)\n",
			 n, ave_stage_name[n], stop_after);
		return false;
	}
	dev_info(dev, "stage %d (%s): starting\n", n, ave_stage_name[n]);
	return true;
}

static void ave_stage_ok(struct device *dev, enum ave_stage n)
{
	dev_info(dev, "stage %d (%s): OK\n", n, ave_stage_name[n]);
}

/*
 * Linux's DT already encodes the AVE power-domain tree (venc_sys -> venc_dma
 * -> venc_pipe4 / venc_pipe5 -> venc_me0 -> venc_me1), so genpd brings up the
 * whole chain from a leaf. The DT node lists the two leaves; we simply attach
 * to whatever it lists and let genpd order them.
 */

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
	dev_info(dev, "probe: staged bring-up, stop_after=%d (max %d)\n",
		 stop_after, AVE_STAGE_MAX);

	if (ave_stage(dev, AVE_STAGE_MAP_BANKS)) {
		for (i = 0; i < AVE_NUM_BANKS; i++) {
			struct resource *res;

			dev_info(dev, "  mapping bank %u (%s)\n", i, bank_names[i]);
			ave->bank[i].base = devm_platform_get_and_ioremap_resource(
				pdev, i, &res);
			if (IS_ERR(ave->bank[i].base))
				return dev_err_probe(dev, PTR_ERR(ave->bank[i].base),
						     "bank %u (%s) failed\n",
						     i, bank_names[i]);
			ave->bank[i].size = resource_size(res);
		}
		ave_stage_ok(dev, AVE_STAGE_MAP_BANKS);
	} else {
		return 0;
	}

	if (ave_stage(dev, AVE_STAGE_DMA_MASK)) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
		if (ret)
			return dev_err_probe(dev, ret, "no suitable DMA mask\n");
		ave_stage_ok(dev, AVE_STAGE_DMA_MASK);
	} else {
		return 0;
	}

	if (ave_stage(dev, AVE_STAGE_GET_IRQ)) {
		ave->irq = platform_get_irq(pdev, AVE_IRQ_INDEX);
		if (ave->irq < 0)
			return ave->irq;
		dev_info(dev, "  irq = %d\n", ave->irq);
		ave_stage_ok(dev, AVE_STAGE_GET_IRQ);
	} else {
		return 0;
	}

	if (ave_stage(dev, AVE_STAGE_REQUEST_IRQ)) {
		ret = devm_request_irq(dev, ave->irq, ave_irq_handler, 0,
				       dev_name(dev), ave);
		if (ret)
			return dev_err_probe(dev, ret, "request_irq failed\n");
		ave_stage_ok(dev, AVE_STAGE_REQUEST_IRQ);
	} else {
		return 0;
	}

	if (ave_stage(dev, AVE_STAGE_POWER_ATTACH)) {
		/*
		 * PD_FLAG_ATTACH_POWER_ON matters here. The default creates a
		 * device link per domain but WITHOUT DL_FLAG_RPM_ACTIVE, so
		 * the domains are not guaranteed powered. The 2026-09-07 hang
		 * happened on the first register read with only the default
		 * behaviour, which is consistent with reading an unpowered
		 * block.
		 */
		static const struct dev_pm_domain_attach_data pd_data = {
			.pd_flags = PD_FLAG_ATTACH_POWER_ON |
				    PD_FLAG_DEV_LINK_ON,
		};

		ret = devm_pm_domain_attach_list(dev, &pd_data, &ave->pd_list);
		if (ret < 0)
			return dev_err_probe(dev, ret, "power domain attach\n");
		dev_info(dev, "  attached %d power domain(s)\n", ret);
		if (ret < AVE_PD_LEAVES)
			dev_warn(dev, "only %d power domain(s), expected %d\n",
				 ret, AVE_PD_LEAVES);
		devm_pm_runtime_enable(dev);
		ave_stage_ok(dev, AVE_STAGE_POWER_ATTACH);
	} else {
		return 0;
	}

	/*
	 * Power on, but touch NOTHING. The device is deliberately left
	 * resumed so that userspace can confirm the domains actually came up
	 * before the next stage reads a register:
	 *
	 *   cat /sys/kernel/debug/pm_genpd/pm_genpd_summary | grep venc
	 *
	 * If they do not all read "on" here, do not proceed to stage 7.
	 */
	if (ave_stage(dev, AVE_STAGE_POWER_ON)) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			return dev_err_probe(dev, ret, "power up failed\n");
		dev_info(dev, "  resumed; left powered for inspection\n");
		ave_stage_ok(dev, AVE_STAGE_POWER_ON);
	} else {
		return 0;
	}

	/*
	 * THE experiment (docs/29-first-access-hypothesis.md).
	 *
	 * This is the first register access Apple's driver makes, exactly:
	 *
	 *   AVE_HwC::Init -> AVE_PMGR::SetClockGating(true)
	 *     -> AVE_SVECtrl::SetIdle(1) -> Write32(bank 2, +0x38, 1)
	 *
	 * It is a WRITE. Every previous attempt here was a read, and all of them
	 * hung the fabric. On an AXI-style fabric a posted write need not wait
	 * for a response where a read must, so a non-responding agent would hang
	 * reads and swallow writes - which would explain six identical failures.
	 *
	 * One write, nothing else, so the stage has exactly one variable.
	 */
	if (ave_stage(dev, AVE_STAGE_WRITE_IDLE)) {
		dev_info(dev, "  writing 1 to SVE+0x%x (Apple's first access) ...\n",
			 AVE_SVE_IDLE);
		ave_write(ave, AVE_BANK_SVE, AVE_SVE_IDLE, 1);
		dev_info(dev, "  write returned\n");
		ave_stage_ok(dev, AVE_STAGE_WRITE_IDLE);
	} else {
		return 0;
	}

	/*
	 * First hardware access, and the ordering here is not arbitrary.
	 *
	 * A scan of every AVE_Reg::Read32/Write32 call site in the kext shows
	 * the only unconditional register accesses are bank 1 (the twenty
	 * AVE_IOP_Start_ and CheckIdle_ variants) and the AVE_SVECtrl methods on
	 * bank 2 - and every bank 2 user runs only after the IOP has been
	 * started. AVE_HwC::Init's single access is Read32(bank 5, 0x9c000),
	 * a device-revision read on a bank ave0 does not even have.
	 *
	 * An earlier attempt read bank 2 cold, before touching bank 1 at all,
	 * and hung the machine with every power domain confirmed on. Apple
	 * never does that. Bank 1 goes first.
	 */
	if (ave_stage(dev, AVE_STAGE_READ_ASC)) {
		u32 v;

		dev_info(dev, "  reading ASC+0x%x (CPU_STATUS) ...\n",
			 AVE_ASC_CPU_STATUS);
		v = ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_STATUS);
		dev_info(dev, "  ASC CPU_STATUS = 0x%08x\n", v);
		ave_stage_ok(dev, AVE_STAGE_READ_ASC);
	} else {
		return 0;
	}
	if (ave_stage(dev, AVE_STAGE_IPC_ALLOC)) {
		ret = ave_ipc_init(ave);
		if (ret)
			return dev_err_probe(dev, ret, "IPC setup\n");
		ave_stage_ok(dev, AVE_STAGE_IPC_ALLOC);
	} else {
		return 0;
	}
	if (ave_stage(dev, AVE_STAGE_FW_LOAD)) {
		ret = ave_fw_load(ave);
		if (ret)
			return dev_err_probe(dev, ret, "firmware load\n");
		ave_stage_ok(dev, AVE_STAGE_FW_LOAD);
	} else {
		return 0;
	}
	if (ave_stage(dev, AVE_STAGE_BOOT_CFG)) {
		ret = ave_boot_config(ave);
		if (ret)
			return dev_err_probe(dev, ret, "boot config\n");
		ave_stage_ok(dev, AVE_STAGE_BOOT_CFG);
	} else {
		return 0;
	}

	/*
	 * Tell the coprocessor where its firmware is. This is what
	 * AVE_IOP_Config_Nyx does, and it is on our path precisely because we
	 * are the not-iBoot-loaded case: Apple skips it when iBoot has already
	 * placed the image.
	 *
	 * We map at IOVA 0, so the masked base contributes nothing and the
	 * value is the tag alone.
	 */
	if (ave_stage(dev, AVE_STAGE_IOP_CONFIG)) {
		u64 v = (ave->fw.mapped_at_zero ? 0 : ave->fw.iova) &
			AVE_ASC_FW_BASE_MASK;

		u64 pre = readq_relaxed(ave->bank[AVE_BANK_ASC].base +
					AVE_ASC_FW_BASE);

		v |= AVE_ASC_FW_BASE_TAG;

		/*
		 * Read before writing. If iBoot pre-loaded AVE firmware this
		 * register already points at it, and overwriting it would be
		 * actively wrong. ISP is the precedent: its firmware IS
		 * iBoot-preloaded and m1n1 reserves it, and ISP has the same
		 * sids/bypass shape as AVE.
		 */
		dev_info(dev, "  ASC+0x%x BEFORE any write = %#llx  (base field %#llx)\n",
			 AVE_ASC_FW_BASE, pre, pre & AVE_ASC_FW_BASE_MASK);
		dev_info(dev, "  writing fw base %#llx to ASC+0x%x ...\n",
			 v, AVE_ASC_FW_BASE);
		ave_write64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE, v);
		dev_info(dev, "  readback = %#llx\n",
			 readq_relaxed(ave->bank[AVE_BANK_ASC].base +
				       AVE_ASC_FW_BASE));
		ave_stage_ok(dev, AVE_STAGE_IOP_CONFIG);
	} else {
		return 0;
	}

	/*
	 * ASC start must come AFTER firmware is mapped at IOVA 0. With a DART
	 * attached and nothing at 0, the core's first instruction fetch faults,
	 * the DART raises its interrupt, the handler clears it, the core
	 * retries - a handled-interrupt flood rather than a clean failure.
	 */
	if (ave_stage(dev, AVE_STAGE_ASC_START)) {
		ret = ave_asc_start(ave);
		if (ret)
			return dev_err_probe(dev, ret, "ASC start\n");
		ave_stage_ok(dev, AVE_STAGE_ASC_START);
	} else {
		return 0;
	}
	if (ave_stage(dev, AVE_STAGE_RECV_MSG)) {
		u32 msg[4];

		dev_info(dev, "  waiting up to 2s for the firmware to speak ...\n");
		ret = ave_recv_iop_msg(ave, msg, 2000);
		if (ret) {
			dev_warn(dev, "  no message from firmware (%d)\n", ret);
		} else {
			dev_info(dev, "  MSG 1: %#010x %#010x %#010x %#010x\n",
				 msg[0], msg[1], msg[2], msg[3]);
		}
		ave_fw_log_dump(ave);
		ave_stage_ok(dev, AVE_STAGE_RECV_MSG);
	} else {
		return 0;
	}

	if (ave_stage(dev, AVE_STAGE_PROBE_STATE)) {
		u32 before[AVE_SVE_NUM_SCRATCH], after[AVE_SVE_NUM_SCRATCH];
		u32 st0, st1, i;
		bool moved = false;

		st0 = ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_STATUS);
		for (i = 0; i < AVE_SVE_NUM_SCRATCH; i++)
			before[i] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(i));

		msleep(200);

		st1 = ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_STATUS);
		for (i = 0; i < AVE_SVE_NUM_SCRATCH; i++)
			after[i] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(i));

		dev_info(dev, "  CPU_STATUS %#010x -> %#010x%s\n",
			 st0, st1, st0 != st1 ? "  CHANGED" : "");
		for (i = 0; i < AVE_SVE_NUM_SCRATCH; i++) {
			dev_info(dev, "  scratch[%u] %#010x -> %#010x%s\n",
				 i, before[i], after[i],
				 before[i] != after[i] ? "  CHANGED" : "");
			if (before[i] != after[i])
				moved = true;
		}
		dev_info(dev, "  verdict: %s\n",
			 (moved || st0 != st1)
			 ? "something is executing"
			 : "no observable activity - core may not be running");
		ave_stage_ok(dev, AVE_STAGE_PROBE_STATE);
	} else {
		return 0;
	}

	if (stop_after >= AVE_STAGE_START) {
		ret = ave_start(ave);
		if (ret) {
			ave_ipc_fini(ave);
			return dev_err_probe(dev, ret, "coprocessor start\n");
		}
		dev_info(dev, "Apple AVE video encoder ready\n");
	}

	return 0;
}

static void ave_remove(struct platform_device *pdev)
{
	struct ave_device *ave = platform_get_drvdata(pdev);

	ave_stop(ave);
	ave_fw_unload(ave);
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
