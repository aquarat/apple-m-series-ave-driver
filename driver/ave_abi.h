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
 * The two an encode can fail with, which we were reporting as a bare -EIO:
 * 0xEE0003 the parameter-sets buffer is too small for the SPS+PPS the
 * firmware wants to write (fw 0x5de44 - vacuous on the first Start_AVC of a
 * session, live on the second), 0xEE0004 the bitstream overflowed the coded
 * buffer (fw 0x5bf88, after checking [8312]+[8316]+[8356] <= [8360]), and
 * 0xEE0007 a hardware transcode error (fw 0x5be88). docs/67 §5.
 */
#define AVE135_STATUS_PSETS_SMALL	0x00ee0003
#define AVE135_STATUS_CODED_OVERFLOW	0x00ee0004
#define AVE135_STATUS_TRANSCODE_ERR	0x00ee0007

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
/* LowResResult surfaces a session publishes: 4 for DevType 12 (docs/65 §Q4). */
#define AVE_LOW_RES_RESULT_MAX	4
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

/*
 * IMG_FRAME_TYPE. Values 0 and 3 are the same on both versions (13.5
 * AVE_H264_PrepareSliceHeader 0x20ff0 / 0x210d4, docs/47 §1.3); the rest of
 * the 13.5 enum comes from the jump table at fw 0xcef80, base 0x20e38, which
 * sets nal_unit_type at [x19+8], slice_type at [x19+16] and IdrPicFlag at
 * [x19+34]. 4, 5 and 6 are rejected with a log rather than an assert.
 * docs/64 §1.3.
 */
#define AVE_FRAME_TYPE_I		0
#define AVE_FRAME_TYPE_P		1
#define AVE_FRAME_TYPE_B		2
#define AVE_FRAME_TYPE_IDR		3
/*
 * Only ever seen coming BACK, in CODED_DATA_HDR.FrameTypeReturned: the
 * firmware dropped the frame (written at fw 0x13340 and 0x5c930, tested at
 * 0x14bc0). docs/67 §5.
 */
#define AVE135_FRAME_TYPE_DROPPED	4

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
/*
 * Source-neighbour scratch tables: four groups (Info, Pixel, Data, FwData) of
 * four IOVAs each, published both at Start_AVC and per frame. The 13.5 kext
 * writes exactly four entries per group in AVE_CHM_SetFwBuf
 * (0xfffffe0008eaf174-0xfffffe0008eaf210, "cmp x27,#0x4") and in
 * AVE_CHM_SetDataInfo_FwBuf (0xfffffe0008eb0be8-0xfffffe0008eb0cb4).
 */
#define AVE_SRC_NBR_GROUPS		4
#define AVE_SRC_NBR_MAX			4

/*
 * Rows of encoder_addr_entropy[][] the host fills. The wire table is [16][4];
 * the firmware copies and asserts only the first ctrl+3768 rows, which our
 * arm sets to 4 (docs/54).
 */
