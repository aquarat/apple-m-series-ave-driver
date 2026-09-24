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

#include <linux/crc32.h>
#include <linux/dma-mapping.h>
#include <linux/unaligned.h>
#include <linux/firmware.h>
#include <linux/iommu.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <crypto/sha2.h>
#include <linux/libnvdimm.h>	/* arch_wb_cache_pmem / arch_invalidate_pmem */

#include "ave.h"
#include "ave_dapf.h"

#define AVE_FW_NAME		"apple/ave_h13c.bin"
/*
 * Version-specific on purpose: this blob is the macOS 13.5 image's DATA, and
 * ave_fw_restore_data() refuses to use it for any other ABI. The older
 * (identical-content) apple/ave-data-pristine.bin still works via
 * fw_restore_path=.
 */
#define AVE_FW_PRISTINE_NAME	"apple/ave-13.5-data-pristine.bin"

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
 * The value is AP-PHYSICAL (0x40c000000), not the bus address. An earlier
 * version wrote bus 0x20c000000 on reasoning alone; the live AVE DATA segment
 * iBoot filled on this machine holds IOBA = 0x40c000000, next to CpAd =
 * 0x40d800000 and WrAd = 0x40dc00000 - the AP-physical ASC bank - and the GPU's
 * list uses AP-physical addresses the same way (docs/40 §4, docs/31 "the
 * dump"). Deriving it from the mapped resource keeps it tied to the DT.
 *
 * iBoot also fills CpAd, WrAd, SOC_ and SOCR, which we still leave unset.
 * Irrelevant while the core can only run iBoot's image (RVBAR is locked), but
 * required before our own image could ever run.
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
	bus = (u64)fabric;	/* AP-physical, as iBoot writes it */

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
 * docs/44 E3: DART mappings for iBoot's own firmware segments.
 *
 * Both options are OFF by default, and with both off ave_fw_load() behaves
 * exactly as before.
 *
 * DATA (fw_map_data=1). The 13.5 bootstrap loads DATA's base from the
 * iBoot-patched literal at image 0x423c, 0x1f0000ec000 (docs/44 §2.4), i.e.
 * DATA VA 0xec000 inside the 0x1f0_0000_0000 window that DAPF entry 0 admits.
 * The window is 4 GiB, the DART's input space is 32 bits ("AS 32"), so the
 * inference is that the DART translates the low 32 bits: DVA 0xec000 ->
 * physical 0x10001a90000, size 0x134000 (DATA vmsize). That mapping is what
 * this creates, read/write.
 *
 * TEXT (fw_map_text). Does the physical TEXT fetch also need a DART mapping?
 * docs/44 does NOT settle it, so it is a separate, selectable option:
 *
 *   - The fault address register reported the full 0x10000b28200 with
 *     NO_DAPF_MATCH while IOVA 0xb28000 was mapped. So the DAPF compares the
 *     full-width address, before any translation. What the DART does with a
 *     >32-bit address the DAPF *admits* has never been observed.
 *   - H1 reads it as a physical pass-through: the bootstrap builds its TEXT
 *     PTE from the MMU-off PC, i.e. TEXT's real DRAM address.
 *   - But the 0x1f0 window entry has the same r0 (0x33) we give TEXT, and the
 *     window is inferred to be *translated* by its low 32 bits. By that
 *     analogy an admitted 0x10000b28200 would be translated to DVA 0xb28200,
 *     and TEXT would need DVA 0xb28000 -> phys 0x10000b28000.
 *
 *   fw_map_text=0  legacy: our request_firmware() image is mapped at the DVA
 *                  taken from RVBAR's low 32 bits (0xb28000). NOTE the
 *                  installed apple/ave_h13c.bin is NOT the 13.5 image iBoot
 *                  loaded (different sha256), so a translated fetch would
 *                  run a mismatched copy - a confound for E3.
 *   fw_map_text=1  iBoot's TEXT, phys 0x10000b28000 size 0xec000, at DVA
 *                  0xb28000, READ-ONLY (t6000 is APPLE_DART2 format, which
 *                  honours IOMMU_WRITE absence). Physical and translated
 *                  fetches then see identical bytes: success cannot tell
 *                  them apart.
 *   fw_map_text=2  nothing at DVA 0xb28000. The discriminating run: with TEXT
 *                  admitted by the DAPF, a translated fetch faults NO_PTE /
 *                  NO_PMD / NO_TTBR at 0xb28200, a physical one does not.
 *
 * Safety. Neither range may touch memory Linux owns, and mapping it into a
 * DART is still a statement that the device may read (and for DATA write)
 * it. So before any iommu_map():
 *   - RVBAR's base field must be 0x10000b28000 and TEXT+0x423c must hold
 *     0x1f0000ec000, i.e. this boot's iBoot placed the image where the
 *     constants say (read through memremap, inside the 16 MiB window the
 *     liveness snapshot already reads);
 *   - region_intersects() must report the range disjoint from System RAM;
 *   - no /memory node reg may overlap it;
 *   - no /reserved-memory child (reg, or its dynamic reserved_mem) may
 *     overlap it;
 *   - every page of the DVA range must be unmapped in the domain, and inside
 *     its aperture, and everything page aligned.
 * Any failure refuses, and ave_fw_load() fails so the core is never started.
 */
/* Default on since f68: every working encode needed it (docs/53). */
static bool fw_map_data = true;
module_param(fw_map_data, bool, 0444);
MODULE_PARM_DESC(fw_map_data,
		 "E3: DART-map iBoot's DATA, DVA 0xec000 -> phys 0x10001a90000 +0x134000 (translating domain only; default on since f68)");

/* Default on since f68: every working encode needed it (docs/53). */
static int fw_map_text = 2;
module_param(fw_map_text, int, 0444);
MODULE_PARM_DESC(fw_map_text,
		 "E3: DVA 0xb28000 holds 0 = our image (legacy) | 1 = iBoot TEXT phys 0x10000b28000 read-only | 2 = nothing (default)");

int ave_fw_map_text_mode(void)
{
	return fw_map_text;
}

/*
 * DATA restore (fw_restore_data), mirroring macOS.
 *
 * The firmware's DATA segment is iBoot-preloaded once per boot. The FIRST
 * start of the core writes ~19 pages of DATA in place; a SECOND start within
 * the same boot then runs against that dirtied DATA and is silent. macOS never
 * hits this because it restores a pristine copy of DATA over the physical
 * segment before *every* start (13.5 AVE_Firmware::UpdateImage at kext VA
 * 0xfffffe0008f11ae4; 26.6.2 AVE_FwImg::UpdateImage -> RestoreCTRRData,
 * docs/42 §4-5, docs/45 row 32). This does the same so the core can be
 * (re)started without a reboot.
 *
 * Source of the pristine bytes - why Option B (committed blob), not Option A
 * (snapshot-at-load):
 *
 *   Option A would snapshot physical DATA into a buffer at module load and
 *   restore from that. It is only correct if DATA is still pristine at load,
 *   i.e. no start has happened since boot. Within one module lifetime we know
 *   we have not started the core - but Linux does not own this DRAM, so it
 *   survives rmmod: at a fresh load a *previous* load may have started the
 *   core and dirtied DATA, and nothing in the driver's own state records that
 *   across the reload. The only reliable way to decide "is this pristine?" is
 *   to compare against a known-pristine reference - which is exactly the blob
 *   Option B already carries. So a sound Option A needs Option B's data
 *   anyway, and B then subsumes it. Silently snapshotting possibly-dirty DATA
 *   is the failure the methodology warns against, so we do not.
 *
 *   Option B restores from data/blobs/ave-data-pristine.bin, produced by
 *   tools/extract_pristine_data.py from data/blobs/iboot-window-16m.bin (a
 *   physical dump taken before any start - what macOS itself snapshots).
 *   Verified against that dump: DATA sits at window offset 0xf68000, its first
 *   0x98000 bytes are covered (exactly to the window end) and 0x64000..0x98000
 *   is already zero; the tail 0x98000..0x134000 is bss, zero-padded. The dump
 *   (not the shipped Mach-O) is authoritative: iBoot fills 147 bytes of DATA
 *   at boot - the RTKit tag list, e.g. IOBA = 0x40c000000 - which macOS
 *   snapshots and the file lacks.
 *
 * What 13.5 actually copies (read out of the kext, not assumed from 26.6.2):
 *
 *   AVE_HwC::StartUpIOP        0xfffffe0008f11ae4  bl AVE_Firmware::UpdateImage
 *   AVE_Firmware::UpdateImage  0xfffffe0008ef7670  ldrb w8,[x19]; iBoot-loaded?
 *                              0xfffffe0008ef7758  bl RestoreCTRRData, return 0
 *   AVE_Firmware::RestoreCTRRData 0xfffffe0008ef5fe4:
 *       0xfffffe0008ef60b0  ldr x0, [x19,#120]   ; kernel VA of physical DATA
 *       0xfffffe0008ef60b8  ldr x1, [x19,#112]   ; the snapshot buffer
 *       0xfffffe0008ef60c0  ldr x2, [x19,#72]    ; segment 1 size = 0x134000
 *       0xfffffe0008ef60c4  bl  0xfffffe00083e4a30 ; memcpy
 *
 *   +120 is built in AVE_Firmware::AcquireCTRRData (0xfffffe0008ef5b68): the
 *   IOMemoryDescriptor over both iBoot segments is mapped, and DATA's offset
 *   inside it is segment 0's size (TEXT, +56). The snapshot at +112 is a
 *   kalloc of +72 bytes filled by the same memcpy in the other direction, and
 *   AcquireCTRRData is called exactly once, from AVE_Firmware::Init
 *   (0xfffffe0008ef7404) - i.e. before the core has ever run in that boot.
 *   So: the WHOLE DATA vmsize (0x134000, bss included) is restored, TEXT is
 *   never rewritten, and the source is DATA as iBoot left it. Confirmed.
 *
 * Install the blob at /lib/firmware/apple/ave-13.5-data-pristine.bin (the
 * default fw_restore_path). Off by default; with fw_restore_data=0 this is a
 * no-op, and fw_restore_data=2 is a dry run that compares and reports but
 * never writes.
 *
 * Refusals, because this is the first time Linux WRITES this DRAM (it has only
 * ever read it). Every one of these fails the probe before the core is started:
 *   - fw_abi must be 13.5: the blob belongs to that image and no other;
 *   - the blob must be exactly AVE_IBOOT_DATA_SIZE bytes and match the
 *     compiled-in sha256;
 *   - the running image must be the image the blob belongs to: RVBAR base,
 *     the iBoot literal at TEXT+0x423c, and three 16 KiB windows of TEXT
 *     hashed against the 13.5 image (docs/43 §3.2: aligned TEXT identity is
 *     the discriminator that separates this image from every sibling);
 *   - the destination must be outside System RAM, /memory and /reserved-memory
 *     (the same check the DART mapping path uses);
 *   - the core must not be running: CPU_CONTROL's RUN bit clear and
 *     CPU_STATUS STOPPED. The block is powered from stage 6 but the core is
 *     only released at stage 13, so this is the window the restore belongs in;
 *   - after the copy every byte is read back and compared, and any mismatch
 *     refuses.
 * Nothing outside [AVE_IBOOT_DATA_PHYS, +AVE_IBOOT_DATA_SIZE) is ever written:
 * that is the extent of the memremap and the length of the single memcpy.
 *
 * Power/coherency. DATA at AVE_IBOOT_DATA_PHYS is ordinary DRAM below Linux's
 * /memory map, not the VENC MMIO block, so VENC power is irrelevant to writing
 * it - the peek/snapshot code already reads it with no power handling. It uses
 * a cacheable (MEMREMAP_WB) mapping exactly as that read path does, then cleans
 * the range to the point of coherency (arch_wb_cache_pmem) so the copy is in
 * DRAM before the core fetches it, and invalidates it (arch_invalidate_pmem)
 * so the read-back verification reads DRAM and not our own dirty lines.
 */
static int fw_restore_data;
module_param(fw_restore_data, int, 0444);
MODULE_PARM_DESC(fw_restore_data,
		 "0 = off (default) | 1 = restore pristine DATA over phys 0x10001a90000 before the core starts (mirrors macOS) | 2 = dry run, compare and report but write nothing");

/*
 * Override the STKG stack-guard word (DATA+0x3a38) in the restored image.
 *
 * The pristine blob carries the cookie of the boot its dump came from, and
 * docs/51 2.3 argued that installing a stale cookie is self-consistent. R2
 * (docs/55 12) then showed a reset-and-restore start dying early, and a cold
 * full dump of the next boot (r3) differed from the blob ONLY inside this
 * word. Passing that boot's cold value here makes the restore write exactly
 * the cold DATA, so the next restart either works (the cookie was the cause)
 * or rules it out for good. 0 = use the blob's value. Applied to the in-memory
 * copy after the sha256 gate, so the read-back verifies what was written.
 */
static ulong fw_restore_stkg;
module_param(fw_restore_stkg, ulong, 0444);
MODULE_PARM_DESC(fw_restore_stkg,
		 "write this 64-bit value over the STKG word at DATA+0x3a38 in the restored image (0 = the blob's own)");

static char *fw_restore_path = AVE_FW_PRISTINE_NAME;
module_param(fw_restore_path, charp, 0444);
MODULE_PARM_DESC(fw_restore_path,
		 "request_firmware() path for the pristine DATA blob (default " AVE_FW_PRISTINE_NAME ")");

/*
 * sha256 of data/blobs/ave-13.5-data-pristine.bin, 0x134000 bytes, as produced
 * by tools/make_ave_data_blob.py (which also re-derives it from the 13.5 image
 * and refuses to emit anything else). Byte-identical to the blob
 * tools/extract_pristine_data.py emits.
 */
static const u8 ave_pristine_sha256[SHA256_DIGEST_SIZE] = {
	0xf1, 0xaf, 0x1e, 0xf4, 0x2b, 0xe0, 0x4a, 0x7d,
	0x60, 0x7b, 0x0b, 0xc1, 0xa2, 0x7d, 0xfd, 0xa5,
	0x72, 0x68, 0xb4, 0x76, 0xfe, 0xcf, 0x1d, 0x92,
	0xc8, 0xc1, 0x3c, 0x76, 0x0c, 0x71, 0xa1, 0x03,
};

/*
 * Three 16 KiB windows of the 13.5 image's __TEXT (file offsets 0x4000 +
 * these), which the pre-boot dump reproduces byte for byte. If the DRAM at
 * AVE_IBOOT_TEXT_PHYS does not hash to these, the image in memory is not the
 * one this blob's DATA belongs to and the restore is refused.
 */
#define AVE_TEXT_WINDOW_SIZE	SZ_16K
struct ave_text_window {
	u32	off;
	u8	sha[SHA256_DIGEST_SIZE];
};

static const struct ave_text_window ave_text_windows[] = {
	{ 0x0, {
		0x88, 0x5a, 0x78, 0x37, 0x38, 0xb3, 0xf7, 0xf8,
		0x91, 0xad, 0x16, 0xd3, 0xd5, 0x49, 0xb7, 0xd9,
		0x0d, 0x11, 0x4f, 0x67, 0x7b, 0xa1, 0x12, 0xc6,
		0xe3, 0xe5, 0x42, 0xb9, 0x5c, 0x72, 0xb9, 0xbe, } },
	{ 0x80000, {
		0x37, 0x54, 0x7c, 0x93, 0x89, 0xfb, 0x84, 0xc3,
		0x2b, 0x4f, 0x88, 0x8d, 0xde, 0x06, 0x1d, 0xd7,
		0xd4, 0xcc, 0xb7, 0xf0, 0x3c, 0x1e, 0x49, 0xee,
		0xd9, 0x65, 0xfd, 0x4e, 0x9b, 0xee, 0x7f, 0xbf, } },
	{ 0xe8000, {
		0x4d, 0x57, 0x01, 0x4b, 0xa7, 0x73, 0xd5, 0x76,
		0xd6, 0x5d, 0x1f, 0xdb, 0xcb, 0x51, 0xa6, 0x6d,
		0x6d, 0x8c, 0x59, 0x95, 0x9a, 0x48, 0x29, 0x2e,
		0xf2, 0xcb, 0xb5, 0xb8, 0xf4, 0x17, 0x0d, 0x09, } },
};

/* Where iBoot's per-boot stack guard sits inside DATA (docs/43 §3.4). */
#define AVE_DATA_STKG_OFF	0x3a38

#define AVE_IBOOT_DATA_LITERAL_OFF	0x423c	/* image offset, docs/44 §2.4 */
#define AVE_IBOOT_DATA_LITERAL		0x1f0000ec000ULL

static bool ave_ranges_overlap(u64 a, u64 alen, u64 b, u64 blen)
{
	return a < b + blen && b < a + alen;
}

/* 0 if [phys, phys+size) belongs to nothing Linux knows about. */
static int ave_fw_range_is_foreign(struct ave_device *ave, u64 phys, u64 size,
				   const char *what)
{
	struct device_node *np, *rmem_np, *child;
	struct resource r;
	unsigned int i;
	int ret;

	ret = region_intersects(phys, size, IORESOURCE_SYSTEM_RAM,
				IORES_DESC_NONE);
	if (ret != REGION_DISJOINT) {
		dev_err(ave->dev,
			"  %s: REFUSING - %#llx+%#llx intersects System RAM (%d)\n",
			what, phys, size, ret);
		return -EBUSY;
	}

	for_each_node_by_type(np, "memory") {
		for (i = 0; !of_address_to_resource(np, i, &r); i++) {
			if (ave_ranges_overlap(phys, size, r.start,
					       resource_size(&r))) {
				dev_err(ave->dev,
					"  %s: REFUSING - overlaps %pOF %pR\n",
					what, np, &r);
				of_node_put(np);
				return -EBUSY;
			}
		}
	}

	rmem_np = of_find_node_by_path("/reserved-memory");
	if (!rmem_np) {
		dev_err(ave->dev, "  %s: REFUSING - no /reserved-memory node to check against\n",
			what);
		return -ENODEV;
	}
	for_each_child_of_node(rmem_np, child) {
		struct reserved_mem *rm;
		bool had_reg = false;

		for (i = 0; !of_address_to_resource(child, i, &r); i++) {
			had_reg = true;
			if (ave_ranges_overlap(phys, size, r.start,
					       resource_size(&r))) {
				dev_err(ave->dev,
					"  %s: REFUSING - overlaps reserved-memory %pOF %pR\n",
					what, child, &r);
				of_node_put(child);
				of_node_put(rmem_np);
				return -EBUSY;
			}
		}
		if (had_reg)
			continue;
		rm = of_reserved_mem_lookup(child);
		if (rm && ave_ranges_overlap(phys, size, rm->base, rm->size)) {
			dev_err(ave->dev,
				"  %s: REFUSING - overlaps dynamic reserved-memory %pOF %pa+%pa\n",
				what, child, &rm->base, &rm->size);
			of_node_put(child);
			of_node_put(rmem_np);
			return -EBUSY;
		}
	}
	of_node_put(rmem_np);

	dev_info(ave->dev, "  %s: %#llx+%#llx is outside System RAM, /memory and /reserved-memory\n",
		 what, phys, size);
	return 0;
}

/* Is this boot's iBoot image where the constants say? */
static int ave_fw_check_iboot_placement(struct ave_device *ave)
{
	u64 fwreg = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE);
	u64 base = fwreg & AVE_ASC_FW_BASE_MASK;
	u64 lit;
	void *p;

	if (base != AVE_IBOOT_TEXT_PHYS) {
		dev_err(ave->dev,
			"  iboot: REFUSING - RVBAR %#llx base %#llx, constants assume %#llx\n",
			fwreg, base, AVE_IBOOT_TEXT_PHYS);
		return -EINVAL;
	}

	/*
	 * Map through the end of the literal, not a guessed page size: SZ_16K
	 * is 0x4000, and the literal sits at 0x423c. That mistake oopsed E3a
	 * on 2026-09-13 (results/e3a-*.log).
	 */
	ave_step(ave, "next: memremap + read iBoot TEXT literal");
	p = memremap(AVE_IBOOT_TEXT_PHYS,
		     PAGE_ALIGN(AVE_IBOOT_DATA_LITERAL_OFF + 8), MEMREMAP_WB);
	if (!p) {
		dev_err(ave->dev, "  iboot: REFUSING - cannot memremap TEXT to check the DATA literal\n");
		return -ENOMEM;
	}
	/* ldp w0, w1 at 0x308-0x310: low word then high word. */
	lit = get_unaligned_le32(p + AVE_IBOOT_DATA_LITERAL_OFF) |
	      ((u64)get_unaligned_le32(p + AVE_IBOOT_DATA_LITERAL_OFF + 4) << 32);
	memunmap(p);

	if (lit != AVE_IBOOT_DATA_LITERAL) {
		dev_err(ave->dev,
			"  iboot: REFUSING - TEXT+%#x holds DATA base %#llx, expected %#llx\n",
			AVE_IBOOT_DATA_LITERAL_OFF, lit, AVE_IBOOT_DATA_LITERAL);
		return -EINVAL;
	}
	dev_info(ave->dev, "  iboot: RVBAR base %#llx, DATA literal %#llx - placement as expected\n",
		 base, lit);
	return 0;
}

