#!/usr/bin/env python3
"""Pull single files out of a macOS restore IPSW's filesystem DMG, by range request.

The macOS 13.5 system volume (096-63007-081.dmg, 7.5 GB) is *stored*, not
deflated, inside the IPSW zip, so any byte of the DMG is one HTTP range request
away. The DMG is UDIF: a koly trailer points at a plist whose blkx "mish"
tables map sectors of each partition to (mostly LZFSE) chunks. This maps an
APFS partition's byte offsets onto those chunks, fetches and caches only the
chunks that are touched, and hands the result to libfsapfs (python3-fsapfs) as
a file object. Pulling the 1.2 MB AppleVideoEncoder bundle costs ~2 MB of
download instead of 7.5 GB. docs/72-userspace-video-params.md.

Apple binaries fetched here are proprietary: they go under data/blobs/
(gitignored) and must never be committed.

Needs: python3-fsapfs (apt), lzfse (apt; only for LZFSE chunks).

Usage:
  fetch_userspace.py --list-parts
  fetch_userspace.py --out data/blobs/macos-13.5/userspace/fs \\
      /System/Library/Video/Plug-Ins/AppleVideoEncoder.bundle/Contents/MacOS/AppleVideoEncoder
"""
import argparse, bisect, bz2, hashlib, io, lzma, os, plistlib, struct, subprocess
import sys, time, urllib.request, zlib

IPSW = ("https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/"
        "D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw")
# Zip member 096-63007-081.dmg (the FileSystem DMG, compress_type 0 = stored):
# local header offset and size from the IPSW central directory.
MEMBER_HDR = 4455860353
MEMBER_SIZE = 7481685376


class Remote:
    def __init__(self, url, hdr, size, cache):
        self.url, self.cache = url, cache
        os.makedirs(cache, exist_ok=True)
        self.nreq = self.nbytes = 0
        lh = self.rng(hdr, hdr + 29)
        assert lh[:4] == b"PK\x03\x04", "not a zip local header"
        nl, el = struct.unpack_from("<HH", lh, 26)
        self.doff, self.dsize = hdr + 30 + nl + el, size
        koly = self.rng(self.doff + size - 512, self.doff + size - 1)
        assert koly[:4] == b"koly", "no UDIF trailer"
        xoff, xlen = struct.unpack_from(">QQ", koly, 0xD8)
        pl = plistlib.loads(self.rng(self.doff + xoff, self.doff + xoff + xlen - 1))
        self.parts = [(b["Name"], b["Data"]) for b in pl["resource-fork"]["blkx"]]

    def rng(self, a, b):
        for _ in range(6):
            try:
                req = urllib.request.Request(self.url, headers={"Range": f"bytes={a}-{b}"})
                d = urllib.request.urlopen(req, timeout=120).read()
                self.nreq += 1
                self.nbytes += len(d)
                return d
            except OSError as e:
                print("retry:", e, file=sys.stderr)
                time.sleep(2)
        raise OSError("range request failed")

    def use(self, idx):
        m = self.parts[idx][1]
        assert m[:4] == b"mish"
        _, count, dataoff = struct.unpack_from(">QQQ", m, 8)
        n = struct.unpack_from(">I", m, 0xC8)[0]
        self.runs = []
        for i in range(n):
            t, _, ss, sc, co, cl = struct.unpack_from(">IIQQQQ", m, 0xCC + 40 * i)
            if t != 0xFFFFFFFF:
                self.runs.append((ss * 512, sc * 512, t, dataoff + co, cl))
        self.starts = [r[0] for r in self.runs]
        self.size = count * 512
        self.memo = {}

    def chunk(self, i):
        if i in self.memo:
            return self.memo[i]
        _, ul, t, co, cl = self.runs[i]
        if t in (0, 2):
            out = bytes(ul)
        else:
            fn = os.path.join(self.cache, f"{co:x}_{cl:x}_{t:x}")
            if os.path.exists(fn):
                raw = open(fn, "rb").read()
            else:
                raw = self.rng(self.doff + co, self.doff + co + cl - 1)
                open(fn, "wb").write(raw)
            if t == 1:
                out = raw
            elif t == 0x80000005:
                out = zlib.decompress(raw)
            elif t == 0x80000006:
                out = bz2.decompress(raw)
            elif t == 0x80000007:
                out = subprocess.run(["lzfse", "-decode"], input=raw,
                                     capture_output=True, check=True).stdout
            elif t == 0x80000008:
                out = lzma.decompress(raw)
            else:
                raise ValueError(f"UDIF chunk type {t:#x}")
        if len(self.memo) > 64:
            self.memo.clear()
        self.memo[i] = out
        return out

    def read(self, off, n):
        res = bytearray()
        while n > 0 and off < self.size:
            i = bisect.bisect_right(self.starts, off) - 1
            us, ul = self.runs[i][0], self.runs[i][1]
            c = self.chunk(i)
            take = min(n, ul - (off - us))
            res += c[off - us:off - us + take]
            off += take
            n -= take
        return bytes(res)


class FileObj(io.RawIOBase):
    def __init__(self, r):
        self.r, self.p = r, 0
    def seekable(self): return True
    def readable(self): return True
    def seek(self, o, w=0):
        self.p = o if w == 0 else (self.p + o if w == 1 else self.r.size + o)
        return self.p
    def tell(self): return self.p
    def get_size(self): return self.r.size
    def read(self, n=-1):
        if n is None or n < 0:
            n = self.r.size - self.p
        d = self.r.read(self.p, n)
        self.p += len(d)
        return d
    def readinto(self, b):
        d = self.read(len(b))
        b[:len(d)] = d
        return len(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=IPSW)
    ap.add_argument("--member-hdr", type=int, default=MEMBER_HDR)
    ap.add_argument("--member-size", type=int, default=MEMBER_SIZE)
    ap.add_argument("--cache", default="data/blobs/tmp/fsdmg-cache")
    ap.add_argument("--part", default="Apple_APFS")
    ap.add_argument("--list-parts", action="store_true")
    ap.add_argument("--out", default="data/blobs/macos-13.5/userspace/fs")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args()

    r = Remote(a.url, a.member_hdr, a.member_size, a.cache)
    if a.list_parts:
        for i, (n, _) in enumerate(r.parts):
            print(i, n)
        return
    idx = next(i for i, (n, _) in enumerate(r.parts) if a.part in n)
    r.use(idx)
    import pyfsapfs
    c = pyfsapfs.container()
    c.open_file_object(FileObj(r))
    vol = c.get_volume(0)
    print(f"volume {vol.name!r}, partition {r.parts[idx][0]!r}", file=sys.stderr)
    for path in a.paths:
        e = vol.get_file_entry_by_path(path)
        if e is None:
            print("missing:", path, file=sys.stderr)
            continue
        data = e.read_buffer(e.size) if e.size else b""
        dst = os.path.join(a.out, path.lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(data)
        print(f"{hashlib.sha256(data).hexdigest()}  {len(data):>9}  {path}")
    print(f"{r.nreq} range requests, {r.nbytes} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
