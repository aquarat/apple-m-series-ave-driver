import sys, collections
def load(n):
    last = collections.OrderedDict(); fn = {}
    for l in open(n):
        if not l.startswith("W "): continue
        p = l.split(); a = int(p[1], 16); v = int(p[3], 16)
        last[a] = v; fn[a] = p[6] if len(p) > 6 else "?"
    return last, fn
a, fa = load(sys.argv[1]); b, fb = load(sys.argv[2])
print(f"# registers written: {sys.argv[1]} {len(a)}, {sys.argv[2]} {len(b)}")
for r in sorted(set(a) | set(b)):
    va, vb = a.get(r), b.get(r)
    if va != vb:
        f = (fa.get(r) or fb.get(r))[:50]
        s = lambda v: "-" if v is None else hex(v)
        print(f"{r:#012x}  {sys.argv[1]} {s(va):>12}  {sys.argv[2]} {s(vb):>12}  {f}")
