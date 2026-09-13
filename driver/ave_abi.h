/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE (Video Encoder) - host <-> firmware ABI
 *
 * Every constant here was read out of AppleAVE2.kext or the AVE firmware and
 * re-verified against the binary. Anything not verified is marked UNVERIFIED.
 * See docs/00-methodology.md before adding to this file.
 *
 * TWO FIRMWARE ABIS
 * -----------------
 * The command/session/frame ABI differs between the macOS 13.5 firmware this
 * machine runs (AppleAVE2FW-6070.11.1, docs/46, docs/47) and the macOS 26.6.2
 * firmware most of this project was read from (AppleAVE2FW-9003.78.0). The
 * firmware has no version check: a wrongly sized command makes it spin.
 *
 * So every value that differs by version lives in a per-version descriptor,
 * struct ave_cmd_abi (bottom of this file), selected by ave_cmd_abi_get().
 * Command builders (ave_cmd.c) must only read offsets through it.
 *
 * The un-prefixed AVE_CMD_* / AVE_CMDSZ_* / AVE_START_* / AVE_SPS_* /
 * AVE_PPS_* / AVE_PIC_* / AVE_PICMGMT_* macros in the middle of this file are
 * the historical macOS 26.6.2 names. They are kept so existing code compiles
 * and they are what the 26.6.2 descriptor is built from, but they are
 * 26.6.2-ONLY: writing them into a 13.5 command corrupts it. Values that are
 * identical on both versions stay plain constants and say so.
 */
#ifndef __AVE_ABI_H__
#define __AVE_ABI_H__

#include <linux/compiler_attributes.h>
#include <linux/types.h>

#include "ave_version.h"

/*
 * Wire command ids - macOS 26.6.2 ONLY. 13.5 ids: enum ave135_cmd below.
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
 * Wire command ids - macOS 13.5 ONLY.
 *
 * CFlowControllerBase::CmdProcessor (13.5 fw 0xd614) loads the u16 id
 * (ldrh at 0xd65c), accepts 1..14 (cmp w8,#0xd at 0xd6e4) and jumps through a
 * 14-entry table at 0xe2e8; names are the firmware's own GetString_eCAveCmdID
 * table at 0xed060 (docs/46 §1.1). One id per operation, codec in the id.
 * The host operation each id implements is from the 13.5 kext builders'
 * argument types (docs/46 §1.2), e.g. MakeFwCmd_Open(..., sCAveCmdStart*).
 */
enum ave135_cmd {
	AVE135_CMD_CONFIG		= 1,	/* host Config     size check 0xd704 */
	AVE135_CMD_START		= 2,	/* host Open       0xd794 */
	AVE135_CMD_RESET		= 3,	/* host Reset      0xd824 */
	AVE135_CMD_AVC_INIT		= 4,	/* host Start_AVC  0xd8bc */
	AVE135_CMD_HEVC_INIT		= 5,	/* host Start_HEVC 0xd970 */
	AVE135_CMD_UNINIT		= 6,	/* host Stop       0xda0c */
	AVE135_CMD_AVC_ENCODE		= 7,	/* host Process_AVC 0xda9c */
	AVE135_CMD_HEVC_ENCODE		= 8,	/* host Process_HEVC 0xdb30 */
	AVE135_CMD_LRME_STANDALONE	= 9,	/* 0xdbc4 */
	/* 10 MCTF_PROCESS: jump-table entry is the default arm 0xe218 */
	AVE135_CMD_FLUSH		= 11,	/* host Flush      0xdc58 */
	AVE135_CMD_STOP			= 12,	/* host Close      0xdce8 */
	AVE135_CMD_COMPLETE		= 13,	/* host Complete   0xdd78 */
	AVE135_CMD_POWERDOWN		= 14,	/* host Halt       0xde08 */
};

/*
 * 13.5 firmware -> host completion ids, same table (0xed060, docs/46 §1.1).
 * Each is the literal passed to NotificationToHost (13.5 fw 0x13684) by the
 * handler: CONFIG_DONE mov w1,#0xe01 0xe724; START_DONE 0xe98c; RESET_DONE
 * 0x124d8; INIT_DONE 0x14528; UNINIT_DONE 0xfb04; ENCODE_DONE 0x1337c;
 * LRME_DONE 0xec18; FLUSH_DONE 0x10604; STOP_DONE 0x1089c; COMPLETE_DONE
 * 0x10bcc.
 */
enum ave135_reply {
	AVE135_REPLY_CONFIG_DONE	= 0xe01,
	AVE135_REPLY_START_DONE		= 0xe02,
	AVE135_REPLY_RESET_DONE		= 0xe03,
	AVE135_REPLY_INIT_DONE		= 0xe04,
	AVE135_REPLY_UNINIT_DONE	= 0xe05,
	AVE135_REPLY_ENCODE_DONE	= 0xe06,
	AVE135_REPLY_LRME_DONE		= 0xe07,
	AVE135_REPLY_MCTF_DONE		= 0xe08,
	AVE135_REPLY_FLUSH_DONE		= 0xe09,
	AVE135_REPLY_STOP_DONE		= 0xe0a,
	AVE135_REPLY_COMPLETE_DONE	= 0xe0b,
};

/*
 * 13.5 reply status words (AVE_FW_ERROR). Success is 0xEE0000, not 0
 * (ProcessConfig mov w3,#0xee0000 at 0xe730). START failure 0xEE0001
 * (0xe94c-0xe958), INIT/ENCODE failure 0xEE0002 (0xef00-0xef0c, 0xf154),
 * bFWCreatesHeader SPS/PPS mismatch 0xEE0005 (0x5dd58). docs/46 §2.1, §9.3.
 */
#define AVE135_STATUS_OK		0x00ee0000
#define AVE135_STATUS_START_FAIL	0x00ee0001
#define AVE135_STATUS_FAIL		0x00ee0002
#define AVE135_STATUS_BAD_HEADER_CFG	0x00ee0005

/*
 * Command struct sizes - macOS 26.6.2 ONLY, enforced by the firmware - it
 * rejects any other length. 13.5 sizes are in ave_cmd_abi_13_5.
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
 * Common command header (first 0x40 bytes of every command) - macOS 26.6.2
 * layout. The 13.5 layout is struct ave135_cmd_hdr below; the size (0x40) is
 * the same on both, nothing else past +0x10 is.
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

/*
 * macOS 13.5 common header, 0x40 bytes (docs/46 §2). 13.5 kext VAs.
 */
struct ave135_cmd_hdr {
	__le16	id;		/* +0x00 enum ave135_cmd; fw ldrh 0xd65c   */
	__le16	unk_02;		/* +0x02 zeroed by fw (0xd69c) and host    */
	__le32	unk_04;		/* +0x04 not written                       */
	__le64	count;		/* +0x08 CNT, str x20,[x23,#8] 0xfffffe0008ea9168 */
	__le32	client_id;	/* +0x10 u32 CID, stp w8,w9,[x23,#16]
				 *       0xfffffe0008ea9174; fw ldr w1,[x20,#16]
				 *       0xe8e0 -> RegisterClient(unsigned int) */
	__le32	client_type;	/* +0x14 host client[212]; no fw read found,
				 *       name inferred                         */
	__le32	codec;		/* +0x18 0 = AVC, 1 = HEVC; fw cbz 0x13e60  */
	__le32	slot;		/* +0x1c stur d0,[x23,#28] 0xfffffe0008ea9188 */
	__le32	priority;	/* +0x20 fw ldr w2,[x21,#32] 0xee7c ->
				 *       SetClientPriority                      */
	__le32	dpm_handle;	/* +0x24 AVE_DPM_RetrievePipe, inside SendFwCmd
				 *       0xfffffe0008efb7a8                     */
	__u8	timeout[16];	/* +0x28 stur q0,[x23,#40] 0xfffffe0008ea9190 */
	__u8	unk_38[8];	/* +0x38 not written                       */
} __packed;

