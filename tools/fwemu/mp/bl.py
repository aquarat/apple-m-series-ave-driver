import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import sys,struct
P=REPO_ROOT + '/data/blobs/macos-13.5-userspace/fs/System/Library/Video/Plug-Ins/AppleVideoEncoder.bundle/Contents/MacOS/AppleVideoEncoder'
d=open(P,'rb').read()
ts=[int(a,16) for a in sys.argv[1:]]
for x in range(0x1290,0x1290+0xbd450,4):
    w=struct.unpack_from('<I',d,x)[0]
    if (w&0xfc000000) in (0x94000000,0x14000000):
        imm=w&0x3ffffff
        if imm&(1<<25): imm-=1<<26
        t=x+imm*4
        if t in ts: print('%x -> %x %s'%(x,t,'bl' if w&0x80000000 else 'b'))