static int ave_fw_map_one(struct ave_device *ave, struct iommu_domain *domain,
			  u64 dva, u64 phys, u64 size, int prot, const char *what)
{
	u64 pgsz, off;
	phys_addr_t back;
	int ret;

	if (!domain->pgsize_bitmap)
		return -EINVAL;
	pgsz = 1ULL << __ffs(domain->pgsize_bitmap);
	if ((dva | phys | size) & (pgsz - 1)) {
		dev_err(ave->dev, "  %s: REFUSING - not aligned to DART page %#llx\n",
			what, pgsz);
		return -EINVAL;
	}
	if (domain->geometry.force_aperture &&
	    (dva < domain->geometry.aperture_start ||
	     dva + size - 1 > domain->geometry.aperture_end)) {
		dev_err(ave->dev, "  %s: REFUSING - DVA %#llx+%#llx outside aperture\n",
			what, dva, size);
		return -EINVAL;
	}
	for (off = 0; off < size; off += pgsz) {
		if (iommu_iova_to_phys(domain, dva + off)) {
			dev_err(ave->dev,
				"  %s: REFUSING - DVA %#llx is already mapped; not replacing someone's mapping\n",
				what, dva + off);
			return -EEXIST;
		}
	}

	ret = ave_fw_range_is_foreign(ave, phys, size, what);
	if (ret)
		return ret;

	ave_step(ave, "next: iommu_map %s dva %#llx -> phys %#llx +%#zx", what,
		 (u64)dva, (u64)phys, (size_t)size);
	ret = iommu_map(domain, dva, phys, size, prot, GFP_KERNEL);
	if (ret) {
		dev_err(ave->dev, "  %s: iommu_map failed: %d\n", what, ret);
		return ret;
	}

	back = iommu_iova_to_phys(domain, dva + size - pgsz);
	if (iommu_iova_to_phys(domain, dva) != phys ||
	    back != phys + size - pgsz) {
		dev_err(ave->dev, "  %s: verify MISMATCH after map (last page -> %pa)\n",
			what, &back);
		iommu_unmap(domain, dva, size);
		return -EIO;
	}
	dev_info(ave->dev, "  %s: DVA %#llx -> phys %#llx +%#llx %s, verified\n",
		 what, dva, phys, size, prot & IOMMU_WRITE ? "RW" : "RO");
	return 0;
}

