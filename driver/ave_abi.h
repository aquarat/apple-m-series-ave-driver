/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE (Video Encoder) - host <-> firmware ABI
 *
 * Every constant here was read out of AppleAVE2.kext or the AVE firmware and
 * re-verified against the binary. Anything not verified is marked UNVERIFIED.
 * See docs/00-methodology.md before adding to this file.
 */
#ifndef __AVE_ABI_H__
#define __AVE_ABI_H__

#include <linux/compiler_attributes.h>
#include <linux/types.h>

/*
 * Wire command ids.
 *
 * Read from the firmware's own dispatcher: CFlowControllerBase::CmdProcessor
 * (firmware VA 0x28134) loads a u16 from offset 0 of the command struct
 * (ldrh w8,[x19] at 0x28244), rejects id-1 > 0xb, and branches through a
 * 12-entry jump table at 0x28c6c. Each table target size-checks the struct and
 * calls the handler, so ids and sizes were recovered together.
 *
 * This is the firmware's opinion, not an inference about the host - the
 * strongest evidence available.
 */
enum ave_cmd {
	AVE_CMD_CONFIG		= 1,
	AVE_CMD_HALT		= 2,
	AVE_CMD_OPEN		= 3,
	AVE_CMD_CLOSE		= 4,
	/* 5 is unused: its jump-table entry targets the default block */
	AVE_CMD_START		= 6,	/* codec from hdr.enc_type */
	AVE_CMD_STOP		= 7,
	AVE_CMD_PROCESS		= 8,	/* engine from hdr.work_type */
	AVE_CMD_COMPLETE	= 9,
	AVE_CMD_PRIORITY	= 10,
	AVE_CMD_FLUSH		= 11,
	AVE_CMD_RESET		= 12,
};

/*
 * Command struct sizes, enforced by the firmware - it rejects any other length.
 * Verified from the size checks in the jump-table targets at 0x28278..0x283d8.
 */
#define AVE_CMDSZ_COMMON	0x48	/* Open, Close, Stop, Complete,
					   Priority, Flush, Halt */
#define AVE_CMDSZ_CONFIG	0x78
#define AVE_CMDSZ_RESET		0x13f08	/* 81672 - contents unexplained */
#define AVE_CMDSZ_AVC_START	0x3180
#define AVE_CMDSZ_HEVC_START	0x13f28
#define AVE_CMDSZ_AVC_PROCESS	0x63d8
#define AVE_CMDSZ_HEVC_PROCESS	0xb1c0

/*
 * Common command header (first 0x40 bytes of every command).
 *
 * Field roles are fixed by the firmware's own log format string,
 * "CMD %d %d | %d 0x%x | CID %llu CNT %llu", not by inference.
 * Offsets 0x02, 0x04, 0x28 and 0x2c are written but their meaning is unknown.
 */
struct ave_cmd_hdr {
	__le16	id;		/* +0x00  enum ave_cmd                     */
	__le16	unk_02;		/* +0x02  unknown                          */
	__le32	unk_04;		/* +0x04  unknown                          */
	__le64	count;		/* +0x08  "CNT" - sequence number          */
	__le64	client_id;	/* +0x10  "CID"                            */
	__le32	work_type;	/* +0x18  enum ave_work_type               */
	__le32	enc_type;	/* +0x1c  enum ave_enc_type                */
	__le32	slot;		/* +0x20  in-flight slot index, < 51       */
	__le32	priority;	/* +0x24  client priority - NOT command
				 *        specific. The firmware passes it to
				 *        CAVEPriorityQueue::SetClientPriority
				 *        from Start and Process as well as
				 *        Priority (ldr w2,[x21,#36] then
				 *        bl 0x401c8 at firmware 0x29840).   */
	__le32	unk_28;		/* +0x28  unknown                          */
	__le32	unk_2c;		/* +0x2c  unknown                          */
	__u8	timeout[16];	/* +0x30  _S_AVE_TimeOut, copied verbatim  */
} __packed;

#define AVE_MAX_INFLIGHT	51	/* hdr.slot is bounds-checked < 51 */

/* CAVEPriorityQueue::RegisterClient caps concurrent clients (cmp w8,#0x80). */
#define AVE_MAX_CLIENTS		128

/* hdr.enc_type */
enum ave_enc_type {
	AVE_ENC_AVC	= 1,
	AVE_ENC_HEVC	= 2,
};

/*
 * hdr.work_type - selects the engine for AVE_CMD_PROCESS.
 * Same enum as _S_AVE_CHM+0x34. Note this is NOT _E_AVE_ClientType.
 */
