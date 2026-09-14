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

static struct dentry *pd_dir;
static struct debugfs_blob_wrapper pd_blob;

static int __init pd_init(void)
{
	size_t size = full ? PD_SIZE_FULL : PD_SIZE;
	void *src;

	pd_blob.data = vmalloc(size);
	if (!pd_blob.data)
		return -ENOMEM;

	src = memremap(PD_BASE, size, MEMREMAP_WB);
	if (!src) {
		pr_err("physdump: cannot map %#llx\n", PD_BASE);
		vfree(pd_blob.data);
		return -ENOMEM;
	}
	memcpy(pd_blob.data, src, size);
	memunmap(src);
	pd_blob.size = size;

	pd_dir = debugfs_create_dir("ave_physdump", NULL);
	debugfs_create_blob("window.bin", 0400, pd_dir, &pd_blob);
	pr_info("physdump: copied %#zx bytes from %#llx\n", size, PD_BASE);
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
