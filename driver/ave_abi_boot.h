/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - boot handshake and IPC transport, per firmware ABI.
 *
 * UNTESTED ON HARDWARE. Static analysis only.
 *
 * Spec: docs/45-abi-13.5-boot-ipc.md (both columns), docs/34 (26.6.2
 * handshake), docs/36 (26.6.2 IPC implementation), docs/08 (ring transport),
 * docs/33 (26.6.2 log ring).
 *
 * VA conventions, as in docs/45:
 *   13.5 kext   "k13 f11d08"  = 0xfffffe0008f11d08 in data/blobs/macos-13.5/kc.macho
 *   13.5 fw     "f13 0xa5c50" = image VA in data/blobs/macos-13.5/ave_h13c.bin
 *   26.6.2 kext "k26 c1d68c"  = 0xfffffe0008c1d68c in data/blobs/kc.macho
 *   26.6.2 fw   "f26 0xe0e70" = image VA in data/blobs/ave_h13c.bin
 * Re-check any of them with
 *   AVE_MACOS=13.5 python3 tools/disas.py --kext|--fw --addr VA -n 0x40
 *
 * This header is deliberately free of kernel-only dependencies outside the
 * __KERNEL__ blocks, so tools/ipc_selftest can compile the ring and message-1
 * logic against a fake shared-memory buffer.
 *
 * Supersedes these former ave_abi.h definitions (now removed):
 * AVE_IPC_SURFACE_SIZE, AVE_IPC_MAX_CHANNELS, AVE_IPC_CHAN_*, AVE_IPC_SLOT_SIZE,
 * AVE_IPC_DESC_*, struct ave_ipc_slot, AVE_IPC_GRANULE, struct ave_fw_cfg,
 * AVE_FW_CFG_SIZE, AVE_IOP_FLAG_HOST_MODE, AVE_SURF_IDX_FWLOG, AVE_FWLOG_*,
 * AVE_LOG_*; and in ave_hw.h the use of AVE_DEVID_T6000 (a 26.6.2-only value).
 */
#ifndef __AVE_ABI_BOOT_H__
#define __AVE_ABI_BOOT_H__

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <asm/barrier.h>
#define AVE_RING_WMB()	dma_wmb()
#define AVE_RING_RMB()	dma_rmb()
#else
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
#define AVE_RING_WMB()	do { } while (0)
#define AVE_RING_RMB()	do { } while (0)
#endif

#include "ave_version.h"

/* ------------------------------------------------------------------------ */
/* Constants that are the same on both versions                             */
/* ------------------------------------------------------------------------ */

/*
 * Host-mode magic. Scratch 0 before Start (k13 f11d08 SetIOPFlag(0),
 * k26 c1da64); firmware standalone test f13 0xa5f90..0xa5fa0,
 * f26 0xe115c. Also the ready flag in scratch 3 (k13 f13ed8, k26 c1fcbc)
 * and SetIOPFlag's literal (k13 f411a0/a4, k26 c91258).
 */
#define AVE_BOOT_MAGIC			0x08042006u

/* Message 1 scratch 2: literal in firmware (f13 0xa9368, f26 0xe4e50). */
#define AVE_BOOT_PROTO_VERSION		0x100u

/*
 * Doorbell / status bit 0 is the scratch mailbox: SendIOPMsg -> SetIntr(1)
 * (k13 f413b8, k26 c914f8); RecvIOPMsg polls status bit 0 and W1Cs it
 * before reading (k13 f41534/f41614, k26 c91664/c91754).
 */
#define AVE_BOOT_MBOX_BIT		0

/* RecvIOPMsg: 2000 x IODelay(1000) x cfg[0x18]=1 (k13 f41558/f4163c, k26 c91720). */
#define AVE_BOOT_RECV_TIMEOUT_MS	2000
/* Ready flag: 20000 x IODelay(100) (k13 f13f08/f13fcc, k26 c1fcec/c1fd9c). */
#define AVE_BOOT_READY_POLLS		20000
#define AVE_BOOT_READY_DELAY_US		100

