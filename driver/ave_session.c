// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE - first-command session self-test.
 *
 * UNTESTED ON HARDWARE. See ave_session.h.
 *
 * What this does, once the boot handshake is complete:
 *
 *   1. Config      - the IOP/device configuration command (global, no client).
 *   2. Open        - register a client id / session.
 *   3. Start_AVC   - a minimal fixed-QP, I-only, 8-bit 4:2:0 AVC session.
 *   4. Process     - one I-frame, only with session_frame=1 (docs/53). The
 *                    encoded bitstream is published under
 *                    /sys/kernel/debug/apple_ave/ once a frame comes back.
 *
 * Each step builds the command for ave_cmd_abi_get(ave->fw_abi) into a buffer
 * carved from the FwIPC region (so ave_ipc_send() can hand its IOVA to the
 * firmware), sends it on the IO channel, waits for the reply with a timeout,
 * decodes the status via ave_cmd_check_reply(), and logs the raw reply words.
 *
 * The firmware answers a command by writing its reply back into the *same*
 * buffer and Sending it back on IO with the opposite phase (docs/36 §8). The
 * live IRQ handler (ave_ipc.c) drains IO and hands each reply payload to
 * ave->ipc_rx(). We install a capturing hook for the duration of the run,
 * copy the reply out under a completion, and restore the previous hook on
 * exit. ipc_rx runs in hard IRQ context, so the hook only memcpy()s and
 * complete()s.
 *
 * None of it runs unless session_selftest=1 (or session_frame=1) is passed, so
 * a wrong guess degrades to a logged firmware rejection or assert, not a
 * wedged encoder. With session_frame=0 the three commands are byte-identical
 * to the ones the firmware accepted on 2026-09-13 21:13 (docs/31).
 */
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "ave.h"
#include "ave_abi_boot.h"	/* AVE_CH_IO, ave_ipc_alloc/free */
#include "ave_cmd.h"
#include "ave_session.h"

/* ------------------------------------------------------------------------ */
/* Module parameters - the gate and the values we cannot read from the fw   */
/* ------------------------------------------------------------------------ */

static bool session_selftest;
module_param(session_selftest, bool, 0444);
MODULE_PARM_DESC(session_selftest,
	"send the opening Config/Open/Start_AVC command sequence after boot and log each reply (default off)");

/*
 * Config +0x48 on 13.5 is AVE_Reg::GetDARTAddr(reg type 3), used later by
 * ProcessInitStage2. We have no way to compute it here; the operator can
 * supply it if known. 0 is sent otherwise (and logged loudly).
 */
static unsigned long session_reg_dart;
module_param(session_reg_dart, ulong, 0444);
MODULE_PARM_DESC(session_reg_dart,
	"Config reg-DART address (13.5 cmd +0x48, AVE_Reg::GetDARTAddr(3)); 0 = unknown");

/* AVE_MCC::GetDSID(0); <= 0xff on 13.5. Unknown, default 0. */
static unsigned int session_dsid;
module_param(session_dsid, uint, 0444);
MODULE_PARM_DESC(session_dsid, "Config MCC DSID (default 0)");

static unsigned int session_width = 1280;
module_param(session_width, uint, 0444);
MODULE_PARM_DESC(session_width, "Start_AVC display width (default 1280)");

static unsigned int session_height = 720;
module_param(session_height, uint, 0444);
MODULE_PARM_DESC(session_height, "Start_AVC display height (default 720)");

static unsigned int session_qp = 30;
module_param(session_qp, uint, 0444);
MODULE_PARM_DESC(session_qp, "Start_AVC fixed QP for I/P/B (0..51, default 30)");

/* ---- phase 6: one encoded frame (docs/53) ---- */

static bool session_frame;
module_param(session_frame, bool, 0444);
MODULE_PARM_DESC(session_frame,
	"after Start_AVC, encode one I-frame with Process and publish the bitstream under /sys/kernel/debug/apple_ave (default off; implies session_selftest)");

/*
 * The source-neighbour scratch tables. Their sizes ARE known - docs/47 line
 * 302, from the kext (0xea5970, 0x59e8, 0x5a70, 0x5adc): per macroblock
 * column, Info 256, Pixel 1024, Data 56, FwData 64 bytes, with a 16 KiB
 * floor. At 1280 wide (80 MB columns) that is 20/80/4.5/5 KiB, so the floor
 * dominates all but Pixel. session_nbr_kb overrides the per-slot size for
 * bisecting; 0 means "use the formula".
 *
 * Note for a failure: the 16 slots are one contiguous mapping, so a slot that
 * is too small overruns into the next slot rather than faulting - the symptom
 * is wrong output, not a DART fault (review of 19b9d93, finding 2).
 *
 * session_nbr=0 sends the tables zero, which is what the pre-phase-6 self-test
 * did - useful as a bisect: it should then assert
 * "encoder_addr_src_nbr_info != 0" at setPipe line 6990.
 */
static bool session_nbr = true;
module_param(session_nbr, bool, 0444);
MODULE_PARM_DESC(session_nbr,
	"publish the SrcNeighbor scratch tables at Start_AVC and in Process (default on; 0 to prove the assert)");

static unsigned int session_nbr_kb;	/* 0 = size from the docs/47 formula */
module_param(session_nbr_kb, uint, 0444);
MODULE_PARM_DESC(session_nbr_kb,
	"size of each SrcNeighbor scratch slot in KiB (default 256; size is unknown, this is a guess)");

/*
 * sLowResOutput.LowResSrcLumaScaled - the low-resolution motion-estimation
 * (LRME) scaled-source-luma surface.
 *
 * CAVCController::setPipe calls setLRME for every frame (fw 0x57d30). The two
 * gates in front of that call - the byte at controller+0x23FEC and the word at
 * controller+0x13A3C - are both cleared by the CAVCController constructor
 * (fw 0x460fc / 0x46104) and are only ever written by the firmware's own LRME
 * state machine (ProcessLRMEStart, ProcessLRMEDone, ProcessPipeReset,
 * ResetBetweenPasses). There is NO host-settable flag in Start_AVC or in
 * PICMGMT that switches the pass off, so the buffer has to be supplied.
 * (docs/53 §9.)
 *
 * Size, from the kext's AVE_CalcBufSizeOfLowResRef (0xfffffe0008ea560c), AVC
 * arm, DevType 12 < 0x13:
 *
 *     lr_stride = ALIGN(4 * W, 256)
 *     size      = ALIGN(lr_stride * ((H + 63) >> 4), 512)
 *
 * lr_stride matches the firmware's own expression bit for bit
 * (fw 0x523c0-0x523c8: lsl #2, add #0xfc, and #0xffffff00), which is what
 * makes this the right formula and not a guess. At 1280x720: 0x3C000.
 *
 * WHERE IT ACTUALLY HAS TO GO (2026-09-13, after the first hardware Process):
 * supplying the buffer at PICMGMT + 0xC20 changed nothing, because the field
 * is overwritten before setLRME reads it. CAVECommonDPB::setRefPointers loads
 * the DPB entry's +224 and stores it there (fw ldp x11,x8,[x2,#216] 0x2c318,
 * str x8,[x1,#3104] 0x2c320), and that value comes from the Start_AVC command:
 *
 *   Start_AVC wire 0x2A8 + slot*8   (AVE_VIDEO_PARAMS + 0x248, cmd+0x60)
 *     -> ProvideReferenceFrames   fw 0x2b780 / 0x2b788  -> DPBctx+0x10A0+slot*8
 *     -> InitPointerAndVariables  fw 0x2bddc / 0x2bde8  -> DPB entry + 64
 *     -> ManageDPBBuffer          fw 0x2d544 / 0x2d55c  -> RefFrameInfo + 224
 *     -> setRefPointers           fw 0x2c318 / 0x2c320  -> PICMGMT + 0xC20
 *
 * So the buffers are published per DPB slot at Start_AVC (see session_dpb),
 * and the per-frame field is written too - it is inert, but it is free, and
 * it keeps the field's documented meaning visible in the command dump.
 *
 * session_lowres=0 leaves BOTH zero, which reproduces the 2026-09-13 hardware
 * failure exactly ("ASSERT: CAVCController_H13C.cpp, 5782") and is the
 * negative control for this change. session_lowres_kb overrides the size; the
 * FORMULA is confirmed but the number of rows the engine actually writes is
 * inferred, so the override exists to raise it without a rebuild.
 */
static bool session_lowres = true;
module_param(session_lowres, bool, 0444);
MODULE_PARM_DESC(session_lowres,
	"publish the LowResRef (LRME scaled-luma) surfaces at Start_AVC and in Process (default on; 0 reproduces the setLRME:5782 assert)");

static unsigned int session_lowres_kb;	/* 0 = the kext formula above */
module_param(session_lowres_kb, uint, 0444);
MODULE_PARM_DESC(session_lowres_kb,
	"size of each LRME scaled-luma surface in KiB (0 = AVE_CalcBufSizeOfLowResRef formula)");