#define AVE_CMD_HDR_SIZE	0x40	/* same on both versions (docs/46 §2) */

/* 26.6.2 ONLY: hdr.slot is bounds-checked < 51. 13.5: <= 40, see abi->hdr. */
#define AVE_MAX_INFLIGHT	51

/*
 * CAVEPriorityQueue::RegisterClient caps concurrent clients (cmp w8,#0x80).
 * Same on both: 26.6.2 fw 0x3f710, 13.5 fw 0x17790 (docs/46 §2).
 */
#define AVE_MAX_CLIENTS		128

/*
 * hdr.enc_type - macOS 26.6.2 ONLY (header +0x1c). 13.5 carries the codec at
 * +0x18 as 0 = AVC, 1 = HEVC; use abi->hdr.codec_avc/codec_hevc.
 */
enum ave_enc_type {
	AVE_ENC_AVC	= 1,
	AVE_ENC_HEVC	= 2,
};

/*
 * hdr.work_type - macOS 26.6.2 ONLY (header +0x18): selects the engine for
 * AVE_CMD_PROCESS. 13.5 has no such field; the engine is in the command id
 * (AVC_ENCODE / HEVC_ENCODE / LRME_STANDALONE, docs/46 §1).
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
 * Boot/IPC transport constants moved to ave_abi_boot.h, per firmware ABI.
 */

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
 * THE hard constraint on client buffers. The 64-byte stride rule is the same
 * on both versions (13.5 kext 0xfffffe0008eb075c-0778, docs/47 §5); 13.5
 * drops the luma-size term.
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
 * keys, rounded up to this granularity. Same on both (13.5 kext
 * 0xfffffe0008f33128-3144, docs/47 §5).
 */
#define AVE_SURFACE_ALLOC_ALIGN	0x4000		/* max(PAGE_SIZE, 16 KB) */

/* Only two planes are ever used; a third would alias onto plane 1.
 * Same on both (13.5 kext 0xfffffe0008f36970, docs/47 §5). */
#define AVE_MAX_PLANES		2

/*
 * ---------------------------------------------------------------------------
 * Host-side surface bookkeeping - macOS 26.6.2 kext ONLY.
 *
 * The 13.5 kext is structurally different (docs/47 §4): _S_AVE_SurfaceInfoSet
 * is 0x1EC bytes of named members (memset 0xfffffe0008ec685c), there are 30
 * surface kinds not 41 (bound cmp w0,#0x1e at 0xfffffe0008f37408), and there is
 * no index->slot map. None of this is on the wire; a Linux driver allocates
 * its own buffers and publishes them through Start_AVC / Process.
 * ---------------------------------------------------------------------------
 *
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

/* DPB capacity: refNum is capped at 16 and the total at a hard 17.
 * Same on both (13.5 kext mov w9,#0x11 0xfffffe0008ea505c; docs/47 §4). */
#define AVE_DPB_MAX		17
#define AVE_MAX_REF_FRAMES	16

/*
 * ---------------------------------------------------------------------------
 * macOS 26.6.2 ONLY from here to the descriptor section: command layouts and
 * their historical macro names. 13.5 values live in ave_cmd_abi_13_5.
 * ---------------------------------------------------------------------------
 *
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
#define AVE_START_FWCLIENT_ADDR	0x048	/* docs/21 §4, fw ldr x1,[x21,#72] 0x297d8 */
#define AVE_START_FWCLIENT_SIZE	0x050
#define AVE_START_FWCLIENTMEM_ADDR 0x058	/* docs/21 §4 */
#define AVE_START_FWCLIENTMEM_SIZE 0x060
#define AVE_START_FRAMERATE	0x220
#define AVE_START_RCMODE	0x234
#define AVE_START_BITRATE	0x238
#define AVE_START_QP_I		0x240
#define AVE_START_QP_P		0x244
#define AVE_START_QP_B		0x248
#define AVE_START_QP_MIN	0x298
#define AVE_START_QP_MAX	0x29c
#define AVE_START_MAX_GOP	0x2b8	/* MaxKeyFrameInterval */
#define AVE_START_STRICT_GOP	0x2bc	/* StrictKeyFrameInterval, docs/20 §3.3 */
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
 * Coded (bitstream) output buffer size. The default-path formula below is the
 * same on both versions (13.5 kext 0xfffffe0008ea4da0-0xea4f88 gives identical
 * 720p/1080p/4K results, docs/47 §3); only the RC-mode inflation term and its
 * gate differ, and the builders never take that path.
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

/*
 * AVE_CalcBufSizeOfCodedHeader takes no arguments: mov w0,#0xc000; ret.
 * 26.6.2 ONLY: 13.5 returns 0x23000 (kext 0xfffffe0008ea4fb8), see
 * abi->start_avc.coded_hdr_bytes.
 */
#define AVE_CODED_HEADER_SIZE	0xc000		/* 49152 */

/* 26.6.2 ONLY: buffer count is clamped to at most 30. 13.5: 20. */
#define AVE_CODED_MAX_BUFS	30

/* Firmware must be mapped at DART IOVA 0 (assert at 0xfffffe0008bec408).
 * Value same on 13.5 (-1018 at 0xfffffe0008ef5664, docs/46 §10.3). */
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

/* IMG_FRAME_TYPE values 0 and 3 are the same on both versions
 * (13.5 AVE_H264_PrepareSliceHeader 0x20ff0 / 0x210d4, docs/47 §1.3). */
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
#define AVE_PIC_OUT_MODE		0x4ef0	/* u8, docs/32 §6.4 (fw 0x688dc) */
#define AVE_PIC_OUT_INDEX		0x4ef4	/* u32, docs/32 §6.4 (fw 0x68910) */
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

/* Session-scoped rate control, in sCAveCmdAvcStart (aliases of the above). */
#define AVE_START_RC_MODE		AVE_START_RCMODE	/* u32, docs/35 */
#define AVE_START_GOP			AVE_START_MAX_GOP

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
#define AVE_START_RECON_SET		0x0390	/* 0x20 {dataAddr,dataSize,metaAddr,
						 *  metaSize} x 17, docs/21 §3.2 */
#define AVE_START_RECON_STRIDE		0x20
#define AVE_START_CODED_DATA_SET	0x0d50	/* {u64 iAddr,u32 iSize} x 30 */
#define AVE_START_CODED_HDR_SET		0x0f50	/* {u64 iAddr,u32 iSize} x 30 */
#define AVE_START_BUF_STRIDE		0x10	/* _S_AVE_Buf, docs/21 §2.2 */

#define AVE_ERR_BAD_HEADER_CFG		(-1001)
#define AVE_ERR_HEADER_TOO_BIG		(-1019)
#define AVE_ERR_PPS_TOO_SMALL		(-1003)

/* sRC.Feature bit 31 selects the modern RateControl framework (fw 0x6d36c). */
#define AVE_START_RC_FEATURE		0x228
#define AVE_RC_FEATURE_NEW_FRAMEWORK	BIT(31)
#define AVE_RC_MODE_CBR			2
#define AVE_RC_MODE_CONST_QP		3

