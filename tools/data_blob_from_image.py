#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a pristine 13.5 AVE DATA blob from the firmware image plus iBoot's fills.

Why
---
The driver needs DATA "as iBoot left it" (docs/51) to start a core a second
time in a boot (in-place restore) or, where iBoot's DATA cannot be used at all,
to build the core's DATA itself (owned DATA: t6001's ave1, docs/82; a Mac mini
on stock m1n1, docs/89 §6). Until now every blob came from a cold RAM dump
(tools/make_ave_data_blob.py, docs/87 §3). This tool builds one from the image
alone, plus the few bytes iBoot writes, and says where each of those bytes came
from.

What iBoot fills (docs/89 §6.2, re-derived by `validate` on every run)
-----------------------------------------------------------------------
The image's __DATA carries the RTKit patchbay, a packed list of
{4-char code stored byte-reversed, u32 len, payload} records starting with
STKG and ending with IOSZ (symbols __rtk_patch_*). iBoot fills six payloads:

  STKG  8  _rtk_stack_guard              random per boot; any value works [I, docs/51]
  SOC_  4  RTK_soc                       0x6000 / 0x8103 / 0x8112 ...
  SOCR  4  RTK_soc_revision              0x21 (t6000 C1), 0x11 (B1)
  CpAd  8  RTK_cpu_physical_address      the ASC bank
  WrAd  8  RTK_cpu_wrapper_physical_address  ASC + 0x400000
  IOBA  8  RTK_io_base                   the fabric bank on t600x, 0 on t8103/t8112

and the table at TUNS (TUNZ bytes, __rtk_platform_asc_tunables_block): a header
{u8 version 1, u8 3, u8 capacity, u8 count, u32 h} followed by `capacity`
entries of {u32 offset, u64 mask, u64 value}. The image ships it with count 0
and h 0xffffffff; iBoot writes count, h and the entries. H14G's TUNZ holds a
second table (version 3) that iBoot leaves empty. Nothing else in DATA differs
from the image, and DATA past the file-backed part is zero [C for H13S, H13C,
H14G: `validate`]. H13C's (t6001) table is H13S's 21 entries with h 0x11.

Subcommands
-----------
  fills IMAGE                 the fill set of an image, from its own tag list
  diff IMAGE PRISTINE         decode a real pristine blob's fills; fail if any
                              other byte differs from the image
  validate                    rebuild the H13S, H13C and H14G blobs from their images
                              and the recorded values below; must equal the real
                              blobs byte for byte (and differ only in STKG when
                              STKG is not carried over); plus negative controls
  build --soc S ...           write a blob for SoC S (t6000, t6001, t8112, t8103)
  dump-check --soc S DUMP     which fill pages of a (partly overwritten) DATA
                              dump survived, and what they hold

t8103 (H13G): the tunables are UNKNOWN
--------------------------------------
STKG, SOC_, SOCR, CpAd, WrAd and IOBA are known (the j313 port, docs/89). The
tunables are not: they come from iBoot's own tables [I] (its images are
encrypted in the IPSW), they are not in the j274/j313/j314s/j314c/j473 ADTs nor
in the M2's 13.5 kernelcache, and the j313 blob was never published. Its
sha256 (pinned in driver/ave_soc.c) rules out H13S's table with the known
tags, STKG and a zero bss (`validate` shows it), so H13G's differ from H13S's.
`build --soc t8103` therefore needs an explicit --tunables:

  --tunables dump --dump DATA.bin   take ONLY iBoot's fill bytes from a DATA
                                    dump of this machine (fresh boot, read-only,
                                    test/physdump.ko base=0x8019b0000
                                    size=0x128000); the pages holding them must
                                    have survived (checked against the image)
  --tunables none                   leave the image's empty table (count 0): the
                                    firmware applies no ASC tunables [I]
  --tunables h13s                   borrow t6000's table (same H13 ASC) [U]

The driver accepts such a blob for owned DATA only, after checking it against
the image itself (soc->iboot.blob_by_image, driver/ave_fw.c
ave_fw_blob_by_image); the in-place path keeps requiring the pinned dump.