enum ave_work_type {
	AVE_WORK_ENC	= 1,
	AVE_WORK_LRME	= 2,
	AVE_WORK_MCTF	= 3,
	AVE_WORK_MSC	= 4,
	AVE_WORK_GGM	= 5,
	AVE_WORK_DMV	= 6,
};
/*
 * Read from the AVE_MD_SVE::CalcSurfaceInfo dispatch at
 * 0xfffffe0008c8c83c..0xc8c894: a clean switch on 1..6 in which 3 branches to
 * AVE_Work_MCTF_CalcSurfaceInfo and 6 to AVE_Work_DMV_CalcSurfaceInfo, with 0
 * and anything > 6 falling to the error arm. Note Enc is 1, not 0 - an earlier
 * reading of docs/11 ("else Enc") left this ambiguous and it was briefly
 * recorded as 0 here.
 */

/*
 * Firmware pipeline states, reported back to the host.
 * From the contiguous __cstring run at firmware VA 0x122a87, used by
 * SendState() and logged as "remoteSystemState[%d] = %s".
 */
enum ave_pipeline_state {
	AVE_STATE_INVALID		= 0,
	AVE_STATE_READY			= 1,
	AVE_STATE_ENQUEUE		= 2,
	AVE_STATE_LRME_FS_START		= 3,
	AVE_STATE_LRME_FS_DONE		= 4,
	AVE_STATE_LRME_RC_START		= 5,
	AVE_STATE_LRME_RC_DONE		= 6,
	AVE_STATE_PIPE_RESET_READY	= 7,
	AVE_STATE_PIPE_RESET_DONE	= 8,
	AVE_STATE_PIPE_START		= 9,
	AVE_STATE_PIPE_DONE		= 10,
	AVE_STATE_XC_START		= 11,
	AVE_STATE_XC_DONE		= 12,
	AVE_STATE_CMD_ACK		= 13,
	AVE_STATE_CMD_READY		= 14,
};

/*
 * IPC transport.
 *
 * AVE_IPC wraps a generic Apple IOProcessorChannel library that is statically
 * linked into BOTH the kext (0xfffffe0008cb4124) and the firmware (0xcd90c),
 * so the ring semantics can be read from either side.
 *
 * Shared memory is one DART-mapped surface named "FwIPC". Head and tail live in
 * the host's private handle, NOT in shared memory - the only shared
 * synchronisation is the per-slot phase bit.
 */
#define AVE_IPC_SURFACE_SIZE	0x1400000	/* 20 MiB */
#define AVE_IPC_MAX_CHANNELS	3
#define AVE_IPC_CHAN_H2T	1		/* "IO"     host -> firmware */
#define AVE_IPC_CHAN_T2H	2		/* "IO_T2H" firmware -> host */

#define AVE_IPC_SLOT_SIZE	0x40		/* only 24 bytes used */
#define AVE_IPC_DESC_STRIDE	0x100		/* firmware-supplied descriptors */

/* Channel descriptor fields (firmware-supplied, AVE_IPC_DESC_STRIDE apart). */
#define AVE_IPC_DESC_NAME	0x00
#define AVE_IPC_DESC_DIR	0x40
#define AVE_IPC_DESC_DOORBELL	0x44	/* bit number for AVE_SVE_DOORBELL */
#define AVE_IPC_DESC_NSLOTS	0x48
#define AVE_IPC_DESC_FWADDR	0x4c

/* One ring slot. Phase bit 0 of addr is compared against (type & 1). */
struct ave_ipc_slot {
	__le64	payload_fw_addr;	/* low bit is the phase bit */
	__le64	arg1;
	__le64	arg2;
	__u8	pad[AVE_IPC_SLOT_SIZE - 24];
} __packed;

/*
 * The FwIPC region is carved by a buddy allocator (AVE_ChkPool) with a 64-byte
 * granule and 64-byte alignment - both defaulted from a 0 argument in
 * AVE_IPC::Init (0xfffffe0008c42850) and AVE_IPC::Alloc (0xfffffe0008c43010).
 */
#define AVE_IPC_GRANULE		64

/*
 * Boot argument block, _S_AVE_Fw_Cfg - 56 bytes.
 *
 * The host allocates this, fills it, DART-maps it, and hands the firmware its
 * address in scratch registers 1 and 2 with scratch 0 set to
 * AVE_IOP_FLAG_HOST_MODE, all BEFORE starting the core.
 *
 * The firmware's very first act (CPlatformEnvironment ctor, fw 0xe0fec) is to
 * read scratch 0 and compare it against 0x08042006. If it does not match it
 * takes a standalone branch and never talks to the host at all - which is
 * exactly what we saw when starting the core with the scratch registers zero.
 *
 * Field offsets confirmed from both sides: the host writer AVE_HwC::MakeFwCfg
 * (0xfffffe0008c1cbf0) and the firmware reader at fw 0xe1218..0xe1344.
 */