/*
 * Source surface geometry.
 *
 * The macroblock grid is taken from the host-supplied SPS block
 * (pic_*_minus1 + 1 at fw 0x6c384 / 0x6c3dc), NOT from VideoParams.ui32Height.
 * So the encoder reads a whole number of macroblock rows regardless of the
 * height we declare: a 1080p encode fetches 1088 luma rows.
 *
 * A source buffer allocated for 1080 rows is therefore eight rows short and
 * the read runs off the end of the mapping. Always allocate and DART-map the
 * input at macroblock-aligned height.
 *
 * Hardware width granularity is 2 MB, not 1: the picture-size register
 * computes ((w + 31) >> 4) & 0x7fe (fw 0xb4514-0xb4524).
 */
#define AVE_MB_SIZE			16
#define ave_mb_align(v)			(((v) + AVE_MB_SIZE - 1) & ~(AVE_MB_SIZE - 1))
#define ave_src_luma_rows(h)		ave_mb_align(h)
#define ave_src_chroma_rows(h)		(ave_mb_align(h) / 2)

/* Dimension fields that must agree, for 1920x1080: 1920/1088, 119, 67. */
#define AVE_SPS_PIC_WIDTH_MBS_M1	0x2d4c	/* SPS +0x430 */
#define AVE_SPS_PIC_HEIGHT_MAPU_M1	0x2d50	/* SPS +0x434 */
#define AVE_SPS_DIRECT_8X8_INFERENCE	0x2d58
/* Further SPS/PPS fields a fixed-QP session sets - docs/37 §3, §4 (26.6.2
 * DebugInit VAs there; widths u32 unless noted). */
#define AVE_SPS_LEVEL			0x2938	/* _E_AVC_Level, 12 = 4.0     */
#define AVE_SPS_LOG2_MAX_FRAME_NUM_M4	0x2d34
#define AVE_SPS_POC_TYPE		0x2d38
#define AVE_SPS_GAPS_IN_FRAME_NUM	0x2d48	/* u8 */
#define AVE_SPS_FRAME_MBS_ONLY		0x2d54
#define AVE_SPS_VUI_PRESENT		0x2d5c	/* u8 */
#define AVE_SPS_FRAME_CROPPING		0x2db4	/* u8 */
#define AVE_SPS_CROP_LEFT		0x2db8
#define AVE_SPS_CROP_RIGHT		0x2dbc
#define AVE_SPS_CROP_TOP		0x2dc0
#define AVE_SPS_CROP_BOTTOM		0x2dc4
#define AVE_PPS_PIC_PARAM_SET_ID	0x2fd0
#define AVE_PPS_SEQ_PARAM_SET_ID	0x2fd4
#define AVE_PPS_ENTROPY_CODING_MODE	0x2fd8	/* == AVE_START_ENTROPY       */
#define AVE_PPS_NUM_SLICE_GROUPS_M1	0x2fe0
#define AVE_PPS_PIC_INIT_QP_M26		0x2ff8	/* s32 */
#define AVE_PPS_DEBLOCKING_CTRL		0x3020	/* u8 */
#define AVE_PPS_CONSTRAINED_INTRA	0x3021	/* u8 */
#define AVE_PPS_TRANSFORM_8X8		0x3023	/* u8 */
#define AVE_SPS_FRAME_MBS_ONLY_FLAG_REQ	1	/* CropUnitY assumes it */

/*
 * Input pixel format.
 *
 * There is no host-supplied format selector. The firmware derives the pixel
 * format enumerator itself from SPS.chroma_format_idc and the bit depth
 * (fw 0x6c7dc-0x6c828: base 18/20/22 by chroma format, +1 when bit depth is
 * non-zero) and feeds it to a DMA register. So "tell it we have NV12" means
 * setting the SPS fields correctly and nothing else.
 *
 * WARNING: 10-bit input is not rejected. The firmware logs "10bit content is
 * not supported" (fw 0xb69ec) and then encodes mis-configured - no panic, no
 * error return. That is unlike every other bad input on this path, which is
 * loud, so the driver must refuse non-8-bit itself.
 */
#define AVE_SPS_PROFILE			0x291c	/* 6 = High                   */
#define AVE_SPS_CHROMA_FORMAT_IDC	0x2940	/* 1 = 4:2:0                  */
#define AVE_SPS_BIT_DEPTH_LUMA_M8	0x2948	/* 0 = 8-bit                  */
#define AVE_SPS_BIT_DEPTH_CHROMA_M8	0x294c	/* 0 = 8-bit                  */
#define AVE_START_STRIDE_MODE		0x0094	/* 0 = use per-frame strides  */
#define AVE_START_NEED_LSB_PLANES	0x25a5	/* 0 = no _LSB plane demanded */

#define AVE_PROFILE_HIGH		6

/*
 * Per-frame input planes. The descriptor is a union selected by
 * bInputCompressed (fw 0xb6cf4); these are the sLinear arm. Strides must be
 * non-zero and 64-byte aligned or the frame is refused.
 */
#define AVE_PIC_IN_LUMA_STRIDE		0x457c
#define AVE_PIC_IN_CHROMA_STRIDE	0x459c

/*
 * ===========================================================================
 * Per-version command descriptor
 * ===========================================================================
 *
 * Everything a command builder needs that differs between macOS 13.5 and
 * macOS 26.6.2. Offsets are absolute byte offsets into the command buffer
 * unless the member says otherwise. AVE_OFF_NONE (0) marks a field that does
 * not exist in that ABI; no real field of any layout below sits at offset 0
 * except the header id, which is never optional.
 *
 * Evidence: every 13.5 value in ave_cmd_abi_13_5 carries its firmware (bare
 * VA) or kext (0xfffffe...) address; 26.6.2 values come from the macros above
 * or cite the doc they were read in.
 */
#define AVE_OFF_NONE		0
#define AVE_SLOT_CALLER		0xfffffffe	/* slot chosen per command    */
#define AVE_SLOT_GLOBAL		0xffffffff	/* Config/Halt literal ~0     */
#define AVE_PRIORITY_DEFAULT	200		/* {id,200} literal, both ABIs */

/* Host-level operations; the wire id per ABI is abi->cmd[op].id. */
enum ave_op {
	AVE_OP_CONFIG,
	AVE_OP_HALT,
	AVE_OP_OPEN,
	AVE_OP_CLOSE,
	AVE_OP_START_AVC,
	AVE_OP_START_HEVC,
	AVE_OP_STOP,
	AVE_OP_PROCESS_AVC,
	AVE_OP_PROCESS_HEVC,
	AVE_OP_COMPLETE,
	AVE_OP_PRIORITY,
	AVE_OP_FLUSH,
	AVE_OP_RESET,
	AVE_OP_COUNT,
};

struct ave_cmd_desc {
	u16	id;		/* wire id; 0 = no such command in this ABI */
	u16	reply_id;	/* completion message id; 0 = none / unknown */
	u32	size;		/* exact length the firmware asserts */
	u32	slot;		/* header slot literal, or AVE_SLOT_CALLER */
	u32	priority;	/* header priority literal */
	u32	reply_size;	/* length of the completion message */
};

struct ave_hdr_layout {
	u8	count;			/* u64 CNT */
	u8	client_id;		/* CID, client_id_bytes wide */
	u8	client_id_bytes;	/* 8 on 26.6.2, 4 on 13.5 */
	u8	work_type;		/* u32, AVE_OFF_NONE on 13.5 */
	u8	codec;			/* u32 */
	u8	slot;			/* u32 */
	u8	priority;		/* u32 */
	u8	timeout;		/* 16 bytes */
	u32	codec_avc;
	u32	codec_hevc;
	u32	max_slot;		/* slot must be < max_slot */
};

