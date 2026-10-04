#!/usr/bin/env python3
"""Run macOS 13.5's own multi-pass statistics code under Unicorn (docs/95 §2.4).

The user-space encoder (AppleVideoEncoder.bundle, arm64e Mach-O) is mapped
at SLIDE, its chained fixups applied, and its imports replaced by Python
stubs (memcpy, operator new, CFData, VTMultiPassStorageSetDataAtTimeStamp,
printf/syslog). The functions called are exactly the ones macOS calls:

  0x9c140  MP constructor (pool of 16 records, two deques, header zeroed)
  0x94f6c  per pass-1 record (SendFrame UA 0x970d0): take a free record,
           copy the firmware's 0x626 bytes into it, enqueue_first_pass()
  0xba86c  FlushStats (on DataType_RESETMULTIPASS, UA 0x8b1d8): pushes
           display_order -1 records until the pipeline is empty, stores
           each record with VTMultiPassStorageSetDataAtTimeStamp, then
           resets the pool (0x9c220) and runs FinalizeSeqRcInfo (0xb9ff4)

The binary is not in git; see tools/fetch_userspace.py (docs/72) and P below.
"""
import os
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_ALL, UcError
from unicorn.arm64_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
P = os.environ.get("AVE_USERSPACE_BIN", os.path.join(
    REPO, "data/blobs/macos-13.5-userspace/fs/System/Library/Video/Plug-Ins/"
    "AppleVideoEncoder.bundle/Contents/MacOS/AppleVideoEncoder"))
SHA256 = "b1f8fd38242dfa532d9de56f68cc0f2d46311df10e7071bc4abd55ee834da803"

SLIDE = 0x100000000          # image base (the file's __TEXT is at VA 0)
IMPORTS = 0x180000000        # one 16-byte slot per imported symbol (data imports read here)
HEAP, HEAP_LEN = 0x200000000, 0x4000000
STACK, STACK_LEN = 0x300000000, 0x100000
TRAP = 0x310000000           # return address of every call

# UA addresses (VA == file offset)
UA_MP_CTOR = 0x9c140         # (MP)
UA_SEND_STATS = 0x94f6c      # (FrameReceiver = MP - 8, src record) -> record leaving the pipeline or NULL
UA_FLUSH = 0xba86c           # FlushStats(MP, storage, ...)
UA_LOG_ENABLED = 0xb78f8     # (module, level) -> bool; reads a table in __bss (0 = off)
MP_HDR = 0x6398              # sequence RC info, 0x108 bytes (copier UA 0x899d4 reads FrameReceiver+0x63A0)
MP_SIZE = 0x6600             # covers everything the MP code touches (last field MP+0x64A8)
REC = 0x626
HDR = 0x108


def _u32(b, o): return struct.unpack_from("<I", b, o)[0]


class MachO:
    """Segments and chained-fixup imports of the arm64e bundle (pointer format 12)."""

    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        ncmds = _u32(d, 16)
        o = 32
        self.segs = []          # (name, vmaddr, vmsize, fileoff, filesize)
        self.sects = {}
        fixups = None
        for _ in range(ncmds):
            cmd, cs = struct.unpack_from("<II", d, o)
            if cmd == 0x19:
                name = d[o + 8:o + 24].rstrip(b"\0").decode()
                va, vs, fo, fs = struct.unpack_from("<QQQQ", d, o + 24)
                ns = _u32(d, o + 64)
                self.segs.append((name, va, vs, fo, fs))
                for j in range(ns):
                    so = o + 72 + 80 * j
                    sn = d[so:so + 16].rstrip(b"\0").decode()
                    a, s = struct.unpack_from("<QQ", d, so + 32)
                    self.sects[sn] = (a, s)
            elif cmd == 0x80000034:      # LC_DYLD_CHAINED_FIXUPS
                fixups = struct.unpack_from("<II", d, o + 8)
            o += cs
        self.fixups = self._chained(fixups[0])

    def _chained(self, B):
        d = self.d
        _, starts, imps, syms, icount, ifmt, _ = struct.unpack_from("<IIIIIII", d, B)
        assert ifmt == 2, ifmt   # DYLD_CHAINED_IMPORT_ADDEND
        names = []
        for i in range(icount):
            v = _u32(d, B + imps + 8 * i)
            s = B + syms + (v >> 9)
            names.append(d[s:d.index(b"\0", s)].decode())
        self.import_names = names
        out = {}
        segc = _u32(d, B + starts)
        for si in range(segc):
            so = _u32(d, B + starts + 4 + 4 * si)
            if so == 0:
                continue
            A = B + starts + so
            _, psize, pfmt, _, _, pcount = struct.unpack_from("<IHHQIH", d, A)
            assert pfmt == 12, pfmt  # DYLD_CHAINED_PTR_ARM64E_USERLAND24
            for pi, ps in enumerate(struct.unpack_from("<%dH" % pcount, d, A + 22)):
                if ps == 0xFFFF:
                    continue
                off = self.segs[si][3] + pi * psize + ps
                while True:
                    v = struct.unpack_from("<Q", d, off)[0]
                    auth, bind, nxt = v >> 63, (v >> 62) & 1, (v >> 51) & 0x7FF
                    if bind:
                        out[off] = ("bind", v & 0xFFFFFF)
                    else:
                        out[off] = ("rebase", (v & 0xFFFFFFFF) if auth else (v & 0x7FFFFFFFFFF))
                    if nxt == 0:
                        break
                    off += nxt * 8
        return out

    def stubs(self):
        """__auth_stubs entry VA -> imported symbol name."""
        a0, n = self.sects["__auth_stubs"]
        d, out = self.d, {}
        for x in range(a0, a0 + n, 16):
            w0, w1 = struct.unpack_from("<II", d, x)
            imm = ((((w0 >> 5) & 0x7FFFF) << 2) | ((w0 >> 29) & 3)) << 12
            got = (x & ~0xFFF) + imm + ((w1 >> 10) & 0xFFF)
            if (w1 & 0xFFC00000) != 0x91000000:     # ldr x16, [x17, #imm] form
                got = (x & ~0xFFF) + imm + ((w1 >> 10) & 0xFFF) * 8
            f = self.fixups.get(got)
            out[x] = self.import_names[f[1]] if f and f[0] == "bind" else "?%x" % got
        return out


