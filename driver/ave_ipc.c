// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE (Video Encoder) - shared-memory IPC transport
 *
 * UNTESTED. This has never run on hardware.
 *
 * Step 5 of docs/22-driver-plan.md. The transport is NOT RTKit endpoint
 * messaging: AVE_IPC wraps a generic Apple IOProcessorChannel library that is
 * statically linked into both the kext and the firmware, so the ring semantics
 * below were read from both sides (docs/08-ipc-transport.md).
 *
 * The important structural point: head and tail are host-private. The firmware
 * never reads them. The only shared synchronisation is each slot's phase bit,
 * compared against the channel's current phase.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include "ave.h"

#define AVE_HANDSHAKE_MAGIC	0x08042006	/* UNVERIFIED */
#define AVE_HANDSHAKE_TIMEOUT_US 500000

/*
 * Address translation.
 *
 * Three spaces exist: kernel virtual, DART IOVA, and "firmware". On t6001
 * (arch 0x40) the firmware base is not computed by the host - the firmware
 * reports it over the scratch mailbox during the handshake, and the host
 * installs it. So fw_addr = iova - ipc_iova + fw_base.
 */
static u64 ave_kern_to_fw(struct ave_device *ave, dma_addr_t iova)
{
	return (u64)(iova - ave->ipc.iova) + ave->fw.fw_base;
}

void ave_ipc_ring_doorbell(struct ave_device *ave, u32 chan_id)
{
	struct ave_channel *ch = &ave->chan[chan_id];

	/*
	 * Publish everything written to the ring before the doorbell. The
	 * library uses a single dsb st here and installs its cache-maintenance
	 * hooks as NULL, so no explicit cache maintenance is needed - the
	 * region is DMA-coherent.
	 */
	dma_wmb();
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_DOORBELL, 1u << ch->doorbell_bit);
}

/*
 * Push one message. A slot is {payload_fw_addr | phase, arg1, arg2}; only 24
 * of the 0x40 bytes are used.
 */
int ave_ipc_send(struct ave_device *ave, u32 chan_id, dma_addr_t payload,
		 u64 arg1, u64 arg2)
{
	struct ave_channel *ch;
	struct ave_ipc_slot *slot;
	u32 next;

	if (chan_id >= ave->nchannels)
		return -EINVAL;
	ch = &ave->chan[chan_id];

	next = (ch->head + 1) % ch->nslots;
	if (next == ch->tail)
		return -EAGAIN;			/* ring full */

	slot = (struct ave_ipc_slot *)ch->ring + ch->head;

	/*
	 * The low bit of the address word is the phase bit, so the payload
	 * address must be even. It always is - the ChkPool granule is 64 bytes.
	 */
	slot->payload_fw_addr =
		cpu_to_le64(ave_kern_to_fw(ave, payload) | ch->phase);
	slot->arg1 = cpu_to_le64(arg1);
	slot->arg2 = cpu_to_le64(arg2);

	ch->head = next;
	if (!ch->head)
		ch->phase ^= 1;			/* wrapped: flip the phase */

	ave_ipc_ring_doorbell(ave, chan_id);
	return 0;
}

/* Pop one message if the producer has published it (phase bit matches). */
int ave_ipc_recv(struct ave_device *ave, u32 chan_id, u64 *out, size_t n)
{
	struct ave_channel *ch;
	struct ave_ipc_slot *slot;
	u64 addr;

	if (chan_id >= ave->nchannels || n < 3)
		return -EINVAL;
	ch = &ave->chan[chan_id];

	slot = (struct ave_ipc_slot *)ch->ring + ch->tail;
	addr = le64_to_cpu(slot->payload_fw_addr);

	/*
	 * Each channel is unidirectional ("IO" is host->fw, "IO_T2H" is
	 * fw->host), so one phase per channel is sufficient - send and recv
	 * are never both used on the same channel.
	 */
	if ((addr & 1) != ch->phase)
		return -EAGAIN;			/* nothing published yet */

	dma_rmb();
	out[0] = addr & ~1ULL;
	out[1] = le64_to_cpu(slot->arg1);
	out[2] = le64_to_cpu(slot->arg2);

	ch->tail = (ch->tail + 1) % ch->nslots;
	return 0;
}