static void ave_fw_unmap_iboot(struct ave_device *ave)
{
	struct iommu_domain *domain = ave->iboot_domain;
	size_t n;

	if (!ave->iboot_data_mapped && !ave->iboot_text_mapped)
		return;

	if (!domain || iommu_get_domain_for_dev(ave->dev) != domain) {
		dev_err(ave->dev,
			"iboot: device domain changed under us; cannot unmap iBoot segments\n");
		return;
	}
	if (ave->iboot_data_mapped) {
		n = iommu_unmap(domain, AVE_IBOOT_DATA_DVA, AVE_IBOOT_DATA_SIZE);
		dev_info(ave->dev, "iboot: DATA unmapped, %zu of %#llx bytes\n",
			 n, AVE_IBOOT_DATA_SIZE);
		ave->iboot_data_mapped = false;
	}
	if (ave->iboot_text_mapped) {
		n = iommu_unmap(domain, AVE_IBOOT_TEXT_DVA, AVE_IBOOT_TEXT_SIZE);
		dev_info(ave->dev, "iboot: TEXT unmapped, %zu of %#llx bytes\n",
			 n, AVE_IBOOT_TEXT_SIZE);
		ave->iboot_text_mapped = false;
	}
	ave->iboot_domain = NULL;
}

static int ave_fw_map_iboot(struct ave_device *ave, struct iommu_domain *domain,
			    u64 map_iova)
{
	int ret;

	if (fw_map_text < 0 || fw_map_text > 2) {
		dev_err(ave->dev, "fw_map_text=%d: must be 0, 1 or 2\n", fw_map_text);
		return -EINVAL;
	}
	if (!fw_map_data && !fw_map_text)
		return 0;

	if (!(domain->type & __IOMMU_DOMAIN_PAGING)) {
		dev_err(ave->dev,
			"iboot: REFUSING fw_map_data/fw_map_text - domain type %#x does not translate (need overlay variant=3, DMA group)\n",
			domain->type);
		return -EINVAL;
	}
	if (fw_map_text && map_iova != AVE_IBOOT_TEXT_DVA) {
		dev_err(ave->dev,
			"iboot: REFUSING - RVBAR-derived DVA %#llx, TEXT option assumes %#llx\n",
			map_iova, AVE_IBOOT_TEXT_DVA);
		return -EINVAL;
	}

	ret = ave_fw_check_iboot_placement(ave);
	if (ret)
		return ret;

	ave->iboot_domain = domain;

	if (fw_map_data) {
		ret = ave_fw_map_one(ave, domain, AVE_IBOOT_DATA_DVA,
				     AVE_IBOOT_DATA_PHYS, AVE_IBOOT_DATA_SIZE,
				     IOMMU_READ | IOMMU_WRITE, "iboot DATA");
		if (ret)
			goto fail;
		ave->iboot_data_mapped = true;
	}

	if (fw_map_text == 1) {
		ret = ave_fw_map_one(ave, domain, AVE_IBOOT_TEXT_DVA,
				     AVE_IBOOT_TEXT_PHYS, AVE_IBOOT_TEXT_SIZE,
				     IOMMU_READ, "iboot TEXT");
		if (ret)
			goto fail;
		ave->iboot_text_mapped = true;
	} else if (fw_map_text == 2) {
		dev_info(ave->dev,
			 "  iboot TEXT: leaving DVA %#llx UNMAPPED (discriminating run: a translated fetch must fault there)\n",
			 AVE_IBOOT_TEXT_DVA);
	}
	return 0;

fail:
	ave_fw_unmap_iboot(ave);
	ave->iboot_domain = NULL;
	return ret;
}

