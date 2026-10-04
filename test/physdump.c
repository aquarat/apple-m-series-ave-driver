// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copy the DRAM window at the AVE reset vector out to a debugfs file, so it
 * can be analysed offline instead of by adding printk to the driver.
 *
 * Deliberately touches no hardware: no AVE node, no power domain, no
 * register. The window is the 16 MiB from 0x10000b28000 that the driver's
 * liveness snapshot has already memremap()ed and read on several runs without
 * incident. It is below Linux's memory map (see docs/42 §3.1), so nothing in
 * Linux owns it and the copy races nothing.
 *
 * The address is a constant rather than read from RVBAR precisely so that
 * reading RVBAR - which needs VENC powered - is not required. Change it only
 * with a reason: DRAM outside this window has not been shown safe to read.
 *
 *   sudo insmod test/physdump.ko
 *   sudo cp /sys/kernel/debug/ave_physdump/window.bin data/blobs/
 *   sudo rmmod physdump
 */
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#define PD_BASE	0x10000b28000ULL
#define PD_SIZE	SZ_16M

/*
 * The whole of DATA ends at 0x10001a90000 + 0x134000 = window + 0x109c000,
 * 0x9c000 past the 16 MiB window - so the default copy has only ever held
 * DATA's first 0x98000 bytes (docs/51 7), missing the bss and, very likely,
 * the stack a crashing firmware formats its report on. That tail is no longer
 * unexplored memory: the driver's DATA restore has memremap()ed, read and
 * written all of it on hardware (results/h2-1789369852.kmsg, r2). full=1
 * extends the copy to exactly the end of DATA and not a byte further.
 */
#define PD_SIZE_FULL	0x109c000
static bool full;
module_param(full, bool, 0444);
MODULE_PARM_DESC(full, "copy through the end of the firmware DATA segment (window + 0x109c000) instead of 16 MiB");

/*
 * base= / size=: another window instead, for a port (docs/79). Only ranges
 * that have been shown to be the AVE firmware's own memory are accepted:
 * what iBoot's segment-ranges name for that machine.
 */
static unsigned long base;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "physical base of the window (default: the t6001 window)");
static unsigned long size;
module_param(size, ulong, 0444);
MODULE_PARM_DESC(size, "bytes to copy when base= is given");

static const struct { u64 base, size; const char *what; } pd_ok[] = {
	{ 0x8009f4000ULL, 0xcc000, "t8103 j313 13.5 H13G TEXT" },
	{ 0x8019b0000ULL, 0x128000, "t8103 j313 13.5 H13G DATA" },
	/*
	 * t8112 j473 (docs/90): the carve-outs below Linux RAM (0x801120000)
	 * that no reserved-memory node claims. The first is where RVBAR points
	 * (0x800000000); the AVE TEXT and DATA are looked for in these.
	 */
	{ 0x800000000ULL, 0x214000, "t8112 j473 carve-out 0x800000000 (RVBAR)" },
	{ 0x800818000ULL, 0x1ec000, "t8112 j473 carve-out 0x800818000" },
	{ 0x800a20000ULL, 0x608000, "t8112 j473 carve-out 0x800a20000" },
	{ 0x8008e0000ULL, 0xd0000, "t8112 j473 13.5 H14G TEXT" },
	{ 0x800e88000ULL, 0x128000, "t8112 j473 13.5 H14G DATA" },
};

