/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - first-command session self-test.
 *
 * UNTESTED ON HARDWARE. This sends the opening command sequence (Config ->
 * Open -> Start_AVC) for a fixed-QP, I-frame-only, 8-bit 4:2:0 AVC session
 * over the IO channel and checks each firmware reply. It is a diagnostic
 * probe, not the encode path: it and, with session_frame=1, encodes one I-frame (docs/53) and is gated
 * behind the module parameter ave_session.session_selftest (off by default).
 *
 * The handshake must already be complete (ave->boot_phase == READY, the IO
 * channel bound). Call it once, after ave_start() prints
 * "Apple AVE video encoder ready".
 */
#ifndef __AVE_SESSION_H__
#define __AVE_SESSION_H__

struct ave_device;

/*
 * Run the opening command sequence. Returns 0 if every step was accepted by
 * the firmware, a negative errno otherwise (or 0 immediately when the gate
 * parameter is off, so the caller can invoke it unconditionally). Never
 * touches hardware beyond the already-live IPC transport, and frees every
 * buffer it allocates on all paths.
 */
int ave_session_selftest(struct ave_device *ave);

/*
 * Free the buffers the self-test handed to the firmware. Only safe once the
 * core cannot reach them any more: call it from ave_remove() after the power
 * has been dropped.
 */
void ave_session_release(struct ave_device *ave);

/*
 * Ask the firmware to halt itself (command 14) so that the next load can
 * start it again without rebooting the machine. Sends on IO and waits for SVE
 * scratch 0, because this command never replies (docs/55). Returns 0 when the
 * firmware reached its wfi, a negative errno otherwise, or 0 immediately when
 * the gate parameter is off. Call it from ave_remove() while the IPC
 * transport is still live and BEFORE dropping power.
 */
int ave_session_halt(struct ave_device *ave);
bool ave_session_halt_requested(void);

#endif /* __AVE_SESSION_H__ */
