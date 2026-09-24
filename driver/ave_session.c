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
#include <linux/crc32.h>
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
#include "ave_dapf.h"
#include "ave_session.h"

/* ------------------------------------------------------------------------ */
/* Module parameters - the gate and the values we cannot read from the fw   */
/* ------------------------------------------------------------------------ */

static bool session_selftest;
module_param(session_selftest, bool, 0444);
MODULE_PARM_DESC(session_selftest,
	"send the opening Config/Open/Start_AVC command sequence after boot and log each reply (default off)");

/*
 * Stop the self-test after Config. Halt needs a controller object that only
 * Config creates (fw 0x10d44 / 0xe84c), but halting with a client open is
 * untested (docs/55 6) - so this is the configuration to prove Halt in.
 */
static bool session_config_only;
module_param(session_config_only, bool, 0444);
MODULE_PARM_DESC(session_config_only,
	"with session_selftest=1: send Config only, open no client (the state to prove fw_halt in)");

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
 * The entropy-coding working buffers, EncCommParams.encoder_addr_entropy[i][j].
 * The kext fills a matrix - columns 0..3 outside, rows inside (kext
 * 0xfffffe0008eb0cb8) - and F12 showed the pipe's four entropy write channels
 * (0x1303C0 + 0x40k) enabled with a null address while only column 0 was
 * filled, so every column gets its own buffer now.
 * - the last unconditional assert on the per-frame path (docs/54).
 * SetTranscode requires the first four non-zero and 64-byte aligned, asserting
 * CAVCController_H13C.cpp:8020 / :8021 at fw 0x59558 / 0x595a0.
 *
 * Size per buffer is the kext's AVE_CalcBufSizeOfEntropyCoding AVC arm (kext
 * 0xfffffe0008ea5bd0): ALIGN_DOWN(64*W + 960, 1024) * K, where K is either 8
 * or ceil(ceil(H/16)/4). The flag that picks K was not pinned, so we take the
 * larger - 960 KiB each at 1280x720.
 *
 * session_entropy=0 leaves the table zero and should reproduce the :8020
 * assert: the negative control for this change, same as session_lowres=0.
 */
/*
 * docs/57 cause #1: set Start_AVC NEED_LSB_PLANES and give each DPB slot an
 * LSB (tile-metadata) plane. Without it the firmware never programs the pipe's
 * recon writer ("Uncompress Ref is not supported") and the Pipe never finishes
 * (F5). Layout per slot: the MSB pair at +0 and the LSB pair after it, both
 * exactly sized by ave_recon_planes(); the firmware derives both chroma planes
 * from the two luma ones (fw 0x2d14c, 0x2c314).
 */
static bool session_lsb;
module_param(session_lsb, bool, 0444);
MODULE_PARM_DESC(session_lsb,
	"set NEED_LSB_PLANES (Start_AVC 0xFD7D) and publish per-slot LSB planes, so the firmware programs the recon writer (docs/57 #1)");

/*
 * docs/57 cause #2: macOS calls AVE_DPM_TuneUpPipe before every command, which
 * ends in SetClockGating(false) = SVE +0x38 <- 0. This driver only ever writes 1
 * there (stage 7). session_sve_ungate=1 writes 0 just before Process and 1
 * again once it returns.
 */
static bool session_sve_ungate;
module_param(session_sve_ungate, bool, 0444);
MODULE_PARM_DESC(session_sve_ungate,
	"write SVE +0x38 = 0 (clock gating off, as AVE_DPM_TuneUpPipe does) around Process (docs/57 #2)");

/*
 * docs/57 #5: Config bSkipMcpu = 1. The firmware then writes 1 to the seven MCPU
 * +0x8 registers and skips ConfigureMCPUs / McpuController::Start. macOS sets
 * it only for the ave-platform=3 boot-arg, so this DEPARTS from macOS - a
 * discriminator for the pipe hang, not a fix. Create stays 1 (Halt needs the
 * controller object, docs/55 9).
 */
static bool session_skip_mcpu;
module_param(session_skip_mcpu, bool, 0444);
MODULE_PARM_DESC(session_skip_mcpu,
	"Config bSkipMcpu=1: skip MCPU configure/start (departs from macOS; docs/57 #5 discriminator)");

static bool session_nbr_fill;
module_param(session_nbr_fill, bool, 0444);
MODULE_PARM_DESC(session_nbr_fill,
	"fill the SrcNeighbor arena with 0xA5 and report at a Process timeout how much of Info[0]/Pixel[0] the neighbour writers overwrote (docs/59)");

/*
 * docs/60 cause #1: publish a colocated MV buffer per DPB slot in Start_AVC
 * (wire 0xF6B0). Left zero, setPipe disables the pipe's colocated writer
 * (0x40D130380 = 0), the one write channel off for us and on under macOS.
 * 128 bytes per MB (the Colocated surface), 64-byte aligned, filled with 0x5A
 * so writes show at the timeout.
 */
static bool session_coloc;
module_param(session_coloc, bool, 0444);
MODULE_PARM_DESC(session_coloc,
	"publish per-slot colocated MV buffers in Start_AVC (wire 0xF6B0) so the pipe's colocated writer is enabled (docs/60 #1)");

/*
 * Write the entropy size table beside the address table at Start_AVC
 * (wire 0xFA30, confirmed both sides in docs/62 0). Kept switchable as the
 * control: the kext refuses to send a command with a zero size here, and a
 * zero-length ring cannot drain - which is what fills the SEB.
 */
static bool session_entropy_size;	/* opt-in: see F15 */
module_param(session_entropy_size, bool, 0444);
MODULE_PARM_DESC(session_entropy_size,
	"write the entropy buffer sizes at Start_AVC wire 0xFA30, which switches the SEB drain channels on (docs/62 0; off by default since F15 died at insmod on the first build that wrote it)");

static bool session_diag = true;
module_param(session_diag, bool, 0444);
MODULE_PARM_DESC(session_diag,
	"on a Process timeout, log the VENC power states, the pipe done/go/AXI registers and scratch 7 (read-only, docs/57 #3/#4)");

static bool session_ignore_dart;
module_param(session_ignore_dart, bool, 0444);
MODULE_PARM_DESC(session_ignore_dart,
	"send Process even when the datapath DART's TTBR does not match the CPUDART's (F4 ended in a machine reset)");

static bool session_entropy = true;
module_param(session_entropy, bool, 0444);
MODULE_PARM_DESC(session_entropy,
	"publish the entropy-coding buffers in Process (default on; 0 should reproduce ASSERT CAVCController_H13C.cpp:8020)");

static unsigned int session_entropy_kb;	/* 0 = the kext formula above */
module_param(session_entropy_kb, uint, 0444);
MODULE_PARM_DESC(session_entropy_kb,
	"size of each entropy-coding buffer in KiB (0 = AVE_CalcBufSizeOfEntropyCoding formula, larger K)");

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

/*
 * The source-path experiment knobs (docs/62 §6). Nothing here is known-good:
 * every run up to F17 sent zero for both, and the kext passes both through
 * from user space without ever writing or checking them, so there is no value
 * to copy from Apple. They exist to be swept.
 *
 *   session_src_mode  wire 0xFEC0, u16. setPipe splits it:
 *                     0x40D120050 = v & 3, 0x40D1200D0 = v >> 2.
 *   session_src_cfg   wire 0xFCE8, u8, into bits 16+ of 0x40D12000C.
 *
 * Both registers appear in the 0x20000 channel windows the diagnostics
 * already dump, so a sweep is observable: set a value, read the register.
 */
static unsigned int session_src_mode;
module_param(session_src_mode, uint, 0444);
MODULE_PARM_DESC(session_src_mode,
	"Start_AVC wire 0xFEC0 (u16): source-read mode, split into 0x40D120050 and 0x40D1200D0 (0 = what every run so far sent)");

static unsigned int session_src_cfg;
module_param(session_src_cfg, uint, 0444);
MODULE_PARM_DESC(session_src_cfg,
	"Start_AVC wire 0xFCE8 (u8): high byte of the source format word 0x40D12000C (0 = what every run so far sent)");

/*
 * The other two bytes that reach SRCDMAGO (docs/69). session_src_bit3 also
 * gates whether ProcessPipeReset initialises the THIRD reader channel at
 * 0x40D120100 - and the stage that never runs is IntraEst, which needs
 * source pixels of its own (F21, F22).
 */
static unsigned int session_src_bit3;
module_param(session_src_bit3, uint, 0444);
MODULE_PARM_DESC(session_src_bit3,
	"Start_AVC wire 0xFCE9 (u8): SRCDMAGO bit 3, and the gate on the third reader channel 0x40D120100");

static unsigned int session_src_go;
module_param(session_src_go, uint, 0444);
MODULE_PARM_DESC(session_src_go,
	"Start_AVC wire 0xFECC (u8): SRCDMAGO bits 4 and up");

/*
 * Make the firmware talk (docs/70). Wire 0xFCD8's bit 5 is the single byte
 * CController::Print tests before dropping every "AVC COMMON::" line, so
 * with it clear - every run we have ever done - the firmware has been
 * discarding its own diagnostics before they reach the ring we already
 * drain. 0x20 is bit 5 alone: the per-frame lines, without DebugInit's
 * hundreds. The log path allocates shared memory and sends synchronously,
 * so wider values slow the frame and can overrun the 512-slot ring.
 *
 * Removed in f2b3341 with the f25-f30 deaths, which were the misaddressed
 * session_costs group 4 read (docs/53, f38); restored 2026-09-24. It has
 * never had a run that survived.
 */
static unsigned int session_dbg;
module_param(session_dbg, uint, 0444);
MODULE_PARM_DESC(session_dbg,
	"Start_AVC wire 0xFCD8: firmware debug verbosity. 0x20 = bit 5, which is what lets its own QPY/nQuant lines out at all");

/*
 * docs/73 P2: ask the firmware to code every I-slice macroblock as I_PCM.
 * The coded MB type then cannot be confused with the blank frame's
 * I_16x16, and I_PCM carries raw source samples: the ramp means the pipe
 * sees our source, flat grey means it does not. ~1.39 MB per 720p frame,
 * so pair it with session_coded_kb=2048.
 */
static unsigned int session_ipcm;
module_param(session_ipcm, uint, 0444);
MODULE_PARM_DESC(session_ipcm,
	"Start_AVC wire 0xFCE4 (u8): code I-slice MBs as I_PCM (docs/73 P2; 0 = every run before f43). Use with session_coded_kb=2048");

/*
 * docs/72 §5.2: macOS always sends five 0x400 lambda scales and three
 * per-QP lambda tables in the RC block; we have sent zeros, so ModeDec's
 * lambda registers 0x40D26A09C/0A0 read 0 (f42).
 */
static bool session_lambda = true;
module_param(session_lambda, bool, 0444);
MODULE_PARM_DESC(session_lambda,
	"Start_AVC: send macOS's lambda block (RC+0x68..0x78 = 0x400, per-QP tables at wire 0xFFC0..0x10573); docs/72. Default on since f54 (f53: P frame 47 KB -> 2.5 KB); 0 = zeros");

/*
 * The SPS scaling lists. The firmware turns each list weight w into a
 * quantiser scale register as (0x10000 / w) << 16 | w (docs/74), so zero
 * lists - every run before f47 - zero every coefficient at any QP: the blank
 * frame. macOS sends flat 16, and so do we by default since f49.
 */
static unsigned int session_scaling = 16;
module_param(session_scaling, uint, 0444);
MODULE_PARM_DESC(session_scaling,
	"flat weight for every SPS 4x4/8x8 scaling list (default 16, what macOS sends, docs/74); 0 = zeros, the pre-f47 blank frame");

/*
 * docs/74 R3: wire 0xFCF0 (u16) -> RECONL/RECONC SKIPMODE (bits 0/1,
 * fw 0x5e12c-0x5e138 -> 0x40D28A08C / 0x40D2AA08C). macOS sends 3. Meaning
 * [U]; expected to matter for P frames.
 */
static unsigned int session_skipmode = 3;
module_param(session_skipmode, uint, 0444);
MODULE_PARM_DESC(session_skipmode,
	"Start_AVC wire 0xFCF0 (u16) skip_mode -> RECONL/RECONC SKIPMODE; default 3, what macOS sends (docs/74 R3), since f54. 0 = every run before f51");

/*
 * Rate control. The default reproduces every run so far: ui32RCFlag = 2
 * (AVE_RC_FIXQP), session_qp on every frame type. session_bitrate switches
 * to the firmware's own controller (ui32RCFlag = 1) with that target in
 * BITS PER SECOND - ProcessInit divides it by the frame rate and the pixel
 * count to get bits per pixel (fw 0x401b8). docs/66 §1.
 *
 * A caveat worth knowing before trusting a number: two controller fields
 * (sCRCInitParams+177 and +180) gate the rate controller's construction and
 * neither has been traced to a host field, so mode 1 may quietly do nothing.
 * The discriminator is the slice QP - under fixed QP it provably cannot
 * vary, so a QP that moves is the controller working.
 */
static unsigned int session_bitrate;
module_param(session_bitrate, uint, 0444);
MODULE_PARM_DESC(session_bitrate,
	"target bitrate in bits/s; 0 = fixed QP (the default, and what every run so far used)");

static unsigned int session_fps = 30;
module_param(session_fps, uint, 0444);
MODULE_PARM_DESC(session_fps, "frame rate numerator (default 30)");

static unsigned int session_fps_div = 1;
module_param(session_fps_div, uint, 0444);
MODULE_PARM_DESC(session_fps_div, "frame rate denominator (default 1)");

static unsigned int session_qp_min = 10;
module_param(session_qp_min, uint, 0444);
MODULE_PARM_DESC(session_qp_min, "rate control QP floor (default 10)");

static unsigned int session_qp_max = 51;
module_param(session_qp_max, uint, 0444);
MODULE_PARM_DESC(session_qp_max, "rate control QP ceiling (default 51)");

static unsigned int session_idr_period = 1;
module_param(session_idr_period, uint, 0444);
MODULE_PARM_DESC(session_idr_period,
	"ui32IdrPeriod: frames between IDRs (default 1 = every frame)");

