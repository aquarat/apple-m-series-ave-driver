// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE (Video Encoder) - boot handshake and shared-memory IPC transport
 *
 * UNTESTED. This has never run on hardware.
 *
 * Two firmware ABIs are carried and selected by ave->fw_abi through the
 * descriptor in ave_abi_boot.h. Every constant that differs lives there with
 * its evidence VA; this file is the sequence. Spec: docs/45 (both versions),
 * docs/34 and docs/36 (26.6.2), docs/08 (ring).
 *
 * The transport is NOT RTKit endpoint messaging (docs/36 §1): eight scratch
 * registers plus a doorbell bit for the boot conversation, then rings in the
 * FwIPC surface whose only shared synchronisation is each slot's phase bit.
 *
 * Boot conversation, host side (AVE_HwC::StartUpIOP, k13 f119e0 / k26 c1d354):
 *
 *   pre-start  scratch0 = 0x08042006; scratch1/2 =
 *                13.5:   instance index / DevID (14)       k13 f11d08 f12004 f1211c
 *                26.6.2: IOVA of the 56-byte _S_AVE_Fw_Cfg k26 c1da64 c1dc7c c1dd10
 *   msg 1  F->H  nch, chanmem size, 0x100, heap size       k13 f123fc / k26 c1dfe4
 *              26.6.2: heap surface now; scratch4/5 = time base
 *   msg 2  H->F  FwIPC IOVA lo, hi, size, 0                k13 f12f64 / k26 c1eb88
 *   msg 3  F->H  fw_base lo, hi                            k13 f12ff8 / k26 c1ec1c
 *              13.5: heap surface now (k13 f128b0)
 *              chanmem (64-aligned, zeroed), info block (0x50, zeroed),
 *              26.6.2: + 64 KiB block at info +0x10
 *   msg 4  H->F  info fw addr lo, hi, 0, 0                 k13 f137a0 / k26 c1f76c
 *   msg 5  F->H  desc fw addr lo, hi, client buffer size   k13 f13984 / k26 c1f818
 *              desc must translate to chanmem; bind every descriptor
 *   ready  scratch3 = 0x08042006, poll until 0            k13 f13ed8 / k26 c1fcbc
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <clocksource/arm_arch_timer.h>

#include "ave.h"
#include "ave_abi_boot.h"

static bool boot_lenient;
module_param(boot_lenient, bool, 0444);
MODULE_PARM_DESC(boot_lenient, "continue the boot handshake when message 1 does not match the selected ABI's channel count / block size");

enum ave_boot_phase {
	AVE_BOOT_IDLE = 0,
	AVE_BOOT_ARGS_WRITTEN,	/* ave_boot_config() done, core may be started */
	AVE_BOOT_MSG1,		/* message 1 received and stored in ave->msg1 */
	AVE_BOOT_READY,		/* ready flag cleared, rings live */
	AVE_BOOT_FAILED,
};

/* ------------------------------------------------------------------------ */
/* Scratch mailbox                                                          */
/* ------------------------------------------------------------------------ */

static inline u32 ave_scratch(struct ave_device *ave, unsigned int i)
{
	return ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(i));
}

static inline void ave_set_scratch(struct ave_device *ave, unsigned int i, u32 v)
{
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(i), v);
}

/*
 * SendIOPMsg(a, b, c, d): scratch 0..3, then SetIntr(1) - it rings the
 * mailbox doorbell itself (k13 f41340..f413b8, k26 c9143c..c914f8).
 */
static void ave_send_iop_msg(struct ave_device *ave, u32 a, u32 b, u32 c, u32 d)
{
	ave_set_scratch(ave, 0, a);
	ave_set_scratch(ave, 1, b);
	ave_set_scratch(ave, 2, c);
	ave_set_scratch(ave, 3, d);
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_DOORBELL, BIT(AVE_BOOT_MBOX_BIT));
}

/*
 * RecvIOPMsg: poll status bit 0, W1C it BEFORE reading scratch 0..3
 * (k13 f41534..f416dc, k26 c9161c..c917f8). Apple polls in 1 ms steps.
 *
 * Our IRQ line is live during the handshake (Apple's is not, docs/34 §14), so
 * the handler may see bit 0 first; it then stores the message under ipc_lock
 * and we take it from there. Both paths run under the lock, so exactly one of
 * them observes the bit and the message is neither lost nor seen twice.
 *
 * The first message after ave_boot_config() is message 1 and is kept in
 * ave->msg1, so a caller that reads it early (the staged probe) does not make
 * ave_ipc_handshake() wait for a message that already came.
 */
int ave_recv_iop_msg(struct ave_device *ave, u32 out[4], unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms) + 1;
	unsigned long flags;
	bool got = false;
	u32 st;

	if (!ave->boot_abi)
		return -EINVAL;

	for (;;) {
		spin_lock_irqsave(&ave->ipc_lock, flags);
		if (ave->mbox_pending) {
			memcpy(out, ave->mbox, sizeof(ave->mbox));
			ave->mbox_pending = false;
			got = true;
			dev_info(ave->dev, "  message taken from the IRQ handler (status %#x)\n",
				 ave->mbox_status);
		} else {
			st = ave_read(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS);
			if (st & BIT(AVE_BOOT_MBOX_BIT)) {
				ave_write(ave, AVE_BANK_SVE, AVE_SVE_INTR_STATUS,
					  BIT(AVE_BOOT_MBOX_BIT));
				out[0] = ave_scratch(ave, 0);
				out[1] = ave_scratch(ave, 1);
				out[2] = ave_scratch(ave, 2);
				out[3] = ave_scratch(ave, 3);
				got = true;
			}
		}
		if (got && ave->boot_phase == AVE_BOOT_ARGS_WRITTEN) {
			memcpy(ave->msg1, out, sizeof(ave->msg1));
			ave->boot_phase = AVE_BOOT_MSG1;
		}
		spin_unlock_irqrestore(&ave->ipc_lock, flags);

		if (got)
			return 0;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}
}

/* ------------------------------------------------------------------------ */
/* FwIPC pool and address translation                                       */
/* ------------------------------------------------------------------------ */

/*
 * Kernel2FwAddr(k) = k - kbase + [IPC+0x38]; Fw2KernelAddr the inverse, and
 * range-checked against the surface (k13 f25c50..f25c58, f253b8..f2541c).
 * fw_base comes from message 3 and is not the IOVA.
 */
u64 ave_ipc_cpu_to_fw(struct ave_device *ave, void *cpu)
{
	return (u64)((u8 *)cpu - (u8 *)ave->ipc.cpu) + ave->ipc_fw_base;
}

