// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE (Video Encoder) - firmware loading
 *
 * UNTESTED beyond compilation.
 *
 * Apple's kext does not load the firmware: iBoot places it and the kext adopts
 * it through the "pre-loaded" and "segment-ranges" ADT properties. m1n1 emits
 * no AVE node at all, so Linux never sees those, and whether iBoot pre-loads
 * AVE firmware on a non-macOS boot has never been established.
 *
 * So we load it ourselves. That avoids patching the bootloader - a much higher
 * risk class, since a bad m1n1 stops Linux booting - and it works regardless of
 * what iBoot does.
 *
 * The one hard constraint is placement: AppleAVE2FW is a MH_PRELOAD Mach-O
 * linked at vmaddr 0, and the kext asserts the image is mapped at DART address
 * zero (docs/09). dma_alloc_coherent cannot deliver that - iommu-dma allocates
 * top-down, and our own IPC allocation came back at 0xfe000000. So:
 *
 *   1. allocate the span physically contiguous (DMA_ATTR_FORCE_CONTIGUOUS),
 *   2. recover its physical address with iommu_iova_to_phys(),
 *   3. iommu_map() that physical range at IOVA 0 on the DEFAULT domain.
 *
 * Mapping on the default domain rather than a freshly allocated one matters:
 * attaching our own domain would drop the iommu-dma cookie and break
 * dma_alloc_coherent for this device.
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/iommu.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "ave.h"

#define AVE_FW_NAME		"apple/ave_h13c.bin"

/* Mach-O, enough of it to walk the load commands. */
#define MH_MAGIC_64		0xfeedfacf
#define MH_PRELOAD		5
#define LC_SEGMENT_64		0x19

struct macho_hdr {
	__le32 magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
} __packed;

struct macho_seg {
	__le32 cmd, cmdsize;
	char   segname[16];
	__le64 vmaddr, vmsize, fileoff, filesize;
	__le32 maxprot, initprot, nsects, flags;
} __packed;

/*
 * Walk the load commands twice: once to size the image, once to copy it.
 * @dst is NULL on the sizing pass.
 */
static int ave_fw_walk(struct ave_device *ave, const struct firmware *fw,
		       void *dst, size_t *span)
{
	const struct macho_hdr *h = (const void *)fw->data;
	size_t off, end = 0;
	unsigned int i, ncmds;

	if (fw->size < sizeof(*h) || le32_to_cpu(h->magic) != MH_MAGIC_64) {
		dev_err(ave->dev, "firmware is not a 64-bit Mach-O\n");
		return -EINVAL;
	}
	if (le32_to_cpu(h->filetype) != MH_PRELOAD)
		dev_warn(ave->dev, "unexpected Mach-O filetype %u (want %u)\n",
			 le32_to_cpu(h->filetype), MH_PRELOAD);

	ncmds = le32_to_cpu(h->ncmds);
	off = sizeof(*h);

	for (i = 0; i < ncmds; i++) {
		const struct macho_seg *s;
		u64 vmaddr, vmsize, fileoff, filesize;
		u32 cmd, cmdsize;

		if (off + 8 > fw->size)
			return -EINVAL;
		cmd = le32_to_cpup((__le32 *)(fw->data + off));
		cmdsize = le32_to_cpup((__le32 *)(fw->data + off + 4));
		if (!cmdsize || off + cmdsize > fw->size)
			return -EINVAL;

		if (cmd != LC_SEGMENT_64)
			goto next;
		if (cmdsize < sizeof(*s))
			return -EINVAL;

		s = (const void *)(fw->data + off);
		vmaddr   = le64_to_cpu(s->vmaddr);
		vmsize   = le64_to_cpu(s->vmsize);
		fileoff  = le64_to_cpu(s->fileoff);
		filesize = le64_to_cpu(s->filesize);

		if (!vmsize)
			goto next;
		if (fileoff + filesize > fw->size || filesize > vmsize)
			return -EINVAL;
		if (vmaddr + vmsize > SZ_64M)		/* sanity */
			return -EINVAL;

		if (vmaddr + vmsize > end)
			end = vmaddr + vmsize;

		if (dst) {
			dev_info(ave->dev, "  %-12.16s vm %#llx+%#llx file %#llx+%#llx\n",
				 s->segname, vmaddr, vmsize, fileoff, filesize);
			memcpy(dst + vmaddr, fw->data + fileoff, filesize);
			/* vmsize > filesize is BSS; the buffer is already zeroed. */
		}
next:
		off += cmdsize;
	}

	if (!end)
		return -EINVAL;
	*span = end;
	return 0;
}