/* Override the coded-buffer size (KiB). 0 = Apple's formula. */
static unsigned int session_coded_kb;
module_param(session_coded_kb, uint, 0444);
MODULE_PARM_DESC(session_coded_kb,
	"coded (bitstream) buffer size in KiB; 0 = AVE_CalcBufSizeOfCodedData");

/*
 * How many frames one load encodes: IDR first, then P frames. Default 1, so
 * an experiment that is not about multi-frame behaviour sends exactly what
 * every run so far sent. docs/64.
 */
static unsigned int session_frames = 1;

/*
 * docs/68 §6 item 9: sessions per module load. After the first session's
 * frames, N-1 more rounds of Stop + Close, then Open + Start_AVC + Process
 * on freshly allocated buffers, with the firmware left running and Config
 * sent once. Every open()/close() of a V4L2 node does exactly this. The
 * previous session's buffers stay mapped until remove (the firmware has
 * released them after Close, but freeing them is not what is under test).
 */
static unsigned int session_repeat = 1;
module_param(session_repeat, uint, 0444);
MODULE_PARM_DESC(session_repeat,
	"sessions per load: after the first, Stop+Close then Open+Start_AVC+frames again (default 1); docs/68 §6.9");
module_param(session_frames, uint, 0444);
MODULE_PARM_DESC(session_frames,
	"frames to encode per session: 1 = a single IDR (default), N = IDR followed by N-1 P frames (up to 1000; coded slots rotate over min(N, 4))");

/*
 * Encode a constant luma plane instead of the ramp. The cheapest possible
 * discriminator for F17's result: if the decoded picture comes back at this
 * value the source DMA does read our buffer and the ramp failure is an
 * addressing or layout problem; if it comes back at ~130 again the hardware
 * never delivered our bytes at all. One load, no ABI guesses.
 */
static unsigned int session_flat_luma;
module_param(session_flat_luma, uint, 0444);
MODULE_PARM_DESC(session_flat_luma,
	"fill the source luma with this constant (1..255) instead of the ramp; 0 = ramp");

/*
 * Which groups of ave_session_diag_costs() to read. OFF by default, and it
 * stays off per group until that group is shown survivable: this dump is
 * what killed the machine in f25, f26 and f29. A register read in a gated
 * block hangs the fabric (docs/24), and not every offset here is in a block
 * known to be powered at that point.
 *
 *   bit 0  IntraEst cfg 0x40D24A1C8..1D8 and 0x40D24A394
 *   bit 1  ModeDec cost ladder 0x40D26A0AC..0x104
 *   bit 2  curMB 0x40D243180 and the 0x40D263180 control
 *   bit 3  IntraEst DMem 0x40D448000 and ME 0x40D190630 (until f39 this
 *          read 0x40D348000 - DPE offset 0x248000, off by 0x100000 from
 *          docs/58's DMem 0x1448000 - and that read is an SError, f38)
 *
 * Each group logs an ave_step() marker first, so with ave_step_ms set the
 * last marker on disk names the group that hung.
 */
/* The post-frame CPU scan of the colocated buffer; f33/f34 hung in it. */
static bool session_diag_coloc = true;
module_param(session_diag_coloc, bool, 0444);
MODULE_PARM_DESC(session_diag_coloc,
	"after the frame, scan colocated slot 0 from the CPU to see what the writer changed (default on; f33/f34 hung here with stream 15 attached)");

static unsigned int session_costs;
module_param(session_costs, uint, 0444);
MODULE_PARM_DESC(session_costs,
	"bitmask of ave_session_diag_costs() register groups to read; 0 = none (default, safe). These reads have hung this machine");

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

/*
 * One coded (bitstream) output buffer. The firmware requires it to exceed
 * 3*W*H/4 at encode time (fw 0x58358); Apple sizes it with
 * AVE_CalcBufSizeOfCodedData (kext 0xea4d58), which is what
 * ave_session_coded_size() reproduces. The fixed 2 MiB this driver used is
 * enough at 720p and too small from 1080p up. docs/66 §5.
 */
#define AVE_SESS_CODED_FLOOR		460800u
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
/* Config 1, Open 2, Start 3, Process 4+n; the teardown continues from here. */
#define AVE_SESS_CNT_TEARDOWN		32

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

/*
 * DMA regions one session may hold. A four-frame run needs four coded, four
 * coded-header and eight source planes on top of the fixed set, so this is
 * sized for AVE_SESS_FRAMES_MAX rather than for the single-frame case; the
 * allocator returns NULL past it, which fails the session loudly instead of
 * silently encoding with a buffer that was never allocated.
 */
#define AVE_SESS_MAX_DMA	48
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

/*
 * Frames one load will encode, and therefore coded buffers, coded-header
 * buffers and source pictures allocated. macOS runs up to 20 deep, bounded by
 * its coded-buffer pool (docs/64 §5); we submit synchronously, so this only
 * has to cover the longest sequence an experiment asks for.
 */
#define AVE_SESS_FRAMES_MAX	4

/* A bump allocator over one DMA region, so related buffers share a mapping. */
struct ave_sess_arena {
	void		*cpu;
	dma_addr_t	iova;
	size_t		size;
	size_t		used;
};

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
	 *
	 * One entry per frame in flight. The coded index and the command slot
	 * are a single resource: AVE_Client_AcquireOutputBuf takes the lowest
	 * index whose surface and slot are both free, and SendFwCmd_Process
	 * then uses slot = index + 21 (kext 0xf0166c, add w26,w22,#0x15).
	 * docs/64 §4.
	 */
	struct {
		void		*cpu;
		dma_addr_t	iova;
		u32		size;
		u32		len;	/* what the firmware coded into it */
		u32		span;	/* bytes it touched, trims included */
		u32		cabac_zero_words;
		u32		n_slice;
		struct ave_coded_slice slice[AVE_CODED_SLICE_MAX];
	}		coded[AVE_SESS_FRAMES_MAX], coded_hdr[AVE_SESS_FRAMES_MAX];
	u32		n_coded;
	u32		n_done;		/* frames actually encoded */
	size_t		psets_len;
	/* One source frame per coded buffer, so nothing is overwritten under
	 * a firmware that may still be reading it. */
	struct {
		u8		*luma;
		dma_addr_t	luma_iova;
		u32		luma_bytes;
		u8		*chroma;
		dma_addr_t	chroma_iova;
		u32		chroma_bytes;
	}		src[AVE_SESS_FRAMES_MAX];
	/*
	 * Streaming (f61): frame n uses coded index n % n_coded, and with it
	 * command slot 21 + index, source planes and a Process command that
	 * are allocated on the index's first use and reused after that.
	 */
	struct { void *cpu; dma_addr_t iova; } proc_cmd[AVE_SESS_FRAMES_MAX];
	u32		n_frames;	/* frames to encode this session */
	/* Every completed frame's Annex-B bytes, appended as it completes. */
	u8		*stream;
	size_t		stream_len, stream_cap;
	u32		stride;
	/* Per-frame PICMGMT recon scratch; one arena, reused every frame. */
	struct ave_sess_arena pic_recon;
	/* LowResResult: session-wide, published at Start, read from the first
	 * P frame onwards (docs/65 §Q4). */
	dma_addr_t	low_res_result[AVE_LOW_RES_RESULT_MAX];
	u32		n_low_res_result;
	size_t		low_res_result_size;
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
	u32		recon_msb_span;	/* MSB pair; the LSB pair starts here */
	size_t		low_res_size;	/* per slot; 0 = none published */
	u64		coloc[AVE_SESS_DPB_MAX];	/* colocated MV per slot */
	void		*coloc_cpu[AVE_SESS_DPB_MAX];
	size_t		coloc_size;	/* per slot; 0 = none */
	u32		entropy_size;	/* bytes per entropy buffer */
	u32		low_res_stride;	/* for the log line only */
	u64		nbr[AVE_SRC_NBR_GROUPS][AVE_SRC_NBR_MAX];
	void		*nbr_cpu[AVE_SRC_NBR_GROUPS][AVE_SRC_NBR_MAX];
	size_t		nbr_slot;	/* bytes per slot */
	u32		n_nbr;
	u64		entropy[AVE_ENTROPY_MAX][AVE_ENTROPY_COLS];
	u32		n_entropy;	/* rows */
	u32		n_entropy_cols;

	/* debugfs: only created once a frame actually came back. */
	struct dentry		*dbg_dir;
	struct debugfs_blob_wrapper coded_blob[AVE_SESS_FRAMES_MAX];
	struct debugfs_blob_wrapper hdr_blob[AVE_SESS_FRAMES_MAX];
	struct debugfs_blob_wrapper psets_blob, h264_blob;
	/* source and reconstruction, to tell "encoded our pixels" from "encoded something" */
	struct debugfs_blob_wrapper src_blob, recon_blob;
	void		*src_cpu;
	void		*recon_cpu;
	size_t		src_size, recon_pub_size;
	void			*h264;		/* assembled Annex-B frame */
	size_t			h264_len;
};

/* DART-addressable data buffer in the device DMA domain (the coprocessor's
 * DART - same domain the firmware and FwIPC live in). */
static void *ave_sess_dma_alloc(struct ave_sess_bufs *b, size_t size,
				dma_addr_t *iova)
{
	void *cpu;

	if (b->ndma >= AVE_SESS_MAX_DMA) {
		dev_err(b->ave->dev,
			"session: out of DMA slots (%u); raise AVE_SESS_MAX_DMA\n",
			AVE_SESS_MAX_DMA);
		return NULL;
	}
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

	if (b->nipc >= AVE_SESS_MAX_IPC) {
		/* f59 lost a second session to this returning NULL silently. */
		dev_err(b->ave->dev,
			"session: out of IPC command slots (%u); raise AVE_SESS_MAX_IPC\n",
			AVE_SESS_MAX_IPC);
		return NULL;
	}
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
	vfree(b->stream);
	b->stream = NULL;
	b->stream_len = b->stream_cap = 0;
	for (i = 0; i < b->ndma; i++)
		dma_free_coherent(b->ave->dev, b->dma[i].size,
				  b->dma[i].cpu, b->dma[i].iova);
	for (i = 0; i < b->nipc; i++)
		ave_ipc_free(b->ave, b->ipc[i].cpu, b->ipc[i].size);
	b->ndma = 0;
	b->nipc = 0;
}

/*
 * Free everything allocated after the first @ndma DMA buffers and @nipc IPC
 * commands, newest first. Only for use after the firmware has let go of
 * them: after Close has completed (STOP_DONE, docs/63). The Config-time
 * allocations before the marks stay - the firmware keeps those for the life
 * of the core.
 */
static void ave_sess_free_to(struct ave_sess_bufs *b, unsigned int ndma,
			     unsigned int nipc)
{
	while (b->ndma > ndma) {
		b->ndma--;
		dma_free_coherent(b->ave->dev, b->dma[b->ndma].size,
				  b->dma[b->ndma].cpu, b->dma[b->ndma].iova);
	}
	while (b->nipc > nipc) {
		b->nipc--;
		ave_ipc_free(b->ave, b->ipc[b->nipc].cpu, b->ipc[b->nipc].size);
	}
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
/*
 * The status words the firmware actually returns, named. Reporting a bare
 * -EIO for these threw away the diagnosis: 0xEE0004 in particular says the
 * bitstream did not fit, which is a sizing bug and nothing like a rejection.
 * docs/67 §5, docs/46 §2.1.
 */
static const char *ave_session_status_name(u32 status)
{
	switch (status) {
	case AVE135_STATUS_OK:			return "OK";
	case AVE135_STATUS_START_FAIL:		return "START failed";
	case AVE135_STATUS_FAIL:		return "INIT/ENCODE failed";
	case AVE135_STATUS_PSETS_SMALL:		return "the parameter-sets buffer is too small";
	case AVE135_STATUS_CODED_OVERFLOW:	return "the bitstream OVERFLOWED the coded buffer";
	case AVE135_STATUS_BAD_HEADER_CFG:	return "bFWCreatesHeader/SPS-PPS mismatch";
	case AVE135_STATUS_TRANSCODE_ERR:	return "hardware transcode error";
	default:				return "unknown";
	}
}

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
			"session: %s: firmware REJECTED the command, status %#x (%s; success would be %#x)\n",
			name, status, ave_session_status_name(status),
			abi->reply.status_ok);
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

	p.skip_mcpu = session_skip_mcpu;	/* docs/57 #5 discriminator */
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

	ret = ave_session_cmd(ave, abi, AVE_OP_CONFIG, "Config",
			      cmd_iova, cmd_len, 0);
	if (!ret && p.create_mcpu)
		ave->mcpu_created = true;
	return ret;
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
/*
 * AVE_CalcBufSizeOfCodedData, AVC 8-bit 4:2:0 default path (kext 0xea4d58):
 * one uncompressed frame (3*W*H/2), except for frames small enough that
 * doubling still fits under 460800, which are rounded up towards that floor.
 * So 720p gets 1382400 -> 1384448 after page alignment, not twice that; the
 * values match docs/66 §5's table at 640x480, 1280x720, 1920x1088 and
 * 3840x2160. Comfortably above the firmware's own CodedBufSize > 3*W*H/4
 * check (fw 0x58358), and above CAVLC's 3200-bit/MB ceiling.
 */
static size_t ave_session_coded_size(u32 cw, u32 ch)
{
	size_t base = (size_t)cw * ch * 3 / 2;
	size_t sz = base >= AVE_SESS_CODED_FLOOR
		  ? base : min(2 * base, (size_t)AVE_SESS_CODED_FLOOR);

	if (session_coded_kb)
		sz = (size_t)session_coded_kb << 10;
	return ALIGN(sz, SZ_4K);
}

/*
 * The four reconstruction sub-planes, from AVE_CalcBufSizeOfRecon (AVC,
 * DevType 12, 8-bit 4:2:0, compressed arm - kext 0xfffffe0008ea528c..0xea535c).
 * The firmware recomputes the luma pair identically in H264VideoEncoderDPB
 * (fw 0x2d14c-0x2d1a0) and derives the chroma bases in setRefPointers as
 * UV_MSB = Y_MSB + luma and UV_LSB = Y_LSB + luma_meta (fw 0x2c314-0x2c33c),
 * so each pair must be contiguous and each pair's size must be exact.
 *
 * This replaces the cw*ch*2 over-estimate, which happens to be large enough
 * at 1280x720 and is NOT at other sizes: at 640x480 the MSB region overruns
 * the slot, and from 1080p up the fixed 128 KiB LSB window is too small.
 * docs/66 §5.
 */
