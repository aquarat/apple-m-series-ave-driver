#!/usr/bin/env python3
"""The final pass's rate control under emulation (docs/95 §12), H14G, from a live snapshot
(docs/93; S3, a one-reference P session, is the one used).

Per frame, as the firmware runs it: CAVECommonController::GetFrameType (frame type 5: the
MPQueue, the scene ring, CFrameType), the ProcessPipeStart copies (the 0x108-byte header on
frame 0 to ctrl+0x7B4, one scene block to ctrl+0x664, fw 0x45d44..0x45e40),
CRateControl::RateControl (-> ProcessRateControl, the frame's QP), UpdateBits for both
engines, CRateControl::Accumulate (-> MpFinalPassAccumulate). The session is a bitrate
session re-initialised by CRateControl::Init with a patched sCRCInitParams (multipass
enable 1, pass 2).

Bits per frame come from either
  --sizes FILE   the hardware's coded bytes per frame (replay: the QPs must then match the
                 hardware's, --hwqp checks it), or
  --model QP:SIZES [QP:SIZES ...]  a per-frame model log2(bits) = a_f - QP/k fitted on
                 streams of the same clip at different QPs (closed loop).
QP files: one line per frame; the last-but-two column of h264_mbqp's output (mean MB QP)
or a single number per line.

  fpemu.py SNAP TABLE --sizes park.sizes --hwqp park.qp          # replay
  fpemu.py SNAP TABLE --model p1.qp:p1.sizes 2p.qp:2p.sizes vbr.qp:vbr.sizes   # closed loop
"""
import argparse
import math
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, REPO + '/tools/fwemu')
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UcError  # noqa: E402
from unicorn.arm64_const import UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_X0, UC_ARM64_REG_X30, UC_ARM64_REG_PC, \
    UC_ARM64_REG_SP, UC_ARM64_REG_S0  # noqa: E402
from fwmem import Snap  # noqa: E402
from fwsyms import sized  # noqa: E402

FW = os.environ.get('AVE_FW', REPO + '/data/blobs/macos-13.5-j473/ave_h14g.bin')
CTRL = 0xffffffff80803760          # CAVCController in the docs/93 snapshots
PIC0 = 0xffffffff808049b8          # the snapshot frame's PICMGMT
FT = CTRL + 0x1b0                  # CFrameType
MPC = CTRL + 0x418                 # CMultiPassControl
REC = 0x626
STACK, TRAP, SCR, MPBUF, PRM = 0x10000000, 0x20000000, 0x30000000, 0x40000000, 0x50000000
STUBS = ("__ZNK14CAVEFilterBase5Print", "__ZN7CLogger", "__Z17AVE_History_Print", "_printf",
         "__ZN9CTaskPool16GetCurrentTaskID", "_RTK_lock_lock", "_RTK_lock_unlock")   # RTK locks use LSE `cas`

DEFAULTS = dict(w=1920, h=1088, bitrate=8000000, fps=50, nondrop=1, idr=400, keydur=float(2 ** 20),
                qpmin=10, qpmax=51, qp0=26, mp_pass=2, cqp=-1, qpmod=-1, maxqpmod=6, options=0x1305, entropy=2)