struct ave_fw_cfg {
	__le32	dev_index;	/* +0x00 instance, 0 for ave0            */
	__le32	dev_id;		/* +0x04 AVE_DevInfo::GetDevID()         */
	__le32	dev_num;	/* +0x08                                  */
	__le32	dev_num_per_group; /* +0x0c                              */
	__le64	dev_subid_flag;	/* +0x10                                  */
	__le32	dev_revision;	/* +0x18                                  */
	__le32	pad_1c;		/* +0x1c not written by Apple            */
	__le64	log_addr;	/* +0x20 DART addr of the log surface    */
	__le32	log_size;	/* +0x28 its size                        */
	__le32	pad_2c;		/* +0x2c not written                     */
	__le32	cfg30;		/* +0x30 AVE_Cfg_Get()[+0x30], default 0 */
	__le32	pad_34;		/* +0x34 not written                     */
} __packed;

#define AVE_FW_CFG_SIZE		0x38		/* firmware maps exactly 56 */

/* scratch0 value that selects host mode; anything else means standalone */
#define AVE_IOP_FLAG_HOST_MODE	0x08042006

/* Surface config index 29 is the firmware log surface (AVE_FwLog). */
#define AVE_SURF_IDX_FWLOG	29

/*
 * Firmware log ring.
 *
 * The sink is a host-allocated shared buffer whose DART address goes in the
 * boot config at +0x20. It needs no IPC channel, no doorbell and no interrupt:
 * the firmware appends NUL-terminated strings into a byte ring and both sides
 * track free-running counters. So this is readable even when nothing else
 * works - including crash output, since RTK_platform_crashlog_complete prints
 * the exception and call stack through AVE_Log_Output at subsystem 2.
 *
 * Header is 0x1DC bytes, ring starts at +0x200.
 */
#define AVE_FWLOG_HDR_RING_OFF	0x04	/* u32, = AVE_FWLOG_RING_OFF        */
#define AVE_FWLOG_HDR_RING_SIZE	0x08	/* u32                              */
#define AVE_FWLOG_HDR_CONF	0x0c	/* 256 bytes, one level per subsys  */
#define AVE_FWLOG_HDR_UNK_10C	0x10c	/* u32, Apple writes 25             */
#define AVE_FWLOG_HDR_UNK_110	0x110	/* u32, Apple writes 20000          */
#define AVE_FWLOG_HDR_RD	0x154	/* u32 host read counter            */
#define AVE_FWLOG_HDR_WR	0x198	/* u32 firmware write counter       */
#define AVE_FWLOG_RING_OFF	0x200
#define AVE_FWLOG_RING_SIZE	0x20000
#define AVE_FWLOG_SIZE		(AVE_FWLOG_RING_OFF + AVE_FWLOG_RING_SIZE)

/*
 * Per-subsystem log levels: emit iff (abs(level) & 0xf) <= (conf & 0xf).
 * 3 = CRIT ... 8 = DBG. Subsystems 0-4 bypass the table entirely, so crash and
 * assertion output is unconditional.
 *
 * These three carry the dumps that name structure fields verbatim - 602, 1018
 * and the command-processor sites respectively.
 */
#define AVE_LOG_LEVEL_DBG	8
#define AVE_LOG_SUBSYS_CMDPROC	130
#define AVE_LOG_SUBSYS_AVC	140
#define AVE_LOG_SUBSYS_HEVC	145

/*
 * Pixel formats.
 *
 * Enum names are not inferred - they come from the string pointer array at
 * 0xfffffe000c69a850, which the logging code indexes directly.
 */
enum ave_chroma_fmt {
	AVE_CHROMA_400	= 0,	/* monochrome */
	AVE_CHROMA_420	= 1,
	AVE_CHROMA_422	= 2,
	AVE_CHROMA_444	= 3,
};

enum ave_layout_kind {
	AVE_LAYOUT_LINEAR	= 0,
	AVE_LAYOUT_PACKED	= 1,
	AVE_LAYOUT_HTPC		= 2,
	AVE_LAYOUT_INTERCHANGE	= 3,
};

enum ave_lossy_level {
	AVE_LOSSLESS	= 0,
	AVE_LOSSY_75	= 1,
	AVE_LOSSY_62	= 2,
	AVE_LOSSY_50	= 3,
};

/*
 * Chroma subsampling divisors, from the table at 0xfffffe0007276400.
 * Note index 0 (monochrome) is (1,1), and the HTPC/Interchange paths do not
 * guard on it - so a monochrome Interchange format computes a 4:4:4-sized
 * chroma region. Read from the code; preserved here as a hazard.
 */