struct ave_recon_planes {
	u32	luma, luma_meta, chroma, chroma_meta;
	u32	msb_span, lsb_span;	/* what a slot must hold, 128-aligned */
};

static u32 ave_npo2(u32 n)
{
	return n <= 1 ? 1 : 1u << (32 - __builtin_clz(n - 1));
}

static void ave_recon_planes(u32 w, u32 h, struct ave_recon_planes *p)
{
	u32 cols   = DIV_ROUND_UP(w, 32);
	u32 rows   = (h + 35) >> 5;		/* ceil((h + 4) / 32) */
	u32 cols_c = DIV_ROUND_UP(w / 2, 16);
	u32 rows_c = ((h / 2) + 19) >> 4;

	p->luma        = 1024u * cols * rows;
	p->chroma      = ALIGN(512u * cols_c * rows_c, 128);
	p->luma_meta   = ALIGN(32u * ave_npo2(cols)   * ave_npo2(rows),   128);
	p->chroma_meta = ALIGN(8u  * ave_npo2(cols_c) * ave_npo2(rows_c), 128);
	p->msb_span    = ALIGN(p->luma + p->chroma, 128);
	p->lsb_span    = ALIGN(p->luma_meta + p->chroma_meta, 128);
}

static size_t ave_session_lowres_size(u32 cw, u32 ch, u32 *stride)
{
	u32 lr_stride = ALIGN(4 * cw, 256);

	if (stride)
		*stride = lr_stride;
	return ALIGN((size_t)lr_stride * ((ch + 63) >> 4), 512);
}

/*
 * One LowResResult surface: the low-resolution search's OUTPUT, session-wide
 * rather than per DPB slot. ALIGN(4*W, 128) * ceil(H/64) + 1024, from the
 * kext's own allocator (docs/65 §Q4). 62464 bytes at 1280x720.
 */
static size_t ave_session_lowres_result_size(u32 cw, u32 ch)
{
	return (size_t)ALIGN(4 * cw, 128) * DIV_ROUND_UP(ch, 64) + 1024;
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
	struct ave_recon_planes pl;
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
	ave_recon_planes(cw, ch, &pl);
	recon_slot = ALIGN((size_t)pl.msb_span + (session_lsb ? pl.lsb_span : 0),
			   SZ_4K);
	dev_info(ave->dev,
		 "session: recon planes for %ux%u: luma %u + chroma %u = MSB %u; luma_meta %u + chroma_meta %u = LSB %u; slot %#zx\n",
		 cw, ch, pl.luma, pl.chroma, pl.msb_span,
		 pl.luma_meta, pl.chroma_meta, pl.lsb_span, recon_slot);
	recon.size = recon_slot * n;
	recon.cpu = ave_sess_dma_alloc(bufs, recon.size, &recon.iova);
	if (!recon.cpu) {
		dev_err(ave->dev,
			"session: DPB recon arena (%zu bytes) allocation failed\n",
			recon.size);
		return;
	}
	memset(recon.cpu, 0, recon.size);
	/*
	 * Slot 0's MSB plane is what the recon writer targets with
	 * session_lsb (the MSB pair is at the slot base), and it is the
	 * picture the encoder actually reconstructed - the direct comparison
	 * against the source.
	 */
	bufs->recon_cpu = recon.cpu;
	bufs->recon_pub_size = min_t(size_t, pl.msb_span, SZ_256K);

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
	bufs->recon_msb_span = pl.msb_span;
	bufs->low_res_size = low_slot;
	bufs->low_res_stride = lr_stride;

	/*
	 * LowResResult, out of one arena. An I-frame never reads these - the
	 * reference loop is bounded by num_ref_idx_l0_active_minus1, which is
	 * -1 with no references - so leaving them zero is what every run so
	 * far did and is still the control (session_lowres=0). A P frame
	 * asserts on them at CAVCController_H13C.cpp:6184.
	 */
	if (session_lowres) {
		struct ave_sess_arena res = {};
		size_t slot = ALIGN(ave_session_lowres_result_size(cw, ch),
				    AVE_STRIDE_ALIGN);

		res.size = slot * AVE_LOW_RES_RESULT_MAX;
		res.cpu = ave_sess_dma_alloc(bufs, res.size, &res.iova);
		if (!res.cpu) {
			dev_warn(ave->dev,
				 "session: LowResResult arena (%zu bytes) failed; a P frame would hit ASSERT CAVCController_H13C.cpp:6184\n",
				 res.size);
		} else {
			memset(res.cpu, 0, res.size);
			for (i = 0; i < AVE_LOW_RES_RESULT_MAX; i++) {
				dma_addr_t a;

				if (!ave_sess_arena_take(&res, slot, &a))
					break;
				bufs->low_res_result[i] = a;
				bufs->n_low_res_result = i + 1;
			}
			bufs->low_res_result_size = slot;
			dev_info(ave->dev,
				 "session: LowResResult %u surface(s) of %#zx bytes from %pad (ALIGN(4*%u,128) * ceil(%u/64) + 1024)\n",
				 bufs->n_low_res_result, slot, &res.iova,
				 cw, ch);
		}
	}

	/*
	 * Every slot, not just slot 0: the firmware rotates the recon slot per
	 * frame (ManageDPBBuffer), so a slot whose surfaces were never carved
	 * would only show up as an assert on the frame that happened to land
	 * on it. docs/65 §change 3.
	 */
	for (i = 0; i < bufs->n_dpb; i++)
		dev_info(ave->dev,
			 "session: DPB slot %u: recon %pad LowResRef %pad\n",
			 i, &bufs->dpb[i].recon, &bufs->dpb[i].low_res);
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
	struct ave_buf coded[AVE_SESS_FRAMES_MAX], coded_hdr[AVE_SESS_FRAMES_MAX];
	dma_addr_t cmd_iova, fwc_iova, fwcm_iova;
	dma_addr_t psets_iova;
	u32 cw, ch, fwc_size, i, n;
	size_t coded_size;
	void *psets_cpu;
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
	/*
	 * One coded buffer and one coded header per frame. The index into
	 * these tables is what picks the command slot (21 + index), so a
	 * session that encodes N frames must publish N of them at Start.
	 */
	coded_size = ave_session_coded_size(cw, ch);
	n = clamp_t(u32, session_frames, 1, AVE_SESS_FRAMES_MAX);
	if (n > abi->start_avc.coded_max)
		n = abi->start_avc.coded_max;
	bufs->n_coded = n;
	bufs->n_frames = clamp_t(u32, session_frames, 1, 1000);
	for (i = 0; i < n; i++) {
		bufs->coded[i].cpu = ave_sess_dma_alloc(bufs, coded_size,
							&bufs->coded[i].iova);
		bufs->coded[i].size = coded_size;
		bufs->coded_hdr[i].cpu =
			ave_sess_dma_alloc(bufs, abi->start_avc.coded_hdr_bytes,
					   &bufs->coded_hdr[i].iova);
		bufs->coded_hdr[i].size = abi->start_avc.coded_hdr_bytes;
		if (!bufs->coded[i].cpu || !bufs->coded_hdr[i].cpu)
			return -ENOMEM;
		memset(bufs->coded[i].cpu, 0, bufs->coded[i].size);
		memset(bufs->coded_hdr[i].cpu, 0, bufs->coded_hdr[i].size);
	}
	psets_cpu = ave_sess_dma_alloc(bufs, AVE_SESS_PARAM_SETS_SIZE,
				       &psets_iova);
	if (!psets_cpu)
		return -ENOMEM;

	/*
	 * dma_alloc_coherent already hands back zeroed memory, but the frame
	 * step relies on that to find the end of the SPS+PPS the firmware
	 * writes here (it reports the length only in bits, in a controller
	 * field we cannot read), so make the assumption explicit.
	 */
	memset(psets_cpu, 0, AVE_SESS_PARAM_SETS_SIZE);

	bufs->psets_cpu = psets_cpu;
	bufs->psets_size = AVE_SESS_PARAM_SETS_SIZE;
	memset(recon, 0, sizeof(recon));
	for (i = 0; i < bufs->n_dpb; i++) {
		recon[i].addr = bufs->dpb[i].recon;
		/* used only where the ABI's recon_size != NONE (26.6.2) */
		recon[i].luma_size = cw * ch;
		if (session_lsb) {
			/*
			 * MSB pair at the slot base, LSB pair after it. Both
			 * spans are exact now (docs/66 §5), so the LSB base
			 * moves with the resolution instead of sitting at a
			 * fixed 128 KiB that is too small from 1080p up.
			 */
			recon[i].addr = bufs->dpb[i].recon;
			recon[i].lsb_addr = bufs->dpb[i].recon + bufs->recon_msb_span;
		}
	}
	s.need_lsb_planes = session_lsb;
	if (session_lsb)
		dev_info(ave->dev,
			 "session: Start_AVC: NEED_LSB_PLANES=1; slot 0 LSB %pad MSB %#llx (slot %#x bytes)\n",
			 &bufs->dpb[0].recon, recon[0].addr, bufs->recon_size);
	for (i = 0; i < n; i++) {
		coded[i].addr = bufs->coded[i].iova;
		coded[i].size = bufs->coded[i].size;
		coded_hdr[i].addr = bufs->coded_hdr[i].iova;
		coded_hdr[i].size = bufs->coded_hdr[i].size;
	}

	s.width = session_width;
	s.height = session_height;
	s.src_mode = (u16)session_src_mode;
	s.src_cfg_byte = (u8)session_src_cfg;
	s.src_go_bit3 = (u8)session_src_bit3;
	s.src_go_bits = (u8)session_src_go;
	s.dbg_bits = session_dbg;
	s.ipcm_islice = (u8)session_ipcm;
	s.lambda_block = session_lambda;
	s.scaling_flat = (u16)session_scaling;
	s.skip_mode = (u16)session_skipmode;
	if (session_scaling)
		dev_info(ave->dev,
			 "session: Start_AVC: flat scaling lists %u (docs/74); expect quantiser scale registers %#010x\n",
			 session_scaling, ((0x10000 / session_scaling) << 16 | session_scaling) & 0x3fff00ff);
	if (session_lambda)
		dev_info(ave->dev,
			 "session: Start_AVC: macOS lambda block (docs/72); expect 0x40D26A09C = 0x40D26A0A0 = nQuant\n");
	if (session_ipcm)
		dev_info(ave->dev,
			 "session: Start_AVC: I_PCM in I slices %#x (wire 0xFCE4); expect IntraEst 0x1D0/1D4/1D8 = 0x01000000 and DMem 0x40D448000 bit 9 (docs/73 P2)\n",
			 session_ipcm);
	if (session_dbg) {
		/*
		 * The firmware's own lines come through the same ring as
		 * everything else and would be thrown away by the rate limit
		 * long before the interesting ones arrive - 100 lines per 5 s
		 * against one per macroblock.
		 */
		ratelimit_state_init(&ave->fwlog_rs, 0, 0);
		ratelimit_set_flags(&ave->fwlog_rs, RATELIMIT_MSG_ON_RELEASE);
		dev_info(ave->dev,
			 "session: Start_AVC: firmware debug bits %#x (wire 0xFCD8); fw log rate limit lifted - expect 'AVC COMMON:: QPY %%d nQuant %%d'\n",
			 session_dbg);
	}
	if (session_src_bit3 || session_src_go)
		dev_info(ave->dev,
			 "session: Start_AVC: SRCDMAGO inputs bit3 %#x bits4+ %#x (wire 0xFCE9 / 0xFECC); watch 0x40D110128 and the third reader channel 0x40D120100\n",
			 session_src_bit3, session_src_go);
	if (session_src_mode || session_src_cfg)
		dev_info(ave->dev,
			 "session: Start_AVC: source-path sweep src_mode %#x (expect 0x40D120050=%#x 0x40D1200D0=%#x) src_cfg %#x (expect 0x40D12000C=%#x)\n",
			 s.src_mode, s.src_mode & 3, s.src_mode >> 2,
			 s.src_cfg_byte, (s.src_cfg_byte << 16) | (20 << 8));
	s.frame_rate = session_fps ? session_fps : 30;
	s.frame_rate_div = session_fps_div ? session_fps_div : 1;
	s.bitrate = session_bitrate;
	s.rc_enable = session_bitrate != 0;
	s.qp_i = s.qp_p = s.qp_b = session_qp;
	s.qp_min = min_t(u32, session_qp_min, 51);
	s.qp_max = clamp_t(u32, session_qp_max, s.qp_min, 51);
	s.key_interval = session_idr_period ? session_idr_period : 1;
	if (s.rc_enable)
		dev_info(ave->dev,
			 "session: Start_AVC: rate control ON (ui32RCFlag %u), target %u bit/s at %u/%u fps, QP %u..%u starting at %u - watch the slice QP, which under fixed QP cannot vary\n",
			 abi->start_avc.rc_mode_on, s.bitrate, s.frame_rate,
			 s.frame_rate_div, s.qp_min, s.qp_max, session_qp);
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
	/*
	 * The entropy/SEB buffers belong in Start_AVC: the firmware rebuilds the
	 * per-frame copy from this table every frame (docs/61 10), which is why
	 * F13's per-frame matrix changed nothing. Same buffers as the Process
	 * table; ave_session_alloc_entropy() has already allocated them.
	 */
	if (bufs->n_entropy && abi->start_avc.entropy_set != AVE_OFF_NONE) {
		u32 rows = min_t(u32, bufs->n_entropy, abi->start_avc.entropy_max);

		memcpy(s.entropy, bufs->entropy, sizeof(s.entropy));
		s.n_entropy = rows;
		if (session_entropy_size)
			s.entropy_size = bufs->entropy_size;
		s.n_entropy_cols = min_t(u32, bufs->n_entropy_cols,
					 abi->start_avc.entropy_cols_max);
		dev_info(ave->dev,
			 "session: Start_AVC: entropy %u x %u at wire %#x, slot 0 %#llx, size %#x at wire %#x%s\n",
			 s.n_entropy, s.n_entropy_cols,
			 abi->start_avc.entropy_set, s.entropy[0][0],
			 s.entropy_size, abi->start_avc.entropy_size_set,
			 s.entropy_size ? "" : " (size table OFF - control)");
	}

	/* Colocated MV buffers (docs/60 #1), allocated here, all or nothing. */
	if (session_coloc && abi->start_avc.colocated_set != AVE_OFF_NONE) {
		size_t sz = ALIGN((size_t)128 * (cw / 16) * (ch / 16), SZ_4K);

		for (i = 0; i < bufs->n_dpb; i++) {
			dma_addr_t iova;
			void *cpu = ave_sess_dma_alloc(bufs, sz, &iova);

			if (!cpu) {
				dev_warn(ave->dev, "session: colocated slot %u allocation failed; table dropped\n", i);
				bufs->coloc_size = 0;
				break;
			}
			memset(cpu, 0x5a, sz);
			bufs->coloc[i] = iova;
			bufs->coloc_cpu[i] = cpu;
			bufs->coloc_size = sz;
		}
		if (bufs->coloc_size) {
			for (i = 0; i < bufs->n_dpb; i++)
				s.colocated[i] = bufs->coloc[i];
			s.n_colocated = bufs->n_dpb;
			dev_info(ave->dev,
				 "session: Start_AVC: colocated %u slot(s) of %#zx bytes at wire %#x (0x5A fill)\n",
				 bufs->n_dpb, bufs->coloc_size,
				 abi->start_avc.colocated_set);
			/* Every slot: the firmware rotates which one a frame
			 * writes, so an unpublished slot only shows up as an
			 * assert on the frame that lands on it. */
			for (i = 0; i < bufs->n_dpb; i++)
				dev_info(ave->dev,
					 "session: colocated slot %u: %#llx\n",
					 i, bufs->coloc[i]);
		}
	}
	if (bufs->n_low_res_result && abi->start_avc.low_res_result_set != AVE_OFF_NONE) {
		for (i = 0; i < bufs->n_low_res_result; i++)
			s.low_res_result[i] = bufs->low_res_result[i];
		s.n_low_res_result = bufs->n_low_res_result;
		dev_info(ave->dev,
			 "session: Start_AVC: LowResResult %u surface(s) at wire %#x, [0] %#llx - inert for I, required from the first P\n",
			 s.n_low_res_result, abi->start_avc.low_res_result_set,
			 s.low_res_result[0]);
	}
	s.coded = coded;
	s.coded_hdr = coded_hdr;
	s.n_coded = n;

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
		 "session: Start_AVC: fw_client %pad/%#x mem %pad/%#x coded[0] %pad/%#x hdr[0] %pad/%#x psets %pad/%#x (%u coded buffer(s), slots %u..%u)\n",
		 &fwc_iova, fwc_size, &fwcm_iova, (u32)AVE_SESS_FWCLIENTMEM_SIZE,
		 &bufs->coded[0].iova, bufs->coded[0].size,
		 &bufs->coded_hdr[0].iova, bufs->coded_hdr[0].size,
		 &psets_iova, (u32)AVE_SESS_PARAM_SETS_SIZE,
		 n, AVE_SESS_PROCESS_SLOT, AVE_SESS_PROCESS_SLOT + n - 1);
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
	/*
	 * session_nbr_fill: a known non-zero pattern instead of zeros, so the
	 * timeout can tell whether the neighbour writers ever stored row 0
	 * (docs/59 read 3). The top row is never read as a neighbour, so the
	 * pattern cannot reach an encoded MB.
	 */
	memset(a.cpu, session_nbr_fill ? 0xa5 : 0, a.size);
	bufs->nbr_slot = slot;

	for (g = 0; g < AVE_SRC_NBR_GROUPS; g++)
		for (i = 0; i < AVE_SRC_NBR_MAX; i++) {
			dma_addr_t iova;
			void *cpu = ave_sess_arena_take(&a, slot, &iova);

			if (!cpu)
				return;		/* keeps what it managed */
			bufs->nbr[g][i] = iova;
			bufs->nbr_cpu[g][i] = cpu;
		}
	bufs->n_nbr = AVE_SRC_NBR_MAX;
}

