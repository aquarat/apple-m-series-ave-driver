// SPDX-License-Identifier: GPL-2.0-only
/*
 * Parse the live ADT for one encoder's firmware placement (docs/99).
 *
 * The ADT is a tree of nodes. A node is {u32 nprops, u32 nchildren}, then
 * nprops properties {char name[32], u32 size (bit 31 is a flag), value
 * padded to 4 bytes}, then nchildren nodes. iBoot adds "pre-loaded" and
 * "segment-ranges" to an encoder's node when it loads that encoder's
 * firmware; macOS's kext reads the same two properties (docs/09 §1.1-1.2).
 *
 * Every read is bounds-checked against len; the walk is depth-limited.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "ave_adt.h"

#define ADT_NAME_LEN	32
#define ADT_PROP_HDR	(ADT_NAME_LEN + 4)
#define ADT_MAX_DEPTH	16
#define ADT_MAX_COUNT	4096

/*
 * Walk the property list of the node at @off. With @name, stop at that
 * property (0, with *val and *vlen set) or return -ENOENT at the end of the list.
 * Without, return 0 with *end = the first child's offset.
 */
static int adt_props(const u8 *b, size_t len, size_t off, const char *name,
		     const u8 **val, u32 *vlen, u32 *nkids, size_t *end)
{
	u32 np, nk, i;

	if (off > len || len - off < 8)
		return -EINVAL;
	np = get_unaligned_le32(b + off);
	nk = get_unaligned_le32(b + off + 4);
	if (np > ADT_MAX_COUNT || nk > ADT_MAX_COUNT)
		return -EINVAL;
	off += 8;
	for (i = 0; i < np; i++) {
		size_t padded;
		u32 sz;

		if (len - off < ADT_PROP_HDR)
			return -EINVAL;
		sz = get_unaligned_le32(b + off + ADT_NAME_LEN) & 0x7fffffff;
		padded = ((size_t)sz + 3) & ~(size_t)3;
		if (padded > len - off - ADT_PROP_HDR)
			return -EINVAL;
		if (name && !strncmp((const char *)b + off, name, ADT_NAME_LEN)) {
			*val = b + off + ADT_PROP_HDR;
			*vlen = sz;
			return 0;
		}
		off += ADT_PROP_HDR + padded;
	}
	if (name)
		return -ENOENT;
	if (nkids)
		*nkids = nk;
	if (end)
		*end = off;
	return 0;
}

/* The offset just past the whole subtree at @off */
static int adt_node_end(const u8 *b, size_t len, size_t off, int depth,
			size_t *end)
{
	u32 nk, i;
	int ret;

	if (depth > ADT_MAX_DEPTH)
		return -EINVAL;
	ret = adt_props(b, len, off, NULL, NULL, NULL, &nk, &off);
	if (ret)
		return ret;
	for (i = 0; i < nk; i++) {
		ret = adt_node_end(b, len, off, depth + 1, &off);
		if (ret)
			return ret;
	}
	*end = off;
	return 0;
}

static int adt_prop(const u8 *b, size_t len, size_t off, const char *name,
		    const u8 **val, u32 *vlen)
{
	return adt_props(b, len, off, name, val, vlen, NULL, NULL);
}

/* The node's "name" into @out (NUL-terminated, truncated to 31 chars) */
static void adt_name(const u8 *b, size_t len, size_t off, char *out)
{
	const u8 *v;
	u32 n;

	out[0] = 0;
	if (adt_prop(b, len, off, "name", &v, &n))
		return;
	if (n > ADT_NAME_LEN - 1)
		n = ADT_NAME_LEN - 1;
	memcpy(out, v, n);
	out[n] = 0;
}

