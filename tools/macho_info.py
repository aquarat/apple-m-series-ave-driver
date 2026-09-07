#!/usr/bin/env python3
"""Parse the AVE RTKit firmware Mach-O (MH_PRELOAD, arm64e).

Apple ships AppleAVE2FW_<variant>.im4p as a standard Mach-O preload image.
This dumps its load commands, segments and symbol table so the firmware's
internal structure can be mapped without a disassembler.

Usage: macho_info.py <unwrapped-firmware.bin> [--syms] [--filter REGEX]
"""
import struct, sys, re, argparse

LC_SEGMENT_64, LC_SYMTAB, LC_UUID, LC_UNIXTHREAD = 0x19, 0x02, 0x1b, 0x05
LC_NAMES = {0x19:"LC_SEGMENT_64", 0x02:"LC_SYMTAB", 0x1b:"LC_UUID",
            0x05:"LC_UNIXTHREAD", 0x0b:"LC_DYSYMTAB", 0x2a:"LC_SOURCE_VERSION",
            0x32:"LC_BUILD_VERSION", 0x1d:"LC_CODE_SIGNATURE", 0x26:"LC_FUNCTION_STARTS"}

class MachO:
    def __init__(self, data):
        self.d = data
        magic, self.cputype, self.cpusubtype, self.filetype, self.ncmds, \
            self.sizeofcmds, self.flags = struct.unpack("<IIIIIII", data[:28])
        if magic != 0xfeedfacf:
            raise ValueError(f"not a 64-bit Mach-O (magic {magic:#x})")
        self.cmds, self.segments, self.symtab = [], [], None
        off = 32
        for _ in range(self.ncmds):
            cmd, cmdsize = struct.unpack("<II", data[off:off+8])
            self.cmds.append((cmd, cmdsize, off))
            if cmd == LC_SEGMENT_64:
                name = data[off+8:off+24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack("<QQQQ", data[off+24:off+56])
                nsects = struct.unpack("<I", data[off+64:off+68])[0]
                sects = []
                so = off + 72
                for _ in range(nsects):
                    sname = data[so:so+16].rstrip(b"\0").decode()
                    sgname = data[so+16:so+32].rstrip(b"\0").decode()
                    saddr, ssize, soff = struct.unpack("<QQI", data[so+32:so+52])
                    sects.append((sname, sgname, saddr, ssize, soff))
                    so += 80
                self.segments.append((name, vmaddr, vmsize, fileoff, filesize, sects))
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack("<IIII", data[off+8:off+24])
            off += cmdsize

    def symbols(self):
        """Yield (name, value, type_byte, sect). nlist_64 is 16 bytes."""
        if not self.symtab:
            return
        symoff, nsyms, stroff, strsize = self.symtab
        strtab = self.d[stroff:stroff+strsize]
        for i in range(nsyms):
            o = symoff + i*16
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack("<IBBHQ", self.d[o:o+16])
            end = strtab.find(b"\0", n_strx)
            name = strtab[n_strx:end].decode("utf-8", "replace")
            if name:
                yield name, n_value, n_type, n_sect

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("--syms", action="store_true", help="list symbols")
    ap.add_argument("--filter", help="regex to filter symbol names")
    a = ap.parse_args()
    m = MachO(open(a.firmware, "rb").read())
    print(f"filetype={m.filetype} (5=MH_PRELOAD)  ncmds={m.ncmds}  "
          f"cputype={m.cputype:#x} cpusubtype={m.cpusubtype:#x} (arm64e)")
    print("\n=== load commands ===")
    for cmd, size, off in m.cmds:
        print(f"  {LC_NAMES.get(cmd, hex(cmd)):<22} size={size}")
    print("\n=== segments ===")
    for name, vmaddr, vmsize, fileoff, filesize, sects in m.segments:
        print(f"  {name:<12} vm={vmaddr:#014x}+{vmsize:#010x}  file={fileoff:#010x}+{filesize:#010x}")
        for sname, sgname, saddr, ssize, soff in sects:
            print(f"      {sgname}.{sname:<18} {saddr:#014x}+{ssize:#010x}")
    syms = list(m.symbols())
    print(f"\n=== symbols: {len(syms)} ===")
    if a.syms:
        pat = re.compile(a.filter, re.I) if a.filter else None
        for name, val, t, sect in sorted(syms, key=lambda s: s[1]):
            if pat and not pat.search(name):
                continue
            print(f"  {val:#014x}  {name}")

if __name__ == "__main__":
    main()
