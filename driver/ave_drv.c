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
#include <linux/reset.h>
#include <linux/slab.h>

#include "ave.h"
#include "ave_dapf.h"

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
int ave_step_ms;
module_param_named(step_ms, ave_step_ms, int, 0444);
MODULE_PARM_DESC(step_ms, "hold each STEP marker this many ms so a crash leaves it on disk (0 = off)");

static int stop_after;
static int rvbar_probe;
module_param(rvbar_probe, int, 0444);
MODULE_PARM_DESC(rvbar_probe, "walk test values through the ASC RVBAR and report which bits move");

static bool asc_timer;
module_param(asc_timer, bool, 0444);
MODULE_PARM_DESC(asc_timer, "also read the ASC timebase at +0x178000 during liveness sampling");

static ulong ctl_asc;
module_param(ctl_asc, ulong, 0444);
MODULE_PARM_DESC(ctl_asc, "phys base of a known-running ASC (e.g. DCP coproc) to sample READ-ONLY as a positive control");

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
	/* Non-NULL once ave_ipc_init() has initialised ipc_lock. */
	bool ipc = READ_ONCE(ave->boot_abi) != NULL;
	unsigned long flags = 0;
	bool dispatch = false;
	u32 status;

	if (ipc)
		spin_lock_irqsave(&ave->ipc_lock, flags);

	status = ave_read(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS);
	if (!status) {
		if (ipc)
			spin_unlock_irqrestore(&ave->ipc_lock, flags);
		return IRQ_NONE;
	}

	/*
	 * Capture before clearing, for the log. The first interrupt is
	 * normally message 1, and write-1-to-clear destroys the evidence.
	 */
	if (!ave->hs_seen) {
		ave->hs_status     = status;
		ave->hs_scratch[0] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0));
		ave->hs_scratch[1] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1));
		ave->hs_scratch[2] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2));
		ave->hs_scratch[3] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(3));
		ave->hs_seen = true;
		dev_info(ave->dev,
			 "IRQ: first firmware message, status %#x, scratch %#x %#x %#x %#x\n",
			 status, ave->hs_scratch[0], ave->hs_scratch[1],
			 ave->hs_scratch[2], ave->hs_scratch[3]);
	}

	/*
	 * Until the ready flag clears, bit 0 is the scratch mailbox and every
	 * handshake message (1, 3, 5) raises it. Apple runs StartUpIOP with no
	 * handler (docs/34 §14); ours is live, so keep the message for
	 * ave_recv_iop_msg() instead of letting the W1C below eat it. Same lock
	 * as RecvIOPMsg's poll, so exactly one of the two sees the bit.
	 * After the handshake bit 0 is a channel doorbell (13.5: TERMINAL).
	 */
	if (ipc && !ave->ipc_up && (status & BIT(0))) {
		ave->mbox_status = status;
		ave->mbox[0] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0));
		ave->mbox[1] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1));
		ave->mbox[2] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2));
		ave->mbox[3] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(3));
		ave->mbox_pending = true;
	}

	/*
	 * Write-1-to-clear the whole word, before dispatch, as
	 * AVE_HwC::ProcessIntr does (k13 0xfffffe0008f0d9d8; k26 c19928).
	 */
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS, status);
	dispatch = ipc && ave->ipc_up && ave->ipc_irq;

	if (ipc)
		spin_unlock_irqrestore(&ave->ipc_lock, flags);

	dev_dbg(ave->dev, "irq status %#x\n", status);

	/*
	 * Status bits are doorbell bits, and on 13.5 several channels share
	 * one (bit 1: IO, DEBUG; bit 3: BUF_T2H, SHAREDMALLOC, IO_T2H), so the
	 * dispatcher drains every bound channel of the selected ABI whose bit
	 * is set - Apple's ch = 1..7 loop (k13 0xfffffe0008f0d9dc..f0dae8).
	 * It lives in ave_ipc.c (ave_ipc_irq_dispatch).
	 */
	if (dispatch)
		ave->ipc_irq(ave, status);

	return IRQ_HANDLED;
}


/*
 * Drop VENC power, safely.
 *
 * The IRQ handler reads and write-1-clears the SVE status register, and a
 * register access in a gated block hangs the fabric. Gating is asynchronous
 * (genpd suppliers are put asynchronously), so the handler must be quiesced
 * - synchronously, waiting out any running instance - BEFORE the reference
 * is dropped. Every power-off goes through here: the stage-15 power-off,
 * remove(), probe failure, and the devres action below that covers any
 * probe exit after stage 6. Idempotent. (Review 2026-09-13, finding 1.)
 */
