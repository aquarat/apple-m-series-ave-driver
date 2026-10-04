#!/usr/bin/env python3
"""ave2pass: pass-1 records -> pass-2 table and buffers, as macOS 13.5 does it (docs/95 §2.4).

  ave2pass.py build RECS.bin [N] -o TABLE.bin [--fps F] [--keep-pts] [--backend port|emu|both]
  ave2pass.py frames TABLE.bin OUTDIR
  ave2pass.py dump RECS.bin|TABLE.bin [--all]
  ave2pass.py synth N -o RECS.bin [--cuts 12,30] [--seed S] [--fps F]

RECS.bin   pass-1 records, 0x626 bytes each, in arrival (coding) order; for an
           IPPP stream record i is display frame i. Each record is the
           CodedHeader+0x22638 bytes the firmware wrote. --stride/--offset
           take records out of larger per-frame dumps instead (for whole
           CodedHeader dumps: --stride 0x22C60 --offset 0x22638).
TABLE.bin  header[0x108] + rec[N][0x626], records in display order (the
           layout of macOS's own debug dump, DBUG_DumpMultiPassStats UA 0x950e0).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recfmt  # noqa: E402
import mpport  # noqa: E402

REC, HDR = recfmt.REC, recfmt.HDR


def _int(s):
    return int(s, 0)


def load_records(paths, stride, offset):
    recs = []
    for p in paths:
        data = open(p, "rb").read()
        if stride:
            if len(data) % stride:
                sys.exit("%s: %d bytes is not a multiple of the stride 0x%x" % (p, len(data), stride))
            recs += [data[i + offset:i + offset + REC] for i in range(0, len(data), stride)]
        else:
            if len(data) % REC:
                sys.exit("%s: %d bytes is not a multiple of 0x626 (use --stride/--offset for "
                         "per-frame dumps)" % (p, len(data)))
            recs += [data[i:i + REC] for i in range(0, len(data), REC)]
    return recs


def check_order(recs):
    """The MP code releases records in display order from a heap; a display
    order that is missing or repeated stalls it for good (FlushStats would
    then loop until the pool is empty). Refuse such input."""
    fns = [recfmt.s32(r, recfmt.R_FN) for r in recs]
    if sorted(fns) != list(range(len(recs))):
        bad = [i for i, f in enumerate(fns) if f != i][:8]
        return "rec+0x2C (display order) is not a permutation of 0..%d (first differences at %s: %s)" % (
            len(recs) - 1, bad, [fns[i] for i in bad])
    return None


def build(recs, backend="port", log=None):
    """Returns (header, records in display order)."""
    results = {}
    if backend in ("port", "both"):
        results["port"] = mpport.run(recs, trace=log if backend == "port" else None)
    if backend in ("emu", "both"):
        import mpemu
        results["emu"] = mpemu.run(recs, log=log)
    if backend == "both" and results["port"] != results["emu"]:
        raise SystemExit("MISMATCH between the Python port and the emulation")
    hdr, emitted = results.get("emu", results.get("port"))
    by_fn = {}
    for r in emitted:
        fn = recfmt.s32(r, recfmt.R_FN)
        if fn in by_fn:
            raise SystemExit("display order %d emitted twice" % fn)
        by_fn[fn] = r
    if sorted(by_fn) != list(range(len(recs))):
        raise SystemExit("pipeline emitted %d records, display orders %s" % (len(by_fn), sorted(by_fn)[:20]))
    return hdr, [by_fn[i] for i in range(len(recs))]


def cmd_build(a):
    paths = [p for p in a.args if not _isint(p)]
    ns = [int(p, 0) for p in a.args if _isint(p)]
    if not paths:
        sys.exit("no input file")
    recs = load_records(paths, a.stride, a.offset)
    if ns:
        if ns[0] > len(recs):
            sys.exit("asked for %d records, the input has %d" % (ns[0], len(recs)))
        recs = recs[:ns[0]]
    if not recs:
        sys.exit("no records")
    if a.renumber:
        recs = [_set_fn(r, i) for i, r in enumerate(recs)]
    err = check_order(recs)
    if err:
        sys.exit(err + " (--renumber writes 0..N-1 if the records really are in display order)")
    if not a.keep_pts:
        recs = [recfmt.set_pts(r, i, a.fps) for i, r in enumerate(recs)]
    log = (lambda s: print(s, file=sys.stderr)) if a.trace else None
    hdr, out = build(recs, a.backend, log)
    with open(a.o, "wb") as f:
        f.write(hdr)
        for r in out:
            f.write(r)
    h = recfmt.decode_header(hdr)
    print("%s: header + %d records (%d bytes); %d scene(s), %d frames, %d bits" % (
        a.o, len(out), HDR + REC * len(out), h["total_scenes"], h["cnt_All"], h["bits_All"]))


def _isint(s):
    try:
        int(s, 0)
        return True
    except ValueError:
        return False


def _set_fn(r, i):
    b = bytearray(r)
    b[recfmt.R_FN:recfmt.R_FN + 4] = i.to_bytes(4, "little")
    return bytes(b)


def cmd_frames(a):
    hdr, recs = recfmt.read_table(a.table)
    os.makedirs(a.outdir, exist_ok=True)
    bufs = recfmt.pass2_buffers(hdr, recs)
    for k, b in enumerate(bufs):
        with open(os.path.join(a.outdir, "frame-%04d.bin" % k), "wb") as f:
            f.write(b)
    print("%s: %d buffers (frame 0: 0x%x bytes, others 0x%x)" % (
        a.outdir, len(bufs), len(bufs[0]), len(bufs[1]) if len(bufs) > 1 else 0))


def cmd_dump(a):
    size = os.path.getsize(a.file)
    if size % REC == 0:
        hdr, recs = None, recfmt.read_records(a.file)
        print("%s: %d pass-1 records" % (a.file, len(recs)))
    else:
        hdr, recs = recfmt.read_table(a.file)
        print("%s: table, header + %d records" % (a.file, len(recs)))
    print("  i    fn sl cl     bits  hdr   corr fk  sc  scn      hdiff   hdiff_mx    act   qscale   cplx0   cplx1 lc")
    for i, r in enumerate(recs):
        d = recfmt.decode_record(r)
        print("%3d %5d %2d %2d %8d %4d %6d %2d %3d %4d %10.3f %10.3f %6.2f %8.4f %7.4f %7.4f %d" % (
            i, d["fn"], d["slice"], d["class"], d["bits"], d["hdr_bits"], d["corr"], d["force_key"],
            d["scene"], d["scene_frames"], d["hdiff"], d["hdiff_max"], d["act"], d["qscale"],
            d["cplx"][0], d["cplx"][1], d["lclass"]))
        if a.all:
            print("      pts %s  %dx%d %.3f fps  scene bits %d" % (d["pts"], d["w"], d["h"], d["fps"], d["scene_bits"]))
    if hdr is not None:
        h = recfmt.decode_header(hdr)
        print("header (0x108 bytes):")
        for o, t, n in recfmt.HEADER_FIELDS:
            print("  +0x%02X %-20s %s" % (o, n, h[n]))
        print("  +0x68 cplx_hist_count    %s" % h["cplx_hist_count"])
        print("  +0xA8 cplx_hist_sum      %s" % ["%.4g" % v for v in h["cplx_hist_sum"]])
        print("  +0xE8 quant_count        %s" % h["quant_count"])
        print("  +0xF8 quant_value        %s" % ["%.6g" % v for v in h["quant_value"]])


def cmd_synth(a):
    cuts = [int(c) for c in a.cuts.split(",")] if a.cuts else []
    recs = recfmt.synth(a.n, cuts=cuts, seed=a.seed, fps=a.fps)
    with open(a.o, "wb") as f:
        for r in recs:
            f.write(r)
    print("%s: %d synthetic records, cuts at %s" % (a.o, len(recs), cuts))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    b = sp.add_parser("build", help="pass-1 records -> header + post-processed records")
    b.add_argument("args", nargs="+", metavar="RECS.bin [N]")
    b.add_argument("-o", required=True, metavar="TABLE.bin")
    b.add_argument("--fps", type=float, default=30.0, help="PTS written at rec+0x04 is i/fps (default 30)")
    b.add_argument("--keep-pts", action="store_true", help="leave rec+0x04..0x1B as the input has it")
    b.add_argument("--stride", type=_int, default=0, help="bytes per frame in the input (0: packed records)")
    b.add_argument("--offset", type=_int, default=0, help="record offset inside each stride")
    b.add_argument("--renumber", action="store_true", help="write display order i at rec+0x2C first")
    b.add_argument("--backend", choices=("port", "emu", "both"), default="port",
                   help="port: pure Python (default); emu: Apple's code under Unicorn; both: run both and compare")
    b.add_argument("--trace", action="store_true", help="print the MP: log lines (emu) to stderr")
    f = sp.add_parser("frames", help="table -> per-frame pass-2 input buffers")
    f.add_argument("table")
    f.add_argument("outdir")
    d = sp.add_parser("dump", help="print records and header")
    d.add_argument("file")
    d.add_argument("--all", action="store_true")
    s = sp.add_parser("synth", help="write synthetic pass-1 records")
    s.add_argument("n", type=int)
    s.add_argument("-o", required=True)
    s.add_argument("--cuts", default="")
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--fps", type=float, default=30.0)
    a = ap.parse_args()
    {"build": cmd_build, "frames": cmd_frames, "dump": cmd_dump, "synth": cmd_synth}[a.cmd](a)


if __name__ == "__main__":
    main()