/*
 * Load the pristine DATA blob into a vmalloc buffer, once.
 *
 * The file must be exactly AVE_IBOOT_DATA_SIZE bytes and hash to
 * ave_pristine_sha256. A blob that is merely "about the right size" is refused:
 * this buffer is about to be written over DRAM the coprocessor executes from,
 * and the only cheap way to know it is the right bytes is to check all of them.
 * Idempotent: a second call is a no-op once the buffer exists.
 */
static int ave_fw_load_pristine(struct ave_device *ave)
{
	const struct firmware *fw;
	u8 dig[SHA256_DIGEST_SIZE];
	u8 *buf;
	int ret;

	if (ave->iboot_data_pristine)
		return 0;

	ret = request_firmware(&fw, fw_restore_path, ave->dev);
	if (ret) {
		dev_err(ave->dev,
			"fw_restore_data: no pristine blob at %s (%d). Build it with "
			"tools/make_ave_data_blob.py and install at "
			"/lib/firmware/" AVE_FW_PRISTINE_NAME "\n",
			fw_restore_path, ret);
		return ret;
	}
	if (fw->size != AVE_IBOOT_DATA_SIZE) {
		dev_err(ave->dev,
			"fw_restore_data: REFUSING - %s is %zu bytes, DATA is %#llx\n",
			fw_restore_path, fw->size, AVE_IBOOT_DATA_SIZE);
		ret = -EINVAL;
		goto out;
	}

	sha256(fw->data, fw->size, dig);
	if (memcmp(dig, ave_pristine_sha256, sizeof(dig))) {
		dev_err(ave->dev,
			"fw_restore_data: REFUSING - %s sha256 %*phN, expected %*phN\n",
			fw_restore_path, (int)sizeof(dig), dig,
			(int)sizeof(ave_pristine_sha256), ave_pristine_sha256);
		ret = -EINVAL;
		goto out;
	}

	buf = vmalloc(AVE_IBOOT_DATA_SIZE);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(buf, fw->data, AVE_IBOOT_DATA_SIZE);
	if (fw_restore_stkg) {
		dev_info(ave->dev,
			 "fw_restore_data: STKG override %#lx replaces the blob's %#llx (fw_restore_stkg)\n",
			 fw_restore_stkg,
			 get_unaligned_le64(buf + AVE_DATA_STKG_OFF));
		put_unaligned_le64(fw_restore_stkg, buf + AVE_DATA_STKG_OFF);
	}
	ave->iboot_data_pristine = buf;
	dev_info(ave->dev,
		 "fw_restore_data: pristine DATA from %s, %#llx bytes, sha256 %*phN OK\n",
		 fw_restore_path, AVE_IBOOT_DATA_SIZE,
		 (int)sizeof(dig), dig);
	ret = 0;
out:
	release_firmware(fw);
	return ret;
}

