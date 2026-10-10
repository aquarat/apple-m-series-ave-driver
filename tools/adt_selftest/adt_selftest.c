// SPDX-License-Identifier: GPL-2.0-only
/*
 * adt_selftest: driver/ave_adt.c against a synthetic live ADT with known
 * answers, its malformed and truncated variants, and (optional argv[1]) a
 * real restore ADT. docs/99.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ave_adt.h"

static int fails, checks;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* --- a tiny ADT writer -------------------------------------------------- */
static u8 buf[1 << 16];
static size_t pos;

static void put32(u32 v)
{
	for (int i = 0; i < 4; i++)
		buf[pos++] = v >> (8 * i);
}

static void node(u32 nprops, u32 nkids)
{
	put32(nprops);
	put32(nkids);
}

static void prop(const char *name, const void *val, u32 len)
{
	memset(buf + pos, 0, 32);
	memcpy(buf + pos, name, strlen(name));
	pos += 32;
	put32(len);
	memcpy(buf + pos, val, len);
	pos += len;
	while (pos & 3)
		buf[pos++] = 0;
}

static void prop_str(const char *name, const char *s)
{
	prop(name, s, strlen(s) + 1);
}

static void prop_u32(const char *name, u32 v)
{
	u8 b[4] = { v, v >> 8, v >> 16, v >> 24 };

	prop(name, b, 4);
}

static void prop_u64s(const char *name, const u64 *v, int n)
{
	u8 b[256];

	for (int i = 0; i < n; i++)
		for (int j = 0; j < 8; j++)
			b[8 * i + j] = v[i] >> (8 * j);
	prop(name, b, 8 * n);
}

static void segs(const u64 (*s)[4], int n)
{
	u8 b[128];

	for (int i = 0; i < n; i++) {
		u8 *e = b + 32 * i;

		for (int j = 0; j < 3; j++)
			for (int k = 0; k < 8; k++)
				e[8 * j + k] = s[i][j] >> (8 * k);
		for (int k = 0; k < 4; k++)
			e[24 + k] = s[i][3] >> (8 * k);
		memset(e + 28, 0, 4);
	}
	prop("segment-ranges", b, 32 * n);
}

/* offsets of interesting bytes, for the malformed variants */
static size_t off_armio_hdr, off_ave0_segsize;

/*
 * root (#address-cells 2)
 *   cpus { cpu0 }                      - a subtree to skip
 *   arm-io (#address-cells 2, #size-cells 2, ranges 0 -> 0x200000000 +0x400000000)
 *     ave-hint  (reg 0x20d100000)      - not an encoder name
 *     ave0      (reg 0x20d100000, pre-loaded, TEXT + DATA)  -> 0x40d100000
 *     ave1      (reg 0x307100000, no pre-loaded, no segments) -> 0x507100000
 *     ave3      (reg 0x5_0000_0000)    - outside every range
 */
static size_t build(void)
{
	static const u64 ranges[] = { 0x0, 0x200000000ULL, 0x400000000ULL };
	static const u64 seg0[2][4] = {
		{ 0x10000bfc000ULL, 0x0, 0x10000bfc000ULL, 0xec000 },
		{ 0x10001640000ULL, 0xec000, 0x1f0000ec000ULL, 0x134000 },
	};
	u64 reg[2];

	pos = 0;
	node(3, 2);
	prop_str("name", "device-tree");
	prop_u32("#address-cells", 2);
	prop_u32("#size-cells", 2);

	node(1, 1);
	prop_str("name", "cpus");
	node(1, 0);
	prop_str("name", "cpu0");

	off_armio_hdr = pos;
	node(4, 4);
	prop_str("name", "arm-io");
	prop_u32("#address-cells", 2);
	prop_u32("#size-cells", 2);
	prop_u64s("ranges", ranges, 3);

	reg[0] = 0x20d100000ULL; reg[1] = 0x45c000;
	node(2, 0);
	prop_str("name", "ave-hint");
	prop_u64s("reg", reg, 2);

	node(4, 1);
	prop_str("name", "ave0");
	prop_u64s("reg", reg, 2);
	prop("pre-loaded", "\1\0\0\0", 4);
	off_ave0_segsize = pos + 32;	/* the size word of segment-ranges */
	segs(seg0, 2);
	node(1, 0);
	prop_str("name", "ave0-child");

	reg[0] = 0x307100000ULL;
	node(2, 0);
	prop_str("name", "ave1");
	prop_u64s("reg", reg, 2);

	reg[0] = 0x500000000ULL;
	node(2, 0);
	prop_str("name", "ave3");
	prop_u64s("reg", reg, 2);
	return pos;
}

