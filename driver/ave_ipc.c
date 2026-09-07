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