/*
 * Scratch-register handshake.
 *
 * Sequence from AVE_HwC::StartUpIOP: publish the IPC region's IOVA, wait for
 * the firmware to answer, then adopt the firmware base address it reports.
 *
 * UNVERIFIED: the 0x08042006 magic and the exact scratch register assignment
 * were reported by analysis but not re-checked instruction by instruction.
 * This is the most likely thing here to be wrong on first boot.
 */
int ave_ipc_handshake(struct ave_device *ave)
{
	u32 val;
	int ret;

	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1),
		  lower_32_bits(ave->ipc.iova));
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2),
		  upper_32_bits(ave->ipc.iova));
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(3), ave->ipc.size);
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0), AVE_HANDSHAKE_MAGIC);

	ret = readl_relaxed_poll_timeout(
		ave->bank[AVE_BANK_SVE].base + AVE_SVE_SCRATCH(0),
		val, val != AVE_HANDSHAKE_MAGIC,
		1000, AVE_HANDSHAKE_TIMEOUT_US);
	if (ret) {
		dev_err(ave->dev, "firmware did not answer the handshake\n");
		return ret;
	}

	ave->fw.fw_base = (u64)ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2)) << 32 |
			  ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1));
	dev_info(ave->dev, "firmware reports base %#llx (scratch0 %#x)\n",
		 ave->fw.fw_base, val);

	/*
	 * TODO: a second exchange returns the firmware address of the channel
	 * descriptor array, from which doorbell bit and slot count are read
	 * (AVE_IPC_DESC_* in ave_abi.h). Until that is implemented we cannot
	 * populate ave->chan[] correctly, so no channels are advertised.
	 */
	ave->nchannels = 0;
	return 0;
}

/*
 * Hand the firmware its boot arguments, BEFORE the core is started.
 *
 * This is the step that was missing. AVE_HwC::StartUpIOP does, between
 * AVE_IPC::Alloc and AVE_IOP::Config:
 *
 *   MakeFwCfg(cfg)                  0xfffffe0008c1d978
 *   SetIOPFlag(0)                   0xfffffe0008c1da64  scratch0 = magic
 *   d = Kernel2DARTAddr(cfg)        0xfffffe0008c1db50
 *   WriteScratch(1, lo32(d))        0xfffffe0008c1dc7c
 *   WriteScratch(2, hi32(d))        0xfffffe0008c1dd10
 *
 * and only then Config and Start. Starting the core with the scratch registers
 * zero makes the firmware take its standalone branch (fw 0xe115c) and go quiet,
 * which is precisely what we observed.
 */