Everything here reads data/blobs/ (gitignored, Apple-derived) and writes there.
"""
import argparse
import hashlib
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = 0x4000          # Linux page size on Asahi: survival granularity in a dump

FILL_TAGS = (("STKG", 8), ("SOC_", 4), ("SOCR", 4), ("CpAd", 8), ("WrAd", 8), ("IOBA", 8))

# ---------------------------------------------------------------------------
# Recorded values. Marks as in docs/00: [C] read from a real pristine blob or
# a log, [I] inferred, [U] unknown.
# ---------------------------------------------------------------------------
TUN_H13S = (0x20, [   # [C] ave-13.5-h13s-data-pristine.bin (j314s cold dump, docs/87 §3)
    (0x051c18, 0x0000800000000000, 0x0),
    (0x140020, 0xf80000, 0x780000),
    (0x140120, 0x2eff, 0x2b3),
    (0x140130, 0x3fff, 0x28), (0x140138, 0x7fff, 0xa7),
    (0x140140, 0x3fff, 0x18), (0x140148, 0x7fff, 0x64),
    (0x140150, 0x3fff, 0x18), (0x140158, 0x7fff, 0x64),
    (0x140160, 0x3fff, 0x18), (0x140168, 0x7fff, 0x64),
    (0x140170, 0x3fff, 0x18), (0x140178, 0x7fff, 0x64),
    (0x140180, 0x3fff, 0x10), (0x140188, 0x7fff, 0x43),
    (0x140190, 0x3fff, 0x10), (0x140198, 0x7fff, 0x43),
    (0x1401a0, 0x3fff, 0x10), (0x1401a8, 0x7fff, 0x43),
    (0x145010, 0xc, 0xc),
    (0x1500d0, 0x1, 0x0),
])
TUN_H14G = (0x10, [   # [C] ave-13.5-h14g-data-pristine.bin (j473 cold dump, docs/90)
    (0x051c08, 0x10000000, 0x10000000),
    (0x051c18, 0x0010000000000000, 0x0010000000000000),
    (0x140020, 0x4000000000f80000, 0x4000000000780000),
    (0x140120, 0x2eff, 0x2b1),
    (0x140130, 0x3fff, 0x18), (0x140138, 0x7fff, 0x64),
    (0x140140, 0x3fff, 0x18), (0x140148, 0x7fff, 0x64),
    (0x140150, 0x3fff, 0x20), (0x140158, 0x7fff, 0x86),
    (0x140160, 0x3fff, 0x28), (0x140168, 0x7fff, 0xa7),
    (0x140170, 0x3fff, 0x10), (0x140178, 0x7fff, 0x43),
    (0x140180, 0x3fff, 0x10), (0x140188, 0x7fff, 0x43),
    (0x140190, 0x3fff, 0x18), (0x140198, 0x7fff, 0x64),
    (0x1401a0, 0x3fff, 0x20), (0x1401a8, 0x7fff, 0x86),
    (0x145010, 0x8000c, 0x8000c),
    (0x14a008, 0xff0ffff, 0x2520),
    (0x14a010, 0xff0ffff, 0x3002520),
])
# [C] ave-13.5-data-pristine.bin (j314c cold dump, docs/51): the same 21
# entries as H13S, with header h 0x11 instead of 0x20.
TUN_H13C = (0x11, TUN_H13S[1])

SOCS = {
    "t6000": dict(
        variant="H13S",
        image=["macos-13.5-j314s/ave_h13s.bin", "ave_h13s.bin"],
        pristine=["macos-13.5-j314s/ave-13.5-h13s-data-pristine.bin",
                  "ave-13.5-h13s-data-pristine.bin"],
        out="ave-13.5-h13s-data-pristine.bin",
        tags={"SOC_": (0x6000, "C"), "SOCR": (0x21, "C"), "CpAd": (0x40D800000, "C"),
              "WrAd": (0x40DC00000, "C"), "IOBA": (0x40C000000, "C")},
        stkg=(0xCC006678A98CE3D6, "C: the dump's boot"),
        tunables=(TUN_H13S, "C"),
        pinned="36d82853ea4648abe11a0a2b62499db5152af54a38d056e955eb1c4969f8cec7",
    ),
    "t8112": dict(
        variant="H14G",
        image=["macos-13.5-j473/ave_h14g.bin", "ave_h14g.bin"],
        pristine=["macos-13.5-j473/ave-13.5-h14g-data-pristine.bin",
                  "ave-13.5-h14g-data-pristine.bin"],
        out="ave-13.5-h14g-data-pristine.bin",
        tags={"SOC_": (0x8112, "C"), "SOCR": (0x11, "C"), "CpAd": (0x267800000, "C"),
              "WrAd": (0x267C00000, "C"), "IOBA": (0x0, "C")},
        stkg=(0x0C975400_35E6C44B, "C: the dump's boot"),
        tunables=(TUN_H14G, "C"),
        pinned="fe2d5ec1b2f92798e5847e0fc066fc95dbbdd5e3526651ba00216b45b7839f3d",
    ),
    "t6001": dict(
        variant="H13C",
        image=["macos-13.5/ave_h13c.bin", "macos-13.5-j314c/ave_h13c.bin", "ave_h13c.bin"],
        pristine=["ave-13.5-data-pristine.bin", "macos-13.5-j314c/ave-13.5-data-pristine.bin"],
        out="ave-13.5-data-pristine.bin",
        # ave0's banks; ave1 runs on a driver-owned copy with its own tags (docs/82)
        tags={"SOC_": (0x6001, "C"), "SOCR": (0x11, "C"), "CpAd": (0x40D800000, "C"),
              "WrAd": (0x40DC00000, "C"), "IOBA": (0x40C000000, "C")},
        stkg=(0x816EA533007323BC, "C: the dump's boot"),
        tunables=(TUN_H13C, "C"),
        pinned="f1af1ef42be04a7d607b0bc1a27dfda57268b476fecf1d92c8c13c760c71a103",
    ),
    "t8103": dict(
        variant="H13G",
        image=["macos-13.5-j473/ave_h13g.bin", "macos-13.5-j313/ave_h13g.bin",
               "macos-13.5-j274/ave_h13g.bin", "ave_h13g.bin"],
        pristine=[],
        out="ave-13.5-h13g-data-pristine.bin",
        # the j313 port (docs/89): driver/ave_soc.c row comment and its logs
        tags={"SOC_": (0x8103, "C on j313; I on j274 (same SoC)"),
              "SOCR": (0x11, "C on j313; I on j274 (B1, as every retail M1 known here)"),
              "CpAd": (0x267800000, "C: the ASC bank, j313 and the j274 overlay"),
              "WrAd": (0x267C00000, "C: ASC + 0x400000"),
              "IOBA": (0x0, "C on j313; I on j274")},
        # results/t8103-D2-*.kmsg: "STKG live 0x9fe20cd1e87caa00, blob 0x9fe20cd1e87caa00"
        stkg=(0x9FE20CD1E87CAA00, "C: the j313 blob's boot; any value works (I, docs/51)"),
        tunables=(None, "U"),
        # sha256 of the j313 cold dump (not published); pinned in driver/ave_soc.c
        pinned="ee25944a90f4714491fd4e98a08169500c39ddfd7eeee018f85a9405a5c73e57",
    ),
}


def sha(b):
    return hashlib.sha256(b).hexdigest()


def find_blob(blobs, cands, what):
    for c in cands:
        p = c if os.path.isabs(c) else os.path.join(blobs, c)
        if os.path.exists(p):
            return p
    sys.exit(f"missing {what}: none of {cands} under {blobs} (--blobs DIR)")


# --- Mach-O and tag list -----------------------------------------------------
def segments(img):
    if struct.unpack_from("<I", img, 0)[0] != 0xFEEDFACF:
        sys.exit("not a 64-bit Mach-O")
    ncmds = struct.unpack_from("<I", img, 16)[0]
    off, out = 32, {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", img, off)
        if cmd == 0x19:
            name = img[off + 8:off + 24].rstrip(b"\0").decode()
            out[name] = struct.unpack_from("<QQQQ", img, off + 24)  # vm, vmsize, fileoff, filesize
        off += size
    return out


def image_data(img):
    """__DATA as the core sees it before iBoot: file part + zero-filled rest."""
    vm, vs, fo, fs = segments(img)["__DATA"]
    d = bytearray(vs)
    d[:fs] = img[fo:fo + fs]
    return bytes(d), vm, fs


def walk_tags(d):
    """{name: (payload offset, len)} of the patchbay list (STKG .. IOSZ)."""
    i = d.find(b"GKTS\x08\x00\x00\x00")
    if i < 0:
        sys.exit("no STKG record in DATA")
    out = {}
    for _ in range(64):
        code, n = d[i:i + 4], struct.unpack_from("<I", d, i + 4)[0]
        if not all(0x20 <= c < 0x7F for c in code) or n > 0x100:
            break
        out[code[::-1].decode()] = (i + 8, n)
        if code == b"ZSOI":
            break
        i += 8 + n
    return out


class Fills:
    """iBoot's fill set for one image, derived from the image itself."""

    def __init__(self, img):
        self.data, self.vm, self.filesize = image_data(img)
        self.tags = walk_tags(self.data)
        for name, n in FILL_TAGS + (("TUNS", 8), ("TUNZ", 4)):
            if self.tags.get(name, (0, None))[1] != n:
                sys.exit(f"image tag list has no {name} of length {n}")
        tuns = int.from_bytes(self.val("TUNS", self.data), "little")
        self.tunz = int.from_bytes(self.val("TUNZ", self.data), "little")
        self.tun_off = tuns - self.vm
        hdr = self.data[self.tun_off:self.tun_off + 8]
        self.tun_hdr = hdr
        self.cap = hdr[2]
        if hdr[3] != 0 or hdr[4:8] != b"\xff" * 4 or 8 + 20 * self.cap > self.tunz:
            sys.exit(f"unexpected empty-table header {hdr.hex()} in the image")
        self.ranges = [(self.tags[n][0], self.tags[n][0] + l, n) for n, l in FILL_TAGS]
        self.ranges.append((self.tun_off, self.tun_off + self.tunz, "tunables"))
        self.stkg_off = self.tags["STKG"][0]

    def val(self, name, buf):
        o, n = self.tags[name]
        return buf[o:o + n]

    def in_fill(self, k):
        return any(s <= k < e for s, e, _ in self.ranges)

    def outside_diffs(self, blob):
        """Offsets where blob differs from the image outside the fill set."""
        return [k for k in range(len(self.data)) if blob[k] != self.data[k] and not self.in_fill(k)]

    def decode_tun(self, buf):
        o = self.tun_off
        ver, kind, cap, cnt, h = struct.unpack_from("<BBBBI", buf, o)
        ents = [struct.unpack_from("<IQQ", buf, o + 8 + 20 * i) for i in range(min(cnt, cap))]
        return (ver, kind, cap, cnt, h), ents

    def tun_ok(self, buf):
        (ver, kind, cap, cnt, h), _ = self.decode_tun(buf)
        o = self.tun_off
        rest = buf[o + 8 + 20 * cnt:o + 8 + 20 * cap]
        return (bytes(buf[o:o + 3]) == bytes(self.tun_hdr[:3]) and cnt <= cap and
                not any(rest) and
                bytes(buf[o + 8 + 20 * cap:o + self.tunz]) ==
                bytes(self.data[o + 8 + 20 * cap:o + self.tunz]))

    def write_tun(self, b, table):
        h, ents = table
        if len(ents) > self.cap:
            sys.exit(f"{len(ents)} tunables do not fit a table of {self.cap}")
        o = self.tun_off
        b[o + 3] = len(ents)
        struct.pack_into("<I", b, o + 4, h)
        for i, (off, mask, value) in enumerate(ents):
            struct.pack_into("<IQQ", b, o + 8 + 20 * i, off, mask, value)