def _cfmt(fmt, args, strfn=lambda v: "<s>"):
    """Enough of printf for the MP log lines (Darwin arm64: varargs on the stack)."""
    out, i, k = [], 0, 0
    while i < len(fmt):
        c = fmt[i]
        if c != "%":
            out.append(c); i += 1; continue
        j = i + 1
        while j < len(fmt) and fmt[j] in "0123456789.-+ #lhzjtq":
            j += 1
        conv, spec = fmt[j], fmt[i:j + 1]
        i = j + 1
        if conv == "%":
            out.append("%"); continue
        v = args(k); k += 1
        if conv in "fFeEgG":
            out.append(spec.replace("l", "") % struct.unpack("<d", struct.pack("<Q", v))[0])
        elif conv == "s":
            out.append(strfn(v))
        elif conv == "p":
            out.append("0x%x" % v)
        elif conv in "di":
            bits = 64 if "l" in spec or "z" in spec else 32
            v &= (1 << bits) - 1
            if v >> (bits - 1):
                v -= 1 << bits
            out.append(spec.replace("l", "").replace("z", "") % v)
        else:
            bits = 64 if "l" in spec or "z" in spec else 32
            out.append(spec.replace("l", "").replace("z", "") % (v & ((1 << bits) - 1)))
    return "".join(out)


