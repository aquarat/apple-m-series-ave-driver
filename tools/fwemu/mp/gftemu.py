#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Emulate CAVECommonController::GetFrameType (H14G) in final-pass mode from a live snapshot:
multipass enable=1, pass=2, every frame type 5, PICMGMT+0x900 -> a synthetic multipass input
buffer (0x108-byte header + 11 records for frame 0, then 1 record = frame N+10).
MappedMemory is stubbed: HwToTarget returns the synthetic buffer.
Usage: gftemu.py SNAPDIR NFRAMES [bframes=N] [idr=N] [scene=a,b] [window=10] [off=0|1]"""
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import struct, sys
sys.path.insert(0, REPO_ROOT + '/tools/fwemu')
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UcError
from unicorn.arm64_const import *
from fwmem import Snap
from fwsyms import sized
FW = REPO_ROOT + '/data/blobs/macos-13.5-j473/ave_h14g.bin'
CTRL = 0xffffffff80803760
PIC0 = 0xffffffff808049b8   # the snapshot frame's PICMGMT (locate.py)
FT = CTRL + 0x1b0
MPQ = CTRL + 0x24378
MPB = CTRL + 0x23fc4         # multipass block: u8 enable, u32 pass @+4 (IEP 0x4e6cc..)
REC = 0x626
STACK, TRAP, SCR, MPBUF = 0x10000000, 0x20000000, 0x30000000, 0x40000000

def main():
    snap, n = sys.argv[1], int(sys.argv[2])
    opts = dict(a.split('=', 1) for a in sys.argv[3:])
    scenes = set(int(x) for x in opts.get('scene', '').split(',') if x)
    win = int(opts.get('window', '10'))
    s = Snap(snap)
    syms = sized(FW); addr = {k: v[0] for k, v in syms.items()}; rev = {v[0]: k for k, v in syms.items()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    for base, b, _ in s.regions():
        lo, hi = base & ~0xfff, (base + len(b) + 0xfff) & ~0xfff
        uc.mem_map(lo, hi - lo, UC_PROT_ALL); uc.mem_write(base, bytes(b))
    for va, sz, pa in s.mmio(): uc.mem_map(va, sz, UC_PROT_ALL)
    for a, l in ((STACK, 0x100000), (TRAP, 0x1000), (SCR, 0x20000), (MPBUF, 0x10000)): uc.mem_map(a, l, UC_PROT_ALL)
    uc.mem_write(TRAP, b"\x00\x00\x20\xd4")
    w32 = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    r32 = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    uc.mem_write(MPB, b'\x01'); w32(MPB + 4, 2)                     # enable, pass 2
    if 'bframes' in opts: w32(FT + 0x88, int(opts['bframes'])); uc.mem_write(FT + 2, bytes([int(opts['bframes']) != 0]))
    if 'idr' in opts: w32(FT + 0x80, int(opts['idr'])); w32(FT + 0x84, int(opts['idr']))
    # private copy of the PICMGMT so the snapshot's is untouched
    PIC = SCR + 0x10000
    uc.mem_write(PIC, bytes(uc.mem_read(PIC0, 0x6838 - 0x55b0 if False else 0x1000)))
    stubs = {a for nme, a in addr.items() if nme.startswith(("__ZNK14CAVEFilterBase5Print", "__ZN7CLogger", "__Z17AVE_History_Print", "_printf", "__ZN9CTaskPool16GetCurrentTaskID"))}
    mm_ctor, mm_dtor, mm_h2t = addr['__ZN12MappedMemoryC2Emmb'], addr['__ZN12MappedMemoryD1Ev'], addr['__ZNK12MappedMemory10HwToTargetEm']
    maps = []
    stop = addr['_bsp_assert_fail']; calls = []
    def ret(u, v=None):
        if v is not None: u.reg_write(UC_ARM64_REG_X0, v)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
    def on_code(u, a, sz, _):
        if a == stop: calls.append('ASSERT from %#x' % u.reg_read(UC_ARM64_REG_X30)); u.emu_stop()
        elif a in stubs: ret(u, 0)
        elif a == mm_ctor:
            maps.append((u.reg_read(UC_ARM64_REG_X1), u.reg_read(UC_ARM64_REG_X2))); ret(u, u.reg_read(UC_ARM64_REG_X0))
        elif a == mm_dtor: ret(u, u.reg_read(UC_ARM64_REG_X0))
        elif a == mm_h2t: ret(u, MPBUF)
        elif a in rev and ('MultiPass' in rev[a] or 'CFrameType' in rev[a] or 'FirstPass' in rev[a] or 'RateControl' in rev[a]):
            calls.append(rev[a][4:40])
    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, lambda u, acc, a, sz, v, _: (calls.append('UNMAPPED %#x pc %#x' % (a, u.reg_read(UC_ARM64_REG_PC))), False)[1])
    W, H = 1280, 720
    starts = sorted(scenes | {0})
    def rec(fn):
        r = bytearray(REC)
        struct.pack_into('<I', r, 0x1c, W); struct.pack_into('<I', r, 0x20, H)
        struct.pack_into('<f', r, 0x24, 30.0)
        struct.pack_into('<I', r, 0x2c, fn)
        struct.pack_into('<I', r, 0x134, 1)
        nxt = [x for x in starts if x > fn]
        if scenes and nxt:
            struct.pack_into('<I', r, 0x4b0, 1); struct.pack_into('<I', r, 0x4c4, nxt[0] - fn)
        return bytes(r)
    hdr = bytes(0x108)
    fname = {1: 'P', 2: 'B', 3: 'IDR', 7: 'Bref'}
    out = []
    for f in range(n):
        if f == 0:
            buf = hdr + b''.join(rec(min(k, n - 1)) for k in range(win + 1))
        else:
            buf = hdr + rec(min(f + win, n - 1))
        uc.mem_write(MPBUF, buf.ljust(0x4400, b'\0'))
        w32(PIC + 0xca8, f); w32(PIC + 0xcac, 5); w32(PIC + 8, f)
        uc.mem_write(PIC + 0x900, struct.pack('<Q', 0xf0000000)); w32(PIC + 0xcb0, 0)
        o = SCR
        uc.mem_write(o, bytes(0x40))
        for i, v in enumerate((CTRL, PIC, o, o + 8, o + 0x10, o + 0x18)): uc.reg_write(UC_ARM64_REG_X0 + i, v)
        uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff000); uc.reg_write(UC_ARM64_REG_X30, TRAP)
        calls.clear(); maps.clear()
        try:
            uc.emu_start(addr['__ZN20CAVECommonController12GetFrameTypeEP18AVE_PICMGMT_PARAMSPjPtS3_S2_'], TRAP, count=20_000_000)
        except UcError as e:
            calls.append('ERR %s pc %#x' % (e, uc.reg_read(UC_ARM64_REG_PC)))
        t = r32(PIC + 0xcac)
        cnt, head, tail = r32(MPQ + 0x6260), r32(MPQ + 0x6264), r32(MPQ + 0x6268)
        q = [r32(MPQ + i * REC + 0x2c) for i in range(16)]
        out.append('%d:%s' % (f, fname.get(t, str(t))))
        bad = any(c.startswith(('ASSERT', 'ERR', 'UNMAPPED')) for c in calls)
        if f < 3 or bad or 'v' in opts:
            print('frame %d type %d maps %s queue cnt %d head %d tail %d fns %s' % (f, t, [(hex(a), hex(b)) for a, b in maps], cnt, head, tail, q))
            print('   calls', ' '.join(dict.fromkeys(calls)))
        if bad: break
    print(' '.join(out))
main()
