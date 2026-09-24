#!/usr/bin/env python3
"""Addresses and messages for AVE's SoC performance request to the PMP (docs/75).

Reads only the ADT. Computes, for ave0 and ave1 on t6001:
  - the PMP "PTD" dashboard entries macOS 13.5 writes when the AVE kext moves
    between VMin/VMid/VMid2/VMax (SOC-DEV-DVFS) and when VENC_SYS powers up or
    down (SOC-DEV-PS-REQ / -ACK, PMP-STATUS);
  - the 64-bit SOC-DEV-DVFS message for each AVE PState;
  - the PMGR perf-counter addresses the ADT "perf_block/perf_idx" fields name.

Every rule below is cited to docs/75 (macOS 13.5 kernelcache VAs). The script
refuses to print results unless its controls pass:
  C1  VENC_SYS PS register == 0x28e5803b0 (Linux DT, known good)
  C2  PS-REQ/ACK/STATUS offsets == Asahi pmp-report t600x constants
      (0xf80, 0x107c0, 0x1000, 0x10), derived independently
  C3  /arm-io/pmp reg[ptd-update-reg-index] - 0x10000 == pmgr reg[41]
      (RegMap 8 -> ADT reg 0x29, AppleT6001PMGR 0xfffffe0009b8ad78)
  C4  soc-device names at the computed indices are AVE0 / AVE1

Usage: python3 tools/pmp_ptd_map.py [adt.bin]
"""
import os, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "m1n1-src/proxyclient"))
from m1n1.adt import load_adt  # noqa: E402

adt_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "data/blobs/macos-13.5/adt.bin")
adt = load_adt(open(adt_path, "rb").read())
pmgr = adt["/arm-io/pmgr"]
pmp = adt["/arm-io/pmp"]
nub = adt["/arm-io/pmp/iop-pmp-nub"]._properties