/*
 * Is the image in DRAM the one this blob's DATA belongs to?
 *
 * Read-only, three 16 KiB windows of TEXT, hashed against the 13.5 image.
 * docs/43 §3.2: aligned __TEXT identity is the one measure that separates this
 * image from every sibling variant and from 26.6.2 (100 % vs <= 22.5 %), so
 * this is the test that has to pass before anything is written to DATA.
 */
static int ave_fw_check_text_identity(struct ave_device *ave)
{
	u8 dig[SHA256_DIGEST_SIZE];
	unsigned int i;
	void *p;

	for (i = 0; i < ARRAY_SIZE(ave_text_windows); i++) {
		const struct ave_text_window *w = &ave_text_windows[i];

		p = memremap(AVE_IBOOT_TEXT_PHYS + w->off, AVE_TEXT_WINDOW_SIZE,
			     MEMREMAP_WB);
		if (!p) {
			dev_err(ave->dev,
				"  restore: REFUSING - cannot memremap TEXT+%#x to identify the image\n",
				w->off);
			return -ENOMEM;
		}
		sha256(p, AVE_TEXT_WINDOW_SIZE, dig);
		memunmap(p);

		if (memcmp(dig, w->sha, sizeof(dig))) {
			dev_err(ave->dev,
				"  restore: REFUSING - TEXT+%#x hashes %*phN, the 13.5 image has %*phN; "
				"the firmware in DRAM is not the one this blob belongs to\n",
				w->off, (int)sizeof(dig), dig,
				(int)sizeof(w->sha), w->sha);
			return -EINVAL;
		}
	}
	dev_info(ave->dev,
		 "  restore: TEXT at %#llx matches the 13.5 image over %u x %#x bytes\n",
		 AVE_IBOOT_TEXT_PHYS, (unsigned int)ARRAY_SIZE(ave_text_windows),
		 AVE_TEXT_WINDOW_SIZE);
	return 0;
}

