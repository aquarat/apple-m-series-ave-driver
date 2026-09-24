#!/usr/bin/env python3
"""Whole-image MMIO access map of the macOS 13.5 AVE firmware (docs/74).

Every str/ldr (all widths, pair and register-offset forms) whose base resolves
to the firmware's MMIO window pointer *(0x21a7b8) plus a constant is listed as

    <VA>  <st|ld>  fw <offset>  AP <0x40C000000 + offset>

The window pointer is recognised as `adrp xN, 0x21a000` followed by
`ldr xM, [xN, #1976]` (docs/69 §1, docs/70 §1.3). Constants are tracked
through mov/movk/orr-with-zr/add/sub (immediate, shifted and register forms);
a register is forgotten on any other write to it and x0-x18 are forgotten at
each bl. Flow-insensitive within a function (reset at ret): the output is a
candidate list, and every entry docs/74 relies on is re-read by hand.

Control: the scan must reproduce the sites docs/70 §1.3 and docs/73 already
cite (e.g. 0x55e98 -> 0x124A1C8, 0x60338 -> 0x124A394, 0x57818 area ->
0x128A088). `--check` asserts them.

  python3 tools/mmio_map.py                         # everything
  python3 tools/mmio_map.py --lo 0x1270000 --hi 0x12e0000
  python3 tools/mmio_map.py --check
"""
import argparse, os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "B"

# (VA, fw offset, st/ld) that earlier documents read by hand.
CONTROLS = [
    (0x55e98, 0x124A1C8, "st"),   # docs/70 §1.3 IntraEst QPY
    (0x55eb8, 0x124A1CC, "st"),   # docs/70 §1.3 nQuant
    (0x60338, 0x124A394, "st"),   # docs/70 §1.3 stage enable
    (0x57130, 0x124A1D0, "st"),   # docs/73 §3
    (0x62044, 0x1110128, "st"),   # docs/69 §3.1 setPipeGo SRCDMAGO
    (0x4ee28, 0x1120018, "st"),   # docs/69 §1.3 ProcessPipeReset
    (0x57874, 0x12AA08C, "st"),   # docs/74: adrp+add #0x7b8 idiom (ReconChroma SKIPMODE)
]


def disasm():
    env = dict(os.environ, AVE_MACOS="13.5")
    return subprocess.run([sys.executable, os.path.join(REPO, "tools/disas.py"),
                           "--fw", "--addr", "0x0", "-n", "0xec000"],
                          env=env, capture_output=True, text=True, check=True).stdout


LINE = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$")
MEM = re.compile(r"\[(x\d+|sp)(?:,\s*([^\]]+))?\](!?)(?:,\s*#(-?(?:0x)?[0-9a-f]+))?")


def imm(s):
    s = s.strip().lstrip("#")
    return int(s, 0)


def X(r):
    return "x" + r[1:] if re.fullmatch(r"[wx]\d+", r) else r