static inline void ave_chroma_div(enum ave_chroma_fmt fmt, u8 *h, u8 *v)
{
	static const u8 div[4][2] = {
		[AVE_CHROMA_400] = { 1, 1 },
		[AVE_CHROMA_420] = { 2, 2 },
		[AVE_CHROMA_422] = { 2, 1 },
		[AVE_CHROMA_444] = { 1, 1 },
	};

	*h = div[fmt][0];
	*v = div[fmt][1];
}

/*
 * THE hard constraint on client buffers.
 *
 * AVE_CHM_SetDataInfo_FwBuf returns -1015 unless luma addr, luma size and luma
 * stride are all non-zero AND both luma and chroma stride are multiples of 64.
 * Apple's own assertion text at 0xfffffe00072832bd states it verbatim and the
 * code matches instruction for instruction at 0xfffffe0008b72500..0xb72524.
 *
 * A V4L2 driver must enforce this on userspace buffers.
 */
#define AVE_STRIDE_ALIGN	64

/*
 * Surfaces the kext allocates itself are linear byte blobs with no geometry
 * keys, rounded up to this granularity.
 */
#define AVE_SURFACE_ALLOC_ALIGN	0x4000		/* max(PAGE_SIZE, 16 KB) */

/* Only two planes are ever used; a third would alias onto plane 1. */
#define AVE_MAX_PLANES		2

/*
 * _S_AVE_SurfaceInfoSet: 35 entries of 48 bytes, from the bzero at the head of
 * AVE_Work_Enc_CalcSurfaceInfo (mov w1,#0x690 at 0xfffffe0008ca9a64).
 * +0x18 is a count and +0x1c a size: AVE_CreateInternalSurfaces creates
 * [+0x18] surfaces and rejects any smaller than [+0x1c].
 */
#define AVE_INFOSET_ENTRIES	35
#define AVE_INFOSET_ENTRY_SIZE	48
#define AVE_INFOSET_SIZE	0x690

struct ave_surface_info {
	__le64	flags;		/* +0x00 OR'd into the static SurfaceCfg flags */
	__le32	mode_num;	/* +0x08 */
	__le32	type_num;	/* +0x0c */
	__le32	set_num;	/* +0x10 */
	__le32	layer_num;	/* +0x14 */
	__le32	count;		/* +0x18 number of buffers to create          */
	__le32	size;		/* +0x1c bytes per buffer (minimum accepted)   */
	__le32	extra[4];	/* +0x20 sub-region sizes; for Recon these are
				 *       luma/chroma data and meta sizes       */
} __packed;

/*
 * Surface indices (_E_AVE_SurfaceIdx), from the name table at
 * 0xfffffe0007ee1050 (41 entries, 16 bytes each).
 */
enum ave_surface_idx {
	AVE_SURF_INPUT_DATA		= 0,
	AVE_SURF_INPUT_SCALED_DATA	= 1,
	AVE_SURF_DIRECT_RECON		= 2,
	AVE_SURF_UC_INFO		= 3,	/* no InfoSet slot */
	AVE_SURF_MULTI_PASS_STATS	= 4,
	AVE_SURF_MB_INPUT_CTRL		= 5,
	AVE_SURF_RECON			= 6,
	AVE_SURF_LINK			= 7,
	AVE_SURF_CODED_DATA		= 8,
	AVE_SURF_CODED_HEADER		= 9,
	AVE_SURF_SLICE_HEADER		= 10,
	AVE_SURF_PROTECTED_DATA		= 11,
	AVE_SURF_MB_STATS		= 12,
	AVE_SURF_STATIC_AREA_QPMOD	= 13,	/* count 0 on M1 */
	AVE_SURF_STATIC_AREA_CBP0	= 14,	/* count 0 on M1 */
	AVE_SURF_COLOCATED		= 15,
	AVE_SURF_HSC_OUTPUT		= 16,	/* count 0 on M1 */
	AVE_SURF_LFS_REF		= 17,
	AVE_SURF_LRS_NEIGHBOR_MV	= 18,	/* count 0 on M1 */
	AVE_SURF_LFS_RESULT		= 19,
	AVE_SURF_LRS_RESULT		= 20,
	AVE_SURF_SRC_NEIGHBOR_INFO	= 21,
	AVE_SURF_SRC_NEIGHBOR_PIXEL	= 22,
	AVE_SURF_SRC_NEIGHBOR_DATA	= 23,
	AVE_SURF_SRC_NEIGHBOR_FW_DATA	= 24,
	AVE_SURF_TRANSCODED_DATA	= 25,	/* count 0 on M1 */
	AVE_SURF_ENTROPY_CODING		= 26,
	AVE_SURF_IOP_IPC		= 27,	/* no InfoSet slot */
	AVE_SURF_FW_IMAGE		= 28,	/* no InfoSet slot */
	AVE_SURF_FW_LOG			= 29,	/* no InfoSet slot */
	AVE_SURF_FW_HEAP		= 30,	/* no InfoSet slot */
	AVE_SURF_FW_IPC			= 31,	/* no InfoSet slot */
	AVE_SURF_FW_CLIENT		= 32,
	AVE_SURF_FW_CLIENT_MEM		= 33,
	AVE_SURF_INIT_PARAMS_COPY	= 34,
	AVE_SURF_MCTF_OUTPUT		= 35,
	AVE_SURF_MCTF_REF		= 36,
	AVE_SURF_GGM_REF		= 37,
	AVE_SURF_GGM_STATS		= 38,
	AVE_SURF_GGM_OUTPUT		= 39,
	AVE_SURF_DMV_OUTPUT		= 40,
	AVE_SURF_COUNT			= 41,
};