def build(f, tags, stkg, table):
    b = bytearray(f.data)
    struct.pack_into("<Q", b, f.stkg_off, stkg)
    for name, n in FILL_TAGS[1:]:
        b[f.tags[name][0]:f.tags[name][0] + n] = tags[name].to_bytes(n, "little")
    if table is not None:
        f.write_tun(b, table)
    return bytes(b)


def show_fills(f, buf, title):
    print(title)
    for name, n in FILL_TAGS:
        o = f.tags[name][0]
        print(f"   {name}  DATA+{o:#07x}  len {n}  {int.from_bytes(buf[o:o + n], 'little'):#x}")
    (ver, kind, cap, cnt, h), ents = f.decode_tun(buf)
    print(f"   tunables DATA+{f.tun_off:#x} +{f.tunz:#x}: version {ver} kind {kind} "
          f"capacity {cap} count {cnt} h {h:#x}")
    for off, mask, value in ents:
        print(f"      +{off:#08x}  mask {mask:#018x}  value {value:#018x}")


# --- subcommands -------------------------------------------------------------
def cmd_fills(a):
    img = open(a.image, "rb").read()
    f = Fills(img)
    print(f"{a.image}: __DATA vm {f.vm:#x} +{len(f.data):#x}, file-backed {f.filesize:#x}")
    for s, e, n in f.ranges:
        print(f"   DATA+{s:#07x}..{e:#07x}  {e - s:4d} bytes  {n}")
    show_fills(f, f.data, "as shipped:")
    return 0