class Emu:
    def __init__(self, snap):
        s = Snap(snap)
        syms = sized(FW)
        self.addr = {k: v[0] for k, v in syms.items()}
        uc = self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
        for base, b, _ in s.regions():
            lo, hi = base & ~0xfff, (base + len(b) + 0xfff) & ~0xfff
            uc.mem_map(lo, hi - lo, UC_PROT_ALL)
            uc.mem_write(base, bytes(b))
        for va, sz, pa in s.mmio():
            uc.mem_map(va, sz, UC_PROT_ALL)
        for a, n in ((STACK, 0x100000), (TRAP, 0x1000), (SCR, 0x20000), (MPBUF, 0x10000), (PRM, 0x1000)):
            uc.mem_map(a, n, UC_PROT_ALL)
        uc.mem_write(TRAP, b"\x00\x00\x20\xd4")
        a = self.addr
        self.stubs = {v for n, v in a.items() if n.startswith(STUBS)}
        self.mm = (a['__ZN12MappedMemoryC2Emmb'], a['__ZN12MappedMemoryD1Ev'], a['__ZNK12MappedMemory10HwToTargetEm'])
        self.stop = a['_bsp_assert_fail']
        self.errs = []
        self.probes = {}
        uc.hook_add(UC_HOOK_CODE, self._code)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, lambda u, acc, ad, sz, v, _: (
            self.errs.append('UNMAPPED %#x pc %#x' % (ad, u.reg_read(UC_ARM64_REG_PC))), False)[1])
        self.RC = self.q(CTRL + 0x1a0)

    def _code(self, u, a, sz, _):
        if a in self.probes:
            self.probes[a](self)
        if a == self.stop:
            self.errs.append('ASSERT from %#x' % u.reg_read(UC_ARM64_REG_X30))
            u.emu_stop()
        elif a in self.stubs or a in self.mm[:2]:
            self._ret(0 if a in self.stubs else u.reg_read(UC_ARM64_REG_X0))
        elif a == self.mm[2]:
            self._ret(MPBUF)               # MappedMemory::HwToTarget -> the multipass input buffer

    def _ret(self, v):
        self.uc.reg_write(UC_ARM64_REG_X0, v)
        self.uc.reg_write(UC_ARM64_REG_PC, self.uc.reg_read(UC_ARM64_REG_X30))

    def rd(self, a, n): return bytes(self.uc.mem_read(a, n))
    def wr(self, a, b): self.uc.mem_write(a, bytes(b))
    def u32(self, a): return struct.unpack('<I', self.rd(a, 4))[0]
    def s32(self, a): return struct.unpack('<i', self.rd(a, 4))[0]
    def q(self, a): return struct.unpack('<Q', self.rd(a, 8))[0]
    def sq(self, a): return struct.unpack('<q', self.rd(a, 8))[0]
    def f32(self, a): return struct.unpack('<f', self.rd(a, 4))[0]
    def w32(self, a, v): self.wr(a, struct.pack('<I', v & 0xffffffff))
    def sreg(self, i): return struct.unpack('<f', struct.pack('<I', self.uc.reg_read(UC_ARM64_REG_S0 + i) & 0xffffffff))[0]
    def xreg(self, i): return self.uc.reg_read(UC_ARM64_REG_X0 + i)

    def call(self, fn, args, stack=b''):
        uc = self.uc
        for i, v in enumerate(args):
            uc.reg_write(UC_ARM64_REG_X0 + i, v & 0xffffffffffffffff)
        sp = STACK + 0xf0000
        if stack:
            self.wr(sp, stack)
        uc.reg_write(UC_ARM64_REG_SP, sp)
        uc.reg_write(UC_ARM64_REG_X30, TRAP)
        self.errs = []
        try:
            uc.emu_start(self.addr[fn], TRAP, count=50_000_000)
        except UcError as e:
            self.errs.append('ERR %s pc %#x' % (e, uc.reg_read(UC_ARM64_REG_PC)))
        if self.errs:
            raise RuntimeError('%s: %s' % (fn, self.errs))
        return uc.reg_read(UC_ARM64_REG_X0)


def crc_init_params(e, c):
    """sCRCInitParams (docs/76 §2.1 layout): the snapshot's copy at RC+0x1C0, patched."""
    p = bytearray(e.rd(e.RC + 0x1c0, 0xe8))
    def w(o, v): struct.pack_into('<I', p, o, v & 0xffffffff)
    w(4, c['w']); w(8, c['h']); w(12, 1)                            # ui32RCFlag 1: bitrate
    w(16, c['bitrate']); w(20, c['fps']); w(24, c['nondrop']); w(28, c['idr'])
    w(36, c['qpmin']); w(40, c['qpmax'])
    for o in (92, 96, 100):
        w(o, c['qp0'])
    p[177] = 1 if c['mp_pass'] else 0                              # VP+0xFE9C
    w(180, c['mp_pass']); w(184, c['cqp']); w(188, c['qpmod']); w(192, c['maxqpmod']); w(196, c['options'])
    return bytes(p)


