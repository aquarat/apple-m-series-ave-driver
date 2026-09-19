/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - pure command builders, one code path for every firmware ABI.
 *
 * Nothing here does I/O, allocates, or keeps state. Each builder zero-fills
 * the caller's buffer to the exact size the selected firmware asserts, writes
 * the fields through struct ave_cmd_abi, and returns that size (or -EINVAL).
 * All buffer addresses are parameters.
 *
 * UNTESTED ON HARDWARE. Checked only against the evidence tables in
 * tools/abi_selftest.
 */
#ifndef __AVE_CMD_H__
#define __AVE_CMD_H__

#include <linux/types.h>

#include "ave_abi.h"

/* Per-command header values that are not ABI literals. */
struct ave_cmd_ctx {
	u64	count;		/* CNT, the host's sequence number */
	u64	client_id;	/* CID; must fit in u32 on 13.5 */
	u8	timeout[16];	/* _S_AVE_TimeOut, copied verbatim */
};

struct ave_config_params {
	bool	skip_mcpu;		/* 1 -> firmware creates no McpuController */
	bool	create_mcpu;		/* must be 1 to get one */
	u64	reg_dart_addr;		/* 13.5 only (AVE_Reg::GetDARTAddr(3)) */
	u32	doorbell_cadence[2];	/* Apple leaves both 0 */
	u32	dsid;			/* AVE_MCC::GetDSID(0); <= 0xff on 13.5 */
	u64	shmem_addr;		/* IOVA of the IPC shared region */
	u32	shmem_size;
};

/* One DPB (reconstruction) surface. */
struct ave_recon_buf {
	u64	addr;		/* luma data plane, 128-byte aligned */
	u32	luma_size;	/* luma data bytes (26.6.2 entry dataSize) */
	/*
	 * 13.5 only, with ave_avc_session.need_lsb_planes: the tile-metadata
	 * ("LSB") plane, the entry's second u64. The firmware derives both
	 * chroma planes from the two luma ones (docs/57 Q4). 0 = none.
	 */
	u64	lsb_addr;
};

struct ave_buf {
	u64	addr;
	u32	size;
};

/*
 * A fixed-QP, I-frame-only, 8-bit 4:2:0, single-slice progressive AVC session.
 */
struct ave_avc_session {
	u32	width, height;		/* display size; coded size is MB-aligned */
	u32	frame_rate;		/* fps, > 0 */
	u32	bitrate;		/* ignored at fixed QP; may be 0 */
	u32	qp_i, qp_p, qp_b;	/* 0..51 */
	u32	key_interval;		/* >= 1; 1 = every frame is an IDR */
	u8	profile_idc;		/* 66, 77 or 100 */
	u8	level_idc;		/* 10..62 (not 1b) */
	bool	cabac;			/* entropy_coding_mode_flag; not with 66 */

	u64	fw_client_addr;		/* per-client firmware buffer */
	u32	fw_client_size;
	u64	fw_client_mem_addr;	/* must be non-zero on 13.5 */
	u32	fw_client_mem_size;

	/*
	 * Where the firmware writes the SPS+PPS it generates. Required
	 * wherever the ABI has the field: 13.5 dereferences it unguarded and
	 * asserts "MappedMemory.cpp, 39: paddr != 0" if it is zero (docs/52).
	 * The firmware's copies into it are NOT bounded by param_sets_size, so
	 * the buffer must be comfortably larger than any SPS+PPS pair.
	 */
	/*
	 * Start_AVC NEED_LSB_PLANES (13.5 wire 0xFD7D). The only code that
	 * programs the pipe's recon writer runs when it is set (fw 0x54f90 ->
	 * 0x40D130240 = 0x800314B1); left 0, the firmware logs "Uncompress Ref
	 * is not supported" and the Pipe never finishes (F5, docs/57). When
	 * true every recon[] entry needs an lsb_addr.
	 */
	bool	need_lsb_planes;
	/*
	 * The two AVE_VIDEO_PARAMS scalars that reach the source-read register
	 * block (start_avc.src_mode / .src_cfg_byte, docs/62 §6). Neither value
	 * is known - the kext passes both through from user space without
	 * touching them - so these are here to be swept on hardware rather than
	 * set from analysis. Zero reproduces every run up to F17.
	 */
	u16	src_mode;
	u8	src_cfg_byte;
	u64	param_sets_addr;
	u32	param_sets_size;

