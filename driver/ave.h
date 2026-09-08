/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE (Video Encoder) - driver private definitions
 *
 * UNTESTED. No part of this has run on hardware. The register sequences and
 * constants come from ave_hw.h / ave_abi.h, which are transcribed from
 * AppleAVE2.kext and the AVE firmware; the surrounding logic is new code and
 * has never been executed.
 */
#ifndef __AVE_H__
#define __AVE_H__

#include <linux/device.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/types.h>

#include "ave_hw.h"
#include "ave_abi.h"

/* One mapped MMIO bank; index matches the ADT reg index. */
struct ave_bank {
	void __iomem	*base;
	resource_size_t	size;
};

/* A DMA-coherent region shared with the coprocessor. */
struct ave_dma_buf {
	void		*cpu;
	dma_addr_t	iova;
	size_t		size;
};

/*
 * One IPC channel. The descriptor comes from the firmware; head and tail are
 * host-private (the firmware never reads them - the only shared state is each
 * slot's phase bit).
 */
struct ave_channel {
	u32		id;
	u32		doorbell_bit;
	u32		nslots;
	u32		head;
	u32		tail;
	u8		phase;
	void		*ring;		/* into ipc.cpu */
	dma_addr_t	ring_iova;
};

struct ave_device {
	struct device		*dev;
	struct ave_bank		bank[AVE_NUM_BANKS];
	int			irq;
	struct reset_control	*rst;

	/* Power domains, in ADT power-gates order. */
	struct dev_pm_domain_list *pd_list;

	/*
	 * Firmware. We load it ourselves rather than adopting an iBoot
	 * pre-load, so that no bootloader patch is needed. It must end up at
	 * DART IOVA 0; see ave_fw.c.
	 */
	struct {
		void		*cpu;
		dma_addr_t	iova;		/* where dma_alloc put it   */
		size_t		size;
		bool		mapped_at_zero;
		u64		fw_base;	/* reported back by the fw  */
	} fw;

	struct ave_dma_buf	ipc;		/* the 20 MiB FwIPC region */
	struct ave_channel	chan[AVE_IPC_MAX_CHANNELS];
	unsigned int		nchannels;

	bool			running;
};

static inline u32 ave_read(struct ave_device *ave, unsigned int bank, u32 off)
{
	return readl_relaxed(ave->bank[bank].base + off);
}

static inline void ave_write(struct ave_device *ave, unsigned int bank,
			     u32 off, u32 val)
{
	writel_relaxed(val, ave->bank[bank].base + off);
}

/* ave_fw.c */
int ave_fw_load(struct ave_device *ave);
void ave_fw_unload(struct ave_device *ave);

/* ave_ipc.c */
int ave_ipc_init(struct ave_device *ave);
void ave_ipc_fini(struct ave_device *ave);
int ave_ipc_handshake(struct ave_device *ave);
int ave_ipc_send(struct ave_device *ave, u32 chan_id, dma_addr_t payload,
		 u64 arg1, u64 arg2);
int ave_ipc_recv(struct ave_device *ave, u32 chan_id, u64 *out, size_t n);
void ave_ipc_ring_doorbell(struct ave_device *ave, u32 chan_id);

#endif /* __AVE_H__ */