/* AVE_CalcBufSizeOfEntropyCoding, AVC arm, taking the larger K (see above). */
static size_t ave_session_entropy_size(u32 cw, u32 ch)
{
	size_t row = ALIGN_DOWN((size_t)64 * cw + 960, 1024);
	u32 k = max_t(u32, 8, DIV_ROUND_UP(DIV_ROUND_UP(ch, 16), 4));

	return row * k;
}

static void ave_session_alloc_entropy(struct ave_device *ave,
				      const struct ave_cmd_abi *abi,
				      struct ave_sess_bufs *bufs)
{
	struct ave_sess_arena a = {};
	u32 cw = ave_mb_align(session_width);
	u32 ch = ave_mb_align(session_height);
	u32 n = abi->process_avc.entropy_max, cols;
	size_t each;
	u32 i;

	if (!session_frame)
		return;
	if (!session_entropy) {
		dev_warn(ave->dev,
			 "session: session_entropy=0: the entropy table is left zero, expect ASSERT CAVCController_H13C.cpp:8020\n");
		return;
	}
	if (abi->process_avc.entropy_set == AVE_OFF_NONE || !n) {
		dev_info(ave->dev,
			 "session: ABI %s has no located entropy table; not publishing one\n",
			 abi->name);
		return;
	}
	n = min_t(u32, n, AVE_ENTROPY_MAX);

	each = session_entropy_kb ? (size_t)session_entropy_kb << 10
				  : ave_session_entropy_size(cw, ch);
	if (!each || each > SZ_64M) {
		dev_warn(ave->dev,
			 "session: entropy buffer size %zu out of range; not publishing\n",
			 each);
		return;
	}

	cols = abi->process_avc.entropy_cols_max ? abi->process_avc.entropy_cols_max : 1;
	if (cols > AVE_ENTROPY_COLS)
		cols = AVE_ENTROPY_COLS;
	a.size = ALIGN(each, 128) * n * cols + 128;
	a.cpu = ave_sess_dma_alloc(bufs, a.size, &a.iova);
	if (!a.cpu) {
		dev_warn(ave->dev,
			 "session: entropy arena (%zu bytes) allocation failed\n",
			 a.size);
		return;
	}
	memset(a.cpu, 0, a.size);