/*
 * How many DPB slots Start_AVC publishes - one reconstruction surface and one
 * LowResRef surface each.
 *
 * The firmware reads exactly max_num_ref_frames+1 slots of set 0:
 * CAVCController::InitEncodingParameters calls ProvideReferenceFrames with
 * numRefs = SPS max_num_ref_frames (fw ldr w1,[x24,#1072] 0x5dd14) and that
 * function copies slots 0..numRefs inclusive (fw 0x2b638-0x2b644, loop bound
 * numRefs+1) for sets 0..[dpb+32], and the H264VideoEncoderDPB constructor
 * sets [dpb+32] = 1 (fw strb w8,[x0,#32] 0x2d1c4) - so one set only. We send
 * max_num_ref_frames = 1 (ave_cmd.c), hence 2.
 *
 * Apple allocates the same count: AVE_CalcBufNumOfLowResRef (kext
 * 0xfffffe0008ea55d8) returns n+1 and AVE_CreateInternalSurfaces (kext
 * 0xfffffe0008f3a414) creates one surface per slot.
 *
 * session_dpb=1 reproduces the recon table of the Start_AVC the firmware
 * accepted on 2026-09-13 21:13, which is the bisect for this change: the
 * first frame uses slot 0 (ManageDPBBuffer reads the index at ctx+4228, which
 * InitPointerAndVariables zeroes, fw 0x2bd1c) but also reads slot 1 as the
 * "next" entry (fw 0x2d504-0x2d524).
 */
static unsigned int session_dpb = 2;
module_param(session_dpb, uint, 0444);
MODULE_PARM_DESC(session_dpb,
	"DPB slots published at Start_AVC: recon + LowResRef surfaces (default 2 = max_num_ref_frames+1)");

/* AVE_FRAME_TYPE_IDR (3) by default; 0 = I (non-IDR). */
static unsigned int session_frame_type = AVE_FRAME_TYPE_IDR;
module_param(session_frame_type, uint, 0444);
MODULE_PARM_DESC(session_frame_type,
	"IMG_FRAME_TYPE for the single Process (3 = IDR, 0 = I; default 3)");

/* ------------------------------------------------------------------------ */
/* Tunables that are pure sizing guesses (flagged in the report)            */
/* ------------------------------------------------------------------------ */

#define AVE_SESS_TIMEOUT_MS	2000	/* == AVE_BOOT_RECV_TIMEOUT_MS */
#define AVE_SESS_CLIENT_ID	1u	/* host-assigned; RegisterClient(cid) */

#define AVE_SESS_REPLY_MAX	0x80	/* replies are 0x40 / 0x48 */

/* Config shared-memory region: >= 4 x carve set; a dedicated buffer so the
 * firmware's AddSharedMemory carve cannot land on the live channel rings. */
#define AVE_SESS_SHMEM_SIZE	0x40000

/* Start_AVC per-client buffers. */
#define AVE_SESS_FWCLIENT_FALLBACK	0xb4000	/* 13.5 GetClientBufferSize */
#define AVE_SESS_FWCLIENTMEM_SIZE	0x100000

/* One coded (bitstream) output buffer must exceed 3*W*H/4 at encode time. */
#define AVE_SESS_CODED_SIZE		0x200000
/*
 * The SPS+PPS the firmware generates (docs/52). A pair is a few hundred bytes,
 * but the firmware's copies into this buffer are NOT bounded by the size we
 * declare - the only length check in InitEncodingParameters compares against a
 * field that is still zero on a first init (fw 0x5de44) - so give it a whole
 * page rather than a tight fit.
 */
#define AVE_SESS_PARAM_SETS_SIZE	0x1000

/*
 * The Process slot. Any value < hdr.max_slot is legal; macOS's own client uses
 * 21..40 (kext 0xfffffe0008f0166c / 0xfffffe0008f01188), so stay in that range
 * rather than reusing a slot the fixed-slot commands own.
 */
#define AVE_SESS_PROCESS_SLOT		21

/* ------------------------------------------------------------------------ */
/* Reply capture (written from hard IRQ by the ipc_rx hook)                 */
/* ------------------------------------------------------------------------ */

struct ave_sess_rx {
	struct completion	done;
	u8			buf[AVE_SESS_REPLY_MAX];
	u32			size;
	u32			flags;
	bool			overflow;
	/* The IO echo of the command buffer, which is an ack, not the answer. */
	u32			ack_size;
	bool			ack_seen;
	/*
	 * The id this command is waiting for. The encode path has seven
	 * NotificationToHost sites (0xE03/E04/E06/E07/E09/E0A/E0B) and
	 * LRME_DONE (0xE07) is raised inside the same ProcessEncDone as
	 * ENCODE_DONE (fw 0x14d38 vs 0x14e70). Completing on whatever lands
	 * first would report a frame that actually succeeded as -EPROTO, with
	 * the real completion going to the restored hook. Keep waiting
	 * instead, and log what was skipped. (Review of 19b9d93, finding 1.)
	 */
	u16			want_id;
	u32			other_id;
	unsigned int		other_count;
};

/*
 * A single device is bound in practice and the self-test runs once at the end
 * of probe under the caller's serialisation, so a file-static capture context
 * is adequate. It is published to the IRQ via ave->ipc_rx (an ordered store)
 * before the first send and cleared after the last reply.
 */
static struct ave_sess_rx ave_sess_rx;

static void ave_session_ipc_rx(struct ave_device *ave, u32 chan_id,
			       void *buf, u32 size, u32 flags)
{
	struct ave_sess_rx *rx = &ave_sess_rx;

	/*
	 * Two different messages come back per command, and only one is the
	 * answer (review of 7998bf1, finding 1):
	 *
	 *   IO      - the firmware echoes the command buffer back
	 *             (CController::CmdProcess, fw 0xa1cf8, on handle [this+120]);
	 *             byte 0 is still the command id and +0x38 is untouched.
	 *             The kext treats ch 1 as ProcessIntr_CmdAck
	 *             (0xfffffe0008f0d710). It is an ack.
	 *   IO_T2H  - the completion NotificationToHost built at fw 0x13684 and
	 *             sent through PostCmdSynchronous on handle [this+144]
	 *             (fw 0xa1fb8); the kext's ch 2 path decodes id@0 and
	 *             cid@0x10 from it. This is the reply to check.
	 *
	 * Capturing IO and checking it as the reply is why the first version
	 * could only ever fail with -EPROTO.
	 */
	if (chan_id == AVE_CH_IO) {
		rx->ack_size = size;
		rx->ack_seen = true;
		return;
	}
	if (chan_id != AVE_CH_IO_T2H)
		return;

	if (rx->want_id && size >= 2 && buf) {
		u16 id = get_unaligned_le16(buf);

		if (id != rx->want_id) {
			rx->other_id = id;
			rx->other_count++;
			return;		/* not ours: keep waiting */
		}
	}

	rx->flags = flags;
	rx->overflow = size > sizeof(rx->buf);
	rx->size = min_t(u32, size, (u32)sizeof(rx->buf));
	if (buf && rx->size)
		memcpy(rx->buf, buf, rx->size);
	else
		rx->size = 0;	/* payload outside FwIPC: report nothing, not stale */
	complete(&rx->done);
}

/* ------------------------------------------------------------------------ */
/* Buffer bookkeeping - freed on every exit path                            */
/* ------------------------------------------------------------------------ */

#define AVE_SESS_MAX_DMA	16
#define AVE_SESS_MAX_IPC	8

/* 16 SrcNeighbor slots: 4 groups x 4 entries. */
#define AVE_SESS_NBR_SLOTS	(AVE_SRC_NBR_GROUPS * AVE_SRC_NBR_MAX)

/*
 * DPB slots the self-test is willing to publish. The wire table holds 17 per
 * set (AVE_DPB_MAX), but each slot costs a reconstruction surface and a
 * LowResRef surface, so the self-test caps it well below that; session_dpb
 * above explains why the answer for this session is 2.
 */
#define AVE_SESS_DPB_MAX	4

struct ave_sess_bufs {
	struct ave_device *ave;
	struct { void *cpu; dma_addr_t iova; size_t size; } dma[AVE_SESS_MAX_DMA];
	unsigned int ndma;
	struct { void *cpu; size_t size; } ipc[AVE_SESS_MAX_IPC];
	unsigned int nipc;

	/*
	 * What Start_AVC published, so the Process step can repeat the same
	 * addresses (the firmware asserts sOutput.Coded == the Start-time
	 * table entry: fw 0x58404, "pPicParams->sOutput.Coded ==
	 * EncCommParams.bitstream_addr_dst[index]").
	 */
	void		*coded_cpu;
	dma_addr_t	coded_iova;
	u32		coded_size;
	void		*coded_hdr_cpu;
	dma_addr_t	coded_hdr_iova;
	u32		coded_hdr_size;
	void		*psets_cpu;
	u32		psets_size;
	/*
	 * The DPB slots published at Start_AVC. The firmware rebuilds both the
	 * per-frame recon pointers and sLowResOutput.LowResSrcLumaScaled out of
	 * these (setRefPointers, fw 0x2c314-0x2c33c), so they are the ones that
	 * matter, not the PICMGMT copies.
	 */
	struct {
		dma_addr_t recon;
		dma_addr_t low_res;
	}		dpb[AVE_SESS_DPB_MAX];
	u32		n_dpb;
	u32		recon_size;	/* per slot */
	size_t		low_res_size;	/* per slot; 0 = none published */
	u32		low_res_stride;	/* for the log line only */
	u64		nbr[AVE_SRC_NBR_GROUPS][AVE_SRC_NBR_MAX];
	u32		n_nbr;

