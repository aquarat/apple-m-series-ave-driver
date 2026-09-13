#!/usr/bin/env python3
"""Check whether a firmware Mach-O is the image actually loaded, against a RAM dump.

iBoot copies the RTKit image's __TEXT and __DATA file contents into DRAM. If a
candidate firmware is the build that is running, its __TEXT bytes appear in the
dump nearly verbatim at the TEXT load address, and its __DATA bytes appear at
the DATA load address except where the loader filled in the boot-argument tag
list (_rtk_patchbay) and where the firmware has since written at runtime.

Three measures, so a wrong candidate cannot pass by accident:
  1. aligned byte identity of __TEXT and __DATA at the given dump offsets;
  2. aligned 32-byte window identity (a chunk-level view of the same thing);
  3. position-free alignment: unique 32-byte samples of the candidate's __TEXT
     searched anywhere in the dump, with the dump-minus-image offset histogram.
Run it on a known-wrong build too (docs/43): if both pass, it measures nothing.

  tools/fw_dump_compare.py data/blobs/macos-13.5/ave_h13c.bin data/blobs/iboot-window-16m.bin
  tools/fw_dump_compare.py data/blobs/ave_h13c.bin data/blobs/iboot-window-16m.bin   # control
"""
import argparse, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho_info import MachO

def same_bytes(a, b):
    n = min(len(a), len(b))
    # zero bytes of the XOR are equal bytes; avoids a Python loop over n
    xb = (int.from_bytes(a[:n], "little") ^ int.from_bytes(b[:n], "little")).to_bytes(n, "little")
    return xb.count(0), n

def nz_identity(a, b):
    """Identity over bytes that are non-zero in the image a. Zero-filled stacks
    and page tables match any mostly-empty dump, so the plain ratio flatters a
    wrong candidate; this one does not."""
    n = min(len(a), len(b))
    xb = (int.from_bytes(a[:n], "little") ^ int.from_bytes(b[:n], "little")).to_bytes(n, "little")
    # a nonzero and xor zero: build masks with bytes.translate
    anz = a[:n].translate(bytes([0] + [1]*255))
    xz = xb.translate(bytes([1] + [0]*255))
    both = (int.from_bytes(anz, "little") & int.from_bytes(xz, "little")).to_bytes(n, "little").count(1)
    total = anz.count(1)
    return both, total

def windows(a, b, w=32):
    n = min(len(a), len(b)) // w
    return sum(a[i*w:(i+1)*w] == b[i*w:(i+1)*w] for i in range(n)), n

def diff_runs(a, b, base, gap=16, limit=12):
    """Coalesced [start, end) ranges where a and b differ, relative to base."""
    runs, cur = [], None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if cur and i - cur[1] <= gap:
                cur[1] = i + 1
            else:
                cur = [i, i + 1]
                runs.append(cur)
    return [(base + s, base + e) for s, e in runs[:limit]], len(runs)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("dump")
    ap.add_argument("--dump-base", type=lambda x: int(x, 0), default=0x10000b28000,
                    help="physical address of dump offset 0")
    ap.add_argument("--text-off", type=lambda x: int(x, 0), default=0x0,
                    help="dump offset where __TEXT was loaded")
    ap.add_argument("--data-off", type=lambda x: int(x, 0), default=0xf68000,
                    help="dump offset where __DATA was loaded")
    ap.add_argument("--samples", type=int, default=4000)
    a = ap.parse_args()

    m = MachO(open(a.firmware, "rb").read())
    dump = open(a.dump, "rb").read()
    seg = {s[0]: s for s in m.segments}
    _, tva, tvs, tfo, tfs, _ = seg["__TEXT"]
    _, dva, dvs, dfo, dfs, dsects = seg["__DATA"]
    text = m.d[tfo:tfo+tfs]
    data = m.d[dfo:dfo+dfs]
    print(f"{a.firmware}")
    print(f"  __TEXT vm {tva:#x}+{tvs:#x} file {tfo:#x}+{tfs:#x}")
    print(f"  __DATA vm {dva:#x}+{dvs:#x} file {dfo:#x}+{dfs:#x}")
    print(f"  TEXT+DATA vmsize = {tvs + dvs:#x}")

    t = dump[a.text_off:a.text_off+tfs]
    same, n = same_bytes(text, t)
    ws, wn = windows(text, t)
    zs, zn = nz_identity(text, t)
    print(f"\n[1] __TEXT vs dump @{a.text_off:#x} (phys {a.dump_base+a.text_off:#x}), {n:#x} bytes")
    print(f"    identical bytes   {same}/{n} = {100*same/n:.4f}%")
    print(f"    identical bytes, non-zero in image only {zs}/{zn} = {100*zs/max(zn,1):.4f}%")
    print(f"    identical 32-byte windows {ws}/{wn} = {100*ws/wn:.2f}%")
    runs, nruns = diff_runs(text, t, tva)
    print(f"    differing ranges (image VA): {nruns}" + "".join(f"\n      {s:#x}-{e:#x}" for s, e in runs))

    dd = dump[a.data_off:a.data_off+dfs]
    same, n = same_bytes(data, dd)
    ws, wn = windows(data, dd)
    zs, zn = nz_identity(data, dd)
    print(f"\n[2] __DATA filecontent vs dump @{a.data_off:#x} (phys {a.dump_base+a.data_off:#x}), {n:#x} bytes")
    print(f"    identical bytes   {same}/{n} = {100*same/n:.4f}%")
    print(f"    identical bytes, non-zero in image only {zs}/{zn} = {100*zs/max(zn,1):.4f}%")
    print(f"    identical 32-byte windows {ws}/{wn} = {100*ws/wn:.2f}%")
    for sname, sg, saddr, ssize, soff in dsects:
        if ssize == 0 or soff == 0:
            continue
        o = saddr - dva
        if o >= dfs:
            continue
        sz = min(ssize, dfs - o)
        s2, n2 = same_bytes(data[o:o+sz], dd[o:o+sz])
        print(f"      {sname:<18} DATA+{o:#07x}+{sz:#07x}  identical {100*s2/n2:7.3f}%")
    runs, nruns = diff_runs(data, dd, dva)
    print(f"    differing ranges (image VA): {nruns}" + "".join(f"\n      {s:#x}-{e:#x}" for s, e in runs))

    # [3] position-free: where do unique TEXT samples land in the dump?
    step = max(32, (tfs // a.samples) & ~3)
    hist, found, tried = collections.Counter(), 0, 0
    for off in range(0, tfs - 32, step):
        chunk = text[off:off+32]
        if chunk.count(chunk[:4]) == 8 or text.count(chunk) != 1:
            continue          # skip padding-like and non-unique samples
        tried += 1
        hit = dump.find(chunk)
        if hit >= 0:
            found += 1
            hist[hit - off] += 1
    print(f"\n[3] unique 32-byte __TEXT samples found anywhere in dump: {found}/{tried}"
          f" = {100*found/max(tried,1):.2f}%")
    for delta, c in hist.most_common(3):
        print(f"    dump_off - image_text_off = {delta:#x}  ({c} samples)")

if __name__ == "__main__":
    main()
