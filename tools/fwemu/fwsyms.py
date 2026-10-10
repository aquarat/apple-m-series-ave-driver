# SPDX-License-Identifier: GPL-2.0-only
"""Function symbols and sizes from an AVE firmware Mach-O (it keeps its symbol table)."""
import struct
def syms(p):
    d=open(p,"rb").read()
    ncmds=struct.unpack_from("<I",d,16)[0]; off=32; out={}
    for _ in range(ncmds):
        cmd,sz=struct.unpack_from("<II",d,off)
        if cmd==2:
            symoff,nsyms,stroff,strsize=struct.unpack_from("<IIII",d,off+8)
            for i in range(nsyms):
                n_strx,n_type,n_sect,n_desc,n_value=struct.unpack_from("<IBBHQ",d,symoff+16*i)
                name=d[stroff+n_strx:d.index(b"\0",stroff+n_strx)].decode()
                if n_sect==1 and name: out[name]=n_value
        off+=sz
    return out
def sized(p):
    s=syms(p); items=sorted(s.items(), key=lambda kv: kv[1])
    res={}
    for i,(n,a) in enumerate(items):
        nxt=items[i+1][1] if i+1<len(items) else a
        res[n]=(a, nxt-a)
    return res
