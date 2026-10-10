#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the AVE driver's install-time data from the user's own Apple files.

The driver source carries no data taken from Apple's binaries. What it needs
from them is made on the user's machine, from the user's copy, by this tool
(and by apple-ave-fetch-firmware, which imports it), and loaded by the driver
with request_firmware():

  apple/ave-13.5-dpe-<set>.bin   the AVE_DPE register tunables macOS applies
                                 at every power-on (docs/58 §5.1, §7.1), read
                                 from AppleAVE2.kext's gs_sAVE_DPE_CfgSet_<set>
                                 in a macOS 13.5 kernelcache

The pristine DATA blob comes from the machine itself instead: the driver
copies the firmware's DATA before the first start of a boot and exposes it
(docs/100); `check_pristine` here verifies such a copy against the image.

Everything is stdlib Python. A kernelcache IM4P is LZFSE-compressed: it is
decompressed with Python's `lzfse` module if present, else the `lzfse`
command (Apple's reference implementation, BSD-3-Clause; Fedora: lzfse).

  ave_fwgen.py dpe KERNELCACHE --set Castor_6000 -o ave-13.5-dpe-castor_6000.bin
  ave_fwgen.py dpe KERNELCACHE --list
  ave_fwgen.py dpe-dump FILE              print a generated DPE file
  ave_fwgen.py check-pristine IMAGE BLOB --soc t6001 [--inst N]
"""
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys

# ---------------------------------------------------------------------------
# Image4 (DER): IM4P = SEQUENCE { IA5String "IM4P", IA5String type,
# IA5String description, OCTET STRING payload, [OCTET STRING keybags],
# [SEQUENCE { INTEGER algorithm, INTEGER size } compression] };
# IMG4 = SEQUENCE { IA5String "IMG4", IM4P, [0] IM4M, ... }.
# ---------------------------------------------------------------------------


class Refused(Exception):
    """Input that is not what it should be; nothing was written."""


class DERError(Refused):
    pass


def der_read(buf, off, end=None):
    """One DER element at off: (tag, value_start, value_end)."""
    end = len(buf) if end is None else end
    if off + 2 > end:
        raise DERError("truncated DER element")
    tag = buf[off]
    if tag & 0x1F == 0x1F:
        raise DERError("multi-byte DER tags are not used in Image4")
    n = buf[off + 1]
    off += 2
    if n & 0x80:
        k = n & 0x7F
        if k == 0:
            raise DERError("indefinite DER length")
        if k > 4 or off + k > end:
            raise DERError("bad DER length")
        n = int.from_bytes(buf[off:off + k], "big")
        off += k
    if off + n > end:
        raise DERError("DER element runs past its container")
    return tag, off, off + n


def der_children(buf, start, end):
    """[(tag, element_start, value_start, value_end)] of a constructed value."""
    out = []
    off = start
    while off < end:
        tag, vs, ve = der_read(buf, off, end)
        out.append((tag, off, vs, ve))
        off = ve
    return out


def der_int(buf, k):
    if k[0] != 0x02:
        raise DERError("expected an INTEGER")
    return int.from_bytes(buf[k[2]:k[3]], "big")


def im4p_parse(buf, depth=0):
    """(type, payload, compression) of an IM4P, or of the IM4P in an IMG4.

    compression is None or (algorithm, uncompressed size); algorithm 1 is LZFSE.
    """
    buf = bytes(buf)
    tag, vs, ve = der_read(buf, 0)
    if tag != 0x30:
        raise DERError("not an Image4 file (no outer SEQUENCE)")
    kids = der_children(buf, vs, ve)
    if not kids or kids[0][0] != 0x16:
        raise DERError("not an Image4 file (no magic)")
    magic = buf[kids[0][2]:kids[0][3]]
    if magic == b"IMG4":
        if depth or len(kids) < 2 or kids[1][0] != 0x30:
            raise DERError("IMG4 without an IM4P")
        return im4p_parse(buf[kids[1][1]:kids[1][3]], depth + 1)
    if magic != b"IM4P":
        raise DERError(f"unknown Image4 magic {magic!r}")
    if len(kids) < 4 or [k[0] for k in kids[:4]] != [0x16, 0x16, 0x16, 0x04]:
        raise DERError("IM4P is not {IM4P, type, description, payload}")
    fourcc = buf[kids[1][2]:kids[1][3]].decode("ascii", "replace")
    payload = buf[kids[3][2]:kids[3][3]]
    comp = None
    for k in kids[4:]:
        if k[0] == 0x04:
            raise Refused("the IM4P payload is encrypted (it has keybags)")
        if k[0] == 0x30:
            ck = der_children(buf, k[2], k[3])
            if len(ck) != 2:
                raise DERError("bad IM4P compression info")
            comp = (der_int(buf, ck[0]), der_int(buf, ck[1]))
    if comp is None and (payload[:4] in (b"bvx2", b"bvx1", b"bvxn", b"bvx-")
                         or payload[:8] == b"complzss"):
        raise Refused("the IM4P payload is compressed but says nothing about it")
    return fourcc, payload, comp


def im4p_payload(buf):
    """(type, payload) of an uncompressed IM4P/IMG4 (the AVE firmware)."""
    fourcc, payload, comp = im4p_parse(buf)
    if comp is not None:
        raise Refused("the IM4P payload is compressed; the AVE firmware is not")
    return fourcc, payload


def lzfse_decode(data, size):
    """LZFSE -> bytes of exactly size, with the lzfse module or command."""
    try:
        import lzfse  # optional
        out = lzfse.decompress(data)
    except ImportError:
        exe = shutil.which("lzfse")
        if not exe:
            raise Refused("a kernelcache is LZFSE-compressed: install lzfse "
                          "(dnf install lzfse) or the Python lzfse module")
        p = subprocess.run([exe, "-decode"], input=data, capture_output=True, check=False)
        if p.returncode:
            raise Refused(f"lzfse -decode failed: {p.stderr.decode(errors='replace').strip()}")
        out = p.stdout
    if len(out) != size:
        raise Refused(f"LZFSE output is {len(out)} bytes, the IM4P says {size}")
    return out


def kernelcache_macho(buf):
    """The kernelcache Mach-O from an IM4P (LZFSE) or an already raw Mach-O."""
    if buf[:4] == b"\xcf\xfa\xed\xfe":
        return bytes(buf)
    fourcc, payload, comp = im4p_parse(buf)
    if fourcc not in ("krnl",):
        raise Refused(f"IM4P type {fourcc!r} is not a kernelcache")
    if comp is None:
        out = payload
    elif comp[0] == 1:
        out = lzfse_decode(payload, comp[1])
    else:
        raise Refused(f"unknown IM4P compression {comp[0]}")
    if out[:4] != b"\xcf\xfa\xed\xfe":
        raise Refused("the kernelcache payload is not a 64-bit Mach-O")
    return out


# ---------------------------------------------------------------------------
# Mach-O
# ---------------------------------------------------------------------------
LC_SEGMENT_64, LC_SYMTAB, LC_FILESET_ENTRY = 0x19, 0x02, 0x35
LC_REQ_DYLD = 0x80000000


def load_commands(d, base=0):
    if len(d) < base + 32 or struct.unpack_from("<I", d, base)[0] != 0xFEEDFACF:
        raise Refused("not a 64-bit Mach-O")
    ncmds, sizeofcmds = struct.unpack_from("<II", d, base + 16)
    off = base + 32
    end = off + sizeofcmds
    for _ in range(ncmds):
        if off + 8 > min(end, len(d)):
            raise Refused("truncated Mach-O load commands")
        cmd, size = struct.unpack_from("<II", d, off)
        if size < 8 or off + size > len(d):
            raise Refused("bad Mach-O load command")
        yield cmd, off, size
        off += size


def segments(d, base=0):
    """{name: (vmaddr, vmsize, fileoff, filesize)} of a Mach-O at base."""
    out = {}
    for cmd, off, size in load_commands(d, base):
        if cmd == LC_SEGMENT_64 and size >= 72:
            name = d[off + 8:off + 24].rstrip(b"\0").decode("ascii", "replace")
            out[name] = struct.unpack_from("<QQQQ", d, off + 24)
    return out


class Kernelcache:
    """An MH_FILESET kernelcache: VA lookup and one kext's symbols."""

    def __init__(self, d):
        self.d = d
        self.segs = list(segments(d).values())
        if not self.segs:
            raise Refused("the kernelcache has no segments")
        self.base = min(va for va, vs, fo, fs in self.segs if fs)

    def off(self, va, n=1):
        for sva, vs, fo, fs in self.segs:
            if sva <= va and va + n <= sva + min(vs, fs):
                return fo + (va - sva)
        raise Refused(f"VA {va:#x}+{n:#x} is not file-backed in the kernelcache")

    def kext(self, bundle):
        for cmd, off, size in load_commands(self.d):
            if (cmd & ~LC_REQ_DYLD) == LC_FILESET_ENTRY:
                vmaddr, fileoff, stroff = struct.unpack_from("<QQI", self.d, off + 8)
                end = self.d.find(b"\0", off + stroff, off + size)
                if self.d[off + stroff:end].decode("ascii", "replace") == bundle:
                    return fileoff
        raise Refused(f"no {bundle} in this kernelcache")

    def symbols(self, kext_off):
        """{name: va} from the kext's LC_SYMTAB (offsets are kernelcache-wide)."""
        for cmd, off, size in load_commands(self.d, kext_off):
            if cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", self.d, off + 8)
                break
        else:
            raise Refused("the kext has no symbol table")
        if symoff + 16 * nsyms > len(self.d) or stroff + strsize > len(self.d):
            raise Refused("the kext's symbol table is out of bounds")
        out = {}
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, value = struct.unpack_from("<IBBHQ", self.d, symoff + 16 * i)
            if strx >= strsize:
                continue
            s = stroff + strx
            e = self.d.find(b"\0", s, stroff + strsize)
            if e > s:
                out[self.d[s:e].decode("utf-8", "replace")] = value
        return out

    def pointer(self, raw):
        """A chained-fixup pointer in a kernelcache (DYLD_CHAINED_PTR_64_KERNEL_CACHE)."""
        if (raw >> 30) & 3:
            raise Refused(f"pointer {raw:#x} names another cache level")
        return self.base + (raw & 0x3FFFFFFF)


# ---------------------------------------------------------------------------
# AVE_DPE tunables
#
# gs_sAVE_DPE_CfgSet_<set> is {u64 x4 (the CAT and CAC windows), then six
# {pointer, count} pairs}; slot 0 is CAT Default, 3 CAC Default, 4 CAC 8-bit
# (docs/58 §5.1). An entry is {u32 offset, u32 width, u32 clear, u32 set},
# applied by AVE_DPE::ApplyTunables as a read-modify-write.
# ---------------------------------------------------------------------------
DPE_CAT_BASE = 0xDC000
DPE_CAC_BASE = 0xDC400
DPE_SLOTS = (("cat_default", 0), ("cac_default", 3), ("cac_8bit", 4))
DPE_MAGIC = b"AVEDPE01"
DPE_SETS = ("Castor_6000", "Acis_8103", "Atlas_8112")
KEXT = "com.apple.driver.AppleAVE2"


def dpe_file_name(setname):
    return f"apple/ave-13.5-dpe-{setname.lower()}.bin"


def dpe_tables(kc, setname):
    """{'cat_default': [(off, clear, set)], 'cac_default': [...], 'cac_8bit': [...]}"""
    syms = kc.symbols(kc.kext(KEXT))
    want = "gs_sAVE_DPE_CfgSet_" + setname
    hits = [va for name, va in syms.items() if name.endswith(want)
            and name[:-len(want)].lstrip("_ZL0123456789") == ""]
    if len(hits) != 1:
        raise Refused(f"{KEXT} has {len(hits)} symbol(s) for {want}")
    o = kc.off(hits[0], 0x80)
    hdr = struct.unpack_from("<4Q", kc.d, o)
    if (hdr[0] & 0xFFFFFFFF) != DPE_CAT_BASE or (hdr[2] & 0xFFFFFFFF) != DPE_CAC_BASE:
        raise Refused(f"{want}: CAT/CAC windows {hdr[0]:#x}/{hdr[2]:#x} are not the driver's")
    out = {}
    for key, slot in DPE_SLOTS:
        raw, n = struct.unpack_from("<QQ", kc.d, o + 0x20 + 16 * slot)
        if n > 512:
            raise Refused(f"{want}: {key} has an implausible {n} entries")
        ents = []
        if n:
            to = kc.off(kc.pointer(raw), 16 * n)
            for i in range(n):
                off, width, clear, value = struct.unpack_from("<4I", kc.d, to + 16 * i)
                if width != 4 or off & 3 or off >= 0x400:
                    raise Refused(f"{want}: {key}[{i}] = {off:#x}/{width} is not a 32-bit register")
                ents.append((off, clear, value))
        out[key] = ents
    return out


def dpe_encode(t):
    keys = [k for k, _ in DPE_SLOTS]
    b = bytearray(DPE_MAGIC)
    b += struct.pack("<4I", *(len(t[k]) for k in keys), 0)
    for k in keys:
        for e in t[k]:
            b += struct.pack("<3I", *e)
    return bytes(b)


def dpe_decode(b):
    keys = [k for k, _ in DPE_SLOTS]
    if len(b) < 24 or b[:8] != DPE_MAGIC:
        raise Refused("not an AVE DPE tunables file")
    counts = struct.unpack_from("<3I", b, 8)
    if len(b) != 24 + 12 * sum(counts):
        raise Refused("DPE tunables file size does not match its counts")
    out, off = {}, 24
    for k, n in zip(keys, counts):
        out[k] = [struct.unpack_from("<3I", b, off + 12 * i) for i in range(n)]
        off += 12 * n
    return out


# ---------------------------------------------------------------------------
# Pristine DATA: a copy of the firmware's DATA as iBoot left it, checked
# against the image: equal outside iBoot's fill set (the tag payloads and the
# tunables table), the table well-formed, the address tags this encoder's.
# ---------------------------------------------------------------------------
FILL_TAGS = (("STKG", 8), ("SOC_", 4), ("SOCR", 4), ("CpAd", 8), ("WrAd", 8), ("IOBA", 8))

# Each encoder's own banks (CpAd = the ASC bank, WrAd = CpAd + 0x400000,
# IOBA = the fabric bank or 0): register addresses, from the device tree.
ENCODER_TAGS = {
    ("t6000", 0): (0x6000, 0x40D800000, 0x40C000000),
    ("t6001", 0): (0x6001, 0x40D800000, 0x40C000000),
    ("t6001", 1): (0x6001, 0x507800000, 0x506000000),
    ("t8103", 0): (0x8103, 0x267800000, 0x0),
    ("t8112", 0): (0x8112, 0x267800000, 0x0),
    ("t6002", 0): (None, 0x40D800000, 0x40C000000),
    ("t6002", 1): (None, 0x507800000, 0x506000000),
    ("t6002", 2): (None, 0x240D800000, 0x240C000000),
    ("t6002", 3): (None, 0x2507800000, 0x2506000000),
}


def image_data(img):
    """__DATA as the core sees it before iBoot (file part, then zeros), and its vmaddr."""
    seg = segments(img).get("__DATA")
    if not seg:
        raise Refused("the firmware image has no __DATA segment")
    vm, vs, fo, fs = seg
    if fo + fs > len(img) or fs > vs or vs > 0x1000000:
        raise Refused("the firmware image's __DATA is out of bounds")
    d = bytearray(vs)
    d[:fs] = img[fo:fo + fs]
    return d, vm


def walk_tags(d):
    """{name: (payload offset, length)} of the RTKit patchbay (STKG .. IOSZ)."""
    i = bytes(d).find(b"GKTS\x08\x00\x00\x00")
    if i < 0:
        raise Refused("no STKG record in the image's __DATA")
    out = {}
    for _ in range(64):
        if i + 8 > len(d):
            break
        code, n = bytes(d[i:i + 4]), struct.unpack_from("<I", d, i + 4)[0]
        if not all(0x20 <= c < 0x7F for c in code) or n > 0x100:
            break
        out[code[::-1].decode()] = (i + 8, n)
        if code == b"ZSOI":
            break
        i += 8 + n
    return out


def fill_layout(img):
    """(image __DATA, tags, tunables offset, tunables size) of a firmware image."""
    d, vm = image_data(img)
    tags = walk_tags(d)
    for name, n in FILL_TAGS + (("TUNS", 8), ("TUNZ", 4)):
        if tags.get(name, (0, None))[1] != n:
            raise Refused(f"the image's tag list has no {name} of length {n}")
    o = tags["TUNS"][0]
    tun = int.from_bytes(d[o:o + 8], "little") - vm
    tunz = int.from_bytes(d[tags["TUNZ"][0]:tags["TUNZ"][0] + 4], "little")
    if not (0 <= tun and tunz >= 8 and tun + tunz <= len(d)):
        raise Refused("the image's tunables table is outside __DATA")
    return d, tags, tun, tunz


def build_pristine(img, fills):
    """Image __DATA with iBoot's fill values written in; tunables None = the empty table."""
    d, tags, tun, tunz = fill_layout(img)
    for name, n in FILL_TAGS:
        o = tags[name][0]
        d[o:o + n] = fills[name].to_bytes(n, "little")
    if fills.get("tunables") is not None:
        cap = d[tun + 2]
        if d[tun + 3] != 0 or bytes(d[tun + 4:tun + 8]) != b"\xff" * 4 or 8 + 20 * cap > tunz:
            raise Refused("the image's tunables table is not the empty one expected")
        h, ents = fills["tunables"]
        if len(ents) > cap:
            raise Refused("the tunables do not fit the image's table")
        d[tun + 3] = len(ents)
        struct.pack_into("<I", d, tun + 4, h)
        for k, (off, mask, value) in enumerate(ents):
            struct.pack_into("<IQQ", d, tun + 8 + 20 * k, off, mask, value)
    return bytes(d)


def check_pristine(img, blob, soc, inst=0):
    """Raise Refused unless blob is img's DATA plus a well-formed iBoot fill set
    for this encoder. Returns the fill values found (for the log)."""
    d, tags, tun, tunz = fill_layout(img)
    if len(blob) != len(d):
        raise Refused(f"the DATA copy is {len(blob):#x} bytes, the image's DATA {len(d):#x}")
    fill = set()
    for name, n in FILL_TAGS:
        fill.update(range(tags[name][0], tags[name][0] + n))
    fill.update(range(tun, tun + tunz))
    diff = [k for k in range(len(d)) if blob[k] != d[k] and k not in fill]
    if diff:
        raise Refused(f"{len(diff)} byte(s) outside iBoot's fill set differ from the image "
                      f"(first at DATA+{diff[0]:#x}): not this image's cold DATA")
    cap, cnt = blob[tun + 2], blob[tun + 3]
    if bytes(blob[tun:tun + 3]) != bytes(d[tun:tun + 3]) or cnt > cap or 8 + 20 * cap > tunz \
            or any(blob[tun + 8 + 20 * cnt:tun + 8 + 20 * cap]):
        raise Refused("the tunables table is not well formed")
    val = {name: int.from_bytes(blob[tags[name][0]:tags[name][0] + n], "little")
           for name, n in FILL_TAGS}
    want = ENCODER_TAGS.get((soc, inst))
    if want is None:
        raise Refused(f"no encoder {inst} on {soc}")
    socid, cpad, ioba = want
    if (val["CpAd"], val["WrAd"], val["IOBA"]) != (cpad, cpad + 0x400000, ioba) or \
            (socid is not None and val["SOC_"] != socid):
        raise Refused(f"the copy's SOC_ {val['SOC_']:#x} CpAd {val['CpAd']:#x} IOBA "
                      f"{val['IOBA']:#x} are not {soc} encoder {inst}'s")
    val["tunables"] = cnt
    return val


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("dpe", help="AVE_DPE tunables from a kernelcache")
    p.add_argument("kernelcache", help="kernelcache.release.* (IM4P) or its Mach-O")
    p.add_argument("--set", choices=DPE_SETS)
    p.add_argument("--list", action="store_true")
    p.add_argument("-o", "--out")
    p = sub.add_parser("dpe-dump")
    p.add_argument("file")
    p = sub.add_parser("check-pristine")
    p.add_argument("image")
    p.add_argument("blob")
    p.add_argument("--soc", required=True)
    p.add_argument("--inst", type=int, default=0)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "dpe":
            kc = Kernelcache(kernelcache_macho(open(a.kernelcache, "rb").read()))
            if a.list:
                for name in sorted(kc.symbols(kc.kext(KEXT))):
                    if "gs_sAVE_DPE_CfgSet_" in name:
                        print(name)
                return 0
            if not a.set:
                ap.error("--set or --list")
            b = dpe_encode(dpe_tables(kc, a.set))
            out = a.out or dpe_file_name(a.set).split("/")[-1]
            with open(out, "wb") as f:
                f.write(b)
            print(f"wrote {out}: {len(b)} bytes, sha256 {hashlib.sha256(b).hexdigest()}")
        elif a.cmd == "dpe-dump":
            t = dpe_decode(open(a.file, "rb").read())
            for k, ents in t.items():
                base = DPE_CAT_BASE if k.startswith("cat") else DPE_CAC_BASE
                for off, clear, value in ents:
                    print(f"{k:12s} {base + off:#07x} clear {clear:#010x} set {value:#010x}")
        elif a.cmd == "check-pristine":
            v = check_pristine(open(a.image, "rb").read(), open(a.blob, "rb").read(),
                               a.soc, a.inst)
            print("OK: " + ", ".join(f"{k} {v[k]:#x}" for k in v))
    except (Refused, OSError) as e:
        print(f"ave_fwgen: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