void *ave_ipc_fw_to_cpu(struct ave_device *ave, u64 fw, size_t len)
{
	u64 off;

	if (!ave->ipc.cpu || fw < ave->ipc_fw_base)
		return NULL;
	off = fw - ave->ipc_fw_base;
	if (off >= ave->ipc.size || len > ave->ipc.size - off)
		return NULL;
	return (u8 *)ave->ipc.cpu + off;
}

static dma_addr_t ave_ipc_cpu_to_iova(struct ave_device *ave, void *cpu)
{
	return ave->ipc.iova + ((u8 *)cpu - (u8 *)ave->ipc.cpu);
}

/*
 * AVE_IPC::Alloc carves FwIPC with a buddy pool on a 64-byte granule
 * (k26 c42f00; k13 f233f8, ChkPool shape k13 f22c04..f22c34). A 64-byte
 * genalloc granule gives the same alignment, which message 5 depends on:
 * the firmware aligns the channel block up to 64 and the host compares for
 * equality (docs/36 §4). Safe from IRQ context (SHAREDMALLOC).
 */
void *ave_ipc_alloc(struct ave_device *ave, size_t size, dma_addr_t *iova)
{
	unsigned long va;

	if (!ave->ipc_pool || !size)
		return NULL;
	va = gen_pool_alloc(ave->ipc_pool, size);
	if (!va)
		return NULL;
	memset((void *)va, 0, size);
	if (iova)
		*iova = ave_ipc_cpu_to_iova(ave, (void *)va);
	return (void *)va;
}

void ave_ipc_free(struct ave_device *ave, void *cpu, size_t size)
{
	if (ave->ipc_pool && cpu && size)
		gen_pool_free(ave->ipc_pool, (unsigned long)cpu, size);
}

/* ------------------------------------------------------------------------ */
/* Rings                                                                    */
/* ------------------------------------------------------------------------ */

static void ave_ch_to_ring(const struct ave_channel *ch, struct ave_ring *r)
{
	r->slots  = ch->ring;
	r->nslots = ch->nslots;
	r->type   = ch->type;
	r->rd     = ch->rd;
	r->wr     = ch->wr;
	r->nrecv  = ch->nrecv;
	r->nsend  = ch->nsend;
}

static void ave_ring_to_ch(const struct ave_ring *r, struct ave_channel *ch)
{
	ch->rd    = r->rd;
	ch->wr    = r->wr;
	ch->nrecv = r->nrecv;
	ch->nsend = r->nsend;
}

static struct ave_channel *ave_chan(struct ave_device *ave, u32 id)
{
	if (id >= ARRAY_SIZE(ave->chan) || !ave->chan[id].bound)
		return NULL;
	return &ave->chan[id];
}

/*
 * SetChIntr -> SetIPCIntr(desc +0x44) -> Write32(bank 2, 0x0C, 1 << bit)
 * (k26 c43bc8, c91c48; k13 SetChIntr f24244, SetIntr f413f4..f41404).
 * One doorbell per Send, including the IO_T2H credit return.
 */
void ave_ipc_ring_doorbell(struct ave_device *ave, u32 chan_id)
{
	struct ave_channel *ch = ave_chan(ave, chan_id);

	if (!ch)
		return;
	dma_wmb();
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_DOORBELL, BIT(ch->doorbell_bit));
}

/* Send64 on a bound channel; payload is a firmware address. */
static int ave_chan_send_fw(struct ave_device *ave, u32 id, u64 fw, u32 a1, u32 a2)
{
	struct ave_channel *ch = ave_chan(ave, id);
	struct ave_ring r;
	unsigned long flags;
	int ret;

	if (!ch)
		return -ENODEV;
	spin_lock_irqsave(&ave->ipc_lock, flags);
	ave_ch_to_ring(ch, &r);
	ret = ave_ring_send(&r, fw, a1, a2);
	ave_ring_to_ch(&r, ch);
	spin_unlock_irqrestore(&ave->ipc_lock, flags);
	if (ret)
		return -EAGAIN;		/* ring full */
	ave_ipc_ring_doorbell(ave, id);
	return 0;
}

/* Receive64, or UnidirectionalReceive64 for type-2 rings (TERMINAL). */
static int ave_chan_recv_fw(struct ave_device *ave, u32 id, u64 *fw, u32 *a1, u32 *a2)
{
	struct ave_channel *ch = ave_chan(ave, id);
	struct ave_ring r;
	unsigned long flags;
	int ret;

	if (!ch)
		return -ENODEV;
	spin_lock_irqsave(&ave->ipc_lock, flags);
	ave_ch_to_ring(ch, &r);
	if (r.type & 2)
		ret = ave_ring_urecv(&r, fw, a1, a2);
	else
		ret = ave_ring_recv(&r, fw, a1, a2);
	ave_ring_to_ch(&r, ch);
	spin_unlock_irqrestore(&ave->ipc_lock, flags);
	return ret ? -EAGAIN : 0;
}

/*
 * Push one message. payload is an IOVA inside FwIPC (0 is allowed and sent
 * as 0); it is translated to the firmware's address. Only 32 bits of each
 * argument reach the slot (Send64 zero-extends, docs/36 §6).
 */
int ave_ipc_send(struct ave_device *ave, u32 chan_id, dma_addr_t payload,
		 u64 arg1, u64 arg2)
{
	u64 fw = 0;

	/*
	 * Apple gates sends on HwC state 3, i.e. after the ready flag clears.
	 * ipc_up now flips before the flag is written (so the IRQ handler
	 * treats bit 0 as a doorbell), so it is not the right gate here.
	 */
	if (READ_ONCE(ave->boot_phase) != AVE_BOOT_READY)
		return -ENODEV;
	if (payload) {
		if (payload < ave->ipc.iova ||
		    payload - ave->ipc.iova >= ave->ipc.size)
			return -EINVAL;
		fw = payload - ave->ipc.iova + ave->ipc_fw_base;
		if (fw & 3)
			return -EINVAL;	/* Receive masks two low bits */
	}
	return ave_chan_send_fw(ave, chan_id, fw, (u32)arg1, (u32)arg2);
}

/*
 * Pop one message if the producer has published it.
 * out[0] = payload as an IOVA (0 if the firmware address is outside FwIPC),
 * out[1] = arg1 (size), out[2] = arg2 (flags), out[3] = raw firmware address
 * if n >= 4.
 */