/* FwIPC IOVA and size must be 16 KiB aligned (f13 0xa93a8..0xa93f4, f26 0xe4f94/0xe518c). */
#define AVE_BOOT_ALIGN_16K		0x4000u

/* IPC info block: Alloc(0x50) (k13 f13104, k26 c1f00c), zeroed (k13 f133fc). */
#define AVE_BOOT_INFO_SIZE		0x50
#define AVE_BOOT_INFO_CHANMEM		0x08	/* u64 fw addr   k13 f13420, k26 c1f3d0 */
#define AVE_BOOT_INFO_LOG_ADDR		0x10	/* u64, 26.6.2   k26 c1f408 */
#define AVE_BOOT_INFO_LOG_SIZE		0x18	/* u32, 26.6.2   k26 c1f410 */
#define AVE_BOOT_INFO_HEAP_ADDR		0x1c	/* u64 unaligned k13 f13438, k26 c1f3e8 */
#define AVE_BOOT_INFO_HEAP_SIZE		0x24	/* u32           k13 f13444, k26 c1f3f4 */
#define AVE_BOOT_INFO_DEV_TYPE		0x28	/* u32           k13 f13450, k26 c1f41c */
#define AVE_BOOT_INFO_EXTRA_COUNT	0x4c	/* u32 must be 0 f13 0xa9908, f26 0xe58e8 */

/*
 * Channel descriptor, stride 0x100; name strncpy 0x40, +0x40 dir,
 * +0x44 doorbell bit, +0x48 slot count, +0x4c u64 slot array fw address
 * (f13 0xa5c78..0xa5c9c; k13 CreateChannel f248a0 ldur x1,[x24,#76],
 * stride f249f0; f26 0xe0ea8..0xe0ee0; k26 c441e0, c44350).
 */
#define AVE_BOOT_DESC_STRIDE		0x100
#define AVE_BOOT_DESC_NAME_LEN		0x40
#define AVE_BOOT_DESC_DIR		0x40
#define AVE_BOOT_DESC_BIT		0x44
#define AVE_BOOT_DESC_NSLOTS		0x48
#define AVE_BOOT_DESC_SLOTS		0x4c

/* Channel block base is aligned up to 64 by the firmware (f13 0xa5c64/68, f26 0xe0e88). */
#define AVE_BOOT_CHANMEM_ALIGN		64

/* Ring slot, stride 0x40; {payload|phase, arg1, arg2} (k13 f44ffc/f4512c, k26 cb4324). */
#define AVE_BOOT_SLOT_SIZE		0x40

/*
 * Host channel ids = index into the host name table and the argument of
 * AVE_IPC::Send/Recv. 13.5 table gs_piaAVE_IPC_ChMap k13 0xfffffe0007bc3c20
 * (bound cmp x21,#8 at f22530); 26.6.2 has only entries 0..2
 * (k26 0xfffffe0007ee0b38, bound c420d8). Ids 1 and 2 agree.
 */
enum ave_boot_ch_id {
	AVE_CH_NONE		= 0,	/* ""             */
	AVE_CH_IO		= 1,	/* host -> fw commands, fw acks back  */
	AVE_CH_IO_T2H		= 2,	/* fw -> host, host echoes the slot   */
	AVE_CH_SHAREDMALLOC	= 3,	/* 13.5: fw asks host for FwIPC memory */
	AVE_CH_TERMINAL		= 4,	/* 13.5: firmware CLogger output      */
	AVE_CH_DEBUG		= 5,	/* 13.5: host drains, -1002           */
	AVE_CH_BUF_H2T		= 6,	/* 13.5: host drains, -1002           */
	AVE_CH_BUF_T2H		= 7,	/* 13.5: host drains, -1002           */
	AVE_CH_MAX		= 8,
};

/* ------------------------------------------------------------------------ */
/* 26.6.2 only: the pre-start boot config block and its FwLog ring          */
/* ------------------------------------------------------------------------ */