	const struct ave_recon_buf *recon;	/* DPB surfaces */
	u32	n_recon;			/* 1..abi recon_max */
	/*
	 * The LRME scaled-luma surfaces ("LowResRef"), one per DPB slot, in
	 * the SAME slot order as recon[]. Written to
	 * start_avc.low_res_ref_set + slot*low_res_ref_stride.
	 *
	 * This - not the per-frame PICMGMT field - is what ends up in
	 * sLowResOutput.LowResSrcLumaScaled: CAVECommonDPB::setRefPointers
	 * overwrites the per-frame field from the DPB entry (fw 0x2c318 /
	 * 0x2c320) before CAVCController::setLRME reads it. Each must be
	 * 64-byte aligned (setLRME:5783) and
	 * ALIGN(ALIGN(4*W,256) * ((H+63)>>4), 512) bytes long, from the kext's
	 * AVE_CalcBufSizeOfLowResRef (0xfffffe0008ea560c).
	 *
	 * n_low_res_ref == 0 writes none, which leaves the assert in place;
	 * that is the deliberate negative control.
	 */
	u64	low_res_ref[AVE_DPB_MAX];
	u32	n_low_res_ref;			/* 0, or exactly n_recon */
	/*
	 * Colocated MV buffers, one per DPB slot, same order as recon[]
	 * (docs/60). 0 entries leaves the table zero, which disables the pipe's
	 * colocated writer; kept possible as the control.
	 */
	u64	colocated[AVE_DPB_MAX];
	u32	n_colocated;			/* 0, or exactly n_recon */
	/*
	 * encoder_addr_entropy[row][col], the SEB write buffers. This is where
	 * the firmware actually takes them from (docs/61 10); the per-frame copy
	 * in ave_avc_frame is overwritten each frame by setRefPointers.
	 */
	u64	entropy[AVE_ENTROPY_MAX][AVE_ENTROPY_COLS];
	u32	n_entropy;			/* rows; 0 = write none */
	u32	n_entropy_cols;			/* columns; 0 or 1 = column 0 */
	u32	entropy_size;			/* bytes per buffer; 0 = no size table */
	const struct ave_buf	*coded;		/* bitstream buffers */
	const struct ave_buf	*coded_hdr;	/* coded-header buffers, same count */
	u32	n_coded;			/* 1..abi coded_max */

	/*
	 * Source-neighbour scratch, [group][index]. Optional: with
	 * n_src_nbr == 0 the builder writes nothing there, which is what the
	 * pre-first-frame self-test sent and what the firmware accepted at
	 * Start. The first Process asserts on them, though - see
	 * ave_start_avc_layout.src_nbr_set. Every entry written must be
	 * non-zero and 64-byte aligned.
	 */
	u64	src_nbr[AVE_SRC_NBR_GROUPS][AVE_SRC_NBR_MAX];
	u32	n_src_nbr;			/* 0 or <= abi src_nbr_max */
};

struct ave_avc_frame {
	u32	frame_type;		/* AVE_FRAME_TYPE_{I,P,IDR} */
	/*
	 * frameInfo.frameNumber: a monotone per-client counter the firmware
	 * keys its queue on, not the H.264 frame_num syntax element (which
	 * the firmware maintains itself). u32 on 13.5, u64 on 26.6.2 - the
	 * ABI table picks. docs/64 §3.
	 */
	u64	frame_num;
	u32	poc;			/* 26.6.2 only */
	u32	frame_rate;		/* 26.6.2 only (double field); 0 = omit */

	u64	in_luma_addr;		/* != 0, % 64 */
	u32	in_luma_stride;		/* != 0, % 64 */
	u32	in_luma_size;		/* 26.6.2: != 0 */
	u64	in_chroma_addr;		/* % 64 */
	u32	in_chroma_stride;	/* % 64 */
	u32	in_chroma_size;

	u32	coded_index;		/* index into the Start-time coded table */
	u64	coded_addr;		/* must equal that table entry's address */
	u64	coded_hdr_addr;
	u32	coded_size;

	u64	recon_luma_addr;	/* 0 = leave to the firmware; % 128 */
	u64	recon_chroma_addr;
	u64	recon_mv_addr;
	/*
	 * 10-bit MSB/LSB split planes. Only read when the session was started
	 * with the LSB gate set, which we never do - but setPipe dereferences
	 * them behind that gate without a null check, so filling them costs
	 * nothing and removes one way to assert. 0 = leave the field zero.
	 */
	u64	recon_luma_lsb_addr;	/* % 128 */
	u64	recon_chroma_lsb_addr;	/* % 128 */

	u32	ctx_index;		/* per-context slot; 0 for one client */
	bool	force_key_frame;
	bool	force_non_ref;		/* -> nal_ref_idc = 0 for this frame */
	bool	update_param_sets;	/* re-emit SPS/PPS accounting on an IDR */

	/*
	 * sLowResOutput.LowResSrcLumaScaled: the low-resolution motion
	 * estimation pass's scaled-source-luma target. setLRME runs for every
	 * frame, including an I-frame with no references, and asserts this is
	 * non-zero and 64-byte aligned (fw 0x52430 / 0x523e8, lines 5782 and
	 * 5783). 0 leaves the field zero, which reproduces that assert - the
	 * driver exposes it as a module parameter for exactly that bisect.
	 * Required size: ALIGN(ALIGN(4*W,256) * ((H+63)>>4), 512), from the
	 * kext's AVE_CalcBufSizeOfLowResRef (0xfffffe0008ea560c).
	 */
	u64	low_res_src_addr;	/* % 64 */

