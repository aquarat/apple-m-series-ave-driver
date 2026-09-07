#!/usr/bin/env python3
"""List/extract kexts from an Apple Silicon MH_FILESET kernelcache.

AppleAVE2.kext is the HOST side of the AVE protocol: it holds the mailbox
endpoint numbers, the command ids, and the shared-memory struct handling that
the firmware image alone does not reveal. Getting at it requires no hardware
and no hypervisor -- just the kernelcache from any IPSW for the target Mac.

Usage:
  kext_extract.py <kernelcache.macho> --list
  kext_extract.py <kernelcache.macho> --extract com.apple.driver.AppleAVE2 -o out.macho
"""
import argparse, struct, sys

LC_REQ_DYLD = 0x80000000
LC_SEGMENT_64, LC_SYMTAB, LC_FILESET_ENTRY = 0x19, 0x02, 0x35

def load_commands(d):
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags = struct.unpack("<IIIIIII", d[:28])
    if magic != 0xfeedfacf:
        sys.exit(f"not a 64-bit Mach-O ({magic:#x})")
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", d[off:off+8])
        yield cmd, cmdsize, off
        off += cmdsize

def fileset_entries(d):
    """(entry_id, vmaddr, fileoff) for each embedded kext."""
    for cmd, cmdsize, off in load_commands(d):
        # Apple sets LC_REQ_DYLD on LC_FILESET_ENTRY in kernelcaches.
        if (cmd & ~LC_REQ_DYLD) == LC_FILESET_ENTRY:
            vmaddr, fileoff, stroff = struct.unpack("<QQI", d[off+8:off+28])
            name_off = off + stroff
            end = d.find(b"\0", name_off)
            yield d[name_off:end].decode(), vmaddr, fileoff

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kernelcache")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--grep", help="substring filter for --list")
    ap.add_argument("--extract", help="entry id to extract")
    ap.add_argument("-o", "--out", default="kext.macho")
    a = ap.parse_args()
    d = open(a.kernelcache, "rb").read()
    entries = sorted(fileset_entries(d), key=lambda e: e[2])

    if a.list or not a.extract:
        for name, vmaddr, fileoff in entries:
            if a.grep and a.grep.lower() not in name.lower():
                continue
            print(f"  {fileoff:#012x}  vm={vmaddr:#014x}  {name}")
        print(f"\n{len(entries)} fileset entries")
        return

    for i, (name, vmaddr, fileoff) in enumerate(entries):
        if name == a.extract:
            # Slice to the next entry's file offset. The kext's own Mach-O
            # header sits at fileoff; segments are shared with the cache, so
            # this is enough for symbol/string analysis but is NOT a
            # standalone loadable kext.
            end = entries[i+1][2] if i+1 < len(entries) else len(d)
            open(a.out, "wb").write(d[fileoff:end])
            print(f"extracted {name}: {end-fileoff} bytes at {fileoff:#x} -> {a.out}")
            return
    sys.exit(f"no fileset entry named {a.extract}")

if __name__ == "__main__":
    main()