/* The child of the node at @off named @name: 0 and *child, or -ENOENT */
static int adt_child(const u8 *b, size_t len, size_t off, int depth,
		     const char *name, size_t *child)
{
	char cn[ADT_NAME_LEN];
	u32 nk, i;
	int ret;

	ret = adt_props(b, len, off, NULL, NULL, NULL, &nk, &off);
	if (ret)
		return ret;
	for (i = 0; i < nk; i++) {
		adt_name(b, len, off, cn);
		if (!strcmp(cn, name)) {
			*child = off;
			return 0;
		}
		ret = adt_node_end(b, len, off, depth + 1, &off);
		if (ret)
			return ret;
	}
	return -ENOENT;
}

static u32 adt_u32(const u8 *b, size_t len, size_t off, const char *name,
		   u32 dflt)
{
	const u8 *v;
	u32 n;

	if (adt_prop(b, len, off, name, &v, &n) || n < 4)
		return dflt;
	return get_unaligned_le32(v);
}

/* "ave" or "ave" followed by digits only */
static bool adt_is_encoder_name(const char *n)
{
	if (strncmp(n, "ave", 3))
		return false;
	for (n += 3; *n; n++)
		if (*n < '0' || *n > '9')
			return false;
	return true;
}

int ave_adt_find_encoder(const u8 *b, size_t len, u64 cpu_reg0,
			 struct ave_adt_enc *out)
{
	const u8 *ranges, *v;
	u32 nranges, rlen, n, nk, i, j;
	size_t armio, off;
	int ret;

	if (!b || len < 8)
		return -EINVAL;
	ret = adt_child(b, len, 0, 0, "arm-io", &armio);
	if (ret)
		return ret;
	/* 64-bit addresses and sizes on both sides of /arm-io's ranges */
	if (adt_u32(b, len, 0, "#address-cells", 0) != 2 ||
	    adt_u32(b, len, armio, "#address-cells", 0) != 2 ||
	    adt_u32(b, len, armio, "#size-cells", 0) != 2)
		return -EINVAL;
	ret = adt_prop(b, len, armio, "ranges", &ranges, &rlen);
	if (ret)
		return ret == -ENOENT ? -EINVAL : ret;
	if (rlen % 24)
		return -EINVAL;
	nranges = rlen / 24;

	ret = adt_props(b, len, armio, NULL, NULL, NULL, &nk, &off);
	if (ret)
		return ret;
	for (i = 0; i < nk; i++) {
		size_t node = off;
		u64 bus, cpu = 0;
		bool mapped = false;

		ret = adt_node_end(b, len, node, 2, &off);
		if (ret)
			return ret;
		memset(out, 0, sizeof(*out));
		adt_name(b, len, node, out->name);
		if (!adt_is_encoder_name(out->name))
			continue;
		if (adt_prop(b, len, node, "reg", &v, &n) || n < 16)
			continue;
		bus = get_unaligned_le64(v);
		for (j = 0; j < nranges; j++) {
			u64 c = get_unaligned_le64(ranges + 24 * j);
			u64 p = get_unaligned_le64(ranges + 24 * j + 8);
			u64 s = get_unaligned_le64(ranges + 24 * j + 16);

			if (bus >= c && bus - c < s) {
				cpu = p + (bus - c);
				mapped = true;
				break;
			}
		}
		if (!mapped || cpu != cpu_reg0)
			continue;

		out->reg0 = cpu;
		out->preloaded = !adt_prop(b, len, node, "pre-loaded", &v, &n);
		ret = adt_prop(b, len, node, "segment-ranges", &v, &n);
		if (ret == -ENOENT)
			return 0;
		if (ret)
			return ret;
		if (n % 32 || n / 32 > AVE_ADT_MAX_SEGS)
			return -EINVAL;
		out->nseg = n / 32;
		for (j = 0; j < out->nseg; j++) {
			const u8 *e = v + 32 * j;

			out->seg[j].phys = get_unaligned_le64(e);
			out->seg[j].iova = get_unaligned_le64(e + 8);
			out->seg[j].remap = get_unaligned_le64(e + 16);
			out->seg[j].size = get_unaligned_le32(e + 24);
			out->seg[j].flags = get_unaligned_le32(e + 28);
		}
		return 0;
	}
	memset(out, 0, sizeof(*out));
	return -ENOENT;
}