int ave_boot_config(struct ave_device *ave)
{
	struct ave_fw_cfg *cfg;

	/*
	 * The firmware log surface. Its address goes in the boot block, so the
	 * firmware writes its log where we can read it - which is the whole
	 * observability story for this project.
	 *
	 * Size is a guess: gs_saAVE_SurfaceCfg carries no size field, and the
	 * real size comes from a per-surface calculation we have not mapped.
	 */
	ave->fwlog.size = AVE_FWLOG_SIZE;
	ave->fwlog.cpu = dma_alloc_coherent(ave->dev, ave->fwlog.size,
					    &ave->fwlog.iova, GFP_KERNEL);
	if (!ave->fwlog.cpu)
		return -ENOMEM;

	ave->fwcfg.size = AVE_FW_CFG_SIZE;
	ave->fwcfg.cpu = dma_alloc_coherent(ave->dev, ave->fwcfg.size,
					    &ave->fwcfg.iova, GFP_KERNEL);
	if (!ave->fwcfg.cpu) {
		dma_free_coherent(ave->dev, ave->fwlog.size, ave->fwlog.cpu,
				  ave->fwlog.iova);
		ave->fwlog.cpu = NULL;
		return -ENOMEM;
	}

	/*
	 * Initialise the log ring header before the firmware ever looks at it.
	 * The per-subsystem level table is host memory that the firmware reads
	 * live but never writes, and it maps the buffer once at init - so
	 * levels must be set now, not later.
	 */
	{
		u8 *h = ave->fwlog.cpu;

		memset(h, 0, AVE_FWLOG_RING_OFF);
		*(__le32 *)(h + AVE_FWLOG_HDR_RING_OFF)  = cpu_to_le32(AVE_FWLOG_RING_OFF);
		*(__le32 *)(h + AVE_FWLOG_HDR_RING_SIZE) = cpu_to_le32(AVE_FWLOG_RING_SIZE);
		*(__le32 *)(h + AVE_FWLOG_HDR_UNK_10C)   = cpu_to_le32(25);
		*(__le32 *)(h + AVE_FWLOG_HDR_UNK_110)   = cpu_to_le32(20000);
		h[AVE_FWLOG_HDR_CONF + AVE_LOG_SUBSYS_CMDPROC] = AVE_LOG_LEVEL_DBG;
		h[AVE_FWLOG_HDR_CONF + AVE_LOG_SUBSYS_AVC]     = AVE_LOG_LEVEL_DBG;
		h[AVE_FWLOG_HDR_CONF + AVE_LOG_SUBSYS_HEVC]    = AVE_LOG_LEVEL_DBG;
	}

	cfg = ave->fwcfg.cpu;
	memset(cfg, 0, sizeof(*cfg));
	cfg->dev_index         = cpu_to_le32(0);	  /* ave0            */
	cfg->dev_id            = cpu_to_le32(AVE_DEVID_T6001);
	cfg->dev_num           = cpu_to_le32(2);	  /* two instances   */
	cfg->dev_num_per_group = cpu_to_le32(1);	  /* TODO: unverified */
	cfg->dev_subid_flag    = cpu_to_le64(0);
	cfg->dev_revision      = cpu_to_le32(0);	  /* TODO: unreadable */
	cfg->log_addr          = cpu_to_le64(ave->fwlog.iova);
	cfg->log_size          = cpu_to_le32(ave->fwlog.size);
	cfg->cfg30             = cpu_to_le32(0);

	dev_info(ave->dev, "  boot cfg at iova %pad, log surface %zu KiB at %pad\n",
		 &ave->fwcfg.iova, ave->fwlog.size >> 10, &ave->fwlog.iova);

	/* Order matters: flag first, then the pointer halves. */
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0), AVE_IOP_FLAG_HOST_MODE);
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1),
		  lower_32_bits(ave->fwcfg.iova));
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2),
		  upper_32_bits(ave->fwcfg.iova));

	dev_info(ave->dev, "  scratch0=%#x scratch1=%#x scratch2=%#x\n",
		 ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0)),
		 ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1)),
		 ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2)));
	return 0;
}

/*
 * Wait for a message from the firmware.
 *
 * RecvIOPMsg (0xfffffe0008c9161c) polls bank2+0x10 bit 0, clears it
 * write-1-to-clear BEFORE reading, then takes scratch 0..3 as the payload.
 * Apple's default timeout is about 2 s.
 */
int ave_recv_iop_msg(struct ave_device *ave, u32 out[4], unsigned int timeout_ms)
{
	unsigned int i;
	u32 st;

	for (i = 0; i < timeout_ms * 1000 / 200; i++) {
		st = ave_read(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS);
		if (st & 1) {
			ave_write(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS, 1);
			out[0] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0));
			out[1] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(1));
			out[2] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(2));
			out[3] = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(3));
			return 0;
		}
		udelay(200);
	}
	return -ETIMEDOUT;
}