int ave_ipc_recv(struct ave_device *ave, u32 chan_id, u64 *out, size_t n)
{
	u32 a1, a2;
	void *cpu;
	u64 fw;
	int ret;

	if (n < 3)
		return -EINVAL;
	ret = ave_chan_recv_fw(ave, chan_id, &fw, &a1, &a2);
	if (ret)
		return ret;
	cpu = fw ? ave_ipc_fw_to_cpu(ave, fw, 1) : NULL;
	out[0] = cpu ? ave_ipc_cpu_to_iova(ave, cpu) : 0;
	out[1] = a1;
	out[2] = a2;
	if (n >= 4)
		out[3] = fw;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Channel services after the handshake (13.5 channels 3..7)                */
/* ------------------------------------------------------------------------ */

/*
 * TERMINAL -> AVE_HwC::ProcessIntr_Log(buf, len, flags) (k13 f0d5ec):
 * NULL buf ignored (f0d604); len clamped to 0x200 (f0d648..f0d650); Apple
 * prints only if flags is 1..3 or a debug cfg byte is set (f0d610..f0d628),
 * and NUL-terminates in place in shared memory (f0d654). We copy instead and
 * print every line with its level.
 *
 * Firmware side (f13 CLoggerInterProcessor::PrintInterProcessor 0xa46f4):
 * vsnprintf into a rotating 0x80-byte slice of a buffer from
 * CSharedMemory::MallocSynchronous, then UnidirectionalSend(buf, len, 0).
 * Whether that buffer lies in FwIPC (reachable through SHAREDMALLOC) or in
 * the firmware heap (not translatable by us) depends on a CSharedMemory flag
 * (+104, f13 0x97d58) that was not traced. Out-of-range lines are counted.
 */
static void ave_ipc_terminal_line(struct ave_device *ave, u64 fw, u32 len, u32 level)
{
	char line[0x201];
	const u8 *p;
	size_t n;

	if (!fw)
		return;
	n = min_t(u32, len, 0x200);
	p = ave_ipc_fw_to_cpu(ave, fw, n ? n : 1);
	if (!p) {
		dev_warn_ratelimited(ave->dev, "fw log line at fw %#llx (len %u) is outside FwIPC\n",
				     fw, len);
		return;
	}
	memcpy(line, p, n);
	line[n] = 0;
	n = strnlen(line, n);
	while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = 0;

	ave->fwlog_lines++;
	/*
	 * While a V4L2 stream runs the firmware's per-frame chatter (the RC's
	 * "MiniGOP" line, f87) goes to debug; anything that asserts or says it
	 * failed is still shown.
	 */
	if (ave->dart_check_quiet && !strstr(line, "ASSERT") &&
	    !strstr(line, "rror") && !strstr(line, "ail")) {
		dev_dbg(ave->dev, "fw[%u]| %s\n", level, line);
		return;
	}
	if (__ratelimit(&ave->fwlog_rs))
		dev_info(ave->dev, "fw[%u]| %s\n", level, line);
}

/*
 * SHAREDMALLOC -> AVE_HwC::ProcessIntr_IPCMem(buf, size) (k13 f0d34c):
 *   buf != NULL: AVE_IPC::Free(buf) (f0d450), Send(3, buf, 0) (f0d478)
 *   buf == NULL: AVE_IPC::Alloc(size, &buf) (f0d4a0), Send(3, buf, size) (f0d4cc)
 * On allocation failure Apple's Send gets a NULL buffer and sends nothing; we
 * do the same and log it.
 */
static void ave_ipc_shmalloc(struct ave_device *ave, u64 fw, u32 size)
{
	unsigned long flags;
	unsigned int i;
	void *cpu;

	if (fw) {
		u32 sz = 0;

		cpu = ave_ipc_fw_to_cpu(ave, fw, 1);
		if (!cpu) {
			dev_err_ratelimited(ave->dev, "SHAREDMALLOC free of fw %#llx outside FwIPC\n", fw);
			return;
		}
		spin_lock_irqsave(&ave->ipc_lock, flags);
		for (i = 0; i < AVE_SHMALLOC_MAX; i++) {
			if (ave->shm[i].cpu == cpu) {
				sz = ave->shm[i].size;
				ave->shm[i].cpu = NULL;
				break;
			}
		}
		spin_unlock_irqrestore(&ave->ipc_lock, flags);
		if (sz)
			ave_ipc_free(ave, cpu, sz);
		else
			dev_err_ratelimited(ave->dev, "SHAREDMALLOC free of untracked fw %#llx\n", fw);
		ave_chan_send_fw(ave, AVE_CH_SHAREDMALLOC, fw, 0, 0);
		return;
	}

	cpu = ave_ipc_alloc(ave, size, NULL);
	if (cpu) {
		spin_lock_irqsave(&ave->ipc_lock, flags);
		for (i = 0; i < AVE_SHMALLOC_MAX && ave->shm[i].cpu; i++)
			;
		if (i < AVE_SHMALLOC_MAX) {
			ave->shm[i].cpu = cpu;
			ave->shm[i].size = size;
		}
		spin_unlock_irqrestore(&ave->ipc_lock, flags);
		if (i == AVE_SHMALLOC_MAX) {
			ave_ipc_free(ave, cpu, size);
			cpu = NULL;
		}
	}
	if (!cpu) {
		dev_err_ratelimited(ave->dev, "SHAREDMALLOC of %u bytes failed; firmware left waiting\n",
				    size);
		return;
	}
	dev_dbg(ave->dev, "SHAREDMALLOC %u bytes -> fw %#llx\n", size,
		ave_ipc_cpu_to_fw(ave, cpu));
	ave_chan_send_fw(ave, AVE_CH_SHAREDMALLOC, ave_ipc_cpu_to_fw(ave, cpu), size, 0);
}

/*
 * AVE_HwC::ProcessIntr_IPCCh(ch) (k13 f0d6d4; k26 c196a0): drain while Recv
 * succeeds; 1 -> CmdAck, 2 -> Send(2) credit return then Cmd, 3 -> IPCMem,
 * 4 -> Log; 5..7 -> one message consumed, -1002 (k13 f0d718 -> f0d894).
 */
static void ave_ipc_drain(struct ave_device *ave, u32 id)
{
	void (*rx)(struct ave_device *, u32, void *, u32, u32);
	u32 size, flags;
	u64 fw;

	while (!ave_chan_recv_fw(ave, id, &fw, &size, &flags)) {
		switch (id) {
		case AVE_CH_IO:
		case AVE_CH_IO_T2H:
			if (id == AVE_CH_IO_T2H)
				ave_chan_send_fw(ave, id, fw, size, 0);
			rx = READ_ONCE(ave->ipc_rx);
			if (rx)
				/*
				 * Validate for the length the consumer will
				 * read, not one byte: a reply in the last
				 * bytes of FwIPC would otherwise be copied
				 * past the end of the region.
				 */
				rx(ave, id,
				   fw ? ave_ipc_fw_to_cpu(ave, fw, size ?: 1) : NULL,
				   size, flags);
			else
				dev_dbg(ave->dev, "ch %u: fw %#llx size %u flags %#x (no consumer)\n",
					id, fw, size, flags);
			break;
		case AVE_CH_SHAREDMALLOC:
			ave_ipc_shmalloc(ave, fw, size);
			break;
		case AVE_CH_TERMINAL:
			ave_ipc_terminal_line(ave, fw, size, flags);
			break;
		default:
			dev_warn_ratelimited(ave->dev, "ch %u: unhandled message fw %#llx size %u (Apple: -1002)\n",
					     id, fw, size);
			return;
		}
	}
}

/*
 * AVE_HwC::ProcessIntr (k13 f0d8ac): W1C of the whole word happens in the
 * IRQ handler; then ch = 1..7 (k13 f0d9dc, cmp w21,#8 f0dae8), CheckChIntr
 * = (status >> doorbellBit[ch]) & 1 (k13 f246c0..f246d4). Doorbell bits are
 * shared on 13.5 (1: IO, DEBUG; 3: BUF_T2H, SHAREDMALLOC, IO_T2H), so every
 * channel whose bit is set is drained.
 */
static void ave_ipc_irq_dispatch(struct ave_device *ave, u32 status)
{
	u32 id;

	for (id = 1; id < AVE_CH_MAX; id++) {
		struct ave_channel *ch = ave_chan(ave, id);

		if (ch && (status & BIT(ch->doorbell_bit)))
			ave_ipc_drain(ave, id);
	}
}

/* ------------------------------------------------------------------------ */
/* Pre-start boot arguments                                                 */
/* ------------------------------------------------------------------------ */

/* 26.6.2: the FwLog surface whose address goes in the cfg block (docs/33). */
static int ave_boot_fwlog_alloc(struct ave_device *ave)
{
	u8 *h;

	if (ave->fwlog.cpu)
		return 0;
	ave->fwlog.size = AVE_BOOT_FWLOG_SIZE;
	ave->fwlog.cpu = dma_alloc_coherent(ave->dev, ave->fwlog.size,
					    &ave->fwlog.iova, GFP_KERNEL);
	if (!ave->fwlog.cpu)
		return -ENOMEM;

	/*
	 * The firmware reads the per-subsystem level table live and maps the
	 * buffer once at init, so the header is set before Start (docs/33 §8).
	 */
	h = ave->fwlog.cpu;
	memset(h, 0, AVE_BOOT_FWLOG_RING_OFF);
	put_unaligned_le32(AVE_BOOT_FWLOG_RING_OFF, h + AVE_BOOT_FWLOG_HDR_RING_OFF);
	put_unaligned_le32(AVE_BOOT_FWLOG_RING_SIZE, h + AVE_BOOT_FWLOG_HDR_RING_SIZE);
	put_unaligned_le32(25, h + AVE_BOOT_FWLOG_HDR_UNK_10C);
	put_unaligned_le32(20000, h + AVE_BOOT_FWLOG_HDR_UNK_110);
	h[AVE_BOOT_FWLOG_HDR_CONF + AVE_BOOT_FWLOG_SUBSYS_CMDPROC] = AVE_BOOT_FWLOG_LEVEL_DBG;
	h[AVE_BOOT_FWLOG_HDR_CONF + AVE_BOOT_FWLOG_SUBSYS_AVC]     = AVE_BOOT_FWLOG_LEVEL_DBG;
	h[AVE_BOOT_FWLOG_HDR_CONF + AVE_BOOT_FWLOG_SUBSYS_HEVC]    = AVE_BOOT_FWLOG_LEVEL_DBG;
	return 0;
}

/*
 * Hand the firmware its boot arguments, BEFORE AVE_IOP::Config / Start.
 *
 * The firmware's first act reads scratch 0 against the magic
 * (f13 0xa5f90, f26 0xe115c); anything else is its standalone branch, which
 * never speaks to the host.
 *
 * 13.5 (k13 f11d00..f1211c): SetIOPFlag(0); WriteScratch(1, instance);
 * WriteScratch(2, GetDevID()). No block, no Alloc before Start. The firmware
 * reads 1/2 as integers and only uses their low bytes, after the handshake,
 * for _AVE_FindByDevID (f13 0xa63c8..0xa63d0, 0xa9cb0) - a DevID outside
 * 1..28 is a NULL dereference there (0xa9cb4).
 *
 * 26.6.2 (k26 c1d690..c1dd10): Alloc(0x38) in FwIPC, MakeFwCfg,
 * SetIOPFlag(0), WriteScratch(1, lo32(IOVA)), WriteScratch(2, hi32(IOVA)).
 */
int ave_boot_config(struct ave_device *ave)
{
	const struct ave_boot_abi *a = ave->boot_abi;
	unsigned long flags;
	u32 s1, s2;
	int ret;

	if (!a || !ave->ipc.cpu) {
		dev_err(ave->dev, "boot config before ave_ipc_init()\n");
		return -EINVAL;
	}

	if (a->cfg_block) {
		struct ave_boot_fw_cfg *cfg;

		ret = ave_boot_fwlog_alloc(ave);
		if (ret)
			return ret;
		if (!ave->fwcfg.cpu) {
			ave->fwcfg.cpu = ave_ipc_alloc(ave, AVE_BOOT_FW_CFG_SIZE,
						       &ave->fwcfg.iova);
			if (!ave->fwcfg.cpu)
				return -ENOMEM;
			ave->fwcfg.size = AVE_BOOT_FW_CFG_SIZE;
		}
		cfg = ave->fwcfg.cpu;
		memset(cfg, 0, sizeof(*cfg));
		cfg->dev_index         = cpu_to_le32(a->instance);
		cfg->dev_id            = cpu_to_le32(a->dev_id);
		cfg->dev_num           = cpu_to_le32(a->dev_num);
		cfg->dev_num_per_group = cpu_to_le32(a->dev_num_per_group);
		cfg->dev_subid_flag    = cpu_to_le64(0);	/* UNVERIFIED */
		cfg->dev_revision      = cpu_to_le32(0);	/* UNVERIFIED */
		cfg->log_addr          = cpu_to_le64(ave->fwlog.iova);
		cfg->log_size          = cpu_to_le32(ave->fwlog.size);
		cfg->cfg30             = cpu_to_le32(0);	/* AVE_Cfg_Default */
		dma_wmb();
		s1 = lower_32_bits(ave->fwcfg.iova);
		s2 = upper_32_bits(ave->fwcfg.iova);
		dev_info(ave->dev, "  %s: boot cfg at iova %pad, FwLog %zu KiB at %pad\n",
			 a->name, &ave->fwcfg.iova, ave->fwlog.size >> 10,
			 &ave->fwlog.iova);
	} else {
		s1 = a->instance;
		s2 = a->dev_id;
		dev_info(ave->dev, "  %s: no boot cfg block; instance %u, DevID %u\n",
			 a->name, s1, s2);
	}

	spin_lock_irqsave(&ave->ipc_lock, flags);
	ave->mbox_pending = false;
	ave->ipc_up = false;
	ave->boot_phase = AVE_BOOT_ARGS_WRITTEN;
	spin_unlock_irqrestore(&ave->ipc_lock, flags);

	ave_step(ave, "next: pre-start scratch writes");
	/* Order is Apple's: flag first, then scratch 1, then scratch 2. */
	ave_set_scratch(ave, 0, AVE_BOOT_MAGIC);
	ave_set_scratch(ave, 1, s1);
	ave_set_scratch(ave, 2, s2);

	dev_info(ave->dev, "  scratch0=%#x scratch1=%#x scratch2=%#x\n",
		 ave_scratch(ave, 0), ave_scratch(ave, 1), ave_scratch(ave, 2));
	return 0;
}

/* ------------------------------------------------------------------------ */
/* The conversation                                                         */
/* ------------------------------------------------------------------------ */

static int ave_boot_check_msg1_log(struct ave_device *ave)
{
	const struct ave_boot_abi *a = ave->boot_abi;
	const u32 *m = ave->msg1;
	u32 r = ave_boot_check_msg1(a, m);

	dev_info(ave->dev, "  msg1: nch=%u chanmem=%#x ver=%#x heap=%#x   expect %u %#x %#x %#x (%s)\n",
		 m[0], m[1], m[2], m[3], a->msg1_nch, a->msg1_chsz,
		 AVE_BOOT_PROTO_VERSION, a->msg1_heap, a->name);

	if (r & AVE_MSG1_BAD_NCH)
		dev_err(ave->dev, "  msg1 REJECT: channel count %u outside 1..%u\n",
			m[0], a->max_channels);
	if (r & AVE_MSG1_BAD_CHSZ)
		dev_err(ave->dev, "  msg1 REJECT: channel block size %#x\n", m[1]);
	if (r & AVE_MSG1_BAD_VER)
		dev_err(ave->dev, "  msg1 REJECT: protocol version %#x != %#x\n",
			m[2], AVE_BOOT_PROTO_VERSION);
	if (r & AVE_MSG1_CHSZ_SHORT)
		dev_err(ave->dev, "  msg1 REJECT: %#x bytes cannot hold %u descriptors\n",
			m[1], m[0]);
	if (r & (AVE_MSG1_REJECT | AVE_MSG1_CHSZ_SHORT))
		return -EPROTO;

	if (r & (AVE_MSG1_UNEXP_NCH | AVE_MSG1_UNEXP_CHSZ)) {
		dev_err(ave->dev,
			"  msg1 MISMATCH: firmware offers %u channels / %#x bytes, %s ABI expects %u / %#x - the selected ABI (fw_abi) is probably wrong%s\n",
			m[0], m[1], a->name, a->msg1_nch, a->msg1_chsz,
			boot_lenient ? "; continuing (boot_lenient)" : "");
		if (!boot_lenient)
			return -EPROTO;
	}
	if (r & AVE_MSG1_UNEXP_HEAP)
		dev_warn(ave->dev, "  msg1: heap size %#x differs from the inferred %#x; using the firmware's\n",
			 m[3], a->msg1_heap);
	return 0;
}

/*
 * AVE_HwC::CreateFwHeap: round to the page size, then to 16 KiB
 * (k13 f10eec..f10f0c; k26 c1c7e8..c1c818); surface 23 (13.5) / 30 (26.6.2).
 * The firmware checks address and size are 16 KiB aligned (docs/34 §9).
 */
static int ave_boot_heap(struct ave_device *ave, u32 size)
{
	size_t sz;

	if (!size || ave->fwheap.cpu)
		return 0;
	sz = ALIGN(PAGE_ALIGN((size_t)size), AVE_BOOT_ALIGN_16K);
	ave->fwheap.cpu = dma_alloc_coherent(ave->dev, sz, &ave->fwheap.iova,
					     GFP_KERNEL);
	if (!ave->fwheap.cpu)
		return -ENOMEM;
	ave->fwheap.size = sz;
	if (ave->fwheap.iova & (AVE_BOOT_ALIGN_16K - 1)) {
		dev_err(ave->dev, "  fw heap iova %pad not 16 KiB aligned\n",
			&ave->fwheap.iova);
		return -EINVAL;
	}
	dev_info(ave->dev, "  fw heap (surface %u): %zu KiB at iova %pad\n",
		 ave->boot_abi->heap_surf_idx, sz >> 10, &ave->fwheap.iova);
	return 0;
}

/*
 * 26.6.2 only: t = AVE_IOP::GetCurrTime() (ASC +0x178000, 64-bit, docs/45
 * row 14); delta = host_abs_time() - t; scratch4 = lo, scratch5 = hi
 * (k26 c1e208..c1e5cc; firmware reads f26 0xe51c8/0xe51f4). Apple skips the
 * subtraction when t == 0 (k26 c1e210 cbz).
 *
 * INFERRED: host_abs_time (k26 0xfffffe0008cac7e4) is mach_absolute_time,
 * i.e. the CPU's counter; arch_timer_read_counter() is our equivalent. The
 * units and consequence of a wrong value are unknown (docs/34 §15).
 */
static void ave_boot_time_base(struct ave_device *ave)
{
	u64 t = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_TIMER);

	ave->time_base = t ? arch_timer_read_counter() - t : 0;
	ave_set_scratch(ave, 4, lower_32_bits(ave->time_base));
	ave_set_scratch(ave, 5, upper_32_bits(ave->time_base));
	dev_info(ave->dev, "  time base: asc %#llx, delta %#llx\n", t, ave->time_base);
}