	/* debugfs: only created once a frame actually came back. */
	struct dentry		*dbg_dir;
	struct debugfs_blob_wrapper coded_blob, hdr_blob, psets_blob, h264_blob;
	void			*h264;		/* assembled Annex-B frame */
	size_t			h264_len;
};

/* DART-addressable data buffer in the device DMA domain (the coprocessor's
 * DART - same domain the firmware and FwIPC live in). */
static void *ave_sess_dma_alloc(struct ave_sess_bufs *b, size_t size,
				dma_addr_t *iova)
{
	void *cpu;

	if (b->ndma >= AVE_SESS_MAX_DMA)
		return NULL;
	cpu = dma_alloc_coherent(b->ave->dev, size, iova, GFP_KERNEL);
	if (!cpu)
		return NULL;
	/*
	 * SetTranscode programs only the low 32 bits of the coded address and
	 * size (fw str w10 0x592fc / 0x59310). Today the DART aperture is
	 * 32-bit so every IOVA fits, but nothing enforces that, and a buffer
	 * above 4 GiB would be silently truncated into someone else's mapping.
	 * (Review of 19b9d93, finding 5.)
	 */
	if ((u64)*iova + size > SZ_4G) {
		dev_err(b->ave->dev,
			"session: IOVA %pad +%#zx crosses 4 GiB; the firmware would truncate it\n",
			iova, size);
		dma_free_coherent(b->ave->dev, size, cpu, *iova);
		return NULL;
	}
	b->dma[b->ndma].cpu = cpu;
	b->dma[b->ndma].iova = *iova;
	b->dma[b->ndma].size = size;
	b->ndma++;
	return cpu;
}

/* Command buffer inside FwIPC; its IOVA is what ave_ipc_send() accepts. */
static void *ave_sess_ipc_alloc(struct ave_sess_bufs *b, size_t size,
				dma_addr_t *iova)
{
	void *cpu;

	if (b->nipc >= AVE_SESS_MAX_IPC)
		return NULL;
	cpu = ave_ipc_alloc(b->ave, size, iova);
	if (!cpu)
		return NULL;
	b->ipc[b->nipc].cpu = cpu;
	b->ipc[b->nipc].size = size;
	b->nipc++;
	return cpu;
}

/*
 * A 128-byte-aligned sub-allocation out of one coherent arena. Used for the
 * recon planes (the firmware asserts & 127 == 0 on all four, fw 0x55310 /
 * 0x5541c / 0x58068 / 0x54f94) and the SrcNeighbor slots (& 63 == 0).
 */
struct ave_sess_arena {
	void		*cpu;
	dma_addr_t	iova;
	size_t		size;
	size_t		used;
};

static void *ave_sess_arena_take(struct ave_sess_arena *a, size_t size,
				 dma_addr_t *iova)
{
	size_t off = ALIGN(a->used, 128);

	if (size > a->size || off > a->size - size)
		return NULL;
	a->used = off + size;
	*iova = a->iova + off;
	return a->cpu + off;
}

static void ave_sess_free_all(struct ave_sess_bufs *b)
{
	unsigned int i;

	debugfs_remove_recursive(b->dbg_dir);
	b->dbg_dir = NULL;
	vfree(b->h264);
	b->h264 = NULL;
	for (i = 0; i < b->ndma; i++)
		dma_free_coherent(b->ave->dev, b->dma[i].size,
				  b->dma[i].cpu, b->dma[i].iova);
	for (i = 0; i < b->nipc; i++)
		ave_ipc_free(b->ave, b->ipc[i].cpu, b->ipc[i].size);
	b->ndma = 0;
	b->nipc = 0;
}

/* ------------------------------------------------------------------------ */
/* One command: send, wait, decode                                          */
/* ------------------------------------------------------------------------ */

/*
 * Send a built command of @cmd_len bytes at FwIPC IOVA @cmd_iova on IO, wait
 * for the reply, and validate it. @client_id is the id the reply must echo (0
 * for the global Config). Returns 0 on an accepted reply, or a negative errno
 * (-ETIMEDOUT, -EPROTO, -EIO, or the send error). Always logs what happened.
 */
