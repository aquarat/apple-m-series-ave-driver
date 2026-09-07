#!/usr/bin/env python3
"""Dump Apple Device Tree nodes from an unwrapped DeviceTree image.

The ADT is the ground truth for how AVE is wired: MMIO ranges, interrupts,
power/clock gates and its DART. It is NOT visible from a booted Linux system
-- /proc/device-tree is the hand-written kernel DTS that m1n1 patches, so it
only contains nodes somebody has already written a binding for. AVE is absent
there, which is why we read Apple's own DeviceTree image instead.

Requires m1n1's ADT parser:
  git clone --depth 1 https://github.com/AsahiLinux/m1n1 m1n1-src

Usage: adt_dump.py <adt.bin> [--grep REGEX] [--node PATH] [--full]
"""
import argparse, re, sys, os

def load(path_adt, m1n1="m1n1-src/proxyclient"):
    if not os.path.isdir(m1n1):
        sys.exit(f"m1n1 proxyclient not found at {m1n1}; see docstring")
    sys.path.insert(0, m1n1)
    from m1n1.adt import load_adt
    return load_adt(open(path_adt, "rb").read())

def walk(node, path=""):
    for child in node:
        p = f"{path}/{child.name}"
        yield p, child
        yield from walk(child, p)

def show(node, path):
    print(f"===== {path} =====")
    for k, v in node._properties.items():
        if isinstance(v, bytes) and len(v) > 48:
            v = v[:48].hex() + f"...({len(v)} bytes)"
        print(f"  {k:<24} = {v}")
    print()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("adt")
    ap.add_argument("--grep", help="regex matched against node paths")
    ap.add_argument("--node", action="append", help="dump this exact path (repeatable)")
    ap.add_argument("--full", action="store_true", help="use m1n1's full node repr")
    a = ap.parse_args()
    adt = load(a.adt)
    if a.node:
        for p in a.node:
            print(adt[p] if a.full else "", end="")
            if not a.full:
                show(adt[p], p)
        return
    pat = re.compile(a.grep, re.I) if a.grep else re.compile(r"ave|avd")
    for p, n in walk(adt):
        if pat.search(p):
            show(n, p) if not a.full else print(f"===== {p} =====\n{n}\n")

if __name__ == "__main__":
    main()