/*
 * Surface index -> InfoSet slot.
 *
 * Six indices have no slot: 3 (UCInfo) and 27..31 (the device-global and
 * firmware surfaces). Those are allocated directly with the static config
 * flags, with no InfoSet entry to OR in.
 *
 * Settled by AVE_CreateDataSurfaces (0xfffffe0008c78600), which pairs
 * GetSurfaceCfg(idx) with the InfoSet load in the same basic block:
 * idx 2 -> [x26,#96] at 0xfffffe0008c7886c, then idx 4 -> [x26,#144] at
 * 0xfffffe0008c789cc. AVE_DARTMapDataSurfaces reproduces it while emitting
 * idx 5 before idx 4, so this is not an ordering artefact.
 * AVE_GetSurfaceCfg itself does no remapping (sbfiz x8,x0,#4 at
 * 0xfffffe0008ca8d00, bound cmp w0,#0x29).
 */
#define AVE_SLOT_NONE	0xff

static inline u8 ave_surface_slot(enum ave_surface_idx idx)
{
	if (idx == AVE_SURF_UC_INFO || (idx >= AVE_SURF_IOP_IPC && idx <= AVE_SURF_FW_IPC))
		return AVE_SLOT_NONE;
	if (idx <= AVE_SURF_DIRECT_RECON)
		return idx;			/* 0..2   -> 0..2   */
	if (idx <= AVE_SURF_ENTROPY_CODING)
		return idx - 1;			/* 4..26  -> 3..25  */
	return idx - 6;				/* 32..40 -> 26..34 */
}

/* DPB capacity: refNum is capped at 16 and the total at a hard 17. */
#define AVE_DPB_MAX		17
#define AVE_MAX_REF_FRAMES	16

/*
 * sCAveCmdOpen (0x48): the 8 bytes past the common header are unused. The
 * builder writes str xzr there and the firmware's ProcessCmd_Open reads only
 * +0x08, +0x10 and +0x20 in the entire function. Open's real work is
 * CAVEPriorityQueue::RegisterClient(cmd[0x10]).
 *
 * sCAveCmdConfig (0x78), fields past the header:
 *   +0x48, +0x49  u8   gate McpuController creation
 *   +0x58, +0x5c  u32  doorbell cadence (CChannelManager::DoorBellCadenceSet)
 *   +0x60         u32  memory-controller DSID (AVE_MCC::GetDSID)
 *   +0x68         u64  IOVA of a shared region given to the firmware's
 *                      PlatformIOPIPCManager::AddSharedMemory
 *   +0x70         u32  size of that region
 * (+0x4a is written but never read; +0x50 is logged but never written.)
 */

/*
 * sCAveCmdAvcStart (0x3180) - session parameters. Only the fields a minimal
 * encode needs are listed; roughly 11.5 KB of the struct is unmapped.
 * Names come from AVE_Alg_PrintCfg's sub-printers, which name every field, and
 * were cross-checked against the firmware's AVE_KeyFrame::Init argument list.
 */
#define AVE_START_FRAMERATE	0x220
#define AVE_START_RCMODE	0x234
#define AVE_START_BITRATE	0x238
#define AVE_START_QP_I		0x240
#define AVE_START_QP_P		0x244
#define AVE_START_QP_B		0x248
#define AVE_START_QP_MIN	0x298
#define AVE_START_QP_MAX	0x29c
#define AVE_START_MAX_GOP	0x2b8	/* MaxKeyFrameInterval */
#define AVE_START_REF_NUM	0x2e0
#define AVE_START_WIDTH		0x368
#define AVE_START_HEIGHT	0x36c
#define AVE_START_BUF_SET	0x390	/* _S_AVE_Buf_Set, 0x21f0 bytes */
#define AVE_START_MAX_REF	0x2d44
#define AVE_START_PROFILE	0x291c
#define AVE_START_LEVEL		0x2938
#define AVE_START_ENTROPY	0x2fd8