def run(snap, table, bits_of, frames=None, probes=None, **cfg):
    c = dict(DEFAULTS); c.update(cfg)
    e = Emu(snap)
    e.probes = probes or {}
    RC = e.RC
    if e.q(RC + 0x2990) != MPC or e.q(MPC + 0x4a8) != RC:
        raise SystemExit('snapshot: CRateControl/CMultiPassControl not where expected')
    # CFrameType (docs/95 §2.7) and the multipass block (§2.2) as InitEncodingParameters leaves them
    e.w32(FT + 0x80, c['idr']); e.w32(FT + 0x84, c['idr']); e.wr(FT + 0x78, struct.pack('<d', c['keydur']))
    e.w32(FT + 0x98, c['fps']); e.w32(FT + 0x88, 0); e.wr(FT + 2, b'\0')
    e.wr(CTRL + 0x23fc4, b'\x01'); e.w32(CTRL + 0x23fc8, c['mp_pass'])
    for o, k in ((0x23fcc, 'cqp'), (0x23fd0, 'qpmod'), (0x23fd4, 'maxqpmod'), (0x23fd8, 'options')):
        e.w32(CTRL + o, c[k])
    e.w32(CTRL + 0xb0c, 1)
    e.wr(PRM, crc_init_params(e, c))
    e.call('__ZN12CRateControl4InitEP14sCRCInitParamsbh', [RC, PRM, 0, 0])
    hdr = table[:0x108]
    n = (len(table) - 0x108) // REC
    recs = [table[0x108 + i * REC:0x108 + (i + 1) * REC] for i in range(n)]
    PIC = SCR + 0x10000
    e.wr(PIC, e.rd(PIC0, 0x1000))
    stats = CTRL + 0x1918                  # sCRCStatPerFrame
    rows = []
    for f in range(frames or n):
        buf = hdr + (b''.join(recs[min(k, n - 1)] for k in range(11)) if f == 0 else recs[min(f + 10, n - 1)])
        e.wr(MPBUF, buf.ljust(0x4400, b'\0'))
        e.w32(PIC + 0xca8, f); e.w32(PIC + 0xcac, 5); e.w32(PIC + 8, f)
        e.wr(PIC + 0x900, struct.pack('<Q', 0xf0000000)); e.w32(PIC + 0xcb0, 0)
        e.wr(SCR, bytes(0x40))
        e.call('__ZN20CAVECommonController12GetFrameTypeEP18AVE_PICMGMT_PARAMSPjPtS3_S2_',
               [CTRL, PIC, SCR, SCR + 8, SCR + 0x10, SCR + 0x18])
        t = e.u32(PIC + 0xcac)
        st = 2 if t in (2, 3) else 0       # slice type for the controller: I 2, P 0
        e.w32(CTRL + 0x19c0 + 0x60 * e.u32(CTRL + 0xf98), f)
        if f == 0:
            e.wr(CTRL + 0x7b4, hdr)        # -> CMultiPassControl+0x39C
        cnt, rdi = e.u32(CTRL + 0x2b454), e.s32(CTRL + 0x2b458)
        if cnt >= 1:
            ent = CTRL + 0x2a5e4 + (rdi % 11) * 0x150
            if e.u32(ent + 4) <= f:        # rec+0x4B4 <= this frame
                e.wr(CTRL + 0x664, e.rd(ent, 0x150))      # -> CMultiPassControl+0x24C
                e.w32(CTRL + 0x2b454, cnt - 1); e.w32(CTRL + 0x2b458, (rdi + 1) % 11)
        stk = bytearray(0x20); struct.pack_into('<I', stk, 0xc, st)
        qp = e.call('__ZN12CRateControl11RateControlEj9SliceTypejjbbjbbbjiS0_bbj', [RC, f, st, f, f, 1, 0, 0], bytes(stk)) & 0xff
        q_used = e.f32(MPC + 0xc)
        b = int(bits_of(f, qp, t))
        e.wr(stats, bytes(0x28)); e.w32(stats + 4, b); e.w32(stats + 8, b); e.w32(stats + 0x24, c['entropy'])
        for eng in (0, 1):
            e.call('__ZN12CRateControl10UpdateBitsEjjP16sCRCStatPerFrame6Engineb', [RC, f, f, stats, eng, 1])
        stk = bytearray(0x10); struct.pack_into('<I', stk, 0, st)
        e.call('__ZN12CRateControl10AccumulateEj9SliceTypejjP16sCRCStatPerFramebhS0_b', [RC, f, st, f, f, stats, 1, 0], bytes(stk))
        rows.append(dict(f=f, type=t, qp=qp, q=q_used, bits=b,
                         seqF=e.sq(MPC + 0x28) >> 16, seqS=e.sq(MPC + 0x20) >> 16,
                         scnF=e.sq(MPC + 0xb0) >> 16, scnS=e.sq(MPC + 0xa8) >> 16,
                         qlo=e.f32(MPC + 0x98 + 0x7c), qhi=e.f32(MPC + 0x98 + 0x80)))
    return rows


