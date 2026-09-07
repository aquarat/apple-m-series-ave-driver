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
	__le32	arg;		/* +0x24  command specific (priority value
				 *        for AVE_CMD_PRIORITY)            */
	__le32	unk_28;		/* +0x28  unknown                          */
	__le32	unk_2c;		/* +0x2c  unknown                          */
	__u8	timeout[16];	/* +0x30  _S_AVE_TimeOut, copied verbatim  */
} __packed;

#define AVE_MAX_INFLIGHT	51	/* hdr.slot is bounds-checked < 51 */

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
static const struct { u8 h, v; } ave_chroma_div[4] = {
	[AVE_CHROMA_400] = { 1, 1 },
	[AVE_CHROMA_420] = { 2, 2 },
	[AVE_CHROMA_422] = { 2, 1 },
	[AVE_CHROMA_444] = { 1, 1 },
};

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

/* Firmware must be mapped at DART IOVA 0 (assert at 0xfffffe0008bec408). */
#define AVE_FW_IOVA		0

#endif /* __AVE_ABI_H__ */
