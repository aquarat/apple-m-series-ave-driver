#!/usr/bin/env python3
"""ave1 facts from the 13.5 ADT (docs/82 §1): read-only. Run from the repo root."""
import sys
sys.path.insert(0,'m1n1-src/proxyclient')
from m1n1.adt import load_adt
adt=load_adt(open('data/blobs/macos-13.5/adt.bin','rb').read())
pmgr=adt['/arm-io/pmgr']
ranges=adt['/arm-io']._properties['ranges']
def tr(bus):
    for r in ranges:
        if r.bus_addr<=bus<r.bus_addr+r.size: return bus-r.bus_addr+r.parent_addr
print('ranges',[(hex(r.bus_addr),hex(r.parent_addr),hex(r.size)) for r in ranges])
by2={d.id2:d for d in pmgr.devices}
def parents(d):
    try: return [p for p in d.parents_un.u16id.parents if p]
    except Exception: return []
def ps(d):
    if d.flags.no_ps: return None
    r=pmgr.ps_regs[d.psreg]
    return pmgr.get_reg(r.reg)[0]+r.offset+d.psidx*8
def show(i):
    d=by2[i]; a=ps(d)
    fl=[k for k in ('no_ps','notify_pmp','critical','perf','on','b7','b6','b2') if getattr(d.flags,k)]
    print(f"  {i:4d} {d.name:18s} ps={hex(a) if a else '-':14s} psreg={d.psreg} psidx={d.psidx} id1={d.id1} lvl={d.unk1_0} dom={d.unk1_1} parents={[ (p, by2[p].name if p in by2 else '?') for p in parents(d)]} flags={fl}")
for inst in ('ave0','ave1'):
    n=adt['/arm-io/'+inst]; dn=adt['/arm-io/dart-'+inst]
    print('==',inst)
    for i,r in enumerate(n.reg): print(f'  reg[{i}] bus {r.addr:#x} -> {tr(r.addr):#x} +{r.size:#x}')
    print('  irqs',list(n.interrupts), 'dart irq', list(dn.interrupts))
    for i,r in enumerate(dn.reg): print(f'  dart reg[{i}] bus {r.addr:#x} -> {tr(r.addr):#x} +{r.size:#x}')
    print(' power-gates:')
    for g in n._properties['power-gates']: show(g)
    print(' dart gates:')
    for g in dn._properties['power-gates']: show(g)
print('== chains')
for start in (294,364):
    seen=[start]; q=[start]
    while q:
        x=q.pop(0)
        for p in parents(by2[x]):
            if p not in seen: seen.append(p); q.append(p)
    for s in seen: show(s)
    print()
print('== ps in 0x28e680180..0x28e6802a0 and 0x28e688000..')
for d in pmgr.devices:
    a=ps(d)
    if a and (0x28e680180<=a<0x28e6802a0 or 0x28e688000<=a<0x28e688040 or 0x28e588000<=a<0x28e588040): print(f"  {a:#x} {d.id2} {d.name} parents={[by2[p].name for p in parents(d) if p in by2]}")
for i in (10,13,14,15,17):
    r=pmgr.ps_regs[i]; print('ps_regs',i,r, hex(pmgr.get_reg(r.reg)[0]), hex(pmgr.get_reg(r.reg)[1]))
