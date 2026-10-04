#!/usr/bin/env python3
"""Run a firmware function on SYNTHETIC state (no snapshot) and log its pipe-register writes
(docs/89 §8.4). For builds we have no snapshot of (H13G, H13S): zeroed controller, a few
fields set by hand, every other input zero.

  synth.py FW FUNC [SPEC ...] > writes.txt        FW: h13g | h13s | h14g
  synth.py FW avc-setpipe [SPEC ...]               the preset below, then SPECs

x0 = ctrl (0x40000000), x1 = picmgmt (0x42000000); regions ctrl, sh (0x41000000), pic, scr.
SPEC:
  ctrl+0x22f12=1:1             write a value (size 1/2/4/8, default 4)
  ptr:ctrl+0x2d198=sh          store a region's address
  fill:ctrl+0xd18..0xd58:4=0x70000000   distinct non-zero 0x40000-aligned values
  refs                         PICMGMT reference/recon/low-res pointer arrays, distinct and aligned
Output: one "W <window offset> <size> <value> pc <pc>" per write to the pipe-register window
(the firmware's [window global] = AP 0x266000000 on t8103/t8112, so 0x11e0000 = DPE+0xE0000);
tools/fwemu/diff.py compares two runs. Asserts stop the run and name their condition.

Controller offsets are per build (H13G's MultiME is ctrl+0x22f12, H13S's 0x22f1a, H14G's
0x23292); the avc-setpipe preset knows h13g and h13s. Zeroed state means zero-sized
pictures and default modes: compare builds or settings against each other, never against
hardware values.
"""
import sys, os, struct, re
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, \
    UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL, UcError
from unicorn.arm64_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fwsyms import sized
REPO = os.path.dirname(os.path.dirname(HERE))
B = os.path.join(REPO, 'data/blobs')
# firmware image, and the global holding the pipe-register window pointer
FWS = {'h14g': (B + '/macos-13.5-j473/ave_h14g.bin', 0x1f3e60),
       'h13s': (B + '/macos-13.5-j314s/ave_h13s.bin', 0x1f3aa0),
       'h13g': (B + '/macos-13.5-j473/ave_h13g.bin', 0x1efaa0)}
REG = {'ctrl': 0x40000000, 'sh': 0x41000000, 'pic': 0x42000000, 'scr': 0x43000000}
WIN = 0xfffffffff4000000
STACK, TRAP = 0x10000000, 0x20000000
# CAVCController::setPipe(PICMGMT*): the slice block pointer for slot 0, the fields its
# asserts need (encoder_addr_fw_data, src_nbr_info, src_nbr_pixels, entropy[][]),
# and the reference pointers. SH+0x10 slice type (0 P, 1 B, 2 I), SH+0x44/0x48 L0/L1
# active - 1; MultiME is the byte named below.
PRESETS = {
    ('h13g', 'avc-setpipe'): ('__ZN14CAVCController7setPipeEP18AVE_PICMGMT_PARAMS',
        ['refs', 'ptr:ctrl+0x2d198=sh', 'ctrl+0xef4=0x60000000', 'ctrl+0xd58=0x61000000',
         'ctrl+0xd60=0x62000000', 'fill:ctrl+0xd18..0xd58:4=0x70000000']),
    ('h13s', 'avc-setpipe'): ('__ZN14CAVCController7setPipeEP18AVE_PICMGMT_PARAMS',
        ['refs', 'ptr:ctrl+0x2d1a0=sh', 'ctrl+0xef8=0x60000000', 'ctrl+0xd58=0x61000000',
         'ctrl+0xd60=0x62000000', 'fill:ctrl+0xd18..0xd58:4=0x70000000']),
}
MULTIME = {'h13g': 0x22f12, 'h13s': 0x22f1a, 'h14g': 0x23292}
STUBS = ("__ZNK14CAVEFilterBase5Print", "__ZN7CLogger", "__Z17AVE_History_Print", "_printf",
         "__ZN9CTaskPool16GetCurrentTaskID", "__ZNK10CAVEObject5Print", "__ZN10CAVEObject5Print",
         "_rtk_printf")