fail = []
def check(name, ok, detail):
    print(f"  [{'ok' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        fail.append(name)

devs = {d.id2: d for d in pmgr.devices}
byname = {d.name: d for d in pmgr.devices}

def ps_addr(d):
    r = pmgr.ps_regs[d.psreg]
    return pmgr.get_reg(r.reg)[0] + r.offset + d.psidx * 8

# ptd-range: 32-byte records {u32 id, u32 base, u32 count, u32 ?, char name[16]}
ptd = {}
pr = nub["ptd-range"]
for i in range(0, len(pr), 32):
    rid, base, cnt, x = struct.unpack("<4I", pr[i:i + 16])
    ptd[pr[i + 16:i + 32].rstrip(b"\0").decode()] = (rid, base, cnt)

# soc-device: 124-byte records; [0] id (== PMGR devices.id1), [+44] != 0 -> has a
# SOC-DEV-DVFS slot, numbered in record order (_initPMPv2 0xfffffe0009840dd8..e20)
sd = nub["soc-device"]
soc = []
slot = 0
for i in range(0, len(sd), 124):
    w = struct.unpack("<31I", sd[i:i + 124])
    name = struct.pack("<I", w[29]).decode("latin1") + struct.pack("<I", w[30]).decode("latin1").rstrip("\0")
    dv = None
    if w[11]:
        dv, slot = slot, slot + 1
    soc.append(dict(index=i // 124, id=w[0], name=name, dvfs_slot=dv, flags=w[2]))
by_id = {s["id"]: s for s in soc}

ptd_rd = pmgr.get_reg(41)[0]                    # RegMap 8 (read side, 16 B/entry)
ptd_wr_pmp = pmp.get_reg(pmp.ptd_update_reg_index)[0]
ptd_wr = ptd_rd + 0x10000                        # ApplePTD::_writePTD 0xfffffe000987aa7c
def rd(idx): return ptd_rd + idx * 16
def wr(idx): return ptd_wr + idx * 8

print("controls")
check("C1", ps_addr(byname["VENC_SYS"]) == 0x28e5803b0, f"VENC_SYS PS {ps_addr(byname['VENC_SYS']):#x}")
got = (ptd["SOC-DEV-PS-REQ"][1] * 16, ptd["SOC-DEV-PS-REQ"][1] * 8 + 0x10000,
       ptd["SOC-DEV-PS-ACK"][1] * 16, ptd["PMP-STATUS"][1] * 16)
check("C2", got == (0xf80, 0x107c0, 0x1000, 0x10), "PS-REQ rd/wr, PS-ACK, STATUS = " + ", ".join(hex(x) for x in got))
check("C3", ptd_wr_pmp == ptd_wr, f"pmp reg[{pmp.ptd_update_reg_index}] {ptd_wr_pmp:#x} vs pmgr reg[41]+0x10000 {ptd_wr:#x}")
check("C4", by_id[byname["VENC_SYS"].id1]["name"] == "AVE0" and by_id[byname["VENC1_SYS"].id1]["name"] == "AVE1",
      "soc-device(VENC_SYS.id1, VENC1_SYS.id1) = " + by_id[byname["VENC_SYS"].id1]["name"] + ", " + by_id[byname["VENC1_SYS"].id1]["name"])
if fail:
    sys.exit(f"controls failed: {fail} -- not printing results")

print("\nPTD ranges used (read side = pmgr reg[41] + 16*idx, write side = +0x10000 + 8*idx)")
for n in ("PMP-STATUS", "DVFS-STATE", "SOC-DEV-PS-REQ", "SOC-DEV-PS-ACK", "SOC-DEV-DVFS"):
    rid, base, cnt = ptd[n]
    print(f"  {n:15s} id {rid:2d} base {base:3d} count {cnt:2d}  read {rd(base):#x}  write {wr(base):#x}")

# message: SOC | FAB0<<32 | FAB1<<36 | FAB2<<40 | FAB3<<44 | DCS<<48 | 1<<61
# (_pmpWriteDashBoardSetVirtualDeviceState 0xfffffe0009866af4..0xfffffe0009866b88)
def msg(soc_lvl, fab0=0):
    return soc_lvl | (fab0 << 32) | (1 << 61)

for sysname, inst in (("VENC_SYS", "ave0"), ("VENC1_SYS", "ave1")):
    d = byname[sysname]
    s = by_id[d.id1]
    idx = ptd["SOC-DEV-DVFS"][1] + s["dvfs_slot"]
    print(f"\n{inst}: {sysname} id2 {d.id2} id1 {d.id1} -> soc-device #{s['index']} {s['name']} "
          f"(flags {s['flags']:#x}), dvfs slot {s['dvfs_slot']}")
    print(f"  PS register            {ps_addr(d):#x}")
    print(f"  SOC-DEV-PS-REQ bit     {s['index']} (macOS: 1 << soc-device index); Asahi DT uses id1-1 = {d.id1 - 1}")
    print(f"  SOC-DEV-DVFS entry     idx {idx}: read {rd(idx):#x} (+8 status), write {wr(idx):#x}")
    pseudo = [x for x in pmgr.devices if x.id1 == d.id1 and x.flags.no_ps]
    for x in pseudo:
        dom = {32: "SOC", 33: "FAB0", 34: "FAB1", 35: "FAB2", 36: "FAB3", 37: "DCS"}.get(x.unk1_1, "-")
        print(f"    pseudo {x.id2:3d} {x.name:16s} domain {x.unk1_1} ({dom}) level {x.unk1_0}")
    for n, v in (("VMin (nothing on)", msg(0)), ("VMid  (VNOM on)", msg(1)), ("VMid2 (+VMID2)", msg(2)),
                 ("VMax  (+VMAX)", msg(3)), ("VMax + FAB0-VMAX", msg(3, 3))):
        print(f"    {n:20s} -> {v:#018x}")

print("\nPMGR perf counters (NOT perf states): ctl = base + perf.offset + 0x100 + idx*0x10, lo +8, hi +0xc")
print("  (_getSOCPerfCounter 0xfffffe00098633f8..0xfffffe0009863428; m1n1 dump_pmgr.py omits perf.offset)")
def pc(blk, idx):
    r = pmgr.perf_regs[blk]
    return pmgr.get_reg(r.reg)[0] + r.offset + 0x100 + idx * 0x10
for name, blk, idx in (("VENC_SYS device", 9, 36), ("MSR0 device", 9, 33), ("VENC1_SYS device", 10, 35)):
    print(f"  {name:22s} blk {blk:2d} idx {idx:3d} ctl {pc(blk, idx):#x}")
for c in pmgr.clocks:
    if c.name.startswith("VENC"):
        print(f"  clock {c.name:16s} blk {c.perf_block:2d} idx {c.perf_idx:3d} ctl {pc(c.perf_block, c.perf_idx):#x}")
for e in pmgr.events:
    if e.name.startswith("AVEMSR0_"):
        s2 = f", perf2 ctl {pc(e.perf2_block, e.perf2_idx):#x}" if e.perf2_idx else ""
        print(f"  event {e.name:16s} blk {e.perf_block:2d} idx {e.perf_idx:3d} ctl {pc(e.perf_block, e.perf_idx):#x}{s2}")
