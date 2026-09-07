#!/usr/bin/env python3
"""Group AppleAVE2 kext symbols into a class -> method map.

The kext is the HOST side of the AVE protocol. Its class layout shows how
Apple's driver is decomposed, which is the closest thing to a specification
a Linux driver author is going to get.

Usage: kext_classmap.py <kext-symbols.txt> [--out data/derived/kext-classmap.txt]
"""
import argparse, re

def parse(m):
    """Split Itanium __ZN<len><name>...E into its components."""
    if not m.startswith("__ZN"):
        return None
    i, parts = 4, []
    while i < len(m) and m[i].isdigit():
        j = i
        while j < len(m) and m[j].isdigit():
            j += 1
        n = int(m[i:j])
        parts.append(m[j:j+n])
        i = j + n
    return parts if len(parts) >= 2 else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symbols")
    ap.add_argument("--out", default="data/derived/kext-classmap.txt")
    ap.add_argument("--prefix", default="AVE", help="only classes starting with this")
    a = ap.parse_args()
    classes = {}
    for line in open(a.symbols):
        va, _, name = line.partition("  ")
        name = name.strip()
        if "_os_log_fmt" in name:
            continue
        p = parse(name)
        if not p:
            continue
        cls, meth = p[0], "::".join(p[1:])
        classes.setdefault(cls, {}).setdefault(meth, int(va, 16))
    with open(a.out, "w") as f:
        f.write("# AppleAVE2.kext class -> method map (host side of the protocol).\n")
        f.write("# Extracted from the kernelcache symbol table; addresses are kernel VAs.\n\n")
        for cls in sorted(c for c in classes if c.startswith(a.prefix) or c.startswith("AppleAVE")):
            f.write(f"===== {cls} ({len(classes[cls])} methods) =====\n")
            for meth, va in sorted(classes[cls].items()):
                f.write(f"  {va:#018x}  {meth}\n")
            f.write("\n")
    n = sum(len(v) for k, v in classes.items() if k.startswith(a.prefix) or k.startswith("AppleAVE"))
    print(f"{len(classes)} classes total, {n} AVE methods -> {a.out}")

if __name__ == "__main__":
    main()