/*
 * _S_AVE_Fw_Cfg, 56 bytes. Host writer AVE_HwC::MakeFwCfg k26 c1cbf0; fw
 * reader f26 0xe1218..0xe1368 (maps exactly 0x38 at 0xe11f8). Absent on 13.5:
 * no MakeFwCfg and no Alloc before Start (docs/45 row 28, k13 f119e0..f1422c).
 */
struct ave_boot_fw_cfg {
	u32	dev_index;		/* +0x00 k26 c1cc18 */
	u32	dev_id;			/* +0x04 k26 c1cc24 */
	u32	dev_num;		/* +0x08 k26 c1cc30 */
	u32	dev_num_per_group;	/* +0x0c k26 c1cc3c */
	u64	dev_subid_flag;		/* +0x10 k26 c1cc48 */
	u32	dev_revision;		/* +0x18 k26 c1cc54 */
	u32	pad_1c;
	u64	log_addr;		/* +0x20 k26 c1cc68 FwLog DART address */
	u32	log_size;		/* +0x28 k26 c1cc6c */
	u32	pad_2c;
	u32	cfg30;			/* +0x30 k26 c1cc5c, default 0 */
	u32	pad_34;
} __attribute__((packed));
#define AVE_BOOT_FW_CFG_SIZE		0x38

/*
 * FwLog ring (26.6.2, docs/33 §2): header 0x1DC, ring at +0x200, counters at
 * +0x154 (host rd) / +0x198 (fw wr). No FwLog surface on 13.5 (docs/45 row 71).
 */
#define AVE_BOOT_FWLOG_HDR_RING_OFF	0x04
#define AVE_BOOT_FWLOG_HDR_RING_SIZE	0x08
#define AVE_BOOT_FWLOG_HDR_CONF		0x0c
#define AVE_BOOT_FWLOG_HDR_UNK_10C	0x10c	/* Apple writes 25    */
#define AVE_BOOT_FWLOG_HDR_UNK_110	0x110	/* Apple writes 20000 */
#define AVE_BOOT_FWLOG_HDR_RD		0x154
#define AVE_BOOT_FWLOG_HDR_WR		0x198
#define AVE_BOOT_FWLOG_RING_OFF		0x200
#define AVE_BOOT_FWLOG_RING_SIZE	0x20000	/* size is ours; docs/33 */
#define AVE_BOOT_FWLOG_SIZE		(AVE_BOOT_FWLOG_RING_OFF + AVE_BOOT_FWLOG_RING_SIZE)
#define AVE_BOOT_FWLOG_LEVEL_DBG	8
#define AVE_BOOT_FWLOG_SUBSYS_CMDPROC	130
#define AVE_BOOT_FWLOG_SUBSYS_AVC	140
#define AVE_BOOT_FWLOG_SUBSYS_HEVC	145

/* ------------------------------------------------------------------------ */
/* Per-version descriptor                                                   */
/* ------------------------------------------------------------------------ */

/* One descriptor the firmware writes into our channel block. */
struct ave_boot_chan {
	const char	*name;
	u8		id;		/* enum ave_boot_ch_id                */
	u8		dir;		/* desc +0x40: 0 -> host type 1, ...  */
	u8		bit;		/* desc +0x44                          */
	u16		nslots;		/* desc +0x48                          */
	u32		slots_off;	/* desc +0x4c minus the aligned base   */
};

struct ave_boot_abi {
	enum ave_fw_abi	abi;
	const char	*name;

	/* Pre-start scratch 1/2. */
	bool		cfg_block;	/* 26.6.2: IOVA of _S_AVE_Fw_Cfg; 13.5: integers */
	u32		instance;	/* ave0 */
	/* dev_id / dev_type / chip_type are per SoC: ave->soc->dev[abi] */
	u32		dev_num;	/* 26.6.2 cfg +0x08 */
	u32		dev_num_per_group;

	/* FwIPC surface. */
	u32		fwipc_size;
	u8		fwipc_surf_idx;
	u8		heap_surf_idx;