class MPEmu:
    def __init__(self, path=P, log=None):
        if not os.path.exists(path):
            raise FileNotFoundError("AppleVideoEncoder not found at %s (tools/fetch_userspace.py, docs/72)" % path)
        self.m = MachO(path)
        self.log = log                     # None, or a callable(str) to receive the MP: log lines
        uc = self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)     # pacibsp/retab/autibsp
        top = max(va + vs for _, va, vs, _, _ in self.m.segs if va + vs <= 0x200000)
        uc.mem_map(SLIDE, (top + 0xFFF) & ~0xFFF, UC_PROT_ALL)
        for name, va, vs, fo, fs in self.m.segs:
            if name == "__LINKEDIT":
                continue
            uc.mem_write(SLIDE + va, self.m.d[fo:fo + fs])
        uc.mem_map(IMPORTS, 0x10000, UC_PROT_ALL)
        for off, (kind, t) in self.m.fixups.items():
            # file offset == VA in this image
            val = SLIDE + t if kind == "rebase" else IMPORTS + 16 * t
            uc.mem_write(SLIDE + off, struct.pack("<Q", val))
        uc.mem_map(HEAP, HEAP_LEN, UC_PROT_ALL)
        uc.mem_map(STACK, STACK_LEN, UC_PROT_ALL)
        uc.mem_map(TRAP, 0x1000, UC_PROT_ALL)
        uc.mem_write(TRAP, b"\x00\x00\x20\xd4")    # brk #0
        self.brk = HEAP
        self.cfdata = {}
        self.stored = []                           # (CMTime bytes, data) from VTMultiPassStorageSetDataAtTimeStamp
        self.error = None
        self.stub_names = self.m.stubs()
        a0, n = self.m.sects["__auth_stubs"]
        uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=SLIDE + a0, end=SLIDE + a0 + n - 1)
        uc.hook_add(UC_HOOK_CODE, self._on_log_enabled, begin=SLIDE + UA_LOG_ENABLED, end=SLIDE + UA_LOG_ENABLED)

    # ---- memory helpers
    def alloc(self, n, zero=True):
        a = (self.brk + 15) & ~15
        self.brk = a + n
        assert self.brk < HEAP + HEAP_LEN, "emulated heap exhausted"
        if zero:
            self.uc.mem_write(a, b"\0" * n)
        return a

    def rd(self, a, n): return bytes(self.uc.mem_read(a, n))
    def wr(self, a, b): self.uc.mem_write(a, bytes(b))
    def x(self, i): return self.uc.reg_read(UC_ARM64_REG_X0 + i)

    def _cstr(self, a):
        b = self.rd(a, 512)
        return b[:b.index(b"\0")].decode("latin-1")

    # ---- hooks
    def _ret(self, v=None):
        uc = self.uc
        if v is not None:
            uc.reg_write(UC_ARM64_REG_X0, v & 0xFFFFFFFFFFFFFFFF)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def _on_log_enabled(self, uc, addr, size, _):
        # 0 is what the zeroed __bss log-level table answers; 1 turns the MP: log lines on
        self._ret(0 if self.log is None else 1)

    def _on_stub(self, uc, addr, size, _):
        name = self.stub_names.get(addr - SLIDE, "?")
        x = self.x
        if name in ("_memcpy", "_memmove"):
            dst, src, n = x(0), x(1), x(2)
            if n:
                self.wr(dst, self.rd(src, n))
            return self._ret(dst)
        if name == "_memset":
            self.wr(x(0), bytes([x(1) & 0xFF]) * x(2)); return self._ret(x(0))
        if name == "_bzero":
            self.wr(x(0), b"\0" * x(1)); return self._ret(0)
        if name == "__Znwm":
            return self._ret(self.alloc(x(0), zero=False))
        if name in ("__ZdlPv", "_CFRelease"):
            return self._ret(0)
        if name == "_CFDataCreateMutable":
            h = self.alloc(16)
            self.cfdata[h] = b""
            return self._ret(h)
        if name == "_CFDataAppendBytes":
            self.cfdata[x(0)] += self.rd(x(1), x(2)); return self._ret(0)
        if name == "_VTMultiPassStorageSetDataAtTimeStamp":
            self.stored.append((self.rd(x(1), 24), self.cfdata[x(3)]))
            return self._ret(0)
        if name in ("_printf", "_syslog$DARWIN_EXTSN"):
            if self.log is not None:
                fa = 0 if name == "_printf" else 1
                sp = uc.reg_read(UC_ARM64_REG_SP)
                fmt = self._cstr(x(fa))
                def arg(k, sp=sp, fmt=fmt):
                    return struct.unpack("<Q", self.rd(sp + 8 * k, 8))[0]
                s = _cfmt(fmt, arg, self._cstr)
                if name == "_syslog$DARWIN_EXTSN":
                    self.log(s.rstrip("\n"))
            return self._ret(0)
        if name in ("_clock_gettime_nsec_np",):
            return self._ret(0)
        self.error = "unexpected call to %s from 0x%x" % (name, uc.reg_read(UC_ARM64_REG_LR) - SLIDE)
        uc.emu_stop()

    # ---- calling convention
    def call(self, ua, *args):
        uc = self.uc
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM64_REG_X0 + i, a)
        uc.reg_write(UC_ARM64_REG_SP, STACK + STACK_LEN - 0x100)
        uc.reg_write(UC_ARM64_REG_LR, TRAP)
        self.error = None
        try:
            uc.emu_start(SLIDE + ua, TRAP, count=50_000_000)
        except UcError as e:
            raise RuntimeError("emulation fault at UA 0x%x: %s" %
                               (uc.reg_read(UC_ARM64_REG_PC) - SLIDE, e))
        if self.error:
            raise RuntimeError(self.error)
        if uc.reg_read(UC_ARM64_REG_PC) != TRAP:
            raise RuntimeError("did not return (pc UA 0x%x)" % (uc.reg_read(UC_ARM64_REG_PC) - SLIDE))
        return self.x(0)


class EmuSession:
    """One first pass: feed records in arrival (coding) order, then finish()."""

    def __init__(self, path=P, log=None):
        self.e = e = MPEmu(path, log)
        self.fr = e.alloc(8 + MP_SIZE)          # FrameReceiver; the MP object is at +8
        self.mp = self.fr + 8
        self.src = e.alloc(REC)
        e.call(UA_MP_CTOR, self.mp)
        self.emitted = []                       # what SendFrame stores, in emission order

    def send(self, rec):
        assert len(rec) == REC
        e = self.e
        e.wr(self.src, rec)
        p = e.call(UA_SEND_STATS, self.fr, self.src)
        if p:
            self.emitted.append(e.rd(p, REC))
            return self.emitted[-1]
        return None

    def finish(self):
        e = self.e
        e.stored = []
        e.call(UA_FLUSH, self.mp, 0x5354)      # storage handle: any non-zero value
        self.emitted += [d for _, d in e.stored]
        return e.rd(self.mp + MP_HDR, HDR), list(self.emitted)

    def state(self, off, n):
        return self.e.rd(self.mp + off, n)


def run(records, path=P, log=None):
    """records: list of 0x626-byte pass-1 records in arrival order.
    Returns (header, emitted records in emission order)."""
    s = EmuSession(path, log)
    for r in records:
        s.send(r)
    return s.finish()