static void ave_power_off(struct ave_device *ave, const char *why)
{
	if (!ave->powered)
		return;
	if (ave->irq_enabled) {
		disable_irq(ave->irq);
		ave->irq_enabled = false;
	}
	if (ave->bank[AVE_BANK_ASC].base)
		ave_write(ave, AVE_BANK_ASC, AVE_ASC_CPU_CONTROL, 0);
	ave->powered = false;
	pm_runtime_put_sync(ave->dev);
	dev_info(ave->dev, "powered off (%s)\n", why);
}

static void ave_power_off_action(void *data)
{
	ave_power_off(data, "devres");
}

/*
 * Finish bringing the coprocessor up.
 *
 * By the time this runs, the staged probe has already powered the block
 * (stage 6), programmed the pre-start scratch registers (stage 11), started
 * the core (stage 13) and received message 1 (stage 14, which ave_ipc.c keeps
 * as the handshake's first message). An earlier version powered up and
 * started the ASC a second time here and never programmed the scratch
 * registers at all. All that remains is messages 2-5 and the ready flag.
 *
 * Power is owned by ave->powered and dropped in ave_remove(), not here.
 */
static int ave_start(struct ave_device *ave)
{
	int ret;

	ret = ave_ipc_handshake(ave);
	if (ret) {
		dev_err(ave->dev, "IPC handshake failed: %d\n", ret);
		return ret;
	}

	ave->running = true;
	dev_info(ave->dev, "coprocessor up, %u IPC channel(s)\n",
		 ave->nchannels);
	return 0;
}

static void ave_stop(struct ave_device *ave)
{
	if (!ave->running)
		return;
	ave_asc_stop(ave);
	/* Power is dropped by ave_remove() via ave->powered. */
}

/*
 * Is the core executing? A liveness test that can say "no".
 *
 * The page-checksum test cannot: base+0x200 is the synchronous exception
 * vector and holds "b .", so a core that aborts on its first fetch spins
 * there forever writing nothing, exactly like a core that never ran.
 *
 * CPU_STATUS can, in principle. m1n1 names bit 0 RUNNING, bit 1 STOPPED and
 * bit 5 IDLE; a core spinning in a branch-to-self is not idle. One read tells
 * us little, so sample it a couple of thousand times and report the
 * histogram, which also catches a core that alternates.
 *
 * It is only evidence against controls, so it is taken three ways:
 *   - this block, halted, before CPU_CONTROL gets RUN   (negative control)
 *   - this block, started                               (the measurement)
 *   - this block, halted again at the end of probe      (negative control)
 * and, separately, on an ASC whose firmware is known to be running - DCP -
 * as the positive control. That one is strictly read-only.
 */
#define AVE_STATUS_SAMPLES	2000

static void ave_status_histogram(struct device *dev, const char *tag,
				 void __iomem *status)
{
	struct { u32 v; unsigned int n; } h[8];
	unsigned int nh = 0, other = 0, i, j;
	ktime_t t0 = ktime_get();

	for (i = 0; i < AVE_STATUS_SAMPLES; i++) {
		u32 v = readl_relaxed(status);

		for (j = 0; j < nh && h[j].v != v; j++)
			;
		if (j < nh)
			h[j].n++;
		else if (nh < ARRAY_SIZE(h))
			h[nh].v = v, h[nh].n = 1, nh++;
		else
			other++;
		usleep_range(50, 100);
	}

	dev_info(dev, "  [%s] CPU_STATUS: %u samples over %lld ms, %u distinct%s\n",
		 tag, AVE_STATUS_SAMPLES,
		 ktime_ms_delta(ktime_get(), t0), nh,
		 other ? " (histogram full, some values uncounted)" : "");
	for (j = 0; j < nh; j++) {
		u32 v = h[j].v;

		dev_info(dev, "  [%s]   %#06x x%-4u%s%s%s%s%s%s\n", tag, v, h[j].n,
			 v & AVE_ASC_ST_RUNNING ? " RUNNING" : "",
			 v & AVE_ASC_ST_STOPPED ? " STOPPED" : "",
			 v & AVE_ASC_ST_IRQ_NOT_PEND ? " IRQ_NOT_PEND?" : "",
			 v & AVE_ASC_ST_FIQ_NOT_PEND ? " FIQ_NOT_PEND?" : "",
			 v & AVE_ASC_ST_IDLE ? " IDLE" : "",
			 v & ~(u32)(AVE_ASC_ST_RUNNING | AVE_ASC_ST_STOPPED |
				    AVE_ASC_ST_IRQ_NOT_PEND |
				    AVE_ASC_ST_FIQ_NOT_PEND | AVE_ASC_ST_IDLE)
			 ? " +unnamed bits" : "");
	}
}