#define AVE_BUF_SET_SIZE	0x21f0

/*
 * AVE_CMD_RESET carries a verbatim replay of the Start parameter block,
 * buffer table included. The arithmetic closes exactly:
 *   0x68 + 0x3118  = 0x3180  (AvcStart)
 *   0x68 + 0x13ec0 = 0x13f28 (HevcStart)
 *   0x48 + 0x13ec0 = 0x13f08 (Reset)
 * The 0x20 difference is the FwClient/FwClientMem pair that Start carries and
 * Reset does not. Reset's buffer table sits at +0x370.
 */

/*
 * Plane-offset constraint, from a kext assertion (alongside the stride rule):
 *   offset >= 0 && offset <= size && offset % 64 == 0
 *   && PerFrameData.StillOffsetW % 64 == 0
 *   && PerFrameData.StillOffsetH % 16 == 0
 */
#define AVE_PLANE_OFFSET_ALIGN	64

/*
 * Coded (bitstream) output buffer size.
 *
 * AVE_CalcBufSizeOfCodedData (0xfffffe0008b5f58c) IS a closed form. Its
 * parameters are named by its own os_log format strings:
 *   (devType, encType, W, H, chromaFmt, bitDepth, bufSize, bLossless,
 *    bufSizeFactor, bMaxBufSize, encMode, RCMode, initialQPI)
 * Bitrate, framerate, level, profile and entropy mode are NOT inputs, and no
 * level/MaxCPB table is consulted - verified by listing every data load in the
 * function.
 *
 * The base quantity is one raw 8-bit 4:2:0 frame at width aligned to the
 * macroblock size: 16 for AVC, 32 for HEVC (csel on encType==1 at
 * 0xfffffe0008b5f5d8). Height is NOT aligned.
 *
 * Rate control can inflate the base by factors from a table at
 * 0xfffffe000723e9f0 (1.3, 1.6, 1.2, 2.8, 51.0), but an UNCONDITIONAL ceiling
 * of 2 x base applies at 0xfffffe0008b5f86c (lsl w8,w22,#1; csel ... lt), so
 * nothing can route around it. Result is then rounded up to 4 KB
 * (0xfffffe0008b5f880).
 *
 * The 460800 floor is itself computed, as a 640x480 frame
 * (AVE_Linear_CalcFrameSize(640, 480, 8, 420) at 0xfffffe0008b5f80c).
 *
 * The expression below is EXACT - not conservative - for 8-bit 4:2:0 with
 * bLossless, bMaxBufSize, bufSize and bufSizeFactor all zero, RCMode != 3 and
 * encMode != 2. A driver that builds its own client state controls all of
 * those. If any are left unpinned, the domain maximum read off the ceiling is
 * 3 bytes/pixel: 3 * ALIGN(w, mb) * h.
 *
 * Checked: 1280x720 -> 1384448, 1920x1080 -> 3112960, 3840x2160 -> 12443648.
 */
#define AVE_CODED_MIN_SIZE	460800		/* 640 * 480 * 3 / 2 */
#define AVE_CODED_ALIGN		4096

static inline u32 ave_coded_data_size(u32 w, u32 h, bool hevc)
{
	u32 mb = hevc ? 32 : 16;
	u32 wa = (w + mb - 1) & ~(mb - 1);
	u32 base = wa * h * 3 / 2;
	u32 size;

	if (base >= AVE_CODED_MIN_SIZE)
		size = base;
	else
		size = (2 * base < AVE_CODED_MIN_SIZE) ? 2 * base : AVE_CODED_MIN_SIZE;

	return (size + AVE_CODED_ALIGN - 1) & ~(AVE_CODED_ALIGN - 1);
}

/* Absolute worst case over the whole input domain, from the 2x ceiling. */
static inline u32 ave_coded_data_size_max(u32 w, u32 h, bool hevc)
{
	u32 mb = hevc ? 32 : 16;

	return 3 * ((w + mb - 1) & ~(mb - 1)) * h;
}

/* AVE_CalcBufSizeOfCodedHeader takes no arguments: mov w0,#0xc000; ret. */
#define AVE_CODED_HEADER_SIZE	0xc000		/* 49152 */

/* Buffer count is clamped to at most 30. */
#define AVE_CODED_MAX_BUFS	30

/* Firmware must be mapped at DART IOVA 0 (assert at 0xfffffe0008bec408). */
#define AVE_FW_IOVA		0

