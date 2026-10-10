/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal kernel-API shim so driver/ave_adt.c compiles as userspace C. */
#ifndef __AVE_ADT_KSHIM_H__
#define __AVE_ADT_KSHIM_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

/* Linux values */
#define ENOENT		2
#define EINVAL		22

static inline u32 get_unaligned_le32(const void *p)
{
	const u8 *b = p;

	return b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

static inline u64 get_unaligned_le64(const void *p)
{
	return get_unaligned_le32(p) | (u64)get_unaligned_le32((const u8 *)p + 4) << 32;
}

#endif