static void ave_asc_liveness(struct ave_device *ave, const char *tag)
{
	struct device *dev = ave->dev;

	dev_info(dev, "  [%s] CPU_CONTROL = %#x\n", tag,
		 ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_CONTROL));
	ave_status_histogram(dev, tag,
			     ave->bank[AVE_BANK_ASC].base + AVE_ASC_CPU_STATUS);

	if (!asc_timer)
		return;

	/*
	 * First read of this register by us. Announce it and give the test
	 * script time to get the line onto disk, so that if it hangs the log
	 * says which access did it.
	 */
	dev_info(dev, "  [%s] reading ASC timer +%#x ...\n", tag, AVE_ASC_TIMER);
	msleep(500);
	{
		u64 c0 = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_TIMER);
		u32 f = ave_read(ave, AVE_BANK_ASC, AVE_ASC_TIMER_FREQ);
		u64 c1;

		msleep(100);
		c1 = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_TIMER);
		dev_info(dev, "  [%s] timer %#llx -> %#llx (delta %llu over ~100 ms), freq %u\n",
			 tag, c0, c1, c1 - c0, f);
	}
}

/*
 * Positive control: sample a different, known-running ASC. Reads only -
 * that block belongs to another driver. The address is supplied by the test
 * script, which checks the owning power domain is on before passing it.
 */
static void ave_ctl_asc_sample(struct device *dev)
{
	void __iomem *b;

	if (!ctl_asc)
		return;
	b = ioremap(ctl_asc, SZ_4K);
	if (!b) {
		dev_info(dev, "  [ctl] cannot map %#lx\n", ctl_asc);
		return;
	}
	dev_info(dev, "  [ctl] ASC at %#lx: CPU_CONTROL = %#x\n", ctl_asc,
		 readl_relaxed(b + 0x44));
	ave_status_histogram(dev, "ctl", b + 0x48);
	iounmap(b);
}

static int ave_probe_stages(struct platform_device *pdev)
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

	ave_ctl_asc_sample(dev);

	/* Before any stage: every later layout depends on it. */
	ret = ave_detect_fw_abi(ave);
	if (ret)
		return ret;

	/*
	 * With fw_map_text=0, DVA 0xb28000 holds OUR image, a different build
	 * linked at vmaddr 0. If an admitted TEXT fetch is translated, the core
	 * would run it. Any DAPF programming therefore requires an explicit
	 * TEXT policy. (Review 2026-09-13, finding 4.)
	 */
	if (ave_dapf_program_requested() && ave_fw_map_text_mode() == 0)
		return dev_err_probe(dev, -EINVAL,
				     "dapf_set= requires fw_map_text=1 or 2\n");

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
			ave->bank[i].phys = res->start;
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
		/*
		 * Not enabled until VENC is powered (stage 6): a line left
		 * asserted by an earlier run would otherwise fire the handler
		 * against a gated block the moment it is requested.
		 */
		ret = devm_request_irq(dev, ave->irq, ave_irq_handler,
				       IRQF_NO_AUTOEN, dev_name(dev), ave);
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
		ave->powered = true;
		/*
		 * Registered after the IRQ and the power-domain devres, so it is
		 * released first on every probe failure: quiesce the IRQ and drop
		 * power before pd detach and free_irq.
		 */
		ret = devm_add_action_or_reset(dev, ave_power_off_action, ave);
		if (ret)
			return dev_err_probe(dev, ret, "power-off action\n");
		if (ave->irq > 0) {
			enable_irq(ave->irq);
			ave->irq_enabled = true;
		}
		dev_info(dev, "  resumed; left powered for inspection\n");

		/*
		 * N1k (docs/49): program the DAPF straight after power-on,
		 * before stage 7's SVE+0x38 write (AVE_SVECtrl::SetIdle, called
		 * from SetClockGating(true)). apple-dart's resume-time writes to
		 * this DART succeed; every rejected write of ours came after
		 * stage 7.
		 */
		if (ave_dapf_early() == 2) {
			ret = ave_dapf_dump(ave);
			if (ret)
				return dev_err_probe(dev, ret, "DAPF dump\n");
			ave_step(ave, "stage 6: DAPF programming before the SVE idle write, next");
			ret = ave_dapf_program_selected(ave);
			if (ret)
				return dev_err_probe(dev, ret, "stage-6 DAPF program\n");
			ave_step(ave, "stage 6: DAPF programming returned");
		}
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

		/* E2 (docs/48): read-only DART + DAPF dump; no-op unless dapf_dump=1. */
		ret = ave_dapf_dump(ave);
		if (ret)
			return dev_err_probe(dev, ret, "DAPF dump\n");

		/*
		 * docs/49: host writes to CPUDART/DAPF raise a fatal SError once
		 * the later stages have run (plausibly the stage-11 IOP flag),
		 * while apple-dart's writes at runtime resume succeed. With
		 * dapf_early=1 program it here, before any of that.
		 */
		if (ave_dapf_early() == 1) {
			ave_step(ave, "stage 8: early DAPF programming next");
			ret = ave_dapf_program_selected(ave);
			if (ret)
				return dev_err_probe(dev, ret, "early DAPF program\n");
			ave_step(ave, "stage 8: early DAPF programming returned");
		}
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
		/*
		 * The 13.5 kext skips Config for iBoot-loaded chip types above 5
		 * (0xfffffe0008f1221c, docs/44); t6000 is type 8. Do as Apple
		 * does. The register is locked regardless.
		 */
		if (ave->fw_abi == AVE_ABI_MACOS_13_5) {
			dev_info(dev, "  13.5: Apple does not write RVBAR on this SoC; skipping\n");
			ave_stage_ok(dev, AVE_STAGE_IOP_CONFIG);
			goto iop_config_done;
		}
		dev_info(dev, "  writing fw base %#llx to ASC+0x%x ...\n",
			 v, AVE_ASC_FW_BASE);
		ave_write64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE, v);
		dev_info(dev, "  readback = %#llx\n",
			 readq_relaxed(ave->bank[AVE_BANK_ASC].base +
				       AVE_ASC_FW_BASE));
		ave_stage_ok(dev, AVE_STAGE_IOP_CONFIG);