def scan(text):
    val = {}           # reg -> int constant, or (BASE, off), or ("PG", page)
    out = []

    def get(r):
        r = X(r)
        if r in ("xzr", "wzr"):
            return 0
        return val.get(r)

    def add(a, b, sign=1):
        if a is None or b is None:
            return None
        if isinstance(a, int) and isinstance(b, int):
            return (a + sign * b) & 0xFFFFFFFFFFFFFFFF
        if isinstance(a, tuple) and a[0] == BASE and isinstance(b, int):
            return (BASE, a[1] + sign * b)
        if isinstance(b, tuple) and b[0] == BASE and isinstance(a, int) and sign == 1:
            return (BASE, b[1] + a)
        if isinstance(a, tuple) and a[0] == "PG" and isinstance(b, int):
            return ("PG", a[1] + sign * b)
        return None

    for L in text.splitlines():
        m = LINE.match(L)
        if not m:
            continue
        va, op, args = int(m.group(1), 16), m.group(2), m.group(3).split("//")[0].strip()
        ops = [p.strip() for p in re.split(r",(?![^\[]*\])", args)] if args else []
        if op == "ret":
            val = {}
            continue
        if op in ("bl", "blr"):
            for i in range(19):
                val.pop("x%d" % i, None)
            continue
        mm = MEM.search(args)
        if (op.startswith("ld") or op.startswith("st")) and mm and not op.startswith(("ldxr", "stxr", "ldar", "stlr", "ldaxr", "stlxr")):
            b = X(mm.group(1))
            o = mm.group(2)
            bv = val.get(b)
            addr = None
            if o is None:
                addr = bv
            else:
                o0 = o.split(",")[0].strip()
                if o0.startswith("#"):
                    addr = add(bv, imm(o0))
                else:
                    addr = add(bv, get(o0))
            pair = op in ("ldp", "stp", "ldnp", "stnp", "ldpsw")
            # the MMIO window pointer: an 8-byte load from 0x21a7b8, reached as
            # adrp 0x21a000 + [#1976] or adrp + add #0x7b8 + [#0]
            if op == "ldr" and ops[0][0] == "x" and isinstance(bv, tuple) and bv[0] == "PG":
                k = imm(o.split(",")[0]) if o and o.strip().startswith("#") else 0
                if bv[1] + k == 0x21a7b8 and not mm.group(3) and mm.group(4) is None:
                    val[X(ops[0])] = (BASE, 0)
                    continue
            if isinstance(addr, tuple) and addr[0] == BASE and 0x1000000 <= addr[1] < 0x2000000:
                kind = "st" if op.startswith("st") else "ld"
                out.append((va, kind, addr[1], op, args))
                if pair:
                    out.append((va, kind, addr[1] + (8 if ops[0][0] == "x" else 4), op, args))
            # write-back / post-index
            if mm.group(3) == "!" and o is not None and o.strip().startswith("#"):
                val[b] = add(bv, imm(o))
            elif mm.group(4) is not None:
                val[b] = add(bv, int(mm.group(4), 0))
            if op.startswith("ld"):
                dsts = ops[:2] if pair else ops[:1]
                for d in dsts:
                    val.pop(X(d), None)
            continue
        if not ops:
            continue
        d = X(ops[0])
        try:
            if op == "adrp":
                val[d] = ("PG", int(ops[1], 0))
            elif op in ("mov", "movz") and len(ops) == 2:
                if ops[1].startswith("#"):
                    v = imm(ops[1]) & (0xFFFFFFFF if ops[0][0] == "w" else 0xFFFFFFFFFFFFFFFF)
                    val[d] = v
                else:
                    v = get(ops[1])
                    if v is None:
                        val.pop(d, None)
                    else:
                        val[d] = v
            elif op == "movk":
                v = get(ops[0])
                sh = int(ops[2].split("#")[1], 0) if len(ops) > 2 else 0
                if isinstance(v, int):
                    v = (v & ~(0xFFFF << sh)) | (imm(ops[1]) << sh)
                    val[d] = v
                else:
                    val.pop(d, None)
            elif op == "orr" and ops[1] in ("wzr", "xzr") and ops[2].startswith("#"):
                val[d] = imm(ops[2])
            elif op in ("add", "sub") and len(ops) >= 3:
                a = get(ops[1])
                if ops[2].startswith("#"):
                    k = imm(ops[2])
                    if len(ops) > 3 and "lsl" in ops[3]:
                        k <<= int(ops[3].split("#")[1], 0)
                    b = k
                else:
                    b = get(ops[2])
                    if isinstance(b, int) and len(ops) > 3 and ops[3].startswith("lsl"):
                        b <<= int(ops[3].split("#")[1], 0)
                r = add(a, b, 1 if op == "add" else -1)
                if r is None:
                    val.pop(d, None)
                else:
                    val[d] = r
            elif re.fullmatch(r"[wx]\d+", ops[0]) and not op.startswith(("cmp", "cmn", "tst", "b", "cb", "tb", "ccmp", "ccmn", "fcmp")):
                val.pop(d, None)
        except (ValueError, IndexError):
            val.pop(d, None)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lo", type=lambda x: int(x, 0), default=0x1000000)
    ap.add_argument("--hi", type=lambda x: int(x, 0), default=0x2000000)
    ap.add_argument("--st", action="store_true", help="stores only")
    ap.add_argument("--check", action="store_true", help="assert the controls")
    a = ap.parse_args()
    acc = scan(disasm())
    if a.check:
        s = {(va, off, k) for va, k, off, _, _ in acc}
        bad = [c for c in CONTROLS if c not in s]
        for c in CONTROLS:
            print(("ok  " if c not in bad else "MISS"), "%#x -> fw %#x %s" % c)
        print(f"{len(acc)} accesses in total")
        sys.exit(1 if bad else 0)
    for va, k, off, op, args in sorted(acc, key=lambda t: (t[2], t[0])):
        if a.lo <= off < a.hi and (k == "st" or not a.st):
            print(f"{va:#07x}  {k}  fw {off:#09x}  AP {0x40C000000 + off:#011x}   {op} {args}")


if __name__ == "__main__":
    main()