/*
 * Drain the firmware log ring.
 *
 * Entries are NUL-terminated strings packed back to back; the counters are
 * free-running and wrap modulo the ring size.
 */
void ave_fw_log_dump(struct ave_device *ave)
{
	u8 *h = ave->fwlog.cpu;
	u32 rd, wr, ring;
	char line[256];
	unsigned int n = 0;

	if (!h)
		return;

	ring = le32_to_cpu(*(__le32 *)(h + AVE_FWLOG_HDR_RING_SIZE));
	rd   = le32_to_cpu(*(__le32 *)(h + AVE_FWLOG_HDR_RD));
	wr   = le32_to_cpu(*(__le32 *)(h + AVE_FWLOG_HDR_WR));

	dev_info(ave->dev, "  fw log: rd=%u wr=%u ring=%#x\n", rd, wr, ring);
	if (!ring || rd == wr)
		return;

	while (rd != wr && n < 64) {
		unsigned int i = 0;
		char c;

		do {
			c = h[AVE_FWLOG_RING_OFF + (rd++ % ring)];
			if (i < sizeof(line) - 1)
				line[i++] = c;
		} while (c && rd != wr);
		line[i] = 0;
		if (line[0])
			dev_info(ave->dev, "  fw| %s\n", line);
		n++;
	}
	*(__le32 *)(h + AVE_FWLOG_HDR_RD) = cpu_to_le32(rd);
}

/*
 * Read the firmware image's own globals.
 *
 * The image is mapped at IOVA 0 out of a buffer we still hold a CPU pointer
 * to, and it is linked at vmaddr 0, so a firmware VA is just an offset into
 * that buffer. This needs no protocol at all and works even if the core is
 * dead - which is exactly what makes it useful: it distinguishes "never
 * executed" from "booted and rejected our configuration".
 */
void ave_fw_globals_dump(struct ave_device *ave)
{
	static const struct { u32 off; const char *name; } g[] = {
		{ 0x195090, "gs_psCfg (NULL => AVE_Log_Output disabled)" },
		{ 0x1950b0, "gs_psCfg+0x20" },
		{ 0x2649a0, "__rtk_crashlog_local_buffer" },
		{ 0x2649c8, "crashlog related" },
	};
	unsigned int i;

	if (!ave->fw.cpu)
		return;

	dev_info(ave->dev, "  firmware globals (read straight out of our image buffer):\n");
	for (i = 0; i < ARRAY_SIZE(g); i++) {
		if (g[i].off + 8 > ave->fw.size)
			continue;
		dev_info(ave->dev, "    %#08x = %#018llx  %s\n", g[i].off,
			 le64_to_cpup((__le64 *)((u8 *)ave->fw.cpu + g[i].off)),
			 g[i].name);
	}
}

int ave_ipc_init(struct ave_device *ave)
{
	/*
	 * One 20 MiB DART-mapped region. Apple carves it with a buddy allocator
	 * on a 64-byte granule; we can start with plain offsets and add an
	 * allocator when the command layer needs one.
	 */
	ave->ipc.size = AVE_IPC_SURFACE_SIZE;
	ave->ipc.cpu = dma_alloc_coherent(ave->dev, ave->ipc.size,
					  &ave->ipc.iova, GFP_KERNEL);
	if (!ave->ipc.cpu)
		return -ENOMEM;

	dev_info(ave->dev, "IPC region %zu MiB at iova %pad\n",
		 ave->ipc.size >> 20, &ave->ipc.iova);
	return 0;
}

void ave_ipc_fini(struct ave_device *ave)
{
	if (!ave->ipc.cpu)
		return;
	dma_free_coherent(ave->dev, ave->ipc.size, ave->ipc.cpu, ave->ipc.iova);
	ave->ipc.cpu = NULL;
}