struct ave_reply_layout {
	u8	id;			/* u16 */
	u8	client_id;
	u8	client_id_bytes;
	u8	slot;			/* u32 */
	u8	status;			/* u32 */
	u32	status_ok;
};

struct ave_config_layout {
	u32	skip_mcpu;		/* u8 */
	u32	create_mcpu;		/* u8, must be 1 for an McpuController */
	u32	reg_dart_addr;		/* u64, 13.5 only */
	u32	doorbell_cadence0;	/* u32 */
	u32	doorbell_cadence1;	/* u32 */
	u32	dsid;			/* u32 (26.6.2) or u8 (13.5) */
	u32	dsid2;			/* second u8 DSID, 13.5 only */
	u8	dsid_bytes;
	u32	shmem_addr;		/* u64 */
	u32	shmem_size;		/* u32 */
	u32	shmem_min;		/* 4 x carve set, INFERRED lower bound */
};

struct ave_start_avc_layout {
	u32	fw_client_addr;		/* u64 */
	u32	fw_client_size;		/* u32 */
	u32	fw_client_mem_addr;	/* u64 */
	u32	fw_client_mem_size;	/* u32 */
	u32	width;			/* u32, MB-aligned coded width */
	u32	height;			/* u32, MB-aligned coded height */
	u32	frame_rate;		/* u32, must be > 0 */
	u32	bitrate;		/* u32 */
	u32	rc_mode;		/* u32 RC selector */
	u32	rc_mode_fixed_qp;	/* value of rc_mode for fixed QP */
	u32	rc_feature;		/* u64 sRC.Feature, 26.6.2 only */
	u64	rc_feature_fixed_qp;	/* bits to set in rc_feature */
	u32	qp_i, qp_p, qp_b;	/* u32 */
	u32	qp_min, qp_max;		/* u32 */
	u32	key_interval;		/* u32 MaxKeyFrameInterval / IdrPeriod */
	u32	key_interval_strict;	/* u32, 26.6.2 only */
	u32	slice_num;		/* s32 sSliceMap.iNum */
	/*
	 * Where the firmware writes the SPS+PPS NAL bytes it generates
	 * (Apple: pVP->p_ParameterSetsBuffer / _BufferSize). 13.5 dereferences
	 * it unguarded in CAVCController::InitEncodingParameters (fw 0x5df28 ->
	 * MappedMemory 0x20bc0), which is the "MappedMemory.cpp, 39: paddr != 0"
	 * assert we hit with it zero (docs/52). The kext refuses to send the
	 * command with either half zero (AVE_CHM_SetFwBuf, -1015,
	 * 0xfffffe0008eaf4c8).
	 */
	u32	param_sets_addr;	/* u64 DART IOVA */
	u32	param_sets_size;	/* u32, must be non-zero */
	/* DPB (recon) table: recon_max entries, recon_stride apart */
	u32	recon_set;
	u32	recon_stride;
	u32	recon_max;
	u32	recon_addr;		/* u64, offset inside an entry */
	u32	recon_size;		/* u32 luma bytes, inside entry; NONE = absent */
	u32	recon_meta_addr;	/* u64 second-plane addr, inside entry */
	/* coded data / coded header tables */
	u32	coded_max;
	u32	coded_addr, coded_addr_stride;		/* u64[] */
	u32	coded_size, coded_size_stride;		/* u32[] */
	u32	coded_hdr_addr, coded_hdr_addr_stride;	/* u64[] */
	u32	coded_hdr_size, coded_hdr_size_stride;	/* u32[] */
	u32	coded_hdr_bytes;	/* AVE_CalcBufSizeOfCodedHeader */
	/* parameter-set blocks */
	u32	sps_block, sps_block_size;
	u32	pps_block, pps_block_size;
};

/* Absolute command offsets of H264 SPS/PPS parameter fields. */
struct ave_sps_layout {
	bool	enum_profile_level;	/* 26.6.2 _E_AVC_*; 13.5 raw idc */
	u32	profile;		/* u32 */
	u32	level;			/* u32 */
	u32	seq_parameter_set_id;
	u32	chroma_format_idc;
	u32	bit_depth_luma_minus8;
	u32	bit_depth_chroma_minus8;
	u32	log2_max_frame_num_minus4;
	u32	pic_order_cnt_type;
	u32	max_num_ref_frames;
	u32	gaps_in_frame_num_allowed;	/* u8 */
	u32	pic_width_in_mbs_minus1;
	u32	pic_height_in_map_units_minus1;
	u32	frame_mbs_only_flag;
	u32	direct_8x8_inference_flag;
	u32	vui_parameters_present_flag;	/* u8 */
	u32	frame_cropping_flag;		/* u8 */
	u32	crop_left, crop_right, crop_top, crop_bottom;
	u32	fw_creates_header;		/* u8 */
	u32	header_len;			/* u32, bits */
};

struct ave_pps_layout {
	u32	pic_parameter_set_id;
	u32	seq_parameter_set_id;
	u32	entropy_coding_mode_flag;
	u32	num_slice_groups_minus1;
	u32	pic_init_qp_minus26;		/* s32 */
	u32	deblocking_filter_control;	/* u8 */
	u32	constrained_intra_pred;		/* u8 */
	u32	transform_8x8_mode;		/* u8 */
	u32	fw_creates_header;		/* u8 */
};

/* Offsets RELATIVE TO AVE_PICMGMT_PARAMS (process_avc.picmgmt). */
struct ave_process_avc_layout {
	u32	picmgmt;		/* block offset in the command */
	u32	picmgmt_size;
	bool	picmgmt_size_word;	/* block's first u32 = its own size */
	u32	frame_num;		/* u64, AVE_OFF_NONE on 13.5 */
	u32	poc;			/* s32, AVE_OFF_NONE on 13.5 */
	u32	frame_rate_f64;		/* IEEE double fps, AVE_OFF_NONE on 13.5 */
	u32	frame_type;		/* s32 IMG_FRAME_TYPE */
	u32	input_compressed;	/* u8, 0 = linear */
	u32	in_luma_addr;		/* u64 */
	u32	in_luma_size;		/* u32, AVE_OFF_NONE on 13.5 */
	u32	in_luma_stride;		/* u32 */
	u32	in_chroma_addr;
	u32	in_chroma_size;
	u32	in_chroma_stride;
	u32	out_mode;		/* u8 */
	u32	out_index;		/* u32 */
	u32	out_coded;		/* u64 */
	u32	out_coded_hdr;		/* u64 */
	u32	out_coded_size;		/* u32 */
	u32	recon_y;		/* u64 sRecon.Y_MSB */
	u32	recon_uv;		/* u64 sRecon.UV_MSB */
	u32	recon_mv;		/* u64 colocated MV store */
};

struct ave_cmd_abi {
	enum ave_fw_abi			abi;
	const char			*name;
	struct ave_cmd_desc		cmd[AVE_OP_COUNT];
	struct ave_hdr_layout		hdr;
	struct ave_reply_layout		reply;
	struct ave_config_layout	config;
	struct ave_start_avc_layout	start_avc;
	struct ave_sps_layout		sps;
	struct ave_pps_layout		pps;
	struct ave_process_avc_layout	process_avc;
};

extern const struct ave_cmd_abi ave_cmd_abi_13_5;
extern const struct ave_cmd_abi ave_cmd_abi_26_6;

/* NULL for AVE_ABI_UNKNOWN: never guess, a wrong size wedges the firmware. */
const struct ave_cmd_abi *ave_cmd_abi_get(enum ave_fw_abi abi);

