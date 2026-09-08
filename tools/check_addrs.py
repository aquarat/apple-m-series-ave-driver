#!/usr/bin/env python3
"""Assert that ADT bus addresses are translated to CPU physical addresses.

Nodes under /arm-io carry BUS addresses. The /arm-io node's "ranges" property
maps them to parent (CPU physical) space. On t6001 the first range is
bus 0x0 -> parent 0x2_00000000, size 0x4_00000000, so essentially every
peripheral address needs +0x2_00000000.

Getting this wrong points every access at an undecoded hole, which on Apple
silicon hangs the fabric with no fault and nothing logged - indistinguishable
from "the device is not responding". That is exactly what happened across eight
bring-up attempts here; see docs/30-address-translation-bug.md.

This script positive-controls the translation against nodes that Linux already
has, then prints the corrected AVE addresses.

Usage: check_addrs.py
"""
import os, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "m1n1-src/proxyclient"))
from m1n1.adt import load_adt


def translate(ranges, bus):
    for r in ranges:
        if r.bus_addr <= bus < r.bus_addr + r.size:
            return bus - r.bus_addr + r.parent_addr
    return None


def linux_node_addr(prefix):
    """Find /proc/device-tree/soc/<prefix>@<addr> and return the addr."""
    base = "/proc/device-tree/soc"
    if not os.path.isdir(base):
        return None
    for n in os.listdir(base):
        if n.startswith(prefix + "@"):
            return int(n.split("@", 1)[1], 16)
    return None


def main():
    adt = load_adt(open(os.path.join(REPO, "data/blobs/adt.bin"), "rb").read())
    ranges = adt["/arm-io"]._properties["ranges"]
    print("/arm-io ranges:")
    for r in ranges:
        print(f"  bus {r.bus_addr:#014x} -> parent {r.parent_addr:#014x} "
              f"size {r.size:#x}")

    print("\nPositive controls (ADT vs the live Linux device tree):")
    ok = True
    for adt_path, linux_prefix in (("/arm-io/avd0", "avd"),
                                   ("/arm-io/isp0", "isp"),
                                   ("/arm-io/dart-avd0", "iommu")):
        bus = adt[adt_path]._properties["reg"][0].addr
        want = translate(ranges, bus)
        got = linux_node_addr(linux_prefix)
        if linux_prefix == "iommu":     # many iommu nodes; just check ours exists
            got = want if os.path.isdir(f"/proc/device-tree/soc/iommu@{want:x}") else None
        status = "OK" if got == want else f"MISMATCH (linux says {got:#x})" if got else "n/a"
        if got is not None and got != want:
            ok = False
        print(f"  {adt_path:<20} bus {bus:#012x} -> phys {want:#012x}   {status}")

    print("\nCorrected AVE addresses (bus -> physical):")
    for path in ("/arm-io/ave0", "/arm-io/dart-ave0", "/arm-io/ave1"):
        try:
            node = adt[path]
        except Exception:
            continue
        print(f"  {path}")
        for i, r in enumerate(node._properties["reg"]):
            print(f"    reg[{i}] {r.addr:#014x} -> {translate(ranges, r.addr):#014x} "
                  f"+ {r.size:#x}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
