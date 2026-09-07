#!/usr/bin/env python3
"""Recover AVE's host<->firmware protocol from the firmware's own symbols.

Apple shipped AppleAVE2FW with its symbol table and C++ source paths intact,
so a large part of the command interface can be reconstructed statically,
before any hypervisor tracing. This regenerates data/derived/*.

Usage: extract_protocol.py <unwrapped-firmware.bin> [--out data/derived]
"""
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho_info import MachO

def strings(data, minlen=4):
    return [s.decode("utf-8", "replace") for s in re.findall(rb"[ -~]{%d,}" % minlen, data)]

def cstrings(m):
    """(vmaddr -> string) for __TEXT,__cstring so string tables can be located."""
    for name, vmaddr, vmsize, fileoff, filesize, sects in m.segments:
        for sname, sgname, saddr, ssize, soff in sects:
            if sname == "__cstring":
                blob = m.d[soff:soff+ssize]
                out, off = {}, 0
                for part in blob.split(b"\0"):
                    if part:
                        out[saddr+off] = part.decode("utf-8", "replace")
                    off += len(part) + 1
                return out
    return {}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("--out", default="data/derived")
    a = ap.parse_args()
    m = MachO(open(a.firmware, "rb").read())
    syms = sorted(m.symbols(), key=lambda s: s[1])
    os.makedirs(a.out, exist_ok=True)

    # 1. Command handlers. CFlowControllerBase::CmdProcessor dispatches to these.
    #    The zero-argument handlers form one contiguous run in __text, emitted in
    #    source order -- very likely the dispatch/enum order. ProcessCmd_Start
    #    takes arguments and sits outside that run, so it is a helper called by
    #    Start_AVC/Start_HEVC rather than a table entry.
    def method(mangled):
        """Split __ZN<len><class><len><method>E<args>. Lengths must be honoured
        exactly -- method names such as ProcessCmd_Start_HEVC contain 'E', so a
        regex that stops at the first 'E' truncates them."""
        if not mangled.startswith("__ZN"):
            return None
        i, parts = 4, []
        while i < len(mangled) and mangled[i].isdigit():
            j = i
            while j < len(mangled) and mangled[j].isdigit():
                j += 1
            n = int(mangled[i:j])
            parts.append(mangled[j:j+n])
            i = j + n
        if len(parts) != 2 or i >= len(mangled) or mangled[i] != "E":
            return None
        return parts[0], parts[1], mangled[i+1:]

    handlers = []
    for n, v, _, _ in syms:
        parsed = method(n)
        if parsed and parsed[1].startswith("ProcessCmd_"):
            handlers.append((v, parsed[0], parsed[1], parsed[2]))
    table = [h for h in handlers if h[3] == "v"]     # void(void) -> dispatch entries
    helpers = [h for h in handlers if h[3] != "v"]
    cmds = table
    with open(f"{a.out}/commands.txt", "w") as f:
        f.write("# AVE command handlers recovered from the firmware symbol table.\n")
        f.write("# Dispatcher: CFlowControllerBase::CmdProcessor(void*, uint32_t, uint32_t*)\n")
        f.write("#\n# The index is this handler's position in the contiguous __text run.\n")
        f.write("# That is STRONGLY SUGGESTIVE of the wire command id but is NOT confirmed:\n")
        f.write("# it assumes the compiler emitted the handlers in enum order. Must be\n")
        f.write("# verified against a real mailbox trace before any driver relies on it.\n\n")
        for i, (v, cls, name, _) in enumerate(table):
            f.write(f"{i:>3}  {v:#010x}  {cls}::{name}()\n")
        if helpers:
            f.write("\n# Not dispatch-table entries (take arguments):\n")
            for v, cls, name, args in helpers:
                f.write(f"     {v:#010x}  {cls}::{name}(...)   [mangled args: {args}]\n")

    # 2. Pipeline state names. These sit as one contiguous run in __cstring,
    #    which is the classic shape of an enum-to-string table.
    cs = cstrings(m)
    order = sorted(cs)
    states = []
    for i, va in enumerate(order):
        if cs[va] == "INVALID":
            for va2 in order[i:]:
                s = cs[va2]
                if not re.fullmatch(r"[A-Z][A-Z0-9_]*", s):
                    break
                states.append((va2, s))
            break
    with open(f"{a.out}/pipeline-states.txt", "w") as f:
        f.write("# Contiguous __cstring run used by SendState()/remoteSystemState[].\n")
        f.write("# Index is the string's position in the run, i.e. probable enum value.\n\n")
        for i, (va, s) in enumerate(states):
            f.write(f"{i:>3}  {va:#010x}  {s}\n")

    # 3. Firmware source tree, recovered from assert()/log path strings.
    paths = sorted({s for s in strings(m.d) if s.startswith("./") and s.endswith((".cpp", ".c", ".h"))})
    with open(f"{a.out}/source-paths.txt", "w") as f:
        f.write("# Source paths left in the shipped binary by assert/log macros.\n\n")
        f.write("\n".join(paths) + "\n")

    # 4. Type names recovered from Itanium mangling (<len><identifier>).
    types = set()
    for n, _, _, _ in syms:
        for ln, nm in re.findall(r"(\d+)([A-Za-z_][A-Za-z0-9_]*)", n):
            if len(nm) >= int(ln):
                types.add(nm[:int(ln)])
    with open(f"{a.out}/types.txt", "w") as f:
        f.write("# Class/struct/enum names demangled out of the symbol table.\n\n")
        f.write("\n".join(sorted(types)) + "\n")

    # 5. Full symbol table, for grepping.
    with open(f"{a.out}/symbols.txt", "w") as f:
        for n, v, _, _ in syms:
            f.write(f"{v:#014x}  {n}\n")

    print(f"commands={len(cmds)} states={len(states)} paths={len(paths)} "
          f"types={len(types)} symbols={len(syms)} -> {a.out}/")

if __name__ == "__main__":
    main()