/*
 * How far has DATA drifted from pristine? 0 on a fresh boot; non-zero once the
 * firmware has run, which is the evidence that the restore is needed at all.
 * Page granularity is 4 KiB to match ave_fw_diff_phys()'s page numbering.
 */
static u64 ave_fw_diff_pristine(struct ave_device *ave, const u8 *live,
				const u8 *want, const char *when)
{
	u64 bytes = 0, first = 0;
	unsigned int pages = 0;
	size_t i, off;
	bool got_first = false;

	for (off = 0; off < AVE_IBOOT_DATA_SIZE; off += SZ_4K) {
		if (!memcmp(live + off, want + off, SZ_4K))
			continue;
		pages++;
		for (i = off; i < off + SZ_4K; i++) {
			if (live[i] == want[i])
				continue;
			if (!got_first) {
				first = i;
				got_first = true;
			}
			bytes++;
		}
	}

	if (bytes)
		dev_info(ave->dev,
			 "  restore: DATA %s pristine: %llu byte(s) in %u page(s) differ, first at DATA+%#llx (phys %#llx)\n",
			 when, bytes, pages, first, AVE_IBOOT_DATA_PHYS + first);
	else
		dev_info(ave->dev,
			 "  restore: DATA %s pristine: 0 bytes differ over %#llx\n",
			 when, AVE_IBOOT_DATA_SIZE);
	return bytes;
}

/*
 * Restore pristine DATA over physical AVE_IBOOT_DATA_PHYS, as 13.5's
 * AVE_Firmware::UpdateImage -> RestoreCTRRData does at the head of StartUpIOP.
 *
 * Must be called with the core NOT started in this power session - Apple calls
 * it before AVE_IOP::Start and so do we. A no-op when fw_restore_data=0, so it
 * is safe to call unconditionally; fw_restore_data=2 reports and writes nothing.
 */
int ave_fw_restore_data(struct ave_device *ave)
{
	u32 ctl, status;
	u64 before, after;
	void *p;
	int ret;

	/*
	 * ave->recover_halted means stage 7 found a core a previous load had
	 * halted and has just reset the block. A second start over drifted
	 * DATA is silent (docs/51), so the restore is not optional there - it
	 * is the other half of the recovery, and leaving it to a module
	 * parameter is how s1-9 came to fail.
	 */
	int mode = fw_restore_data;

	if (!mode && ave->recover_halted) {
		dev_info(ave->dev,
			 "restore: recovering a halted core, so DATA is restored whether or not fw_restore_data was given\n");
		mode = 1;
	}
	if (!mode)
		return 0;
	if (mode < 0 || mode > 2) {
		dev_err(ave->dev, "fw_restore_data=%d: must be 0, 1 or 2\n",
			mode);
		return -EINVAL;
	}

	/*
	 * The blob is the 13.5 image's DATA. Restoring it under any other ABI
	 * would put one firmware's data under another firmware's code.
	 */
	if (ave->fw_abi != AVE_ABI_MACOS_13_5) {
		dev_err(ave->dev,
			"fw_restore_data: REFUSING - the blob is macOS 13.5 DATA and fw_abi is %s\n",
			ave_fw_abi_name(ave->fw_abi));
		return -EINVAL;
	}

	/*
	 * The core must not be running. The block is powered from stage 6, but
	 * the core is only released at stage 13; writing its DATA underneath a
	 * running firmware would corrupt whatever it is doing.
	 */
	ctl = ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_CONTROL);
	status = ave_read(ave, AVE_BANK_ASC, AVE_ASC_CPU_STATUS);
	if ((ctl & AVE_ASC_CPU_RUN) || !(status & AVE_ASC_ST_STOPPED)) {
		dev_err(ave->dev,
			"fw_restore_data: REFUSING - core is not halted (CPU_CONTROL %#x, CPU_STATUS %#x); "
			"the restore belongs before the ASC start, not after it\n",
			ctl, status);
		return -EBUSY;
	}
	dev_info(ave->dev,
		 "fw_restore_data=%d: core halted (CPU_CONTROL %#x, CPU_STATUS %#x)\n",
		 fw_restore_data, ctl, status);

	ret = ave_fw_load_pristine(ave);
	if (ret)
		return ret;

	/* Is iBoot's image where the constants say, and is it that image? */
	ret = ave_fw_check_iboot_placement(ave);
	if (ret)
		return ret;
	ret = ave_fw_check_text_identity(ave);
	if (ret)
		return ret;

	/*
	 * We are about to WRITE this physical range. It must belong to nothing
	 * Linux knows about - reuse the same System-RAM / /memory /
	 * /reserved-memory check the DART mapping path uses. Any overlap refuses
	 * and fails the start.
	 */
	ret = ave_fw_range_is_foreign(ave, AVE_IBOOT_DATA_PHYS,
				      AVE_IBOOT_DATA_SIZE, "restore DATA");
	if (ret)
		return ret;

	p = memremap(AVE_IBOOT_DATA_PHYS, AVE_IBOOT_DATA_SIZE,
		     ARCH_MEMREMAP_PMEM);
	if (!p) {
		dev_err(ave->dev,
			"fw_restore_data: cannot memremap DATA phys %#llx +%#llx\n",
			AVE_IBOOT_DATA_PHYS, AVE_IBOOT_DATA_SIZE);
		return -ENOMEM;
	}

	/*
	 * Evidence, before anything is written: 0 on the first start after a
	 * reboot (the write is then a no-op in content), non-zero afterwards.
	 */
	before = ave_fw_diff_pristine(ave, p, ave->iboot_data_pristine, "before restore vs");
	dev_info(ave->dev,
		 "  restore: STKG live %#llx, blob %#llx (random per boot; the blob's value is the dump's boot)\n",
		 get_unaligned_le64((u8 *)p + AVE_DATA_STKG_OFF),
		 get_unaligned_le64(ave->iboot_data_pristine + AVE_DATA_STKG_OFF));

	if (mode == 2) {
		dev_info(ave->dev,
			 "  restore: DRY RUN (fw_restore_data=2) - nothing written\n");
		memunmap(p);
		return 0;
	}

	/*
	 * The first write Linux has ever made to this DRAM. Marker first, so a
	 * machine that dies here names the operation that killed it.
	 */
	ave_step(ave, "next: WRITE %#llx bytes of pristine DATA over phys %#llx (first write to this DRAM)",
		 AVE_IBOOT_DATA_SIZE, AVE_IBOOT_DATA_PHYS);
	memcpy(p, ave->iboot_data_pristine, AVE_IBOOT_DATA_SIZE);
	/* Clean to the point of coherency: the core fetches DRAM, not our cache. */
	arch_wb_cache_pmem(p, AVE_IBOOT_DATA_SIZE);
	wmb();	/* land the copy before the ASC-start writes that follow */
	ave_step(ave, "write returned; next: read back and verify every byte");

	/* So the verification reads DRAM rather than the lines we just wrote. */
	arch_invalidate_pmem(p, AVE_IBOOT_DATA_SIZE);
	after = ave_fw_diff_pristine(ave, p, ave->iboot_data_pristine, "after restore vs");
	memunmap(p);

	if (after) {
		dev_err(ave->dev,
			"fw_restore_data: REFUSING to continue - %llu byte(s) did not take; "
			"DATA is now in an unknown state, do not start the core\n", after);
		return -EIO;
	}

	dev_info(ave->dev,
		 "fw_restore_data: restored %#llx bytes over phys %#llx (%llu had drifted), read-back verified\n",
		 AVE_IBOOT_DATA_SIZE, AVE_IBOOT_DATA_PHYS, before);

	/*
	 * Re-baseline the liveness snapshot. ave_drv.c checksums the 16 MiB
	 * window before the ASC start and diffs it afterwards to decide whether
	 * the core ran; the snapshot is taken before this call, so without this
	 * the pages we just rewrote would be counted as the firmware's own
	 * writes. (docs/51 §5 argues for moving the call earlier instead.)
	 */
	if (ave->snap_valid)
		ave_fw_snapshot_phys(ave);
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
	 *
	 * Correction (docs/44 §0): those faults were code 0x800 NO_DAPF_MATCH,
	 * the address filter, not a translation failure. See ave_fw_map_iboot()
	 * for what that changes.
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

	/* docs/44 E3 options; a no-op unless fw_map_data / fw_map_text. */
	ret = ave_fw_map_iboot(ave, domain, map_iova);
	if (ret)
		goto err_free;

	/*
	 * In bypass there is nothing to map, and nothing we could usefully
	 * map: the core fetches the physical address in the fw-base register,
	 * which is iBoot's bootstrap, not our image. Mapping is only
	 * meaningful for a translating domain.
	 */
	if (domain->type == IOMMU_DOMAIN_IDENTITY) {
		dev_info(ave->dev,
			 "  identity domain: DART in bypass, core will fetch phys %#llx directly\n",
			 fwreg & AVE_ASC_FW_BASE_MASK);
		ave->fw.mapped_at_zero = false;
		release_firmware(fw);
		return 0;
	}

	if (fw_map_text) {
		/*
		 * DVA map_iova now holds iBoot's TEXT (1) or deliberately
		 * nothing (2). Our image stays allocated - the identify scan
		 * uses it as its control - but is not mapped anywhere.
		 */
		dev_info(ave->dev,
			 "  fw_map_text=%d: NOT mapping our image at IOVA %#llx\n",
			 fw_map_text, map_iova);
		ave->fw.mapped_at_zero = false;
		release_firmware(fw);
		return 0;
	}

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
	ave_fw_unmap_iboot(ave);
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

	/* Before the early return: these do not depend on our image. */
	ave_fw_unmap_iboot(ave);

	/* fw_restore_data buffer: independent of our loaded image. */
	if (ave->iboot_data_pristine) {
		vfree(ave->iboot_data_pristine);
		ave->iboot_data_pristine = NULL;
	}

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
		       p + 0x200, 0x400, true);
	memunmap(p);
}

