#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""kd.py VA|symbol [len] -- disassemble the M2 13.5 kernelcache (AppleAVE2), with symbol/string annotation."""
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import sys,struct,re,bisect
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
B=REPO_ROOT + '/data/blobs/macos-13.5-j473/'
d=open(B+'kc14g.macho','rb').read()
def segs():
    ncmds=struct.unpack_from('<I',d,16)[0]; off=32; out=[]
    for _ in range(ncmds):
        cmd,sz=struct.unpack_from('<II',d,off)
        if cmd==0x19:
            name=d[off+8:off+24].rstrip(b'\0').decode(); va,vs,fo,fs=struct.unpack_from('<QQQQ',d,off+24); out.append((name,va,vs,fo,fs))
        off+=sz
    return out
S=segs()
def v2o(va):
    for n,sva,vs,fo,fs in S:
        if sva<=va<sva+fs: return fo+va-sva
SY=[]
for l in open(B+'kext14g-symbols.txt'):
    a,_,n=l.partition('  '); a=int(a,16)
    if a: SY.append((a,n.strip()))
SY.sort(); K=[a for a,n in SY]
def sym(va):
    i=bisect.bisect_right(K,va)-1
    if i<0: return ''
    a,n=SY[i]; return n if a==va else '%s+0x%x'%(n,va-a)
def cstr(va):
    o=v2o(va)
    if o is None: return None
    e=d.find(b'\0',o,o+200)
    if e<0: return None
    s=d[o:e]
    if len(s)>=4 and all(32<=c<127 or c in (9,10) for c in s): return s.decode()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
a=sys.argv[1]
if a.startswith('0x') or re.fullmatch('[0-9a-f]{16}',a): va=int(a,16)
else:
    m=[(x,n) for x,n in SY if a in n and '_os_log' not in n]
    if len(m)!=1: print(m[:20]); sys.exit()
    va=m[0][0]
if len(sys.argv)>2: ln=int(sys.argv[2],0)
else:
    i=bisect.bisect_right(K,va); ln=K[i]-va if i<len(K) else 0x400
    ln=min(ln,0x4000)
adrp={}
for x in range(va,va+ln,4):
    o=v2o(x); w=d[o:o+4]
    if x in dict(SY): print('<%s>'%dict(SY)[x]) if False else None
    ins=list(md.disasm(w,x))
    if not ins: print('%x: .word 0x%08x'%(x,struct.unpack('<I',w)[0])); continue
    i=ins[0]; ann=''
    if i.mnemonic=='adrp': adrp[i.op_str.split(',')[0]]=int(i.op_str.split('#')[1],16)
    elif i.mnemonic=='add':
        ops=[t.strip() for t in i.op_str.split(',')]
        if len(ops)>2 and ops[1] in adrp and ops[2].startswith('#'):
            t=adrp[ops[1]]+int(ops[2][1:],0); s=cstr(t); ann=' ; =0x%x %s'%(t,('"%s"'%s[:90]) if s else sym(t))
    elif i.mnemonic in ('bl','b'):
        try: ann=' ; '+sym(int(i.op_str.lstrip('#'),16))
        except: pass
    print('%x: %s %s%s'%(x&0xffffffffff,i.mnemonic,i.op_str,ann))
