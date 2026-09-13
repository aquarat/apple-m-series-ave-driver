#!/usr/bin/env python3
"""Build the pristine macOS 13.5 AVE firmware __DATA blob, as iBoot leaves it.

Why this exists
---------------
The AVE firmware's first start after a reboot works; a second start in the same
boot is silent, because the first run modified 19 pages of the firmware's
__DATA segment and nothing put them back (docs/31, 2026-09-13 19:30). macOS
does put them back: 13.5 `AVE_HwC::StartUpIOP` calls `AVE_Firmware::UpdateImage`
(`0xfffffe0008ef7670`) at its head, which on the iBoot-loaded path calls
`AVE_Firmware::RestoreCTRRData` (`0xfffffe0008ef5fe4`) - a memcpy of a pristine
snapshot over the whole physical __DATA carve-out, every start. See docs/51.

This tool produces that snapshot offline, so the driver can do the same without
ever having to capture one from a fresh boot itself.

Provenance
----------
13.5 __DATA is VA 0xec000 + 0x134000, of which only the first 0x64000 bytes are
file-backed in `ave_h13c.bin` (file offset 0xf0000); the rest is zero-fill
(docs/43 §4). Two sources are available for the file-backed part:

  * the image (`data/blobs/macos-13.5/ave_h13c.bin`), which lacks the 147 bytes
    iBoot fills in (STKG / SOC_ / SOCR / CpAd / WrAd / IOBA and the tunables
    region), and
  * the pre-boot DRAM dump (`data/blobs/iboot-window-16m.bin`), 16 MiB from
    physical 0x10000b28000 taken before any firmware start, whose DATA window
    at dump offset 0xf68000 is exactly what the coprocessor would have read on
    its first fetch.

The dump is used as the source, because it is the only artefact that contains
what iBoot actually wrote on this machine; the image alone would boot a firmware
with a zero I/O base. The image is still used, as a check: every byte of the
dump outside the four known iBoot-filled ranges must equal the image, or the
dump is not the image's memory and the build is refused. `--from-image` builds
the other way round (image + the iBoot bytes lifted out of the dump); both
constructions are computed on every run and must agree byte for byte.

The stack guard (STKG, DATA+0x3a38, 8 bytes) is random per boot: the blob
carries the value from the dump's boot, not from the boot it is restored into.
That is deliberate and harmless - the firmware reads the cookie out of this same
tag list at run time and compares against what it itself installed, so any
well-formed value works; there is no second copy anywhere that must agree with
it. macOS's snapshot happens to hold the current boot's value only because it
is captured in the same boot. (The other iBoot-filled tags - SOC_ 0x6001,
SOCR 0x11, CpAd 0x40d800000, WrAd 0x40dc00000, IOBA 0x40c000000 - are SoC
constants and MMIO bases, not per-boot allocations, so a stale copy is the same
copy. `--verify` re-checks them against the dump and against the live-looking
values recorded in docs/43 §3.4.)

Usage
-----
  tools/make_ave_data_blob.py                     # build + self-check
  tools/make_ave_data_blob.py --verify            # verify an existing blob only
  tools/make_ave_data_blob.py --print-tags        # decode the tag list too

Output: data/blobs/ave-13.5-data-pristine.bin, 0x134000 bytes. It is
Apple-derived, so it stays in data/blobs/ (gitignored) and is never committed.
"""
import argparse
import hashlib
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IMG_13_5 = "data/blobs/macos-13.5/ave_h13c.bin"
DUMP = "data/blobs/iboot-window-16m.bin"
OUT = "data/blobs/ave-13.5-data-pristine.bin"

# Negative controls: images that must NOT pass as the pristine DATA.
CONTROLS = [
    ("26.6.2 H13C", "data/blobs/ave_h13c.bin", 0x138000, 0x134000),
    ("13.5 H13D (M1 Ultra)", "data/blobs/macos-13.5/ave_all/ave_h13d.bin", 0xF0000, 0x64000),
    ("13.5 H13S (M1 Pro)", "data/blobs/macos-13.5/ave_all/ave_h13s.bin", 0xF0000, 0x64000),
    ("13.5 H13G (M1)", "data/blobs/macos-13.5/ave_all/ave_h13g.bin", 0xF0000, 0x64000),
]

