#!/usr/bin/env python3
"""fd.py FW VA|symbol [len]  -- disassemble AVE firmware word by word with symbol + string annotation.
FW: h14g | h13s | h13c"""
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import sys, struct, re, bisect, os
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
R=REPO_ROOT + '/data/blobs/'
FWS={'h14g':R+'macos-13.5-j473/ave_h14g.bin','h13s':R+'macos-13.5-j314s/ave_h13s.bin','h13c':R+'macos-13.5-j314s/ave_h13c.bin','h13g':R+'macos-13.5-j473/ave_h13g.bin'}
def segs(d):
    ncmds=struct.unpack_from('<I',d,16)[0]; off=32; out=[]
    for _ in range(ncmds):
        cmd,sz=struct.unpack_from('<II',d,off)
        if cmd==0x19:
            name=d[off+8:off+24].rstrip(b'\0').decode(); va,vs,fo,fs=struct.unpack_from('<QQQQ',d,off+24); out.append((name,va,vs,fo,fs))
        off+=sz
    return out
def load(fw):
    d=open(FWS[fw],'rb').read(); S=segs(d)
    sys.path.insert(0,REPO_ROOT + '/tools/fwemu'); import fwsyms
    sy=fwsyms.syms(FWS[fw])
    return d,S,sy
def v2o(S,va):
    for n,sva,vs,fo,fs in S:
        if sva<=va<sva+fs: return fo+va-sva
    return None
def cstr(d,S,va):
    o=v2o(S,va)
    if o is None: return None
    e=d.find(b'\0',o,o+200)
    if e<0: return None
    s=d[o:e]
    if len(s)>=4 and all(32<=c<127 or c in (9,10) for c in s): return s.decode()
    return None
def main():
    fw=sys.argv[1]; d,S,sy=load(fw)
    a=sys.argv[2]
    if re.fullmatch(r'(0x)?[0-9a-fA-F]+',a): va=int(a,16)
    else:
        m=[(v,n) for n,v in sy.items() if a in n]
        if len(m)!=1: print(m); return
        va=m[0][0]
    ln=int(sys.argv[3],0) if len(sys.argv)>3 else None
    items=sorted((v,n) for n,v in sy.items()); keys=[v for v,n in items]
    if ln is None:
        i=bisect.bisect_right(keys,va); ln=(keys[i]-va) if i<len(keys) else 0x200
    md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
    adrp={}
    names={v:n for v,n in items}
    for x in range(va,va+ln,4):
        if x in names: print('<%s>'%names[x])
        o=v2o(S,x); w=d[o:o+4]
        ins=list(md.disasm(w,x))
        if not ins: print('%6x: .word 0x%08x'%(x,struct.unpack('<I',w)[0])); continue
        i=ins[0]; s='%s %s'%(i.mnemonic,i.op_str); ann=''
        if i.mnemonic=='adrp':
            r=i.op_str.split(',')[0]; adrp[r]=int(i.op_str.split('#')[1],16)
        elif i.mnemonic=='add' and '#' in i.op_str:
            ops=[t.strip() for t in i.op_str.split(',')]
            if ops[1] in adrp and len(ops)>2 and ops[2].startswith('#'):
                t=adrp[ops[1]]+int(ops[2].lstrip('#'),0)
                st=cstr(d,S,t); ann=' ; =0x%x'%t+(' "%s"'%st[:90] if st else '')
                if t in names: ann+=' <%s>'%names[t]
        elif i.mnemonic=='adr':
            t=int(i.op_str.split('#')[1],16); st=cstr(d,S,t); ann=' ; "%s"'%st[:90] if st else ''
        elif i.mnemonic in ('bl','b') or i.mnemonic.startswith('b.') or i.mnemonic in ('cbz','cbnz','tbz','tbnz'):
            if i.mnemonic!='bl' and i.mnemonic!='b': pass
            try:
                t=int(i.op_str.lstrip('#'),16)
                j=bisect.bisect_right(keys,t)-1
                if j>=0: ann=' ; %s%s'%(items[j][1],'' if items[j][0]==t else '+0x%x'%(t-items[j][0]))
            except: pass
        print('%6x: %s%s'%(x,s,ann))
main()
