#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pure-Python port of macOS 13.5's multi-pass statistics code (docs/95 §2.4).

A line-by-line port of AppleVideoEncoder (UA = file offset):

  get_free_record    0x94fcc   pool of 16 records, LIFO
  enqueue_first_pass 0xb9a08   2-deep fixup FIFO, then a min-heap on display order
  bits_correction    0xb97b4
  scene_change_pipeline 0xb9130 (histogram_diff 0xb8270, scene_change_detect 0xb8398)
  accumulate_scene_info 0xb84e8
  FlushStats         0xba86c
  FinalizeSeqRcInfo  0xb9ff4 (QuantizeData 0xba6a0, std::sort 0xbb550 / 0xbc194)

Records are bytearrays in a pool of 16 slots, as in the original, because
the code reads stale bytes of recycled slots (FlushStats sets only
display_order on its padding records, and their rec+0x48 then corrects the
second-to-last frame). Arithmetic keeps the widths of the code: u32/u64
wrap-around, float32 where the code uses s registers (including one fused
multiply-add), float64 where it uses d registers, and AArch64 NaN
semantics for every floating compare.
"""
import math
import struct
from fractions import Fraction

REC = 0x626
HDR = 0x108
POOL = 16
M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF


# ---------------------------------------------------------------- numerics
# Floats are Python floats (doubles). A float32 NaN is held as the double
# with the same sign and payload (the payload shifted up 29 bits, quiet bit
# unchanged), so loads and stores are exact bit moves, as ldr/str s are.
# Every arithmetic helper applies the AArch64 rules (FPCR.DN = 0): a
# signalling NaN operand wins and is quietened, else the first quiet NaN
# operand; an invalid operation on non-NaN operands gives the default NaN
# (+0x7FC00000 / +0x7FF8000000000000). That keeps results identical on
# any host CPU.
def _b64(x): return struct.unpack("<Q", struct.pack("<d", x))[0]
def _d64(b): return struct.unpack("<d", struct.pack("<Q", b))[0]


DNAN = _d64(0x7FF8000000000000)


def isnan(x): return x != x
def _snan(x): return x != x and not (_b64(x) >> 51) & 1
def quiet(x): return _d64(_b64(x) | (1 << 51)) if x != x else x


def _nans(*ops):
    for o in ops:
        if _snan(o):
            return quiet(o)
    for o in ops:
        if o != o:
            return o
    return None


def ld32(b, o):
    v = struct.unpack_from("<I", b, o)[0]
    if (v >> 23) & 0xFF == 0xFF and v & 0x7FFFFF:
        return _d64(((v >> 31) << 63) | (0x7FF << 52) | ((v & 0x7FFFFF) << 29))
    return struct.unpack_from("<f", b, o)[0]


def st32(b, o, x):
    if x != x:
        q = _b64(x)
        struct.pack_into("<I", b, o, ((q >> 63) << 31) | (0xFF << 23) | ((q >> 29) & 0x7FFFFF))
    else:
        struct.pack_into("<f", b, o, x)


def f32(x):
    """fcvt s, d: round a double to float32 (RNE; NaN payload kept, quietened)."""
    if x != x:
        return quiet(_d64(_b64(x) & ~((1 << 29) - 1)))
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def cvt_sd(x):          # fcvt d, s (and fcvtl): exact, quietens a signalling NaN
    return quiet(x)


def u32f(v):            # ucvtf s, w
    return f32(float(v & M32))


def _op(a, b, fn, single):
    n = _nans(a, b)
    if n is not None:
        return n
    try:
        r = fn(a, b)
    except ZeroDivisionError:
        r = DNAN if a == 0 else math.copysign(math.inf, a) * math.copysign(1.0, b)
    if r != r:
        return DNAN
    return f32(r) if single else r


def fadd32(a, b): return _op(a, b, lambda x, y: x + y, True)
def fsub32(a, b): return _op(a, b, lambda x, y: x - y, True)
def fmul32(a, b): return _op(a, b, lambda x, y: x * y, True)
def fdiv32(a, b): return _op(a, b, lambda x, y: x / y, True)
def fadd64(a, b): return _op(a, b, lambda x, y: x + y, False)
def fdiv64(a, b): return _op(a, b, lambda x, y: x / y, False)


def fmax32(a, b):       # fmax: NaN-propagating
    n = _nans(a, b)
    if n is not None:
        return n
    if a == 0 and b == 0:
        return b if math.copysign(1, a) < 0 else a
    return a if a > b else b


def fmaxnm32(a, b):     # fmaxnm: a single quiet NaN loses to a number
    if a != a and not _snan(a) and not (b != b):
        a = -math.inf
    elif b != b and not _snan(b) and not (a != a):
        b = -math.inf
    return fmax32(a, b)


def fma32(addend, n, m):
    """fmadd s: addend + n*m with one rounding to float32."""
    r = _nans(addend, n, m)
    inv = (math.isinf(n) and m == 0) or (n == 0 and math.isinf(m))
    if r is not None:
        return DNAN if (addend != addend and not _snan(addend) and inv) else r
    if inv:
        return DNAN
    if math.isinf(n) or math.isinf(m) or math.isinf(addend):
        p = n * m
        s = p + addend
        return DNAN if s != s else s
    x = Fraction(n) * Fraction(m) + Fraction(addend)
    if x == 0:
        neg = math.copysign(1, n) * math.copysign(1, m) < 0 and math.copysign(1, addend) < 0
        return -0.0 if neg else 0.0
    sign = -1.0 if x < 0 else 1.0
    x = abs(x)
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if Fraction(2) ** e > x:
        e -= 1
    e = max(e, -126)
    q = Fraction(2) ** (e - 23)
    t = x / q
    i = t.numerator // t.denominator
    rem = t - i
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and i & 1):
        i += 1
    v = i * q
    return sign * (math.inf if v >= Fraction(2) ** 128 else float(v))


def fcvtzu32(x):        # fcvtzu w, s (saturating, NaN -> 0)
    if x != x or x <= -1.0:
        return 0
    if x >= 4294967296.0:
        return M32
    return int(x) & M32


def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


def s64(v):
    v &= M64
    return v - (1 << 64) if v >> 63 else v


# AArch64 condition codes after fcmp a, b (unordered sets C and V)
def c_mi(a, b): return a < b                       # N
def c_pl(a, b): return not (a < b)                 # !N  (true when unordered)
def c_le(a, b): return a <= b or a != a or b != b  # Z | N!=V (true when unordered)
def c_gt(a, b): return a > b
def c_ls(a, b): return a <= b                      # !C | Z (false when unordered)


# ---------------------------------------------------------------- record access
class Rec:
    """A pool slot: 0x626 bytes and typed accessors."""
    __slots__ = ("b", "slot")

    def __init__(self, slot):
        self.b = bytearray(REC)
        self.slot = slot

    def u32(self, o): return struct.unpack_from("<I", self.b, o)[0]
    def s32(self, o): return struct.unpack_from("<i", self.b, o)[0]
    def u16(self, o): return struct.unpack_from("<H", self.b, o)[0]
    def u64(self, o): return struct.unpack_from("<Q", self.b, o)[0]
    def f(self, o): return ld32(self.b, o)
    def d(self, o): return struct.unpack_from("<d", self.b, o)[0]
    def set32(self, o, v): struct.pack_into("<I", self.b, o, v & M32)
    def set64(self, o, v): struct.pack_into("<Q", self.b, o, v & M64)
    def setf(self, o, v): st32(self.b, o, v)
    def setd(self, o, v): struct.pack_into("<d", self.b, o, v)
    def add32(self, o, v): self.set32(o, self.u32(o) + v)
    def add64(self, o, v): self.set64(o, self.u64(o) + v)

    @property
    def fn(self): return self.u32(0x2C)       # display order, compared unsigned


# ---------------------------------------------------------------- header
class Header:
    """MP+0x6398, the 0x108-byte sequence RC info (field names from UA 0xb88c0)."""

    def __init__(self):
        self.b = bytearray(HDR)

    def u32(self, o): return struct.unpack_from("<I", self.b, o)[0]
    def u64(self, o): return struct.unpack_from("<Q", self.b, o)[0]
    def f(self, o): return ld32(self.b, o)
    def d(self, o): return struct.unpack_from("<d", self.b, o)[0]
    def set32(self, o, v): struct.pack_into("<I", self.b, o, v & M32)
    def set64(self, o, v): struct.pack_into("<Q", self.b, o, v & M64)
    def setf(self, o, v): st32(self.b, o, v)
    def setd(self, o, v): struct.pack_into("<d", self.b, o, v)


# ---------------------------------------------------------------- libc++ std::sort (len <= 30)
def _sort3(a, i, j, k, lt):
    if not lt(a[j], a[i]):
        if not lt(a[k], a[j]):
            return
        a[j], a[k] = a[k], a[j]
        if lt(a[j], a[i]):
            a[i], a[j] = a[j], a[i]
        return
    if lt(a[k], a[j]):
        a[i], a[k] = a[k], a[i]
        return
    a[i], a[j] = a[j], a[i]
    if lt(a[k], a[j]):
        a[j], a[k] = a[k], a[j]


def _sort4(a, i, j, k, l, lt):
    _sort3(a, i, j, k, lt)
    if lt(a[l], a[k]):
        a[k], a[l] = a[l], a[k]
        if lt(a[k], a[j]):
            a[j], a[k] = a[k], a[j]
            if lt(a[j], a[i]):
                a[i], a[j] = a[j], a[i]


def _sort5(a, i, j, k, l, m, lt):
    _sort4(a, i, j, k, l, lt)
    if lt(a[m], a[l]):
        a[l], a[m] = a[m], a[l]
        if lt(a[l], a[k]):
            a[k], a[l] = a[l], a[k]
            if lt(a[k], a[j]):
                a[j], a[k] = a[k], a[j]
                if lt(a[j], a[i]):
                    a[i], a[j] = a[j], a[i]


def cxx_sort(a, lt):
    n = len(a)
    assert n <= 30, "only the insertion-sort range of libc++'s introsort is ported"
    if n < 2:
        return
    if n == 2:
        if lt(a[1], a[0]):
            a[0], a[1] = a[1], a[0]
        return
    if n == 3:
        return _sort3(a, 0, 1, 2, lt)
    if n == 4:
        return _sort4(a, 0, 1, 2, 3, lt)
    if n == 5:
        return _sort5(a, 0, 1, 2, 3, 4, lt)
    _sort3(a, 0, 1, 2, lt)                       # __insertion_sort_3
    for i in range(3, n):
        if lt(a[i], a[i - 1]):
            t = a[i]
            j = i
            while True:
                a[j] = a[j - 1]
                j -= 1
                if j == 0 or not lt(t, a[j - 1]):
                    break
            a[j] = t


# ---------------------------------------------------------------- the MP object
class MP:
    def __init__(self):
        self.slots = [Rec(i) for i in range(POOL)]
        self.reset_pool()
        self.hdr = Header()          # MP+0x6398 (bzero at 0x9c200)
        self.expected = 0            # MP+0x6388 next display order the heap may release
        self.cur_scene = None        # MP+0x6390 first record of the open scene
        self.qsum = 0.0              # MP+0x64A0 sum of rec+0x614 (double)
        self.trace = None

    def reset_pool(self):            # 0x9c220
        self.pool = list(self.slots)  # MP+0x6268: pool[i] = MP + 2 + i*0x626
        self.count = POOL            # MP+0x62E8
        self.fifo = [None, None]     # MP+0x62F0
        self.fifo_len = 0            # MP+0x6300
        self.fifo_idx = 0            # MP+0x6304
        self.heap = []               # MP+0x6308 std::vector<stats*>
        self.d1 = []                 # MP+0x6328 std::deque<stats*>, the 4-deep window
        self.d2 = []                 # MP+0x6358 std::deque<stats*>, held scene starts

    # -- pool (0x94fcc and the put-back at 0xb9f24)
    def get_free(self):
        if self.count == 0:
            raise RuntimeError("free_pool_available > 0 failed")
        self.count -= 1
        return self.pool[self.count]

    def put_free(self, r):
        c = self.count
        if c < POOL:
            self.pool[c] = r
        self.count = POOL if c >= POOL else c + 1

    # -- heap (libc++ push_heap 0xbb428, pop_heap/floyd 0xbb48c), min on unsigned display order
    def heap_push(self, r):
        h = self.heap
        h.append(r)
        i = len(h) - 1
        if i < 1:
            return
        p = (i - 1) // 2
        if not (h[p].fn > r.fn):
            return
        while True:
            h[i] = h[p]
            i = p
            if i == 0:
                break
            p = (i - 1) // 2
            if not (h[p].fn > r.fn):
                break
        h[i] = r

    def heap_pop(self):
        h = self.heap
        n = len(h)
        top = h[0]
        if n > 1:
            # __floyd_sift_down: move the hole to a leaf, put the last element there, sift it up
            hole, i = 0, 0
            last = (n - 2) // 2
            while True:
                c = 2 * i + 1
                best = h[c]
                if c + 1 < n and best.fn > h[c + 1].fn:
                    c += 1
                    best = h[c]
                h[hole] = best
                hole = i = c
                if i > last:
                    break
            if hole == n - 1:
                h[hole] = top
            else:
                h[hole] = h[n - 1]
                h[n - 1] = top
                # push_heap on [0, hole+1)
                r = h[hole]
                k = hole
                if k >= 1:
                    p = (k - 1) // 2
                    if h[p].fn > r.fn:
                        while True:
                            h[k] = h[p]
                            k = p
                            if k == 0:
                                break
                            p = (k - 1) // 2
                            if not (h[p].fn > r.fn):
                                break
                        h[k] = r
        h.pop()
        return top

    # -- 0xb97b4
    def bits_correction(self, rec, corr):
        if rec is None or corr == 0:
            return
        corr = s32(corr)
        fb = rec.s32(0x40)
        if s32(fb + corr) < 1:
            return
        prod = s64((rec.u32(0x44)) * corr)
        if fb == 0:
            q = 0
        else:
            q = abs(prod) // abs(fb)
            if (prod < 0) != (fb < 0):
                q = -q
            q = s64(q)
        hc = s32(q)
        rec.add64(0x4CC, corr)
        rec.add64(0x4DC, hc)
        rec.set32(0x40, fb + corr)
        rec.set32(0x44, rec.u32(0x44) + hc)
        cls = rec.u32(0x34)
        if cls == 0:
            rec.add64(0x4EC, hc)
        elif cls == 2:
            rec.add64(0x4E4, hc)
        for o in (0x524, 0x52C, 0x534, 0x53C):
            if rec.u64(o):
                rec.add64(o, corr)

    # -- 0xb9a08 (the callers in 13.5 always pass flush = 0)
    def enqueue_first_pass(self, rec):
        if self.fifo_len == 0:
            self.fifo[self.fifo_idx] = rec
            self.fifo_len = 1
            return None
        if self.fifo_len == 1:
            self.fifo[1 if self.fifo_idx == 0 else 0] = rec
            self.fifo_len = 2
            return None
        if self.fifo_len != 2:
            out = None
        else:
            old = self.fifo[self.fifo_idx]
            self.bits_correction(old, rec.u32(0x48))
            self.heap_push(old)
            self.fifo[self.fifo_idx] = rec
            self.fifo_idx = 1 if self.fifo_idx == 0 else 0
        top = self.heap[0]
        fn = top.fn
        if fn != M32 and fn != self.expected:
            return None
        self.expected = (self.expected + 1) & M32
        top = self.heap_pop()
        out = self.scene_change_pipeline(top)
        if out is not None:
            self.put_free(out)
        return out

    # -- 0xb8270
    @staticmethod
    def histogram_diff(a, b):
        d0, d8, sa = 0.0, 0.0, 0
        for i in range(256):
            x = a.u32(0xB0 + 4 * i)
            sa = (sa + x) & M32
            d0 = d0 + float(x)
            d0 = d0 - float(b.u32(0xB0 + 4 * i))
            d8 = d8 + (d0 if d0 >= 0.0 else -d0)
        return f32(fdiv64(d8, float(sa))), d8, sa

    # -- 0xb8398
    @staticmethod
    def scene_change_detect(m1, m0m2, rp, rn):
        if not c_ls(m0m2, 0.00272072):
            if not c_ls(m1, 71.58768845):
                w8 = c_gt(rp, 4.51769352)
                w9 = c_ls(m0m2, 0.03005953)
            else:
                w8 = c_gt(rp, 23.24848175)
                w9 = c_gt(m1, 26.7539587)
        else:
            w8 = c_ls(rn, 0.96605313)
            w9 = c_gt(rp, 1.34009841)
        return bool(w8 and w9)

    # -- 0xb9130
    def scene_change_pipeline(self, rec):
        if self.cur_scene is None:
            rec.set32(0x4B0, 1)
            self.cur_scene = rec
            self.d2.append(rec)
        d1 = self.d1
        d1.append(rec)
        n = len(d1)
        if n <= 1:
            rec.set64(0x4B8, 0)
            return None
        prev = d1[n - 2]
        if rec.fn == M32:
            s0 = prev.f(0x4B8)
        else:
            s8 = fmul32(fmax32(fadd32(rec.f(0x4C0), prev.f(0x4C0)), 1.0), 0.001953125)
            h, _, _ = self.histogram_diff(rec, prev)
            s0 = fmaxnm32(fdiv32(h, s8), f32(0.01))
        s1 = prev.f(0x4B8)
        s1 = s1 if c_mi(s0, s1) else s0
        rec.setf(0x4B8, s0)
        rec.setf(0x4BC, s1)
        if n < 4:
            if n == 2:
                d1[0].b[0x4B8:0x4C0] = d1[1].b[0x4B8:0x4C0]
                self.accumulate_scene_info(d1[0])
            return None
        cand = d1[n - 3]
        m0 = d1[n - 4].f(0x4BC)
        m1 = cand.f(0x4B8)
        m2 = d1[n - 1].f(0x4BC)
        rp = fdiv32(m1, m0)
        rn = fdiv32(m2, m1)
        m0m2 = fdiv32(rn, rp)
        if rec.fn == M32 or cand.fn < 3:
            cand.set32(0x4B0, cand.u32(0x50) & 1)
            self.accumulate_scene_info(cand)
            cand.set32(0x4B0, 1 if cand.fn == M32 else (1 if cand.u32(0x4B0) else 0))
        else:
            sc = self.scene_change_detect(m1, m0m2, rp, rn)
            cand.set32(0x4B0, 1 if sc else (cand.u32(0x50) & 1))
            self.accumulate_scene_info(cand)
        if self.trace:
            self.trace("scene_change_pipeline() display_order %d forceKeyFrame %d scene_change %d"
                       % (s32(cand.fn), cand.u32(0x50) & 1, cand.u32(0x4B0)))
        if cand.u32(0x4B0):
            self.d2.append(cand)
            self.cur_scene = cand
        out = d1.pop(0)
        if not out.u32(0x4B0):
            return out
        front = self.d2[0]
        if front is self.cur_scene:
            return None
        self.d2.pop(0)
        return front

    # -- 0xb84e8
    def accumulate_scene_info(self, rec):
        if rec.fn == M32:
            return
        H = self.hdr
        cnt = (H.u32(0x04) + 1) & M32
        H.set32(0x04, cnt)
        if rec.u32(0x4B0):
            H.set32(0x00, H.u32(0x00) + 1)
        bits = rec.u32(0x40)
        H.set64(0x08, H.u64(0x08) + bits)
        q = rec.f(0x614)
        if rec.u32(0x34) == 2:
            H.set64(0x14, H.u64(0x14) + bits)
            H.set32(0x10, H.u32(0x10) + 1)
            H.setd(0x50, fadd64(H.d(0x50), cvt_sd(q)))
        self.qsum = fadd64(self.qsum, cvt_sd(q))
        H.setf(0x4C, f32(fdiv64(self.qsum, float(cnt))))
        H.setd(0x58, fadd64(H.d(0x58), cvt_sd(rec.f(0x618))))
        H.setd(0x60, fadd64(H.d(0x60), cvt_sd(rec.f(0x61C))))
        for i in range(16):
            H.set32(0x68 + 4 * i, H.u32(0x68 + 4 * i) + rec.u32(0x574 + 4 * i))
            H.setf(0xA8 + 4 * i, fadd32(rec.f(0x5B4 + 4 * i), H.f(0xA8 + 4 * i)))
        cls = rec.u16(0x624)
        if cls <= 3:
            c_off, b_off = ((0x1C, 0x20), (0x28, 0x2C), (0x34, 0x38), (0x40, 0x44))[cls]
            H.set64(b_off, H.u64(b_off) + bits)
            H.set32(c_off, H.u32(c_off) + 1)
        if not rec.u32(0x4B0):
            S = self.cur_scene
            old, r = S.u32(0x4C4), rec.u32(0x4C4)
            s1, s3 = u32f(old), u32f(r)
            S.set32(0x4C4, old + r)
            S.set32(0x4C8, S.u32(0x4C8) + rec.u32(0x4C8))
            s0 = u32f(old + r)
            for o in (0x4CC, 0x4D4, 0x4DC, 0x4E4, 0x4EC, 0x50C):
                S.add64(o, rec.u64(o))
            s3 = fmul32(rec.f(0x4C0), s3)
            s1 = fma32(s3, S.f(0x4C0), s1)
            S.setf(0x4C0, fdiv32(s1, s0))
            for o in (0x4F4, 0x4FC, 0x544, 0x54C, 0x554, 0x55C):
                S.setd(o, fadd64(rec.d(o), S.d(o)))
            a, b = rec.f(0x504), S.f(0x504)
            S.setf(0x504, a if c_mi(a, b) else b)
            a, b = S.f(0x508), rec.f(0x508)
            S.setf(0x508, b if c_mi(a, b) else a)
            for i in range(4):
                S.add32(0x514 + 4 * i, rec.u32(0x514 + 4 * i))
            for o in (0x524, 0x52C, 0x534, 0x53C):
                S.add64(o, rec.u64(o))
            S.setd(0x564, fadd64(S.d(0x564), cvt_sd(rec.f(0x618))))
            S.setd(0x56C, fadd64(S.d(0x56C), cvt_sd(rec.f(0x61C))))
            for i in range(16):
                S.add32(0x574 + 4 * i, rec.u32(0x574 + 4 * i))
                S.setf(0x5B4 + 4 * i, fadd32(rec.f(0x5B4 + 4 * i), S.f(0x5B4 + 4 * i)))

    # -- SendFrame's part (UA 0x970c4-0x970e8: 0x94f6c)
    def send(self, src):
        r = self.get_free()
        r.b[:] = src
        return self.enqueue_first_pass(r)

    # -- 0xba86c
    def flush(self):
        out = []
        x26 = None
        while True:
            if x26 is not None and x26.fn == M32:
                break
            r = self.get_free()
            r.set32(0x2C, M32)
            x26 = self.enqueue_first_pass(r)
            if x26 is not None and x26.fn != M32:
                out.append(bytes(x26.b))
        self.reset_pool()
        self.finalize()
        return out

    # -- 0xb9ff4
    def finalize(self):
        H = self.hdr
        bins = []                    # [count u32, mean f32, lo f32, hi f32]
        lo, hi = 0.0, 0.1875
        for i in range(16):
            c = H.u32(0x68 + 4 * i)
            if c:
                bins.append([c, fdiv32(H.f(0xA8 + 4 * i), u32f(c)), lo, hi])
            lo = hi
            hi = fadd32(hi, 0.1875)
        if not bins:
            bins.append([1, 1.5, 0.0, 3.0])
        while len(bins) < 4:
            cxx_sort(bins, lambda a, b: a[0] > b[0])
            c, mean, lo, hi = bins[0]
            s11 = fmul32(fadd32(lo, mean), 0.5)
            bins[0] = [c - (c >> 1), fmul32(fadd32(mean, hi), 0.5), mean, hi]
            bins.append([c >> 1, s11, lo, mean])
        if len(bins) > 30:
            raise AssertionError("unreachable: at most 16 bins")
        cxx_sort(bins, lambda a, b: c_mi(a[1], b[1]))
        lo, hi = bins[0][2], bins[-1][3]
        step = fmul32(fsub32(hi, lo), 0.25)
        s8 = lo
        s1 = fadd32(lo, step)
        s2 = fmul32(fadd32(lo, s1), 0.5)
        cents = []
        for j in range(4):
            cents.append([0, s2, s8, s1])
            s2 = fadd32(step, s2)
            s8 = fadd32(step, s8)
            s1 = fadd32(step, s1)
        for _ in range(3):
            self.quantize(bins, cents)
        for j in range(4):
            H.set32(0xE8 + 4 * j, cents[j][0])
            H.setf(0xF8 + 4 * j, cents[j][1])

    # -- 0xba6a0
    @staticmethod
    def quantize(bins, cents):
        s1 = 0.0
        for c in cents:
            s2 = s3 = 0.0
            clo, chi = c[2], c[3]
            for b in bins:
                bhi, blo = b[3], b[2]
                if c_le(bhi, clo):
                    continue
                if c_pl(blo, chi):
                    continue
                lo = clo if c_mi(blo, clo) else blo
                hi = chi if c_mi(chi, bhi) else bhi
                w = fdiv32(fmul32(fsub32(hi, lo), u32f(b[0])), fsub32(bhi, blo))
                mid = fmul32(fadd32(lo, hi), 0.5)
                s2 = fadd32(s2, w)
                s3 = fma32(s3, w, mid)
            s1 = fadd32(s1, s2)
            s1 = fadd32(s1, 0.5)
            c[0] = fcvtzu32(s1)
            c[1] = fdiv32(s3, s2)
            s1 = fsub32(s2, u32f(fcvtzu32(s1)))
        for i in range(len(cents) - 1):
            m = fmul32(fadd32(cents[i][1], cents[i + 1][1]), 0.5)
            cents[i][3] = m
            cents[i + 1][2] = m


def run(records, trace=None):
    """records: 0x626-byte pass-1 records in arrival order.
    Returns (header bytes, records in emission order)."""
    mp = MP()
    mp.trace = trace
    emitted = []
    for r in records:
        if len(r) != REC:
            raise ValueError("record is %d bytes, not 0x626" % len(r))
        o = mp.send(r)
        if o is not None:
            emitted.append(bytes(o.b))
    emitted += mp.flush()
    return bytes(mp.hdr.b), emitted
