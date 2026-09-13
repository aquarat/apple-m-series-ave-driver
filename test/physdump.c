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

static struct dentry *pd_dir;
static struct debugfs_blob_wrapper pd_blob;

static int __init pd_init(void)
{
	void *src;

	pd_blob.data = vmalloc(PD_SIZE);
	if (!pd_blob.data)
		return -ENOMEM;

	src = memremap(PD_BASE, PD_SIZE, MEMREMAP_WB);
	if (!src) {
		pr_err("physdump: cannot map %#llx\n", PD_BASE);
		vfree(pd_blob.data);
		return -ENOMEM;
	}
	memcpy(pd_blob.data, src, PD_SIZE);
	memunmap(src);
	pd_blob.size = PD_SIZE;

	pd_dir = debugfs_create_dir("ave_physdump", NULL);
	debugfs_create_blob("window.bin", 0400, pd_dir, &pd_blob);
	pr_info("physdump: copied %#x bytes from %#llx\n", PD_SIZE, PD_BASE);
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
