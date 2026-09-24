#!/usr/bin/env python3
"""List every store (and optionally load) into AVE_VIDEO_PARAMS / AVEFWRCSettings
made by the macOS user-space encoder (AppleVideoEncoder.bundle), from an
`ipsw macho disass` listing. docs/72-userspace-video-params.md.

The encoder's derived storage S holds AVE_SessionSettings_UserKernel_Data at
S+0xB0, so (docs/62 §1.2):
    RC  (AVEFWRCSettings, wire 0xFF30)  r  <->  S + 0xB0  + r
    VP  (AVE_VIDEO_PARAMS, wire 0x60)   v  <->  S + 0x860 + v
    DRV (pInfo+0x10680, 0x3A0 bytes)    d  <->  S + 0x10730 + d
Names are the firmware's own (CHEVCController::DebugInit, fw 0x39d10).

The register tracking is linear (no control-flow merges): a register is S+k
from an `add`/`mov` chain until it is redefined; `bl` kills x0..x18, and
CMBaseObjectGetDerivedStorage returns S. It is a finder, not a proof: check
each hit against the listing before citing it.

usage: ua_vp_stores.py LISTING LO HI SEEDS [loads]
   SEEDS: comma list of reg=off applied at each function entry (x0=0), and/or
          'auto' (a register used as [reg,#0x860|0x864|0x868] is S)
"""
#   also: 'auto' seeds any reg used as [reg,#0x868]/[reg,#0x864]/[reg,#0x860] at that point
import re,sys,json
names = {
    0x0: "ui32Width", 0x4: "ui32Height", 0x8: "EnableSelStatsFlags",
    0xc: "bEnableFwOverride", 0xd: "bEnableMBInputCtrl",
    0xe: "bEnableContextSwitchInTheMiddleOfAFrame",
    0xf: "bDisableBinCountsInNALunitCheck", 0x10: "MaxMvsPer2Mb",
    0x14: "MaxSubMbRectSize", 0x18: "BFrames", 0x1c: "bClosedGOP",
    0x1d: "bEnableAdaptB", 0x20: "LowDelay", 0xfc78: "verbose",
    0xfc7c: "bDisableIntra4x4", 0xfc7d: "bDisableIntra8x8",
    0xfc7e: "bDisableIntra16x16", 0xfc7f: "bRestrictInter4x4",
    0xfc80: "search_range", 0xfc82: "disable_skip_mode",
    0xfc83: "disable_intra_mode", 0xfc84: "enable_IPCM_in_IntraSlice",
    0xfc86: "bSkipThrdEn", 0xfc87: "no_bipred", 0xfc88: "pix_pck",
    0xfc8c: "mode_8x8_transform", 0xfc90: "skip_mode",
    0xfc94: "qcoeff_cancel", 0xfc98: "enable_tmvp", 0xfc9c: "numFPCPUCand",
    0xfca0: "visible_offx", 0xfca4: "visible_offy", 0xfca8: "visible_width",
    0xfcac: "visible_height", 0xfcb0: "sao_enb_config",
    0xfcb4: "sao_rdd_off_offset_luma", 0xfcb8: "sao_rdd_off_offset_chroma",
    0xfcbc: "sao_eo_bo_offset_config", 0xfcc0: "input_bitdepth",
    0xfcc4: "ltr_refidx", 0xfcc8: "long_term_ref",
    0xfccc: "max_num_ref_frames", 0xfcd4: "split_mode",
    0xfd48: "bSliceEncodingMode", 0xfe58: "numTemporalLayers",
    0xfe5a: "numBTemporalLayers", 0xfe98: "adaptB_poc_delay",
    0xfed0: "ui32Bitrate", 0xfed4: "ui32IdrPeriod",
    0xfee0: "bAllowFrameReordering", 0xfee4: "TotalNumberOfFrames",
    0xfee8: "ui32AverageNonDroppableFrameRate",
    0xfeec: "ui32ExpectedFrameRate", 0xfef0: "ui32RCFlag",
    0xff10: "bEnableQPMod", 0xff11: "bEnableQPModChroma",
    0xff12: "bEnableLamdaMod", 0xff18: "bEnableQPModRefresh",
    0xff19: "bUseCAVLCBits", 0xff1a: "bUseFrameDrop",
    0xff20: "eStaticAreasLowQpSel", 0xff24: "RealTimeClient",
    0xff28: "SoftMinQP", 0xff38: "ME_FullPelLambda",
    0xff3c: "ME_SubPelLambda", 0xff40: "ME_LowResLambda",
    0xff44: "MD_InterLambda", 0xff48: "MD_IntraLambda",
    0xff4c: "MD_IntraOffset", 0xff50: "usageMode", 0xff54: "ui32InitialQpI",
    0xff58: "ui32InitialQpP", 0xff5c: "ui32InitialQpB",
    0x10514: "pRCParams->RefSpacingP", 0x10518: "pRCParams->RefSpacingB0",
    0x1051c: "pRCParams->RefSpacingB1",
}