#ifdef AVE_CMD_ABI_DEFINE_TABLES
/*
 * The tables themselves. Defined exactly once, by ave_cmd.c.
 */

/* ------------------------------------------------------------------------
 * macOS 13.5 (22G74) - AppleAVE2FW-6070.11.1. Bare VAs are 13.5 firmware
 * image VAs, 0xfffffe... are 13.5 kernelcache VAs. docs/46, docs/47.
 * ------------------------------------------------------------------------ */
const struct ave_cmd_abi ave_cmd_abi_13_5 = {
	.abi	= AVE_ABI_MACOS_13_5,
	.name	= "macOS 13.5",
	.cmd = {
		/* sizes: fw insize checks in CmdProcessor; slot/prio literals:
		 * kext q-literal table 0xfffffe000722f210 ({3,200},{4,200},
		 * {6,200},{7,200}); reply ids: NotificationToHost call sites
		 * (enum ave135_reply); reply sizes: 0x40, or 0x48 for
		 * ENCODE/LRME_DONE (fw 0x136d0 mask 0x71f, 0x13784 tst #0x60). */
		[AVE_OP_CONFIG] = {		/* fw 0xd704; kext memset 0xfffffe0008efada4;
						 * slot ~0 q literal 0xfffffe000723ccc0,
						 * prio str wzr 0xfffffe0008efadc4 */
			AVE135_CMD_CONFIG, AVE135_REPLY_CONFIG_DONE, 0x70,
			AVE_SLOT_GLOBAL, 0, 0x40 },
		[AVE_OP_HALT] = {		/* fw 0xde08; kext 0xfffffe0008efb0b8;
						 * ProcessPowerDown sends no reply found */
			AVE135_CMD_POWERDOWN, 0, 0x40, AVE_SLOT_GLOBAL, 0, 0 },
		[AVE_OP_OPEN] = {		/* fw 0xd794; kext 0xfffffe0008ea9198 */
			AVE135_CMD_START, AVE135_REPLY_START_DONE, 0x40,
			3, AVE_PRIORITY_DEFAULT, 0x40 },
		[AVE_OP_CLOSE] = {		/* fw 0xdce8; kext 0xfffffe0008ea956c */
			AVE135_CMD_STOP, AVE135_REPLY_STOP_DONE, 0x48,
			4, AVE_PRIORITY_DEFAULT, 0x40 },
		[AVE_OP_START_AVC] = {		/* fw 0xd8bc; kext 0xfffffe0008ea993c */
			AVE135_CMD_AVC_INIT, AVE135_REPLY_INIT_DONE, 0x10e10,
			6, AVE_PRIORITY_DEFAULT, 0x40 },
		[AVE_OP_START_HEVC] = {		/* fw 0xd970-0xd97c; kext 0xfffffe0008ea9fcc */
			AVE135_CMD_HEVC_INIT, AVE135_REPLY_INIT_DONE, 0x32dc8,
			6, AVE_PRIORITY_DEFAULT, 0x40 },
		[AVE_OP_STOP] = {		/* fw 0xda0c; kext 0xfffffe0008eaa724 */
			AVE135_CMD_UNINIT, AVE135_REPLY_UNINIT_DONE, 0x40,
			7, AVE_PRIORITY_DEFAULT, 0x40 },
		[AVE_OP_PROCESS_AVC] = {	/* fw 0xda9c; kext memset 0xfffffe0008eac878 */
			AVE135_CMD_AVC_ENCODE, AVE135_REPLY_ENCODE_DONE, 0x1940,
			AVE_SLOT_CALLER, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_PROCESS_HEVC] = {	/* fw 0xdb30; kext 0xfffffe0008eaccec */
			AVE135_CMD_HEVC_ENCODE, AVE135_REPLY_ENCODE_DONE, 0x6838,
			AVE_SLOT_CALLER, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_COMPLETE] = {		/* fw 0xdd78; kext slot 9 0xfffffe0008ead1e0 */
			AVE135_CMD_COMPLETE, AVE135_REPLY_COMPLETE_DONE, 0x40,
			9, AVE_PRIORITY_DEFAULT, 0x40 },
		/* AVE_OP_PRIORITY: no such command on 13.5 (docs/46 §1.2) */
		[AVE_OP_FLUSH] = {		/* fw 0xdc58; kext slot 10 0xfffffe0008ead5cc */
			AVE135_CMD_FLUSH, AVE135_REPLY_FLUSH_DONE, 0x40,
			10, AVE_PRIORITY_DEFAULT, 0x40 },
		[AVE_OP_RESET] = {		/* fw 0xd824; kext slot 11 0xfffffe0008ead9d0 */
			AVE135_CMD_RESET, AVE135_REPLY_RESET_DONE, 0x32db0,
			11, AVE_PRIORITY_DEFAULT, 0x40 },
	},
	.hdr = {
		.count		= 0x08,	/* kext str x20,[x23,#8] 0xfffffe0008ea9168 */
		.client_id	= 0x10,	/* fw ldr w1,[x20,#16] 0xe8e0 */
		.client_id_bytes = 4,	/* RegisterClient(unsigned int) 0x17738 */
		.work_type	= AVE_OFF_NONE,	/* +0x14 host client[212], unread */
		.codec		= 0x18,	/* fw cbz w22 0x13e60 */
		.slot		= 0x1c,	/* kext stur d0,[x23,#28] 0xfffffe0008ea9188 */
		.priority	= 0x20,	/* fw ldr w2,[x21,#32] 0xee7c */
		.timeout	= 0x28,	/* kext stur q0,[x23,#40] 0xfffffe0008ea9190 */
		.codec_avc	= 0,	/* kext AVE_Cmd2FwCmd 0xfffffe0008ed6cd0 */
		.codec_hevc	= 1,
		.max_slot	= 41,	/* Process builder cmp w24,#0x28
					 * 0xfffffe0008eac860 (slot <= 40) */
	},
	.reply = {			/* NotificationToHost 0x13684 */
		.id		= 0x00,	/* strh w21,[x22] 0x13708 */
		.client_id	= 0x10,	/* str w20,[x22,#16] 0x136f8 */
		.client_id_bytes = 4,
		.slot		= 0x1c,	/* 0x1370c; for UNINIT/FLUSH/COMPLETE_DONE
					 * overwritten with cmd+0x20 (0x13710-0x13734) */
		.status		= 0x38,	/* str w23,[x22,#56] 0x136fc */
		.status_ok	= AVE135_STATUS_OK,
	},
	.config = {			/* ProcessConfig 0xe4d4, x23 = cmd */
		.skip_mcpu	= 0x40,	/* ldrb w9,[x23,#64] 0xe550 */
		.create_mcpu	= 0x41,	/* ldrb w8,[x23,#65] 0xe54c */
		.reg_dart_addr	= 0x48,	/* ldr x10,[x23,#72] 0xe554 */
		.doorbell_cadence0 = 0x50, /* ldr w1,[x23,#80] 0xe574 */
		.doorbell_cadence1 = 0x54, /* [x23,#84] 0xe584 */
		.dsid		= 0x58,	/* ldrb [x23,#88] 0xe560 */
		.dsid2		= 0x59,	/* ldrb [x23,#89] 0xe564 */
		.dsid_bytes	= 1,
		.shmem_addr	= 0x60,	/* ldr x1,[x23,#96] 0xe5dc */
		.shmem_size	= 0x68,	/* ldr w2,[x23,#104] 0xe5e0 */
		.shmem_min	= 4 * (2744 + 2048 + 4), /* carve 0xe5ec-0xe6fc, INFERRED */
	},
	.start_avc = {
		.fw_client_addr	    = 0x40,	/* kext 0xfffffe0008ea995c; fw 0xee2c */
		.fw_client_size	    = 0x48,	/* kext 0xfffffe0008ea9968; fw 0xee40 */
		.fw_client_mem_addr = 0x50,	/* kext 0xfffffe0008ea9980; fw 0x463f0 */
		.fw_client_mem_size = 0x58,	/* kext 0xfffffe0008ea998c */
		/* AVE_VIDEO_PARAMS at cmd+0x60 (fw add x10,x8,#0x60 0x463e8) */
		.width		= 0x60,		/* fw ldr x8,[x20] 0x5ced0 */
		.height		= 0x64,
		/* AVE_FW_RC_PARAMS at cmd+0xff30 (x10 = payload+0xfed0, 0x5cdf0) */
		.frame_rate	= 0xff4c,	/* fw 0x5d9c4, assert 0x5ef00 */
		.bitrate	= 0xff30,	/* fw 0x5d9b4 */
		.param_sets_addr = 0xfb30,	/* fw ldr x20,[x23,#880] 0x5df28; kext VP+0xFAD0 0xfffffe0008eaee10 */
		.param_sets_size = 0xfb38,	/* fw ldr w2,[x23,#888] 0x5de44 */
		.rc_mode	= 0xff50,	/* ui32RCFlag, ldr w8,[x10,#32] 0x5ceb4 */
		.rc_mode_fixed_qp = 2,		/* AVE_RC_FIXQP: cmp w10,#0x2 0x41158,
						 * string 0x4e69c */
		.rc_feature	= AVE_OFF_NONE,	/* no RC framework switch on 13.5 */
		.rc_feature_fixed_qp = 0,
		.qp_i		= 0xffb4,	/* ldp w9,w8,[x10,#132] 0x5cebc */
		.qp_p		= 0xffb8,
		.qp_b		= 0xffbc,	/* ldr w8,[x10,#140] 0x5cec8 */
		.qp_min		= 0xff88,	/* fw 0x5d9f0 (kept if < 51), name INFERRED */
		.qp_max		= 0xff8c,	/* fw 0x5da00 (1..51), name INFERRED */
		.key_interval	= 0xff34,	/* ui32IdrPeriod, fw 0x5d9e8 */
		.key_interval_strict = AVE_OFF_NONE,
		.slice_num	= 0xfdac,	/* 0x104 sSliceMap copy fw 0x14414;
						 * iNum at +0 INFERRED */
		.recon_set	= 0x88,		/* fw ldr x8,[x20,#40] 0x5d6ac (x20 = cmd+0x60) */
		.recon_stride	= 0x10,		/* fw loads +40,+56..+280, 0x5d6ac-0x5d728 */
		.recon_max	= 16,		/* the fw copy loop reads 16; host writes
						 * 2 x 17 (kext 0xfffffe0008eaef04-58) */
		.recon_addr	= 0x00,
		.recon_size	= AVE_OFF_NONE,	/* entry is {u64, u64}, not {addr,size} */
		.recon_meta_addr = AVE_OFF_NONE, /* +8: second-plane addr, 0 on the
						 * linear arm (kext stp x8,xzr 0xfffffe0008eae710);
						 * not read by the fw loop */
		.coded_max	= 20,		/* kext cmp w1,#0x14 0xfffffe0008ea4ce8 */
		.coded_addr	= 0x4b8,	/* fw ldr x8,[x20,#1112] 0x5d4c8 */
		.coded_addr_stride = 8,
		.coded_size	= 0x558,	/* fw ldr w8,[x20,#1272] 0x5d4d4 */
		.coded_size_stride = 4,
		.coded_hdr_addr	= 0x5c0,	/* fw ldr x8,[x20,#1376] 0x5d4dc */
		.coded_hdr_addr_stride = 8,
		.coded_hdr_size	= 0x660,	/* kext str w0,[x8,#1536] 0xfffffe0008eaeed4;
						 * no fw read in that loop */
		.coded_hdr_size_stride = 4,
		.coded_hdr_bytes = 0x23000,	/* kext 0xfffffe0008ea4fb8 */
		.sps_block	= 0x105b0,	/* memcpy 0x6ac from payload+0x10550 0x5ce68-90 */
		.sps_block_size	= 0x6ac,
		.pps_block	= 0x10c5c,	/* memcpy 0x184 from payload+0x10bfc 0x5ce94-ac */
		.pps_block_size	= 0x184,
	},
	.sps = {	/* AVC_SPS::seq_parameter_set_rbsp, x8 = sps_block */
		.enum_profile_level		= false,	/* raw idc, u(8) */
		.profile			= 0x105b4,	/* [x8,#4]    0x194e4 */
		.level				= 0x105d0,	/* [x8,#32]   0x19564 */
		.seq_parameter_set_id		= 0x105d4,	/* [x8,#36]   0x19570 */
		.chroma_format_idc		= 0x105d8,	/* [x8,#40]   0x195ac */
		.bit_depth_luma_minus8		= 0x105e0,	/* [x8,#48]   0x195dc */
		.bit_depth_chroma_minus8	= 0x105e4,	/* [x8,#52]   0x195e8 */
		.log2_max_frame_num_minus4	= 0x109cc,	/* [x8,#1052] 0x197c4 */
		.pic_order_cnt_type		= 0x109d0,	/* [x8,#1056] 0x197d0 */
		.max_num_ref_frames		= 0x109dc,	/* [x8,#1068] 0x19860 */
		.gaps_in_frame_num_allowed	= 0x109e0,	/* ldrb #1072 0x19870 */
		.pic_width_in_mbs_minus1	= 0x109e4,	/* [x8,#1076] 0x1987c */
		.pic_height_in_map_units_minus1	= 0x109e8,	/* [x8,#1080] 0x19888 */
		.frame_mbs_only_flag		= 0x109ec,	/* [x8,#1084] 0x19898 */
		.direct_8x8_inference_flag	= 0x109f0,	/* [x8,#1088] 0x198c4 */
		.vui_parameters_present_flag	= 0x109f4,	/* ldrb #1092 0x19924 */
		.frame_cropping_flag		= 0x10a40,	/* ldrb #1168 0x198d8 */
		.crop_left			= 0x10a44,	/* [x8,#1172] 0x198f0 */
		.crop_right			= 0x10a48,	/* [x8,#1176] 0x198fc */
		.crop_top			= 0x10a4c,	/* [x8,#1180] 0x19908 */
		.crop_bottom			= 0x10a50,	/* [x8,#1184] 0x19914 */
		.fw_creates_header		= 0x10a54,	/* ldrb [x24,#1192] 0x5dd64,
								 * x24 = SPS copy - 4 (0x5caac) */
		.header_len			= 0x10a58,	/* fw str [x8,#1192] 0x19974 */
	},
	.pps = {	/* AVC_PPS::pic_parameter_set_rbsp, x8 = pps_block */
		.pic_parameter_set_id		= 0x10c60,	/* [x8,#4]  0x199c4 */
		.seq_parameter_set_id		= 0x10c64,	/* [x8,#8]  0x199d0 */
		.entropy_coding_mode_flag	= 0x10c68,	/* [x8,#12] 0x199e0 */
		.num_slice_groups_minus1	= 0x10c70,	/* [x8,#20] 0x199fc */
		.pic_init_qp_minus26		= 0x10c88,	/* [x8,#44] 0x19b10 */
		.deblocking_filter_control	= 0x10cb0,	/* ldrb #84 0x19b38 */
		.constrained_intra_pred		= 0x10cb1,	/* ldrb #85 0x19b48 */
		.transform_8x8_mode		= 0x10cb3,	/* ldrb #87 0x19b64 */
		.fw_creates_header		= 0x10cd8,	/* ldrb [x24,#1836] 0x5dd68 */
	},
	.process_avc = {
		.picmgmt	= 0x9c8,	/* kext add x0,x23,#0x9c8 0xfffffe0008eac9bc;
						 * fw add x27,x21,#0x9c8 0x145c8 */
		.picmgmt_size	= 0xf68,	/* kext mov w2,#0xf68 0xfffffe0008eac9c4 */
		.picmgmt_size_word = true,	/* kext str w8,[x25] 0xfffffe0008eac9a0 */
		.frame_num	= AVE_OFF_NONE,	/* no host field; fw SetFrameNum 0x52a84 */
		.poc		= AVE_OFF_NONE,	/* no host field (docs/47 §1.2) */
		.frame_rate_f64	= AVE_OFF_NONE,	/* not located */
		.frame_type	= 0xcac,	/* kext str w8,[x20,#3244] 0xfffffe0008eaaa50 */
		.input_compressed = 0x6f3,	/* kext strb w8,[x19,#1779] 0xfffffe0008eabb98 */
		.in_luma_addr	= 0x8c0,	/* kext str x10,[x20,#2240] 0xfffffe0008eb0904 */
		.in_luma_size	= AVE_OFF_NONE,	/* 13.5 host writes no plane size */
		.in_luma_stride	= 0x8c8,	/* kext 0xfffffe0008eb0908 */
		.in_chroma_addr	= 0x8d0,	/* kext 0xfffffe0008eb0910 */
		.in_chroma_size	= AVE_OFF_NONE,
		.in_chroma_stride = 0x8d8,	/* kext 0xfffffe0008eb0914 */
		.out_mode	= 0xc00,	/* kext strb wzr,[x3,#3072] 0xfffffe0008eb051c */
		.out_index	= 0xc04,	/* kext str w26,[x3,#3076] 0xfffffe0008eb0520 */
		.out_coded	= 0xc08,	/* kext 0xfffffe0008eb0548; fw 0x58384 */
		.out_coded_hdr	= 0xc10,	/* kext 0xfffffe0008eb05a0 */
		.out_coded_size	= 0xc18,	/* kext 0xfffffe0008eb0554; fw 0x58364 */
		.recon_y	= 0x898,	/* fw dumper 0x3c0c4; setRefPointers 0x2c338 */
		.recon_uv	= 0x8a8,	/* fw dumper 0x3c0d0 */
		.recon_mv	= 0x8b8,	/* fw setRefPointers 0x2c4b0 */
	},
};