#define AVE_ENTROPY_MAX			16
/* Columns of encoder_addr_entropy the kext fills (0xfffffe0008eb0cb8). */
#define AVE_ENTROPY_COLS		4
/* Loose per-frame scratch IOVAs (13.5 PICMGMT 0x8E0/0x8E8/0x8F0/0x900). */
#define AVE_PIC_SCRATCH_MAX		4
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
	/*
	 * ui32RCFlag's other legal values, and the fields the firmware's own
	 * rate controller reads once one of them is selected (docs/66 §1).
	 * 13.5: {0 OFF, 1 ON, 2 FIXQP}; >2 takes the error branch, so 3 is
	 * NOT "another fixed QP" - ProcessInit sends it down the bitrate arm
	 * (fw 0x403a0).
	 *
	 * ui32BitRate (the existing .bitrate, wire 0xFF30) is in BITS PER
	 * SECOND: ProcessInit computes bitrate / framerate / (W*H) as
	 * bits-per-pixel (fw 0x401b8-0x401cc). The frame rate it divides by is
	 * frame_rate / frame_rate_div.
	 *
	 * There is no HRD/VBV on 13.5 - the 26.6.2 kext has VBV, DecideLevel,
	 * MaxBitRate and CheckResolution symbols and 13.5 has none of them.
	 * The only leaky-bucket-shaped thing is a 40-byte _S_AVE_DRL_Cfg block
	 * gated by drl_enable, whose layout is [U]; we leave both at zero.
	 */
	u32	rc_mode_on;		/* wire value for "firmware RC"; NONE = absent */
	u32	frame_rate_div;		/* u32; frame rate is frame_rate/this */
	u32	bitrate_sel;		/* u32; 2 selects bitrate_alt instead */
	u32	bitrate_alt;		/* u32 bits/s */
	u32	drl_enable;		/* u8; gates the DRL config block */
	u32	drl_cfg;		/* 40-byte block, layout UNKNOWN */
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
	/*
	 * sSVEMap.iNum - how many SVE cores the session spans. We run one.
	 *
	 * 0 and 1 both take the single-core arm, so this is not what stops a
	 * frame; writing 1 closes CollectDataFromCpus:8657 (asserts
	 * sSVEMap.iNum == 1), which is otherwise reached only via a flag that
	 * is *inferred* zero rather than confirmed (docs/54). Cheap insurance
	 * against a late, quiet failure.
	 *
	 * Also decides whether iFwClientMemAddr is dereferenced at all: the
	 * map and carve are behind iNum > 1 (fw 0x5cb50), so with 1 the
	 * firmware never touches that buffer.
	 */
	u32	sve_num;		/* u32; AVE_OFF_NONE = not located */
	/* u8 NEED_LSB_PLANES and the recon entry's LSB-plane u64 (docs/57). */
	u32	need_lsb_planes;	/* AVE_OFF_NONE = not located */
	u32	recon_lsb_addr;		/* offset inside a recon entry */
	/* DPB (recon) table: recon_max entries, recon_stride apart */
	u32	recon_set;
	u32	recon_stride;
	u32	recon_max;
	u32	recon_addr;		/* u64, offset inside an entry */
	u32	recon_size;		/* u32 luma bytes, inside entry; NONE = absent */
	u32	recon_meta_addr;	/* u64 second-plane addr, inside entry */
	/*
	 * Low-resolution (LRME) reference table - Apple's LowResRef surfaces.
	 * ONE u64 IOVA PER DPB SLOT, same slot index as recon_set, and this is
	 * what actually reaches sLowResOutput.LowResSrcLumaScaled: the per-frame
	 * PICMGMT field is overwritten before setLRME reads it. The chain, all
	 * 13.5 image VAs (docs/53 s13):
	 *
	 *   host  AVE_CHM_SetFwBuf    kext 0xfffffe0008eaefcc / 0xeaf004
	 *         -> AVE_VIDEO_PARAMS + 0x248 + set*0x88 + slot*8
	 *            (= command wire 0x2A8, since VideoParams sits at cmd+0x60)
	 *   fw    CAVECommonDPB::ProvideReferenceFrames 0x2b780 / 0x2b788
	 *         -> DPBContext + 0x10A0 + set*0x80 + slot*8
	 *   fw    CAVECommonDPB::InitPointerAndVariables 0x2bddc / 0x2bde8
	 *         -> DPB entry + 64        (entry = ctx + 0x680 + slot*0x50)
	 *   fw    H264VideoEncoderDPB::ManageDPBBuffer 0x2d544 / 0x2d55c
	 *         -> ReferenceFrameInfoData + 224
	 *   fw    CAVECommonDPB::setRefPointers 0x2c318 / 0x2c320
	 *         -> AVE_PICMGMT_PARAMS + 0xC20
	 *   fw    CAVCController::setLRME 0x523bc, assert 5782 / 5783
	 *
	 * How many entries the firmware reads: ProvideReferenceFrames is called
	 * with numRefs = SPS max_num_ref_frames (fw ldr w1,[x24,#1072] 0x5dd14)
	 * and copies slots 0..numRefs inclusive, for sets 0..[dpb+32]; the
	 * H264VideoEncoderDPB constructor sets [dpb+32] = 1 (fw 0x2d1c4), so
	 * only set 0 is used and exactly max_num_ref_frames+1 slots must be
	 * valid. Apple allocates the same count: AVE_CalcBufNumOfLowResRef
	 * (kext 0xfffffe0008ea55d8) returns n+1, and AVE_CreateInternalSurfaces
	 * (0xfffffe0008f3a414) creates one surface per slot at SurfaceSet+0x9A8.
	 */
	u32	low_res_ref_set;	/* u64[] per DPB slot; NONE = not located */
	u32	low_res_ref_stride;	/* bytes between slots */
	u32	low_res_ref_max;	/* slots in one set */
	/*
	 * sLowResOutput.LowResResults[] - the low-resolution motion search's
	 * OUTPUT surfaces, session-wide rather than per DPB slot. Not the same
	 * table as LowResRef above, and the one nobody has ever filled.
	 *
	 * Dormant on an I-frame and live from the first P: the readers
	 * (0x40D120F80 + 0x40i, RDDMAMESFSRSLTS, from
	 * sLowResOutput.LowResResults[i]) are bounded by
	 * num_ref_idx_l0_active_minus1, which is -1 with no references. From
	 * the first P frame setPipe asserts on them:
	 *   "pPicParams->sLowResOutput.LowResResults[me_ref_index] != 0"
	 *   CAVCController_H13C.cpp:6184
	 * The per-frame PICMGMT field (+0xC28) is inert as usual -
	 * setRefPointers rewrites it - so this Start-time table is the real
	 * interface, exactly like the entropy table in docs/61 §10.
	 *
	 * Size per buffer: ALIGN(4*W, 128) * ceil(H/64) + 1024, 64-byte
	 * aligned. Count 4 for DevType 12. docs/65 §Q4.
	 */
	u32	low_res_result_set;
	u32	low_res_result_stride;
	u32	low_res_result_max;
	/*
	 * Colocated MV table, one u64 per DPB slot, same slot order as recon.
	 * docs/60: left zero, setRefPointers (fw 0x2c4b0) copies the zero over
	 * the per-frame field and setPipe (cbz 0x554f0) writes the pipe's
	 * colocated-MV writer 0x40D130380 = 0 - disabled - which is the only
	 * write channel off for us and on under macOS, where the kext fills this
	 * for every Colocated surface (0xfffffe0008eaef74..efb8). docs/53 13.2
	 * located the table: VideoParams +0xF650 = wire 0xF6B0, 2 x 17 x 8.
	 */
	u32	colocated_set;		/* u64[] per DPB slot; NONE = not located */
	u32	colocated_stride;
	u32	colocated_max;
	/*
	 * encoder_addr_entropy[16][4], the SEB/entropy write buffers - a
	 * START-time table, not a per-frame one (docs/61 10). Every frame
	 * CAVECommonDPB::setRefPointers copies PICMGMT +0x980..+0xBF8 out of the
	 * DPB record (fw 0x2c98c..0x2ca4c), so a per-frame write here is
	 * overwritten before setPipe reads it; the record itself is filled at
	 * Start_AVC by ProvideReferenceFrames from VP+0xF770 + 96 + 8n
	 * (fw 0x2ba98..0x2bc8c) = wire 0xF830. Left unpublished, the four SEB
	 * drain channels (0x40D1303C0 + 0x40k) come up with address 0 and the
	 * syntax-element buffer fills: "Cveseb buffer write full!" (F12, F13).
	 */
	u32	entropy_set;
	u32	entropy_stride_i;
	u32	entropy_stride_j;
	u32	entropy_max;		/* rows setPipe reads */
	u32	entropy_cols_max;
	/*
	 * The per-buffer SIZE table that goes with entropy_set. CONFIRMED from
	 * both sides (docs/62 0; first inferred from the 0x200 gap, which turned
	 * out right):
	 *
	 *   kext AVE_CHM_SetFwBuf stores AVE_Surface::GetSize() to
	 *     VP+0xF9D0 + 0x10*i + 4*j = wire 0xFA30 (str w0,[x8,#512]
	 *     0xfffffe0008eaf2ec), and refuses to send the command if either the
	 *     IOVA or the size is zero (0xeaf2f0);
	 *   AVE_Client_InitFwBuf memsets VP+0xF7D0 for 0x200 (u64[16][4]) and
	 *     VP+0xF9D0 for 0x100 (u32[16][4]) back to back (0xec8fe4/0xec8ff8);
	 *   firmware InitEncodingParameters copies VP+0xF9D0 -> ctrl+0x10C0 at
	 *     Start_AVC (fw 0x5d734..0x5d870), 4*sSVEMap.iNum rows x 4 columns -
	 *     so sve_num = 0 would silently skip the whole copy;
	 *   setPipe then reads size from ctrl+0x10C0 + 0x10*ch + 4*j into the
	 *     channel's +0x10 (fw 0x5595c).
	 *
	 * docs/61 2.4 concluded no instruction writes ctrl+0x10C0; that scan
	 * could not see stur with negative offsets off a pre-biased base, which
	 * is how the copy is written - the Trap 3 caveat docs/61 attached to it.
	 * AVE_OFF_NONE = not located.
	 */
	u32	entropy_size_set;
	u32	entropy_size_stride_i;
	u32	entropy_size_stride_j;
	/* coded data / coded header tables */
	u32	coded_max;
	u32	coded_addr, coded_addr_stride;		/* u64[] */
	u32	coded_size, coded_size_stride;		/* u32[] */
	u32	coded_hdr_addr, coded_hdr_addr_stride;	/* u64[] */
	u32	coded_hdr_size, coded_hdr_size_stride;	/* u32[] */
	u32	coded_hdr_bytes;	/* AVE_CalcBufSizeOfCodedHeader */
	/*
	 * Source-neighbour scratch tables (Apple: sExtraBuff.SrcNbr*). Four
	 * groups of src_nbr_max u64 IOVAs each. Start_AVC is accepted with
	 * these zero, but the first Process is NOT: CAVCController::setPipe
	 * asserts "EncCommParams.encoder_addr_src_nbr_info != 0" (fw 0x55618,
	 * line 6990) and "..._pixels != 0" (0x556b8, line 6993), and
	 * SetTranscode asserts "..._src_nbr_data != 0" (0x5869c, line 7928).
	 * InitEncodingParameters loads them from the wire at 0x5d874-0x5d8bc.
	 */
	u32	src_nbr_set[AVE_SRC_NBR_GROUPS];
	u32	src_nbr_max;		/* entries per group; 0 = no such table */
	/*
	 * The only two host-supplied AVE_VIDEO_PARAMS scalars that reach the
	 * source-read register block at 0x40D1120000, and we have been sending
	 * both as zero since the first encode (docs/62 §6).
	 *
	 * src_mode (u16) is split in two by CAVCController::setPipe:
	 *   0x40D1120050 = src_mode & 3    (fw 0x54334)
	 *   0x40D11200D0 = src_mode >> 2   (fw 0x547ec)
	 * reaching the firmware as [x22,#444] off ctrl+0x23FC4 (fw 0x5d018).
	 * The kext neither writes nor validates it - AVE_VIDEO_PARAMS is a
	 * byte-for-byte pass-through from user space (AVE_Client_Config,
	 * kext 0xecc1bc) - so the value is [U] and must be swept, not guessed.
	 *
	 * src_cfg_byte (u8) lands in bits 16+ of the source format word:
	 *   0x40D112000C = (src_cfg_byte << 16) | (fmt_code << 8)  (fw 0x54a08)
	 * with fmt_code from the chroma format (20 for our 8-bit 4:2:0).
	 * Firmware side [x22,#41] = ctrl+0x23FED (fw 0x5d118). Also [U].
	 */
	u32	src_mode;		/* u16; AVE_OFF_NONE = not located */
	u32	src_cfg_byte;		/* u8;  AVE_OFF_NONE = not located */
	/*
	 * The other two host bytes that reach the source path, both of which
	 * we have sent as zero since the first encode, and both traced by
	 * docs/69 to SRCDMAGO (0x40D110128):
	 *
	 *   src_go_bit3  -> SRCDMAGO bit 3   (fw 0x57d94)
	 *   src_go_bits  -> SRCDMAGO bits 4+ (fw 0x57da8 / 0x57db4)
	 *
	 * src_go_bit3 additionally gates whether ProcessPipeReset initialises
	 * a THIRD reader channel at 0x40D120100 (fw 0x4edf0) - the two we
	 * know about, luma at 0x40D120000 and chroma at +0x80, are both
	 * programmed and both run. docs/65 read the same byte as a
	 * sync/async LRME selector.
	 *
	 * Like src_mode, neither value is knowable from either binary: the
	 * kext passes AVE_VIDEO_PARAMS through from user space without
	 * writing or validating them.
	 */
	u32	src_go_bit3;		/* u8; AVE_OFF_NONE = not located */
	u32	src_go_bits;		/* u8; AVE_OFF_NONE = not located */
	/*
	 * The controller's debug-verbosity bitfield (docs/70). It reaches
	 * ctrl+0xA7C (fw 0x5cedc), and setPipe copies its BIT 5 into
	 * this+408 (fw 0x58550) - which is the single byte CController::Print
	 * tests before returning (fw 0x924d4: ldrb w8,[x0,#408]; cbz w8).
	 *
	 * So with this zero, as in every run we have ever done, the firmware
	 * drops all of its own "AVC COMMON::" diagnostics before they reach
	 * the TERMINAL ring the driver already drains. Setting bit 5 makes
	 * the firmware report its own QP, quantiser and mode parameters
	 * instead of us inferring them from registers.
	 *
	 * Any non-zero value also runs CAVCController::DebugInit; bits 1, 3,
	 * 4 and 7 add more sections and much more traffic. The log path
	 * allocates from shared memory and sends synchronously, so start at
	 * 0x20 - bit 5 alone - rather than anything wider.
	 */
	u32	dbg_bits;		/* u32; AVE_OFF_NONE = not located */
	/*
	 * iNumViews. Apple's own kext refuses to send the command unless
	 * 0 < iNumViews <= 2 (pInfo validator, kext 0xec9078, assert string
	 * 0xfffffe00071f090b) and we send zero, which macOS would reject. The
	 * firmware only tests == 2, for a stereo path (fw 0x78188, 0x79dac),
	 * so writing 1 is hygiene rather than a fix. Two adjacent offsets carry
	 * the same range check. docs/62 §6.5.
	 */
	u32	num_views[2];		/* u32 each; AVE_OFF_NONE = not located */
	/* parameter-set blocks */
	u32	sps_block, sps_block_size;
	u32	pps_block, pps_block_size;
};

