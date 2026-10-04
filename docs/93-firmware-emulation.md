# 93. Emulating the firmware from live snapshots: the two-reference stall

*2026-10-04, M2 (t8112, docs/90). Instead of tracing macOS, run the
encoder's own firmware code under an emulator, from a snapshot of its live
memory, and see every register it programs for a frame. Applied to the
stall that blocks B-frames and multiple references (docs/81, docs/92 §2).*

## Result

- The firmware's per-frame set-up (`CAVCController::ProcessPipeStart`) runs
  to completion under Unicorn from a snapshot taken on the M2: DPB
  management, rate control, slice header, `setPipe`, `setLRME`, the MCPU
  start. **2964 register writes for a one-reference P frame, 2998 for a
  two-reference one.**
- The two-reference frame differs in **51 registers**, all of them the
  second reference's set-up, and every one mirrors reference 0's pattern:
  its luma/chroma readers point at DPB slot 1 with exactly slot 0's
  geometry, its low-res reader at the matching LowResRef slot, its
  per-reference enables and table entries set (§3). No zero address, no
  missing size, no field we send that only two references read.
- On the hardware, at the stall, **every register the firmware wrote holds
  what it wrote** (all 2921 read back; the differences are self-clearing go
  bits, status, progress counters, and 64-bit writes read 32 bits at a
  time) (§4).

So the firmware programs a two-reference frame completely, the hardware
accepts all of it, and the pipe still stops after two macroblocks. With
docs/53's runs (macOS's whole Start image, the firmware's own frame-type
path, clocks, DART streams) and docs/92 (the M2 repeats the M1 Max byte for
byte), what remains is hardware state that neither our commands nor the
firmware's per-frame code set up. The next step is the macOS trace
(docs/92 §3), now with a narrower question: what does macOS set **outside**
the encoder's per-frame registers.

## 1. Snapshots

Two read-only driver switches (off by default; loaded after boot):

| switch | what |
|---|---|
| `snap_hold_frame=N snap_hold_s=S` | in the probe-time self-test, wait S seconds after frame N's result (or timeout), before anything is torn down |
| `dva_debugfs=1` | `/sys/kernel/debug/apple_ave_dva`: read at offset X = what the encoder sees at device address X (through the IOMMU domain; unmapped reads as zeros). `/sys/kernel/debug/apple_ave_mmio`: read at offset X = `readl` of physical address X, refused outside the encoder's own register banks |

`tools/fwemu/snap-target.py` copies the firmware (DVA 0x800000000,
0x1f8000 bytes: TEXT, DATA and its heap, docs/90 §3) and the driver's
buffers (IOVA 0x80000000-0xffffffff) during the hold.

Two things the reader had to get right, both found the hard way:
- the firmware carve-outs are no-map memory: `pfn_valid()`, but not in the
  kernel's linear map, so `page_address()` faults there (an oops in the
  reading process, nothing else). They are read through `memremap()`.
- reads go through **uncached** mappings: the driver writes its buffers
  through uncached aliases and the coprocessor writes DRAM behind the CPU
  caches, so the cacheable linear map returned stale zeros where the Process
  command had been written.

## 2. The firmware's address space

From its boot code (fw 0x200-0x560) and its live page tables:

| VA | what | backing |
|---|---|---|
| 0x0-0x1f8000 | TEXT (0x0, page 0 unmapped), DATA (0xd0000), heap | DVA 0x800000000 + VA: the firmware runs at its link addresses |
| 0xffffffff80000000 | FwIPC (7 MiB) | driver buffer |
| 0xffffffff80700000 | the firmware heap surface (768 KiB) | driver buffer |
| 0xffffffff807c0000 | the client buffer (528 KiB): the per-frame PICMGMT copy and the `CAVCController` | driver buffer |
| 0xfffffffff2000000 | ASC CPU-control and ASC (0x267c00000, 0x267800000) | MMIO |
| 0xfffffffff4000000 | the register window used for the pipe (`[0x1f3e60]`) = AP 0x266000000 + offset | MMIO |

16 KiB pages; the high half is described by the L2 table at DVA
0x8000f0000 (DATA+0x20000, `_rtk_page_tables`); `tools/fwemu/fwmem.py`
decodes it per snapshot, since the driver's IOVAs differ between loads.

## 3. The emulation

