#!/usr/bin/env python3
"""Find and decode RTKit boot-argument tag lists in a memory dump or image.

An RTKit firmware's DATA segment carries a packed list of tags that the loader
(iBoot, or a host driver) fills in: 4-char code stored byte-reversed, u32
length, payload, no alignment. The list ends with IOBA/IOSZ. Filled values say
which coprocessor a segment belongs to - CpAd/WrAd are its ASC register bases.

  tools/rtkit_tags.py data/blobs/iboot-window-16m.bin --base 0x10000b28000
  tools/rtkit_tags.py data/blobs/ave_h13c.bin --base 0

See docs/31, run 2026-09-13 ("the dump").
"""
import argparse, re

def walk(buf, start, limit=64):
    out, i = [], start
    for _ in range(limit):
        if i + 8 > len(buf):
            break
        tag, n = buf[i:i+4], int.from_bytes(buf[i+4:i+8], 'little')
        if not all(0x20 <= c < 0x7f for c in tag) or n > 0x1000:
            break
        out.append((i, tag[::-1].decode(), buf[i+8:i+8+n]))
        i += 8 + n
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--base', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()
    buf = open(a.file, 'rb').read()
    # Every list seen so far starts with STKG (stack guard).
    for m in re.finditer(rb'GKTS\x08\x00\x00\x00', buf):
        tags = walk(buf, m.start())
        if len(tags) < 8:
            continue
        print(f"== tag list at {a.base + m.start():#x}")
        for off, name, val in tags:
            v = int.from_bytes(val, 'little')
            print(f"   {a.base + off:#014x}  {name}  len {len(val)}  {v:#x}")

if __name__ == '__main__':
    main()