static int ave_session_cmd(struct ave_device *ave, const struct ave_cmd_abi *abi,
			   enum ave_op op, const char *name,
			   dma_addr_t cmd_iova, size_t cmd_len, u64 client_id)
{
	struct ave_sess_rx *rx = &ave_sess_rx;
	unsigned long left;
	u32 status = 0;
	int ret;

	reinit_completion(&rx->done);
	rx->size = 0;
	rx->flags = 0;
	rx->overflow = false;
	rx->ack_seen = false;
	rx->ack_size = 0;
	rx->other_id = 0;
	rx->other_count = 0;
	rx->want_id = abi->cmd[op].reply_id;

	dev_info(ave->dev, "session: %s: sending %zu bytes at IOVA %pad on IO\n",
		 name, cmd_len, &cmd_iova);

	ret = ave_ipc_send(ave, AVE_CH_IO, cmd_iova, cmd_len, 0);
	if (ret) {
		dev_err(ave->dev, "session: %s: ave_ipc_send failed: %d\n",
			name, ret);
		return ret;
	}

	left = wait_for_completion_timeout(&rx->done,
					   msecs_to_jiffies(AVE_SESS_TIMEOUT_MS));
	if (!left) {
		dev_err(ave->dev,
			"session: %s: TIMEOUT after %d ms - no id %#06x on IO_T2H (IO ack %s; %u other completion(s), last %#06x)\n",
			name, AVE_SESS_TIMEOUT_MS, rx->want_id,
			rx->ack_seen ? "did arrive: the firmware took the command"
				     : "did not arrive either",
			rx->other_count, rx->other_id);
		return -ETIMEDOUT;
	}
	/*
	 * Expected, not a failure: the firmware builds the completion inside
	 * the dispatcher (fw 0xa1cc8) and only echoes the command buffer
	 * afterwards (0xa1cf8), so when the two doorbells arrive as separate
	 * interrupts the completion wins the race and the ack lands just after
	 * this point. Logged at info for that reason.
	 */
	if (rx->other_count)
		dev_info(ave->dev,
			 "session: %s: skipped %u other completion(s), last id %#06x, while waiting for %#06x\n",
			 name, rx->other_count, rx->other_id, rx->want_id);
	if (!rx->ack_seen)
		dev_info(ave->dev,
			 "session: %s: completion arrived before the IO ack echo (expected ordering)\n",
			 name);
	if (!rx->size)
		dev_warn(ave->dev,
			 "session: %s: completion payload is not inside FwIPC; nothing to check\n",
			 name);

	/* The reply words. print4() would be nicer; keep it explicit. */
	dev_info(ave->dev,
		 "session: %s: reply %u bytes flags %#x%s: id=%#06x cid=%#x slot=%#x status=%#x\n",
		 name, rx->size, rx->flags, rx->overflow ? " (TRUNCATED)" : "",
		 rx->size >= 2 ? get_unaligned_le16(rx->buf) : 0,
		 rx->size >= 0x14 ? get_unaligned_le32(rx->buf + 0x10) : 0,
		 rx->size >= 0x20 ? get_unaligned_le32(rx->buf + 0x1c) : 0,
		 rx->size >= 0x3c ? get_unaligned_le32(rx->buf + 0x38) : 0);
	print_hex_dump(KERN_INFO, "session: reply: ", DUMP_PREFIX_OFFSET, 16, 1,
		       rx->buf, rx->size, false);

	ret = ave_cmd_check_reply(abi, op, rx->buf, rx->size, client_id, &status);
	if (ret == -EPROTO)
		dev_err(ave->dev,
			"session: %s: reply is not the answer to this command (wrong id/len/cid), status word %#x\n",
			name, status);
	else if (ret == -EIO)
		dev_err(ave->dev,
			"session: %s: firmware REJECTED the command, status %#x (success would be %#x)\n",
			name, status, abi->reply.status_ok);
	else if (ret)
		dev_err(ave->dev, "session: %s: reply check error %d\n", name, ret);
	else
		dev_info(ave->dev, "session: %s: ACCEPTED, status %#x\n",
			 name, status);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* The three commands                                                       */
/* ------------------------------------------------------------------------ */

static int ave_session_config(struct ave_device *ave,
			      const struct ave_cmd_abi *abi,
			      struct ave_sess_bufs *bufs)
{
	struct ave_config_params p = {};
	struct ave_cmd_ctx ctx = { .count = 1, .client_id = 0 };
	dma_addr_t cmd_iova, shmem_iova;
	size_t cmd_len;
	void *cmd, *shmem;
	int ret;

	cmd_len = ave_cmd_size(abi, AVE_OP_CONFIG);
	cmd = ave_sess_ipc_alloc(bufs, cmd_len, &cmd_iova);
	if (!cmd)
		return -ENOMEM;

	shmem = ave_sess_dma_alloc(bufs, AVE_SESS_SHMEM_SIZE, &shmem_iova);
	if (!shmem)
		return -ENOMEM;

	p.skip_mcpu = false;
	p.create_mcpu = true;			/* create the McpuController */
	p.reg_dart_addr = session_reg_dart;	/* 13.5 only; builder ignores on 26.6 */
	p.dsid = session_dsid;
	p.shmem_addr = shmem_iova;
	p.shmem_size = AVE_SESS_SHMEM_SIZE;

	/*
	 * 0 is correct here on 13.5: Config +0x48 reaches
	 * CFlowController::SetPipeClockGating (fw 0x3c8f8), which returns early
	 * on a gate byte no instruction in the image ever writes, and whose own
	 * assert names "pmgrAddr" - not the MappedMemory one we hit (docs/52).
	 * The parameter left overridable in case that changes.
	 */

	ret = ave_cmd_build_config(abi, cmd, cmd_len, &ctx, &p);
	if (ret < 0) {
		dev_err(ave->dev, "session: Config build failed: %d\n", ret);
		return ret;
	}
	dev_info(ave->dev,
		 "session: Config: shmem IOVA %pad size %#x, dsid %#x, reg_dart %#lx\n",
		 &shmem_iova, (u32)AVE_SESS_SHMEM_SIZE, session_dsid,
		 session_reg_dart);

	return ave_session_cmd(ave, abi, AVE_OP_CONFIG, "Config",
			       cmd_iova, cmd_len, 0);
}

static int ave_session_open(struct ave_device *ave,
			    const struct ave_cmd_abi *abi,
			    struct ave_sess_bufs *bufs, u64 client_id)
{
	struct ave_cmd_ctx ctx = { .count = 2, .client_id = client_id };
	dma_addr_t cmd_iova;
	size_t cmd_len;
	void *cmd;
	int ret;

	cmd_len = ave_cmd_size(abi, AVE_OP_OPEN);
	cmd = ave_sess_ipc_alloc(bufs, cmd_len, &cmd_iova);
	if (!cmd)
		return -ENOMEM;

	ret = ave_cmd_build_open(abi, cmd, cmd_len, &ctx);
	if (ret < 0) {
		dev_err(ave->dev, "session: Open build failed: %d\n", ret);
		return ret;
	}
	dev_info(ave->dev, "session: Open: client id %llu\n", client_id);

	return ave_session_cmd(ave, abi, AVE_OP_OPEN, "Open",
			       cmd_iova, cmd_len, client_id);
}

/*
 * Bytes one LRME scaled-source-luma surface needs for a @cw x @ch coded frame,
 * and the row stride the firmware will program for it. Both come from the
 * comment on session_lowres above; *stride is only for the log line.
 */
static size_t ave_session_lowres_size(u32 cw, u32 ch, u32 *stride)
{
	u32 lr_stride = ALIGN(4 * cw, 256);

	if (stride)
		*stride = lr_stride;
	return ALIGN((size_t)lr_stride * ((ch + 63) >> 4), 512);
}

/*
 * Carve the DPB slots Start_AVC publishes: one reconstruction surface and one
 * LowResRef surface per slot, out of two coherent arenas so the number of
 * mappings does not grow with session_dpb.
 *
 * Never fatal. Every failure here leaves a table entry zero, and a zero is a
 * named firmware assert (setLRME:5782 for the LowResRef) rather than a silent
 * fault - which is exactly what the session_lowres=0 control wants.
 */
static void ave_session_alloc_dpb(struct ave_device *ave,
				  struct ave_sess_bufs *bufs)
{
	struct ave_sess_arena recon = {}, low = {};
	size_t recon_slot, low_slot = 0;
	u32 cw, ch, lr_stride = 0;
	unsigned int i, n;

	cw = ave_mb_align(session_width);
	ch = ave_mb_align(session_height);

	n = session_dpb;
	if (!n || n > AVE_SESS_DPB_MAX) {
		dev_warn(ave->dev,
			 "session: session_dpb=%u out of range (1..%u); using 1\n",
			 session_dpb, AVE_SESS_DPB_MAX);
		n = 1;
	}

	/*
	 * Reconstruction surface: luma + chroma. setRefPointers derives
	 * sRecon.UV_MSB as luma + a firmware-computed offset (fw add x8,x12,x10
	 * 0x2c324), so the slot has to hold both planes contiguously; cw*ch*2
	 * is the docs/38 over-estimate this driver has used since Start_AVC was
	 * first accepted.
	 */
	recon_slot = ALIGN((size_t)cw * ch * 2, SZ_4K);
	recon.size = recon_slot * n;
	recon.cpu = ave_sess_dma_alloc(bufs, recon.size, &recon.iova);
	if (!recon.cpu) {
		dev_err(ave->dev,
			"session: DPB recon arena (%zu bytes) allocation failed\n",
			recon.size);
		return;
	}
	memset(recon.cpu, 0, recon.size);

	if (session_lowres) {
		low_slot = ave_session_lowres_size(cw, ch, &lr_stride);
		if (session_lowres_kb) {
			low_slot = (size_t)session_lowres_kb << 10;
			if (low_slot > SZ_64M) {
				dev_warn(ave->dev,
					 "session: session_lowres_kb=%u out of range; using the formula\n",
					 session_lowres_kb);
				low_slot = ave_session_lowres_size(cw, ch,
								   &lr_stride);
			}
		}
		low_slot = ALIGN(low_slot, SZ_4K);
		low.size = low_slot * n;
		low.cpu = ave_sess_dma_alloc(bufs, low.size, &low.iova);
		if (!low.cpu) {
			dev_warn(ave->dev,
				 "session: LowResRef arena (%zu bytes) allocation failed; expect ASSERT CAVCController_H13C.cpp:5782\n",
				 low.size);
			low_slot = 0;
		} else {
			memset(low.cpu, 0, low.size);
		}
	} else {
		dev_warn(ave->dev,
			 "session: session_lowres=0: the LowResRef table and LowResSrcLumaScaled are left zero, expect ASSERT CAVCController_H13C.cpp:5782\n");
	}

	for (i = 0; i < n; i++) {
		dma_addr_t r, l = 0;

		if (!ave_sess_arena_take(&recon, recon_slot, &r))
			break;
		if (low_slot && !ave_sess_arena_take(&low, low_slot, &l))
			l = 0;
		/*
		 * The firmware asserts & 127 == 0 on the recon planes
		 * (fw 0x55310 / 0x54f94 / 0x58068 / 0x5541c) and & 63 == 0 on
		 * the LowResRef (fw 0x523dc, setLRME:5783). Both arenas are
		 * page-aligned and both slot sizes are 4 KiB multiples, so this
		 * holds by construction - check it rather than assume it.
		 */
		if ((r & 127) || (l & (AVE_STRIDE_ALIGN - 1))) {
			dev_err(ave->dev,
				"session: DPB slot %u misaligned (recon %pad, lowres %pad)\n",
				i, &r, &l);
			break;
		}
		bufs->dpb[i].recon = r;
		bufs->dpb[i].low_res = l;
		bufs->n_dpb = i + 1;
	}

	bufs->recon_size = recon_slot;
	bufs->low_res_size = low_slot;
	bufs->low_res_stride = lr_stride;

	dev_info(ave->dev,
		 "session: DPB %u slot(s): recon %pad +%#zx each; LowResRef %pad +%#zx each, lr_stride %u, %u rows%s\n",
		 bufs->n_dpb, &recon.iova, recon_slot,
		 &low.iova, low_slot, lr_stride, (ch + 63) >> 4,
		 !low_slot ? " (NOT PUBLISHED)"
			   : session_lowres_kb
			     ? " (size overridden by session_lowres_kb)"
			     : " (AVE_CalcBufSizeOfLowResRef formula)");
}

static int ave_session_start_avc(struct ave_device *ave,
				 const struct ave_cmd_abi *abi,
				 struct ave_sess_bufs *bufs, u64 client_id)
{
	struct ave_cmd_ctx ctx = { .count = 3, .client_id = client_id };
	struct ave_avc_session s = {};
	struct ave_recon_buf recon[AVE_SESS_DPB_MAX];
	struct ave_buf coded, coded_hdr;
	dma_addr_t cmd_iova, fwc_iova, fwcm_iova, coded_iova, hdr_iova;
	dma_addr_t psets_iova;
	u32 cw, ch, fwc_size, i;
	void *coded_cpu, *hdr_cpu, *psets_cpu;
	size_t cmd_len;
	void *cmd;
	int ret;

	cmd_len = ave_cmd_size(abi, AVE_OP_START_AVC);
	cmd = ave_sess_ipc_alloc(bufs, cmd_len, &cmd_iova);
	if (!cmd)
		return -ENOMEM;

	/* MB-aligned coded geometry, for the reconstruction buffer sizing. */
	cw = ave_mb_align(session_width);
	ch = ave_mb_align(session_height);

	/* ave_session_alloc_dpb() must already have run: the recon and
	 * LowResRef tables are published in this command. */
	if (!bufs->n_dpb)
		return -ENOMEM;

	fwc_size = ave->client_buf_size ? ave->client_buf_size
					: AVE_SESS_FWCLIENT_FALLBACK;

	if (!ave_sess_dma_alloc(bufs, fwc_size, &fwc_iova) ||
	    !ave_sess_dma_alloc(bufs, AVE_SESS_FWCLIENTMEM_SIZE, &fwcm_iova))
		return -ENOMEM;
	coded_cpu = ave_sess_dma_alloc(bufs, AVE_SESS_CODED_SIZE, &coded_iova);
	hdr_cpu = ave_sess_dma_alloc(bufs, abi->start_avc.coded_hdr_bytes,
				     &hdr_iova);
	psets_cpu = ave_sess_dma_alloc(bufs, AVE_SESS_PARAM_SETS_SIZE,
				       &psets_iova);
	if (!coded_cpu || !hdr_cpu || !psets_cpu)
		return -ENOMEM;

	/*
	 * dma_alloc_coherent already hands back zeroed memory, but the frame
	 * step relies on that to find the end of the SPS+PPS the firmware
	 * writes here (it reports the length only in bits, in a controller
	 * field we cannot read), so make the assumption explicit.
	 */
	memset(psets_cpu, 0, AVE_SESS_PARAM_SETS_SIZE);
	memset(hdr_cpu, 0, abi->start_avc.coded_hdr_bytes);
	memset(coded_cpu, 0, AVE_SESS_CODED_SIZE);

	bufs->coded_cpu = coded_cpu;
	bufs->coded_iova = coded_iova;
	bufs->coded_size = AVE_SESS_CODED_SIZE;
	bufs->coded_hdr_cpu = hdr_cpu;
	bufs->coded_hdr_iova = hdr_iova;
	bufs->coded_hdr_size = abi->start_avc.coded_hdr_bytes;
	bufs->psets_cpu = psets_cpu;
	bufs->psets_size = AVE_SESS_PARAM_SETS_SIZE;
	memset(recon, 0, sizeof(recon));
	for (i = 0; i < bufs->n_dpb; i++) {
		recon[i].addr = bufs->dpb[i].recon;
		/* used only where the ABI's recon_size != NONE (26.6.2) */
		recon[i].luma_size = cw * ch;
	}
	coded.addr = coded_iova;
	coded.size = AVE_SESS_CODED_SIZE;
	coded_hdr.addr = hdr_iova;
	coded_hdr.size = abi->start_avc.coded_hdr_bytes;

	s.width = session_width;
	s.height = session_height;
	s.frame_rate = 30;
	s.bitrate = 0;				/* fixed QP */
	s.qp_i = s.qp_p = s.qp_b = session_qp;
	s.key_interval = 1;			/* every frame an IDR (I-only) */
	s.profile_idc = 66;			/* Baseline */
	s.level_idc = 40;			/* 4.0 - covers 1080p */
	s.cabac = false;			/* CAVLC (required with Baseline) */

	s.fw_client_addr = fwc_iova;
	s.fw_client_size = fwc_size;
	s.fw_client_mem_addr = fwcm_iova;
	s.fw_client_mem_size = AVE_SESS_FWCLIENTMEM_SIZE;

	s.param_sets_addr = psets_iova;
	s.param_sets_size = AVE_SESS_PARAM_SETS_SIZE;

	s.recon = recon;
	s.n_recon = bufs->n_dpb;

	/*
	 * The LowResRef table. All or nothing: the builder refuses a partial
	 * one, because a slot the firmware selects with a zero here asserts at
	 * setLRME:5782 and a slot published without its recon peer would be a
	 * pointer with no frame behind it.
	 */
	if (abi->start_avc.low_res_ref_set != AVE_OFF_NONE && bufs->low_res_size) {
		for (i = 0; i < bufs->n_dpb; i++)
			s.low_res_ref[i] = bufs->dpb[i].low_res;
		s.n_low_res_ref = bufs->n_dpb;
		for (i = 0; i < bufs->n_dpb; i++)
			if (!s.low_res_ref[i]) {
				/* A hole would be refused by the builder and
				 * take the whole command down; drop the table
				 * instead and let setLRME name the field. */
				dev_warn(ave->dev,
					 "session: DPB slot %u has no LowResRef; dropping the whole table, expect ASSERT CAVCController_H13C.cpp:5782\n",
					 i);
				s.n_low_res_ref = 0;
				break;
			}
	}
	s.coded = &coded;
	s.coded_hdr = &coded_hdr;
	s.n_coded = 1;

	/*
	 * SrcNeighbor scratch. Only published for the frame run: sending it on
	 * a Start that is not followed by Process changes a sequence that is
	 * already known to be accepted, for no gain.
	 */
	if (bufs->n_nbr && abi->start_avc.src_nbr_max) {
		memcpy(s.src_nbr, bufs->nbr, sizeof(s.src_nbr));
		s.n_src_nbr = min(bufs->n_nbr, abi->start_avc.src_nbr_max);
	}

	ret = ave_cmd_build_start_avc(abi, cmd, cmd_len, &ctx, &s);
	if (ret < 0) {
		dev_err(ave->dev, "session: Start_AVC build failed: %d\n", ret);
		return ret;
	}
	dev_info(ave->dev,
		 "session: Start_AVC: %ux%u (coded %ux%u) QP %u I-only, profile 66 level 40\n",
		 session_width, session_height, cw, ch, session_qp);
	dev_info(ave->dev,
		 "session: Start_AVC: fw_client %pad/%#x mem %pad/%#x coded %pad/%#x hdr %pad/%#x psets %pad/%#x\n",
		 &fwc_iova, fwc_size, &fwcm_iova, (u32)AVE_SESS_FWCLIENTMEM_SIZE,
		 &coded_iova, (u32)AVE_SESS_CODED_SIZE,
		 &hdr_iova, abi->start_avc.coded_hdr_bytes,
		 &psets_iova, (u32)AVE_SESS_PARAM_SETS_SIZE);
	for (i = 0; i < bufs->n_dpb; i++)
		dev_info(ave->dev,
			 "session: Start_AVC: DPB slot %u: recon %pad at wire %#x, LowResRef %pad at wire %#x\n",
			 i, &bufs->dpb[i].recon,
			 abi->start_avc.recon_set +
				 i * abi->start_avc.recon_stride,
			 &bufs->dpb[i].low_res,
			 abi->start_avc.low_res_ref_set == AVE_OFF_NONE ? 0 :
				 abi->start_avc.low_res_ref_set +
				 i * abi->start_avc.low_res_ref_stride);
	if (abi->start_avc.low_res_ref_set == AVE_OFF_NONE)
		dev_warn(ave->dev,
			 "session: Start_AVC: ABI %s has no LowResRef table; sLowResOutput.LowResSrcLumaScaled will be whatever setRefPointers finds in the DPB\n",
			 abi->name);
	if (s.n_src_nbr)
		dev_info(ave->dev,
			 "session: Start_AVC: SrcNeighbor %u entries/group at %#llx %#llx %#llx %#llx (+%u KiB each)\n",
			 s.n_src_nbr, s.src_nbr[0][0], s.src_nbr[1][0],
			 s.src_nbr[2][0], s.src_nbr[3][0], session_nbr_kb);
	else
		dev_warn(ave->dev,
			 "session: Start_AVC: SrcNeighbor tables left ZERO - the first Process will assert at setPipe:6990 if it gets that far\n");

	return ave_session_cmd(ave, abi, AVE_OP_START_AVC, "Start_AVC",
			       cmd_iova, cmd_len, client_id);
}

/* ------------------------------------------------------------------------ */
/* Phase 6 - one encoded frame                                              */
/* ------------------------------------------------------------------------ */

/*
 * Allocate the SrcNeighbor arena and hand out 16 64-byte-aligned slots. Done
 * before Start_AVC because those tables are published there as well as per
 * frame. Failure is not fatal: the run continues with the tables zero and the
 * firmware's own assert names the field.
 */
static void ave_session_alloc_nbr(struct ave_device *ave,
				  struct ave_sess_bufs *bufs)
{
	static const unsigned int per_mb_col[AVE_SRC_NBR_GROUPS] = {
		256, 1024, 56, 64,	/* Info, Pixel, Data, FwData - docs/47 */
	};
	struct ave_sess_arena a = {};
	unsigned int mb_cols = ave_mb_align(session_width) / 16;
	size_t slot = (size_t)session_nbr_kb << 10;
	unsigned int g, i;

	if (!session_nbr || !session_frame)
		return;
	if (!slot) {
		/* Largest group's requirement, so one slot size fits all four. */
		slot = SZ_16K;
		for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
			slot = max_t(size_t, slot,
				     (size_t)per_mb_col[g] * mb_cols);
		slot = ALIGN(slot, SZ_16K);
		dev_info(ave->dev,
			 "session: SrcNeighbor slot %zu KiB for %u MB columns (docs/47 formula)\n",
			 slot >> 10, mb_cols);
	}
	if (slot > SZ_16M) {
		dev_warn(ave->dev, "session: session_nbr_kb=%u out of range\n",
			 session_nbr_kb);
		return;
	}
	a.size = ALIGN(slot, 128) * AVE_SESS_NBR_SLOTS + 128;
	a.cpu = ave_sess_dma_alloc(bufs, a.size, &a.iova);
	if (!a.cpu) {
		dev_warn(ave->dev,
			 "session: SrcNeighbor arena (%zu bytes) allocation failed\n",
			 a.size);
		return;
	}
	memset(a.cpu, 0, a.size);

	for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
		for (i = 0; i < AVE_SRC_NBR_MAX; i++) {
			dma_addr_t iova;

			if (!ave_sess_arena_take(&a, slot, &iova))
				return;		/* keeps what it managed */
			bufs->nbr[g][i] = iova;
		}
	bufs->n_nbr = AVE_SRC_NBR_MAX;
}

/*
 * A deterministic, legal, non-uniform NV12 frame: a horizontal luma ramp over
 * the legal 16..235 range that also steps per macroblock row, near-grey chroma
 * with a slow horizontal Cr drift. The content does not matter; what matters
 * is that it is not flat (a flat frame compresses to almost nothing and would
 * make "the output is 30 bytes" ambiguous) and that a decoded PNG of it is
 * recognisable by eye.
 *
 * The allocation is MB-aligned in height because the encoder fetches
 * 16*ceil(H/16) luma rows regardless of the declared height (docs/38 §5); an
 * allocation sized to the display height is short by up to 15 rows and the
 * read runs off the end of the DART mapping.
 */
static void ave_session_fill_input(u8 *luma, u8 *chroma, u32 stride,
				   u32 w, u32 h)
{
	u32 x, y;

	for (y = 0; y < h; y++) {
		u8 *row = luma + (size_t)y * stride;

		for (x = 0; x < w; x++)
			row[x] = (u8)(16 + ((x * 219) / (w ? w : 1)) +
				      ((y / AVE_MB_SIZE) & 7));
		if (stride > w)
			memset(row + w, 0, stride - w);
	}
	for (y = 0; y < h / 2; y++) {
		u8 *row = chroma + (size_t)y * stride;

		for (x = 0; x < w; x += 2) {
			row[x] = 128;				/* Cb */
			row[x + 1] = (u8)(128 + ((x / 32) & 15) - 8);	/* Cr */
		}
		if (stride > w)
			memset(row + w, 0, stride - w);
	}
}

/*
 * Length of the SPS+PPS the firmware wrote into the parameter-sets buffer.
 *
 * The firmware memcpy()s SPS then PPS into it at Start_AVC (fw 0x5df5c /
 * 0x5df78) and records the total only as a bit count in a controller field
 * ([x19+2740], fw 0x5df9c) that never reaches the host. The buffer was zeroed
 * before Start, and an H.264 NAL always ends with the rbsp_stop_one_bit, so
 * the last non-zero byte is the last byte of the PPS. Emulation prevention
 * guarantees no three consecutive zero bytes inside, so scanning back from the
 * end cannot stop early.
 */
static size_t ave_session_psets_len(const u8 *buf, size_t size)
{
	while (size && !buf[size - 1])
		size--;
	return size;
}

/* Annex-B 4-byte start code + nal_unit_type, as WriteBits::nal_header emits. */
static int ave_session_nal_type(const u8 *buf, size_t len)
{
	if (len < 5 || buf[0] || buf[1] || buf[2] || buf[3] != 1)
		return -1;
	return buf[4] & 0x1f;
}

static void ave_session_publish(struct ave_device *ave,
				struct ave_sess_bufs *bufs, u32 coded_len,
				size_t psets_len)
{
	int coded_nal = ave_session_nal_type(bufs->coded_cpu, coded_len);
	bool need_psets = coded_nal != 7;	/* 7 = SPS */
	size_t total = coded_len + (need_psets ? psets_len : 0);

	bufs->dbg_dir = debugfs_create_dir("apple_ave", NULL);
	if (IS_ERR(bufs->dbg_dir)) {
		dev_warn(ave->dev, "session: debugfs dir failed: %pe\n",
			 bufs->dbg_dir);
		bufs->dbg_dir = NULL;
		return;
	}

	bufs->coded_blob.data = bufs->coded_cpu;
	bufs->coded_blob.size = coded_len;
	debugfs_create_blob("coded.bin", 0444, bufs->dbg_dir, &bufs->coded_blob);

	bufs->hdr_blob.data = bufs->coded_hdr_cpu;
	bufs->hdr_blob.size = bufs->coded_hdr_size;
	debugfs_create_blob("coded_hdr.bin", 0444, bufs->dbg_dir,
			    &bufs->hdr_blob);

	bufs->psets_blob.data = bufs->psets_cpu;
	bufs->psets_blob.size = psets_len;
	debugfs_create_blob("paramsets.bin", 0444, bufs->dbg_dir,
			    &bufs->psets_blob);

	/*
	 * The assembled elementary stream. The coded buffer holds the slice
	 * NAL(s) only - the firmware writes SPS+PPS to the parameter-sets
	 * buffer at Start and reports their length in the coded header
	 * (ui32_SPSPPSHeaderBits) purely for rate-control accounting - so they
	 * are prepended here. If the bitstream turns out to start with an SPS
	 * after all, it is emitted unchanged and the log says so.
	 */
	if (!total)
		return;
	bufs->h264 = vmalloc(total);
	if (!bufs->h264)
		return;
	if (need_psets) {
		memcpy(bufs->h264, bufs->psets_cpu, psets_len);
		memcpy(bufs->h264 + psets_len, bufs->coded_cpu, coded_len);
	} else {
		memcpy(bufs->h264, bufs->coded_cpu, coded_len);
	}
	bufs->h264_len = total;
	bufs->h264_blob.data = bufs->h264;
	bufs->h264_blob.size = total;
	debugfs_create_blob("frame.h264", 0444, bufs->dbg_dir,
			    &bufs->h264_blob);

	dev_info(ave->dev,
		 "session: frame: /sys/kernel/debug/apple_ave/frame.h264 = %zu bytes (%s; coded starts with NAL type %d)\n",
		 total,
		 need_psets ? "SPS+PPS prepended from paramsets.bin"
			    : "coded buffer already carries the parameter sets",
		 coded_nal);
}

static int ave_session_process(struct ave_device *ave,
			       const struct ave_cmd_abi *abi,
			       struct ave_sess_bufs *bufs, u64 client_id)
{
	struct ave_cmd_ctx ctx = { .count = 4, .client_id = client_id };
	struct ave_avc_frame f = {};
	struct ave_coded_info info;
	struct ave_sess_arena recon = {};
	dma_addr_t cmd_iova, luma_iova, chroma_iova;
	dma_addr_t ry, ruv, ry_lsb, ruv_lsb, rmv;
	u32 cw, ch, stride, luma_bytes, chroma_bytes, mb_w, mb_h;
	size_t cmd_len, psets_len;
	u8 *luma, *chroma;
	void *cmd;
	int ret;

	if (!bufs->coded_cpu)
		return -EINVAL;

	cw = ave_mb_align(session_width);
	ch = ave_mb_align(session_height);
	mb_w = cw / AVE_MB_SIZE;
	mb_h = ch / AVE_MB_SIZE;

	/*
	 * Stride must be non-zero and a multiple of 64 (kext 0xeb0768, and the
	 * firmware re-checks the addresses at setPipe 6440/6454). The coded
	 * width is already a multiple of 16; round it to 64.
	 */
	stride = ALIGN(cw, AVE_STRIDE_ALIGN);
	luma_bytes = stride * ave_src_luma_rows(ch);
	chroma_bytes = stride * ave_src_chroma_rows(ch);

	luma = ave_sess_dma_alloc(bufs, luma_bytes, &luma_iova);
	chroma = ave_sess_dma_alloc(bufs, chroma_bytes, &chroma_iova);
	if (!luma || !chroma)
		return -ENOMEM;
	if ((luma_iova | chroma_iova) & (AVE_STRIDE_ALIGN - 1)) {
		dev_err(ave->dev,
			"session: frame: input planes not 64-aligned (%pad / %pad)\n",
			&luma_iova, &chroma_iova);
		return -EINVAL;
	}
	ave_session_fill_input(luma, chroma, stride, cw, ch);

	/*
	 * Reconstruction planes. setPipe asserts all four are non-zero and
	 * 128-aligned behind their gates (fw 0x55358/0x55310 Y_MSB,
	 * 0x580b0/0x58068 UV_MSB, 0x550e4/0x54f94 Y_LSB, 0x55978/0x5541c
	 * UV_LSB), plus a colocated MV store. The Start-time DPB entry points
	 * at the same arena; the sizes below are the docs/38 §7 tile formula
	 * rounded up, which is an over-estimate and therefore safe.
	 */
	recon.size = (size_t)cw * ch * 3 + (size_t)mb_w * mb_h * 1024 + SZ_64K;
	recon.cpu = ave_sess_dma_alloc(bufs, recon.size, &recon.iova);
	if (!recon.cpu)
		return -ENOMEM;
	memset(recon.cpu, 0, recon.size);
	if (!ave_sess_arena_take(&recon, (size_t)cw * ch, &ry) ||
	    !ave_sess_arena_take(&recon, (size_t)cw * ch / 2, &ruv) ||
	    !ave_sess_arena_take(&recon, (size_t)cw * ch, &ry_lsb) ||
	    !ave_sess_arena_take(&recon, (size_t)cw * ch / 2, &ruv_lsb) ||
	    !ave_sess_arena_take(&recon, (size_t)mb_w * mb_h * 1024, &rmv))
		return -ENOMEM;

	cmd_len = ave_cmd_size(abi, AVE_OP_PROCESS_AVC);
	cmd = ave_sess_ipc_alloc(bufs, cmd_len, &cmd_iova);
	if (!cmd)
		return -ENOMEM;

	f.frame_type = session_frame_type;
	f.in_luma_addr = luma_iova;
	f.in_luma_stride = stride;
	f.in_luma_size = luma_bytes;
	f.in_chroma_addr = chroma_iova;
	f.in_chroma_stride = stride;
	f.in_chroma_size = chroma_bytes;

	/* out_mode is left 0, so the size check is against the Start table. */
	f.coded_index = 0;
	f.coded_addr = bufs->coded_iova;
	f.coded_hdr_addr = bufs->coded_hdr_iova;
	f.coded_size = bufs->coded_size;

	f.recon_luma_addr = ry;
	f.recon_chroma_addr = ruv;
	f.recon_luma_lsb_addr = ry_lsb;
	f.recon_chroma_lsb_addr = ruv_lsb;
	f.recon_mv_addr = rmv;

	f.ctx_index = 0;
	f.force_key_frame = session_frame_type == AVE_FRAME_TYPE_IDR;
	f.update_param_sets = false;

	/*
	 * The LRME pass runs even for an I-frame with no references, and
	 * setLRME asserts on its scaled-luma target. LowResResults[] stay zero
	 * on purpose: every firmware read of them is cbz-skipped and only the
	 * alignment is checked when non-zero (fw 0x51e88 / 0x51ed8 / 0x52050 /
	 * 0x520a4), so publishing an address there would only add a way to be
	 * wrong.
	 */
	if (abi->process_avc.low_res_src != AVE_OFF_NONE)
		f.low_res_src_addr = bufs->dpb[0].low_res;

	if (bufs->n_nbr && abi->process_avc.src_nbr_max) {
		memcpy(f.src_nbr, bufs->nbr, sizeof(f.src_nbr));
		f.n_src_nbr = min(bufs->n_nbr, abi->process_avc.src_nbr_max);
	}

	ret = ave_cmd_build_process_avc(abi, cmd, cmd_len, &ctx,
					AVE_SESS_PROCESS_SLOT, &f);
	if (ret < 0) {
		dev_err(ave->dev, "session: Process build failed: %d\n", ret);
		return ret;
	}

	dev_info(ave->dev,
		 "session: Process: %ux%u coded, stride %u, luma %pad/%#x chroma %pad/%#x, frame_type %u, slot %u\n",
		 cw, ch, stride, &luma_iova, luma_bytes, &chroma_iova,
		 chroma_bytes, session_frame_type, AVE_SESS_PROCESS_SLOT);
	dev_info(ave->dev,
		 "session: Process: coded %pad/%#x hdr %pad/%#x recon Y %pad UV %pad MV %pad\n",
		 &bufs->coded_iova, bufs->coded_size, &bufs->coded_hdr_iova,
		 bufs->coded_hdr_size, &ry, &ruv, &rmv);
	dev_info(ave->dev,
		 "session: Process: LowResSrcLumaScaled %#llx at PICMGMT+%#x - INERT, setRefPointers overwrites it from DPB slot 0 (fw 0x2c320); LowResResults left zero\n",
		 f.low_res_src_addr, abi->process_avc.low_res_src);

	ret = ave_session_cmd(ave, abi, AVE_OP_PROCESS_AVC, "Process",
			      cmd_iova, cmd_len, client_id);
	if (ret)
		return ret;

	/*
	 * The completion says nothing about the length; everything comes out
	 * of the coded-header buffer (docs/53 §3).
	 */
	dma_rmb();
	print_hex_dump(KERN_INFO, "session: coded_hdr: ", DUMP_PREFIX_OFFSET,
		       16, 4, bufs->coded_hdr_cpu, 0x40, false);
	print_hex_dump(KERN_INFO, "session: slice0: ", DUMP_PREFIX_OFFSET,
		       16, 4,
		       (u8 *)bufs->coded_hdr_cpu + abi->coded_hdr.slice_bytes_written,
		       0x20, false);

	ret = ave_cmd_coded_length(abi, bufs->coded_hdr_cpu,
				   bufs->coded_hdr_size, &info);
	if (ret) {
		dev_err(ave->dev,
			"session: frame: cannot decode the coded header: %d\n",
			ret);
		return ret;
	}
	dev_info(ave->dev,
		 "session: frame: %u bytes in %u slice(s) (written %u - trimmed %u), FrameTypeReturned %u, frame_num %u, SPS+PPS %u bits\n",
		 info.bytes, info.slices, info.bytes + info.bytes_removed,
		 info.bytes_removed, info.frame_type, info.frame_num,
		 info.sps_pps_bits);

	if (!info.bytes) {
		dev_err(ave->dev,
			"session: frame: the firmware reported ZERO coded bytes - the encode did not produce a bitstream\n");
		return -ENODATA;
	}
	if (info.bytes > bufs->coded_size) {
		dev_err(ave->dev,
			"session: frame: reported length %u exceeds the coded buffer (%u); header is not what we think it is\n",
			info.bytes, bufs->coded_size);
		return -EPROTO;
	}

	print_hex_dump(KERN_INFO, "session: coded: ", DUMP_PREFIX_OFFSET,
		       16, 1, bufs->coded_cpu, min_t(u32, info.bytes, 64),
		       false);

	psets_len = ave_session_psets_len(bufs->psets_cpu, bufs->psets_size);
	dev_info(ave->dev,
		 "session: frame: parameter sets %zu bytes (firmware said %u bits = %u bytes), first NAL type %d\n",
		 psets_len, info.sps_pps_bits, info.sps_pps_bits / 8,
		 ave_session_nal_type(bufs->psets_cpu, psets_len));
	print_hex_dump(KERN_INFO, "session: psets: ", DUMP_PREFIX_OFFSET,
		       16, 1, bufs->psets_cpu, min_t(size_t, psets_len, 64),
		       false);

	ave_session_publish(ave, bufs, info.bytes, psets_len);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

void ave_session_release(struct ave_device *ave)
{
	struct ave_sess_bufs *bufs = ave->session_bufs;

	if (!bufs)
		return;
	ave->session_bufs = NULL;
	ave_sess_free_all(bufs);
	kfree(bufs);
}

int ave_session_selftest(struct ave_device *ave)
{
	const struct ave_cmd_abi *abi;
	struct ave_sess_bufs *bufs;
	void (*prev_rx)(struct ave_device *, u32, void *, u32, u32);
	int ret;

	if (!session_selftest && !session_frame)
		return 0;

	if (!ave->running) {
		dev_warn(ave->dev, "session: coprocessor not running; skipping\n");
		return -ENODEV;
	}

	abi = ave_cmd_abi_get(ave->fw_abi);
	if (!abi) {
		dev_err(ave->dev,
			"session: no command ABI for fw_abi %d; refusing to guess\n",
			ave->fw_abi);
		return -ENODEV;
	}
	if (session_qp > 51) {
		dev_err(ave->dev, "session: session_qp %u out of range (0..51)\n",
			session_qp);
		return -EINVAL;
	}

	dev_info(ave->dev, "session: self-test start (ABI %s)\n", abi->name);

	/*
	 * Owned by the device, not by this function. Config hands the firmware
	 * a shared-memory region it carves into four (fw ProcessConfig 0xe5e4
	 * -> PlatformIOPIPCManager::AddSharedMemory 0xaa85c), and Start_AVC
	 * hands it the client, recon and coded buffers. The firmware keeps
	 * those addresses; freeing them here - with the core still running -
	 * would unmap live IOVAs and invite the fault storm docs/31 measured.
	 * ave_remove() frees them after ave_power_off(). (Review finding 2.)
	 */
	bufs = kzalloc(sizeof(*bufs), GFP_KERNEL);
	if (!bufs)
		return -ENOMEM;
	bufs->ave = ave;
	ave_session_release(ave);	/* a previous run's, if any */
	ave->session_bufs = bufs;

	init_completion(&ave_sess_rx.done);

	/* Publish the capturing hook to the IRQ handler before the first send. */
	prev_rx = ave->ipc_rx;
	smp_store_release(&ave->ipc_rx, ave_session_ipc_rx);

	ret = ave_session_config(ave, abi, bufs);
	if (ret)
		goto out;

	ret = ave_session_open(ave, abi, bufs, AVE_SESS_CLIENT_ID);
	if (ret)
		goto out;

	/* Must precede Start_AVC: these tables are published in that command. */
	ave_session_alloc_nbr(ave, bufs);
	ave_session_alloc_dpb(ave, bufs);

	ret = ave_session_start_avc(ave, abi, bufs, AVE_SESS_CLIENT_ID);
	if (ret || !session_frame)
		goto out;

	if (!ave_cmd_size(abi, AVE_OP_PROCESS_AVC)) {
		dev_err(ave->dev,
			"session: no Process command for ABI %s\n", abi->name);
		ret = -ENODEV;
		goto out;
	}
	if (!abi->coded_hdr.slice_stride)
		dev_warn(ave->dev,
			 "session: ABI %s has no coded-header layout; the frame length will not be recoverable\n",
			 abi->name);

	ret = ave_session_process(ave, abi, bufs, AVE_SESS_CLIENT_ID);

out:
	/* Stop the hook before freeing the buffers replies were written into. */
	smp_store_release(&ave->ipc_rx, prev_rx);
	/*
	 * A reply could still be in flight after a timeout; give the IRQ a
	 * moment to run against the (now restored) previous hook rather than a
	 * freed buffer, then drop everything.
	 */
	synchronize_irq(ave->irq);
	/*
	 * The buffers stay mapped: the firmware still holds their addresses.
	 * ave_remove() releases them once the core is powered off.
	 */

	dev_info(ave->dev, "session: self-test %s (%d)\n",
		 ret ? "FAILED"
		     : (session_frame ? "encoded one frame OK"
				      : "reached Start_AVC OK"), ret);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Halt: stop the core without rebooting the machine                        */
/* ------------------------------------------------------------------------ */

/*
 * venc_sys no longer gates off under the patched m1n1, so once the core has
 * been started nothing in Linux can stop it: CPU_CONTROL = 0 does not stop a
 * started core, and the next insmod finds it running on drifted DATA. That is
 * why every experiment has cost a reboot.
 *
 * macOS does have a way, and it is a firmware command rather than a register
 * poke: AVE_HwC::SendFwCmd_Halt (kext 0xfffffe0008efc600) builds command id
 * 14 and sends it on IO, then AVE_IOP::Stop polls for idle. An exhaustive
 * scan of the 13.5 kext found no other stop mechanism - nothing outside
 * AVE_IOP_Start_* ever writes CPU_CONTROL. Full trace in docs/55.
 *
 * Three things about this command are unlike every other one we send:
 *
 *  - **It never replies.** ProcessPowerDown tail-branches into an infinite
 *    wfi loop (fw 0xa689c), so the dispatcher's reply epilogue is
 *    unreachable and NotificationToHost is never called. Waiting for a
 *    completion would time out on a *successful* halt, so we do not wait.
 *  - **The completion signal is SVE scratch 0**, which the firmware sets to
 *    AVE_SCRATCH0_STOPPED one instruction before the wfi (fw 0xa6898).
 *  - **That value is already there.** StartUpIOP writes it at boot and it
 *    stays. Polling without clearing it first is a check that can never say
 *    "no" - it would report success whether or not the firmware ever saw the
 *    command. macOS clears it immediately before the send (kext
 *    0xfffffe0008efc704) and so do we, refusing to send if the clear does not
 *    stick.
 *
 * Only the u16 id at +0 and the 0x40 length are load-bearing: the firmware
 * asserts the length (fw 0xde08) and ProcessPowerDown never dereferences the
 * command body. The builder already emits exactly this shape, so there is no
 * Halt-specific wire code here.
 *
 * Known deviation from macOS: ShutDownIOP drops the PMGR power states and
 * clock gating before the Halt. We cannot do that from here, and the purpose
 * is unknown; if it matters it should show up as one of the polls timing out
 * rather than as a hang.
 */
static bool fw_halt;
module_param(fw_halt, bool, 0444);
MODULE_PARM_DESC(fw_halt,
		 "at unload, ask the firmware to halt (command 14) so the next load can start it again without a reboot");

/* The advisory _S_AVE_TimeOut ms field; macOS computes cfg[+20]*3000. */
#define AVE_HALT_CMD_TIMEOUT_MS	3000
/* How long we give scratch 0 to change, and CPU_STATUS to settle after. */
#define AVE_HALT_SCRATCH_US	(1000 * 1000)
#define AVE_HALT_IDLE_SAMPLES	3

bool ave_session_halt_requested(void)
{
	return fw_halt;
}

int ave_session_halt(struct ave_device *ave)
{
	const struct ave_cmd_abi *abi = ave_cmd_abi_get(ave->fw_abi);
	struct ave_cmd_ctx ctx = { .count = 0, .client_id = 0 };
	struct device *dev = ave->dev;
	dma_addr_t cmd_iova;
	unsigned int i, idle;
	size_t cmd_len;
	u8 *cmd;
	u32 v;
	int ret;

	if (!fw_halt)
		return 0;
	/*
	 * Everything below touches AVE registers, and a register access in a
	 * gated block hangs the fabric (docs/24, docs/25 7a). ave_ipc_send()
	 * checks the transport, but that is too late - the scratch write comes
	 * first. stop_after=15 drops power inside probe, so without this gate
	 * an rmmod with fw_halt=1 would write bank 2 in a gated block; with
	 * stop_after=0 the bank base is NULL and it would oops inside rmmod.
	 *
	 * ave->running is also the driver's stand-in for the precondition
	 * macOS enforces as AVE_HwC::m_state == 3, "handshake completed"
	 * (docs/55 §2.1): Halt is only defined for a firmware that is up.
	 * (Review 2026-09-13, finding 1.)
	 */
	if (!ave->powered || !ave->running ||
	    !ave->bank[AVE_BANK_SVE].base || !ave->bank[AVE_BANK_ASC].base) {
		dev_info(dev, "halt: firmware is not up (powered %d, running %d); nothing to halt\n",
			 ave->powered, ave->running);
		return -ENODEV;
	}
	/* Advisory, but macOS fills it in; match the shape. */
	put_unaligned_le32(AVE_HALT_CMD_TIMEOUT_MS, ctx.timeout);
	if (!abi) {
		dev_warn(dev, "halt: no command ABI selected\n");
		return -ENODEV;
	}
	cmd_len = ave_cmd_size(abi, AVE_OP_HALT);
	if (!cmd_len) {
		dev_warn(dev, "halt: ABI %s has no Halt command\n", abi->name);
		return -ENODEV;
	}

	/*
	 * Clear the completion word and prove the clear landed. Without this
	 * the poll below is meaningless (see the comment above).
	 */
	ave_write(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0), 0);
	v = ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(0));
	if (v) {
		dev_err(dev, "halt: scratch 0 still %#010x after clearing it; not sending - the poll could not tell success from failure\n",
			v);
		return -EIO;
	}

	cmd = ave_ipc_alloc(ave, cmd_len, &cmd_iova);
	if (!cmd)
		return -ENOMEM;

	ret = ave_cmd_build_simple(abi, AVE_OP_HALT, cmd, cmd_len, &ctx);
	if (ret < 0) {
		dev_err(dev, "halt: builder failed: %d\n", ret);
		goto out_free;
	}

	dev_info(dev, "halt: sending command %u, %zu bytes at IOVA %pad on IO (no reply is expected)\n",
		 abi->cmd[AVE_OP_HALT].id, cmd_len, &cmd_iova);

	ret = ave_ipc_send(ave, AVE_CH_IO, cmd_iova, cmd_len, 0);
	if (ret) {
		dev_err(dev, "halt: send failed: %d\n", ret);
		goto out_free;
	}

	ret = readl_relaxed_poll_timeout(
		ave->bank[AVE_BANK_SVE].base + AVE_SVE_SCRATCH(0),
		v, v == AVE_SCRATCH0_STOPPED, 100, AVE_HALT_SCRATCH_US);
	if (ret) {
		dev_err(dev, "halt: scratch 0 is %#010x, never became %#010x - the firmware did not halt\n",
			v, AVE_SCRATCH0_STOPPED);
		/*
		 * Do not return it to the pool: the core may still be running
		 * and still reading it. This is presentational only - ave_ipc_
		 * fini() frees the whole FwIPC region a moment later either
		 * way, so the unload is exactly as hard as it is today.
		 * (Review 2026-09-13, finding 7.)
		 */
		return ret;
	}
	dev_info(dev, "halt: scratch 0 = %#010x, the firmware reached its wfi\n", v);

	/*
	 * AVE_IOP::Stop's own check: three consecutive samples with
	 * CPU_STATUS & (RUNNING|STOPPED) set. Reported either way - what this
	 * register does after a Halt is inferred, not confirmed (docs/55).
	 */
	for (i = 0, idle = 0; i < 200 && idle < AVE_HALT_IDLE_SAMPLES; i++) {
		v = ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_STATUS);
		idle = (v & (AVE_ASC_ST_RUNNING | AVE_ASC_ST_STOPPED)) ? idle + 1 : 0;
		udelay(50);
	}
	dev_info(dev, "halt: CPU_STATUS %#010x%s after %u sample(s)%s\n",
		 v, v & AVE_ASC_ST_STOPPED ? " STOPPED" : "", i,
		 idle >= AVE_HALT_IDLE_SAMPLES ? "" : " - never settled");

out_free:
	/* Safe only now: a halted core cannot read the command any more. */
	ave_ipc_free(ave, cmd, cmd_len);
	return ret;
}