def apply(uc, a):
    m = re.fullmatch(r'fill:(\w+)\+(0x[0-9a-f]+)\.\.(0x[0-9a-f]+):(\d)=(0x[0-9a-f]+)', a)
    if m:
        s = int(m.group(4))
        for k, o in enumerate(range(int(m.group(2), 16), int(m.group(3), 16), s)):
            v = (int(m.group(5), 16) + k * 0x40000) & ((1 << (8 * s)) - 1)
            uc.mem_write(REG[m.group(1)] + o, v.to_bytes(s, 'little'))
        return
    if a == 'refs':
        # sRef L0 0x6f8..0x797, L1 0x798..0x837, Colocated_L1 0x838, sRecon 0x898..0x8b8,
        # low-res outputs up to 0xa00; LowResResults etc. 0xc10..0xca7 (0xca8 = frameNumber)
        for k, o in enumerate(range(0x6f8, 0xa00, 8)):
            uc.mem_write(REG['pic'] + o, struct.pack('<Q', 0x80000000 + 0x100000 * k))
        for k, o in enumerate(range(0xc10, 0xca8, 8)):
            uc.mem_write(REG['pic'] + o, struct.pack('<Q', 0xa0000000 + 0x100000 * k))
        return
    m = re.fullmatch(r'ptr:(\w+)\+(0x[0-9a-f]+)=(\w+)(?:\+(0x[0-9a-f]+))?', a)
    if m:
        uc.mem_write(REG[m.group(1)] + int(m.group(2), 16),
                     struct.pack('<Q', REG[m.group(3)] + int(m.group(4) or '0', 16)))
        return
    m = re.fullmatch(r'(\w+)\+(0x[0-9a-f]+)=(-?0x[0-9a-f]+|-?\d+)(?::(\d))?', a)
    if not m:
        sys.exit('bad SPEC %r' % a)
    s = int(m.group(4) or 4)
    v = int(m.group(3), 0) & ((1 << (8 * s)) - 1)
    uc.mem_write(REG[m.group(1)] + int(m.group(2), 16), v.to_bytes(s, 'little'))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fw, func = sys.argv[1], sys.argv[2]
    specs = sys.argv[3:]
    if (fw, func) in PRESETS:
        func, pre = PRESETS[(fw, func)]
        specs = pre + specs
    specs = [s.replace('multime=', 'ctrl+%#x=' % MULTIME[fw]) + (':1' if s.startswith('multime=') else '')
             for s in specs]
    path, wglob = FWS[fw]
    d = open(path, 'rb').read()
    syms = sized(path)
    addr = {k: v[0] for k, v in syms.items()}
    rev = {v[0]: k for k, v in syms.items()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    uc.mem_map(0, 0x200000, UC_PROT_ALL)              # TEXT, DATA, bss at their link addresses
    ncmds = struct.unpack_from('<I', d, 16)[0]; off = 32
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', d, off)
        if cmd == 0x19:
            va, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
            if fs and va < 0x200000:
                uc.mem_write(va, d[fo:fo + fs])
        off += sz
    for b in REG.values():
        uc.mem_map(b, 0x100000, UC_PROT_ALL)
    uc.mem_map(WIN, 0x2000000, UC_PROT_ALL)
    uc.mem_map(STACK, 0x100000, UC_PROT_ALL)
    uc.mem_map(TRAP, 0x1000, UC_PROT_ALL)
    uc.mem_write(TRAP, b"\x00\x00\x20\xd4")           # brk #0
    uc.mem_write(wglob, struct.pack('<Q', WIN))
    for a in specs:
        apply(uc, a)

    log = []
    def on_w(u, acc, a, size, val, _):
        if WIN <= a < WIN + 0x2000000:
            log.append((a - WIN, size, val & ((1 << (8 * size)) - 1), u.reg_read(UC_ARM64_REG_PC)))
    uc.hook_add(UC_HOOK_MEM_WRITE, on_w)
    stubs = {a for n, a in addr.items() if n.startswith(STUBS)}
    stop = addr.get('_bsp_assert_fail')
    calls = []
    def on_code(u, a, sz, _):
        if a == stop:
            # the assert's message: the adr x8 at the return address
            lr = u.reg_read(UC_ARM64_REG_X30)
            w = struct.unpack('<I', u.mem_read(lr, 4))[0]
            msg = ''
            if (w & 0x9f000000) == 0x10000000:
                imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                b = bytes(u.mem_read(lr + imm, 200))
                msg = b[:b.index(b'\0')].decode(errors='replace')
            calls.append('ASSERT<-%#x "%s"' % (lr, msg))
            u.emu_stop()
        elif a in stubs:
            u.reg_write(UC_ARM64_REG_X0, 0)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
        elif a in rev and a != addr[func]:
            calls.append(re.sub(r'__ZN\d+', '', rev[a])[:40])
    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, lambda u, acc, a, sz, v, _: (
        calls.append('UNMAPPED %#x pc %#x' % (a, u.reg_read(UC_ARM64_REG_PC))), False)[1])
    uc.reg_write(UC_ARM64_REG_X0, REG['ctrl'])
    uc.reg_write(UC_ARM64_REG_X1, REG['pic'])
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff000)
    uc.reg_write(UC_ARM64_REG_X30, TRAP)
    try:
        uc.emu_start(addr[func], TRAP, count=20_000_000)
        pc = uc.reg_read(UC_ARM64_REG_PC)
        end = 'returned' if pc == TRAP else 'stopped at %#x' % pc
    except UcError as e:
        end = 'error %s at pc %#x' % (e, uc.reg_read(UC_ARM64_REG_PC))
    print('# %s %s: %s, %d writes; calls: %s' % (fw, func, end, len(log), ' '.join(dict.fromkeys(calls))))
    for o, s, v, pc in log:
        print('W %#09x %d %#x pc %#x' % (o, s, v, pc))


if __name__ == '__main__':
    main()