/*
 * Is the core actually executing?
 *
 * In bypass the core runs iBoot's image at a physical address we can read
 * directly, and the diagnostics that used to answer this question are dead:
 * the firmware-globals readback indexes *our* image, which the core never
 * touches, so it reports zero whatever happens.
 *
 * This asks the only question that survives - does the memory change? A
 * firmware that is running writes something: a stack, a heap, a log cursor,
 * a zeroed BSS. One that is parked in the b .+0 loop at +0x200 writes
 * nothing. Checksum each page before starting the core and again after, and
 * the answer is the list of pages that differ.
 */
static void *ave_snap_map(struct ave_device *ave, phys_addr_t *pa_out)
{
	u64 fwreg = ave_read64(ave, AVE_BANK_ASC, AVE_ASC_FW_BASE);
	phys_addr_t pa = fwreg & AVE_ASC_FW_BASE_MASK;

	if (!pa)
		return NULL;
	*pa_out = pa;
	return memremap(pa, AVE_SNAP_PAGES * SZ_4K, MEMREMAP_WB);
}

void ave_fw_snapshot_phys(struct ave_device *ave)
{
	phys_addr_t pa;
	void *p = ave_snap_map(ave, &pa);
	unsigned int i;

	ave->snap_valid = false;
	if (!p) {
		dev_info(ave->dev, "  snapshot: cannot map iBoot image\n");
		return;
	}
	for (i = 0; i < AVE_SNAP_PAGES; i++)
		ave->snap[i] = crc32(0, (u8 *)p + i * SZ_4K, SZ_4K);
	memunmap(p);
	ave->snap_valid = true;
	dev_info(ave->dev, "  snapshot: %u pages of %pa checksummed before start\n",
		 AVE_SNAP_PAGES, &pa);
}

void ave_fw_diff_phys(struct ave_device *ave)
{
	phys_addr_t pa;
	void *p;
	unsigned int i, changed = 0, first = 0;

	if (!ave->snap_valid)
		return;
	p = ave_snap_map(ave, &pa);
	if (!p)
		return;

	for (i = 0; i < AVE_SNAP_PAGES; i++) {
		if (crc32(0, (u8 *)p + i * SZ_4K, SZ_4K) != ave->snap[i]) {
			if (!changed)
				first = i;
			changed++;
			if (changed <= 8)
				dev_info(ave->dev, "    page %u (+%#x) changed\n",
					 i, i * SZ_4K);
		}
	}
	memunmap(p);

	if (changed)
		dev_info(ave->dev,
			 "  THE CORE IS ALIVE: %u/%u pages changed, first at +%#x\n",
			 changed, AVE_SNAP_PAGES, first * SZ_4K);
	else
		dev_info(ave->dev,
			 "  no page of iBoot's image changed (cannot tell parked from dead)\n");
}