	/* Message 1: expected values and the host's own acceptance rule. */
	u32		msg1_nch;
	u32		msg1_chsz;
	u32		msg1_heap;	/* inferred on both, read from scratch 3 */
	u32		max_channels;	/* host accepts 1..max_channels */

	/* Differences in the conversation. */
	bool		time_base;	/* scratch 4/5 before message 2 */
	bool		heap_before_msg2;
	bool		info_log_block;	/* ipcinfo +0x10/+0x18 */
	u32		info_log_size;
	bool		fwlog_ring;	/* FwLog surface in the cfg block */
	bool		terminal_log;	/* firmware logs over TERMINAL */

	/* Message 5. */
	u32		client_buf_max;
	u32		client_buf_expect;	/* 0 = not established */

	u32		name_table_size;	/* host ChName2ID accepts ids < this */
	const struct ave_boot_chan *chans;
	unsigned int	nchans;
};

static inline const struct ave_boot_abi *ave_boot_abi_get(enum ave_fw_abi abi)
{
	/*
	 * Descriptor order below is firmware order (ChannelTableCreate), which
	 * is what the channel block will contain.
	 */
	static const struct ave_boot_chan chans_13_5[] = {
		/* ChannelTableCreate(void*) f13 0xa5c50..0xa5dfc; names 0xce359.. */
		{ "TERMINAL",     AVE_CH_TERMINAL,     2, 0, 512, 0x0700 }, /* 0xa5cb4 dir, 0xa5c7c bit, 0xa5c9c n, 0xa5c98 */
		{ "IO",           AVE_CH_IO,           0, 1,  32, 0x8700 }, /* 0xa5ce4, 0xa5cb8, 0xa5ce8, 0xa5cec */
		{ "DEBUG",        AVE_CH_DEBUG,        0, 1,   8, 0x8f00 }, /* 0xa5d1c, 0xa5cf0, 0xa5d20, 0xa5d24 */
		{ "BUF_H2T",      AVE_CH_BUF_H2T,      0, 2,   1, 0x9100 }, /* 0xa5d58, 0xa5d28, 0xa5d5c, 0xa5d40 */
		{ "BUF_T2H",      AVE_CH_BUF_T2H,      1, 3,   1, 0x9140 }, /* 0xa5d88, 0xa5d60, 0xa5d8c, 0xa5d90 */
		{ "SHAREDMALLOC", AVE_CH_SHAREDMALLOC, 1, 3,   8, 0x9180 }, /* 0xa5dbc, 0xa5d94, 0xa5dc0, 0xa5dc4 */
		{ "IO_T2H",       AVE_CH_IO_T2H,       1, 3,  32, 0x9380 }, /* 0xa5ddc, 0xa5dc8, 0xa5de0, 0xa5dec */
	};
	static const struct ave_boot_chan chans_26_6[] = {
		/* ChannelTableCreate f26 0xe0e70..0xe0f04 (docs/36 §4) */
		{ "IO",           AVE_CH_IO,           0, 1, 992, 0x0200 }, /* 0xe0edc, 0xe0eb0, 0xe0ec4, 0xe0ee0 */
		{ "IO_T2H",       AVE_CH_IO_T2H,       1, 3, 993, 0xfa00 }, /* 0xe0efc, (bit: docs/36 §4), 0xe0ef4, 0xe0f04 */
	};
	static const struct ave_boot_abi abi_13_5 = {
		.abi			= AVE_ABI_MACOS_13_5,
		.name			= "macOS 13.5",
		.cfg_block		= false,	/* k13 f12004 / f1211c write integers */
		.instance		= 0,		/* HwC+0x40 (k13 f11ffc); 0 INFERRED: t6000 maxNum = 1 (k13 0xfffffe0007bc2a38 entry 13) */
		/* DevID/DevType/ChipType: ave_soc.c (k13 table 0xfffffe0007bc2a38, fw 0xee4d0; ipcinfo +0x28 k13 f13450) */
		.dev_num		= 1,		/* same entry +0x0c (unused on 13.5) */
		.dev_num_per_group	= 1,		/* same entry +0x10 (unused on 13.5) */
		.fwipc_size		= 0x700000,	/* k13 f22ad4 */
		.fwipc_surf_idx		= 24,		/* k13 f22ac0 */
		.heap_surf_idx		= 23,		/* k13 CreateFwHeap f10f10 */
		.msg1_nch		= 7,		/* f13 ChannelTableSizeGet 0xa5bd8 */
		.msg1_chsz		= 0x9bc0,	/* f13 0xa5bdc */
		.msg1_heap		= 0xc0000,	/* f13 0x9ea44..0x9ea50 max(global, 0xC0000), INFERRED value */
		.max_channels		= 8,		/* k13 f126dc..f126e4: nch-1 < 8 */
		.time_base		= false,	/* no WriteScratch(4/5) in k13 StartUpIOP; fw reads 0..2 only */
		.heap_before_msg2	= false,	/* k13 f128b0, reached after message 3 */
		.info_log_block		= false,	/* k13 fill f13414..f13450 has no +0x10/+0x18 */
		.info_log_size		= 0,
		.fwlog_ring		= false,	/* no AVE_FwLog / AVE_Log_* (docs/45 rows 71, 94) */
		.terminal_log		= true,		/* f13 0xa636c..0xa63a0; k13 ProcessIntr_IPCCh f0d790 */
		.client_buf_max		= 0x100000,	/* k13 f13ec8 cmp w8,#0x100,lsl#12; b.hi */
		.client_buf_expect	= 0xb4000,	/* f13 GetClientBufferSize 0x15f2c */
		.name_table_size	= 8,		/* k13 f22530 */
		.chans			= chans_13_5,
		.nchans			= sizeof(chans_13_5) / sizeof(chans_13_5[0]),
	};
	static const struct ave_boot_abi abi_26_6 = {
		.abi			= AVE_ABI_MACOS_26_6,
		.name			= "macOS 26.6.2",
		.cfg_block		= true,		/* k26 c1d690 Alloc(0x38), c1dc7c/c1dd10 */
		.instance		= 0,		/* HwC+0x48 k26 c1cc18; 0 INFERRED (docs/34 §4 fn) */
		/* DevID/DevType/ChipType: ave_soc.c (docs/45 row 3; MakeFwCfg k26 c1cc24, ipcinfo +0x28 k26 c1f41c) */
		.dev_num		= 1,		/* GetDevNum k26 c1cc30, "1 / 2" docs/34 §4 */
		.dev_num_per_group	= 1,		/* GetDevNumPerGroup k26 c1cc3c: VALUE UNVERIFIED */
		.fwipc_size		= 0x1400000,	/* k26 c426ec mov w4,#0x1400000 */
		.fwipc_surf_idx		= 31,		/* docs/08 §2 / docs/00 canary */
		.heap_surf_idx		= 30,		/* CreateFwHeap k26 c1c7f0 */
		.msg1_nch		= 2,		/* f26 ChannelTableSizeGet 0xe0da4 */
		.msg1_chsz		= 0x1f280,	/* f26 0xe0d84 */
		.msg1_heap		= 0xc0000,	/* f26 0xd9d18..0xd9d24, INFERRED value */
		.max_channels		= 3,		/* k26 c1e1f0: nch-1 <= 2 */
		.time_base		= true,		/* k26 c1e208..c1e5cc */
		.heap_before_msg2	= true,		/* k26 c1e784, before SendIOPMsg c1eb88 */
		.info_log_block		= true,		/* k26 c1f1f0 Alloc(0x10000) */
		.info_log_size		= 0x10000,	/* k26 c1f410 */
		.fwlog_ring		= true,		/* cfg +0x20/+0x28, k26 c1cc68 */
		.terminal_log		= false,	/* TERMINAL flag-gated dead, f26 0xe0f08 */
		.client_buf_max		= 0x13c000,	/* k26 c1fcac */
		.client_buf_expect	= 0,		/* not established on 26.6.2 */
		.name_table_size	= 3,		/* k26 c420d8 */
		.chans			= chans_26_6,
		.nchans			= sizeof(chans_26_6) / sizeof(chans_26_6[0]),
	};

	switch (abi) {
	case AVE_ABI_MACOS_13_5:	return &abi_13_5;
	case AVE_ABI_MACOS_26_6:	return &abi_26_6;
	default:			return NULL;
	}
}