	/*
	 * EncCommParams.encoder_addr_entropy[i][0] - the entropy-coding
	 * working buffers, the last unconditional per-frame assert (docs/54).
	 * SetTranscode requires the first ctrl+3768 (= 4 on our arm) to be
	 * non-zero and 64-byte aligned, fw 0x59558 / 0x595a0, asserting
	 * CAVCController_H13C.cpp:8020 and :8021.
	 *
	 * Size per buffer, from the kext's AVE_CalcBufSizeOfEntropyCoding
	 * (0xfffffe0008ea5bd0) AVC arm: ALIGN_DOWN(64*W + 960, 1024) * K,
	 * with K either 8 or ceil(ceil(H/16)/4). Which flag picks K could not
	 * be pinned, so the driver uses the larger: 960 KiB at 1280x720.
	 *
	 * 0 entries leave the table zero, which reproduces the assert - kept
	 * as the negative control, exactly like low_res_src_addr above.
	 */
	u64	entropy[AVE_ENTROPY_MAX][AVE_ENTROPY_COLS];
	u32	n_entropy;		/* rows; 0 = write none */
	u32	n_entropy_cols;		/* columns; 0 or 1 = column 0 only */

	/* Per-frame source-neighbour scratch, [group][index]; % 64. */
	u64	src_nbr[AVE_SRC_NBR_GROUPS][AVE_SRC_NBR_MAX];
	u32	n_src_nbr;		/* 0 = write none */
	/* Loose per-frame scratch IOVAs; 0 = write none. */
	u64	scratch[AVE_PIC_SCRATCH_MAX];
	u32	n_scratch;
};

/* What ave_cmd_coded_length() recovers from a completed frame's header. */
struct ave_coded_info {
	u32	bytes;			/* the encoded frame length */
	u32	slices;			/* records with a non-zero byte count */
	u32	bytes_removed;		/* sum of the per-slice tail trims */
	u32	frame_type;		/* CODED_DATA_HDR FrameTypeReturned */
	u32	frame_num;
	u32	sps_pps_bits;		/* SPS+PPS length in bits, at Start */
};

size_t ave_cmd_size(const struct ave_cmd_abi *abi, enum ave_op op);

int ave_cmd_build_hdr(const struct ave_cmd_abi *abi, enum ave_op op,
		      u8 *buf, size_t len, const struct ave_cmd_ctx *ctx,
		      u32 slot);

/* Header-only commands and Close: Halt, Open, Close, Stop, Complete, Flush. */
int ave_cmd_build_simple(const struct ave_cmd_abi *abi, enum ave_op op,
			 u8 *buf, size_t len, const struct ave_cmd_ctx *ctx);

int ave_cmd_build_config(const struct ave_cmd_abi *abi, u8 *buf, size_t len,
			 const struct ave_cmd_ctx *ctx,
			 const struct ave_config_params *p);

static inline int ave_cmd_build_open(const struct ave_cmd_abi *abi, u8 *buf,
				     size_t len, const struct ave_cmd_ctx *ctx)
{
	return ave_cmd_build_simple(abi, AVE_OP_OPEN, buf, len, ctx);
}

static inline int ave_cmd_build_close(const struct ave_cmd_abi *abi, u8 *buf,
				      size_t len, const struct ave_cmd_ctx *ctx)
{
	return ave_cmd_build_simple(abi, AVE_OP_CLOSE, buf, len, ctx);
}

int ave_cmd_build_start_avc(const struct ave_cmd_abi *abi, u8 *buf, size_t len,
			    const struct ave_cmd_ctx *ctx,
			    const struct ave_avc_session *s);

int ave_cmd_build_process_avc(const struct ave_cmd_abi *abi, u8 *buf,
			      size_t len, const struct ave_cmd_ctx *ctx,
			      u32 slot, const struct ave_avc_frame *f);

/*
 * Validate a firmware completion message for @op. Returns 0 if the id, length
 * and client id match and the status is this ABI's success value; -EPROTO if
 * the message is not the reply to @op; -EIO if it is but reports failure.
 * *status (if non-NULL) receives the raw status word whenever it was read.
 */
int ave_cmd_check_reply(const struct ave_cmd_abi *abi, enum ave_op op,
			const u8 *msg, size_t len, u64 client_id, u32 *status);

/*
 * Decode the coded-header buffer the firmware wrote for one completed frame.
 *
 * This is the *only* place the encoded length comes from: the ENCODE_DONE
 * completion carries no byte count (its extra word at +0x40 is the slice
 * number, fw ProcessEncDone 0x14e14-0x14e74). Apple's kext computes the
 * length in AVE_RetrieveRCStats (0xfffffe0008ec4e38) as
 *
 *     sum over slices of ui32BytesWritten
 *   - sum over slices of ui32BytesToRemoveAtTheEndOfTheSliceForContextSwitch
 *
 * stopping at the first record whose byte count is zero. This reproduces that
 * exactly. Returns 0 and fills @out, -EINVAL (bad argument, a buffer smaller
 * than the layout needs, or an ABI whose coded-header layout has not been read
 * - check abi->coded_hdr.slice_stride first to tell those apart), or -EPROTO
 * (a negative trim count or an impossible byte count, which is what Apple
 * treats as a corrupt header).
 */
int ave_cmd_coded_length(const struct ave_cmd_abi *abi, const void *hdr,
			 size_t hdr_len, struct ave_coded_info *out);

#endif /* __AVE_CMD_H__ */
