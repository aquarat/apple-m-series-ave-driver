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
#include <linux/module.h>
#include <linux/sizes.h>
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
};

static struct dentry *pd_dir;
static struct debugfs_blob_wrapper pd_blob;

static int __init pd_init(void)
{
	u64 from = PD_BASE;
	size_t n = full ? PD_SIZE_FULL : PD_SIZE;
	void *src;

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
	debugfs_remove(pd_dir);
	vfree(pd_blob.data);
}

module_init(pd_init);
module_exit(pd_exit);
MODULE_DESCRIPTION("Read-only copy of the DRAM window at the AVE reset vector");
MODULE_LICENSE("GPL");