	for (i = 0; i < n; i++) {
		u32 j;

		for (j = 0; j < cols; j++) {
			dma_addr_t iova;

			/* 128-aligned by the arena, so the :8021 check holds. */
			if (!ave_sess_arena_take(&a, each, &iova)) {
				/* A partial table would fail the builder. */
				bufs->n_entropy = 0;
				return;
			}
			bufs->entropy[i][j] = iova;
		}
	}
	bufs->n_entropy = n;
	bufs->n_entropy_cols = cols;
	bufs->entropy_size = each;
	dev_info(ave->dev,
		 "session: entropy: %u x %u buffers of %zu KiB at %#llx..%#llx%s\n",
		 n, cols, each >> 10, bufs->entropy[0][0],
		 bufs->entropy[n - 1][cols - 1],
		 session_entropy_kb ? " (size overridden by session_entropy_kb)"
				    : " (kext formula, larger K)");
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
				   u32 w, u32 h, u32 shift)
{
	u32 x, y;

	for (y = 0; y < h; y++) {
		u8 *row = luma + (size_t)y * stride;

		for (x = 0; x < w; x++)
			row[x] = session_flat_luma ? (u8)session_flat_luma :
				 (u8)(16 + ((((x + shift) % (w ? w : 1)) * 219) / (w ? w : 1)) +
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
				struct ave_sess_bufs *bufs)
{
	size_t psets_len = bufs->psets_len;
	u32 coded_len = bufs->coded[0].len;
	int coded_nal = ave_session_nal_type(bufs->coded[0].cpu, coded_len);
	bool need_psets = coded_nal != 7;	/* 7 = SPS */
	u32 n_slots = min(bufs->n_done, bufs->n_coded);
	u32 i;

	(void)psets_len;

	bufs->dbg_dir = debugfs_create_dir("apple_ave", NULL);
	if (IS_ERR(bufs->dbg_dir)) {
		dev_warn(ave->dev, "session: debugfs dir failed: %pe\n",
			 bufs->dbg_dir);
		bufs->dbg_dir = NULL;
		return;
	}

	/*
	 * The frame decoding is not the same question as the frame being ours:
	 * F16 produced a valid 1280x720 Baseline bitstream whose every pixel
	 * decoded to luma 130, i.e. the encoder did not read the ramp we wrote.
	 * Publish the source and the reconstruction so the two can be compared
	 * directly, and print a CRC of each - a flat recon says the pipe never
	 * saw our pixels, a ramp-shaped recon says the fault is downstream.
	 */
	if (bufs->src_cpu && bufs->src_size) {
		bufs->src_blob.data = bufs->src_cpu;
		bufs->src_blob.size = bufs->src_size;
		debugfs_create_blob("input_luma.bin", 0444, bufs->dbg_dir,
				    &bufs->src_blob);
	}
	if (bufs->recon_cpu && bufs->recon_pub_size) {
		bufs->recon_blob.data = bufs->recon_cpu;
		bufs->recon_blob.size = bufs->recon_pub_size;
		debugfs_create_blob("recon_luma.bin", 0444, bufs->dbg_dir,
				    &bufs->recon_blob);
	}
	if (bufs->src_cpu && bufs->recon_cpu) {
		const u8 *src = bufs->src_cpu, *rec = bufs->recon_cpu;
		u32 n = min_t(u32, bufs->src_size, bufs->recon_pub_size);
		u32 src_distinct = 0, rec_distinct = 0, rec_nonzero = 0;
		DECLARE_BITMAP(seen_s, 256) = {};
		DECLARE_BITMAP(seen_r, 256) = {};
		u32 k;

		dma_rmb();
		for (k = 0; k < n; k++) {
			/* A u8 tally wrapped at 256 and printed >256 distinct
			 * values of a byte in F17; bitmaps cannot. */
			if (!test_and_set_bit(src[k], seen_s))
				src_distinct++;
			if (!test_and_set_bit(rec[k], seen_r))
				rec_distinct++;
			rec_nonzero += !!rec[k];
		}
		dev_info(ave->dev,
			 "session: frame: source crc %#010x (%u distinct values, first %u %u %u %u), recon crc %#010x (%u distinct, first %u %u %u %u) over %u bytes\n",
			 crc32(0, src, n), src_distinct, src[0], src[1], src[2], src[3],
			 crc32(0, rec, n), rec_distinct, rec[0], rec[1], rec[2], rec[3], n);
		dev_info(ave->dev, "session: frame: recon non-zero bytes %u of %u\n",
			 rec_nonzero, n);
		if (rec_distinct <= 2 && src_distinct > 2)
			dev_warn(ave->dev,
				 "session: frame: the reconstruction is flat while the source is not - the encoder did not read our pixels\n");
	}

	/*
	 * Frame 0 keeps the unsuffixed names, so a single-frame run publishes
	 * exactly the file set every previous run did and tools/check_frame.py
	 * keeps working unchanged.
	 */
	for (i = 0; i < n_slots; i++) {
		char name[24];

		/*
		 * span, not len: the raw blob is for inspection, and with a
		 * non-zero trim the last slice ends after len bytes.
		 * frame.h264 is the assembled, trimmed stream.
		 */
		bufs->coded_blob[i].data = bufs->coded[i].cpu;
		bufs->coded_blob[i].size = bufs->coded[i].span;
		if (i)
			scnprintf(name, sizeof(name), "coded%u.bin", i);
		else
			strscpy(name, "coded.bin", sizeof(name));
		debugfs_create_blob(name, 0444, bufs->dbg_dir,
				    &bufs->coded_blob[i]);

		bufs->hdr_blob[i].data = bufs->coded_hdr[i].cpu;
		bufs->hdr_blob[i].size = bufs->coded_hdr[i].size;
		if (i)
			scnprintf(name, sizeof(name), "coded_hdr%u.bin", i);
		else
			strscpy(name, "coded_hdr.bin", sizeof(name));
		debugfs_create_blob(name, 0444, bufs->dbg_dir,
				    &bufs->hdr_blob[i]);
	}

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
	if (!bufs->stream_len)
		return;
	bufs->h264_blob.data = bufs->stream;
	bufs->h264_blob.size = bufs->stream_len;
	debugfs_create_blob("frame.h264", 0444, bufs->dbg_dir,
			    &bufs->h264_blob);

	dev_info(ave->dev,
		 "session: frame: /sys/kernel/debug/apple_ave/frame.h264 = %zu bytes, %u frame(s) (%s; coded starts with NAL type %d)\n",
		 bufs->stream_len, bufs->n_done,
		 need_psets ? "SPS+PPS prepended from paramsets.bin"
			    : "coded buffer already carries the parameter sets",
		 coded_nal);
}

/*
 * docs/59's reads, all read-only and inside bank 0. The question they settle:
 * is "currMbRow 1" a stall at row 1, or the pipeline head running ~15 MBs
 * ahead of a stall still inside row 0?
 */
static void ave_session_diag_row0(struct ave_device *ave,
				  struct ave_sess_bufs *bufs)
{
	/* stage host interfaces: MbInput, IntraEst, CAVLC, MotionEst, ModeDecision, ReconLuma, ReconChroma */
	static const struct { const char *name; u32 off; } hif[] = {
		{ "MbInput", 0x68000 }, { "IntraEst", 0x142000 },
		{ "CAVLC", 0x1c8000 }, { "MotionEst", 0x88000 },
		{ "ModeDec", 0x162000 }, { "ReconLuma", 0x182000 },
		{ "ReconChroma", 0x1a2000 },
	};
	u32 ev0, ev1;
	int i;

	/* 1. MbInput lookahead counters (DMem 0x40D408000 + x) */
	ev0 = ave_read(ave, AVE_BANK_DPE, 0x75800);
	ev1 = ave_read(ave, AVE_BANK_DPE, 0x75804);
	dev_info(ave->dev,
		 "session: diag MbInput produced %u consumed %u drain %#x lag %u; last src event %#010x %#010x (y %u x %u last %u); IntraEst curMB %#010x\n",
		 ave_read(ave, AVE_BANK_DPE, 0x3088a8),
		 ave_read(ave, AVE_BANK_DPE, 0x309584),
		 ave_read(ave, AVE_BANK_DPE, 0x309570),
		 ave_read(ave, AVE_BANK_DPE, 0x70110),
		 ev0, ev1, (ev0 >> 16) & 0xfff, ev0 & 0x1fff, !!(ev0 & BIT(26)),
		 ave_read(ave, AVE_BANK_DPE, 0x143180));

	/* 2. every stage's host-if +0/+4/+0xc/+0x10/+0x14 first, +8 last */
	for (i = 0; i < ARRAY_SIZE(hif); i++)
		dev_info(ave->dev,
			 "session: diag hif %-11s +0 %#010x +4 %#010x +c %#010x +10 %#010x +14 %#010x\n",
			 hif[i].name,
			 ave_read(ave, AVE_BANK_DPE, hif[i].off),
			 ave_read(ave, AVE_BANK_DPE, hif[i].off + 0x4),
			 ave_read(ave, AVE_BANK_DPE, hif[i].off + 0xc),
			 ave_read(ave, AVE_BANK_DPE, hif[i].off + 0x10),
			 ave_read(ave, AVE_BANK_DPE, hif[i].off + 0x14));
	for (i = 0; i < ARRAY_SIZE(hif); i++)
		dev_info(ave->dev, "session: diag hif %-11s +8 %#010x\n",
			 hif[i].name, ave_read(ave, AVE_BANK_DPE, hif[i].off + 0x8));

	/* 3. neighbour DMA channels: readers 0x40D120C00.., writers 0x40D130600.. */
	for (i = 0; i < 0x60; i += 0x10)
		dev_info(ave->dev,
			 "session: diag nbr rd +%#04x %08x %08x %08x %08x | wr %08x %08x %08x %08x\n",
			 i,
			 ave_read(ave, AVE_BANK_DPE, 0x20c00 + i),
			 ave_read(ave, AVE_BANK_DPE, 0x20c04 + i),
			 ave_read(ave, AVE_BANK_DPE, 0x20c08 + i),
			 ave_read(ave, AVE_BANK_DPE, 0x20c0c + i),
			 ave_read(ave, AVE_BANK_DPE, 0x30600 + i),
			 ave_read(ave, AVE_BANK_DPE, 0x30604 + i),
			 ave_read(ave, AVE_BANK_DPE, 0x30608 + i),
			 ave_read(ave, AVE_BANK_DPE, 0x3060c + i));

	/* how much of the 0xA5-filled Info[0] / Pixel[0] did the writers change? */
	if (session_nbr_fill && bufs->nbr_cpu[0][0] && bufs->nbr_cpu[1][0]) {
		static const char * const gname[2] = { "Info[0]", "Pixel[0]" };
		int g;

		dma_rmb();
		for (g = 0; g < 2; g++) {
			const u8 *b = bufs->nbr_cpu[g][0];
			size_t k, changed = 0, first = SIZE_MAX, last = 0;

			for (k = 0; k < bufs->nbr_slot; k++) {
				if (b[k] != 0xa5) {
					changed++;
					if (first == SIZE_MAX)
						first = k;
					last = k;
				}
			}
			dev_info(ave->dev,
				 "session: diag nbr fill %s: %zu of %zu bytes changed%s (first %#zx last %#zx)\n",
				 gname[g], changed, bufs->nbr_slot,
				 changed ? "" : " - the writer never stored anything",
				 changed ? first : 0, last);
		}
	}
}

/*
 * docs/60 reads: the pipe's hardware write channels (full 0x40 windows), once
 * after Start_AVC and again at the Process timeout, so a channel the firmware
 * programs for the frame shows up as a change. Read-only, bank 0.
 */
static void ave_session_diag_channels(struct ave_device *ave, const char *tag)
{
	static const u32 ch[] = { 0x30240, 0x30300, 0x30380, 0x303c0, 0x30400,
				  0x30440, 0x30480, 0x30600, 0x30640, 0x30700,
				  0x30780, 0x20bc0,
				  /*
				   * The source reader. F17: the frame completed
				   * with every stage counting 3845 MBs, yet the
				   * reconstruction was empty and the source ramp
				   * untouched - so where this channel points is
				   * the question.
				   */
				  0x20000, 0x20040, 0x20080, 0x200c0,
				  /*
				   * The third reader channel. ProcessPipeReset
				   * only initialises it when wire 0xFCE9 is set
				   * (fw 0x4edf0), and we have always sent 0 -
				   * so this window has never been looked at.
				   */
				  0x20100, 0x20140 };
	int i, k;

	for (i = 0; i < ARRAY_SIZE(ch); i++) {
		u32 v[16];

		for (k = 0; k < 16; k++)
			v[k] = ave_read(ave, AVE_BANK_DPE, ch[i] + 4 * k);
		dev_info(ave->dev,
			 "session: chan [%s] %llx: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 tag, 0x40D100000ULL + ch[i], v[0], v[1], v[2], v[3], v[4],
			 v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13],
			 v[14], v[15]);
	}
}

/*
 * The parameters the encoder actually ran with. The minimum addition to the
 * last binary that reached stage 15 five times out of five - no ABI change,
 * no new module parameter, just reads (docs/70 §9.1). Every offset is inside
 * the DPE bank's 0x45c000.
 *
 * curMB is read for IntraEst AND for ModeDecision, because ModeDec provably
 * processes macroblocks: without that control, IntraEst's 0 means nothing,
 * which is the mistake docs/70 caught.
 */
static void ave_session_diag_costs(struct ave_device *ave)
{
	u32 i, v[6];

	/*
	 * Every group is opt-in and announces itself first. This function is
	 * what hung the machine in f25, f26 and f29 - the known-good driver
	 * plus these reads and nothing else died - so it must not be possible
	 * to reach any of it by accident, and when it does hang, the last
	 * marker on disk has to name which group did it.
	 */
	if (!session_costs)
		return;

	if (session_costs & BIT(0)) {
		ave_step(ave, "diag costs: group 1, IntraEst cfg 0x40D24A1C8..1D8");
		for (i = 0; i < 6; i++)
			v[i] = ave_read(ave, AVE_BANK_DPE, 0x14a1c8 + 4 * i);
		dev_info(ave->dev,
			 "session: diag QP/lambda QPY %u nQuant %#x +0x1D0 %#x +0x1D4 %#x +0x1D8 %#x | enable 0x40D24A394 %#x\n",
			 v[0], v[1], v[2], v[3], v[4],
			 ave_read(ave, AVE_BANK_DPE, 0x14a394));
	}
	if (session_costs & BIT(1)) {
		/*
		 * 0x40D26A09C..0x104: the two lambda words, then record 17's
		 * 25 words (docs/73 §1.3) - word 0 intra enable, word 1 skip,
		 * 2..24 the ladder. Until f42 this read 0x0AC..0x108, so its
		 * last "0" was 0x108, past the ladder.
		 */
		ave_step(ave, "diag costs: group 2, ModeDec 0x40D26A09C..104");
		for (i = 0; i < 27; i += 9)
			dev_info(ave->dev,
				 "session: diag ModeDec 0x40D26A%03X: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 0x09c + 4 * i,
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 0)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 1)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 2)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 3)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 4)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 5)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 6)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 7)),
				 ave_read(ave, AVE_BANK_DPE, 0x16a09c + 4 * (i + 8)));
	}
	if (session_costs & BIT(2)) {
		/*
		 * IntraEst's curMB next to ModeDecision's. ModeDec provably
		 * processes macroblocks, so its value is the control that
		 * makes IntraEst's mean anything - the control docs/70 caught
		 * me reasoning without.
		 */
		ave_step(ave, "diag costs: group 3, curMB 0x40D243180 / 0x40D263180");
		dev_info(ave->dev,
			 "session: diag curMB IntraEst %#010x %#010x | ModeDec %#010x %#010x (the control)\n",
			 ave_read(ave, AVE_BANK_DPE, 0x143180),
			 ave_read(ave, AVE_BANK_DPE, 0x143184),
			 ave_read(ave, AVE_BANK_DPE, 0x163180),
			 ave_read(ave, AVE_BANK_DPE, 0x163184));
	}
	if (session_costs & BIT(3)) {
		/*
		 * IntraEst MCPU DMem is firmware offset 0x1448000 (docs/58
		 * section 1.2), AP 0x40D448000, DPE offset 0x348000. This group
		 * used 0x248000 (AP 0x40D348000), which is no MCPU block, and
		 * f38 took an SError on it with the wired receiver watching.
		 * One read per marker, so a fault names its access.
		 */
		ave_step(ave, "diag costs: group 4a, IntraEst DMem 0x40D448000");
		v[0] = ave_read(ave, AVE_BANK_DPE, 0x348000);
		ave_step(ave, "diag costs: group 4b, IntraEst DMem 0x40D448764");
		v[1] = ave_read(ave, AVE_BANK_DPE, 0x348764);
		ave_step(ave, "diag costs: group 4c, ME 0x40D190630");
		v[2] = ave_read(ave, AVE_BANK_DPE, 0x90630);
		ave_step(ave, "diag costs: group 4d, ModeDec DMem 0x40D468000");
		v[3] = ave_read(ave, AVE_BANK_DPE, 0x368000);
		ave_step(ave, "diag costs: group 4e, ModeDec DMem 0x40D4689AC");
		v[4] = ave_read(ave, AVE_BANK_DPE, 0x3689ac);
		dev_info(ave->dev,
			 "session: diag IntraEst DMem %#x +0x764 %#x | MESATDSCALING %#x | ModeDec DMem %#x +0x9AC %#x\n",
			 v[0], v[1], v[2], v[3], v[4]);
	}
	if (session_costs & BIT(4)) {
		/*
		 * docs/74 R1: the quantiser scale registers setPipe writes every
		 * frame (fw 0x576b0-0x57814). IntraEst's page is group 1's
		 * (0x40D24A1C8); ReconLuma's is the one diag_mcpu reads every run
		 * (0x40D28A080). QPY/nQuant at 0x090/0x094 are the controls.
		 */
		static const u32 ie[] = { 0x14a088, 0x14a08c, 0x14a0c8 };
		static const u32 rl[] = { 0x18a088, 0x18a08c, 0x18a090, 0x18a094,
					  0x18a0a0, 0x18a0a4, 0x18a0e0, 0x18a120 };
		u32 r[ARRAY_SIZE(rl)];

		ave_step(ave, "diag costs: group 5a, IntraEst scaling 0x40D24A088/08C/0C8");
		for (i = 0; i < ARRAY_SIZE(ie); i++)
			v[i] = ave_read(ave, AVE_BANK_DPE, ie[i]);
		ave_step(ave, "diag costs: group 5b, ReconLuma 0x40D28A088..120");
		for (i = 0; i < ARRAY_SIZE(rl); i++)
			r[i] = ave_read(ave, AVE_BANK_DPE, rl[i]);
		dev_info(ave->dev,
			 "session: diag scaling IntraEst 088 %#x 08C %#x 0C8 %#x | ReconLuma 8x8 %#x SKIPMODE %#x QPY %u nQuant %#x 0A0 %#x 0A4 %#x 0E0 %#x 120 %#x\n",
			 v[0], v[1], v[2], r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
	}
	if (session_costs & BIT(5)) {
		/* docs/74 R4: ReconChroma's page; never read before f47. */
		ave_step(ave, "diag costs: group 6, ReconChroma 0x40D2AA08C/0A4");
		v[0] = ave_read(ave, AVE_BANK_DPE, 0x1aa08c);
		v[1] = ave_read(ave, AVE_BANK_DPE, 0x1aa0a4);
		dev_info(ave->dev,
			 "session: diag ReconChroma SKIPMODE %#x scaling 0A4 %#x\n",
			 v[0], v[1]);
	}
}

/* docs/60 Q5: MCPU counters, stacks, stage context/go registers. Read-only. */
static void ave_session_diag_mcpu(struct ave_device *ave,
				  struct ave_sess_bufs *bufs)
{
	static const struct { const char *name; u32 base; } stk[] = {
		{ "ModeDec", 0x368f60 }, { "ReconLuma", 0x388f60 },
	};
	int i, k;

	dev_info(ave->dev,
		 "session: diag MCPU counters ModeDec entries %u ReconLuma granted %u CAVLC entries %u\n",
		 ave_read(ave, AVE_BANK_DPE, 0x368a1c),
		 ave_read(ave, AVE_BANK_DPE, 0x388280),
		 ave_read(ave, AVE_BANK_DPE, 0x3c87a4));
	for (i = 0; i < ARRAY_SIZE(stk); i++)
		for (k = 0; k < 0xa0; k += 0x20)
			dev_info(ave->dev,
				 "session: diag stack %-9s %llx: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 stk[i].name, 0x40D100000ULL + stk[i].base + k,
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 4),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 8),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 12),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 16),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 20),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 24),
				 ave_read(ave, AVE_BANK_DPE, stk[i].base + k + 28));
	dev_info(ave->dev,
		 "session: diag ModeDec ctx 0x40D263000 %08x +180 %08x +184 %08x +228 %08x go 0x40D26A080 %08x +084 %08x | ReconLuma ctx 0x40D283000 %08x +180 %08x +184 %08x +228 %08x go 0x40D28A080 %08x\n",
		 ave_read(ave, AVE_BANK_DPE, 0x163000),
		 ave_read(ave, AVE_BANK_DPE, 0x163180),
		 ave_read(ave, AVE_BANK_DPE, 0x163184),
		 ave_read(ave, AVE_BANK_DPE, 0x163228),
		 ave_read(ave, AVE_BANK_DPE, 0x16a080),
		 ave_read(ave, AVE_BANK_DPE, 0x16a084),
		 ave_read(ave, AVE_BANK_DPE, 0x183000),
		 ave_read(ave, AVE_BANK_DPE, 0x183180),
		 ave_read(ave, AVE_BANK_DPE, 0x183184),
		 ave_read(ave, AVE_BANK_DPE, 0x183228),
		 ave_read(ave, AVE_BANK_DPE, 0x18a080));

	/*
	 * f33 and f34 (2026-09-22) both hung the machine inside this loop -
	 * a CPU read of coherent memory, 0.2 ms after the frame completed -
	 * with stream 15 attached translating and every IOVA below 2 GiB.
	 * The marker used to sit after the loop, which put the death one
	 * line later than it was. It is before the loop now, and the loop
	 * is behind session_diag_coloc so a run can skip the one memory
	 * access known to coincide with the hang.
	 */
	if (session_diag_coloc && bufs->coloc_size && bufs->coloc_cpu[0]) {
		const u8 *b = bufs->coloc_cpu[0];
		size_t n, changed = 0;

		ave_step(ave, "diag: colocated scan (CPU read of %zu bytes) - f33/f34 hung here", bufs->coloc_size);
		dma_rmb();
		for (n = 0; n < bufs->coloc_size; n++)
			changed += b[n] != 0x5a;
		dev_info(ave->dev, "session: diag colocated slot 0: %zu of %zu bytes changed\n",
			 changed, bufs->coloc_size);
	}
}