# --- 13.5 layout, docs/43 §4 (confirmed from the section table) -------------
DATA_VA = 0xEC000
DATA_SIZE = 0x134000          # vmsize: what the restore must cover
DATA_FILE_OFF = 0xF0000       # in ave_h13c.bin
DATA_FILE_SIZE = 0x64000      # file-backed part; the rest is zero-fill
DUMP_BASE = 0x10000B28000     # physical address of the dump's first byte
DATA_PHYS = 0x10001A90000     # physical base of __DATA
DUMP_DATA_OFF = DATA_PHYS - DUMP_BASE          # 0xf68000
DUMP_DATA_LEN = 16 << 20      # dump is 16 MiB; DATA coverage is what is left

# The four ranges iBoot fills in, as DATA offsets [start, end). docs/43 §3.4;
# re-derived by this tool on every run and checked against this list.
IBOOT_RANGES = [
    (0x3A38, 0x3A40, "STKG stack guard (_rtk_patchbay, random per boot)"),
    (0x3BA3, 0x3BE9, "SOC_ / SOCR / CpAd / WrAd / IOBA (_rtk_patchbay)"),
    (0x6036B, 0x60505, "_rtk_tunables values (the region TUNS/TUNZ point at)"),
]
STKG_RANGE = (0x3A38, 0x3A40)

# Tag values that are SoC/MMIO constants, not per-boot allocations. Checked
# against the dump so a blob from another machine or SoC cannot pass.
EXPECT_TAGS = {
    "SOC_": 0x6001,
    "SOCR": 0x11,
    "CpAd": 0x40D800000,
    "WrAd": 0x40DC00000,
    "IOBA": 0x40C000000,
}


def p(rel):
    return os.path.join(REPO, rel)


def read(rel, what):
    path = p(rel)
    if not os.path.exists(path):
        sys.exit(f"missing {what}: {rel} (see docs/43 §1 for how to produce it)")
    with open(path, "rb") as f:
        return f.read()


def sha(buf):
    return hashlib.sha256(buf).hexdigest()


def diff_ranges(a, b, gap=0):
    """Coalesced [start, end) ranges where a and b differ."""
    runs, cur = [], None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if cur and i - cur[1] <= gap:
                cur[1] = i + 1
            else:
                cur = [i, i + 1]
                runs.append(cur)
    return [tuple(r) for r in runs]


def ndiff(a, b):
    n = min(len(a), len(b))
    return sum(1 for i in range(n) if a[i] != b[i]), n


def tags(buf, start_hint=0):
    """Decode the RTKit tag list (same walk as tools/rtkit_tags.py)."""
    out = {}
    m = re.search(rb"GKTS\x08\x00\x00\x00", buf[start_hint:])
    if not m:
        return out, None
    i = start_hint + m.start()
    first = i
    for _ in range(64):
        if i + 8 > len(buf):
            break
        tag, n = buf[i:i + 4], int.from_bytes(buf[i + 4:i + 8], "little")
        if not all(0x20 <= c < 0x7F for c in tag) or n > 0x1000:
            break
        out[tag[::-1].decode()] = (i, buf[i + 8:i + 8 + n])
        i += 8 + n
    return out, first


# __const + __data, the only part of the file-backed __DATA whose content
# differs between firmware variants of the same build (docs/43 §4 section
# table: _rtk_patchbay starts at DATA+0x3a30, everything after it is stack
# filler, zero-filled boot/page-table space, and the tiny power/tunables
# blocks, all of which are identical across variants).
HEAD_WINDOW = 0x3A30
CONTROL_MAX_PCT = 99.0

# __TEXT windows driver/ave_fw.c hashes to prove the image in DRAM is this one.
TEXT_WINDOWS = (0x0, 0x80000, 0xE8000)
TEXT_WINDOW_SIZE = 0x4000