def fname(so):
    if 0xB0<=so<0x730: vp=so-0xB0+0xFED0
    elif 0x860<=so<0x10730: vp=so-0x860
    elif 0x10730<=so<0x10ad0: return f'DRV+{so-0x10730:#x} (pInfo+{so-0xb0:#x})'
    else: return None
    return f'VP+{vp:#x} wire {vp+0x60:#x} {names.get(vp,"")}'
LIST=sys.argv[1]
lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
seed={}
auto=False
for kv in sys.argv[4].split(','):
    if kv=='auto': auto=True
    elif kv: k,v=kv.split('='); seed[k]=int(v,0)
doloads=len(sys.argv)>5
def R(r): return 'x'+r[1:] if re.match(r'^[wx]\d+$',r) else r
base={}; const={}
for L in open(LIST):
    s=L.rstrip('\n')
    if re.match(r'^(sub_[0-9a-f]+|_[A-Za-z0-9_$]+):$',s):
        base=dict(seed); const={}; cur=s[:-1]; continue
    m=re.match(r'^0x([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$',s)
    if not m: continue
    va=int(m.group(1),16)
    if not(lo<=va<hi): continue
    op=m.group(2); a=m.group(3).split(';')[0]
    p=[x.strip() for x in re.split(r',(?![^\[]*\])',a)]
    if auto:
        mm=re.search(r'\[(x\d+), #0x86[048]\]',a)
        if mm and mm.group(1) not in base: base[mm.group(1)]=0
    ismem=re.match(r'(st|ld)(u?r|p|n?p)',op) is not None
    m2=re.search(r'\[(x\d+)(?:, (#-?0x[0-9a-f]+|#-?\d+|[xw]\d+(?:, [su]xtw)?))?\](!)?',a)
    if ismem and m2 and m2.group(1) in base:
        b=m2.group(1); o=m2.group(2)
        off=None
        if o is None: off=0
        elif o.startswith('#'): off=int(o[1:],0)
        elif R(o.split(',')[0]) in const: off=const[R(o.split(',')[0])]
        if off is not None and (op.startswith('st') or doloads):
            so=base[b]+off
            regs=p[:2] if op in('stp','ldp','stnp','ldnp') else p[:1]
            wch={'b':1,'h':2}.get(op[-1]) if op[-1] in 'bh' and not op.endswith('sh') and not op.endswith('sb') else None
            if op.endswith(('sb',)): wch=1
            if op.endswith(('sh',)): wch=2
            for i,r in enumerate(regs):
                w=wch or (8 if r[0] in 'xd' else 4 if r[0] in 'ws' else 16 if r[0]=='q' else 4)
                n=fname(so+i*w)
                if n:
                    cv=''
                    if op.startswith('st'):
                        rr=R(r)
                        if r in('wzr','xzr'): cv=' =0'
                        elif rr in const: cv=f' ={const[rr]:#x}'
                    print(f'{cur} {va:#x} {op} {r}{cv} S+{so+i*w:#x} ({w}) {n}')
    # destination update
    if op.startswith(('st','cmp','cmn','tst','b','cb','tb','ret','fcmp','ccmp','nop','pac','aut')) and op not in('bic','bics','bfi','bfxil'):
        if op in ('bl','blr','blraa','blraaz','blrab','blrabz'):
            for i in range(19): base.pop(f'x{i}',None); const.pop(f'x{i}',None)
            if 'GetDerivedStorage' in a: base['x0']=0
        continue
    if not p or not p[0]: continue
    d=R(p[0])
    if op in('mov','movz','orr') and len(p)==2 and p[1].startswith('#'):
        const[d]=int(p[1][1:],0); base.pop(d,None); continue
    if op=='movk' and d in const:
        sh=int(p[2].split('#')[1],0) if len(p)>2 else 0
        const[d]=(const[d] & ~(0xffff<<sh)) | (int(p[1][1:],0)<<sh); continue
    if op=='add' and len(p)>=3 and R(p[1]) in base and p[2].startswith('#'):
        imm=int(p[2][1:],0)
        if len(p)>3 and 'lsl' in p[3]: imm<<=int(p[3].split('#')[1],0)
        base[d]=base[R(p[1])]+imm; const.pop(d,None); continue
    if op=='sub' and len(p)>=3 and R(p[1]) in base and p[2].startswith('#'):
        base[d]=base[R(p[1])]-int(p[2][1:],0); const.pop(d,None); continue
    if op=='mov' and len(p)==2 and R(p[1]) in base and d.startswith('x'):
        base[d]=base[R(p[1])]; const.pop(d,None); continue
    if op=='mov' and len(p)==2 and R(p[1]) in const:
        const[d]=const[R(p[1])]; base.pop(d,None); continue
    if op.startswith('ld') and m2 and m2.group(3)=='!' : pass
    # kill
    base.pop(d,None); const.pop(d,None)
    if op in('ldp','ldnp') and len(p)>1: base.pop(R(p[1]),None); const.pop(R(p[1]),None)