/*
 * adt=1 / seg=N (docs/98): the live Apple device tree, and the firmware
 * segments iBoot names in it.
 *
 * m1n1 (v1.6.1) publishes the ADT that iBoot handed it as a reserved-memory
 * node, compatible "phram", label "adt" (m1n1 src/kboot.c,
 * dt_setup_mtd_phram). That is ordinary DRAM Linux has reserved and never
 * writes, and Fedora's kernel has no phram driver to expose it. Unlike the
 * IPSW's restore ADT, the live one carries what iBoot injected at boot: the
 * AVE nodes' "segment-ranges" and "pre-loaded", the properties macOS's
 * AVE_Firmware::RetrieveInfo adopts the preloaded firmware from (docs/42
 * §3.1, docs/44 §1.2). adt=1 copies it to ave_physdump/adt.bin.
 *
 * seg=N parses that copy, finds /arm-io/aveN, and copies each of its
 * segment-ranges entries ({u64 phys, u64 iova, u64 remap, u32 size, u32
 * flags}) to aveN-text.bin (iova 0), aveN-data.bin (iova 0xec000) or
 * aveN-segK.bin. The ranges are taken only as iBoot names them for an AVE
 * node, at most 16 MiB each, and only if they intersect no System RAM:
 * firmware carve-outs below Linux's memory, the kind of memory the default
 * window and docs/87 §3's dump read. Nothing here touches a register, a
 * power domain, or any AVE node.
 */
static bool adt;
module_param(adt, bool, 0444);
MODULE_PARM_DESC(adt, "copy the live ADT (m1n1's phram reserved-memory node \"adt\") to ave_physdump/adt.bin");
static int seg = -1;
module_param(seg, int, 0444);
MODULE_PARM_DESC(seg, "copy the iBoot segment-ranges of /arm-io/ave<seg> (from the live ADT) to ave_physdump/ave<seg>-{text,data,segK}.bin");

#define PD_MAX_BLOBS	8
#define PD_SEG_MAX	SZ_16M
#define PD_ADT_MAX	SZ_4M

static struct dentry *pd_dir;
static struct debugfs_blob_wrapper pd_blob;
static struct debugfs_blob_wrapper pd_blobs[PD_MAX_BLOBS];
static unsigned int pd_nblobs;

struct pd_adt {
	const u8	*b;
	size_t		n;
};

/*
 * One node's properties: returns the offset of its first child, or -1. A
 * property is {char name[32], u32 size (bit 31: template flag), value},
 * value padded to 4; a node is {u32 nprops, u32 nchildren, props, children}.
 */
static long pd_adt_props(const struct pd_adt *a, size_t off, u32 *nchild,
			 const char *want, const u8 **val, u32 *vlen)
{
	u32 np, i, sz;

	if (off + 8 > a->n)
		return -1;
	np = get_unaligned_le32(a->b + off);
	*nchild = get_unaligned_le32(a->b + off + 4);
	if (np > 4096 || *nchild > 4096)
		return -1;
	off += 8;
	for (i = 0; i < np; i++) {
		if (off + 36 > a->n)
			return -1;
		sz = get_unaligned_le32(a->b + off + 32) & 0x7fffffff;
		if (sz > a->n - off - 36)
			return -1;
		if (want && !strncmp((const char *)a->b + off, want, 32)) {
			*val = a->b + off + 36;
			*vlen = sz;
		}
		off += 36 + ALIGN(sz, 4);
	}
	return off;
}

static long pd_adt_skip(const struct pd_adt *a, size_t off, int depth)
{
	u32 nc, i;
	long p;

	if (depth > 16)
		return -1;
	p = pd_adt_props(a, off, &nc, NULL, NULL, NULL);
	for (i = 0; p >= 0 && i < nc; i++)
		p = pd_adt_skip(a, p, depth + 1);
	return p;
}

static long pd_adt_child(const struct pd_adt *a, size_t off, const char *name)
{
	u32 nc, dummy, i, l;
	const u8 *v;
	long p;

	p = pd_adt_props(a, off, &nc, NULL, NULL, NULL);
	for (i = 0; p >= 0 && i < nc; i++) {
		v = NULL;
		l = 0;
		if (pd_adt_props(a, p, &dummy, "name", &v, &l) < 0)
			return -1;
		if (v && strnlen((const char *)v, l) == strlen(name) &&
		    !memcmp(v, name, strlen(name)))
			return p;
		p = pd_adt_skip(a, p, 1);
	}
	return -1;
}