def check_real(f, real, name):
    """A real pristine blob differs from its image only inside the fill set."""
    out = f.outside_diffs(real)
    inside = sum(1 for k in range(len(f.data)) if real[k] != f.data[k] and f.in_fill(k))
    ok = not out and len(real) == len(f.data) and f.tun_ok(real)
    print(f"   {name}: {inside} byte(s) differ inside the fill set, {len(out)} outside"
          f"{'' if not out else ' (first DATA+%#x)' % out[0]}; tunables table well-formed: "
          f"{f.tun_ok(real)} -> {'PASS' if ok else 'FAIL'}")
    return ok


def cmd_diff(a):
    img = open(a.image, "rb").read()
    real = open(a.pristine, "rb").read()
    f = Fills(img)
    ok = check_real(f, real, os.path.basename(a.pristine))
    show_fills(f, real, "fills in the blob:")
    return 0 if ok else 1


def cmd_validate(a):
    ok = True
    skipped = []
    for soc in ("t6000", "t6001", "t8112"):
        s = SOCS[soc]
        paths = [next((os.path.join(a.blobs, c) for c in cands
                       if os.path.exists(os.path.join(a.blobs, c))), None)
                 for cands in (s["image"], s["pristine"])]
        if None in paths:
            print(f"\n{soc} ({s['variant']}): SKIPPED - its image or real pristine blob "
                  f"is not under {a.blobs}")
            skipped.append(soc)
            continue
        img = open(paths[0], "rb").read()
        real = open(paths[1], "rb").read()
        f = Fills(img)
        print(f"\n{soc} ({s['variant']}): pristine sha256 {sha(real)[:16]}..., "
              f"pinned {s['pinned'][:16]}...: {'same' if sha(real) == s['pinned'] else 'DIFFERENT'}")
        ok &= sha(real) == s["pinned"]
        # 1. the fill set is complete
        ok &= check_real(f, real, "real blob vs image")
        # 2. rebuilt from the recorded values: identical
        tags = {k: v for k, (v, _) in s["tags"].items()}
        same = build(f, tags, s["stkg"][0], s["tunables"][0])
        r1 = same == real
        print(f"   rebuilt with the recorded STKG: {'IDENTICAL' if r1 else 'DIFFERS'} "
              f"(sha256 {sha(same)[:16]}...)")
        # 3. with another STKG: differs in STKG only
        other = build(f, tags, 0x1122334455667700, s["tunables"][0])
        d = [k for k in range(len(real)) if other[k] != real[k]]
        r2 = bool(d) and all(f.stkg_off <= k < f.stkg_off + 8 for k in d)
        print(f"   rebuilt with another STKG: {len(d)} byte(s) differ, "
              f"{'all in STKG' if r2 else 'NOT ONLY STKG'}")
        ok &= r1 and r2
        # 4. negative controls: the test must be able to say no
        wrong = TUN_H14G if s["tunables"][0] is not TUN_H14G else TUN_H13S
        c1 = build(f, tags, s["stkg"][0], wrong) != real
        t2 = dict(tags, CpAd=tags["CpAd"] + 0x1000)
        c2 = build(f, t2, s["stkg"][0], s["tunables"][0]) != real
        c3 = build(f, tags, s["stkg"][0], None) != real
        print(f"   controls (other SoC's tunables / wrong CpAd / no tunables) rejected: "
              f"{c1} / {c2} / {c3}")
        ok &= c1 and c2 and c3

    # t8103: what can and cannot be said without a dump
    s = SOCS["t8103"]
    p = None
    for c in s["image"]:
        q = os.path.join(a.blobs, c)
        if os.path.exists(q):
            p = q
            break
    if p:
        f = Fills(open(p, "rb").read())
        tags = {k: v for k, (v, _) in s["tags"].items()}
        print(f"\nt8103 (H13G) from {os.path.relpath(p, a.blobs)}:")
        for s0, e, n in f.ranges:
            print(f"   fill {n:9s} DATA+{s0:#07x}..{e:#07x}")
        for name, table in (("H13S", TUN_H13S), ("none", None)):
            b = build(f, tags, s["stkg"][0], table)
            n = sum(1 for k in range(len(b)) if b[k] != f.data[k])
            print(f"   known tags + j313 STKG + {name} tunables: {n} byte(s) differ from the "
                  f"image (the j313 dump: 146); sha256 {'MATCHES' if sha(b) == s['pinned'] else 'is not'} "
                  f"the pinned j313 blob")
        print("   -> H13G's tunables are not H13S's (given the tags, STKG and a zero bss); "
              "they stay UNKNOWN [U]")
    if skipped and len(skipped) == 3:
        ok = False
        print("\nnothing to validate: no image with its real pristine blob was found")
    print("\n" + ("VALIDATE: ALL PASS" if ok else "VALIDATE: FAILED")
          + (f" (skipped: {', '.join(skipped)})" if skipped else ""))
    return 0 if ok else 1


