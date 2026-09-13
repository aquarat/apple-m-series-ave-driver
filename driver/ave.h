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

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/types.h>

#include "ave_hw.h"
#include "ave_abi.h"
#include "ave_version.h"

/* One mapped MMIO bank; index matches the ADT reg index. */
struct ave_bank {
	phys_addr_t		phys;	/* AP-physical start, for the IOBA patch */
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
 * One IPC channel, indexed by host channel id (enum ave_boot_ch_id in
 * ave_abi_boot.h). The descriptor comes from the firmware; rd/wr are
 * host-private (the firmware never reads them - the only shared state is each
 * slot's phase bit). Layout mirrors Apple's 0x30-byte handle, docs/36 §6.
 */
struct ave_channel {
	bool		bound;
	u32		id;
	u32		desc_index;	/* position in the firmware's table */
	u32		dir;		/* descriptor +0x40 */
	u32		doorbell_bit;	/* descriptor +0x44 */
	u32		nslots;		/* descriptor +0x48 */
	u32		type;		/* host ring type, from dir */
	s32		rd;		/* -1 = nothing outstanding */
	s32		wr;		/* -1 = full */
	u32		nrecv;
	u32		nsend;
	void		*ring;		/* into ipc.cpu */
	u64		ring_fw;	/* descriptor +0x4c */
};

struct ave_device {
	struct device		*dev;
	struct ave_bank		bank[AVE_NUM_BANKS];
	enum ave_fw_abi		fw_abi;		/* chosen before any command */

	/* --- boot/IPC state (owner: ave_ipc.c work) --- */
	/*
	 * Per-version boot/IPC descriptor (ave_abi_boot.h), set by
	 * ave_ipc_init(). NULL until then; the IRQ handler uses it to know the
	 * lock below has been initialised.
	 */
	const struct ave_boot_abi *boot_abi;
	spinlock_t		ipc_lock;	/* mailbox capture, rings, pool */
	struct gen_pool		*ipc_pool;	/* 64-byte granule over FwIPC */
	u8			boot_phase;	/* enum in ave_ipc.c */
	bool			ipc_up;		/* ready flag cleared: rings live */
	/* Mailbox messages the IRQ handler took before RecvIOPMsg could. */
	bool			mbox_pending;
	u32			mbox_status;
	u32			mbox[4];
	u32			msg1[4];	/* message 1 as received */
	u64			ipc_fw_base;	/* message 3: fw view of FwIPC */
	struct ave_dma_buf	fwheap;		/* message 1 scratch 3 */
	void			*chanmem;	/* channel block, in FwIPC */
	u32			chanmem_size;
	void			*ipcinfo;	/* 0x50-byte info block, in FwIPC */
	void			*info_log;	/* 26.6.2 +0x10 block, in FwIPC */
	u32			client_buf_size; /* message 5 scratch 2 */
	u64			time_base;	/* 26.6.2 scratch 4/5 */
	/* SHAREDMALLOC allocations (gen_pool needs the size back). */
#define AVE_SHMALLOC_MAX	64
	struct { void *cpu; u32 size; } shm[AVE_SHMALLOC_MAX];
	/*
	 * Channel dispatch after the handshake, called from the IRQ handler.
	 * A pointer so ave_drv.c needs no ave_abi_boot.h include; see ave_ipc.c.
	 */
	void			(*ipc_irq)(struct ave_device *ave, u32 status);
	/* Command-layer hook for IO / IO_T2H payloads (CPU address or NULL). */
	void			(*ipc_rx)(struct ave_device *ave, u32 chan_id,
					  void *buf, u32 size, u32 flags);
	struct ratelimit_state	fwlog_rs;
	unsigned long		fwlog_lines;
	/* --- end boot/IPC --- */

	/* --- DAPF / fetch-path state (owner: ave_fw.c / ave_dapf.c work) --- */
	/* Optional "cpudart"/"dapf" reg entries: devm_ioremap, NOT requested. */
	void __iomem		*cpudart;
	void __iomem		*dapf;
	phys_addr_t		cpudart_phys;
	phys_addr_t		dapf_phys;
	bool			dapf_programmed;
	/* iBoot segment DART mappings (ave_fw.c, fw_map_data / fw_map_text) */
	struct iommu_domain	*iboot_domain;
	bool			iboot_data_mapped;
	bool			iboot_text_mapped;
	/*
	 * Pristine copy of the firmware's DATA segment (ave_fw.c,
	 * fw_restore_data). vmalloc'd AVE_IBOOT_DATA_SIZE buffer, loaded once
	 * from the committed blob and memcpy'd back over physical DATA before
	 * each core start, mirroring macOS. NULL until first use; freed on
	 * unload.
	 */
	u8			*iboot_data_pristine;
	/* --- end DAPF --- */

	/*
	 * Boot-handshake capture. The IRQ is requested before the firmware
	 * has said anything, and the handler must write-1-clear the whole
	 * status word or the line stays asserted. That would consume the
	 * firmware's very first message and we would record a working boot as
	 * silence - so during the handshake the handler stashes it here
	 * instead of dropping it, and ave_recv_iop_msg() accepts either
	 * source.
	 */
#define AVE_SNAP_PAGES	4096			/* 16 MiB at 4 KiB pages */
	u32			snap[AVE_SNAP_PAGES];	/* CRC of iBoot's image */
	bool			snap_valid;
	bool			powered;   /* holds a runtime-PM ref from stage 6 */
	bool			irq_enabled;	/* enabled only while powered */
	bool			hs_seen;
	u32			hs_status;
	u32			hs_scratch[4];
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
		u64		map_iova;	/* where the core fetches */
	} fw;

	struct ave_dma_buf	ipc;		/* FwIPC: 7 MiB (13.5) / 20 MiB (26.6.2) */
	struct ave_dma_buf	fwcfg;		/* 26.6.2 only: 56-byte boot block */
	struct ave_dma_buf	fwlog;		/* 26.6.2 only: FwLog surface      */
	struct ave_channel	chan[8];	/* by host id; AVE_CH_MAX */
	unsigned int		nchannels;	/* descriptors bound */

	bool			running;
};

/*
 * Crash-surviving progress markers. There is no pstore backend on this
 * machine, so a hard crash leaves nothing but what userspace already synced
 * to disk. With step_ms=N every marker is logged and then held for N ms, long
 * enough for an fsync-per-line /dev/kmsg reader to persist it: the last
 * marker on disk names the operation that killed the machine.
 */
extern int ave_step_ms;
#define ave_step(ave, fmt, ...)						\
	do {								\
		dev_info((ave)->dev, "STEP " fmt "\n", ##__VA_ARGS__);	\
		if (ave_step_ms)					\
			msleep(ave_step_ms);				\
	} while (0)

static inline u32 ave_read(struct ave_device *ave, unsigned int bank, u32 off)
{
	return readl_relaxed(ave->bank[bank].base + off);
}

static inline void ave_write(struct ave_device *ave, unsigned int bank,
			     u32 off, u32 val)
{
	writel_relaxed(val, ave->bank[bank].base + off);
}

static inline u64 ave_read64(struct ave_device *ave, unsigned int bank, u32 off)
{
	return readq_relaxed(ave->bank[bank].base + off);
}

static inline void ave_write64(struct ave_device *ave, unsigned int bank,
			       u32 off, u64 val)
{
	writeq_relaxed(val, ave->bank[bank].base + off);
}

/* ave_fw.c */
int ave_fw_load(struct ave_device *ave);
void ave_fw_unload(struct ave_device *ave);
int ave_fw_map_text_mode(void);
int ave_fw_restore_data(struct ave_device *ave);

/* ave_ipc.c */
int ave_boot_config(struct ave_device *ave);
int ave_recv_iop_msg(struct ave_device *ave, u32 out[4], unsigned int timeout_ms);
void ave_fw_log_dump(struct ave_device *ave);
void ave_fw_globals_dump(struct ave_device *ave);
void ave_fw_peek_phys(struct ave_device *ave);
void ave_fw_snapshot_phys(struct ave_device *ave);
void ave_fw_diff_phys(struct ave_device *ave);
void ave_fw_identify_phys(struct ave_device *ave);
int ave_ipc_init(struct ave_device *ave);
void ave_ipc_fini(struct ave_device *ave);
int ave_ipc_handshake(struct ave_device *ave);
int ave_ipc_send(struct ave_device *ave, u32 chan_id, dma_addr_t payload,
		 u64 arg1, u64 arg2);
int ave_ipc_recv(struct ave_device *ave, u32 chan_id, u64 *out, size_t n);
void ave_ipc_ring_doorbell(struct ave_device *ave, u32 chan_id);

#endif /* __AVE_H__ */