iop_config_done:
		;
	} else {
		return 0;
	}

	/*
	 * ASC start must come AFTER firmware is mapped at IOVA 0. With a DART
	 * attached and nothing at 0, the core's first instruction fetch faults,
	 * the DART raises its interrupt, the handler clears it, the core
	 * retries - a handled-interrupt flood rather than a clean failure.
	 */
	/*
	 * Is the RVBAR write ignored because the register is locked, or
	 * because it is read-only, or because we are writing it at the wrong
	 * time? m1n1 treats bit 0 of an ASC RVBAR as a lock bit, and our live
	 * value has it set, so this walks a few values through and reports
	 * exactly which bits move - the cheapest way to tell those apart.
	 */
	if (rvbar_probe) {
		static const u64 test[] = {
			0x0102000000000000ULL,	/* tag only, base 0        */
			0x0000000000000000ULL,	/* everything clear        */
			0xffffffffffffffffULL,	/* everything set          */
		};
		u64 orig = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE);
		unsigned int i;

		dev_info(dev, "rvbar probe: original %#018llx (bit0=%llu)\n",
			 orig, orig & 1);
		for (i = 0; i < ARRAY_SIZE(test); i++) {
			u64 back;

			ave_write64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE, test[i]);
			back = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE);
			dev_info(dev, "  wrote %#018llx -> read %#018llx  %s\n",
				 test[i], back,
				 back == test[i] ? "TOOK" :
				 back == orig    ? "ignored" : "PARTIAL");
		}
		ave_write64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE, orig);
		dev_info(dev, "  restored, now %#018llx\n",
			 ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE));

		/*
		 * If bit 0 really is a lock, only a reset clears it. Power
		 * gating does not - the value survives every rmmod. Use the
		 * reset controller the DT already gives us rather than poking
		 * the PMGR registers behind genpd's back.
		 */
		if (rvbar_probe >= 2) {
			struct reset_control *rst;

			rst = devm_reset_control_get_optional_exclusive(dev, NULL);
			if (IS_ERR(rst)) {
				dev_info(dev, "  reset control unavailable: %ld\n",
					 PTR_ERR(rst));
			} else if (!rst) {
				dev_info(dev, "  no reset control in DT\n");
			} else {
				int rr = reset_control_reset(rst);

				dev_info(dev, "  reset_control_reset() = %d\n", rr);
				dev_info(dev, "  RVBAR after reset: %#018llx\n",
					 ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE));
				ave_write64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE,
					    0x0102000000000000ULL);
				dev_info(dev, "  wrote tag-only after reset -> %#018llx\n",
					 ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE));
			}
		}
	}

	/*
	 * E3 (docs/48): program the AVE DAPF; no-op unless dapf_set= is given.
	 * Reached with stop_after=12 (we are past the stage-12 early return),
	 * so E3a can verify the writes by readback without starting the core.
	 */
	ret = ave_dapf_write_probe(ave);	/* N1b; no-op unless dapf_probe=1 */
	if (ret)
		return dev_err_probe(dev, ret, "DAPF write probe\n");
	ave_step(ave, "stage 12 done; next: DAPF programming (if dapf_set)");
	ret = ave_dapf_program_selected(ave);
	if (ret)
		return dev_err_probe(dev, ret, "DAPF program\n");
	ave_step(ave, "DAPF step returned");

	ave_fw_snapshot_phys(ave);
	if (stop_after >= AVE_STAGE_ASC_START) {
		ave_fw_identify_phys(ave);
		ave_asc_liveness(ave, "halted ");
	}

	if (ave_stage(dev, AVE_STAGE_ASC_START)) {
		ave_step(ave, "next: ASC start (core released)");
		ret = ave_asc_start(ave);
		if (ret)
			return dev_err_probe(dev, ret, "ASC start\n");
		ave_step(ave, "ASC start returned");
		ave_stage_ok(dev, AVE_STAGE_ASC_START);
	} else {
		return 0;
	}
	if (ave_stage(dev, AVE_STAGE_RECV_MSG)) {
		u32 msg[4];

		dev_info(dev, "  waiting up to 2s for the firmware to speak ...\n");
		ret = ave_recv_iop_msg(ave, msg, 6000);
		if (ret) {
			dev_warn(dev, "  no message from firmware (%d)\n", ret);
		} else {
			dev_info(dev, "  MSG 1: %#010x %#010x %#010x %#010x\n",
				 msg[0], msg[1], msg[2], msg[3]);
		}
		ave_fw_log_dump(ave);
		ave_fw_globals_dump(ave);
		ave_fw_peek_phys(ave);
		ave_fw_diff_phys(ave);
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

		ave_asc_liveness(ave, "started");
		ave_dapf_dump(ave);	/* post-run DART/DAPF state, if dapf_dump=1 */

		/*
		 * Power off rather than halt. Writing CPU_CONTROL = 0 to a
		 * started core does nothing observable: on 2026-09-13 status
		 * stayed 0x2c (not STOPPED) and the DART kept faulting at
		 * ~70k/s. Only gating VENC stops it. Leaving it powered made
		 * the desktop stutter for as long as the module stayed loaded.
		 */
		if (stop_after < AVE_STAGE_START)
			ave_power_off(ave, "end of stage 15; CPU_CONTROL = 0 cannot stop a started core");
		ave_stage_ok(dev, AVE_STAGE_PROBE_STATE);
	} else {
		return 0;
	}

	if (stop_after >= AVE_STAGE_START) {
		ret = ave_start(ave);
		if (ret)
			return dev_err_probe(dev, ret, "coprocessor start\n");
		dev_info(dev, "Apple AVE video encoder ready\n");
	}

	return 0;
}

