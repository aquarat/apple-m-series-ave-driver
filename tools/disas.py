#!/usr/bin/env python3
"""Disassemble a symbol from the kernelcache or the AVE firmware.

Handles the VA->file-offset arithmetic that both images need. llvm-objdump
will not disassemble __TEXT_EXEC addresses inside the MH_FILESET kernelcache,
so this resolves the offset itself and hands raw bytes to GNU objdump.

Examples:
  disas.py --kext 'AVE_IPC10CreateChannel'
  disas.py --kext 'AVE_CHM_MakeFwCmd_Open' -n 0x400
  disas.py --fw  'InitMailboxRoute'
  disas.py --kext --addr 0xfffffe0008c06478 -n 0x200
  disas.py --macos 13.5 --fw 'CmdProcessor' --list   # the build iBoot loads (docs/43)

--macos selects which build's blobs to read (default 26.6.2, the one every doc
before docs/43 cites). AVE_MACOS=13.5 in the environment does the same.
"""
import argparse, struct, subprocess, sys, os, re, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (kernelcache, firmware, kext symbols, firmware symbols) per macOS build.
BUILDS = {
    "26.6.2": ("data/blobs/kc.macho", "data/blobs/ave_h13c.bin",
               "data/derived/kext-symbols.txt", "data/derived/symbols.txt"),
    "13.5":   ("data/blobs/macos-13.5/kc.macho", "data/blobs/macos-13.5/ave_h13c.bin",
               "data/blobs/macos-13.5/derived/kext-symbols.txt",
               "data/blobs/macos-13.5/derived/symbols.txt"),
}

def segments(path):
    """[(name, vmaddr, vmsize, fileoff, filesize)] from a Mach-O."""
    d = open(path, "rb").read(1 << 20)
    magic, _, _, _, ncmds, _, _ = struct.unpack("<IIIIIII", d[:28])
    if magic != 0xfeedfacf:
        sys.exit(f"{path}: not a 64-bit Mach-O")
    out, off = [], 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", d[off:off+8])
        if cmd == 0x19:
            name = d[off+8:off+24].rstrip(b"\0").decode()
            va, vs, fo, fs = struct.unpack("<QQQQ", d[off+24:off+56])
            out.append((name, va, vs, fo, fs))
        off += cmdsize
    return out

def va_to_off(segs, va):
    for name, sva, vs, fo, fs in segs:
        if sva <= va < sva + vs and fs:
            return fo + (va - sva), name
    return None, None

def find_symbol(symfile, pattern):
    """Return [(va, name)] whose mangled name contains pattern."""
    hits = []
    for line in open(symfile):
        va, _, name = line.partition("  ")
        name = name.strip()
        if pattern in name and "_os_log_fmt" not in name:
            hits.append((int(va, 16), name))
    return hits

def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--kext", action="store_true", help="AppleAVE2 in the kernelcache")
    g.add_argument("--fw", action="store_true", help="AppleAVE2FW firmware image")
    ap.add_argument("symbol", nargs="?", help="substring of the mangled symbol name")
    ap.add_argument("--addr", type=lambda x: int(x, 0), help="disassemble this VA instead")
    ap.add_argument("-n", "--length", type=lambda x: int(x, 0), default=0x300)
    ap.add_argument("--list", action="store_true", help="only list matching symbols")
    ap.add_argument("--macos", choices=sorted(BUILDS), default=os.environ.get("AVE_MACOS", "26.6.2"),
                    help="which build's blobs to read (default 26.6.2, or $AVE_MACOS)")
    a = ap.parse_args()
    if a.macos not in BUILDS:
        sys.exit(f"AVE_MACOS={a.macos!r}: choose one of {sorted(BUILDS)}")

    kc, fw, ksym, fsym = (os.path.join(REPO, p) for p in BUILDS[a.macos])
    image, symfile = (kc, ksym) if a.kext else (fw, fsym)
    for p in (image, symfile):
        if not os.path.exists(p):
            sys.exit(f"missing {p} -- see docs/05-reproducing.md")
    segs = segments(image)

    if a.addr is not None:
        targets = [(a.addr, f"<{a.addr:#x}>")]
    else:
        if not a.symbol:
            sys.exit("give a symbol substring or --addr")
        targets = find_symbol(symfile, a.symbol)
        if not targets:
            sys.exit(f"no symbol matching {a.symbol!r}")
        if a.list or len(targets) > 1:
            for va, n in sorted(targets):
                print(f"  {va:#018x}  {n}")
            if a.list:
                return
            if len(targets) > 1:
                print(f"\n{len(targets)} matches -- narrow the pattern or use --addr", file=sys.stderr)
                return

    d = open(image, "rb").read()
    for va, name in targets:
        off, seg = va_to_off(segs, va)
        if off is None:
            print(f"!! {name}: VA {va:#x} not in any mapped segment", file=sys.stderr)
            continue
        print(f"\n===== {name}\n===== VA {va:#x}  seg {seg}  file {off:#x}  len {a.length:#x}\n")
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as t:
            t.write(d[off:off+a.length])
            tmp = t.name
        try:
            out = subprocess.run(
                ["objdump", "-D", "-b", "binary", "-m", "aarch64",
                 f"--adjust-vma={va:#x}", tmp],
                capture_output=True, text=True).stdout
            # drop objdump's synthetic header lines
            lines = out.splitlines()
            start = next((i for i, l in enumerate(lines) if re.match(r"^\s*[0-9a-f]+:", l)), 0)
            print("\n".join(lines[start:]))
        finally:
            os.unlink(tmp)

if __name__ == "__main__":
    main()