/*
 * ---------------------------------------------------------------------------
 * AVE_PICMGMT_PARAMS - the per-frame encode parameters.
 *
 * A 0x5118-byte block at sCAveCmdAvcProcess + 0x12C0. Offsets below are into
 * the block; add AVE_PICMGMT_OFF for the offset inside the Process command.
 *
 * For AVC the firmware only ever reads four sub-ranges of it:
 * ProcessCmd_Process_AVC copies the wire command into a pooled internal buffer
 * slice by slice and queues that copy, so anything outside a slice is dropped
 * before the encoder sees it. Everything defined here falls inside one of the
 * four; see docs/32-picmgmt-params.md for the copy sites.
 *
 * Note there is deliberately no per-frame QP field. QP is session-scoped:
 * ConstantQpRateControl::processRateControl (fw 0x8bd4) selects one of the
 * three Start-time QPs by slice type. Only FrameType varies per frame.
 * ---------------------------------------------------------------------------
 */
#define AVE_PICMGMT_OFF			0x12c0
#define AVE_PICMGMT_SIZE		0x5118

/* Copied slices. Anything outside these never reaches the encoder. */
#define AVE_PICMGMT_SLICE0_OFF		0x0000
#define AVE_PICMGMT_SLICE0_LEN		0x03d0
#define AVE_PICMGMT_SLICE1_OFF		0x1738
#define AVE_PICMGMT_SLICE1_LEN		0x20	/* 0x28 when the DRC hdr is present */
#define AVE_PICMGMT_SLICE1_LEN_DRC	0x28
#define AVE_PICMGMT_DRC_OFF		0x1760	/* 8 bytes per entry, count at 0x175c */
#define AVE_PICMGMT_SLICE3_OFF		0x4228
#define AVE_PICMGMT_SLICE3_LEN		0x0ef0

/* What frame this is. */
#define AVE_PIC_FRAME_NUM		0x4f68	/* u64 */
#define AVE_PIC_FRAME_TYPE		0x4f78	/* int, see AVE_FRAME_* */
#define AVE_PIC_POC			0x4f80	/* int */
#define AVE_PIC_CTX_INDEX		0x4fa4	/* u32, 0 for a single client */
#define AVE_PIC_FRAME_RATE		0x4fa8	/* double, fps */

#define AVE_FRAME_TYPE_I		0
#define AVE_FRAME_TYPE_IDR		3

/* Rate-control update sub-block (slice 1). Carries no QP. */
#define AVE_PIC_FORCE_KEYFRAME		0x1738	/* int */
#define AVE_PIC_INPUT_COMPRESSED	0x174e	/* u8, 0 for plain NV12 */
#define AVE_PIC_DRC_COUNT		0x175c	/* int32, 0 if unused */

/* Input surface: NV12, two planes, each 64-byte aligned. */
#define AVE_PIC_IN_LUMA_ADDR		0x4570	/* u64 IOVA */
#define AVE_PIC_IN_LUMA_SIZE		0x4578	/* u32 */
#define AVE_PIC_IN_CHROMA_ADDR		0x4590	/* u64 IOVA */
#define AVE_PIC_IN_CHROMA_SIZE		0x4598	/* u32 */

/*
 * Output bitstream. Coded must equal the CodedData[slot] IOVA published at
 * Start time - the firmware asserts on it rather than using what we pass.
 */
#define AVE_PIC_OUT_CODED		0x4ef8	/* u64 IOVA */
#define AVE_PIC_OUT_CODED_HDR		0x4f00	/* u64 IOVA */
#define AVE_PIC_OUT_CODED_SIZE		0x4f08	/* u32, must exceed 3*W*H/4 */

/* Reconstruction target: the DPB slot this frame writes. 128-byte aligned. */
#define AVE_PIC_RECON_Y_MSB		0x4548	/* u64 */
#define AVE_PIC_RECON_Y_LSB		0x4550	/* u64 */
#define AVE_PIC_RECON_UV_MSB		0x4558	/* u64 */
#define AVE_PIC_RECON_UV_LSB		0x4560	/* u64 */
#define AVE_PIC_RECON_MV		0x4568	/* u64, colocated MV store */

/* Per-frame scratch, all drawn from the Start-time pools. */
#define AVE_PIC_SCRATCH_CMDINFO40	0x45f8	/* u64 */
#define AVE_PIC_SCRATCH_SLOTPOOL	0x4608	/* u64 */
#define AVE_PIC_SCRATCH_CMDINFO48	0x4618	/* u64 */
#define AVE_PIC_SRC_NEIGH_INFO		0x4670	/* u64 */
#define AVE_PIC_SRC_NEIGH_PIXEL		0x4690	/* u64 */
#define AVE_PIC_SRC_NEIGH_DATA		0x46b0	/* u64 */
#define AVE_PIC_SRC_NEIGH_FWDATA	0x46d0	/* u64 */
#define AVE_PIC_LOWRES_LUMA_SCALED	0x4f10	/* u64 */
#define AVE_PIC_LOWRES_RESULTS		0x4f48	/* u64 (+0x4f50 size) */
#define AVE_PIC_LOWRES_RC_RESULTS	0x4f58	/* u64 (+0x4f60 size) */