/*
 * Every probe failure after stage 9 used to leak FwIPC, the firmware buffer
 * and - worse - DART mappings in the device's persistent default domain,
 * including E3's RW mapping of iBoot DATA, so the next probe refused with
 * "already mapped". Unwind here, in the safe order: IRQ and power first,
 * then mappings and memory. Both unload functions are idempotent.
 * (Review 2026-09-13, finding 3.)
 */
static int ave_probe(struct platform_device *pdev)
{
	int ret = ave_probe_stages(pdev);
	struct ave_device *ave = platform_get_drvdata(pdev);

	if (ret && ave) {
		ave_power_off(ave, "probe failed");
		ave_fw_unload(ave);
		ave_ipc_fini(ave);
	}
	return ret;
}

static void ave_remove(struct platform_device *pdev)
{
	struct ave_device *ave = platform_get_drvdata(pdev);

	ave_stop(ave);

	/*
	 * The staged bring-up takes a runtime-PM reference at stage 6 and
	 * leaves the block powered on purpose, but ave_stop() only unwinds a
	 * *fully* started device. Without this, rmmod left VENC powered with
	 * the core still executing - and a core that faults on every
	 * instruction fetch then sits there generating a DART interrupt storm
	 * (measured: ~260k/s) that survives the driver being unloaded.
	 *
	 * Clearing CPU_CONTROL does NOT stop a started core (measured
	 * 2026-09-13: status stays 0x2c, faults continue). The write is kept
	 * because it is harmless and matches the state a fresh power-on reads
	 * back, but the power-down is what actually stops it.
	 *
	 * Known hazard: after the domain gates, the DART's shared IRQ line has
	 * been seen to stay asserted with nothing to report for ~9 s, until the
	 * kernel disables IRQ 129 as "nobody cared". The fault handler is then
	 * reading a gated DART. It has not hung, but it is not benign.
	 */
	ave_power_off(ave, "remove");

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
