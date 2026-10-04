#!/usr/bin/env python3
"""ua.py xref SUBSTR | dis VA LEN | func VA   -- macOS 13.5 AppleVideoEncoder (user space), VA == file offset."""
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import sys, struct, re, functools
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
P=REPO_ROOT + '/data/blobs/macos-13.5-userspace/fs/System/Library/Video/Plug-Ins/AppleVideoEncoder.bundle/Contents/MacOS/AppleVideoEncoder'
d=open(P,'rb').read()
TEXT=(0x1290,0x1290+0xbd450)
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def cstr(va):
    if not (0xbf3e8<=va<0x115f40+0x58e3): return None
    s=va
    e=d.find(b'\0',va,va+300)
    if e<0: return None
    b=d[va:e]
    if len(b)>=3 and all(32<=c<127 or c in (9,10) for c in b): return b.decode()
    return None
@functools.lru_cache(None)
def refs():
    """scan adrp/add pairs -> {target: [insn va]}"""
    out={}
    adrp={}
    for x in range(TEXT[0],TEXT[1],4):
        w=struct.unpack_from('<I',d,x)[0]
        if (w&0x9f000000)==0x90000000:
            rd=w&31; immlo=(w>>29)&3; immhi=(w>>5)&0x7ffff; imm=((immhi<<2)|immlo)<<12
            if imm&(1<<32): imm-=1<<33
            adrp[rd]=((x&~0xfff)+imm, x)
        elif (w&0xffc00000)==0x91000000:
            rn=(w>>5)&31; imm=(w>>10)&0xfff
            if rn in adrp and x-adrp[rn][1]<64:
                t=adrp[rn][0]+imm; out.setdefault(t,[]).append(x)
        elif (w&0x9f000000)==0x10000000:  # adr
            immlo=(w>>29)&3; immhi=(w>>5)&0x7ffff; imm=(immhi<<2)|immlo
            if imm&(1<<20): imm-=1<<21
            out.setdefault(x+imm,[]).append(x)
    return out
def dis(va,ln):
    adrp={}
    for x in range(va,va+ln,4):
        w=d[x:x+4]; ins=list(md.disasm(w,x))
        if not ins: print('%6x: .word 0x%08x'%(x,struct.unpack('<I',w)[0])); continue
        i=ins[0]; ann=''
        if i.mnemonic=='adrp': adrp[i.op_str.split(',')[0]]=int(i.op_str.split('#')[1],16)
        elif i.mnemonic=='add' and '#' in i.op_str:
            ops=[t.strip() for t in i.op_str.split(',')]
            if ops[1] in adrp and len(ops)>2 and ops[2].startswith('#'):
                t=adrp[ops[1]]+int(ops[2].lstrip('#'),0); s=cstr(t); ann=' ; =0x%x'%t+(' "%s"'%s[:100] if s else '')
        elif i.mnemonic=='adr':
            t=int(i.op_str.split('#')[1],16); s=cstr(t); ann=' ; "%s"'%s[:100] if s else ''
        print('%6x: %s %s%s'%(x,i.mnemonic,i.op_str,ann))
def funcstart(va):
    # walk back to a pacibsp / stp x29,x30 prologue
    for x in range(va,va-0x8000,-4):
        w=struct.unpack_from('<I',d,x)[0]
        if w==0xd503237f: return x  # pacibsp
    return None
if sys.argv[1]=='xref':
    sub=sys.argv[2]; R=refs()
    i=0
    while True:
        i=d.find(sub.encode(),i)
        if i<0: break
        s=d.rfind(b'\0',0,i)+1
        for t in (s,):
            for x in R.get(t,[]): print('str@%x xref %x func~%s : %s'%(t,x,hex(funcstart(x) or 0),cstr(t)[:120]))
        i+=1
elif sys.argv[1]=='dis': dis(int(sys.argv[2],16),int(sys.argv[3],0))
elif sys.argv[1]=='func':
    s=funcstart(int(sys.argv[2],16)); print(hex(s))