def page_survived(f, dump, page):
    """True if every byte of this DATA page outside the fill set equals the image."""
    lo, hi = page, min(page + PAGE, len(dump), len(f.data))
    if hi <= lo:
        return False
    return all(dump[k] == f.data[k] for k in range(lo, hi) if not f.in_fill(k))


def dump_report(f, s, dump):
    """Survival of the fill pages; the fills that can be taken from the dump."""
    print(f"dump: {len(dump):#x} bytes of DATA ({len(f.data):#x})")
    npages = (min(len(dump), len(f.data)) + PAGE - 1) // PAGE
    good = [i for i in range(npages) if page_survived(f, dump, i * PAGE)]
    content = [i for i in range(npages) if any(f.data[i * PAGE:(i + 1) * PAGE])]
    print(f"   {len(good)} of {npages} 16K pages equal the image outside the fill set; "
          f"of the {len(content)} pages the image does not ship as zero, "
          f"{len(set(good) & set(content))} (a zero page matches a zero page trivially)")
    tag_page = f.stkg_off & ~(PAGE - 1)
    tun_page = f.tun_off & ~(PAGE - 1)
    if (f.tun_off + f.tunz - 1) & ~(PAGE - 1) != tun_page or \
       (f.tags["IOBA"][0] + 8 - 1) & ~(PAGE - 1) != tag_page:
        sys.exit("fill ranges straddle a page; extend page_survived")
    tags_ok = page_survived(f, dump, tag_page)
    tun_ok = page_survived(f, dump, tun_page) and f.tun_ok(dump)
    print(f"   tag list page DATA+{tag_page:#x}: {'SURVIVED' if tags_ok else 'overwritten'}")
    print(f"   tunables page DATA+{tun_page:#x}: {'SURVIVED' if tun_ok else 'overwritten'}"
          f"{'' if tun_ok or not page_survived(f, dump, tun_page) else ' (page matches but the table is malformed)'}")
    got = {}
    if tags_ok:
        show_fills(f, dump, "   fills in the dump:")
        for name, n in FILL_TAGS[1:]:
            v = int.from_bytes(f.val(name, dump), "little")
            want = s["tags"][name][0]
            mark = "as recorded" if v == want else f"RECORDED {want:#x}"
            print(f"   {name} {v:#x}: {mark}")
            got[name] = v
        got["STKG"] = int.from_bytes(f.val("STKG", dump), "little")
    if tun_ok:
        h, ents = f.decode_tun(dump)
        got["tunables"] = (h[4], ents)
        if not tags_ok:
            show_fills(f, dump, "   fills in the dump (tag page overwritten, tags not used):")
    return got