/* ------------------------------------------------------------------------ */
/* Message 1 validation                                                     */
/* ------------------------------------------------------------------------ */

/* Apple's host rejects these (so do we). */
#define AVE_MSG1_BAD_NCH	(1u << 0)	/* outside 1..max_channels */
#define AVE_MSG1_BAD_CHSZ	(1u << 1)	/* zero (k13 f126e8 cbz; k26 c1e1fc > 0) */
#define AVE_MSG1_BAD_VER	(1u << 2)	/* != 0x100 (k13 f12710; k26 c1e6d0) */
#define AVE_MSG1_REJECT		0xffu
/* Apple accepts these, but they say our ABI model is wrong. */
#define AVE_MSG1_UNEXP_NCH	(1u << 8)
#define AVE_MSG1_UNEXP_CHSZ	(1u << 9)
#define AVE_MSG1_UNEXP_HEAP	(1u << 10)
/* Our own safety: the block must at least hold the descriptors. */
#define AVE_MSG1_CHSZ_SHORT	(1u << 11)

static inline u32 ave_boot_check_msg1(const struct ave_boot_abi *a, const u32 m[4])
{
	u32 r = 0;

	if (m[0] < 1 || m[0] > a->max_channels)
		r |= AVE_MSG1_BAD_NCH;
	if (m[1] == 0 || (a->abi == AVE_ABI_MACOS_26_6 && (s32)m[1] <= 0))
		r |= AVE_MSG1_BAD_CHSZ;
	if (m[2] != AVE_BOOT_PROTO_VERSION)
		r |= AVE_MSG1_BAD_VER;
	if (m[0] != a->msg1_nch)
		r |= AVE_MSG1_UNEXP_NCH;
	if (m[1] != a->msg1_chsz)
		r |= AVE_MSG1_UNEXP_CHSZ;
	if (m[3] != a->msg1_heap)
		r |= AVE_MSG1_UNEXP_HEAP;
	if (!(r & AVE_MSG1_BAD_NCH) &&
	    (u64)m[1] < (u64)m[0] * AVE_BOOT_DESC_STRIDE)
		r |= AVE_MSG1_CHSZ_SHORT;
	return r;
}

