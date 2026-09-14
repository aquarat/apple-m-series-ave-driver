#!/usr/bin/env python3
"""Find what the AVE firmware wrote into its DATA segment, and any crash text in it.

A failed restart leaves no log: before the handshake there is no TERMINAL
channel, and the firmware's crash handler reached for its UART instead (r2,
2026-09-14: one DAPF miss at 0x39b200000, serial@39b200000). But the code runs
on a stack inside DATA and formats its messages there, so a copy of DATA taken
afterwards (test/physdump.ko) can still hold the report.

    tools/crash_scan.py WINDOW.bin [PRISTINE_DATA.bin]
    tools/crash_scan.py --selftest

WINDOW is the 16 MiB physdump window from 0x10000b28000; DATA is the 0x134000
bytes at physical 0x10001a90000, i.e. window offset 0xf68000 (docs/43, docs/51).

It reports only bytes that differ from the pristine DATA blob, because DATA
also holds static format strings - "ASSERT" appears in a pristine image too,
so a keyword hit anywhere in DATA means nothing. The one expected per-boot
difference, the STKG stack-guard word at DATA+0x3a38 (docs/51 2.3), is
reported separately.

--selftest runs the negative control (pristine vs itself: nothing to report)
and a positive control (a planted crash string in a changed region must be
found), so a quiet result on real data can be trusted.
"""
import re
import struct
import sys

WINDOW_BASE = 0x10000B28000
DATA_PHYS = 0x10001A90000
DATA_OFF = DATA_PHYS - WINDOW_BASE          # 0xf68000
DATA_SIZE = 0x134000
DATA_VA = 0xEC000                           # firmware __DATA vmaddr
TEXT_VA_END = 0xEC000
STKG_OFF = 0x3A38
PRISTINE = "data/blobs/ave-13.5-data-pristine.bin"

KEYWORDS = re.compile(rb"Exception|ASSERT|assert|panic|crash|abort|\.cpp|\.c:|"
                      rb"\bpc\b|\besr\b|\bfar\b|Call stack|Registers", re.I)


def changed_ranges(live, pristine, merge=16):
    """[start, end) ranges of differing bytes, gaps under `merge` joined."""
    out = []
    i, n = 0, len(live)
    while i < n:
        if live[i] != pristine[i]:
            j = i
            last = i
            while j < n and j - last <= merge:
                if live[j] != pristine[j]:
                    last = j
                j += 1
            out.append((i, last + 1))
            i = last + 1
        else:
            i += 1
    return out


def strings_in(buf, start, end, minlen=6):
    lo, hi = max(0, start - 64), min(len(buf), end + 64)
    for m in re.finditer(rb"[\x20-\x7e\t\n]{%d,}" % minlen, buf[lo:hi]):
        yield lo + m.start(), m.group()


def code_pointers(buf, start, end):
    """Aligned u64s in [start, end) that look like TEXT addresses."""
    for o in range(start & ~7, end, 8):
        if o + 8 > len(buf):
            break
        v = struct.unpack_from("<Q", buf, o)[0]
        if 0x4000 <= v < TEXT_VA_END:
            yield o, v


def scan(live, pristine, out=print):
    assert len(live) == len(pristine) == DATA_SIZE
    rngs = changed_ranges(live, pristine)
    stkg = [(a, b) for a, b in rngs if a <= STKG_OFF < b]
    other = [(a, b) for a, b in rngs if not (a <= STKG_OFF < b and b - a <= 8)]
    nbytes = sum(1 for a, b in other for k in range(a, b) if live[k] != pristine[k])
    pages = sorted({k >> 12 for a, b in other for k in range(a, b)})

    out(f"DATA changed: {nbytes} byte(s) in {len(other)} range(s), "
        f"{len(pages)} page(s); STKG word {'changed' if stkg else 'unchanged'}")
    if pages:
        out("  pages: " + " ".join(f"+{p << 12:#x}" for p in pages[:40])
            + (" ..." if len(pages) > 40 else ""))

    hits = []
    seen = set()
    for a, b in other:
        for off, s in strings_in(live, a, b):
            if off in seen:
                continue
            seen.add(off)
            new = any(live[k] != pristine[k] for k in range(off, off + len(s)))
            if new:
                hits.append((off, s))

    crash = [(o, s) for o, s in hits if KEYWORDS.search(s)]
    out(f"strings written since boot: {len(hits)}; with crash keywords: {len(crash)}")
    for off, s in (crash or hits)[:60]:
        text = s.decode("ascii", "replace").replace("\n", "\\n").replace("\t", "\\t")
        out(f"  DATA+{off:#07x} (VA {DATA_VA + off:#x}): {text[:160]}")

    if crash:
        lo = max(0, crash[0][0] - 0x400)
        hi = min(DATA_SIZE, crash[-1][0] + 0x400)
        ptrs = list(code_pointers(live, lo, hi))
        out(f"code-looking u64s near the crash text ({len(ptrs)}): "
            + " ".join(f"{v:#x}" for _, v in ptrs[:24]))
    return nbytes, len(crash)


def selftest():
    pristine = open(PRISTINE, "rb").read()
    ok = True

    print("[negative control] pristine vs itself")
    nb, nc = scan(pristine, pristine, out=lambda s: print("   ", s))
    if nb or nc:
        print("FAIL: reported changes in identical data")
        ok = False

    print("[positive control] planted crash text in a zeroed region")
    live = bytearray(pristine)
    zero = next(o for o in range(0x98000, DATA_SIZE - 0x100, 0x1000)
                if not any(pristine[o:o + 0x100]))
    msg = b"\n\n\t!! Exception !! crash type 4\n\tpc    0x0000000000010D48  esr 0x96000007"
    live[zero:zero + len(msg)] = msg
    struct.pack_into("<Q", live, zero + 0x80, 0x10D44)
    nb, nc = scan(bytes(live), pristine, out=lambda s: print("   ", s))
    if not nb or not nc:
        print("FAIL: missed a planted crash string")
        ok = False

    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    win = open(sys.argv[1], "rb").read()
    pristine = open(sys.argv[2] if len(sys.argv) > 2 else PRISTINE, "rb").read()
    if len(win) <= DATA_OFF:
        print(f"window is {len(win):#x} bytes, too short for DATA at {DATA_OFF:#x}")
        return 1
    covered = min(DATA_SIZE, len(win) - DATA_OFF)
    if covered < DATA_SIZE:
        print(f"NOTE: window covers only DATA+0..{covered:#x} of {DATA_SIZE:#x} "
              f"(take it with physdump full=1); the rest is compared as unchanged")
    live = win[DATA_OFF:DATA_OFF + covered] + pristine[covered:]
    scan(live, pristine)
    return 0


if __name__ == "__main__":
    sys.exit(main())