def cmd_dump_check(a):
    s = SOCS[a.soc]
    f = Fills(open(a.image or find_blob(a.blobs, s["image"], "image"), "rb").read())
    dump_report(f, s, open(a.dump, "rb").read())
    return 0


def cmd_build(a):
    s = SOCS[a.soc]
    img_path = a.image or find_blob(a.blobs, s["image"], f"{s['variant']} image")
    img = open(img_path, "rb").read()
    f = Fills(img)
    tags = {k: v for k, (v, _) in s["tags"].items()}
    stkg, stkg_src = s["stkg"]
    table, tun_src = s["tunables"]
    prov = {k: f"recorded [{m}]" for k, (_, m) in s["tags"].items()}

    if a.tunables == "dump" or a.dump:
        if not a.dump:
            sys.exit("--tunables dump needs --dump DATA.bin")
        got = dump_report(f, s, open(a.dump, "rb").read())
        for name in ("SOC_", "CpAd", "WrAd", "IOBA"):
            if name in got and got[name] != tags[name]:
                sys.exit(f"REFUSED: the dump's {name} {got[name]:#x} is not the recorded "
                         f"{tags[name]:#x} the driver checks; is this {a.soc}'s DATA?")
        if "SOCR" in got:
            tags["SOCR"] = got["SOCR"]
            prov["SOCR"] = "this machine's dump [C]"
            for name in ("SOC_", "CpAd", "WrAd", "IOBA"):
                prov[name] = "recorded, confirmed by the dump [C]"
            stkg, stkg_src = got["STKG"], "this machine's dump [C]"
        if a.tunables in (None, "dump"):
            if "tunables" not in got:
                sys.exit("REFUSED: the tunables page did not survive in the dump; "
                         "pick --tunables none|h13s explicitly (docs/89 §6)")
            table, tun_src = got["tunables"], "C: this machine's dump"
    if a.tunables == "none":
        table, tun_src = None, "I: none - the image's empty table, count 0"
    elif a.tunables == "h13s":
        table, tun_src = TUN_H13S, "U: borrowed from t6000 (H13S)"
    elif a.tunables == "h14g":
        table, tun_src = TUN_H14G, "U: borrowed from t8112 (H14G)"
    elif a.tunables == "recorded" and table is None:
        sys.exit(f"{a.soc}: no recorded tunables [U]; use --tunables dump|none|h13s")
    if table is None and a.tunables is None and a.soc == "t8103":
        sys.exit("t8103: the tunables are UNKNOWN; choose --tunables dump (with --dump), "
                 "none or h13s (docs/89 §6)")
    if a.stkg is not None:
        stkg, stkg_src = a.stkg, "--stkg"

    blob = build(f, tags, stkg, table)
    out_diff = f.outside_diffs(blob)
    if out_diff or not f.tun_ok(blob):
        sys.exit("internal error: the built blob differs from the image outside the fill set")
    n = sum(1 for k in range(len(blob)) if blob[k] != f.data[k])
    out = a.out or os.path.join(a.blobs, s["out"])
    with open(out, "wb") as fh:
        fh.write(blob)
    print(f"\n{a.soc} ({s['variant']}) from {img_path}")
    print(f"   image sha256 {sha(img)}")
    print(f"   STKG  {stkg:#018x}  {stkg_src}")
    for name, _ in FILL_TAGS[1:]:
        print(f"   {name}  {tags[name]:#x}  {prov[name]}")
    nt = 0 if table is None else len(table[1])
    print(f"   tunables: {nt} entr{'y' if nt == 1 else 'ies'}  [{tun_src}]")
    print(f"   {n} byte(s) differ from the image, all inside the fill set")
    print(f"wrote {out}: {len(blob):#x} bytes, sha256 {sha(blob)}")
    if sha(blob) == s.get("pinned"):
        print("   = the pinned blob (driver/ave_soc.c)")
    else:
        print("   not the pinned blob: the driver takes it for owned DATA only "
              "(soc->iboot.blob_by_image), after checking it against the image")
    print(f"install: sudo install -m644 {out} /lib/firmware/apple/{s['out']}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="See the module docstring (--help shows only the options).")
    ap.add_argument("--blobs", default=os.path.join(REPO, "data/blobs"),
                    help="where the images and blobs are (default: data/blobs)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fills")
    p.add_argument("image")
    p = sub.add_parser("diff")
    p.add_argument("image")
    p.add_argument("pristine")
    sub.add_parser("validate")
    p = sub.add_parser("dump-check")
    p.add_argument("--soc", required=True, choices=sorted(SOCS))
    p.add_argument("--image")
    p.add_argument("dump")
    p = sub.add_parser("build")
    p.add_argument("--soc", required=True, choices=sorted(SOCS))
    p.add_argument("--image", help="the firmware image (default: searched under --blobs)")
    p.add_argument("--tunables", choices=("recorded", "dump", "none", "h13s", "h14g"))
    p.add_argument("--dump", help="a DATA dump of this machine (data_phys, data_size)")
    p.add_argument("--stkg", type=lambda x: int(x, 0))
    p.add_argument("-o", "--out")
    a = ap.parse_args()
    return {"fills": cmd_fills, "diff": cmd_diff, "validate": cmd_validate,
            "dump-check": cmd_dump_check, "build": cmd_build}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
