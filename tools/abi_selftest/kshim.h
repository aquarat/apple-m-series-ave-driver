/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal kernel-API shim so driver/ave_cmd.c compiles as userspace C.
 * Provides only what ave_cmd.c / ave_abi.h use.
 */
#ifndef __AVE_KSHIM_H__
#define __AVE_KSHIM_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint8_t __u8;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;

/* Linux values; not taken from <errno.h>, which would pull the shimmed
 * <linux/errno.h> back in. */
#define EIO		5
#define EINVAL		22
#define EPROTO		71

#define __packed	__attribute__((__packed__))
#define BIT(n)		(1ULL << (n))

static inline void put_unaligned_le16(u16 v, void *p)
{
	u8 *b = p;

	b[0] = v; b[1] = v >> 8;
}

static inline void put_unaligned_le32(u32 v, void *p)
{
	put_unaligned_le16(v, p);
	put_unaligned_le16(v >> 16, (u8 *)p + 2);
}

static inline void put_unaligned_le64(u64 v, void *p)
{
	put_unaligned_le32(v, p);
	put_unaligned_le32(v >> 32, (u8 *)p + 4);
}

static inline u16 get_unaligned_le16(const void *p)
{
	const u8 *b = p;

	return b[0] | (u16)b[1] << 8;
}

static inline u32 get_unaligned_le32(const void *p)
{
	return get_unaligned_le16(p) | (u32)get_unaligned_le16((const u8 *)p + 2) << 16;
}

static inline u64 get_unaligned_le64(const void *p)
{
	return get_unaligned_le32(p) | (u64)get_unaligned_le32((const u8 *)p + 4) << 32;
}

/* The kernel's own definition; ave_cmd.c walks fixed-size layout arrays. */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	((u32)(sizeof(a) / sizeof((a)[0])))
#endif

#endif
