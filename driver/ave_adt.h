/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The live Apple Device Tree (ADT) that iBoot hands m1n1, and what it says
 * about one encoder's firmware placement (docs/99). Pure parsing: no kernel
 * API beyond types, so tools/adt_selftest builds it in userspace.
 */
#ifndef __AVE_ADT_H__
#define __AVE_ADT_H__

#include <linux/types.h>

#define AVE_ADT_MAX_SEGS	4

/* One "segment-ranges" entry (docs/09 §1.2, m1n1's adt_segment_ranges) */
struct ave_adt_seg {
	u64	phys;
	u64	iova;
	u64	remap;
	u32	size;
	u32	flags;
};

struct ave_adt_enc {
	char	name[32];	/* the node's "name", e.g. "ave0" */
	u64	reg0;		/* reg[0] as a CPU address, through /arm-io's ranges */
	bool	preloaded;	/* "pre-loaded" present and not 0 */
	unsigned int		nseg;
	struct ave_adt_seg	seg[AVE_ADT_MAX_SEGS];
};

/*
 * Find the /arm-io child named "ave" or "ave<digits>" whose first reg,
 * translated through /arm-io's ranges, is cpu_reg0 (the DPE bank, the
 * address the Linux node's first reg and the ave_soc row use).
 *
 * 0: found, *out filled. -ENOENT: no such node. -EINVAL: the buffer is
 * not a well-formed ADT (every offset is bounds-checked; nothing is read
 * outside [adt, adt + len)).
 */
int ave_adt_find_encoder(const u8 *adt, size_t len, u64 cpu_reg0,
			 struct ave_adt_enc *out);

#endif