/*
 * Append a completed frame's Annex-B bytes to the session's stream: the
 * parameter sets first on frame 0 unless the coded buffer already starts
 * with an SPS, then its slices in order. This is the per-frame copy-out the
 * V4L2 CAPTURE path will make (docs/68 §5.4 design B).
 */
static int ave_sess_stream_append(struct ave_sess_bufs *b, u32 idx, bool first)
{
	bool psets = first &&
		ave_session_nal_type(b->coded[idx].cpu, b->coded[idx].len) != 7;
	size_t need = psets ? b->psets_len : 0;
	u32 sl;
	u8 *p;

	for (sl = 0; sl < b->coded[idx].n_slice; sl++)
		need += b->coded[idx].slice[sl].len;
	if (b->stream_len + need > b->stream_cap) {
		size_t cap = max(2 * b->stream_cap, b->stream_len + need + SZ_64K);

		p = vmalloc(cap);
		if (!p)
			return -ENOMEM;
		if (b->stream)
			memcpy(p, b->stream, b->stream_len);
		vfree(b->stream);
		b->stream = p;
		b->stream_cap = cap;
	}
	p = b->stream + b->stream_len;
	dma_rmb();
	if (psets) {
		memcpy(p, b->psets_cpu, b->psets_len);
		p += b->psets_len;
	}
	for (sl = 0; sl < b->coded[idx].n_slice; sl++) {
		const struct ave_coded_slice *e = &b->coded[idx].slice[sl];

		memcpy(p, (u8 *)b->coded[idx].cpu + e->off, e->len);
		p += e->len;
	}
	b->stream_len = p - b->stream;
	return 0;
}