/* ------------------------------------------------------------------------
 * macOS 26.6.2 (25G83) - AppleAVE2FW-9003.78.0. Firmware VAs are 26.6.2.
 * docs/07, 20, 21, 32, 35, 37, 38, 39 and the macros above.
 * ------------------------------------------------------------------------ */
const struct ave_cmd_abi ave_cmd_abi_26_6 = {
	.abi	= AVE_ABI_MACOS_26_6,
	.name	= "macOS 26.6.2",
	.cmd = {
		/* reply ids = command ids, 0x48 message (0x50 for Process):
		 * NotificationToHost 0x311a8, mask 0x1eda at 0x3129c,
		 * Process arm 0x314e0-0x3151c (read for this table).
		 * Slot/prio: docs/07 §4. */
		[AVE_OP_CONFIG]	= { AVE_CMD_CONFIG, AVE_CMD_CONFIG, AVE_CMDSZ_CONFIG,
				    AVE_SLOT_GLOBAL, 0, 0x48 },
		[AVE_OP_HALT]	= { AVE_CMD_HALT, 0, AVE_CMDSZ_COMMON,
				    AVE_SLOT_GLOBAL, 0, 0 },	/* id 2 not in 0x1eda */
		[AVE_OP_OPEN]	= { AVE_CMD_OPEN, AVE_CMD_OPEN, AVE_CMDSZ_COMMON,
				    3, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_CLOSE]	= { AVE_CMD_CLOSE, AVE_CMD_CLOSE, AVE_CMDSZ_COMMON,
				    4, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_START_AVC] = { AVE_CMD_START, AVE_CMD_START, AVE_CMDSZ_AVC_START,
				    6, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_START_HEVC] = { AVE_CMD_START, AVE_CMD_START, AVE_CMDSZ_HEVC_START,
				    6, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_STOP]	= { AVE_CMD_STOP, AVE_CMD_STOP, AVE_CMDSZ_COMMON,
				    7, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_PROCESS_AVC] = { AVE_CMD_PROCESS, AVE_CMD_PROCESS, AVE_CMDSZ_AVC_PROCESS,
				    AVE_SLOT_CALLER, AVE_PRIORITY_DEFAULT, 0x50 },
		[AVE_OP_PROCESS_HEVC] = { AVE_CMD_PROCESS, AVE_CMD_PROCESS, AVE_CMDSZ_HEVC_PROCESS,
				    AVE_SLOT_CALLER, AVE_PRIORITY_DEFAULT, 0x50 },
		[AVE_OP_COMPLETE] = { AVE_CMD_COMPLETE, AVE_CMD_COMPLETE, AVE_CMDSZ_COMMON,
				    AVE_CMD_COMPLETE, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_PRIORITY] = { AVE_CMD_PRIORITY, AVE_CMD_PRIORITY, AVE_CMDSZ_COMMON,
				    AVE_CMD_PRIORITY, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_FLUSH]	= { AVE_CMD_FLUSH, AVE_CMD_FLUSH, AVE_CMDSZ_COMMON,
				    AVE_CMD_FLUSH, AVE_PRIORITY_DEFAULT, 0x48 },
		[AVE_OP_RESET]	= { AVE_CMD_RESET, AVE_CMD_RESET, AVE_CMDSZ_RESET,
				    AVE_CMD_RESET, AVE_PRIORITY_DEFAULT, 0x48 },
	},
	.hdr = {		/* struct ave_cmd_hdr, docs/07 §4 */
		.count		= 0x08,
		.client_id	= 0x10,
		.client_id_bytes = 8,
		.work_type	= 0x18,
		.codec		= 0x1c,
		.slot		= 0x20,
		.priority	= 0x24,
		.timeout	= 0x30,
		.codec_avc	= AVE_ENC_AVC,
		.codec_hevc	= AVE_ENC_HEVC,
		.max_slot	= AVE_MAX_INFLIGHT,
	},
	.reply = {		/* fw strh [x27] / str x19,[x27,#16] / str w23,[x27,#32]
				 * / str w22,[x27,#64] at 0x312c8-0x312d4 */
		.id		= 0x00,
		.client_id	= 0x10,
		.client_id_bytes = 8,
		.slot		= 0x20,
		.status		= 0x40,
		.status_ok	= 0,	/* docs/20 §1 (Open success status 0) */
	},
	.config = {		/* docs/20 §2 */
		.skip_mcpu	= 0x48,
		.create_mcpu	= 0x49,
		.reg_dart_addr	= AVE_OFF_NONE,	/* +0x50 logged only, never read */
		.doorbell_cadence0 = 0x58,
		.doorbell_cadence1 = 0x5c,
		.dsid		= 0x60,
		.dsid2		= AVE_OFF_NONE,
		.dsid_bytes	= 4,
		.shmem_addr	= 0x68,
		.shmem_size	= 0x70,
		.shmem_min	= 4 * (1176 + 2048 + 4),	/* docs/20 §2, INFERRED */
	},
	.start_avc = {
		.fw_client_addr	    = AVE_START_FWCLIENT_ADDR,
		.fw_client_size	    = AVE_START_FWCLIENT_SIZE,
		.fw_client_mem_addr = AVE_START_FWCLIENTMEM_ADDR,
		.fw_client_mem_size = AVE_START_FWCLIENTMEM_SIZE,
		.width		= AVE_START_WIDTH,
		.height		= AVE_START_HEIGHT,
		.frame_rate	= AVE_START_FRAMERATE,
		.bitrate	= AVE_START_BITRATE,
		.rc_mode	= AVE_START_RCMODE,
		.rc_mode_fixed_qp = AVE_RC_MODE_CONST_QP,	/* docs/35 §5 */
		.rc_feature	= AVE_START_RC_FEATURE,
		.rc_feature_fixed_qp = AVE_RC_FEATURE_NEW_FRAMEWORK, /* docs/35 §9 */
		.qp_i		= AVE_START_QP_I,
		.qp_p		= AVE_START_QP_P,
		.qp_b		= AVE_START_QP_B,
		.qp_min		= AVE_START_QP_MIN,
		.qp_max		= AVE_START_QP_MAX,
		.key_interval	= AVE_START_MAX_GOP,
		.key_interval_strict = AVE_START_STRICT_GOP,
		.slice_num	= AVE_START_SLICE_MAP_NUM,
		.recon_set	= AVE_START_RECON_SET,
		.recon_stride	= AVE_START_RECON_STRIDE,
		.recon_max	= 16,		/* docs/37 §1 saRecon[0..15] */
		.recon_addr	= 0x00,		/* docs/21 §3.2 _S_AVE_DPBBuf */
		.recon_size	= 0x08,
		.recon_meta_addr = 0x10,
		.coded_max	= AVE_CODED_MAX_BUFS,
		.coded_addr	= AVE_START_CODED_DATA_SET,
		.coded_addr_stride = AVE_START_BUF_STRIDE,
		.coded_size	= AVE_START_CODED_DATA_SET + 8,
		.coded_size_stride = AVE_START_BUF_STRIDE,
		.coded_hdr_addr	= AVE_START_CODED_HDR_SET,
		.coded_hdr_addr_stride = AVE_START_BUF_STRIDE,
		.coded_hdr_size	= AVE_START_CODED_HDR_SET + 8,
		.coded_hdr_size_stride = AVE_START_BUF_STRIDE,
		.coded_hdr_bytes = AVE_CODED_HEADER_SIZE,
		.sps_block	= AVE_START_SPS_OFF,
		.sps_block_size	= AVE_START_SPS_SIZE,
		.pps_block	= AVE_START_PPS_OFF,
		.pps_block_size	= AVE_START_PPS_SIZE,
	},
	.sps = {		/* docs/37 §3 */
		.enum_profile_level		= true,
		.profile			= AVE_SPS_PROFILE,
		.level				= AVE_SPS_LEVEL,
		.seq_parameter_set_id		= AVE_SPS_SEQ_PARAM_SET_ID,
		.chroma_format_idc		= AVE_SPS_CHROMA_FORMAT_IDC,
		.bit_depth_luma_minus8		= AVE_SPS_BIT_DEPTH_LUMA_M8,
		.bit_depth_chroma_minus8	= AVE_SPS_BIT_DEPTH_CHROMA_M8,
		.log2_max_frame_num_minus4	= AVE_SPS_LOG2_MAX_FRAME_NUM_M4,
		.pic_order_cnt_type		= AVE_SPS_POC_TYPE,
		.max_num_ref_frames		= AVE_START_MAX_REF,
		.gaps_in_frame_num_allowed	= AVE_SPS_GAPS_IN_FRAME_NUM,
		.pic_width_in_mbs_minus1	= AVE_SPS_PIC_WIDTH_MBS_M1,
		.pic_height_in_map_units_minus1	= AVE_SPS_PIC_HEIGHT_MAPU_M1,
		.frame_mbs_only_flag		= AVE_SPS_FRAME_MBS_ONLY,
		.direct_8x8_inference_flag	= AVE_SPS_DIRECT_8X8_INFERENCE,
		.vui_parameters_present_flag	= AVE_SPS_VUI_PRESENT,
		.frame_cropping_flag		= AVE_SPS_FRAME_CROPPING,
		.crop_left			= AVE_SPS_CROP_LEFT,
		.crop_right			= AVE_SPS_CROP_RIGHT,
		.crop_top			= AVE_SPS_CROP_TOP,
		.crop_bottom			= AVE_SPS_CROP_BOTTOM,
		.fw_creates_header		= AVE_SPS_FW_CREATES_HDR,
		.header_len			= AVE_SPS_HEADER_LEN,
	},
	.pps = {		/* docs/37 §4 */
		.pic_parameter_set_id		= AVE_PPS_PIC_PARAM_SET_ID,
		.seq_parameter_set_id		= AVE_PPS_SEQ_PARAM_SET_ID,
		.entropy_coding_mode_flag	= AVE_PPS_ENTROPY_CODING_MODE,
		.num_slice_groups_minus1	= AVE_PPS_NUM_SLICE_GROUPS_M1,
		.pic_init_qp_minus26		= AVE_PPS_PIC_INIT_QP_M26,
		.deblocking_filter_control	= AVE_PPS_DEBLOCKING_CTRL,
		.constrained_intra_pred		= AVE_PPS_CONSTRAINED_INTRA,
		.transform_8x8_mode		= AVE_PPS_TRANSFORM_8X8,
		.fw_creates_header		= AVE_PPS_FW_CREATES_HDR,
	},
	.process_avc = {	/* docs/32 §1, §6; docs/39 §1.2 */
		.picmgmt	= AVE_PICMGMT_OFF,
		.picmgmt_size	= AVE_PICMGMT_SIZE,
		.picmgmt_size_word = false,
		.frame_num	= AVE_PIC_FRAME_NUM,
		.poc		= AVE_PIC_POC,
		.frame_rate_f64	= AVE_PIC_FRAME_RATE,
		.frame_type	= AVE_PIC_FRAME_TYPE,
		.input_compressed = AVE_PIC_INPUT_COMPRESSED,
		.in_luma_addr	= AVE_PIC_IN_LUMA_ADDR,
		.in_luma_size	= AVE_PIC_IN_LUMA_SIZE,
		.in_luma_stride	= AVE_PIC_IN_LUMA_STRIDE,
		.in_chroma_addr	= AVE_PIC_IN_CHROMA_ADDR,
		.in_chroma_size	= AVE_PIC_IN_CHROMA_SIZE,
		.in_chroma_stride = AVE_PIC_IN_CHROMA_STRIDE,
		.out_mode	= AVE_PIC_OUT_MODE,
		.out_index	= AVE_PIC_OUT_INDEX,
		.out_coded	= AVE_PIC_OUT_CODED,
		.out_coded_hdr	= AVE_PIC_OUT_CODED_HDR,
		.out_coded_size	= AVE_PIC_OUT_CODED_SIZE,
		.recon_y	= AVE_PIC_RECON_Y_MSB,
		.recon_uv	= AVE_PIC_RECON_UV_MSB,
		.recon_mv	= AVE_PIC_RECON_MV,
	},
};
#endif /* AVE_CMD_ABI_DEFINE_TABLES */

#endif /* __AVE_ABI_H__ */