/*
 * What is actually in the DRAM below Linux's memory map?
 *
 * docs/42 §3.3 established that the code at the RVBAR address is from the
 * same source family as AppleAVE2FW, but its test matched all nineteen
 * firmware variants equally, so it identified nothing. And §3.4 found that
 * TEXT and DATA cannot both fit between that address and the ISP carve-out,
 * which, if true, means the address is a device address meant to be
 * translated. That is the contradiction at the centre of the fetch problem.
 *
 * This looks for things only an AVE image contains - the IOBA/IOSZ tag pair
 * ave_fw_patch_ioba() edits, the AppleAVE2FW source path, CmdProcessor - and
 * maps which pages hold anything at all. Where the IOBA tag sits says where
 * DATA is; its payload says whether iBoot filled in the I/O base.
 *
 * The same scan is run over our own image first. If it cannot find the
 * patterns there, a miss in iBoot's memory means nothing.
 *
 * Read-only, and confined to the 16 MiB window the liveness snapshot has
 * already read safely on earlier runs. Nothing below the RVBAR address is
 * touched: that DRAM is not ours to know about, and on Apple silicon a read
 * of protected memory is not guaranteed to be harmless.
 */
struct ave_pat {
	const char *name;
	const u8 *bytes;
	size_t len;
};

static const u8 pat_ioba[] = { 'A', 'B', 'O', 'I', 8, 0, 0, 0 };
static const u8 pat_iosz[] = { 'Z', 'S', 'O', 'I', 4, 0, 0, 0 };
static const u8 pat_src[]  = "AppleAVE2FW/Firmware";
static const u8 pat_cmd[]  = "CmdProcessor";
static const u8 pat_macho[] = { 0xcf, 0xfa, 0xed, 0xfe };

static const struct ave_pat ave_pats[] = {
	{ "IOBA tag",	pat_ioba,  sizeof(pat_ioba) },
	{ "IOSZ tag",	pat_iosz,  sizeof(pat_iosz) },
	{ "src path",	pat_src,   sizeof(pat_src) - 1 },
	{ "CmdProc",	pat_cmd,   sizeof(pat_cmd) - 1 },
	{ "MH_MAGIC64",	pat_macho, sizeof(pat_macho) },
};

#define AVE_SCAN_MAX_HITS	6

static void ave_scan_buf(struct ave_device *ave, const char *what, u64 base,
			 const u8 *buf, size_t len)
{
	unsigned int hits[ARRAY_SIZE(ave_pats)] = { 0 };
	size_t i, p;

	for (i = 0; i < len; i++) {
		for (p = 0; p < ARRAY_SIZE(ave_pats); p++) {
			const struct ave_pat *pt = &ave_pats[p];

			if (buf[i] != pt->bytes[0] || i + pt->len > len ||
			    memcmp(buf + i, pt->bytes, pt->len))
				continue;
			/* Mach-O magic is only meaningful page-aligned. */
			if (pt->bytes == pat_macho && (i & 0xfff))
				continue;
			if (++hits[p] > AVE_SCAN_MAX_HITS)
				continue;
			if (pt->bytes == pat_ioba && i + 16 <= len)
				dev_info(ave->dev, "  [%s] %-10s at %#llx (+%#zx) payload %#llx\n",
					 what, pt->name, base + i, i,
					 get_unaligned_le64(buf + i + 8));
			else if (pt->bytes == pat_iosz && i + 12 <= len)
				dev_info(ave->dev, "  [%s] %-10s at %#llx (+%#zx) payload %#x\n",
					 what, pt->name, base + i, i,
					 get_unaligned_le32(buf + i + 8));
			else
				dev_info(ave->dev, "  [%s] %-10s at %#llx (+%#zx)\n",
					 what, pt->name, base + i, i);
		}
	}
	for (p = 0; p < ARRAY_SIZE(ave_pats); p++)
		dev_info(ave->dev, "  [%s] %-10s %u hit(s)\n",
			 what, ave_pats[p].name, hits[p]);
}

/* Runs of pages containing any non-zero byte, merged, 4 KiB granularity. */
static void ave_scan_extent(struct ave_device *ave, u64 base, const u8 *buf,
			    size_t pages)
{
	size_t i, run = 0, nruns = 0, nonzero = 0;
	bool in = false;

	for (i = 0; i <= pages; i++) {
		bool nz = false;

		if (i < pages)
			nz = memchr_inv(buf + i * SZ_4K, 0, SZ_4K) != NULL;
		if (nz)
			nonzero++;
		if (nz && !in) {
			run = i;
			in = true;
		} else if (!nz && in) {
			in = false;
			if (++nruns <= 24)
				dev_info(ave->dev, "  extent: %#llx-%#llx (%zu KiB)\n",
					 base + run * SZ_4K, base + i * SZ_4K,
					 (i - run) * 4);
		}
	}
	dev_info(ave->dev, "  extent: %zu non-zero pages of %zu, %zu run(s)\n",
		 nonzero, pages, nruns);
}

void ave_fw_identify_phys(struct ave_device *ave)
{
	phys_addr_t pa;
	void *p;

	/* Control first: the patterns must be findable in a known AVE image. */
	if (ave->fw.cpu && ave->fw.size)
		ave_scan_buf(ave, "ours ", 0, ave->fw.cpu, ave->fw.size);
	else
		dev_info(ave->dev, "  identify: our image is not loaded; no control scan\n");

	p = ave_snap_map(ave, &pa);
	if (!p) {
		dev_info(ave->dev, "  identify: cannot map the RVBAR window\n");
		return;
	}
	dev_info(ave->dev, "  identify: scanning %u KiB from %pa\n",
		 AVE_SNAP_PAGES * 4, &pa);
	ave_scan_buf(ave, "iboot", pa, p, AVE_SNAP_PAGES * SZ_4K);
	ave_scan_extent(ave, pa, p, AVE_SNAP_PAGES);
	memunmap(p);
}