/*
 * CODED_DATA_HDR - what the firmware writes into the coded-header buffer.
 * Offsets read out of the 13.5 kext's AVE_PrintCodedHeader
 * (0xfffffe0008eb6584, named by its own os_log format strings) and
 * AVE_RetrieveRCStats (0xfffffe0008ec4e38), and confirmed on the firmware
 * side where CAVCController::ProcessTranscodeDone (fw 0x5be18) and
 * CollectDataFromCpus (fw 0x5a7d4) store the same three fields.
 */
struct ave_coded_hdr_layout {
	u32	i_mb_cnt;		/* u32[4] */
	u32	p_mb_cnt;		/* u32[4] */
	u32	skip_mb_cnt;		/* u32[4] */
	u32	b_mb_cnt;		/* u32 */
	u32	sps_pps_bits;		/* u32, SPS+PPS length in BITS */
	u32	frame_num;		/* u32 FrameNumberFromDriverReturned */
	u32	frame_type;		/* u32 FrameTypeReturned */
	/* Per-slice records: slice_max of them, slice_stride bytes apart. */
	u32	slice_stride;		/* 0 = layout unknown for this ABI */
	u32	slice_max;
	u32	slice_bytes_written;	/* u32, offset from the record base */
	u32	slice_bytes_removed;	/* s8, bytes to drop at the slice end */
	/*
	 * numCABACzeroWordInserted. The firmware computes H.264 7.4.2.10's
	 * cabac_zero_word requirement (fw 0x5c134-0x5c1a0, k = excess/32 + 1),
	 * writes the COUNT here (fw 0x5c200) and logs "insert %d CABAZ zero
	 * words" - and does NOT write the bytes. Each one is three bytes,
	 * 00 00 03, appended after the last slice, and frameBytes excludes
	 * them. Always 0 for CAVLC; non-zero for CABAC at a low QP.
	 * docs/67 §2.
	 */
	u32	cabac_zero_words;	/* u32; AVE_OFF_NONE = not located */
	u32	min_bytes;		/* smallest buffer these offsets need */
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
	/*
	 * frameInfo.frameNumber - a monotone per-client counter, NOT the H.264
	 * frame_num syntax element: the firmware maintains that and idr_pic_id
	 * itself at ctrl+0x236E0 (fw 0x20e58, 0x210f8). The firmware's command
	 * queue keys Complete/Dequeue on this word (fw 0x16c78, 0x16858), and
	 * ManageDPBBuffer asserts frameNumber >= m_iFirstFrameNumber and then
	 * spins on "b ." if it fails (fw 0x2d350, CAVEDPB.cpp:963). docs/64 §3.
	 *
	 * Width differs by version, and getting it wrong is not harmless: on
	 * 13.5 a u64 store here would clobber frame_type at +0xCAC.
	 */
	u32	frame_num;
	bool	frame_num_u32;		/* false = u64 */
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
	/* Added for the first-frame work (docs/53). */
	u32	recon_y_lsb;		/* u64 sRecon.Y_LSB, NONE if absent */
	u32	recon_uv_lsb;		/* u64 sRecon.UV_LSB */
	u32	ctx_index;		/* u32 per-context index */
	u32	force_key_frame;	/* s32 */
	u32	force_non_ref;		/* u8 */
	u32	update_param_sets;	/* u8 */
	u32	scaling_matrix_mode;	/* u32 */
	/*
	 * sLowResOutput.LowResSrcLumaScaled - where the low-resolution
	 * motion-estimation pass writes the scaled copy of the source luma.
	 *
	 * CAVCController::setLRME is called unconditionally from setPipe on the
	 * per-frame path (fw 0x57d30) and asserts, straight-line, with no host
	 * flag in front of it:
	 *
	 *   setLRME:5782  (sLowResOutput.LowResSrcLumaScaled
	 *                  + EncCommParams.encode_row_init/4*lr_stride) != 0
	 *   setLRME:5783  (that sum & 63) == 0
	 *
	 * (fw 0x523b4-0x523e0; strings at file 0xc9b85 / 0xc9be6). For a
	 * whole-frame encode encode_row_init is 0, so the requirement is just
	 * "non-zero and 64-byte aligned". See docs/53 §9. AVE_OFF_NONE means
	 * the field has not been located for that ABI.
	 */
	u32	low_res_src;		/* u64 */
	/*
	 * sLowResOutput.LowResResults[] - the per-reference LRME result
	 * buffers. Recorded, never written: every read is cbz-skipped
	 * (fw 0x51e88, 0x51ed8, 0x52050, 0x520a8) and only the alignment is
	 * asserted when the entry is non-zero, so an I-frame leaves them all
	 * zero. low_res_results_max == 0 means "not located for this ABI".
	 */
	u32	low_res_results;
	u32	low_res_results_stride;
	u32	low_res_results_max;
	/*
	 * EncCommParams.encoder_addr_entropy[16][4] - the entropy-coding
	 * working buffers, a u64 table at PICMGMT + 0xA00 on 13.5.
	 *
	 * This is the last unconditional assert left on the per-frame path
	 * (docs/54). SetTranscode copies and checks entry [i][transcode_
	 * buffer_id] for i < ctrl+3768, which our arm sets to 4
	 * unconditionally (fw 0x5d064/0x5d074), and asserts both
	 * CAVCController_H13C.cpp:8020 (non-zero) and :8021 (& 63 == 0) at
	 * fw 0x59558 / 0x595a0.
	 *
	 * Element [i][j] is at entropy_set + entropy_stride_i*i +
	 * entropy_stride_j*j. We use j = transcode_buffer_id = 0. Confirmed
	 * from both sides: firmware copy PipePrepareParam fw 0x48800-0x4881c,
	 * host writer AVE_CHM_SetDataInfo_FwBuf kext 0xfffffe0008eb0cc4.
	 *
	 * Unlike low_res_src (PICMGMT + 0xC20), nothing in the firmware writes
	 * this range before reading it, so filling it is safe. entropy_max ==
	 * 0 means the table has not been located for this ABI.
	 */
	u32	entropy_set;
	u32	entropy_stride_i;
	u32	entropy_stride_j;
	u32	entropy_max;		/* rows the host may fill */
	u32	entropy_cols_max;	/* columns; 0 = only column 0 */
	/* Per-frame SrcNbr tables, same shape as start_avc.src_nbr_set. */
	u32	src_nbr_set[AVE_SRC_NBR_GROUPS];
	u32	src_nbr_max;
	/* Loose scratch IOVAs the host publishes per frame. */
	u32	scratch[AVE_PIC_SCRATCH_MAX];
	u32	scratch_n;
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
	struct ave_coded_hdr_layout	coded_hdr;
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
		.sve_num	= 0x10de8,	/* docs/54; single core = 1 */
		.need_lsb_planes = 0xfd7d,	/* fw 0x5d08c -> this+0x24058, docs/57 */
		.recon_lsb_addr	= 0x08,		/* entry {MSB u64, LSB u64}, docs/53 13 */
		.rc_mode	= 0xff50,	/* ui32RCFlag, ldr w8,[x10,#32] 0x5ceb4 */
		.rc_mode_fixed_qp = 2,		/* AVE_RC_FIXQP: cmp w10,#0x2 0x41158,
						 * string 0x4e69c */
		.rc_mode_on	= 1,		/* docs/66 §1 */
		.frame_rate_div	= 0xff48,	/* AVEFWRCSettings+0x18 */
		.bitrate_sel	= 0xff54,	/* +0x24; 2 -> bitrate_alt */
		.bitrate_alt	= 0xff58,	/* +0x28 */
		.drl_enable	= 0xff80,	/* +0x50 */
		.drl_cfg	= 0x10528,	/* 40 bytes, layout UNKNOWN */
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
		/* LowResRef: VideoParams+0x248 = wire 0x2A8, set stride 0x88,
		 * slot stride 8, 17 slots (kext 0xfffffe0008eaefcc/0xeaf010;
		 * fw ProvideReferenceFrames ldr x23,[x21,#584] 0x2b780). */
		.colocated_set      = 0xf6b0,	/* docs/53 13.2, docs/60 */
		.colocated_stride   = 0x08,
		.colocated_max      = AVE_DPB_MAX,
		.entropy_set	    = 0xf830,	/* docs/61 10: VP+0xF770+96 */
		.entropy_stride_i   = 0x20,
		.entropy_stride_j   = 0x08,
		.entropy_max	    = 4,	/* ctrl[3768] = 4 on our arm */
		.entropy_cols_max   = 4,
		.entropy_size_set   = 0xfa30,	/* INFERRED, docs/61 7.2 */
		.entropy_size_stride_i = 0x10,
		.entropy_size_stride_j = 0x04,
		.low_res_ref_set    = 0x2a8,
		.low_res_ref_stride = 0x08,
		.low_res_ref_max    = AVE_DPB_MAX,
		.low_res_result_set    = 0x3b8,	/* _S_AVE_SurfaceSet +0xBC8; docs/65 §Q4 */
		.low_res_result_stride = 0x08,
		.low_res_result_max    = AVE_LOW_RES_RESULT_MAX,
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
		/*
		 * Wire = VP + 0x60. kext AVE_CHM_SetFwBuf writes VP+0xF770,
		 * 0xF790, 0xF7B0 and 0xF7D0, four u64 each
		 * (0xfffffe0008eaf16c/1a8/1e4/2b8). The firmware reads the
		 * first three in InitEncodingParameters: [x23,#16] (= wire
		 * 0xF7D0) -> encoder_addr_src_nbr_info (fw 0x5d88c),
		 * [x23,#48] (= 0xF7F0) -> _src_nbr_pixels (0x5d89c), and
		 * VP+0xF7B0+8*idx (= 0xF810) -> _src_nbr_data (0x5d8bc).
		 * The fourth (wire 0xF830) is a 4x16 table with a size array
		 * at 0xFA30; not read on any path found - left out.
		 */
		/*
		 * docs/61 10: group 3 (FwData) is at wire 0xFED0, not adjacent to
		 * the other three - 0xF830 is row 0 of encoder_addr_entropy, which
		 * docs/59 3 mislabelled as FwData. InitEncodingParameters reads the
		 * same field as [x23,#1808] with x23 = VP+0xF760 (fw 0x5d8a0) and
		 * stores it to ctrl[7896], the register behind 0x40D13078C - which
		 * reads 0 in every run so far.
		 */
		.src_nbr_set	= { 0xf7d0, 0xf7f0, 0xf810, 0xfed0 },
		.src_nbr_max	= 4,
		.src_mode	= 0xfec0,	/* fw 0x5d018 -> [x22,#444], docs/62 §6.2 */
		.src_cfg_byte	= 0xfce8,	/* fw 0x5d118 -> [x22,#41],  docs/62 §6.2 */
		.src_go_bit3	= 0xfce9,	/* fw 0x5cfcc -> SRCDMAGO bit 3, docs/69 */
		.src_go_bits	= 0xfecc,	/* fw 0x5cfe4 -> SRCDMAGO bits 4+, docs/69 */
		.dbg_bits	= 0xfcd8,	/* fw 0x5cedc -> ctrl+0xA7C, docs/70 */
		.num_views	= { 0xff24, 0xff28 },	/* kext 0xec9078 rejects 0 */
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
		/*
		 * The kext DOES write this (str w8,[x20,#3240], kext
		 * 0xfffffe0008eaaa1c) and the firmware reads it straight out
		 * of the command image in CommandQueue::Enqueue (fw 0x166fc).
		 * Previously AVE_OFF_NONE here, on the mistaken grounds that
		 * 13.5 had no host field. docs/64 §3.
		 */
		.frame_num	= 0xca8,
		.frame_num_u32	= true,
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
		/* setPipe asserts on these two only when the 10-bit LSB gate
		 * is set ([x24,#1312] fw 0x54f7c, [x19,#2692] fw 0x55404);
		 * the offsets are the ones those gated loads use. */
		.recon_y_lsb	= 0x8a0,	/* fw ldr [x27,#2208] 0x54f84 */
		.recon_uv_lsb	= 0x8b0,	/* fw ldr [x27,#2224] 0x5540c */
		.ctx_index	= 0xcb0,	/* kext 0xfffffe0008eaaa58; fw
						 * ldr w27,[x23,#3248] 0x23d14,
						 * strb [x22] 0x5823c */
		.force_key_frame = 0x038,	/* kext str [x19,#56] 0xfffffe0008eab8e4;
						 * fw GetFrameType ldr [x23,#56] 0x23cf0 */
		.force_non_ref	= 0x03c,	/* kext ldrb [x19,#60] 0xfffffe0008eac0f8 */
		.update_param_sets = 0x6f1,	/* same kext log line */
		.scaling_matrix_mode = 0x6f4,	/* same kext log line */
		/*
		 * kext AVE_CHM_SetDataInfo_FwBuf writes four u64 at each of
		 * PICMGMT+0x980/0x9A0/0x9C0/0x9E0
		 * (0xfffffe0008eb0be4/0c1c/0c54/0c8c, "cmp x22,#0x4").
		 * ProcessTranscodeStart re-reads PICMGMT+0x9C0+8*k into
		 * encoder_addr_src_nbr_data (fw 0x583ac-0x583b4), which
		 * SetTranscode then asserts non-zero and 64-aligned
		 * (0x5869c / 0x58654, lines 7928/7929) - so the third entry
		 * is CONFIRMED. The other three are matched to the 26.6.2
		 * SrcNeighbor{Info,Pixel,FwData} group by their identical
		 * 0x20 stride and order; that mapping is INFERRED.
		 */
		/*
		 * fw setLRME ldr x15,[x21,#3104] 0x523bc, with x21 =
		 * pPicParams (mov x21,x2 at 0x51494) - the value the
		 * line-5782 assert at 0x52430 tests. The results array is the
		 * cbz-guarded set of loads at [x21,#3112] (0x51e84),
		 * [x21,#3120] (0x51ed4), [x21,#3128] (0x5204c) and
		 * [x21,#3136] (0x520a4): four entries, 8 bytes apart,
		 * immediately after LowResSrcLumaScaled.
		 */
		.low_res_src	= 0xc20,
		.low_res_results = 0xc28,
		.low_res_results_stride = 0x08,
		.low_res_results_max = 4,
		/* docs/54: PICMGMT +0xA00, u64[16][4]; 4 rows at j = 0. */
		.entropy_set	= 0xa00,
		.entropy_stride_i = 0x20,
		.entropy_stride_j = 0x08,
		.entropy_max	= 4,
		/*
		 * The kext fills a matrix, not a column: AVE_CHM_SetDataInfo_FwBuf
		 * loops j = 0..3 outside and i = 0..15 inside, one surface per
		 * entry (kext 0xfffffe0008eb0cb8..0d0c). F12 showed the four pipe
		 * entropy write channels (0x1303C0 + 0x40k) enabled with a null
		 * address while only column 0 was filled.
		 */
		.entropy_cols_max = 4,
		.src_nbr_set	= { 0x980, 0x9a0, 0x9c0, 0x9e0 },
		.src_nbr_max	= 4,
		/* kext 0xfffffe0008eb0b10/b44/b68/b84; meanings unknown. */
		.scratch	= { 0x8e0, 0x8e8, 0x8f0, 0x900 },
		.scratch_n	= 4,
	},
	.coded_hdr = {
		/* kext AVE_PrintCodedHeader 0xfffffe0008eb6584, field offsets
		 * taken from the loads next to each os_log format string. */
		.cabac_zero_words	= 0xf0,	/* fw str w24,[x0,#240] 0x5c200 */
		.i_mb_cnt		= 0x00,	/* ldr [x21,x28,lsl#2] 0xeb6728 */
		.p_mb_cnt		= 0x10,	/* ldr [x26,#16]       0xeb681c */
		.skip_mb_cnt		= 0x20,	/* ldr [x26,#32]       0xeb6918 */
		.b_mb_cnt		= 0x70,	/* ldr [x21,#112]      0xeb6a1c */
		.sps_pps_bits		= 0x98,	/* ldr [x21,#152]      0xeb6b24;
						 * fw str [x0,#152] 0x5be18 */
		.frame_num		= 0x10c,/* ldr [x21,#268]      0xeb6c2c;
						 * fw str [x0,#268] 0x5be30 */
		.frame_type		= 0x110,/* ldr [x21,#272]      0xeb6d34;
						 * fw str [x0,#272] 0x5be3c */
		/* AVE_RetrieveRCStats 0xfffffe0008ec4e38: x25 = hdr, stepped
		 * by 0x220 (0xec4efc) for up to 0x100 records (0xec4f00). */
		.slice_stride		= 0x220,
		.slice_max		= 0x100,
		.slice_bytes_written	= 0x180,/* ldr w9,[x25,#384]  0xec4ec0 */
		.slice_bytes_removed	= 0x38c,/* ldrsb  [x25,#908]  0xec4ee8 */
		/* The firmware maps 0x22c60 of it (fw 0x59f50/0x5bde4) and
		 * RetrieveRCStats reads up to +0x221ac. */
		.min_bytes		= 0x22c60,
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
		/* The 26.6.2 ABI configures RC through sRC.RCMode/Feature
		 * instead; none of the 13.5 scalars were located there. */
		.rc_mode_on	= AVE_OFF_NONE,
		.frame_rate_div	= AVE_OFF_NONE,
		.bitrate_sel	= AVE_OFF_NONE,
		.bitrate_alt	= AVE_OFF_NONE,
		.drl_enable	= AVE_OFF_NONE,
		.drl_cfg	= AVE_OFF_NONE,
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
		/* 26.6.2: no counterpart located. The 26.6.2 Start command has
		 * no AVE_VIDEO_PARAMS sub-block at a known offset and its
		 * ProvideReferenceFrames was not read, so the builder writes
		 * nothing and the per-frame PICMGMT field stands alone. */
		/* Not located on 26.6.2. */
		.low_res_result_set    = AVE_OFF_NONE,
		.low_res_result_stride = 0,
		.low_res_result_max    = 0,
		.low_res_ref_set    = AVE_OFF_NONE,
		.low_res_ref_stride = 0,
		.low_res_ref_max    = 0,
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
		/* The 26.6.2 counterparts of the 13.5 sExtraBuff.SrcNbr*
		 * tables in the Start command were not located; the per-frame
		 * ones (process_avc.src_nbr_set) are documented. */
		.src_nbr_set	= { AVE_OFF_NONE, AVE_OFF_NONE,
				    AVE_OFF_NONE, AVE_OFF_NONE },
		.src_nbr_max	= 0,
		/* The 13.5 source-path scalars (docs/62 §6) were located in the
		 * 13.5 firmware only; the 26.6.2 offsets are not known. */
		.src_mode	= AVE_OFF_NONE,
		.src_cfg_byte	= AVE_OFF_NONE,
		.src_go_bit3	= AVE_OFF_NONE,
		.src_go_bits	= AVE_OFF_NONE,
		.dbg_bits	= AVE_OFF_NONE,
		.num_views	= { AVE_OFF_NONE, AVE_OFF_NONE },
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
		.recon_y_lsb	= AVE_PIC_RECON_Y_LSB,
		.recon_uv_lsb	= AVE_PIC_RECON_UV_LSB,
		.ctx_index	= AVE_PIC_CTX_INDEX,
		.force_key_frame = AVE_PIC_FORCE_KEYFRAME,
		.force_non_ref	= AVE_OFF_NONE,		/* not located on 26.6.2 */
		.update_param_sets = 0x174d,		/* docs/47 §1.2 */
		.scaling_matrix_mode = 0x1750,		/* docs/47 §1.2 */
		/* 0x20 apart, four u64 each - the shape the 13.5 groups were
		 * matched against. */
		/* docs/32 §6.5: confirmed from CLRMEFSController::
		 * ConfigWrDMALowResSrcScaled (0x8a850) and
		 * ConfigWrDMALowResFSRslts (0x8aa10). The 26.6.2 results
		 * entries are {addr, size} pairs, 0x10 apart. */
		.low_res_src	= AVE_PIC_LOWRES_LUMA_SCALED,
		.low_res_results = AVE_PIC_LOWRES_RESULTS,
		.low_res_results_stride = 0x10,
		.low_res_results_max = 1,	/* only [0] was located */
		/* Not located on 26.6.2; docs/54 analysed 13.5 only. */
		.entropy_set	= AVE_OFF_NONE,
		.entropy_max	= 0,
		.src_nbr_set	= { AVE_PIC_SRC_NEIGH_INFO, AVE_PIC_SRC_NEIGH_PIXEL,
				    AVE_PIC_SRC_NEIGH_DATA, AVE_PIC_SRC_NEIGH_FWDATA },
		.src_nbr_max	= 4,
		.scratch	= { AVE_PIC_SCRATCH_CMDINFO40,
				    AVE_PIC_SCRATCH_SLOTPOOL,
				    AVE_PIC_SCRATCH_CMDINFO48, AVE_OFF_NONE },
		.scratch_n	= 3,
	},
	/*
	 * CODED_DATA_HDR on 26.6.2 has NOT been read. The 26.6.2 kext carries
	 * the same AVE_PrintCodedHeader format strings, so the field names are
	 * the same, but nothing here was checked against that binary and the
	 * per-slice stride in particular is a version-sensitive number (13.5's
	 * coded-header buffer is 0x23000, 26.6.2's is 0xC000, so it cannot
	 * hold 256 x 0x220 records and the layout must differ). slice_stride
	 * = 0 makes ave_cmd_coded_length() refuse rather than guess.
	 */
	.coded_hdr = { .slice_stride = 0 },
};
#endif /* AVE_CMD_ABI_DEFINE_TABLES */

#endif /* __AVE_ABI_H__ */