static int ave_session_process(struct ave_device *ave,
			       const struct ave_cmd_abi *abi,
			       struct ave_sess_bufs *bufs, u64 client_id,
			       u32 n)
{
	struct ave_cmd_ctx ctx = { .count = 4 + n, .client_id = client_id };
	const u32 idx = bufs->n_coded ? n % bufs->n_coded : 0;
	const u32 slot = AVE_SESS_PROCESS_SLOT + idx;
	struct ave_avc_frame f = {};
	struct ave_coded_info info;
	struct ave_sess_arena recon = {};
	dma_addr_t cmd_iova, luma_iova, chroma_iova;
	dma_addr_t ry, ruv, ry_lsb, ruv_lsb, rmv;
	u32 cw, ch, stride, luma_bytes, chroma_bytes, mb_w, mb_h;
	size_t cmd_len, psets_len, scan_len;
	u8 *luma, *chroma;
	void *cmd;
	int ret;

	if (!bufs->n_coded || !bufs->coded[idx].cpu)
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

	if (bufs->src[idx].luma) {
		/* This index's planes; the frame that last used them is done. */
		luma = bufs->src[idx].luma;
		luma_iova = bufs->src[idx].luma_iova;
		chroma = bufs->src[idx].chroma;
		chroma_iova = bufs->src[idx].chroma_iova;
	} else {
		luma = ave_sess_dma_alloc(bufs, luma_bytes, &luma_iova);
		chroma = ave_sess_dma_alloc(bufs, chroma_bytes, &chroma_iova);
		if (!luma || !chroma)
			return -ENOMEM;
	}
	if ((luma_iova | chroma_iova) & (AVE_STRIDE_ALIGN - 1)) {
		dev_err(ave->dev,
			"session: frame: input planes not 64-aligned (%pad / %pad)\n",
			&luma_iova, &chroma_iova);
		return -EINVAL;
	}
	/*
	 * A different picture per frame. Without this, a P frame that never
	 * read its own source is indistinguishable from one that did - which
	 * is exactly the ambiguity F16 and F17 left us in.
	 */
	ave_session_fill_input(luma, chroma, stride, cw, ch, n * 8);
	if (session_flat_luma)
		dev_info(ave->dev,
			 "session: source luma is a CONSTANT %u, not the ramp: a decoded picture at %u means the source DMA read our buffer, one at ~130 means it did not (docs/62 §6)\n",
			 session_flat_luma, session_flat_luma);
	bufs->src[idx].luma = luma;
	bufs->src[idx].luma_iova = luma_iova;
	bufs->src[idx].luma_bytes = luma_bytes;
	bufs->src[idx].chroma = chroma;
	bufs->src[idx].chroma_iova = chroma_iova;
	bufs->src[idx].chroma_bytes = chroma_bytes;
	bufs->stride = stride;
	if (!n) {
		bufs->src_cpu = luma;
		bufs->src_size = min_t(size_t, luma_bytes, SZ_256K);
	}

	/*
	 * Reconstruction planes. setPipe asserts all four are non-zero and
	 * 128-aligned behind their gates (fw 0x55358/0x55310 Y_MSB,
	 * 0x580b0/0x58068 UV_MSB, 0x550e4/0x54f94 Y_LSB, 0x55978/0x5541c
	 * UV_LSB), plus a colocated MV store. The Start-time DPB entry points
	 * at the same arena; the sizes below are the docs/38 §7 tile formula
	 * rounded up, which is an over-estimate and therefore safe.
	 */
	/*
	 * Allocated once and reused by every frame. These per-frame PICMGMT
	 * recon pointers are inert - setRefPointers rebuilds them from the
	 * firmware's own DPB record before setPipe reads them (docs/64 §1) -
	 * but setPipe asserts they are non-zero and 128-aligned, so they have
	 * to be something valid.
	 */
	if (!bufs->pic_recon.cpu) {
		bufs->pic_recon.size = (size_t)cw * ch * 3 +
				       (size_t)mb_w * mb_h * 1024 + SZ_64K;
		bufs->pic_recon.cpu = ave_sess_dma_alloc(bufs,
							 bufs->pic_recon.size,
							 &bufs->pic_recon.iova);
		if (!bufs->pic_recon.cpu)
			return -ENOMEM;
		memset(bufs->pic_recon.cpu, 0, bufs->pic_recon.size);
	}
	recon = bufs->pic_recon;
	recon.used = 0;
	if (!ave_sess_arena_take(&recon, (size_t)cw * ch, &ry) ||
	    !ave_sess_arena_take(&recon, (size_t)cw * ch / 2, &ruv) ||
	    !ave_sess_arena_take(&recon, (size_t)cw * ch, &ry_lsb) ||
	    !ave_sess_arena_take(&recon, (size_t)cw * ch / 2, &ruv_lsb) ||
	    !ave_sess_arena_take(&recon, (size_t)mb_w * mb_h * 1024, &rmv))
		return -ENOMEM;

	cmd_len = ave_cmd_size(abi, AVE_OP_PROCESS_AVC);
	if (!bufs->proc_cmd[idx].cpu) {
		bufs->proc_cmd[idx].cpu = ave_sess_ipc_alloc(bufs, cmd_len,
							     &bufs->proc_cmd[idx].iova);
		if (!bufs->proc_cmd[idx].cpu)
			return -ENOMEM;
	}
	cmd = bufs->proc_cmd[idx].cpu;
	cmd_iova = bufs->proc_cmd[idx].iova;

	/*
	 * Frame 0 is the IDR the session opens with; the rest are P frames
	 * (jump table fw 0xcef80: 1 = P). session_frame_type still chooses
	 * what frame 0 is, so the I-vs-IDR bisect it exists for still works.
	 */
	f.frame_type = n ? AVE_FRAME_TYPE_P : session_frame_type;
	/*
	 * frameInfo.frameNumber - monotone, and the word the firmware's queue
	 * keys Complete and Dequeue on. ManageDPBBuffer asserts it is not
	 * below m_iFirstFrameNumber and spins on "b ." if it is (fw 0x2d350).
	 */
	f.frame_num = n;
	f.in_luma_addr = luma_iova;
	f.in_luma_stride = stride;
	f.in_luma_size = luma_bytes;
	f.in_chroma_addr = chroma_iova;
	f.in_chroma_stride = stride;
	f.in_chroma_size = chroma_bytes;

	/* out_mode is left 0, so the size check is against the Start table. */
	f.coded_index = idx;
	f.coded_addr = bufs->coded[idx].iova;
	f.coded_hdr_addr = bufs->coded_hdr[idx].iova;
	f.coded_size = bufs->coded[idx].size;
	/*
	 * ave_cmd_coded_length() stops at the first slice record with a zero
	 * ui32BytesWritten, and nothing was found on the firmware's
	 * ProcessTranscodeDone path that clears this buffer between frames.
	 * A frame with fewer slices than its predecessor would then read the
	 * previous frame's records and over-report. Cheap insurance; docs/64
	 * §8 could not prove it is needed, only that it might be.
	 */
	memset(bufs->coded_hdr[idx].cpu, 0, bufs->coded_hdr[idx].size);

	f.recon_luma_addr = ry;
	f.recon_chroma_addr = ruv;
	f.recon_luma_lsb_addr = ry_lsb;
	f.recon_chroma_lsb_addr = ruv_lsb;
	f.recon_mv_addr = rmv;

	f.ctx_index = 0;
	f.force_key_frame = !n && session_frame_type == AVE_FRAME_TYPE_IDR;
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

	/* encoder_addr_entropy[i][0], docs/54; 0 rows = the :8020 control. */
	if (bufs->n_entropy) {
		memcpy(f.entropy, bufs->entropy, sizeof(f.entropy));
		f.n_entropy = bufs->n_entropy;
		f.n_entropy_cols = bufs->n_entropy_cols;
	}

	if (bufs->n_nbr && abi->process_avc.src_nbr_max) {
		memcpy(f.src_nbr, bufs->nbr, sizeof(f.src_nbr));
		f.n_src_nbr = min(bufs->n_nbr, abi->process_avc.src_nbr_max);
	}

	ret = ave_cmd_build_process_avc(abi, cmd, cmd_len, &ctx,
					slot, &f);
	if (ret < 0) {
		dev_err(ave->dev, "session: Process build failed: %d\n", ret);
		return ret;
	}

	dev_info(ave->dev,
		 "session: Process: %ux%u coded, stride %u, luma %pad/%#x chroma %pad/%#x, frame_type %u, slot %u\n",
		 cw, ch, stride, &luma_iova, luma_bytes, &chroma_iova,
		 chroma_bytes, f.frame_type, slot);
	dev_info(ave->dev,
		 "session: Process: coded %pad/%#x hdr %pad/%#x recon Y %pad UV %pad MV %pad\n",
		 &bufs->coded[idx].iova, bufs->coded[idx].size,
		 &bufs->coded_hdr[idx].iova, bufs->coded_hdr[idx].size,
		 &ry, &ruv, &rmv);
	dev_info(ave->dev,
		 "session: Process: LowResSrcLumaScaled %#llx at PICMGMT+%#x - INERT, setRefPointers overwrites it from DPB slot 0 (fw 0x2c320); LowResResults left zero\n",
		 f.low_res_src_addr, abi->process_avc.low_res_src);

	/*
	 * Process starts the encoder hardware, whose DMA translates through the
	 * datapath DART (F4). If that DART's tables do not match the CPUDART's,
	 * the run can only end in an SMMU fault storm - F4's did, and the
	 * machine reset shortly after. Check first, refuse on a mismatch.
	 */
	if (!session_ignore_dart) {
		ret = ave_dart_datapath_check(ave, "before Process");
		if (ret) {
			dev_err(ave->dev,
				"session: Process: NOT SENT - the datapath DART does not translate like the CPUDART (%d); session_ignore_dart=1 overrides\n",
				ret);
			return ret;
		}
	}

	if (session_sve_ungate) {
		ave_write(ave, AVE_BANK_SVE, AVE_SVE_IDLE, 0);
		dev_info(ave->dev, "session: SVE +0x%x <- 0 (clock gating off) for Process\n",
			 AVE_SVE_IDLE);
	}

	ret = ave_session_cmd(ave, abi, AVE_OP_PROCESS_AVC, "Process",
			      cmd_iova, cmd_len, client_id);

	/*
	 * Did the firmware program the pipe's recon writer? The only code that
	 * does (fw 0x54f90, behind NEED_LSB_PLANES) writes 0x800314B1 to
	 * 0x40D130240 and the recon Y/UV addresses to +0xC/+0x1C/+0xDC.
	 * Configuration registers the firmware writes; read-only here, and
	 * deliberately not the interrupt-status registers (docs/57 #4).
	 */
	dev_info(ave->dev,
		 "session: recon writer 0x40D130240 = %#010x (want 0x800314b1 when programmed); +0x24c %#010x +0x25c %#010x +0x31c %#010x\n",
		 ave_read(ave, AVE_BANK_DPE, 0x30240),
		 ave_read(ave, AVE_BANK_DPE, 0x3024c),
		 ave_read(ave, AVE_BANK_DPE, 0x3025c),
		 ave_read(ave, AVE_BANK_DPE, 0x3031c));

	/*
	 * Did the source reader get OUR buffer? CAVCController::setPipe writes
	 * the low 32 bits of PICMGMT +0x8C0 to 0x40D120010 and the stride to
	 * +0x14, chroma to +0x90/+0x94, and the mode pair to +0x50/+0xD0 from
	 * wire 0xFEC0 (docs/62 §6.2). F17 left the comparison to be done by
	 * hand afterwards; do it here, where the expected value is known.
	 */
	{
		u32 got = ave_read(ave, AVE_BANK_DPE, 0x20010);
		u32 want = lower_32_bits(luma_iova);

		dev_info(ave->dev,
			 "session: source reader 0x40D120010 = %#010x (want %#010x: %s) stride +0x14 %#x (want %#x) chroma +0x90 %#010x (want %#010x) fmt +0x0C %#010x mode +0x50 %#x +0xD0 %#x\n",
			 got, want,
			 got == want ? "OUR BUFFER" :
			 got ? "A DIFFERENT ADDRESS" : "NEVER PROGRAMMED",
			 ave_read(ave, AVE_BANK_DPE, 0x20014), stride,
			 ave_read(ave, AVE_BANK_DPE, 0x20090),
			 lower_32_bits(chroma_iova),
			 ave_read(ave, AVE_BANK_DPE, 0x2000c),
			 ave_read(ave, AVE_BANK_DPE, 0x20050),
			 ave_read(ave, AVE_BANK_DPE, 0x200d0));
	}

	/*
	 * The headline result first, before any diagnostic touches the block.
	 * f33 (2026-09-22) completed the frame and then hung the machine inside
	 * the diagnostics below, and because the coded length was only logged
	 * after them, the one number that run existed to produce - is the frame
	 * still 2709 bytes now that the stream-15 reads succeed - was lost.
	 * These are CPU reads of coherent memory the firmware has finished
	 * with; they cannot hang anything.
	 */
	if (!ret) {
		struct ave_coded_info early;
		const u8 *h = bufs->coded_hdr[idx].cpu;
		u32 mbs = (cw / AVE_MB_SIZE) * (ch / AVE_MB_SIZE);
		u32 k, i_mb = 0, p_mb = 0, skip_mb = 0;

		dma_rmb();
		for (k = 0; k < 4; k++) {
			i_mb += get_unaligned_le32(h + abi->coded_hdr.i_mb_cnt + 4 * k);
			p_mb += get_unaligned_le32(h + abi->coded_hdr.p_mb_cnt + 4 * k);
			skip_mb += get_unaligned_le32(h + abi->coded_hdr.skip_mb_cnt + 4 * k);
		}
		if (!ave_cmd_coded_length(abi, h, bufs->coded_hdr[idx].size,
					  bufs->coded[idx].size, &early))
			dev_info(ave->dev,
				 "session: RESULT frame %u: %u coded bytes, MB I %u P %u skip %u of %u, FrameTypeReturned %u (2709 has been the blank-frame signature)\n",
				 n, early.bytes, i_mb, p_mb, skip_mb, mbs,
				 early.frame_type);
		else
			dev_info(ave->dev,
				 "session: RESULT frame %u: coded header undecodable; MB I %u P %u skip %u of %u\n",
				 n, i_mb, p_mb, skip_mb, mbs);
	}
	ave_step(ave, "frame result logged; next: post-frame diagnostics");

	/*
	 * docs/57 #3 and #4, read-only, at the moment Process gave up:
	 *  - PMGR power state of VENC_DMA, PIPE4, PIPE5, ME0, ME1 (bank 3 +0x00..
	 *    +0x20, docs/27 7): is ME1 - which no DT domain powers - off?
	 *  - 0x40D110140 (pipe done status, bit 2) and 0x40D110128 (bit 0 "go"),
	 *    0x40D120000/4 (AXI error): did the hardware finish and the done
	 *    interrupt get lost, or did it never finish? The firmware reads
	 *    0x40D110140 before acking it, so this read may disturb the
	 *    diagnosis - never the machine - and the frame is already lost here.
	 *  - SVE scratch 7: the heartbeat sets bit 26 on PIPE HANG.
	 */
	/*
	 * On failure these say where the pipe stopped; on success they say
	 * whether the source reader actually walked the frame - F16 completed
	 * a frame whose picture was flat, so "it finished" is not evidence.
	 */
	if (session_diag) {
		dev_info(ave->dev,
			 "session: diag PS DMA %#010x PIPE4 %#010x PIPE5 %#010x ME0 %#010x ME1 %#010x\n",
			 ave_read(ave, AVE_BANK_PMGR_PS, 0x00),
			 ave_read(ave, AVE_BANK_PMGR_PS, 0x08),
			 ave_read(ave, AVE_BANK_PMGR_PS, 0x10),
			 ave_read(ave, AVE_BANK_PMGR_PS, 0x18),
			 ave_read(ave, AVE_BANK_PMGR_PS, 0x20));
		/*
		 * docs/58 corrections: 0x40D120000 is the source-DMA config word
		 * setPipe writes (0x80034045), not an AXI error; the AXI-error
		 * registers are 0x40D124000/4, 0x40D12C000, 0x40D134000 (fw
		 * 0x38b20). 0x40D110128 bit 0 is SRCDMAGO.
		 */
		dev_info(ave->dev,
			 "session: diag 0x40D110140 %#010x (pipe done = bit 2) enable 0x40D11013C %#010x (want bit 2) SRCDMAGO 0x40D110128 %#010x srcdma cfg 0x40D120000 %#010x +4 %#010x scratch7 %#010x\n",
			 ave_read(ave, AVE_BANK_DPE, 0x10140),
			 ave_read(ave, AVE_BANK_DPE, 0x1013c),
			 ave_read(ave, AVE_BANK_DPE, 0x10128),
			 ave_read(ave, AVE_BANK_DPE, 0x20000),
			 ave_read(ave, AVE_BANK_DPE, 0x20004),
			 ave_read(ave, AVE_BANK_SVE, AVE_SVE_SCRATCH(7)));
		{
			u32 prog = ave_read(ave, AVE_BANK_DPE, 0x2002c);

			/* docs/58 7.0: currMbRow = 0x40D12002C >> 16; 44 = last of 45 rows */
			dev_info(ave->dev,
				 "session: diag srcdma 0x40D12002C %#010x -> currMbRow %u; AXI err 0x40D124000 %#010x 0x40D124004 %#010x 0x40D12C000 %#010x 0x40D134000 %#010x\n",
				 prog, prog >> 16,
				 ave_read(ave, AVE_BANK_DPE, 0x24000),
				 ave_read(ave, AVE_BANK_DPE, 0x24004),
				 ave_read(ave, AVE_BANK_DPE, 0x2c000),
				 ave_read(ave, AVE_BANK_DPE, 0x34000));
		}
		dev_info(ave->dev,
			 "session: diag DPE DC000 %#010x DC004 %#010x DC400 %#010x DC4A4 %#010x DC5B0 %#010x\n",
			 ave_read(ave, AVE_BANK_DPE, 0xdc000),
			 ave_read(ave, AVE_BANK_DPE, 0xdc004),
			 ave_read(ave, AVE_BANK_DPE, 0xdc400),
			 ave_read(ave, AVE_BANK_DPE, 0xdc4a4),
			 ave_read(ave, AVE_BANK_DPE, 0xdc5b0));
		{
			/* MCPU run control +8 (want 1) and ID +4 (want 4..10), docs/58 1.2 */
			static const u32 mcpu[] = { 0x310000, 0x350000, 0x330000, 0x370000,
						    0x390000, 0x3b0000, 0x3d0000 };
			char line[256];
			int n = 0, i;

			for (i = 0; i < ARRAY_SIZE(mcpu); i++)
				n += scnprintf(line + n, sizeof(line) - n, " %llx:%x/%x",
					       0x40D100000ULL + mcpu[i],
					       ave_read(ave, AVE_BANK_DPE, mcpu[i] + 8),
					       ave_read(ave, AVE_BANK_DPE, mcpu[i] + 4));
			dev_info(ave->dev, "session: diag MCPU run/id%s\n", line);
		}
		dev_info(ave->dev,
			 "session: diag MCPU IMem[0] MbInput %#010x (want 0x10004000) IntraEst %#010x (0x10001000) CAVLC %#010x (0x10001800)\n",
			 ave_read(ave, AVE_BANK_DPE, 0x300000),
			 ave_read(ave, AVE_BANK_DPE, 0x340000),
			 ave_read(ave, AVE_BANK_DPE, 0x3c0000));
		/* host interfaces: +0/+4 first; +8 (irq status, side effect U) last */
		dev_info(ave->dev,
			 "session: diag MCPU if MbInput %#x %#x IntraEst %#x %#x CAVLC %#x %#x | +8: %#x %#x %#x\n",
			 ave_read(ave, AVE_BANK_DPE, 0x68000),
			 ave_read(ave, AVE_BANK_DPE, 0x68004),
			 ave_read(ave, AVE_BANK_DPE, 0x142000),
			 ave_read(ave, AVE_BANK_DPE, 0x142004),
			 ave_read(ave, AVE_BANK_DPE, 0x1c8000),
			 ave_read(ave, AVE_BANK_DPE, 0x1c8004),
			 ave_read(ave, AVE_BANK_DPE, 0x68008),
			 ave_read(ave, AVE_BANK_DPE, 0x142008),
			 ave_read(ave, AVE_BANK_DPE, 0x1c8008));
		ave_step(ave, "diag: row0");
		ave_session_diag_row0(ave, bufs);
		ave_step(ave, "diag: mcpu (f33 died between its last line and the colocated scan)");
		ave_session_diag_mcpu(ave, bufs);
		ave_step(ave, "diag: costs (session_costs gated)");
		ave_session_diag_costs(ave);
		ave_step(ave, "diag: channels");
		ave_session_diag_channels(ave, "timeout");
		ave_step(ave, "diag: done");
	}

	if (session_sve_ungate) {
		ave_write(ave, AVE_BANK_SVE, AVE_SVE_IDLE, 1);
		dev_info(ave->dev, "session: SVE +0x%x <- 1 after Process\n", AVE_SVE_IDLE);
	}
	if (ret)
		return ret;

	/*
	 * The completion says nothing about the length; everything comes out
	 * of the coded-header buffer (docs/53 §3).
	 */
	dma_rmb();
	print_hex_dump(KERN_INFO, "session: coded_hdr: ", DUMP_PREFIX_OFFSET,
		       16, 4, bufs->coded_hdr[idx].cpu, 0x40, false);
	print_hex_dump(KERN_INFO, "session: slice0: ", DUMP_PREFIX_OFFSET,
		       16, 4,
		       (u8 *)bufs->coded_hdr[idx].cpu + abi->coded_hdr.slice_bytes_written,
		       0x20, false);

	ret = ave_cmd_coded_length(abi, bufs->coded_hdr[idx].cpu,
				   bufs->coded_hdr[idx].size,
				   bufs->coded[idx].size, &info);
	if (ret) {
		dev_err(ave->dev,
			"session: frame: cannot decode the coded header: %d\n",
			ret);
		return ret;
	}
	dev_info(ave->dev,
		 "session: frame %u: %u bytes in %u slice(s) (written %u - trimmed %u), FrameTypeReturned %u, frame_num %u (sent %u), SPS+PPS %u bits, cabac_zero_words %u\n",
		 n, info.bytes, info.slices, info.span, info.bytes_removed,
		 info.frame_type, info.frame_num, n, info.sps_pps_bits,
		 info.cabac_zero_words);

	/*
	 * FrameTypeReturned == 4 means the firmware DROPPED the frame
	 * (fw 0x13340 / 0x5c930, tested at 0x14bc0). Without naming it, a
	 * dropped frame and a dead pipe look identical from here. docs/67 §5.
	 */
	if (info.frame_type == AVE135_FRAME_TYPE_DROPPED) {
		dev_err(ave->dev,
			"session: frame %u: the firmware DROPPED this frame (FrameTypeReturned 4)\n",
			n);
		return -ENODATA;
	}
	if (info.frame_num != n)
		dev_warn(ave->dev,
			 "session: frame %u: the firmware echoed frameNumber %u, not %u - the field may not be reaching it\n",
			 n, info.frame_num, n);
	/*
	 * Every macroblock must be accounted for by exactly one of the
	 * counters. This is the direct test for F16 and F17's failure shape -
	 * a frame that "completed" having encoded nothing - and it is cheap.
	 */
	{
		u32 mbs = (cw / AVE_MB_SIZE) * (ch / AVE_MB_SIZE);
		const u8 *h = bufs->coded_hdr[idx].cpu;
		u32 k, i_mb = 0, p_mb = 0, skip_mb = 0;

		for (k = 0; k < 4; k++) {
			i_mb += get_unaligned_le32(h + abi->coded_hdr.i_mb_cnt + 4 * k);
			p_mb += get_unaligned_le32(h + abi->coded_hdr.p_mb_cnt + 4 * k);
			skip_mb += get_unaligned_le32(h + abi->coded_hdr.skip_mb_cnt + 4 * k);
		}
		dev_info(ave->dev,
			 "session: frame %u: MB counts I %u P %u skip %u = %u of %u expected%s\n",
			 n, i_mb, p_mb, skip_mb, i_mb + p_mb + skip_mb, mbs,
			 i_mb + p_mb + skip_mb == mbs ? "" : " - MISMATCH");
	}

	if (!info.bytes) {
		dev_err(ave->dev,
			"session: frame: the firmware reported ZERO coded bytes - the encode did not produce a bitstream\n");
		return -ENODATA;
	}
	if (info.bytes > bufs->coded[idx].size) {
		dev_err(ave->dev,
			"session: frame: reported length %u exceeds the coded buffer (%u); header is not what we think it is\n",
			info.bytes, bufs->coded[idx].size);
		return -EPROTO;
	}

	print_hex_dump(KERN_INFO, "session: coded: ", DUMP_PREFIX_OFFSET,
		       16, 1, bufs->coded[idx].cpu, min_t(u32, info.bytes, 64),
		       false);

	/*
	 * ui32_SPSPPSHeaderBits is exact: it is literally sps_bits + pps_bits,
	 * the two memcpy lengths the firmware shifted back up (fw 0x5df48 /
	 * 0x5df70 / 0x5df9c). Use it, and keep the back-scan only as a
	 * cross-check - the scan is wrong by construction the moment a second,
	 * shorter Start_AVC leaves a longer parameter set's tail behind.
	 * docs/67 §4.
	 */
	scan_len = ave_session_psets_len(bufs->psets_cpu, bufs->psets_size);
	if (info.sps_pps_bits % 8)
		dev_warn(ave->dev,
			 "session: frame: SPS+PPS is %u bits, not a whole number of bytes\n",
			 info.sps_pps_bits);
	psets_len = info.sps_pps_bits / 8;
	if (!psets_len || psets_len > bufs->psets_size) {
		dev_warn(ave->dev,
			 "session: frame: SPS+PPS length %zu is unusable; falling back to the back-scan (%zu)\n",
			 psets_len, scan_len);
		psets_len = scan_len;
	}
	dev_info(ave->dev,
		 "session: frame: parameter sets %zu bytes (firmware said %u bits; back-scan says %zu%s), first NAL type %d\n",
		 psets_len, info.sps_pps_bits, scan_len,
		 scan_len == psets_len ? ", agrees" : " - DISAGREES",
		 ave_session_nal_type(bufs->psets_cpu, psets_len));
	print_hex_dump(KERN_INFO, "session: psets: ", DUMP_PREFIX_OFFSET,
		       16, 1, bufs->psets_cpu, min_t(size_t, psets_len, 64),
		       false);

	bufs->coded[idx].len = info.bytes;
	bufs->coded[idx].span = info.span;
	bufs->coded[idx].cabac_zero_words = info.cabac_zero_words;
	bufs->coded[idx].n_slice = min_t(u32, info.n_slice, AVE_CODED_SLICE_MAX);
	memcpy(bufs->coded[idx].slice, info.slice,
	       bufs->coded[idx].n_slice * sizeof(info.slice[0]));
	bufs->psets_len = psets_len;
	bufs->n_done = n + 1;
	return ave_sess_stream_append(bufs, idx, !n);
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

/*
 * Take down the debugfs view without freeing anything behind it. On an
 * unclean teardown the buffers are deliberately leaked, but the debugfs
 * entries must still go: they outlive the module otherwise, and a later run
 * that copies /sys/kernel/debug/apple_ave would silently capture the
 * PREVIOUS load's frame and label it its own. F19b did exactly that.
 */
void ave_session_hide(struct ave_device *ave)
{
	struct ave_sess_bufs *bufs = ave->session_bufs;

	if (!bufs || !bufs->dbg_dir)
		return;
	debugfs_remove_recursive(bufs->dbg_dir);
	bufs->dbg_dir = NULL;
}

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
	unsigned int mark_dma = 0, mark_ipc = 0;
	u32 frame;
	int ret;

	if (!session_selftest && !session_frame)
		return 0;

	if (!ave->running) {
		dev_warn(ave->dev, "session: coprocessor not running; skipping\n");
		return -ENODEV;
	}
	/*
	 * Apple's own limits (kext 0xea3d9c). Nothing checked these before,
	 * and every buffer formula below is a function of the dimensions - an
	 * out-of-range one produces a mis-sized allocation and a DMA fault
	 * rather than an error. docs/66 §4.
	 */
	if (session_width < 192 || session_width > 4096 ||
	    session_height < 96 || session_height > 4096 ||
	    (session_width & 1) || (session_height & 1)) {
		dev_err(ave->dev,
			"session: %ux%u is outside the firmware's 192x96..4096x4096 (and must be even); refusing\n",
			session_width, session_height);
		return -EINVAL;
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
	if (ret || session_config_only)
		goto out;
	/* Everything after these marks belongs to a session, not the device. */
	mark_dma = bufs->ndma;
	mark_ipc = bufs->nipc;

	ret = ave_session_open(ave, abi, bufs, AVE_SESS_CLIENT_ID);
	if (ret)
		goto out;
	/*
	 * From here on the firmware holds a registered client, and unloading
	 * without giving it back is what ave_session_close_client() exists to
	 * undo (docs/63). Record it even if a later step fails: a client that
	 * was opened must be closed whatever happened afterwards.
	 */
	ave->client_open = true;

	/* Must precede Start_AVC: these tables are published in that command. */
	ave_session_alloc_nbr(ave, bufs);
	ave_session_alloc_entropy(ave, abi, bufs);
	ave_session_alloc_dpb(ave, bufs);

	ret = ave_session_start_avc(ave, abi, bufs, AVE_SESS_CLIENT_ID);
	if (!ret && session_frame && session_diag)
		ave_session_diag_channels(ave, "after Start_AVC");
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

	/*
	 * One Process per frame, synchronously: wait for each ENCODE_DONE
	 * before submitting the next. macOS pipelines up to 20 deep, but
	 * nothing here needs the throughput and an outstanding command would
	 * make a failure much harder to attribute. Start_AVC is NOT re-sent:
	 * ProvideReferenceFrames copies the recon, LowResRef and colocated
	 * tables into the firmware's DPB context once, at Start, and the
	 * firmware indexes its own copy per frame (docs/64 §2).
	 */
	for (frame = 0; frame < bufs->n_frames; frame++) {
		ret = ave_session_process(ave, abi, bufs, AVE_SESS_CLIENT_ID,
					  frame);
		if (ret) {
			dev_err(ave->dev,
				"session: frame %u of %u failed: %d\n",
				frame, bufs->n_frames, ret);
			break;
		}
	}
	{
		u32 k, nrep = clamp_t(u32, session_repeat, 1, 8);

		for (k = 1; k < nrep && !ret; k++) {
			ave_step(ave, "session repeat: Stop + Close before the next session");
			ret = ave_session_close_client(ave);
			if (ret) {
				dev_err(ave->dev, "session %u: close failed: %d\n", k, ret);
				break;
			}
			dev_info(ave->dev, "session %u of %u: Open + Start_AVC on the running firmware; freeing the previous session's %u DMA buffers and %u commands\n",
				 k + 1, nrep, bufs->ndma - mark_dma,
				 bufs->nipc - mark_ipc);
			ave_sess_free_to(bufs, mark_dma, mark_ipc);
			/* Caches into what was just freed (pic_recon dangled in f60). */
			memset(bufs->src, 0, sizeof(bufs->src));
			memset(bufs->proc_cmd, 0, sizeof(bufs->proc_cmd));
			memset(&bufs->pic_recon, 0, sizeof(bufs->pic_recon));
			bufs->stream_len = 0;
			bufs->n_done = 0;
			ret = ave_session_open(ave, abi, bufs, AVE_SESS_CLIENT_ID);
			if (ret)
				break;
			ave->client_open = true;
			ave_session_alloc_nbr(ave, bufs);
			ave_session_alloc_entropy(ave, abi, bufs);
			ave_session_alloc_dpb(ave, bufs);
			ret = ave_session_start_avc(ave, abi, bufs, AVE_SESS_CLIENT_ID);
			if (ret)
				break;
			for (frame = 0; frame < bufs->n_frames; frame++) {
				ret = ave_session_process(ave, abi, bufs,
							  AVE_SESS_CLIENT_ID, frame);
				if (ret) {
					dev_err(ave->dev, "session %u: frame %u of %u failed: %d\n",
						k + 1, frame, bufs->n_frames, ret);
					break;
				}
			}
		}
	}
	if (bufs->n_done)
		ave_session_publish(ave, bufs);

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
/* Close the client: what macOS does before it touches power                 */
/* ------------------------------------------------------------------------ */

/*
 * Give the client back to the firmware before unloading (docs/63).
 *
 * Twice now the machine has reset seconds after a clean rmmod - once after a
 * *successful* encode with no faults logged, which is the signature of a
 * stalled fabric transaction and a watchdog rather than a synchronous abort.
 * macOS structurally cannot get into that state: AVE_Drv::PowerOff queues
 * Stop then Close for every live client and drains both queues before any
 * SetPS(..., 0), and it never unmaps anything as part of powering down - the
 * unmapping happens far later, at IOService::stop. We did the exact opposite:
 * gate the domains first, then unmap and free DMA memory, with no Stop and no
 * Close at all.
 *
 * The two replies are the quiesce, not a formality. ProcessUninit (fw 0xf480)
 * cancels the client's outstanding queue slots and then WITHHOLDS UNINIT_DONE
 * until the client's produced and consumed counters match (fw 0xfadc-0xfb10).
 * ProcessStop (fw 0x10690) does not reply at all; it enqueues the Close behind
 * the client's remaining work (0x107e8) and STOP_DONE is emitted only after
 * DestroyClient (0x12a64). So waiting for both is the firmware telling us it
 * has let go of the buffers we are about to free.
 *
 * Returns 0 when both commands completed, a negative errno otherwise. The
 * caller must treat a failure as "the firmware may still be using everything".
 */
int ave_session_close_client(struct ave_device *ave)
{
	const struct ave_cmd_abi *abi = ave_cmd_abi_get(ave->fw_abi);
	void (*prev_rx)(struct ave_device *, u32, void *, u32, u32);
	static const struct {
		enum ave_op	op;
		const char	*name;
	} seq[] = {
		{ AVE_OP_STOP,  "Stop"  },	/* id 6, slot 7  -> UNINIT_DONE */
		{ AVE_OP_CLOSE, "Close" },	/* id 12, slot 4 -> STOP_DONE   */
	};
	unsigned int i;
	int ret = 0;

	if (!ave->client_open)
		return 0;
	if (!abi || !ave->running || !ave->powered) {
		dev_warn(ave->dev,
			 "close: a client is open but the firmware is not up (running %d, powered %d); cannot give it back\n",
			 ave->running, ave->powered);
		return -ENODEV;
	}

	/*
	 * ave_session_cmd() only completes when ave->ipc_rx is our capturing
	 * hook, and the self-test restored the previous one when it returned.
	 * Without this, a *successful* Stop would look like a timeout - the
	 * same class of mistake as polling a scratch word without clearing it
	 * first.
	 */
	init_completion(&ave_sess_rx.done);
	prev_rx = ave->ipc_rx;
	smp_store_release(&ave->ipc_rx, ave_session_ipc_rx);

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		/*
		 * Continue the session's CNT rather than restarting at 0. The
		 * firmware does read it (fw 0xf060, 0xf5a0) and F19 worked
		 * with 0 and 1, so monotonicity is not required - but it is
		 * the one header field where we differed from macOS for no
		 * reason.
		 */
		struct ave_cmd_ctx ctx = { .count = AVE_SESS_CNT_TEARDOWN + i,
					   .client_id = AVE_SESS_CLIENT_ID };
		size_t cmd_len = ave_cmd_size(abi, seq[i].op);
		dma_addr_t cmd_iova;
		u8 *cmd;

		if (!cmd_len) {
			dev_warn(ave->dev, "close: ABI %s has no %s command\n",
				 abi->name, seq[i].name);
			ret = -ENODEV;
			break;
		}
		/*
		 * From the IPC pool, not the session pool: ave_remove() may be
		 * running with session_bufs already gone, and these buffers
		 * must outlive the reply either way.
		 */
		cmd = ave_ipc_alloc(ave, cmd_len, &cmd_iova);
		if (!cmd) {
			ret = -ENOMEM;
			break;
		}
		put_unaligned_le32(AVE_SESS_TIMEOUT_MS, ctx.timeout);
		ret = ave_cmd_build_simple(abi, seq[i].op, cmd, cmd_len, &ctx);
		if (ret < 0) {
			dev_err(ave->dev, "close: %s build failed: %d\n",
				seq[i].name, ret);
			ave_ipc_free(ave, cmd, cmd_len);
			break;
		}
		ret = ave_session_cmd(ave, abi, seq[i].op, seq[i].name,
				      cmd_iova, cmd_len, AVE_SESS_CLIENT_ID);
		/*
		 * Not returned to the pool on failure: a firmware that never
		 * replied may still read the command buffer. ave_ipc_fini()
		 * frees the whole region later regardless.
		 */
		if (!ret)
			ave_ipc_free(ave, cmd, cmd_len);
		else
			break;
	}

	smp_store_release(&ave->ipc_rx, prev_rx);
	synchronize_irq(ave->irq);

	if (ret) {
		/*
		 * A Stop that times out means the deferred-reply path is still
		 * holding UNINIT_DONE because work really is in flight - the
		 * one state in which freeing the buffers is worst.
		 */
		dev_err(ave->dev,
			"close: the firmware did not give the client back (%d); its buffers are still live\n",
			ret);
		return ret;
	}

	ave->client_open = false;
	dev_info(ave->dev,
		 "close: client %u returned (Stop -> UNINIT_DONE, Close -> STOP_DONE); the firmware has let go of its buffers\n",
		 AVE_SESS_CLIENT_ID);
	return 0;
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
/*
 * Default ON since F19b. Without a Halt the core is still running at unload,
 * so the teardown can never be proven clean, so nothing is unmapped and the
 * power reference is kept - and a load that keeps power also cannot release
 * the venc_me1 holder device, which makes the NEXT load fail with -EEXIST.
 * An opt-in safety measure that guarantees the module can only be loaded
 * once per boot is not a safety measure. fw_halt=0 still asks for the old
 * behaviour, deliberately.
 */
static bool fw_halt = true;
module_param(fw_halt, bool, 0444);
MODULE_PARM_DESC(fw_halt,
		 "at unload, ask the firmware to halt (command 14) so the next load can start it again without a reboot (default on)");

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
	/*
	 * Halt calls through the controller at CmdProcessor+0x7A10 - created
	 * behind bCreateMcpu, so presumably the McpuController - with no null
	 * check (fw 0x10ca8:
	 * ldr x0,[x19,#31248] at 0x10d44, then a vtable call), and only
	 * ProcessConfig creates it (fw 0xe84c, gated on bCreateMcpu). Sending
	 * Halt before Config makes the firmware take a NULL data abort instead
	 * of halting - measured 2026-09-14 (results/h1-1789369083.kmsg: esr
	 * 0x96000007, far 0, pc 0x10d48, caller 0xde40). macOS never meets
	 * this because StartUp always sends Config first.
	 */
	if (!ave->mcpu_created) {
		dev_warn(dev, "halt: Config has not run, and Halt calls a controller only Config creates, with no null check (fw 0x10d44); not sending - load with session_selftest=1\n");
		return -ENOTCONN;
	}
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