def read_qp(path):
    out = []
    for line in open(path):
        v = line.split()
        if v:
            out.append(float(v[3]) if len(v) >= 6 else float(v[-1]))
    return out


def read_sizes(path):
    return [int(x) for x in open(path).read().split()]


class Model:
    """log2(bits_f) = a_f - QP/k: one global k, one a_f per frame, least squares over the runs."""

    def __init__(self, runs, k=None):
        n = min(len(r[0]) for r in runs)
        if k is None:
            num = den = 0.0
            for f in range(1, n):
                pts = [(r[0][f], math.log2(r[1][f] * 8)) for r in runs]
                mq = sum(p[0] for p in pts) / len(pts); ml = sum(p[1] for p in pts) / len(pts)
                num += sum((p[0] - mq) * (p[1] - ml) for p in pts); den += sum((p[0] - mq) ** 2 for p in pts)
            k = -den / num
        self.k = k
        self.a = [sum(math.log2(r[1][f] * 8) + r[0][f] / k for r in runs) / len(runs) for f in range(n)]

    def bits(self, f, qp, intra=False):
        f = min(f, len(self.a) - 1)
        b = 2 ** (self.a[f] - qp / self.k)
        if intra and f:
            b *= 2 ** (self.a[0] - self.a[1])          # an IDR inserted at f: frame 0's intra/inter ratio
        return b


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('snap'); ap.add_argument('table')
    ap.add_argument('--sizes'); ap.add_argument('--hwqp'); ap.add_argument('--model', nargs='+', metavar='QP:SIZES')
    ap.add_argument('--k', type=float); ap.add_argument('--frames', type=int); ap.add_argument('-v', action='store_true')
    for k, v in DEFAULTS.items():
        ap.add_argument('--' + k, type=float if isinstance(v, float) else lambda x: int(x, 0), default=v)
    a = ap.parse_args()
    cfg = {k: getattr(a, k) for k in DEFAULTS}
    table = open(a.table, 'rb').read()
    if a.sizes:
        sz = read_sizes(a.sizes)
        bits_of = lambda f, qp, t: sz[f] * 8
    elif a.model:
        runs = [(read_qp(m.split(':')[0]), read_sizes(m.split(':')[1])) for m in a.model]
        mdl = Model(runs, a.k)
        print('# model: k = %.2f (bits halve every k QP)' % mdl.k)
        bits_of = lambda f, qp, t: mdl.bits(f, qp, t == 3)
    else:
        sys.exit('need --sizes or --model')
    rows = run(a.snap, table, bits_of, a.frames, **cfg)
    n = len(rows)
    for r in rows if a.v else ():
        print('%4d %s QP %2d q %8.3f bits %8d | sequence buffer %10d/%10d | scene buffer %10d/%10d q %.3f..%.3f' % (
            r['f'], {3: 'IDR', 1: 'P'}.get(r['type'], r['type']), r['qp'], r['q'], r['bits'], r['seqF'], r['seqS'],
            r['scnF'], r['scnS'], r['qlo'], r['qhi']))
    tot = sum(r['bits'] for r in rows) / 8
    tgt = cfg['bitrate'] * n / cfg['fps'] / 8
    step = max(1, n // 5)
    print('QP  ', ' '.join(str(r['qp']) for r in rows))
    print('size %.0f bytes, target %.0f (%+.1f%%); per %d frames: %s MB; IDR at %s' % (
        tot, tgt, 100 * (tot / tgt - 1), step,
        ' '.join('%.2f' % (sum(r['bits'] for r in rows[i:i + step]) / 8e6) for i in range(0, n, step)),
        [r['f'] for r in rows if r['type'] == 3][:10]))
    if a.hwqp:
        hw = read_qp(a.hwqp)
        bad = [r['f'] for r in rows if r['qp'] != round(hw[r['f']])]
        print('against %s: %d of %d frame QPs differ %s' % (a.hwqp, len(bad), n, bad[:20]))


if __name__ == '__main__':
    main()