/* Bind every descriptor (AVE_IPC::CreateChannel k13 f24764, k26 c440a0). */
static int ave_boot_bind(struct ave_device *ave, u32 nch)
{
	const struct ave_boot_abi *a = ave->boot_abi;
	const size_t chan_end = ave->chanmem_size;
	unsigned long flags;
	u32 i;

	BUILD_BUG_ON(ARRAY_SIZE(ave->chan) != AVE_CH_MAX);

	for (i = 0; i < nch; i++) {
		const u8 *d = (u8 *)ave->chanmem + (size_t)i * AVE_BOOT_DESC_STRIDE;
		const struct ave_boot_chan *want;
		struct ave_boot_desc desc;
		struct ave_channel *ch;
		struct ave_ring r;
		size_t bytes;
		u8 *slots;
		int id;

		if ((size_t)(i + 1) * AVE_BOOT_DESC_STRIDE > chan_end)
			return -EPROTO;
		ave_boot_desc_parse(d, &desc);
		id = ave_boot_name_to_id(a, desc.name);
		want = id >= 0 ? ave_boot_chan_expect(a, id) : NULL;

		dev_info(ave->dev, "  desc[%u] '%s' id=%d dir=%u bit=%u nslots=%u slots_fw=%#llx (+%#llx)\n",
			 i, desc.name, id, desc.dir, desc.bit, desc.nslots,
			 desc.slots_fw,
			 desc.slots_fw - ave_ipc_cpu_to_fw(ave, ave->chanmem));

		/* An unknown name aborts CreateChannel (k26 c44200 -> c44814). */
		if (id < 0) {
			dev_err(ave->dev, "  desc[%u]: unknown channel name '%s'\n",
				i, desc.name);
			return -EPROTO;
		}
		if (!want)
			dev_err(ave->dev, "  desc[%u] '%s': not in the %s channel table\n",
				i, desc.name, a->name);
		else if (want->dir != desc.dir || want->bit != desc.bit ||
			 want->nslots != desc.nslots ||
			 desc.slots_fw != ave_ipc_cpu_to_fw(ave, ave->chanmem) + want->slots_off)
			dev_err(ave->dev, "  desc[%u] '%s' MISMATCH: expected dir=%u bit=%u nslots=%u +%#x; using the firmware's values as Apple does\n",
				i, desc.name, want->dir, want->bit, want->nslots,
				want->slots_off);

		/* Our own bounds, before the host writes any slot. */
		bytes = (size_t)desc.nslots * AVE_BOOT_SLOT_SIZE;
		slots = desc.nslots ? ave_ipc_fw_to_cpu(ave, desc.slots_fw, bytes) : NULL;
		if (!slots || desc.bit >= 32 ||
		    slots < (u8 *)ave->chanmem ||
		    slots + bytes > (u8 *)ave->chanmem + chan_end) {
			dev_err(ave->dev, "  desc[%u] '%s': slot array outside the channel block\n",
				i, desc.name);
			return -EPROTO;
		}

		ch = &ave->chan[id];
		if (ch->bound)
			dev_warn(ave->dev, "  desc[%u]: channel id %d bound twice\n", i, id);

		spin_lock_irqsave(&ave->ipc_lock, flags);
		ave_ring_init(&r, slots, desc.nslots, ave_ring_dir_to_type(desc.dir));
		ch->id           = id;
		ch->desc_index   = i;
		ch->dir          = desc.dir;
		ch->doorbell_bit = desc.bit;
		ch->nslots       = desc.nslots;
		ch->type         = r.type;
		ch->ring         = slots;
		ch->ring_fw      = desc.slots_fw;
		ch->rd           = r.rd;
		ch->wr           = r.wr;
		ch->nrecv        = 0;
		ch->nsend        = 0;
		ch->bound        = true;
		spin_unlock_irqrestore(&ave->ipc_lock, flags);
	}
	ave->nchannels = nch;
	return 0;
}

