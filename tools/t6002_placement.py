#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""t6002 (M1 Ultra): the driver's iBoot placement block from the live ADT.

docs/98 U0. Where iBoot put the AVE firmware is per machine and boot chain,
so the t6002 rows in driver/ave_soc.c read it from a generated block
(BEGIN/END t6002-placement). This tool fills that block from what
test/physdump.ko copied out of the running machine, read-only:

  adt.bin           physdump adt=1: the live ADT (m1n1's phram node "adt")
  aveN-text.bin     physdump seg=N: TEXT as iBoot left it (segment iova 0)
  aveN-data.bin     physdump seg=N: DATA as iBoot left it (iova 0xec000)

What it does, refusing at the first thing that is not as expected:
  1. parses the live ADT itself (no m1n1 dependency): /arm-io/ave0..3 reg,
     sve-id, pre-loaded, segment-ranges; dart-ave0..3's live DAPF filter;
  2. checks every encoder's reg against the j375d restore ADT's (docs/98 §2);
  3. ave0 must have a TEXT (iova 0, 0xec000) and a DATA (iova 0xec000,
     0x134000) segment. ave1-3: their own segments if iBoot loaded any;
     otherwise TEXT = ave0's (t6001's ave1 RVBAR points there, docs/82 A2;
     the driver's stage-10 RVBAR check is what confirms it) and DATA 0, the
     driver-owned copy with the encoder's own CpAd/WrAd/IOBA tags;
  4. with dumps: TEXT must be the H13D image byte for byte except iBoot's
     DATA literal at 0x423c, which must be 0x1f0000ec000; DATA must be the
     image outside the three iBoot-filled ranges (docs/51) and zero past the
     file-backed part - i.e. COLD, no core has run on it. Its tags are
     printed and checked against the encoder's own banks: a die-1 DATA
     whose CpAd/WrAd/IOBA are die 0's numbers means the firmware addresses
     its block die-locally, and then the owned-DATA rows (which would put
     global die-1 addresses in) are withheld;
  5. writes the pristine blob(s) (the cold DATA, as docs/87 §3 did) and
     prints/applies the block.

  python3 tools/t6002_placement.py --adt D/adt.bin --dumps D [--apply driver/ave_soc.c]
  python3 tools/t6002_placement.py --selftest      # offline: a synthetic ADT + dumps

Nothing here touches hardware. Blobs it writes are Apple-derived: keep them
out of git (data/blobs/ is ignored).
"""
import argparse, hashlib, os, re, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEXT_SIZE = 0xEC000
DATA_SIZE = 0x134000
DATA_DVA = 0xEC000
DATA_FILE_OFF = 0xF0000           # in ave_h13d.bin (same as H13C)
DATA_FILE_SIZE = 0x64000
TEXT_FILE_OFF = 0x4000
LITERAL_OFF = 0x423C
LITERAL = 0x1F0000EC000
TEXT_WINDOWS = (0x0, 0x80000, 0xE8000)
H13D_SHA = "dfa0dac9620f6ebcf1a2531309d572b2108f3b8eadad6909cb162b79355251ee"
# docs/51 / tools/make_ave_data_blob.py: what iBoot fills in, DATA offsets.
# H13D's patchbay and tunables regions are H13C's byte for byte (docs/98 §3).
IBOOT_RANGES = [(0x3A38, 0x3A40), (0x3BA3, 0x3BE9), (0x6036B, 0x60505)]
TAG_OFF = {"CpAd": 0x3BBB, "WrAd": 0x3BCB, "IOBA": 0x3BE4}   # value offsets, ave_fw.c

# j375d restore ADT (docs/98 §2): /arm-io/aveN reg[0] (bus) and each
# encoder's own banks in CPU physical (ASC, ASC + 0x400000, axi2af)
ENC = {
    0: dict(bus=0x20D100000, cpad=0x40D800000, wrad=0x40DC00000, ioba=0x40C000000),
    1: dict(bus=0x307100000, cpad=0x507800000, wrad=0x507C00000, ioba=0x506000000),
    2: dict(bus=0x220D100000, cpad=0x240D800000, wrad=0x240DC00000, ioba=0x240C000000),
    3: dict(bus=0x2307100000, cpad=0x2507800000, wrad=0x2507C00000, ioba=0x2506000000),
}


# ---- ADT -------------------------------------------------------------------
def adt_parse(b, off=0, depth=0):
    if depth > 16 or off + 8 > len(b):
        raise ValueError(f"ADT: bad node at {off:#x}")
    np_, nc = struct.unpack_from("<II", b, off)
    if np_ > 4096 or nc > 4096:
        raise ValueError(f"ADT: implausible node at {off:#x} ({np_} props, {nc} children)")
    off += 8
    props = {}
    for _ in range(np_):
        name = b[off:off + 32].split(b"\0")[0].decode("latin-1")
        sz = struct.unpack_from("<I", b, off + 32)[0] & 0x7FFFFFFF
        props[name] = bytes(b[off + 36:off + 36 + sz])
        off += 36 + ((sz + 3) & ~3)
    kids = []
    for _ in range(nc):
        k, off = adt_parse(b, off, depth + 1)
        kids.append(k)
    return {"props": props, "children": kids}, off


def adt_name(n):
    return n["props"].get("name", b"").split(b"\0")[0].decode("latin-1")


def adt_child(n, name):
    for k in n["children"]:
        if adt_name(k) == name:
            return k
    return None


def regs(n):
    r = n["props"].get("reg", b"")
    return [struct.unpack_from("<QQ", r, i) for i in range(0, len(r) - 15, 16)]


def segments(n):
    s = n["props"].get("segment-ranges", b"")
    if len(s) % 32:
        raise ValueError(f"{adt_name(n)}: segment-ranges is {len(s)} bytes")
    return [dict(zip(("phys", "iova", "remap", "size", "flags"), struct.unpack_from("<QQQII", s, i)))
            for i in range(0, len(s), 32)]


def filters(n):
    f = n["props"].get("filter-data-instance-0", b"")
    return [struct.unpack_from("<QQII", f, i) for i in range(0, len(f) - 23, 24)]


# ---- firmware --------------------------------------------------------------
def tags(buf):
    out = {}
    m = re.search(rb"GKTS\x08\x00\x00\x00", buf)
    if not m:
        return out
    i = m.start()
    for _ in range(64):
        if i + 8 > len(buf):
            break
        tag, n = buf[i:i + 4], int.from_bytes(buf[i + 4:i + 8], "little")
        if not all(0x20 <= c < 0x7F for c in tag) or n > 0x1000:
            break
        out[tag[::-1].decode()] = buf[i + 8:i + 8 + n]
        i += 8 + n
    return out


def in_iboot(off):
    return any(a <= off < b for a, b in IBOOT_RANGES)


def check_text(img, dump, who):
    if len(dump) < TEXT_SIZE:
        sys.exit(f"{who} TEXT dump is {len(dump):#x} bytes, want {TEXT_SIZE:#x}")
    want = bytearray(img[TEXT_FILE_OFF:TEXT_FILE_OFF + TEXT_SIZE])
    lit = int.from_bytes(dump[LITERAL_OFF:LITERAL_OFF + 8], "little")
    want[LITERAL_OFF:LITERAL_OFF + 8] = dump[LITERAL_OFF:LITERAL_OFF + 8]
    ndiff = sum(1 for a, b in zip(want, dump[:TEXT_SIZE]) if a != b)
    for w in TEXT_WINDOWS:
        h = hashlib.sha256(dump[w:w + 0x4000]).hexdigest()
        g = hashlib.sha256(img[TEXT_FILE_OFF + w:TEXT_FILE_OFF + w + 0x4000]).hexdigest()
        print(f"    TEXT+{w:#07x} sha256 {h[:16]}... {'= image' if h == g else 'DIFFERS from the image'}")
    print(f"    TEXT vs H13D image: {ndiff} byte(s) differ besides the literal; literal {lit:#x}")
    if ndiff or lit != LITERAL:
        sys.exit(f"{who}: TEXT in DRAM is not the H13D image with iBoot's literal; refusing")


def check_data(img, dump, who, enc):
    if len(dump) < DATA_FILE_SIZE:
        sys.exit(f"{who} DATA dump is only {len(dump):#x} bytes")
    if len(dump) > DATA_SIZE:
        dump = dump[:DATA_SIZE]
    fil = img[DATA_FILE_OFF:DATA_FILE_OFF + DATA_FILE_SIZE]
    bad = [o for o in range(DATA_FILE_SIZE) if dump[o] != fil[o] and not in_iboot(o)]
    tail = [o for o in range(DATA_FILE_SIZE, len(dump)) if dump[o]]
    print(f"    DATA vs H13D image: {len(bad)} byte(s) differ outside iBoot's ranges, "
          f"{len(tail)} non-zero byte(s) past the file-backed {DATA_FILE_SIZE:#x}")
    if bad or tail:
        first = (bad or tail)[0]
        sys.exit(f"{who}: DATA is not cold H13D DATA (first difference at DATA+{first:#x}). "
                 "Dump it on a fresh boot before any AVE load (docs/98 U0); refusing")
    t = tags(bytes(dump))
    for k in ("STKG", "SOC_", "SOCR", "CpAd", "WrAd", "IOBA"):
        if k in t:
            print(f"    tag {k} = {int.from_bytes(t[k], 'little'):#x}")
    got = {k: int.from_bytes(dump[o:o + 8], "little") for k, o in TAG_OFF.items()}
    want = {"CpAd": enc["cpad"], "WrAd": enc["wrad"], "IOBA": enc["ioba"]}
    ok = got == want
    print(f"    CpAd/WrAd/IOBA {got['CpAd']:#x}/{got['WrAd']:#x}/{got['IOBA']:#x}: "
          f"{'this encoder' if ok else 'NOT this encoder' + chr(39) + 's banks'} "
          f"(want {want['CpAd']:#x}/{want['WrAd']:#x}/{want['IOBA']:#x})")
    blob = bytes(dump) + bytes(DATA_SIZE - len(dump))
    return blob, ok, got


# ---- the block -------------------------------------------------------------
def c_sha(hexs):
    if not hexs:
        return "{ 0 }"
    b = bytes.fromhex(hexs)
    return "{ " + ", ".join(f"0x{x:02x}" for x in b) + " }"


def block(pl):
    L = []
    for n in range(4):
        p = pl[n]
        L.append(f"#define T6002_AVE{n}_TEXT_PHYS\t\t{p['text']:#x}ULL")
        L.append(f"#define T6002_AVE{n}_DATA_PHYS\t\t{p['data']:#x}ULL")
        if n == 0 or p["data"]:
            nm = "apple/ave-13.5-h13d-data-pristine.bin" if n == 0 else \
                 f"apple/ave-13.5-h13d-ave{n}-data-pristine.bin"
            L.append(f"#define T6002_AVE{n}_PRISTINE_NAME\t\"{nm}\"")
            L.append(f"#define T6002_AVE{n}_PRISTINE_SHA256\t{c_sha(p['sha'])}")
        else:
            L.append(f"#define T6002_AVE{n}_PRISTINE_NAME\tT6002_AVE0_PRISTINE_NAME")
            L.append(f"#define T6002_AVE{n}_PRISTINE_SHA256\tT6002_AVE0_PRISTINE_SHA256")
    return "\n".join(L) + "\n"


def apply(path, text):
    src = open(path).read()
    b = src.index("BEGIN t6002-placement")
    b = src.index("*/\n", b) + 3
    e = src.index("/* END t6002-placement */")
    open(path, "w").write(src[:b] + text + src[e:])
    print(f"wrote the block into {path}; now: make -C driver")


# ---- main ------------------------------------------------------------------
def run(adt_path, dumps, fw, apply_to, blob_dir):
    img = open(fw, "rb").read()
    h = hashlib.sha256(img).hexdigest()
    print(f"firmware {fw}: sha256 {h[:16]}... {'H13D' if h == H13D_SHA else 'NOT the 13.5 H13D image'}")
    if h != H13D_SHA:
        sys.exit("refusing: the t6002 rows are for macOS 13.5's AppleAVE2FW_H13D")

    root, _ = adt_parse(open(adt_path, "rb").read())
    compat = root["props"].get("compatible", b"").split(b"\0")
    print(f"live ADT: {adt_path}, root compatible {[c.decode() for c in compat if c]}")
    if b"J375dAP" not in compat and b"j375dap" not in [c.lower() for c in compat]:
        print("  (not a j375d ADT; continuing, the encoder checks below still apply)")
    arm = adt_child(root, "arm-io")
    if not arm:
        sys.exit("no /arm-io in the ADT")

    found = {}
    for n in range(4):
        node = adt_child(arm, f"ave{n}")
        dart = adt_child(arm, f"dart-ave{n}")
        if not node:
            sys.exit(f"no /arm-io/ave{n}")
        r = regs(node)
        sve = node["props"].get("sve-id")
        pre = "pre-loaded" in node["props"]
        segs = segments(node)
        print(f"/arm-io/ave{n}: reg[0] {r[0][0]:#x}, sve-id {struct.unpack('<I', sve)[0] if sve else '-'}, "
              f"pre-loaded {'yes' if pre else 'no'}, {len(segs)} segment(s)")
        if r[0][0] != ENC[n]["bus"]:
            sys.exit(f"ave{n} reg[0] {r[0][0]:#x} is not the j375d restore ADT's {ENC[n]['bus']:#x}; refusing")
        for s in segs:
            print(f"    segment phys {s['phys']:#x} iova {s['iova']:#x} remap {s['remap']:#x} "
                  f"size {s['size']:#x} flags {s['flags']:#x}")
        if dart:
            for f in filters(dart):
                print(f"    dart-ave{n} filter {f[0]:#x}-{f[1]:#x} r0 {f[2]:#x} r4 {f[3]:#x}")
        text = [s for s in segs if s["iova"] == 0]
        data = [s for s in segs if s["iova"] == DATA_DVA]
        other = [s for s in segs if s["iova"] not in (0, DATA_DVA)]
        if other or len(text) > 1 or len(data) > 1:
            sys.exit(f"ave{n}: segments other than one TEXT at iova 0 and one DATA at 0xec000; refusing")
        if text and text[0]["size"] != TEXT_SIZE:
            sys.exit(f"ave{n}: TEXT segment is {text[0]['size']:#x} bytes, H13D's is {TEXT_SIZE:#x}")
        if data and data[0]["size"] != DATA_SIZE:
            sys.exit(f"ave{n}: DATA segment is {data[0]['size']:#x} bytes, H13D's is {DATA_SIZE:#x}")
        if data and not text:
            sys.exit(f"ave{n}: a DATA segment without a TEXT one; refusing")
        found[n] = (text[0] if text else None, data[0] if data else None)

    if not found[0][0] or not found[0][1]:
        sys.exit("ave0 has no iBoot TEXT+DATA: iBoot did not preload the encoder, nothing to adopt")

    pl, die_local = {}, False
    for n in range(4):
        t, d = found[n]
        p = dict(text=(t or found[0][0])["phys"], data=d["phys"] if d else 0, sha=None,
                 how="own TEXT" if t else "ave0's TEXT (assumed; the stage-10 RVBAR check decides)")
        print(f"ave{n}: TEXT {p['text']:#x} ({p['how']}), "
              f"DATA {'%#x (iBoot)' % p['data'] if p['data'] else 'driver-owned copy'}")
        if dumps and t:
            f = os.path.join(dumps, f"ave{n}-text.bin")
            if os.path.exists(f):
                check_text(img, open(f, "rb").read(), f"ave{n}")
            else:
                print(f"    (no {f}: TEXT not checked)")
        if dumps and d:
            f = os.path.join(dumps, f"ave{n}-data.bin")
            if os.path.exists(f):
                blob, ok, got = check_data(img, open(f, "rb").read(), f"ave{n}", ENC[n])
                if not ok:
                    if n >= 2 and got == {"CpAd": ENC[n - 2]["cpad"], "WrAd": ENC[n - 2]["wrad"],
                                          "IOBA": ENC[n - 2]["ioba"]}:
                        die_local = True
                        print("    die 1's firmware addresses its block DIE-LOCALLY (die 0's numbers)")
                    else:
                        sys.exit(f"ave{n}: iBoot's tags are not this encoder's banks; refusing")
                nm = "ave-13.5-h13d-data-pristine.bin" if n == 0 else f"ave-13.5-h13d-ave{n}-data-pristine.bin"
                out = os.path.join(blob_dir, nm)
                open(out, "wb").write(blob)
                p["sha"] = hashlib.sha256(blob).hexdigest()
                print(f"    pristine blob {out}: sha256 {p['sha']}")
            else:
                print(f"    (no {f}: no pristine blob; reload{' and owned DATA' if n == 0 else ''} will refuse)")
        pl[n] = p

    if die_local:
        for n in (2, 3):
            if not pl[n]["data"]:
                print(f"ave{n}: WITHHELD (TEXT 0): its owned DATA would carry global die-1 tags, "
                      "and iBoot's own die-1 DATA says the firmware wants die-local ones (docs/98 §6)")
                pl[n]["text"] = 0

    text = block(pl)
    print("\n/* BEGIN t6002-placement block */\n" + text + "/* END */")
    if apply_to:
        apply(apply_to, text)
    return pl


def selftest():
    """Synthetic ADT + dumps from the H13D image: ave0 preloaded, the rest not."""
    import tempfile
    fw = os.environ.get("AVE_H13D", "/lib/firmware/apple/ave_h13d.bin")
    if not os.path.exists(fw):
        sys.exit(f"selftest needs the H13D image (AVE_H13D=...); {fw} missing")
    img = open(fw, "rb").read()

    def prop(name, val):
        return name.encode().ljust(32, b"\0") + struct.pack("<I", len(val)) + val + bytes(-len(val) % 4)

    def node(props, kids=()):
        return struct.pack("<II", len(props), len(kids)) + b"".join(prop(*p) for p in props) + b"".join(kids)

    seg = struct.pack("<QQQII", 0x10000B28000, 0, 0x10000B28000, TEXT_SIZE, 0) + \
        struct.pack("<QQQII", 0x10001A90000, DATA_DVA, 0x1F0000EC000, DATA_SIZE, 0)
    aves = []
    for n in range(4):
        p = [("name", f"ave{n}".encode() + b"\0"), ("reg", struct.pack("<QQ", ENC[n]["bus"], 0x45C000))]
        if n:
            p.append(("sve-id", struct.pack("<I", n)))
        if n == 0:
            p += [("pre-loaded", struct.pack("<I", 1)), ("segment-ranges", seg)]
        aves.append(node(p))
    adt = node([("name", b"device-tree\0"), ("compatible", b"J375dAP\0AppleARM\0")],
               [node([("name", b"arm-io\0")], aves)])
    d = tempfile.mkdtemp()
    open(f"{d}/adt.bin", "wb").write(adt + bytes(0x4000))
    t = bytearray(img[TEXT_FILE_OFF:TEXT_FILE_OFF + TEXT_SIZE])
    t[LITERAL_OFF:LITERAL_OFF + 8] = LITERAL.to_bytes(8, "little")
    open(f"{d}/ave0-text.bin", "wb").write(t)
    data = bytearray(img[DATA_FILE_OFF:DATA_FILE_OFF + DATA_FILE_SIZE]) + bytes(DATA_SIZE - DATA_FILE_SIZE)
    for k, o in TAG_OFF.items():
        data[o:o + 8] = ENC[0][{"CpAd": "cpad", "WrAd": "wrad", "IOBA": "ioba"}[k]].to_bytes(8, "little")
    data[0x3A38:0x3A40] = bytes.fromhex("0123456789abcdef")
    open(f"{d}/ave0-data.bin", "wb").write(data)
    src = open(os.path.join(REPO, "driver/ave_soc.c")).read()
    open(f"{d}/ave_soc.c", "w").write(src)
    pl = run(f"{d}/adt.bin", d, fw, f"{d}/ave_soc.c", d)
    assert pl[0]["text"] == 0x10000B28000 and pl[0]["data"] == 0x10001A90000 and pl[0]["sha"]
    assert all(pl[n]["text"] == 0x10000B28000 and pl[n]["data"] == 0 for n in (1, 2, 3))
    out = open(f"{d}/ave_soc.c").read()
    assert "#define T6002_AVE0_TEXT_PHYS\t\t0x10000b28000ULL" in out
    assert out.count("BEGIN t6002-placement") == 1 and out.count("END t6002-placement") == 1
    assert src.split("BEGIN t6002-placement")[0] == out.split("BEGIN t6002-placement")[0]
    assert src.split("/* END t6002-placement */")[1] == out.split("/* END t6002-placement */")[1]
    # a warm (run) DATA must be refused
    data[0x40000] ^= 0xFF
    open(f"{d}/ave0-data.bin", "wb").write(data)
    try:
        run(f"{d}/adt.bin", d, fw, None, d)
    except SystemExit as e:
        assert "not cold" in str(e)
    else:
        raise AssertionError("a warm DATA was accepted")
    print("\nselftest: PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--adt", help="the live ADT (physdump adt=1)")
    ap.add_argument("--dumps", help="directory with aveN-text.bin / aveN-data.bin (physdump seg=N)")
    ap.add_argument("--fw", default="/lib/firmware/apple/ave_h13d.bin", help="the H13D Mach-O")
    ap.add_argument("--blob-dir", help="where to write the pristine blob(s) (default: --dumps)")
    ap.add_argument("--apply", metavar="AVE_SOC_C", help="rewrite the block in this ave_soc.c")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.adt:
        ap.error("--adt is required")
    run(a.adt, a.dumps, a.fw, a.apply, a.blob_dir or a.dumps or ".")


if __name__ == "__main__":
    main()
