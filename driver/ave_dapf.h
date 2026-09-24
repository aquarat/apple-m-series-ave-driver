/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - CPUDART / DAPF inspection and programming (docs/44 E2, E3).
 *
 * UNTESTED. Compiled only; nothing here has run on hardware.
 *
 * Every entry point is self-gated by its own module parameter and is a no-op
 * (returns 0) when that parameter is at its default, so wiring a call into
 * probe does not by itself change what a default load does.
 */
#ifndef __AVE_DAPF_H__
#define __AVE_DAPF_H__

#include <linux/types.h>

struct ave_device;

/*
 * One t8020/t6000 DAPF entry, in the units m1n1's dapf_init_t8020() writes
 * (m1n1 src/dapf.c:35-41):
 *
 *   base + 0x40*i + 0x04 = r4      (32-bit)
 *   base + 0x40*i + 0x08 = start   (64-bit)
 *   base + 0x40*i + 0x10 = end     (64-bit)
 *   base + 0x40*i + 0x00 = r0      (32-bit, (r0_hi << 4) | r0_lo, written last)
 *
 * @end is INCLUSIVE - the address of the last admitted byte. See the
 * justification at AVE_DAPF_END_INCLUSIVE in ave_dapf.c.
 */
struct ave_dapf_entry {
	u64		start;
	u64		end;		/* inclusive */
	u32		r0;
	u32		r4;
	const char	*what;
};

#define AVE_DAPF_MAX_ENTRIES	16

/*
 * The dart-ave0 addresses and iBoot's firmware placement are per SoC and
 * machine: ave->soc (ave_soc.c, docs/79).
 */
#define AVE_DAPF_OFFSET		0x4000		/* DAPF = CPUDART + 0x4000 (ave0, isp0) */

/*
 * E2: map "cpudart" and "dapf" (devm_ioremap, never request) and dump DART
 * config plus all 16 DAPF entries. Gated by module parameter dapf_dump.
 * Requires ave->powered (the stage 6 runtime-PM reference).
 */
int ave_dapf_dump(struct ave_device *ave);

/*
 * The same dump, ungated, tagged with @tag. For callers with their own gate -
 * ave_core_reset() uses it to check whether m1n1's entries survived a block
 * reset, which Linux could not put back (docs/49).
 */
int ave_dapf_dump_now(struct ave_device *ave, const char *tag,
		      u64 *fingerprint, bool *admits_fetch);

/*
 * E3: write @n entries exactly as m1n1 does, then read every one back.
 * Ungated primitive - callers are responsible for the gate. Returns -EIO on
 * readback mismatch.
 */
int ave_dapf_program(struct ave_device *ave,
		     const struct ave_dapf_entry *ent, unsigned int n,
		     bool preclear);

/*
 * E3: build the entry set chosen by dapf_set / dapf_mmio and program it.
 * No-op when dapf_set is "off" (the default). Refuses (non-zero) on any
 * failed precondition; the caller should fail probe rather than start the
 * core.
 */
int ave_dapf_write_probe(struct ave_device *ave);
bool ave_dapf_program_requested(void);
int ave_dapf_early(void);
int ave_dapf_program_selected(struct ave_device *ave);

/*
 * 0 if the datapath DART's SID-0 TCR/TTBR match the CPUDART's, -EIO if not,
 * -ENODEV if no second DART is attached. Reads only. See ave_dapf.c.
 */
int ave_dart_datapath_check(struct ave_device *ave, const char *tag);

/*
 * After a block reset, copy the CPUDART's SID 0/1 TTBRs and TCRs into the
 * datapath DART, which the pulse clears (F4 vs F5). Reads back every value.
 * 0 on success or when nothing needed writing; -ENODEV without a second DART.
 */
int ave_dart_restore_datapath(struct ave_device *ave);

#endif /* __AVE_DAPF_H__ */