static void test_synthetic(void)
{
	struct ave_adt_enc e;
	size_t len = build();
	u8 *copy;
	int ret;

	ret = ave_adt_find_encoder(buf, len, 0x40d100000ULL, &e);
	CHECK(ret == 0, "ave0 found (%d)", ret);
	CHECK(!strcmp(e.name, "ave0"), "ave0's name, not ave-hint's (%s)", e.name);
	CHECK(e.preloaded, "ave0 pre-loaded");
	CHECK(e.nseg == 2, "ave0 has 2 segments (%u)", e.nseg);
	CHECK(e.seg[0].phys == 0x10000bfc000ULL && e.seg[0].iova == 0 &&
	      e.seg[0].size == 0xec000, "ave0 TEXT");
	CHECK(e.seg[1].phys == 0x10001640000ULL && e.seg[1].iova == 0xec000 &&
	      e.seg[1].remap == 0x1f0000ec000ULL && e.seg[1].size == 0x134000,
	      "ave0 DATA");

	ret = ave_adt_find_encoder(buf, len, 0x507100000ULL, &e);
	CHECK(ret == 0 && !strcmp(e.name, "ave1") && !e.preloaded && e.nseg == 0,
	      "ave1: found, not pre-loaded, no segments (%d %s %d %u)",
	      ret, e.name, e.preloaded, e.nseg);

	/* controls: an address no node has; ave3 is outside every range */
	CHECK(ave_adt_find_encoder(buf, len, 0x40d100004ULL, &e) == -ENOENT,
	      "no encoder at a near-miss address");
	CHECK(ave_adt_find_encoder(buf, len, 0x500000000ULL, &e) == -ENOENT,
	      "an untranslatable reg is not matched as its bus address");
	CHECK(ave_adt_find_encoder(buf, len, 0x700000000ULL, &e) == -ENOENT,
	      "ave3 is not matched at bus + 0x200000000 either");

	/* every truncation: refused, never a read past the end (ASan) */
	copy = malloc(len);
	for (size_t n = 0; n < len; n++) {
		memcpy(copy, buf, n);
		ret = ave_adt_find_encoder(copy, n, 0x40d100000ULL, &e);
		CHECK(ret == -EINVAL || ret == -ENOENT ||
		      /* ave0's node is complete before its child and the later siblings */
		      ret == 0, "truncated to %zu: %d", n, ret);
		if (ret == 0)
			CHECK(e.nseg == 2, "a truncated ADT that parses has all of ave0 (%zu)", n);
	}
	free(copy);

	/* a property size running past the end */
	build();
	buf[off_ave0_segsize + 3] = 0x7f;
	CHECK(ave_adt_find_encoder(buf, len, 0x40d100000ULL, &e) == -EINVAL,
	      "segment-ranges size past the end: -EINVAL");
	/* a segment-ranges length that is not whole entries */
	build();
	buf[off_ave0_segsize] = 60;
	CHECK(ave_adt_find_encoder(buf, len, 0x40d100000ULL, &e) == -EINVAL,
	      "segment-ranges of 60 bytes: -EINVAL");
	/* an absurd child count */
	build();
	buf[off_armio_hdr + 4] = 0xff;
	buf[off_armio_hdr + 5] = 0xff;
	CHECK(ave_adt_find_encoder(buf, len, 0x40d100000ULL, &e) == -EINVAL,
	      "65535 children: -EINVAL");

	/* random single-byte corruption: any answer, but no out-of-bounds read */
	srand(1);
	for (int i = 0; i < 20000; i++) {
		build();
		buf[rand() % len] = rand();
		ave_adt_find_encoder(buf, len, 0x40d100000ULL, &e);
	}
	checks++;
}

static void test_real(const char *path)
{
	struct ave_adt_enc e;
	u8 *b;
	long n;
	FILE *f = fopen(path, "rb");
	int ret;

	if (!f) {
		printf("SKIP real ADT: %s not present\n", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	b = malloc(n);
	if (fread(b, 1, n, f) != (size_t)n) {
		fclose(f);
		free(b);
		CHECK(0, "cannot read %s", path);
		return;
	}
	fclose(f);
	/* the j314c restore ADT: both encoders, neither pre-loaded (no iBoot ran) */
	ret = ave_adt_find_encoder(b, n, 0x40d100000ULL, &e);
	CHECK(ret == 0 && !strcmp(e.name, "ave0") && !e.preloaded && !e.nseg,
	      "restore ADT: ave0 at 0x40d100000, not pre-loaded (%d %s)", ret, e.name);
	ret = ave_adt_find_encoder(b, n, 0x507100000ULL, &e);
	CHECK(ret == 0 && !strcmp(e.name, "ave1"),
	      "restore ADT: ave1 at 0x507100000 (%d %s)", ret, e.name);
	CHECK(ave_adt_find_encoder(b, n, 0x267100000ULL, &e) == -ENOENT,
	      "restore ADT: no t8103/t8112 encoder on a j314c");
	free(b);
	printf("real ADT %s: %ld bytes\n", path, n);
}

int main(int argc, char **argv)
{
	test_synthetic();
	if (argc > 1)
		test_real(argv[1]);
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