/* Reference lists. All zero for I-frame-only. */
#define AVE_PIC_REF_Y_L0_MSB		0x4228	/* u64[4] */
#define AVE_PIC_REF_UV_L0_MSB		0x4268	/* u64[4] */
#define AVE_PIC_REF_Y_L1_MSB		0x4388	/* u64[4] */
#define AVE_PIC_REF_UV_L1_MSB		0x43c8	/* u64[4] */
#define AVE_PIC_REF_COLOCATED_L1	0x44e8	/* u64 */

/* Session-scoped rate control, in sCAveCmdAvcStart. */
#define AVE_START_RC_MODE		0x234	/* u32, see docs/35 */
#define AVE_START_BITRATE		0x238	/* u32 */
#define AVE_START_QP_I			0x240	/* u32 */
#define AVE_START_QP_P			0x244	/* u32 */
#define AVE_START_QP_B			0x248	/* u32 */
#define AVE_START_GOP			0x2b8	/* u32 */
#define AVE_START_WIDTH			0x368	/* u32 */
#define AVE_START_HEIGHT		0x36c	/* u32 */

/*
 * The H.264 parameter sets inside sCAveCmdAvcStart.
 *
 * These are verbatim H264_SEQUENCE_HEADER_PARAMS / H264_PICTURE_HEADER_PARAMS
 * blocks, and the firmware carries its own exp-Golomb writer that turns them
 * into the actual NAL units (AVC_SPS::seq_parameter_set_rbsp at fw 0x41c8c,
 * AVC_PPS at 0x4288c). Fill these in correctly and the emitted stream is
 * conformant by construction rather than by our getting the bit packing right.
 *
 * CAVCController::InitEncodingParameters copies both out of the command:
 *   SPS  src cmd+0x291C  size 0x6B4  -> controller +0x247C4  (fw 0x6c024)
 *   PPS  src cmd+0x2FD0  size 0x180  -> controller +0x24E78  (fw 0x6c070)
 */
#define AVE_START_SPS_OFF		0x291c
#define AVE_START_SPS_SIZE		0x6b4
#define AVE_START_PPS_OFF		0x2fd0
#define AVE_START_PPS_SIZE		0x180

/*
 * Firmware-enforced preconditions on Start_AVC. Each is one instruction, so
 * the offset and the legal value come from the same place.
 *
 * bFWCreatesHeader must be EQUAL in the SPS and PPS blocks or Start returns
 * -1001 (fw 0x6d7bc). Set both to 1 to have the firmware emit the headers.
 *
 * header_len must be ZERO on input or Start returns -1001 (fw 0x41b68). This
 * one is a trap: it is an output field the firmware fills in, so a driver
 * that round-trips a previously returned block back into Start fails.
 *
 * seq_parameter_set_id must be 0. The PPS-side ids are force-zeroed by the
 * call site but the SPS id is not, and Apple's own defaults use 1.
 *
 * sComm.FrameRate must be > 0 - not an error return but a hard panic
 * (fw 0x6e690).
 */
#define AVE_SPS_FW_CREATES_HDR		0x2dc8	/* == AVE_PPS_FW_CREATES_HDR */
#define AVE_SPS_HEADER_LEN		0x2dcc	/* must be 0 on input        */
#define AVE_SPS_SEQ_PARAM_SET_ID	0x293c	/* must be 0                 */
#define AVE_PPS_FW_CREATES_HDR		0x3048
#define AVE_START_SLICE_MAP_NUM		0x25d4	/* 1 slice per frame         */
#define AVE_START_RECON_SET		0x0390
#define AVE_START_CODED_DATA_SET	0x0d50
#define AVE_START_CODED_HDR_SET		0x0f50

#define AVE_ERR_BAD_HEADER_CFG		(-1001)
#define AVE_ERR_HEADER_TOO_BIG		(-1019)
#define AVE_ERR_PPS_TOO_SMALL		(-1003)

/* sRC.Feature bit 31 selects the modern RateControl framework (fw 0x6d36c). */
#define AVE_START_RC_FEATURE		0x228
#define AVE_RC_FEATURE_NEW_FRAMEWORK	BIT(31)
#define AVE_RC_MODE_CBR			2
#define AVE_RC_MODE_CONST_QP		3

#endif /* __AVE_ABI_H__ */
