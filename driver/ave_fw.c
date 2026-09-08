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
#include <linux/unaligned.h>
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
 * Patch the image's IOBA / IOSZ tags.
 *
 * The firmware does not hard-code its register base. CPlatformEnvironment's
 * constructor copies a length-prefixed "IOBA" tag out of its own __DATA into
 * a global (fw 0xe18dc-0xe1910) and forms every MMIO address from it:
 * base + 0x1800000 is the ASC bank, base + 0x1050000 the SVE bank, and the
 * very first register write it performs is base + 0x1c00808 (fw 0xe1a78).
 *
 * In the image we ship, that tag is eight zero bytes - iBoot fills it in on
 * an Apple boot, and nothing on our path did. With a base of zero the
 * coprocessor starts, addresses nothing that exists, and goes quiet, which is
 * exactly the symptom we have been chasing.
 *
 * The value is a BUS address, not an AP-physical one: the coprocessor sits on
 * the far side of the /arm-io translation. Deriving it from a mapped
 * resource rather than hard-coding it keeps the two from being confused
 * again - that confusion cost eight experiments already (docs/30).
 */
static int ave_fw_patch_ioba(struct ave_device *ave, void *img, size_t size)
{
	static const u8 tag_ioba[4] = { 'A', 'B', 'O', 'I' };	/* "IOBA", reversed */
	static const u8 tag_iosz[4] = { 'Z', 'S', 'O', 'I' };	/* "IOSZ", reversed */
	phys_addr_t fabric = ave->bank[AVE_BANK_FABRIC].phys;
	u64 bus;
	size_t i;
	int patched = 0;

	if (!fabric) {
		dev_err(ave->dev, "no fabric bank resource; cannot derive I/O base\n");
		return -EINVAL;
	}
	if (fabric < AVE_ARM_IO_BUS_OFFSET) {
		dev_err(ave->dev, "fabric phys %pa is below the /arm-io offset\n",
			&fabric);
		return -EINVAL;
	}
	bus = (u64)fabric - AVE_ARM_IO_BUS_OFFSET;

	/*
	 * Byte-wise, not word-wise. The tag list is packed with no alignment:
	 * IOBA sits at image 0x1341f5, which is 4-byte aligned only by
	 * accident of nothing. A stride of 4 walks straight past it.
	 */
	for (i = 0; i + 16 <= size; i++) {
		u8 *p = (u8 *)img + i;
		u32 len = get_unaligned_le32(p + 4);

		if (!memcmp(p, tag_ioba, 4) && len == 8) {
			u64 old = get_unaligned_le64(p + 8);

			if (old && old != bus) {
				dev_warn(ave->dev,
					 "IOBA already set to %#llx, leaving it\n", old);
			} else {
				put_unaligned_le64(bus, p + 8);
				dev_info(ave->dev, "  IOBA at image +%#zx: %#llx -> %#llx\n",
					 i + 8, old, bus);
			}
			patched |= 1;
		} else if (!memcmp(p, tag_iosz, 4) && len == 4) {
			u32 old = get_unaligned_le32(p + 8);

			if (!old) {
				put_unaligned_le32(AVE_IOBA_SIZE, p + 8);
				dev_info(ave->dev, "  IOSZ at image +%#zx: 0 -> %#x\n",
					 i + 8, AVE_IOBA_SIZE);
			}
			patched |= 2;
		}
	}

	if (patched != 3) {
		dev_err(ave->dev,
			"IOBA/IOSZ tags not found in image (found mask %d)\n",
			patched);
		return -EINVAL;
	}
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
	u64 fwreg, map_iova;
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
	if (!ret)
		ret = ave_fw_patch_ioba(ave, ave->fw.cpu, span);
	if (ret)
		goto err_free;

	domain = iommu_get_domain_for_dev(ave->dev);
	if (!domain) {
		dev_err(ave->dev, "no IOMMU domain - is the DART attached?\n");
		ret = -ENODEV;
		goto err_free;
	}

	pa = iommu_iova_to_phys(domain, ave->fw.iova);

	/*
	 * Map where the coprocessor actually fetches, not where we would like
	 * it to.
	 *
	 * The ASC firmware-base register (ASC+0x50000) is programmed by iBoot
	 * and our writes to it are ignored - it reads back unchanged. Its base
	 * field is an IOVA, not a physical address, and the core fetches
	 * through this DART at that IOVA. Mapping the image at 0 instead
	 * produced a translation fault on stream 0 at base+0x200, repeating
	 * forever (docs/31).
	 *
	 * So we take the address from the register rather than choosing one.
	 */
	fwreg = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE);

	/*
	 * Take the low 32 bits, not the wide mask.
	 *
	 * This DART reports "AS 32 -> 42": a 32-bit input address space. The
	 * wide field reading gives 0x10000b28000, which is above 4 GiB and so
	 * can never be translated - and indeed mapping there verified clean
	 * via iommu_iova_to_phys() while every fetch still faulted, because
	 * the software page table happily held an address the hardware cannot
	 * present. The DART's fault register reports the same 0x100 prefix it
	 * cannot decode.
	 */
	map_iova = fwreg & 0xfffff000ULL;
	if (!map_iova) {
		dev_warn(ave->dev,
			 "fw-base register is %#llx; falling back to IOVA 0\n", fwreg);
		map_iova = AVE_FW_IOVA;
	}
	dev_info(ave->dev, "  fw-base register %#llx -> mapping image at IOVA %#llx\n",
		 fwreg, map_iova);
	if (!pa) {
		dev_err(ave->dev, "cannot resolve iova %pad to a phys addr\n",
			&ave->fw.iova);
		ret = -EFAULT;
		goto err_free;
	}

	dev_info(ave->dev, "  iova %pad -> phys %pa, mapping at IOVA %#llx\n",
		 &ave->fw.iova, &pa, map_iova);

	ave->fw.map_iova = map_iova;
	ret = iommu_map(domain, map_iova, pa, size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		if (ret == -EEXIST) {
			/*
			 * A previous probe leaked this mapping - the unmap
			 * used to use a hardcoded IOVA 0. The domain belongs
			 * to the device, not to us, so it survives rmmod and
			 * there is nothing else that will clean it up short
			 * of a reboot. Drop it and retry once.
			 */
			dev_warn(ave->dev,
				 "IOVA %#llx already mapped; dropping the stale mapping and retrying\n",
				 map_iova);
			iommu_unmap(domain, map_iova, size);
			ret = iommu_map(domain, map_iova, pa, size,
					IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		}
	}
	if (ret) {
		dev_err(ave->dev, "iommu_map at IOVA %#llx failed: %d\n",
			map_iova, ret);
		goto err_free;
	}
	ave->fw.mapped_at_zero = true;
	dev_info(ave->dev, "firmware mapped at IOVA %#llx, %#zx bytes\n",
		 map_iova, size);

	/*
	 * Verify the mapping rather than trusting iommu_map()'s return. The
	 * core faults at this exact IOVA, so "the call succeeded" is not the
	 * question - the question is whether the DART that the core fetches
	 * through can actually translate it.
	 */
	{
		phys_addr_t back = iommu_iova_to_phys(domain, map_iova);

		dev_info(ave->dev,
			 "  verify: iova_to_phys(%#llx) = %pa, expected %pa%s\n",
			 map_iova, &back, &pa,
			 back == pa ? "" : "   *** MISMATCH ***");
	}

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

	/*
	 * Unmap where we actually mapped. This used to unmap a hardcoded
	 * IOVA 0; once the map address started coming from the fw-base
	 * register the mapping leaked on every unload, and the next probe
	 * failed with -EEXIST.
	 */
	if (ave->fw.mapped_at_zero) {
		domain = iommu_get_domain_for_dev(ave->dev);
		if (domain)
			iommu_unmap(domain, ave->fw.map_iova, ave->fw.size);
		ave->fw.mapped_at_zero = false;
	}

	dma_free_attrs(ave->dev, ave->fw.size, ave->fw.cpu, ave->fw.iova,
		       DMA_ATTR_FORCE_CONTIGUOUS);
	ave->fw.cpu = NULL;
}

