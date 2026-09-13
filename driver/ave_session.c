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
 * Nothing here is on the encode path and none of it runs unless
 * session_selftest=1 is passed, so a wrong guess degrades to a logged
 * firmware rejection, not a wedged encoder.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>

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

#define AVE_SESS_MAX_DMA	8
#define AVE_SESS_MAX_IPC	4

struct ave_sess_bufs {
	struct ave_device *ave;
	struct { void *cpu; dma_addr_t iova; size_t size; } dma[AVE_SESS_MAX_DMA];
	unsigned int ndma;
	struct { void *cpu; size_t size; } ipc[AVE_SESS_MAX_IPC];
	unsigned int nipc;
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

static void ave_sess_free_all(struct ave_sess_bufs *b)
{
	unsigned int i;

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
			"session: %s: TIMEOUT after %d ms - no completion on IO_T2H (IO ack %s)\n",
			name, AVE_SESS_TIMEOUT_MS,
			rx->ack_seen ? "did arrive: the firmware took the command"
				     : "did not arrive either");
		return -ETIMEDOUT;
	}
	if (!rx->ack_seen)
		dev_warn(ave->dev,
			 "session: %s: completion arrived without the IO ack echo\n",
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

	if (!session_reg_dart)
		dev_warn(ave->dev,
			 "session: Config reg-DART addr is 0 (unknown); pass session_reg_dart= if the firmware faults in ProcessInitStage2\n");

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

static int ave_session_start_avc(struct ave_device *ave,
				 const struct ave_cmd_abi *abi,
				 struct ave_sess_bufs *bufs, u64 client_id)
{
	struct ave_cmd_ctx ctx = { .count = 3, .client_id = client_id };
	struct ave_avc_session s = {};
	struct ave_recon_buf recon;
	struct ave_buf coded, coded_hdr;
	dma_addr_t cmd_iova, fwc_iova, fwcm_iova, recon_iova, coded_iova, hdr_iova;
	u32 cw, ch, recon_size, fwc_size;
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
	recon_size = cw * ch * 2;		/* luma + chroma, generous */

	fwc_size = ave->client_buf_size ? ave->client_buf_size
					: AVE_SESS_FWCLIENT_FALLBACK;

	if (!ave_sess_dma_alloc(bufs, fwc_size, &fwc_iova) ||
	    !ave_sess_dma_alloc(bufs, AVE_SESS_FWCLIENTMEM_SIZE, &fwcm_iova) ||
	    !ave_sess_dma_alloc(bufs, recon_size, &recon_iova) ||
	    !ave_sess_dma_alloc(bufs, AVE_SESS_CODED_SIZE, &coded_iova) ||
	    !ave_sess_dma_alloc(bufs, abi->start_avc.coded_hdr_bytes, &hdr_iova))
		return -ENOMEM;

	recon.addr = recon_iova;
	recon.luma_size = cw * ch;		/* used only where recon_size != NONE */
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

	s.recon = &recon;
	s.n_recon = 1;
	s.coded = &coded;
	s.coded_hdr = &coded_hdr;
	s.n_coded = 1;

	ret = ave_cmd_build_start_avc(abi, cmd, cmd_len, &ctx, &s);
	if (ret < 0) {
		dev_err(ave->dev, "session: Start_AVC build failed: %d\n", ret);
		return ret;
	}
	dev_info(ave->dev,
		 "session: Start_AVC: %ux%u (coded %ux%u) QP %u I-only, profile 66 level 40\n",
		 session_width, session_height, cw, ch, session_qp);
	dev_info(ave->dev,
		 "session: Start_AVC: fw_client %pad/%#x mem %pad/%#x recon %pad/%#x coded %pad/%#x hdr %pad/%#x\n",
		 &fwc_iova, fwc_size, &fwcm_iova, (u32)AVE_SESS_FWCLIENTMEM_SIZE,
		 &recon_iova, recon_size, &coded_iova, (u32)AVE_SESS_CODED_SIZE,
		 &hdr_iova, abi->start_avc.coded_hdr_bytes);

	return ave_session_cmd(ave, abi, AVE_OP_START_AVC, "Start_AVC",
			       cmd_iova, cmd_len, client_id);
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

	if (!session_selftest)
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

	ret = ave_session_start_avc(ave, abi, bufs, AVE_SESS_CLIENT_ID);

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
		 ret ? "FAILED" : "reached Start_AVC OK", ret);
	return ret;
}