def control_row(name, blob, other):
    """Print one negative-control row; True if the candidate is rejected."""
    n, cmp_len = ndiff(blob[:len(other)], other)
    whole = 100.0 * (cmp_len - n) / cmp_len
    hn, hlen = ndiff(blob[:HEAD_WINDOW], other[:HEAD_WINDOW])
    head = 100.0 * (hlen - hn) / hlen
    same_hash = sha(blob) == sha(other) and len(blob) == len(other)
    rejected = not same_hash and head < CONTROL_MAX_PCT
    print(f"   {name:24s} {'MATCHES' if same_hash else 'differs':8s}  "
          f"{hlen - hn:6d}/{hlen} = {head:6.2f} %  {whole:6.2f} %  "
          f"{'OK (rejected)' if rejected else '*** FAIL: not rejected ***'}")
    return rejected


def in_iboot_range(off):
    return any(s <= off < e for s, e, _ in IBOOT_RANGES)


def build(img, dump, from_image=False):
    """Return the 0x134000-byte pristine DATA blob."""
    img_data = img[DATA_FILE_OFF:DATA_FILE_OFF + DATA_FILE_SIZE]
    dump_data = dump[DUMP_DATA_OFF:]
    if len(img_data) != DATA_FILE_SIZE:
        sys.exit("13.5 image is too short for __DATA")
    if len(dump_data) < DATA_FILE_SIZE:
        sys.exit("dump does not cover the file-backed part of __DATA")

    if from_image:
        # image + only the iBoot-filled bytes lifted out of the dump
        body = bytearray(img_data)
        for s, e, _ in IBOOT_RANGES:
            body[s:e] = dump_data[s:e]
    else:
        # the dump as it stands: what iBoot actually wrote on this machine
        body = bytearray(dump_data[:DATA_FILE_SIZE])

    blob = bytearray(DATA_SIZE)
    blob[:DATA_FILE_SIZE] = body
    return bytes(blob)


def check_sources(img, dump):
    """Everything the construction assumes. Any failure refuses the build."""
    ok = True
    img_data = img[DATA_FILE_OFF:DATA_FILE_OFF + DATA_FILE_SIZE]
    dump_data = dump[DUMP_DATA_OFF:]

    print(f"source 13.5 image  {IMG_13_5}")
    print(f"                   sha256 {sha(img)}")
    print(f"source pre-boot dump {DUMP}  ({len(dump) >> 20} MiB from {DUMP_BASE:#x})")
    print(f"                   sha256 {sha(dump)}")
    print(f"__DATA VA {DATA_VA:#x} size {DATA_SIZE:#x}; file-backed {DATA_FILE_SIZE:#x} "
          f"at image {DATA_FILE_OFF:#x}; physical {DATA_PHYS:#x} (dump {DUMP_DATA_OFF:#x})")

    # 1. the differences between image and dump are exactly the iBoot ranges
    runs = diff_ranges(img_data, dump_data[:DATA_FILE_SIZE])
    total = sum(e - s for s, e in runs)
    outside = [(s, e) for s, e in runs if not (in_iboot_range(s) and in_iboot_range(e - 1))]
    print(f"\nimage vs dump over {DATA_FILE_SIZE:#x} file-backed bytes: "
          f"{total} bytes differ in {len(runs)} run(s)")
    for s, e, name in IBOOT_RANGES:
        n = sum(1 for i in range(s, e) if img_data[i] != dump_data[i])
        print(f"   DATA+{s:#07x}..{e:#07x} (VA {DATA_VA + s:#x})  {n:3d} byte(s) filled  {name}")
    if outside:
        ok = False
        print(f"   REFUSED: {len(outside)} differing run(s) OUTSIDE the known iBoot ranges: "
              f"{['%#x-%#x' % r for r in outside[:8]]}")
    else:
        print("   no byte outside the known iBoot ranges differs - the dump IS this image's memory")
    if total != 147:
        ok = False
        print(f"   REFUSED: expected 147 iBoot-filled bytes (docs/43 §3.4), found {total}")

    # 2. the rest of DATA that the dump covers is zero (bss the firmware
    #    initialises itself)
    covered = min(len(dump_data), DATA_SIZE)
    tail = dump_data[DATA_FILE_SIZE:covered]
    nz = sum(1 for b in tail if b)
    print(f"\ndump covers DATA+{0:#x}..{covered:#x} of {DATA_SIZE:#x}; "
          f"beyond the file-backed part: {nz} non-zero byte(s) in {len(tail):#x}")
    if nz:
        ok = False
        print("   REFUSED: the pre-boot dump is not zero where the blob will be zero")
    print(f"   DATA+{covered:#x}..{DATA_SIZE:#x} is NOT covered by the dump; "
          f"zero-filled (INFERRED: __zerofill, and everything the dump does cover is zero)")

    # 3. the constant tags are the ones this machine's iBoot wrote
    t, at = tags(dump_data[:DATA_FILE_SIZE])
    print(f"\ntag list in the dump at DATA+{at:#x} (VA {DATA_VA + at:#x}, "
          f"physical {DATA_PHYS + at:#x}):")
    for name, want in EXPECT_TAGS.items():
        if name not in t:
            ok = False
            print(f"   REFUSED: tag {name} not found in the dump")
            continue
        off, val = t[name]
        got = int.from_bytes(val, "little")
        good = got == want
        ok &= good
        print(f"   {name}  {got:#x}  {'as expected' if good else 'REFUSED: expected %#x' % want}")
    if "STKG" in t:
        off, val = t["STKG"]
        print(f"   STKG {int.from_bytes(val, 'little'):#x}  at DATA+{off + 8:#x} "
              f"- random per boot; carried over from the dump's boot deliberately")
        if (off + 8, off + 16) != STKG_RANGE:
            ok = False
            print(f"   REFUSED: STKG payload at {off + 8:#x}, expected {STKG_RANGE[0]:#x}")
    return ok


