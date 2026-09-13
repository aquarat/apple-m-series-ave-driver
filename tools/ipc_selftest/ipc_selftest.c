// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace self-test for driver/ave_abi_boot.h: message-1 validation, the
 * per-version channel tables, descriptor parsing, and the IOProcessorChannel
 * ring (bidirectional and unidirectional) played from both ends over a fake
 * shared-memory channel block.
 *
 * Touches no hardware. Build and run:  make -C tools/ipc_selftest run
 *
 * Every positive check has a negative control next to it (docs/00 trap 2):
 * a check that cannot fail measures nothing.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ave_abi_boot.h"

static int failures, checks;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		printf("FAIL %s:%d: ", __func__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define FW_BASE	0x80000000ull	/* opaque firmware view of FwIPC */

/* ------------------------------------------------------------------------ */

static void test_msg1(void)
{
	const struct ave_boot_abi *a13 = ave_boot_abi_get(AVE_ABI_MACOS_13_5);
	const struct ave_boot_abi *a26 = ave_boot_abi_get(AVE_ABI_MACOS_26_6);
	const u32 m13[4] = { 7, 0x9bc0, 0x100, 0xc0000 };
	const u32 m26[4] = { 2, 0x1f280, 0x100, 0xc0000 };
	u32 m[4], r;

	CHECK(a13 && a26, "getter returned NULL");
	CHECK(ave_boot_abi_get(AVE_ABI_UNKNOWN) == NULL, "unknown ABI has a descriptor");

	/* Positive: each version accepts its own predicted message 1. */
	CHECK(ave_boot_check_msg1(a13, m13) == 0, "13.5 own msg1 -> %#x", ave_boot_check_msg1(a13, m13));
	CHECK(ave_boot_check_msg1(a26, m26) == 0, "26.6 own msg1 -> %#x", ave_boot_check_msg1(a26, m26));

	/* Negative: cross-version. 7 channels exceeds 26.6.2's host limit of 3. */
	r = ave_boot_check_msg1(a26, m13);
	CHECK(r & AVE_MSG1_BAD_NCH, "26.6 must reject 7 channels (%#x)", r);
	/* 2 channels is legal for the 13.5 host (1..8) but not what 13.5 sends. */
	r = ave_boot_check_msg1(a13, m26);
	CHECK(!(r & AVE_MSG1_REJECT), "13.5 host accepts nch 2 (%#x)", r);
	CHECK((r & AVE_MSG1_UNEXP_NCH) && (r & AVE_MSG1_UNEXP_CHSZ), "13.5 flags 26.6 msg1 (%#x)", r);

	/* Apple's own rejections. */
	memcpy(m, m13, sizeof(m)); m[2] = 0x101;
	CHECK(ave_boot_check_msg1(a13, m) & AVE_MSG1_BAD_VER, "version 0x101");
	memcpy(m, m13, sizeof(m)); m[0] = 0;
	CHECK(ave_boot_check_msg1(a13, m) & AVE_MSG1_BAD_NCH, "nch 0");
	memcpy(m, m13, sizeof(m)); m[0] = 9;
	CHECK(ave_boot_check_msg1(a13, m) & AVE_MSG1_BAD_NCH, "nch 9");
	memcpy(m, m13, sizeof(m)); m[0] = 8;
	CHECK(!(ave_boot_check_msg1(a13, m) & AVE_MSG1_BAD_NCH), "nch 8 is legal on 13.5");
	memcpy(m, m26, sizeof(m)); m[0] = 3;
	CHECK(!(ave_boot_check_msg1(a26, m) & AVE_MSG1_BAD_NCH), "nch 3 is legal on 26.6");
	memcpy(m, m13, sizeof(m)); m[1] = 0;
	CHECK(ave_boot_check_msg1(a13, m) & AVE_MSG1_BAD_CHSZ, "chsz 0");
	memcpy(m, m26, sizeof(m)); m[1] = 0x80000000u;
	CHECK(ave_boot_check_msg1(a26, m) & AVE_MSG1_BAD_CHSZ, "26.6 chsz negative");
	CHECK(!(ave_boot_check_msg1(a13, (const u32[4]){ 7, 0x80000000u, 0x100, 0xc0000 }) & AVE_MSG1_BAD_CHSZ),
	      "13.5 host tests only for zero");
	memcpy(m, m13, sizeof(m)); m[1] = 0x600;
	CHECK(ave_boot_check_msg1(a13, m) & AVE_MSG1_CHSZ_SHORT, "chsz too small for 7 descriptors");

	/* Heap is inferred: mismatch is flagged but never rejected. */
	memcpy(m, m13, sizeof(m)); m[3] = 0x100000;
	r = ave_boot_check_msg1(a13, m);
	CHECK((r & AVE_MSG1_UNEXP_HEAP) && !(r & AVE_MSG1_REJECT), "heap mismatch %#x", r);
}

/* ------------------------------------------------------------------------ */

/* The table must tile the block exactly as the firmware's size literal says. */
static void test_tables(void)
{
	static const enum ave_fw_abi abis[] = { AVE_ABI_MACOS_13_5, AVE_ABI_MACOS_26_6 };
	unsigned int v, i;

	for (v = 0; v < 2; v++) {
		const struct ave_boot_abi *a = ave_boot_abi_get(abis[v]);
		u32 off = a->nchans * AVE_BOOT_DESC_STRIDE;
		u32 bits = 0;

		CHECK(a->nchans == a->msg1_nch, "%s: table %u vs msg1 %u", a->name, a->nchans, a->msg1_nch);
		for (i = 0; i < a->nchans; i++) {
			const struct ave_boot_chan *c = &a->chans[i];

			CHECK(c->slots_off == off, "%s %s: slots at %#x, tiling says %#x",
			      a->name, c->name, c->slots_off, off);
			off = c->slots_off + c->nslots * AVE_BOOT_SLOT_SIZE;
			CHECK(ave_boot_name_to_id(a, c->name) == c->id, "%s %s: id", a->name, c->name);
			CHECK(ave_boot_chan_expect(a, c->id) == c, "%s %s: lookup", a->name, c->name);
			bits |= 1u << c->bit;
		}
		/* Same 0x40 of slop on both versions (docs/36, docs/45 §2.4). */
		CHECK(off + 0x40 == a->msg1_chsz, "%s: content %#x + 0x40 != %#x", a->name, off, a->msg1_chsz);
		/* 16 KiB alignment the firmware enforces. */
		CHECK((a->fwipc_size & (AVE_BOOT_ALIGN_16K - 1)) == 0, "%s: FwIPC size", a->name);
		CHECK(a->client_buf_expect <= a->client_buf_max, "%s: client buffer", a->name);
		printf("  %s: %u channels, doorbell bits %#x, block %#x\n",
		       a->name, a->nchans, bits, a->msg1_chsz);
	}

	/* Negative controls for the name table. */
	CHECK(ave_boot_name_to_id(ave_boot_abi_get(AVE_ABI_MACOS_26_6), "TERMINAL") < 0,
	      "26.6.2 host table has no TERMINAL");
	CHECK(ave_boot_name_to_id(ave_boot_abi_get(AVE_ABI_MACOS_13_5), "TERMINAL") == AVE_CH_TERMINAL,
	      "13.5 host table has TERMINAL");
	CHECK(ave_boot_name_to_id(ave_boot_abi_get(AVE_ABI_MACOS_13_5), "IO_T2") < 0, "prefix must not match");

	/* 13.5 shared doorbell bits: bit 3 wakes three channels, bit 1 two. */
	{
		const struct ave_boot_abi *a = ave_boot_abi_get(AVE_ABI_MACOS_13_5);
		unsigned int n1 = 0, n3 = 0, n0 = 0;

		for (i = 0; i < a->nchans; i++) {
			n0 += a->chans[i].bit == 0;
			n1 += a->chans[i].bit == 1;
			n3 += a->chans[i].bit == 3;
		}
		CHECK(n0 == 1 && n1 == 2 && n3 == 3, "13.5 bit sharing %u/%u/%u", n0, n1, n3);
	}
}

/* ------------------------------------------------------------------------ */

/* Play ChannelTableCreate into a zeroed block, as the firmware does. */
static void fw_write_table(const struct ave_boot_abi *a, u8 *blk, u64 base_fw)
{
	unsigned int i;

	for (i = 0; i < a->nchans; i++) {
		u8 *d = blk + i * AVE_BOOT_DESC_STRIDE;

		strncpy((char *)d, a->chans[i].name, AVE_BOOT_DESC_NAME_LEN);
		ave_boot_put_le32(d + AVE_BOOT_DESC_DIR, a->chans[i].dir);
		ave_boot_put_le32(d + AVE_BOOT_DESC_BIT, a->chans[i].bit);
		ave_boot_put_le32(d + AVE_BOOT_DESC_NSLOTS, a->chans[i].nslots);
		ave_boot_put_le64(d + AVE_BOOT_DESC_SLOTS, base_fw + a->chans[i].slots_off);
	}
}

static void test_descriptors(void)
{
	static const enum ave_fw_abi abis[] = { AVE_ABI_MACOS_13_5, AVE_ABI_MACOS_26_6 };
	unsigned int v, i;

	for (v = 0; v < 2; v++) {
		const struct ave_boot_abi *a = ave_boot_abi_get(abis[v]);
		u8 *blk = calloc(1, a->msg1_chsz);
		u64 base = FW_BASE + 0x1000;

		fw_write_table(a, blk, base);
		for (i = 0; i < a->nchans; i++) {
			struct ave_boot_desc d;
			const struct ave_boot_chan *want;
			int id;

			ave_boot_desc_parse(blk + i * AVE_BOOT_DESC_STRIDE, &d);
			id = ave_boot_name_to_id(a, d.name);
			want = ave_boot_chan_expect(a, id);
			CHECK(want && want->dir == d.dir && want->bit == d.bit &&
			      want->nslots == d.nslots && d.slots_fw == base + want->slots_off,
			      "%s desc %u '%s' round trip", a->name, i, d.name);
		}
		/* Negative: a corrupted field must be visible. */
		{
			struct ave_boot_desc d;

			ave_boot_put_le32(blk + AVE_BOOT_DESC_NSLOTS, 511);
			ave_boot_desc_parse(blk, &d);
			CHECK(d.nslots != a->chans[0].nslots, "corruption not detected");
		}
		free(blk);
	}
}

/* ------------------------------------------------------------------------ */

static u64 slot_word0(const struct ave_ring *r, unsigned int i)
{
	return ave_boot_get_le64(r->slots + (u64)i * AVE_BOOT_SLOT_SIZE);
}

/* Host initiator (dir 0, host type 1) against firmware responder (type 0). */
static void test_ring_io(u32 nslots)
{
	u8 *slots = calloc(nslots, AVE_BOOT_SLOT_SIZE);
	struct ave_ring host, fw;
	u64 p = 0;
	u32 a1 = 0, a2 = 0, i;

	/* The firmware side (even) touches nothing; the host (odd) writes phase 1. */
	ave_ring_init(&fw, slots, nslots, 0);
	CHECK(slot_word0(&fw, 0) == 0, "even init wrote a slot");
	ave_ring_init(&host, slots, nslots, ave_ring_dir_to_type(0));
	CHECK(host.type == 1 && slot_word0(&host, nslots - 1) == 1, "odd init");

	/* Negative control: nothing published yet. */
	CHECK(ave_ring_recv(&fw, &p, &a1, &a2) != 0, "fw received from an empty ring");
	CHECK(ave_ring_recv(&host, &p, &a1, &a2) != 0, "host received before sending");

	/* Fill the ring without replies: exactly nslots sends succeed. */
	for (i = 0; i < nslots; i++)
		CHECK(ave_ring_send(&host, FW_BASE + 0x40 * (i + 1), 0x48, 0) == 0, "send %u", i);
	CHECK(ave_ring_send(&host, FW_BASE, 1, 0) != 0, "send into a full ring succeeded");

	/* Firmware takes each, replies into the same slot, host collects. */
	for (i = 0; i < nslots; i++) {
		CHECK(ave_ring_recv(&fw, &p, &a1, &a2) == 0 && p == FW_BASE + 0x40 * (i + 1) && a1 == 0x48,
		      "fw recv %u (p %#llx)", i, (unsigned long long)p);
		CHECK(ave_ring_send(&fw, p, a1, 0x5a) == 0, "fw reply %u", i);
		CHECK(ave_ring_recv(&host, &p, &a1, &a2) == 0 && a2 == 0x5a, "host ack %u", i);
	}
	CHECK(ave_ring_recv(&host, &p, &a1, &a2) != 0, "host received a phantom ack");
	CHECK(ave_ring_recv(&fw, &p, &a1, &a2) != 0, "fw received a phantom command");

	/* Several laps: no per-lap phase flip (docs/36 §6). */
	for (i = 0; i < 3 * nslots + 1; i++) {
		CHECK(ave_ring_send(&host, FW_BASE + 4, i, 0) == 0, "lap send %u", i);
		CHECK(ave_ring_recv(&fw, &p, &a1, &a2) == 0 && a1 == i, "lap fw recv %u", i);
		CHECK(ave_ring_send(&fw, p, a1, 0) == 0, "lap fw send %u", i);
		CHECK(ave_ring_recv(&host, &p, &a1, &a2) == 0 && a1 == i, "lap host recv %u", i);
	}
	/* Receive masks two low bits; phase lives in bit 0. */
	CHECK(ave_ring_send(&host, FW_BASE | 2, 0, 0) == 0 &&
	      ave_ring_recv(&fw, &p, &a1, &a2) == 0 && p == FW_BASE, "payload low bits");
	free(slots);
}

/* Firmware initiator (dir 1, fw type 1) against host responder (type 0): IO_T2H. */
static void test_ring_t2h(u32 nslots)
{
	u8 *slots = calloc(nslots, AVE_BOOT_SLOT_SIZE);
	struct ave_ring host, fw;
	u64 p = 0;
	u32 a1 = 0, a2 = 0, i;

	ave_ring_init(&fw, slots, nslots, ave_ring_fw_type_13_5(1));
	ave_ring_init(&host, slots, nslots, ave_ring_dir_to_type(1));
	CHECK(host.type == 0 && fw.type == 1, "types");
	CHECK(ave_ring_recv(&host, &p, &a1, &a2) != 0, "host received before fw sent");

	for (i = 0; i < 2 * nslots; i++) {
		CHECK(ave_ring_send(&fw, FW_BASE + 0x100, 0x30, 7) == 0, "fw send %u", i);
		CHECK(ave_ring_recv(&host, &p, &a1, &a2) == 0 && a2 == 7, "host recv %u", i);
		/* Credit return (k13 f0d758): echo into the same slot. */
		CHECK(ave_ring_send(&host, p, a1, 0) == 0, "host echo %u", i);
		CHECK(ave_ring_recv(&fw, &p, &a1, &a2) == 0, "fw credit %u", i);
	}
	/* Negative control: without the echo the firmware runs out of slots. */
	for (i = 0; i < nslots; i++) {
		CHECK(ave_ring_send(&fw, FW_BASE, 0, 0) == 0, "fw send w/o credit %u", i);
		CHECK(ave_ring_recv(&host, &p, &a1, &a2) == 0, "host recv w/o echo %u", i);
	}
	CHECK(ave_ring_send(&fw, FW_BASE, 0, 0) != 0, "fw sent with no credit returned");
	free(slots);
}

/* TERMINAL: dir 2 -> host type 2 (unidirectional), firmware type 3. */
static void test_ring_terminal(u32 nslots)
{
	u8 *slots = calloc(nslots, AVE_BOOT_SLOT_SIZE);
	struct ave_ring host, fw;
	u64 p = 0;
	u32 a1 = 0, a2 = 0, i;

	/* Hazard, and a discriminating check: zeroed slots look "available" to
	 * a type-2 receiver. Only the firmware's init (slot[0] = 3) prevents it. */
	ave_ring_init(&host, slots, nslots, ave_ring_dir_to_type(2));
	CHECK(host.type == 2, "type");
	CHECK(ave_ring_urecv(&host, &p, &a1, &a2) == 0 && p == 0,
	      "zeroed TERMINAL slot not seen as available - the hazard test measures nothing");

	memset(slots, 0, (size_t)nslots * AVE_BOOT_SLOT_SIZE);
	ave_ring_init(&fw, slots, nslots, ave_ring_fw_type_13_5(2));
	ave_ring_init(&host, slots, nslots, 2);
	CHECK(fw.type == 3 && slot_word0(&fw, 0) == 3, "fw init writes 3");
	CHECK(ave_ring_urecv(&host, &p, &a1, &a2) != 0, "host read before fw sent");

	for (i = 0; i < 3 * nslots; i++) {
		CHECK(ave_ring_usend(&fw, FW_BASE + 0x80 * (i % nslots), 17, 0) == 0, "fw usend %u", i);
		CHECK(ave_ring_urecv(&host, &p, &a1, &a2) == 0 && a1 == 17 &&
		      p == FW_BASE + 0x80 * (i % nslots), "host urecv %u", i);
		CHECK((slot_word0(&host, i % nslots) & 1) == 1, "host handed slot %u back", i);
	}
	/* Without the host draining, the firmware can fill every slot, then stops. */
	for (i = 0; i < nslots; i++)
		CHECK(ave_ring_usend(&fw, FW_BASE, 1, 0) == 0, "fill %u", i);
	CHECK(ave_ring_usend(&fw, FW_BASE, 1, 0) != 0, "fw overwrote an unread line");
	free(slots);
}

int main(void)
{
	const struct ave_boot_abi *a13 = ave_boot_abi_get(AVE_ABI_MACOS_13_5);
	unsigned int i;

	test_msg1();
	test_tables();
	test_descriptors();
	for (i = 0; i < a13->nchans; i++) {
		const struct ave_boot_chan *c = &a13->chans[i];

		if (c->dir == 0)
			test_ring_io(c->nslots);
		else if (c->dir == 1)
			test_ring_t2h(c->nslots);
		else
			test_ring_terminal(c->nslots);
	}
	test_ring_io(992);	/* 26.6.2 IO    */
	test_ring_t2h(993);	/* 26.6.2 IO_T2H */

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