/* ------------------------------------------------------------------------ */
/* Channel descriptors                                                      */
/* ------------------------------------------------------------------------ */

static inline u32 ave_boot_get_le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u64 ave_boot_get_le64(const u8 *p)
{
	return (u64)ave_boot_get_le32(p) | (u64)ave_boot_get_le32(p + 4) << 32;
}

static inline void ave_boot_put_le32(u8 *p, u32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static inline void ave_boot_put_le64(u8 *p, u64 v)
{
	ave_boot_put_le32(p, (u32)v);
	ave_boot_put_le32(p + 4, (u32)(v >> 32));
}

struct ave_boot_desc {
	char	name[AVE_BOOT_DESC_NAME_LEN + 1];
	u32	dir;
	u32	bit;
	u32	nslots;
	u64	slots_fw;
};

static inline void ave_boot_desc_parse(const u8 *d, struct ave_boot_desc *o)
{
	memcpy(o->name, d, AVE_BOOT_DESC_NAME_LEN);
	o->name[AVE_BOOT_DESC_NAME_LEN] = 0;
	o->dir      = ave_boot_get_le32(d + AVE_BOOT_DESC_DIR);
	o->bit      = ave_boot_get_le32(d + AVE_BOOT_DESC_BIT);
	o->nslots   = ave_boot_get_le32(d + AVE_BOOT_DESC_NSLOTS);
	o->slots_fw = ave_boot_get_le64(d + AVE_BOOT_DESC_SLOTS);
}

/*
 * AVE_IPC_ChName2ID: strcmp against the host name table; a miss aborts
 * CreateChannel (k26 c44200 -> c44814). Returns the id or -1.
 */
static inline int ave_boot_name_to_id(const struct ave_boot_abi *a, const char *name)
{
	static const char * const names[AVE_CH_MAX] = {
		"", "IO", "IO_T2H", "SHAREDMALLOC", "TERMINAL", "DEBUG",
		"BUF_H2T", "BUF_T2H",
	};
	unsigned int i;

	for (i = 0; i < a->name_table_size && i < AVE_CH_MAX; i++)
		if (!strcmp(name, names[i]))
			return (int)i;
	return -1;
}

static inline const struct ave_boot_chan *
ave_boot_chan_expect(const struct ave_boot_abi *a, int id)
{
	unsigned int i;

	for (i = 0; i < a->nchans; i++)
		if (a->chans[i].id == id)
			return &a->chans[i];
	return (const struct ave_boot_chan *)0;
}

/* ------------------------------------------------------------------------ */
/* The ring (IOProcessorChannel, same algorithm on both versions)           */
/* ------------------------------------------------------------------------ */

/*
 * dir -> host ring type: 0 -> 1, 1 -> 0, >= 2 -> 2 (k13 e961ec..e96200,
 * k26 b4f7f4..b4f804). Type bit 1 routes Receive to the unidirectional
 * variant (k13 AppleAVEIOProcessorChannel::Receive e962ac tbnz w8,#1).
 */
static inline u32 ave_ring_dir_to_type(u32 dir)
{
	return dir == 0 ? 1 : dir == 1 ? 0 : 2;
}

/* Host-private handle (Create64 k13 f44d2c): nothing here is shared. */
struct ave_ring {
	u8	*slots;
	u32	nslots;
	u32	type;
	s32	rd;		/* -1 = nothing outstanding */
	s32	wr;		/* -1 = full */
	u32	nrecv;
	u32	nsend;
};

/*
 * Create64 (k13 f44d2c..f44de4): odd type writes slot[0] = type and zeroes
 * slot[8]/slot[16] for every slot, rd = -1, wr = 0; even type touches no
 * slot, rd = 0, wr = -1.
 */
static inline void ave_ring_init(struct ave_ring *r, u8 *slots, u32 nslots, u32 type)
{
	u32 i;

	r->slots = slots;
	r->nslots = nslots;
	r->type = type;
	r->nrecv = r->nsend = 0;
	if (type & 1) {
		for (i = 0; i < nslots; i++) {
			u8 *s = slots + (u64)i * AVE_BOOT_SLOT_SIZE;

			ave_boot_put_le64(s + 8, 0);
			ave_boot_put_le64(s + 16, 0);
			ave_boot_put_le64(s, type);
		}
		r->rd = -1;
		r->wr = 0;
	} else {
		r->rd = 0;
		r->wr = -1;
	}
	AVE_RING_WMB();
}

/* Send64 (k13 f44fa4; slot writes f45004..f45020, dsb st, index f45054..). */
static inline int ave_ring_send(struct ave_ring *r, u64 payload, u32 arg1, u32 arg2)
{
	s32 rd, w;
	u8 *s;

	if (r->wr < 0)
		return -1;
	s = r->slots + (u64)r->wr * AVE_BOOT_SLOT_SIZE;
	ave_boot_put_le64(s + 8, arg1);
	ave_boot_put_le64(s + 16, arg2);
	ave_boot_put_le64(s, payload | ((u64)r->type ^ 1));
	AVE_RING_WMB();

	rd = r->rd;
	w = r->wr;
	if (rd < 0) {
		r->rd = w;
		rd = w;
	}
	w = (w == (s32)r->nslots - 1) ? 0 : w + 1;
	r->wr = (rd == w) ? -1 : w;
	r->nsend++;
	return 0;
}

/* Receive64 (k13 f450d0; phase f4514c..f45180): never writes the slot. */
static inline int ave_ring_recv(struct ave_ring *r, u64 *payload, u32 *arg1, u32 *arg2)
{
	s32 rd, w;
	u64 w0;
	u8 *s;

	if (r->rd < 0)
		return -1;
	s = r->slots + (u64)r->rd * AVE_BOOT_SLOT_SIZE;
	w0 = ave_boot_get_le64(s);
	if ((w0 & 1) != (r->type & 1))
		return -1;
	AVE_RING_RMB();
	*payload = w0 & ~3ULL;
	*arg1 = (u32)ave_boot_get_le64(s + 8);
	*arg2 = (u32)ave_boot_get_le64(s + 16);

	rd = r->rd;
	w = r->wr;
	if (w < 0) {
		r->wr = rd;
		w = rd;
	}
	rd = (rd == (s32)r->nslots - 1) ? 0 : rd + 1;
	r->rd = (w == rd) ? -1 : rd;
	r->nrecv++;
	return 0;
}

/*
 * UnidirectionalSend64 (k13 f45208; fw f13 0x92c6c): send only into a slot
 * whose phase shows the receiver has consumed it; wr just advances.
 */
static inline int ave_ring_usend(struct ave_ring *r, u64 payload, u32 arg1, u32 arg2)
{
	u8 *s = r->slots + (u64)(r->wr < 0 ? 0 : r->wr) * AVE_BOOT_SLOT_SIZE;

	if ((ave_boot_get_le64(s) & 1) != (r->type & 1))
		return -1;
	ave_boot_put_le64(s + 8, arg1);
	ave_boot_put_le64(s + 16, arg2);
	ave_boot_put_le64(s, payload | ((u64)r->type ^ 1));
	AVE_RING_WMB();
	r->wr = (r->wr == (s32)r->nslots - 1) ? 0 : r->wr + 1;
	r->nsend++;
	return 0;
}

/*
 * UnidirectionalReceive64 (k13 f4535c..f45454, read for this driver; docs/45
 * row 89 had it unread): no rd == -1 test; phase test as Receive64; then
 * WRITES slot[0] = type ^ 1 (f45408..f45410) to hand the slot back, and
 * advances rd with wrap. No doorbell.
 */
static inline int ave_ring_urecv(struct ave_ring *r, u64 *payload, u32 *arg1, u32 *arg2)
{
	u8 *s = r->slots + (u64)(r->rd < 0 ? 0 : r->rd) * AVE_BOOT_SLOT_SIZE;
	u64 w0 = ave_boot_get_le64(s);

	if ((w0 & 1) != (r->type & 1))
		return -1;
	AVE_RING_RMB();
	*payload = w0 & ~3ULL;
	*arg1 = (u32)ave_boot_get_le64(s + 8);
	*arg2 = (u32)ave_boot_get_le64(s + 16);
	ave_boot_put_le64(s, (u64)r->type ^ 1);
	AVE_RING_WMB();
	r->rd = (r->rd == (s32)r->nslots - 1) ? 0 : r->rd + 1;
	r->nrecv++;
	return 0;
}

/*
 * Firmware ring type for a descriptor, used only by the self-test to play the
 * firmware side: RealChannelCreate is called with bool 0 (f13 0xa9aa0), so
 * TypeAdjust (0xa5e00) returns dir unchanged, and CChannelManager::Init maps
 * 0 -> 0, 1 -> 1, 2 -> 3 (f13 0xa03b0..0xa03d0).
 */
static inline u32 ave_ring_fw_type_13_5(u32 dir)
{
	return dir == 2 ? 3 : dir == 1 ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
/* Kernel-side API implemented in ave_ipc.c beyond the ave.h prototypes     */
/* ------------------------------------------------------------------------ */
#ifdef __KERNEL__
struct ave_device;

void *ave_ipc_alloc(struct ave_device *ave, size_t size, dma_addr_t *iova);
void ave_ipc_free(struct ave_device *ave, void *cpu, size_t size);
void *ave_ipc_fw_to_cpu(struct ave_device *ave, u64 fw, size_t len);
u64 ave_ipc_cpu_to_fw(struct ave_device *ave, void *cpu);
#endif

#endif /* __AVE_ABI_BOOT_H__ */