/*
 * Load the firmware and map it at IOVA 0.
 *
 * The caller must have attached the IOMMU already - this uses the device's
 * default domain.
 */
int ave_fw_load(struct ave_device *ave)
{
	const struct firmware *fw;
	struct iommu_domain *domain;
	phys_addr_t pa;
	size_t span, size;
	int ret;

	ret = request_firmware(&fw, AVE_FW_NAME, ave->dev);
	if (ret) {
		dev_err(ave->dev,
			"no firmware at " AVE_FW_NAME " (%d). Extract it with "
			"tools/fetch_firmware.py, unwrap with pyimg4, and place "
			"the Mach-O at /lib/firmware/" AVE_FW_NAME "\n", ret);
		return ret;
	}
	dev_info(ave->dev, "firmware image: %zu bytes\n", fw->size);

	ret = ave_fw_walk(ave, fw, NULL, &span);
	if (ret) {
		dev_err(ave->dev, "failed to parse firmware Mach-O: %d\n", ret);
		goto out;
	}

	size = ALIGN(span, SZ_16K);
	dev_info(ave->dev, "  image spans %#zx, allocating %#zx contiguous\n",
		 span, size);

	ave->fw.cpu = dma_alloc_attrs(ave->dev, size, &ave->fw.iova,
				      GFP_KERNEL, DMA_ATTR_FORCE_CONTIGUOUS);
	if (!ave->fw.cpu) {
		ret = -ENOMEM;
		goto out;
	}
	ave->fw.size = size;

	ret = ave_fw_walk(ave, fw, ave->fw.cpu, &span);
	if (ret)
		goto err_free;

	domain = iommu_get_domain_for_dev(ave->dev);
	if (!domain) {
		dev_err(ave->dev, "no IOMMU domain - is the DART attached?\n");
		ret = -ENODEV;
		goto err_free;
	}

	pa = iommu_iova_to_phys(domain, ave->fw.iova);
	if (!pa) {
		dev_err(ave->dev, "cannot resolve iova %pad to a phys addr\n",
			&ave->fw.iova);
		ret = -EFAULT;
		goto err_free;
	}

	dev_info(ave->dev, "  iova %pad -> phys %pa, mapping at IOVA 0\n",
		 &ave->fw.iova, &pa);

	ret = iommu_map(domain, 0, pa, size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		dev_err(ave->dev, "iommu_map at IOVA 0 failed: %d\n", ret);
		goto err_free;
	}
	ave->fw.mapped_at_zero = true;
	dev_info(ave->dev, "firmware mapped at IOVA 0, %#zx bytes\n", size);

	release_firmware(fw);
	return 0;

err_free:
	dma_free_attrs(ave->dev, size, ave->fw.cpu, ave->fw.iova,
		       DMA_ATTR_FORCE_CONTIGUOUS);
	ave->fw.cpu = NULL;
out:
	release_firmware(fw);
	return ret;
}

void ave_fw_unload(struct ave_device *ave)
{
	struct iommu_domain *domain;

	if (!ave->fw.cpu)
		return;

	if (ave->fw.mapped_at_zero) {
		domain = iommu_get_domain_for_dev(ave->dev);
		if (domain)
			iommu_unmap(domain, 0, ave->fw.size);
		ave->fw.mapped_at_zero = false;
	}

	dma_free_attrs(ave->dev, ave->fw.size, ave->fw.cpu, ave->fw.iova,
		       DMA_ATTR_FORCE_CONTIGUOUS);
	ave->fw.cpu = NULL;
}