static int pd_find_adt(u64 *pbase, u64 *psize)
{
	struct device_node *rm, *np;
	struct resource r;
	const char *label;
	int ret = -ENODEV;

	rm = of_find_node_by_path("/reserved-memory");
	if (!rm)
		return -ENODEV;
	for_each_child_of_node(rm, np) {
		if (!of_device_is_compatible(np, "phram") ||
		    of_property_read_string(np, "label", &label) ||
		    strcmp(label, "adt") || of_address_to_resource(np, 0, &r))
			continue;
		*pbase = r.start;
		*psize = resource_size(&r);
		ret = 0;
		of_node_put(np);
		break;
	}
	of_node_put(rm);
	return ret;
}

/* Copy [from, from+n) into a new debugfs blob called @name */
static int pd_copy(u64 from, size_t n, const char *name)
{
	struct debugfs_blob_wrapper *w;
	void *src;

	if (pd_nblobs >= PD_MAX_BLOBS)
		return -ENOSPC;
	w = &pd_blobs[pd_nblobs];
	w->data = vmalloc(n);
	if (!w->data)
		return -ENOMEM;
	src = memremap(from, n, MEMREMAP_WB);
	if (!src) {
		pr_err("physdump: cannot map %#llx+%#zx\n", from, n);
		vfree(w->data);
		w->data = NULL;
		return -ENOMEM;
	}
	memcpy(w->data, src, n);
	memunmap(src);
	w->size = n;
	debugfs_create_blob(name, 0400, pd_dir, w);
	pd_nblobs++;
	pr_info("physdump: %s = %#zx bytes from %#llx\n", name, n, from);
	return 0;
}

static int pd_do_adt(void)
{
	char name[32], path[16];
	u64 abase, asize;
	struct pd_adt a;
	const u8 *v = NULL;
	u32 l = 0, nc, i, nseg;
	long node;
	int ret;

	ret = pd_find_adt(&abase, &asize);
	if (ret) {
		pr_err("physdump: no reserved-memory phram node labelled \"adt\" (m1n1 too old?)\n");
		return ret;
	}
	if (!asize || asize > PD_ADT_MAX) {
		pr_err("physdump: ADT node size %#llx is implausible; refusing\n", asize);
		return -EINVAL;
	}
	if (adt) {
		ret = pd_copy(abase, asize, "adt.bin");
		if (ret)
			return ret;
	}
	if (seg < 0)
		return 0;
	if (seg > 9) {
		pr_err("physdump: seg=%d: no such encoder\n", seg);
		return -EINVAL;
	}

	/* parse a private copy, so nothing below reads the reserved region twice */
	a.n = asize;
	a.b = vmalloc(asize);
	if (!a.b)
		return -ENOMEM;
	{
		void *src = memremap(abase, asize, MEMREMAP_WB);

		if (!src) {
			vfree(a.b);
			return -ENOMEM;
		}
		memcpy((void *)a.b, src, asize);
		memunmap(src);
	}

	snprintf(path, sizeof(path), "ave%d", seg);
	node = pd_adt_child(&a, 0, "arm-io");
	if (node >= 0)
		node = pd_adt_child(&a, node, path);
	if (node < 0) {
		pr_err("physdump: no /arm-io/%s in the live ADT\n", path);
		ret = -ENOENT;
		goto out;
	}
	if (pd_adt_props(&a, node, &nc, "pre-loaded", &v, &l) >= 0 && v)
		pr_info("physdump: /arm-io/%s pre-loaded = %*phN\n", path, (int)min(l, 8u), v);
	else
		pr_info("physdump: /arm-io/%s has no pre-loaded property\n", path);
	v = NULL;
	l = 0;
	if (pd_adt_props(&a, node, &nc, "segment-ranges", &v, &l) < 0 || !v || !l) {
		pr_info("physdump: /arm-io/%s has no segment-ranges: iBoot loaded nothing for it\n", path);
		ret = 0;
		goto out;
	}
	if (l % 32) {
		pr_err("physdump: /arm-io/%s segment-ranges is %u bytes, not 32-byte entries\n", path, l);
		ret = -EINVAL;
		goto out;
	}
	nseg = l / 32;
	for (i = 0; i < nseg && i < PD_MAX_BLOBS - 1; i++) {
		const u8 *e = v + i * 32;
		u64 phys = get_unaligned_le64(e);
		u64 iova = get_unaligned_le64(e + 8);
		u64 remap = get_unaligned_le64(e + 16);
		u32 sz = get_unaligned_le32(e + 24);
		u32 fl = get_unaligned_le32(e + 28);

		pr_info("physdump: /arm-io/%s segment %u: phys %#llx iova %#llx remap %#llx size %#x flags %#x\n",
			path, i, phys, iova, remap, sz, fl);
		if (!sz || sz > PD_SEG_MAX) {
			pr_err("physdump:   size %#x out of range; not copied\n", sz);
			continue;
		}
		if (region_intersects(phys, sz, IORESOURCE_SYSTEM_RAM,
				      IORES_DESC_NONE) != REGION_DISJOINT) {
			pr_err("physdump:   intersects System RAM; not copied\n");
			continue;
		}
		if (iova == 0)
			snprintf(name, sizeof(name), "%s-text.bin", path);
		else if (iova == 0xec000)
			snprintf(name, sizeof(name), "%s-data.bin", path);
		else
			snprintf(name, sizeof(name), "%s-seg%u.bin", path, i);
		ret = pd_copy(phys, sz, name);
		if (ret)
			goto out;
	}
	ret = 0;
out:
	vfree(a.b);
	return ret;
}