int ave_ipc_handshake(struct ave_device *ave)
{
	const struct ave_boot_abi *a = ave->boot_abi;
	u32 m[4], nch, chsz, heap, cbuf;
	u64 desc_fw, info_fw;
	unsigned long flags;
	void *desc_cpu;
	u32 val;
	int ret;

	if (!a || !ave->ipc_pool) {
		dev_err(ave->dev, "handshake before ave_ipc_init()\n");
		return -EINVAL;
	}
	if (ave->boot_phase == AVE_BOOT_READY)
		return 0;
	if (ave->boot_phase != AVE_BOOT_ARGS_WRITTEN &&
	    ave->boot_phase != AVE_BOOT_MSG1) {
		dev_err(ave->dev, "handshake without boot arguments (phase %u)\n",
			ave->boot_phase);
		return -EINVAL;
	}

	/* --- message 1 ------------------------------------------------------ */
	if (ave->boot_phase == AVE_BOOT_ARGS_WRITTEN) {
		ret = ave_recv_iop_msg(ave, m, AVE_BOOT_RECV_TIMEOUT_MS);
		if (ret) {
			dev_err(ave->dev, "  no message 1 (%d)\n", ret);
			goto fail;
		}
	}
	ret = ave_boot_check_msg1_log(ave);
	if (ret)
		goto fail;
	nch  = ave->msg1[0];
	chsz = ave->msg1[1];
	heap = ave->msg1[3];

	if (a->heap_before_msg2) {
		ret = ave_boot_heap(ave, heap);
		if (ret)
			goto fail;
	}
	if (a->time_base)
		ave_boot_time_base(ave);

	/* --- message 2: the FwIPC surface ----------------------------------- */
	ave_send_iop_msg(ave, lower_32_bits(ave->ipc.iova),
			 upper_32_bits(ave->ipc.iova), ave->ipc.size, 0);
	dev_info(ave->dev, "  msg2: FwIPC iova %pad size %#zx\n",
		 &ave->ipc.iova, ave->ipc.size);

	/* --- message 3: the firmware's view of it --------------------------- */
	ret = ave_recv_iop_msg(ave, m, AVE_BOOT_RECV_TIMEOUT_MS);
	if (ret) {
		dev_err(ave->dev, "  no message 3 (%d)\n", ret);
		goto fail;
	}
	ave->ipc_fw_base = (u64)m[1] << 32 | m[0];
	dev_info(ave->dev, "  msg3: fw_base %#llx (FwIPC iova %pad)\n",
		 ave->ipc_fw_base, &ave->ipc.iova);

	if (!a->heap_before_msg2) {
		ret = ave_boot_heap(ave, heap);
		if (ret)
			goto fail;
	}

	/* --- message 4: channel block and info block ------------------------ */
	ave->chanmem = ave_ipc_alloc(ave, chsz, NULL);	/* zeroed, 64-aligned */
	ave->ipcinfo = ave_ipc_alloc(ave, AVE_BOOT_INFO_SIZE, NULL);
	if (!ave->chanmem || !ave->ipcinfo) {
		ret = -ENOMEM;
		goto fail;
	}
	ave->chanmem_size = chsz;

	put_unaligned_le64(ave_ipc_cpu_to_fw(ave, ave->chanmem),
			   (u8 *)ave->ipcinfo + AVE_BOOT_INFO_CHANMEM);
	if (a->info_log_block) {
		ave->info_log = ave_ipc_alloc(ave, a->info_log_size, NULL);
		if (!ave->info_log) {
			ret = -ENOMEM;
			goto fail;
		}
		put_unaligned_le64(ave_ipc_cpu_to_fw(ave, ave->info_log),
				   (u8 *)ave->ipcinfo + AVE_BOOT_INFO_LOG_ADDR);
		put_unaligned_le32(a->info_log_size,
				   (u8 *)ave->ipcinfo + AVE_BOOT_INFO_LOG_SIZE);
	}
	if (ave->fwheap.cpu) {		/* only if message 1 asked (k13 f13428) */
		put_unaligned_le64(ave->fwheap.iova,
				   (u8 *)ave->ipcinfo + AVE_BOOT_INFO_HEAP_ADDR);
		put_unaligned_le32(ave->fwheap.size,
				   (u8 *)ave->ipcinfo + AVE_BOOT_INFO_HEAP_SIZE);
	}
	put_unaligned_le32(a->dev_type, (u8 *)ave->ipcinfo + AVE_BOOT_INFO_DEV_TYPE);
	/* +0x4c stays 0: the firmware copies that many trailing words. */
	dma_wmb();

	info_fw = ave_ipc_cpu_to_fw(ave, ave->ipcinfo);
	ave_send_iop_msg(ave, lower_32_bits(info_fw), upper_32_bits(info_fw), 0, 0);
	dev_info(ave->dev, "  msg4: info fw %#llx, chanmem fw %#llx (%#x bytes), dev_type %u\n",
		 info_fw, ave_ipc_cpu_to_fw(ave, ave->chanmem), chsz, a->dev_type);

	/* --- message 5: where the descriptors went -------------------------- */
	ret = ave_recv_iop_msg(ave, m, AVE_BOOT_RECV_TIMEOUT_MS);
	if (ret) {
		dev_err(ave->dev, "  no message 5 (%d)\n", ret);
		goto fail;
	}
	desc_fw = (u64)m[1] << 32 | m[0];
	cbuf = m[2];
	dev_info(ave->dev, "  msg5: desc fw %#llx, client buffer %#x (max %#x, expect %#x)\n",
		 desc_fw, cbuf, a->client_buf_max, a->client_buf_expect);

	/* Fw2KernelAddr(desc) must be the block we allocated (k13 f13bf0, k26 c1fa1c). */
	desc_cpu = ave_ipc_fw_to_cpu(ave, desc_fw, (size_t)nch * AVE_BOOT_DESC_STRIDE);
	if (desc_cpu != ave->chanmem) {
		dev_err(ave->dev, "  msg5 REJECT: descriptors at cpu %p, channel block at %p\n",
			desc_cpu, ave->chanmem);
		ret = -EPROTO;
		goto fail;
	}

	ret = ave_boot_bind(ave, nch);
	if (ret)
		goto fail;

	/* Client buffer size (k13 f13ec8 after CreateChannel; k26 c1fcac). */
	if (cbuf > a->client_buf_max) {
		dev_err(ave->dev, "  msg5 REJECT: client buffer %#x > %#x\n",
			cbuf, a->client_buf_max);
		ret = -EPROTO;
		goto fail;
	}
	if (a->client_buf_expect && cbuf != a->client_buf_expect)
		dev_warn(ave->dev, "  msg5: client buffer %#x, firmware literal is %#x\n",
			 cbuf, a->client_buf_expect);
	ave->client_buf_size = cbuf;

	/* --- ready flag ----------------------------------------------------- */
	/*
	 * Channels are bound, and once the firmware clears the flag bit 0 is a
	 * doorbell (13.5: TERMINAL), not the mailbox. Flip the IRQ handler over
	 * BEFORE writing the flag, or a doorbell racing the poll below is
	 * captured as a mailbox message nobody consumes (review 2026-09-13).
	 */
	spin_lock_irqsave(&ave->ipc_lock, flags);
	ave->ipc_up = true;
	ave->mbox_pending = false;
	spin_unlock_irqrestore(&ave->ipc_lock, flags);

	ave_set_scratch(ave, 3, AVE_BOOT_MAGIC);
	ret = readl_relaxed_poll_timeout(ave->bank[AVE_BANK_SVE].base + AVE_SVE_SCRATCH(3),
					 val, val == 0, AVE_BOOT_READY_DELAY_US,
					 (u64)AVE_BOOT_READY_POLLS * AVE_BOOT_READY_DELAY_US);
	if (ret) {
		dev_err(ave->dev, "  ready flag not cleared (scratch3 %#x)\n", val);
		goto fail;
	}

	spin_lock_irqsave(&ave->ipc_lock, flags);
	ave->boot_phase = AVE_BOOT_READY;
	spin_unlock_irqrestore(&ave->ipc_lock, flags);

	dev_info(ave->dev, "  %s handshake complete: %u channel(s), heartbeat scratch7 %#x\n",
		 a->name, ave->nchannels, ave_scratch(ave, 7));
	return 0;

fail:
	spin_lock_irqsave(&ave->ipc_lock, flags);
	ave->ipc_up = false;
	ave->boot_phase = AVE_BOOT_FAILED;
	spin_unlock_irqrestore(&ave->ipc_lock, flags);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                              */
/* ------------------------------------------------------------------------ */

/*
 * 26.6.2: drain the FwLog ring (NUL-terminated strings back to back, free
 * running counters, docs/33 §2). 13.5 has no such ring; its log arrives over
 * TERMINAL and only once the handshake is complete (docs/45 §2.3).
 */
void ave_fw_log_dump(struct ave_device *ave)
{
	const struct ave_boot_abi *a = ave->boot_abi;
	u32 rd, wr, ring;
	char line[256];
	unsigned int n = 0;
	u8 *h;

	if (!a)
		return;

	if (!a->fwlog_ring) {
		if (ave->ipc_up && ave_chan(ave, AVE_CH_TERMINAL)) {
			unsigned long before = ave->fwlog_lines;

			ave_ipc_drain(ave, AVE_CH_TERMINAL);
			dev_info(ave->dev, "  fw log (%s, TERMINAL): %lu new line(s), %lu total\n",
				 a->name, ave->fwlog_lines - before, ave->fwlog_lines);
		} else {
			dev_info(ave->dev, "  fw log: %s has no FwLog ring; its log comes over TERMINAL after the handshake\n",
				 a->name);
		}
		return;
	}

	h = ave->fwlog.cpu;
	if (!h)
		return;
	ring = get_unaligned_le32(h + AVE_BOOT_FWLOG_HDR_RING_SIZE);
	rd   = get_unaligned_le32(h + AVE_BOOT_FWLOG_HDR_RD);
	wr   = get_unaligned_le32(h + AVE_BOOT_FWLOG_HDR_WR);

	dev_info(ave->dev, "  fw log: rd=%u wr=%u ring=%#x\n", rd, wr, ring);
	if (!ring || ring > AVE_BOOT_FWLOG_RING_SIZE || rd == wr)
		return;

	while (rd != wr && n < 64) {
		unsigned int i = 0;
		char c;

		do {
			c = h[AVE_BOOT_FWLOG_RING_OFF + (rd++ % ring)];
			if (i < sizeof(line) - 1)
				line[i++] = c;
		} while (c && rd != wr);
		line[i] = 0;
		if (line[0])
			dev_info(ave->dev, "  fw| %s\n", line);
		n++;
	}
	put_unaligned_le32(rd, h + AVE_BOOT_FWLOG_HDR_RD);
}

/*
 * Firmware globals, read out of OUR image buffer (ave->fw.cpu), not out of
 * the running core. Only meaningful when the core executes that buffer.
 *
 * 26.6.2 offsets: docs/33 §3 and docs/40. On 13.5 the equivalents are
 * _gui64SoCRegPhysAd 0x21a7b0 and _gRtkDevControlVbase 0x21a7b8 (f13
 * 0xa6508/0xa65e8), crashlog pointer 0xeefd0 / size 0xeeff8 (0xada74), and
 * there is no gs_psCfg (docs/45 rows 35-38). On this machine the 13.5 image
 * that runs is iBoot's (docs/43, docs/44), and those globals are written at
 * run time in memory we do not hold - so nothing is read on 13.5.
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

	if (ave->fw_abi != AVE_ABI_MACOS_26_6) {
		dev_info(ave->dev, "  firmware globals: skipped for %s - the running image is iBoot's, not our buffer (13.5 VAs 0x21a7b0/0x21a7b8/0xeefd0 in ave_ipc.c)\n",
			 ave_fw_abi_name(ave->fw_abi));
		return;
	}

	dev_info(ave->dev, "  firmware globals (26.6.2 offsets, read out of OUR image buffer, not the running core):\n");
	for (i = 0; i < ARRAY_SIZE(g); i++) {
		if (g[i].off + 8 > ave->fw.size)
			continue;
		dev_info(ave->dev, "    %#08x = %#018llx  %s\n", g[i].off,
			 get_unaligned_le64((u8 *)ave->fw.cpu + g[i].off),
			 g[i].name);
	}
}

/* ------------------------------------------------------------------------ */
/* Setup and teardown                                                       */
/* ------------------------------------------------------------------------ */

int ave_ipc_init(struct ave_device *ave)
{
	const struct ave_boot_abi *a = ave_boot_abi_get(ave->fw_abi);
	int ret;

	if (!a) {
		dev_err(ave->dev, "no boot/IPC ABI for %s\n",
			ave_fw_abi_name(ave->fw_abi));
		return -EOPNOTSUPP;
	}
	if (ave->ipc.cpu)
		return 0;

	/* The lock exists before boot_abi is published: the IRQ handler keys on it. */
	spin_lock_init(&ave->ipc_lock);
	ratelimit_state_init(&ave->fwlog_rs, 5 * HZ, 100);
	ave->ipc_irq = ave_ipc_irq_dispatch;

	/*
	 * FwIPC: 0x700000 on 13.5 (k13 f22ad4), 0x1400000 on 26.6.2
	 * (k26 c426ec). The firmware requires IOVA and size 16 KiB aligned.
	 * Coherent, because neither side does cache maintenance on the rings
	 * (docs/36 §6, docs/45 row 72).
	 */
	ave->ipc.size = a->fwipc_size;
	ave->ipc.cpu = dma_alloc_coherent(ave->dev, ave->ipc.size,
					  &ave->ipc.iova, GFP_KERNEL);
	if (!ave->ipc.cpu)
		return -ENOMEM;
	if ((ave->ipc.iova | ave->ipc.size) & (AVE_BOOT_ALIGN_16K - 1)) {
		dev_err(ave->dev, "FwIPC iova %pad / size %#zx not 16 KiB aligned\n",
			&ave->ipc.iova, ave->ipc.size);
		ret = -EINVAL;
		goto err_free;
	}

	ave->ipc_pool = gen_pool_create(ilog2(AVE_BOOT_CHANMEM_ALIGN), -1);
	if (!ave->ipc_pool) {
		ret = -ENOMEM;
		goto err_free;
	}
	ret = gen_pool_add_virt(ave->ipc_pool, (unsigned long)ave->ipc.cpu,
				ave->ipc.iova, ave->ipc.size, -1);
	if (ret)
		goto err_pool;

	WRITE_ONCE(ave->boot_abi, a);
	dev_info(ave->dev, "IPC (%s): FwIPC surface %u, %zu KiB at iova %pad\n",
		 a->name, a->fwipc_surf_idx, ave->ipc.size >> 10, &ave->ipc.iova);
	return 0;

err_pool:
	gen_pool_destroy(ave->ipc_pool);
	ave->ipc_pool = NULL;
err_free:
	dma_free_coherent(ave->dev, ave->ipc.size, ave->ipc.cpu, ave->ipc.iova);
	ave->ipc.cpu = NULL;
	return ret;
}

void ave_ipc_fini(struct ave_device *ave)
{
	unsigned long flags;
	unsigned int i;

	if (ave->boot_abi) {
		spin_lock_irqsave(&ave->ipc_lock, flags);
		ave->ipc_up = false;
		ave->boot_phase = AVE_BOOT_IDLE;
		ave->mbox_pending = false;
		for (i = 0; i < ARRAY_SIZE(ave->chan); i++)
			ave->chan[i].bound = false;
		ave->nchannels = 0;
		spin_unlock_irqrestore(&ave->ipc_lock, flags);
	}

	if (ave->fwheap.cpu) {
		dma_free_coherent(ave->dev, ave->fwheap.size, ave->fwheap.cpu,
				  ave->fwheap.iova);
		ave->fwheap.cpu = NULL;
	}
	if (ave->fwlog.cpu) {
		dma_free_coherent(ave->dev, ave->fwlog.size, ave->fwlog.cpu,
				  ave->fwlog.iova);
		ave->fwlog.cpu = NULL;
	}

	if (!ave->ipc.cpu)
		return;

	if (ave->ipc_pool) {
		for (i = 0; i < AVE_SHMALLOC_MAX; i++) {
			ave_ipc_free(ave, ave->shm[i].cpu, ave->shm[i].size);
			ave->shm[i].cpu = NULL;
		}
		ave_ipc_free(ave, ave->fwcfg.cpu, ave->fwcfg.size);
		ave_ipc_free(ave, ave->chanmem, ave->chanmem_size);
		ave_ipc_free(ave, ave->ipcinfo, AVE_BOOT_INFO_SIZE);
		if (ave->boot_abi)
			ave_ipc_free(ave, ave->info_log, ave->boot_abi->info_log_size);
		ave->fwcfg.cpu = ave->chanmem = ave->ipcinfo = ave->info_log = NULL;

		/* gen_pool_destroy() BUGs on outstanding allocations: leak instead. */
		if (gen_pool_avail(ave->ipc_pool) == gen_pool_size(ave->ipc_pool))
			gen_pool_destroy(ave->ipc_pool);
		else
			dev_warn(ave->dev, "FwIPC pool still has %zu bytes allocated; leaking it\n",
				 gen_pool_size(ave->ipc_pool) - gen_pool_avail(ave->ipc_pool));
		ave->ipc_pool = NULL;
	}

	dma_free_coherent(ave->dev, ave->ipc.size, ave->ipc.cpu, ave->ipc.iova);
	ave->ipc.cpu = NULL;
}
