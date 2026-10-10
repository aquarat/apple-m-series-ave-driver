#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a firmware function from a snapshot under Unicorn and log its MMIO (docs/93).

  emu.py SNAPDIR FUNC_SYMBOL X0 X1 [X2 ...] > writes.txt

SNAPDIR holds fw.bin and iova.bin from snap-target.py; AVE_FW names the
firmware Mach-O (default: the M2's H14G). Output: one line per register
access, "W|R  physical-address  size  value  pc  function".
"""
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import struct, sys, os
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UcError
from unicorn.arm64_const import *

HERE = REPO_ROOT + "/tools/fwemu"
sys.path.insert(0, HERE)
from fwmem import Snap
from fwsyms import sized
REPO = REPO_ROOT + ""

# the firmware Mach-O the snapshot ran (it carries its symbol table)
FW = os.environ.get("AVE_FW", os.path.join(REPO, "data/blobs/macos-13.5-j473/ave_h14g.bin"))
MMIO_DPE = 0xfffffffff4000000      # [0x1f3e60]: the AXI2AF window, PA 0x266000000
MMIO_LEN = 0x2000000
STACK, STACK_LEN = 0x10000000, 0x100000
TRAP = 0x20000000                  # return address: stop here

# functions replaced by "return 0" (logging, tasks, asserts are reported)
STUB_PREFIX = ("__ZNK14CAVEFilterBase5Print", "__ZN7CLogger", "__Z17AVE_History_Print",
               "__ZN9CTaskPool16GetCurrentTaskID", "_printf", "__Z6printf", "_rtk_printf",
               "__ZNK10CAVEObject5Print", "__ZN10CAVEObject5Print")
STOP = ("_bsp_assert_fail",)


def page(a): return a & ~0xfff


def main():
    snapdir, func = sys.argv[1], sys.argv[2]
    args = [int(x, 0) for x in sys.argv[3:]]
    s = Snap(snapdir)
    syms = sized(FW)
    addr = {k: v[0] for k, v in syms.items()}
    rev = {v[0]: k for k, v in syms.items()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)

    for base, b, what in s.regions():
        lo, hi = page(base), page(base + len(b) + 0xfff)
        uc.mem_map(lo, hi - lo, UC_PROT_ALL)
        uc.mem_write(base, bytes(b))
    for va, n, pa in s.mmio():
        uc.mem_map(va, n, UC_PROT_ALL)
    uc.mem_map(MMIO_DPE, MMIO_LEN, UC_PROT_ALL)
    uc.mem_map(STACK, STACK_LEN, UC_PROT_ALL)
    uc.mem_map(TRAP, 0x1000, UC_PROT_ALL)
    uc.mem_write(TRAP, b"\x00\x00\x20\xd4")    # brk #0

    # PATCH="va:hexbytes;..." applied to the emulated memory only (snapshot untouched)
    for p in filter(None, os.environ.get("PATCH", "").split(";")):
        va, hx = p.split(":"); uc.mem_write(int(va, 0), bytes.fromhex(hx))
    MPBUF = 0x40000000
    uc.mem_map(MPBUF, 0x10000, UC_PROT_ALL)
    if os.environ.get("MPBUF"):
        uc.mem_write(MPBUF, open(os.environ["MPBUF"], "rb").read())
    log = []
    mmio_ranges = [(MMIO_DPE, MMIO_LEN, 0x266000000)] + [(va, n, pa) for va, n, pa in s.mmio()]
    def where(a):
        for va, n, pa in mmio_ranges:
            if va <= a < va + n:
                return pa + (a - va)
    def on_w(uc_, acc, a, size, val, _):
        pa = where(a)
        if pa is not None:
            log.append(("W", uc_.reg_read(UC_ARM64_REG_PC), pa, size, val & ((1 << (8 * size)) - 1)))
    def on_r(uc_, acc, a, size, val, _):
        pa = where(a)
        if pa is not None:
            log.append(("R", uc_.reg_read(UC_ARM64_REG_PC), pa, size, 0))
    uc.hook_add(UC_HOOK_MEM_WRITE, on_w)
    uc.hook_add(UC_HOOK_MEM_READ, on_r)

    stubs = {a: n for n, a in addr.items() if n.startswith(STUB_PREFIX)}
    stops = {addr[n]: n for n in STOP if n in addr}
    calls = []
    mmc, mmd, mmh = addr["__ZN12MappedMemoryC2Emmb"], addr["__ZN12MappedMemoryD1Ev"], addr["__ZNK12MappedMemory10HwToTargetEm"]
    mpaddr = int(os.environ.get("MPADDR", "0"), 0)
    real = {}
    def on_code(uc_, a, size, _):
        # MappedMemory of the multipass input address -> synthetic buffer; everything else runs for real
        if a == mmc and mpaddr and uc_.reg_read(UC_ARM64_REG_X1) == mpaddr:
            calls.append(("MAP-MP", hex(uc_.reg_read(UC_ARM64_REG_X2))))
        if a == mmh and mpaddr and uc_.reg_read(UC_ARM64_REG_X1) == mpaddr:
            uc_.reg_write(UC_ARM64_REG_X0, MPBUF); uc_.reg_write(UC_ARM64_REG_PC, uc_.reg_read(UC_ARM64_REG_X30)); return
        if a in stops:
            calls.append(("ASSERT", hex(uc_.reg_read(UC_ARM64_REG_X30))))
            uc_.emu_stop()
        elif a in stubs:
            uc_.reg_write(UC_ARM64_REG_X0, 1 if "TaskID" in stubs[a] else 0)
            uc_.reg_write(UC_ARM64_REG_PC, uc_.reg_read(UC_ARM64_REG_X30))
        elif a in rev and a not in (start,):
            calls.append(rev[a])
    def on_unmapped(uc_, acc, a, size, val, _):
        calls.append(("UNMAPPED", hex(a), hex(uc_.reg_read(UC_ARM64_REG_PC))))
        return False
    start = addr[func]
    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    for i, v in enumerate(args):
        uc.reg_write(UC_ARM64_REG_X0 + i, v)
    uc.reg_write(UC_ARM64_REG_SP, STACK + STACK_LEN - 0x100)
    uc.reg_write(UC_ARM64_REG_X30, TRAP)
    try:
        uc.emu_start(start, TRAP, count=50_000_000)
        end = "returned" if uc.reg_read(UC_ARM64_REG_PC) == TRAP else f"stopped at {uc.reg_read(UC_ARM64_REG_PC):#x}"
    except UcError as e:
        end = f"error {e} at pc {uc.reg_read(UC_ARM64_REG_PC):#x}"
    print(f"# {func}({', '.join(hex(a) for a in args)}): {end}, x0={uc.reg_read(UC_ARM64_REG_X0):#x}, "
          f"{sum(1 for l in log if l[0] == 'W')} MMIO writes, {sum(1 for l in log if l[0] == 'R')} reads")
    seen = []
    for c in calls:
        if not seen or seen[-1][0] != c:
            seen.append([c, 1])
        else:
            seen[-1][1] += 1
    print("# calls: " + " ".join(f"{c if isinstance(c, str) else c}{'x%d' % n if n > 1 else ''}" for c, n in seen[:400]))
    for k, pc, pa, size, val in log:
        fn = max((a for a in rev if a <= pc), default=0)
        print(f"{k} {pa:#012x} {size} {val:#x}  pc {pc:#x} {rev.get(fn, '?')[:60]}")


if __name__ == "__main__":
    main()