static int __init pd_init(void)
{
	u64 from = PD_BASE;
	size_t n = full ? PD_SIZE_FULL : PD_SIZE;
	void *src;
	int ret;

	if (adt || seg >= 0) {
		if (base || full) {
			pr_err("physdump: adt=/seg= cannot be combined with base= or full=\n");
			return -EINVAL;
		}
		pd_dir = debugfs_create_dir("ave_physdump", NULL);
		ret = pd_do_adt();
		if (ret) {
			unsigned int i;

			debugfs_remove(pd_dir);
			for (i = 0; i < pd_nblobs; i++)
				vfree(pd_blobs[i].data);
		}
		return ret;
	}

	if (base) {
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(pd_ok); i++)
			if (base == pd_ok[i].base && size && size <= pd_ok[i].size)
				break;
		if (i == ARRAY_SIZE(pd_ok)) {
			pr_err("physdump: %#lx+%#lx is not a known firmware range; refusing\n",
			       base, size);
			return -EINVAL;
		}
		from = base;
		n = size;
		pr_info("physdump: %s\n", pd_ok[i].what);
	}

	pd_blob.data = vmalloc(n);
	if (!pd_blob.data)
		return -ENOMEM;

	src = memremap(from, n, MEMREMAP_WB);
	if (!src) {
		pr_err("physdump: cannot map %#llx\n", from);
		vfree(pd_blob.data);
		return -ENOMEM;
	}
	memcpy(pd_blob.data, src, n);
	memunmap(src);
	pd_blob.size = n;

	pd_dir = debugfs_create_dir("ave_physdump", NULL);
	debugfs_create_blob("window.bin", 0400, pd_dir, &pd_blob);
	pr_info("physdump: copied %#zx bytes from %#llx\n", n, from);
	return 0;
}

static void __exit pd_exit(void)
{
	unsigned int i;

	debugfs_remove(pd_dir);
	vfree(pd_blob.data);
	for (i = 0; i < pd_nblobs; i++)
		vfree(pd_blobs[i].data);
}

module_init(pd_init);
module_exit(pd_exit);
MODULE_DESCRIPTION("Read-only copy of the DRAM window at the AVE reset vector");
MODULE_LICENSE("GPL");