def verify(blob, img, dump, controls=True):
    """Compare the blob with both sources, and run the negative controls."""
    ok = True
    img_data = img[DATA_FILE_OFF:DATA_FILE_OFF + DATA_FILE_SIZE]
    dump_data = dump[DUMP_DATA_OFF:]
    covered = min(len(dump_data), DATA_SIZE)

    print(f"\nblob {OUT}")
    print(f"   size {len(blob):#x} ({len(blob)} bytes)")
    print(f"   sha256 {sha(blob)}")
    if len(blob) != DATA_SIZE:
        print(f"   FAIL: size must be exactly {DATA_SIZE:#x}")
        return False

    # positive: the blob equals the dump everywhere the dump covers
    n, _ = ndiff(blob[:covered], dump_data[:covered])
    print(f"\nblob vs pre-boot dump over {covered:#x} covered bytes: {n} byte(s) differ")
    if n:
        ok = False
        print(f"   FAIL: the blob must equal the dump wherever the dump reaches "
              f"(first runs: {['%#x-%#x' % r for r in diff_ranges(blob[:covered], dump_data[:covered])[:8]]})")
    else:
        print("   PASS")

    # positive: the blob differs from the 13.5 image only in the 147 iBoot bytes
    runs = diff_ranges(blob[:DATA_FILE_SIZE], img_data)
    total = sum(e - s for s, e in runs)
    outside = [(s, e) for s, e in runs if not (in_iboot_range(s) and in_iboot_range(e - 1))]
    print(f"\nblob vs 13.5 image __DATA: {total} byte(s) differ in {len(runs)} run(s)")
    if total == 147 and not outside:
        print("   PASS - exactly the 147 iBoot-filled bytes, all inside the known ranges")
    else:
        ok = False
        print(f"   FAIL - expected 147 bytes inside the known ranges; "
              f"{len(outside)} run(s) are outside")
    ntail, _ = ndiff(blob[DATA_FILE_SIZE:], bytes(DATA_SIZE - DATA_FILE_SIZE))
    print(f"   tail DATA+{DATA_FILE_SIZE:#x}..{DATA_SIZE:#x}: "
          f"{'all zero - PASS' if ntail == 0 else 'FAIL: %d non-zero bytes' % ntail}")
    ok &= ntail == 0

    if not controls:
        return ok

    # Negative controls: a test that says yes to everything has measured
    # nothing (00-methodology.md trap 2).
    #
    # Whole-__DATA byte identity is a WEAK discriminator and is printed only to
    # show that: most of the file-backed part is the build-independent
    # RTKSTACK filler plus zero-filled _rtk_boot / _rtk_page_tables, so the
    # M1 Ultra sibling H13D scores 99.7 % against the M1 Max blob (docs/43
    # §3.2 records the same effect). The measure that does discriminate is
    # __const + __data, DATA+0..0x3a30, where the sibling drops to 92.9 %.
    # The gate the driver actually applies is the exact sha256; the running
    # image is identified by its __TEXT, not by __DATA.
    print("\nnegative controls - each must fail the sha256 gate and score < 99 % "
          f"over __const+__data (DATA+0..{HEAD_WINDOW:#x}):")
    print(f"   {'candidate':24s} {'sha256':8s}  {'__const+__data':>16s}  {'whole __DATA':>14s}")
    for name, rel, off, size in CONTROLS:
        path = p(rel)
        if not os.path.exists(path):
            print(f"   {name:24s} SKIPPED (not fetched: {rel})")
            continue
        with open(path, "rb") as f:
            f.seek(off)
            other = f.read(min(size, DATA_SIZE))
        ok &= control_row(name, blob, other)

    # Control on the apparatus itself: the 13.5 __TEXT is not its __DATA.
    ok &= control_row("13.5 H13C __TEXT", blob, img[0x4000:0x4000 + DATA_FILE_SIZE])
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verify", action="store_true",
                    help="verify the existing blob; write nothing")
    ap.add_argument("--from-image", action="store_true",
                    help="build from the image + the dump's iBoot bytes "
                         "(the default builds from the dump; both must agree)")
    ap.add_argument("--print-tags", action="store_true",
                    help="also decode the blob's tag list")
    ap.add_argument("--c-constants", action="store_true",
                    help="print the constants driver/ave_fw.c compiles in "
                         "(blob sha256 and the TEXT window hashes)")
    ap.add_argument("-o", "--out", default=OUT)
    a = ap.parse_args()

    img = read(IMG_13_5, "the 13.5 firmware image")
    dump = read(DUMP, "the pre-boot DRAM dump")

    if a.verify:
        blob = read(a.out, "the blob (build it first without --verify)")
        ok = verify(blob, img, dump)
    else:
        ok = check_sources(img, dump)
        if not ok:
            sys.exit("\nREFUSED: the sources do not agree; no blob written")
        from_dump = build(img, dump, from_image=False)
        from_img = build(img, dump, from_image=True)
        if from_dump != from_img:
            runs = diff_ranges(from_dump, from_img)
            sys.exit(f"\nREFUSED: the two constructions disagree in {len(runs)} run(s): "
                     f"{['%#x-%#x' % r for r in runs[:8]]}")
        print("\nboth constructions (dump-as-is, image+iBoot-bytes) agree byte for byte")
        blob = from_img if a.from_image else from_dump
        with open(p(a.out) if not os.path.isabs(a.out) else a.out, "wb") as f:
            f.write(blob)
        print(f"wrote {a.out}")
        ok = verify(blob, img, dump)

    if a.c_constants:
        def carr(digest):
            rows = [", ".join(f"0x{b:02x}" for b in digest[i:i + 8]) for i in range(0, 32, 8)]
            return "\n".join("\t" + r + "," for r in rows)

        print("\n/* ave_pristine_sha256: " + OUT + " */")
        print(carr(hashlib.sha256(blob).digest()))
        for off in TEXT_WINDOWS:
            w = img[0x4000 + off:0x4000 + off + TEXT_WINDOW_SIZE]
            same = w == dump[off:off + TEXT_WINDOW_SIZE]
            print(f"/* TEXT+{off:#x}, {TEXT_WINDOW_SIZE:#x} bytes; "
                  f"image {'==' if same else '!='} dump */")
            print(carr(hashlib.sha256(w).digest()))

    if a.print_tags:
        t, at = tags(blob[:DATA_FILE_SIZE])
        print(f"\ntag list at DATA+{at:#x}:")
        for name, (off, val) in t.items():
            print(f"   {name}  len {len(val):3d}  {int.from_bytes(val, 'little'):#x}  "
                  f"(DATA+{off + 8:#x})")

    print("\n" + ("ALL CHECKS PASSED" if ok else "CHECKS FAILED"))
    print(f"sha256 {sha(blob)}  {len(blob):#x} bytes")
    if ok:
        print("install with: sudo install -m644 " + a.out +
              " /lib/firmware/apple/ave-13.5-data-pristine.bin")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
