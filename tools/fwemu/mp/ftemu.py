#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Emulate CFrameType::FrameType(RCFrameInfo*, u32*, MPQueue<16>&, codec) (H14G) from a live
snapshot, frame after frame, with synthetic first-pass records in the controller's MPQueue.
Usage: ftemu.py SNAPDIR NFRAMES [bframes=N] [idr=N] [scene=F,F,...] [field=OFF:VAL]
Prints the frame type the final pass would choose for each display-order frame."""
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import struct, sys, os
sys.path.insert(0, REPO_ROOT + '/tools/fwemu')
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UcError
from unicorn.arm64_const import *
from fwmem import Snap
from fwsyms import sized
FW = REPO_ROOT + '/data/blobs/macos-13.5-j473/ave_h14g.bin'
CTRL = 0xffffffff80803760
FT = CTRL + 0x1b0          # CFrameType of context 0 (GetFrameType: this+0x1b0+ctx*0x130)
MPQ = CTRL + 0x24378        # MPQueue<16,S_AVE_MultiPassStats>: 16 x 0x626, then count/head/tail at +0x6260
REC = 0x626
STACK, TRAP, SCR = 0x10000000, 0x20000000, 0x30000000

def main():
    snap, n = sys.argv[1], int(sys.argv[2])
    opts = dict(a.split('=', 1) for a in sys.argv[3:])
    scenes = set(int(x) for x in opts.get('scene', '').split(',') if x)
    sfield = int(opts.get('sfield', '0x34'), 0)      # which u32 marks a scene change (guess)
    s = Snap(snap)
    syms = sized(FW); addr = {k: v[0] for k, v in syms.items()}; rev = {v[0]: k for k, v in syms.items()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    for base, b, _ in s.regions():
        lo, hi = base & ~0xfff, (base + len(b) + 0xfff) & ~0xfff
        uc.mem_map(lo, hi - lo, UC_PROT_ALL); uc.mem_write(base, bytes(b))
    for va, sz, pa in s.mmio(): uc.mem_map(va, sz, UC_PROT_ALL)
    uc.mem_map(STACK, 0x100000, UC_PROT_ALL); uc.mem_map(TRAP, 0x1000, UC_PROT_ALL); uc.mem_map(SCR, 0x10000, UC_PROT_ALL)
    uc.mem_write(TRAP, b"\x00\x00\x20\xd4")
    w32 = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    w16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
    r32 = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    # session knobs in the CFrameType object (init params copied to ft+0x78, CFrameType::init 0x36ee8)
    if 'bframes' in opts: w32(FT + 0x88, int(opts['bframes'])); uc.mem_write(FT + 2, bytes([int(opts['bframes']) != 0]))
    if 'idr' in opts: w32(FT + 0x80, int(opts['idr'])); w32(FT + 0x84, int(opts['idr']))
    # ft+0x78 double = RC+0x08 (wire 0xFF38, MaxKeyFrameIntervalDuration, s); ft+0x98 = RC+0x1C (wire 0xFF4C, fps)
    if 'dur' in opts: uc.mem_write(FT + 0x78, struct.pack('<d', float(opts['dur'])))
    if 'fps' in opts: w32(FT + 0x98, int(opts['fps']))
    print('ft: dur(+0x78) %r idr(+0x80/0x84) %d/%d bframes(+0x88) %d fps(+0x98) %d' % (
        struct.unpack('<d', uc.mem_read(FT + 0x78, 8))[0], r32(FT + 0x80), r32(FT + 0x84), r32(FT + 0x88), r32(FT + 0x98)))
    stubs = {a for nme, a in addr.items() if nme.startswith(("__ZNK14CAVEFilterBase5Print", "__ZN7CLogger", "__Z17AVE_History_Print", "_printf", "__ZN9CTaskPool16GetCurrentTaskID"))}
    stop = addr['_bsp_assert_fail']
    calls = []
    def on_code(u, a, sz, _):
        if a == stop:
            calls.append('ASSERT from %#x' % u.reg_read(UC_ARM64_REG_X30)); u.emu_stop()
        elif a in stubs:
            u.reg_write(UC_ARM64_REG_X0, 0); u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
        elif a in rev and rev[a].startswith(('__ZN10CFrameType', '__ZN9AdaptiveB')):
            calls.append(rev[a].replace('__ZN10CFrameType', '')[:30])
    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, lambda u, acc, a, sz, v, _: (calls.append('UNMAPPED %#x pc %#x' % (a, u.reg_read(UC_ARM64_REG_PC))), False)[1])
    W, H = 1280, 720
    def rec(fn):
        r = bytearray(REC)
        struct.pack_into('<I', r, 0x2c, fn)                  # frameNumber (user space checks +0x2c)
        struct.pack_into('<HHHH', r, 0x1c, W, 0, H, 0)       # FrameType reads u32 at +0x1c and +0x20 as dims [I]
        struct.pack_into('<I', r, 0x134, 1)                  # 'valid' word ProcessFirstPassStats tests [I]
        if scenes:
            starts = sorted(scenes | {0})
            nxt = [x for x in starts if x > fn]
            if nxt:
                struct.pack_into('<I', r, 0x4b0, 1)            # has-next-scene (UpdateNextScene 0x38418)
                struct.pack_into('<I', r, 0x4c4, nxt[0] - fn)  # distance to it (0x38434)
        return bytes(r)
    fname = {0: 'I?', 1: 'P', 2: 'B', 3: 'IDR', 7: 'Bref'}
    out = []
    for f in range(n):
        # queue = frames f..f+10 (what ProcessFirstPassStats keeps: current + 10 lookahead)
        for k in range(11):
            fn = f + k
            uc.mem_write(MPQ + (fn % 16) * REC, rec(fn if fn < n else n - 1))
        w32(MPQ + 0x6260, 11); w32(MPQ + 0x6264, f % 16); w32(MPQ + 0x6268, (f + 11) % 16)
        fi = SCR; uc.mem_write(fi, bytes(0x40))
        w32(fi + 0, f); w32(fi + 4, 5); w32(fi + 0x1c, f)    # frameNumber, type 5 (undecided), frame index
        res = SCR + 0x100; uc.mem_write(res, bytes(0x40)); outp = SCR + 0x200
        for i, v in enumerate((FT, fi, outp, MPQ, 0)): uc.reg_write(UC_ARM64_REG_X0 + i, v)
        uc.reg_write(UC_ARM64_REG_X8, res)
        uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff000); uc.reg_write(UC_ARM64_REG_X30, TRAP)
        calls.clear()
        try:
            uc.emu_start(addr['__ZN10CFrameType9FrameTypeEP11RCFrameInfoPjR7MPQueueILi16E21_S_AVE_MultiPassStatsE20AVE_VIDEO_CODEC_TYPE'], TRAP, count=5_000_000)
        except UcError as e:
            calls.append('ERR %s pc %#x' % (e, uc.reg_read(UC_ARM64_REG_PC)))
        t = r32(res)
        out.append('%d:%s' % (f, fname.get(t, str(t))))
        if any(c.startswith(('ASSERT', 'ERR', 'UNMAPPED')) for c in calls):
            print('frame', f, calls); break
        if f < 3 or 'v' in opts: print('frame', f, 'type', t, 'res', uc.mem_read(res, 0x14).hex(), 'calls', calls)
    print(' '.join(out))

main()