`tools/fwemu/emu.py` maps the image and the high half from a snapshot,
maps both register windows as logging hooks (reads return 0: the reads in
this path are read-modify-write bit updates, not decisions), stubs logging,
task-ID and assert functions by symbol (the firmware keeps its symbol
table), and calls a function with a return trap. `locate.py` finds the
`CAVCController` (its vtable, `__ZTV14CAVCController` + 16 = 0xd1200) and
the frame's `sCmdInformation` (PICMGMT pointer at +0x10).

```sh
# on the target, bs1 (docs/53) held after frame 2: two L0 references, hangs
sudo insmod driver/apple-ave.ko v4l2=0 session_frame=1 session_frames=3 session_poc0=1 \
     session_dpb=3 session_profile=77 session_ref_spacing_p=2 \
     dva_debugfs=1 snap_hold_frame=2 snap_hold_s=75 &
sudo python3 tools/fwemu/snap-target.py /tmp/snap-S2
# on the host (snapshots never go into git: they hold Apple's firmware)
python3 tools/fwemu/locate.py S2 2
.venv/bin/python tools/fwemu/emu.py S2 __ZN14CAVCController16ProcessPipeStartEPv <ctrl> <cmdinfo> > S2.txt
python3 tools/fwemu/diff.py S2.txt S3.txt      # S3: the same without RefSpacingP 2
```

The second reference, against the one-reference frame (excerpt; register =
AP physical address):

| register | 2 refs | 1 ref | what |
|---|---|---|---|
| 0x267128040 / +0x240 | `0x84127f05` / `0x80127f75` | `…04` / `…74` | luma / chroma reference reader 1: enabled |
| 0x267128048 / +0x60 | 0xf3ada000 / 0xf3c33000 | - | reader 1 MSB plane / metadata plane = DPB slot 1 (slot 0 = 0xf396d000 / 0xf3ac6000; slot stride 0x16d000, MSB 0x159000) |
| 0x267128248 / +0x260 | 0xf3bc0000 / 0xf3c43000 | - | chroma, slot 1 + 0xe6000 / + 0x169000, as slot 0's |
| 0x267120600, 0x267120610 | `0x84120005`, 0xf3778000 len 0x1400 | `…04`, - | LRME reference reader 1 = LowResRef slot 2, as the firmware's DPB entry pairs it with recon slot 1 |
| 0x267110010, 0x2671e0010, 0x26720a024 | 0xc0010000 | 0 | per-reference words (docs/53 bs4 saw them on the M1 Max) |
| 0x267190250 | 2 | 1 | reference count |
| 0x267190630, 0x26726a090 | 0x400, 0x100 | - | weighted-prediction scaling for reference 1 (weight 0: unity, as reference 0) |
| 0x2672ba09c / 0ac | 2 | 0xf | per-reference table entry (0xf = none) |
| 0x2674c8000/8008 | MCPU parameters | | ConfigureMCPUs, differs as expected |

Also found on the way: the M2's new ME set-up (`setupMeSetupConfig`,
`setupMeCGenConfig`) runs only when wire 0xFCE9 (`session_src_bit3`) is
set, which macOS's user space leaves at 0 (docs/72). Setting it hangs even
the one-reference frame (`PIPE HANG: 2, 2`), so it is not the missing piece.

## 4. Did the writes land?

`tools/fwemu/mmio-target.py` read back, at the two-reference stall, every
register the emulation says the firmware wrote (2921 addresses; each is a
register the firmware writes, so safe to read; no error, no SError):

- 2635 hold exactly the emulated value;
- 242 are 64-bit MCPU-parameter writes whose low 32 bits match (the read is
  32-bit);
- the rest are self-clearing go bits (`0x267110124/128` 1 -> 0), status
  (`0x26711013c`), the reference readers' "running" bit (0x...05 -> 0x...01),
  and progress counters: the LowResResult readers stand at 0xEC00 of 0xF000,
  as on the M1 Max (docs/53 b4d).

Nothing the firmware programmed was dropped or altered.

## 5. Uses beyond this question

The same snapshot-and-emulate loop answers "what would the firmware program
if …" for any per-frame input, without hardware: e.g. the multi-pass
fields, B-frame reordering (`ProcessPipeStart` for a held B), or the
M2-only source-reader fields (docs/90 §5). Snapshots are ~1-2 MB
compressed.