/*
 * Peek at the physical address the fw-base register points at.
 *
 * The core fetches at an address this DART cannot translate - its input
 * address space is 32 bits and the fetch is at ~1.1 TB - which means the
 * fetch is meant to bypass and land on physical memory. If iBoot left a
 * firmware image there we should see a Mach-O header, and its IOBA tag
 * should already be populated, since iBoot would have done for its own copy
 * what ave_fw_patch_ioba() now does for ours.
 *
 * Read-only, first page only.
 */
void ave_fw_peek_phys(struct ave_device *ave)
{
	phys_addr_t pa;
	void *p;
	u64 fwreg;
	u32 magic;

	fwreg = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE);
	pa = fwreg & AVE_ASC_FW_BASE_MASK;
	if (!pa)
		return;

	p = memremap(pa, SZ_4K, MEMREMAP_WB);
	if (!p)
		p = memremap(pa, SZ_4K, MEMREMAP_WT);
	if (!p) {
		dev_info(ave->dev, "  phys %pa: not mappable\n", &pa);
		return;
	}

	magic = get_unaligned_le32(p);
	dev_info(ave->dev, "  phys %pa: first word %#010x %s\n", &pa, magic,
		 magic == MH_MAGIC_64 ? "= MH_MAGIC_64, a Mach-O is here" :
					"(not a Mach-O header)");
	print_hex_dump(KERN_INFO, "ave phys: ", DUMP_PREFIX_OFFSET, 16, 1,
		       p, 16, true);
	/* The first word is a branch; show where it lands. */
	dev_info(ave->dev, "  branch target region (+0x200):\n");
	print_hex_dump(KERN_INFO, "ave +200: ", DUMP_PREFIX_OFFSET, 16, 1,
		       p + 0x200, 96, true);
	memunmap(p);
}
