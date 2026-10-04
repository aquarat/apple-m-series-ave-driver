"""Firmware address space from a snapshot (docs/93): VA -> bytes.

VA 0..0x1f8000 is the image (TEXT, DATA, heap) at DVA 0x800000000 (fw.bin).
The high half comes from the firmware's own TTBR1 tables, L2 at DVA
0x8000f0000 (16 KiB pages, 32 MiB per L2 entry, base 0xffffffff80000000).
"""
import struct
B = 0x800000000
L2 = 0x8000f0000
HIBASE = 0xffffffff80000000

class Snap:
    def __init__(self, d):
        self.fw = open(f"{d}/fw.bin", "rb").read()
        self.iova = open(f"{d}/iova.bin", "rb").read()   # IOVA 0x80000000..
        self.hi = self._map()

    def _fq(self, dva): return struct.unpack_from("<Q", self.fw, dva - B)[0]

    def _map(self):
        runs = []
        for i in range(2048):
            d = self._fq(L2 + 8 * i)
            if d & 3 != 3:
                continue
            t = d & 0xfffffffff000
            for j in range(2048):
                e = self._fq(t + 8 * j)
                if e & 3 != 3:
                    continue
                va = HIBASE + (i << 25) + j * 0x4000
                pa = e & 0xfffffffff000
                if runs and runs[-1][0] + runs[-1][1] == va and runs[-1][2] + runs[-1][1] == pa:
                    runs[-1][1] += 0x4000
                else:
                    runs.append([va, 0x4000, pa])
        return runs

    def regions(self):
        yield 0, self.fw[:0x1f8000], "image"
        for va, n, pa in self.hi:
            if 0x80000000 <= pa < 0x100000000:
                yield va, self.iova[pa - 0x80000000: pa - 0x80000000 + n], f"iova {pa:#x}"

    def mmio(self):
        return [(va, n, pa) for va, n, pa in self.hi if not (0x80000000 <= pa < 0x100000000)]

    def read(self, va, n):
        for base, b, _ in self.regions():
            if base <= va and va + n <= base + len(b):
                return b[va - base: va - base + n]
        raise KeyError(hex(va))

    def q(self, va): return struct.unpack("<Q", self.read(va, 8))[0]

    def find(self, pat, align=1):
        out = []
        for base, b, _ in self.regions():
            i = b.find(pat)
            while i >= 0:
                if i % align == 0:
                    out.append(base + i)
                i = b.find(pat, i + 1)
        return out
