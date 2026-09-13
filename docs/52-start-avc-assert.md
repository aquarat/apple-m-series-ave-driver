# `MappedMemory.cpp:39` on the first `Start_AVC` (macOS 13.5)

The hardware run of 2026-09-13 20:43 (`results/cmd1-1789328606.kmsg`, [31](31-bringup-state.md))
got `Config` and `Open` accepted (`0xEE0000`) and then, on `Start_AVC`
(13.5 `CAVE_CMD_AVC_INIT`, id 4, `0x10E10` bytes):

```
fw[3]| SCRATCH_REG32_RD: addr fffffffff5050034 value 00000001
fw[3]| SCRATCH_REG32_WR: addr fffffffff5050034 value 40000001
fw[3]| ASSERT: ./AppleAVE2FW/utils/MappedMemory.cpp, 39: paddr != 0
fw[0]| 4  2170415256  1  128  2340  0  0  Start AVC 1
```

and no completion.

**Answer: the zero is the u64 at `Start_AVC` wire offset `0xFB30`, with its
length at `0xFB38`.** Apple's own name for the pair, from the kext's assert
string, is

```
pVP->p_ParameterSetsBuffer != 0 && pVP->p_ParameterSetsBufferSize != 0
```

— the host-provided buffer the firmware copies the generated SPS and PPS NAL
bytes into. `driver/ave_cmd.c` has no field for it, so our command carries zero
there. It is **not** `Config +0x48` (§4 explains why that candidate is dead).

All firmware VAs below are 13.5 image VAs (`__TEXT` VA 0 = file `0x4000`,
`__DATA` VA `0xec000` = file `0xf0000`); kext VAs are 13.5 kernelcache VAs.
Reproduce with `AVE_MACOS=13.5 python3 tools/disas.py --fw|--kext --addr VA -n LEN`.

---

## 1. The assert site

`MappedMemory::MappedMemory(unsigned long paddr, unsigned long size, bool)`
— base ctor `__ZN12MappedMemoryC2Emmb` at **`0x209d4`**, complete ctor
`__ZN12MappedMemoryC1Emmb` at **`0x20bc0`** (the one every caller uses).

```
209e8:  strb w3, [x0]          ; this->flag = arg3
209ec:  stp  x1, x2, [x0, #8]  ; this->paddr = x1, this->size = x2
209f0:  cbz  x1, 0x20a24       ; ---> ASSERT
209f4:  cbz  x2, 0x20a6c       ; ---> ASSERT "size != 0" (line 40)
209f8:  adrp x8, 0x14e000
209fc:  ldrb w8, [x8, #4008]   ; MappedMemory::g_bUseStaticMapping (0x14efa8)
20a08:  cbz  w8, 0x20ab4       ; not static -> RTKit map at 0x20ab4
...
20a24:  bl   0xa56bc           ; scratch-register "I asserted" flag (§5)
20a28:  adr  x8, 0xbe76d       ; "paddr != 0"
20a30:  adr  x1, 0xbe748       ; "./AppleAVE2FW/utils/MappedMemory.cpp"
20a48:  mov  w2, #0x27         ; line 39
20a50:  bl   0x949d8           ; _bsp_assert_fail printf
20a60:  bl   0x22d8c
20a64/68: nop / b .            ; spin
```

**Confirmed** (instructions cited). So the assert's `paddr` is **argument 2
(`x1`) of `MappedMemory::MappedMemory`**, and the caller is what matters.

Strings, for re-checking: file offsets `0xc2748` / `0xc276d` in
`data/blobs/macos-13.5/ave_h13c.bin` → VAs `0xbe748` / `0xbe76d`.

There are 40 call sites of `0x20bc0` in the image. Exactly two lie on the
`AVC_INIT` path (§2): one is guarded against zero, the other is not.

---

## 2. The path from `Start_AVC` to the assert

Each step **confirmed** from the instruction cited.

| step | VA | what |
|---|---|---|
| `CFlowControllerBase::ProcessAvcInit` | `0xed94` | dispatcher entry for id 4 |
| history record `"Start AVC %d"`, **line 2340** | `0xee18` (`mov w4,#0x924`), `0xee28` (`bl 0x22d08`) | this is the last log line before the assert in the kmsg — everything below happens after it |
| `CreateClient(cmd+0x40, cmd+0x48, cid, cmd+0x18)` | `0xee2c`–`0xee40` | `ldr x1,[x21,#64]`, `ldr w2,[x21,#72]` |
| `CAVEPriorityQueue::CreateClient` | `0x17a58` (called `0x13d34`) | |
| &nbsp;&nbsp;→ `MappedMemory` **call site 1** | `0x17c5c` | **guarded**: `cbz x25, 0x17c20` at `0x17bc0`, where `x25` = `cmd+0x40`. If the host passes 0 the firmware allocates the client buffer itself. Our run passed `0xff200000`, so this site is not it. |
| `CFlowControllerBase::ProcessInitStage2` | `0x13dd8` (called `0xeea0`) | |
| &nbsp;&nbsp;→ virtual `[vt+136]` = `CFlowController::SetPipeClockGating` | `0x13e3c`–`0x13e54` → `0x3c8f8` | this is the `Config +0x48` consumer; §4 |
| … `CAVCController::Init` | `0x462e0` | |
| &nbsp;&nbsp;→ virtual `[vt+536]` | `blr` at `0x466f8`; slot at `__DATA` VA `0xed520` inside `__ZTV14CAVCController` (`0xed2f8`) → `0x5c9c8` | |
| `CAVCController::InitEncodingParameters(void*)` | `0x5c9c8` | |
| &nbsp;&nbsp;→ `MappedMemory` **call site 2** | `0x5df38` | **unguarded**. This is the assert. |

Site 2's only other sibling inside `InitEncodingParameters` is `0x5cb90`
(`iFwClientMemAddr`, `cmd+0x50`), which is guarded by `cbz x8, 0x5cc2c` at
`0x5cb6c` and asserts `pAvcInitCmd->iFwClientMemAddr != 0` on line 3396 of
`CAVCController_H13C.cpp` — a *different* assert string, and our run passed a
non-zero value there anyway.

### 2.1 Pointer arithmetic inside `InitEncodingParameters`

`x27` = arg 1 = a `sCAveInitCmdInternalParams` copy; `x20 = [x27+8]` is the
`AVE_VIDEO_PARAMS` pointer.

* `x20 == cmd + 0x60` — **confirmed** two ways: the SPS memcpy at `0x5ce68`–`90`
  reads `x20 + 0x10550` for `0x6AC` bytes, and docs [46](46-abi-13.5-commands-session.md) §9.1
  puts the SPS block at wire `0x105B0`/`0x6AC`; the PPS memcpy at `0x5ce94`–`ac`
  reads `x20 + 0x10BFC` for `0x184`, wire `0x10C5C`/`0x184`.
* `x23 = x20 + 0xF760` (`add x23, x20, x8` at `0x5cdd4`, `w8 = 0xf760` at
  `0x5cdd0`) ⇒ **`x23 = cmd + 0xF7C0`**. Cross-check: `NEED_LSB_PLANES` is
  `ldrb w8,[x23,#1469]` at `0x5d08c`, i.e. wire `0xF7C0 + 0x5BD = 0xFD7D`, which
  is exactly the offset docs/46 §9.2 records. **Confirmed.**

### 2.2 The unguarded call

```
5dd70:  cbz  w10, 0x5de3c     ; w10 = SPS bFWCreatesHeader (wire 0x10A54)
5dd74:  cbz  w8,  0x5ed4c     ; PPS flag must match, else 0xEE0005
5dd78..5ddb0:                 ; firmware writes SPS then PPS into a WriteBits
                              ; buffer (0x194a0 / 0x1997c)
5ddb4:  b    0x5de40
        --- both arms of the bFWCreatesHeader test converge here ---
5de40:  ldr  w8, [x19, #2740]  ; total header length in BITS - see below
5de44:  ldr  w2, [x23, #888]   ; = cmd + 0xFB38  (buffer length, u32)
5de48:  cmp  w2, w8, lsr #3
5de4c:  b.cs 0x5df28           ; length sufficient -> map it
5de50:  sub  w27, w27, #0x2    ; else 0xEE0005-2 = 0xEE0003 and return
5df28:  ldr  x20, [x23, #880]  ; = cmd + 0xFB30  (buffer address, u64)  <== paddr
5df2c:  add  x0, sp, #0xca0
5df30:  mov  w3, #0x0
5df34:  mov  x1, x20
5df38:  bl   0x20bc0           ; MappedMemory(obj, paddr = 0, size = 0, false)
                               ;   -> cbz x1 at 0x209f0 -> ASSERT line 39
5df44:  bl   0x20c00           ; MappedMemory::HwToTarget(paddr)
5df48:  ldr  w8, [x24, #1196]  ; SPS header length, bits
5df5c:  bl   0x55e8            ; memcpy(mapped, sps_bytes, bits>>3)
5df70:  ldr  w8, [x24, #1840]  ; PPS header length, bits
5df78:  bl   0x55e8            ; memcpy(mapped + sps_bytes, pps_bytes, bits>>3)
5df9c:  str  w8, [x19, #2740]  ; NOW store sps_bits + pps_bits
```

Note the ordering: `[x19+2740]` is **written at `0x5df9c`, after** the length
check reads it at `0x5de40`. A scan of the whole image for `#2740]` finds exactly
one store, `0x5df9c`. So on a controller's *first* init the compared length is
whatever the freshly-constructed `CAVCController` holds — zero — and
`cmp 0, 0` satisfies `b.cs`, so the zero address reaches the ctor.

That `[x19+2740] >> 3 == 0` on this run is **confirmed by the observed
behaviour**, not only inferred: had it been non-zero, the firmware would have
taken `0x5de50` and answered `INIT_DONE` with status `0xEE0003`; we received no
reply at all, only the assert.

**Both arms matter.** The buffer is required whether or not the firmware writes
the parameter sets itself — `bFWCreatesHeader = 1` (what our builder sends,
`driver/ave_cmd.c:374`/`386`) just means the firmware generates the bytes before
copying them out.

---

## 3. What the host must put there

### 3.1 The kext side names it

`AVE_CHM_SetFwBuf` (kext `0xfffffe0008eaed44`), called from
`AVE_CHM_MakeFwCmd_Start_AVC` at `0xfffffe0008ea99bc` with `x3` = the staged
`AVE_VIDEO_PARAMS` block (`chm+0x338`, copied into the wire command at `+0x60`
by `0xfffffe0008ea9b3c`; `add x24, x23, #0x60` at `0xfffffe0008ea9b2c`):

```
eaedf4:  ldr x0, [x25, #16]      ; the AVE_Surface for this buffer
eaedf8:  cbz x0, 0xfffffe0008eaee2c
eaedfc:  mov w8, #0xfad0
eaee00:  add x20, x22, x8        ; x22 = AVE_VIDEO_PARAMS  ->  VP + 0xFAD0
eaee0c:  bl  0xfffffe0008f35fa0  ; AVE_Surface::GetDARTAddr(surface, dartIdx, 0)
eaee10:  str x0, [x20]           ; VP + 0xFAD0 = IOVA
eaee18:  bl  0xfffffe0008f360f0  ; AVE_Surface::GetSize
eaee1c:  str w0, [x20, #8]       ; VP + 0xFAD8 = size
eaee20:  ldr x8, [x20]
eaee24:  cbz x8, 0xfffffe0008eaf4c8   ; -> log + return -1015
eaee28:  cbz w0, 0xfffffe0008eaf4c8
```

The failure log at `0xfffffe0008eaf4c8` prints the string at kext
`0xfffffe00071e9b3c`:
`"pVP->p_ParameterSetsBuffer != 0 && pVP->p_ParameterSetsBufferSize != 0"`,
`AVE_CHM_SetFwBuf` line 312, and returns **`-1015`** (`0xfffffe0008eaf71c`),
which makes `MakeFwCmd_Start_AVC` abort before sending (`cbz w0` at
`0xfffffe0008ea99c0`). **macOS never sends a `Start_AVC` with this field zero.**
All **confirmed**.

`VP + 0xFAD0` is wire `0x60 + 0xFAD0 = 0xFB30`, and `VP + 0xFAD8` is `0xFB38` —
the same pair the firmware reads as `[x23+880]` / `[x23+888]`. Two independent
readings agree.

The name `p_ParameterSetsBuffer` exists only in the 13.5 kext
(`grep -ao 'ParameterSets[A-Za-z]*'`: 13.5 kext has `ParameterSetsBuffer`,
`ParameterSetsBufferSize`; the 26.6.2 kext has only `bUpdateParameterSets`
inside an unrelated log). The 26.6.2 counterpart was **not located** — leave it
`AVE_OFF_NONE` in the 26.6.2 table.

### 3.2 Type, alignment and size

* **Type: a plain DART IOVA in the AVE DART domain** — the same
  `AVE_Surface::GetDARTAddr` the kext uses for the recon, coded and coded-header
  addresses our driver already fills from `dma_alloc_coherent`. **Confirmed**
  (same function, `0xfffffe0008f35fa0`, at `0xeaee0c` and in the coded/recon
  loops at `0xeaee70` / `0xeaeec0`).
* **Alignment: none required of the address itself.** `MappedMemory`'s RTKit
  path rounds the base down to 16 KiB (`and x0, x1, #~0x3fff` at `0x20ab8`) and
  the length up, then maps (`0xb724c` at `0x20aec`). Any
  `dma_alloc_coherent` result is fine.
* **Size: must be non-zero** (`cbz x2` at `0x209f4` → `MappedMemory.cpp:40`,
  `"size != 0"`), and on every init *after* the first it must be
  `>= (SPS bits + PPS bits)/8` of the previous init, else `0xEE0003`.
* **Size is also a real overflow bound.** The two `memcpy`s at `0x5df5c` and
  `0x5df78` are **not** bounded by the buffer length — the only length check is
  the stale one at `0x5de40`. A short buffer is a firmware-side DMA overrun into
  whatever follows it in the DART map. Allocate at least one page; an AVC
  SPS+PPS pair is well under 100 bytes.

**Recommendation: a dedicated 4 KiB `dma_alloc_coherent` buffer**, address at
`0xFB30`, `0x1000` at `0xFB38`.

---

## 4. `Config +0x48` is **not** the problem — candidate closed

`driver/ave_session.c` sends 0 there and warns about it. That warning can be
retired for 13.5.

`ProcessConfig` stores it: `ldr x10,[x23,#72]` (`0xe554`) → `str x10,[x19,#1368]`
(`0xe578`). `ProcessInitStage2` loads it and calls `[vt+136]`:

```
13e38: ldr x8, [x20]           ; vptr
13e3c: ldr x1, [x20, #1368]    ; = Config +0x48
13e44: ldr x8, [x8, #136]
13e4c: mov w2, #0x0
13e54: blr x8
```

`vt+136` resolves through `__ZTV15CFlowController` (`__DATA` VA `0xecfc0`, slot
at `0xecfc0 + 16 + 136 = 0xed058`) to
**`CFlowController::SetPipeClockGating(unsigned long long, bool)` at `0x3c8f8`**
— *not* a `MappedMemory` user at all:

```
3c908: ldrb w8, [x0, #1359]
3c90c: cbz  w8, 0x3ca44        ; gate: if this[1359] == 0, return immediately
3c910: cbz  x1, 0x3c96c        ; ASSERT ./AppleAVE2FW/kf_controller/H9/
                               ; CFlowController.cpp, 120: "pmgrAddr != 0"
```

Three independent reasons this is not our failure, all **confirmed**:

1. The assert text would be `CFlowController.cpp, 120: pmgrAddr != 0`, not
   `MappedMemory.cpp, 39`.
2. The argument is never dereferenced or mapped; it is a PMGR register address,
   used only by the clock-gating RMW at `0x3c9b8`ff.
3. The gate byte `this[1359]` is never written anywhere in the image — a scan
   of the full `__TEXT` disassembly for `#1359]` finds exactly one instruction,
   the `ldrb` at `0x3c908`. The `CFlowControllerBase` ctor zeroes its
   neighbours (`strb wzr,[x19,#1360]` `0xb670`, `str xzr,[x19,#1368]` `0xb688`),
   so `SetPipeClockGating` returns at `0x3c90c` without touching `x1`
   (**inferred** that the allocation is zeroed; the assert string alone already
   settles the question).

For completeness, the host side: `AVE_Reg::GetDARTAddr(_E_AVE_RegType, unsigned)`
(kext `0xfffffe0008f2fb44`) is `this->regs[type]` (a 5-entry array at `this+136`)
→ `[obj+24]` → `+ offset`; with type 3, offset 0, it is the device-visible base
of AVE MMIO bank 3 as that bank's mapping object records it. Since 13.5's only
consumer is gated off, **sending 0 is correct for 13.5**. Whether 26.6.2 needs a
real value is out of scope here.

---

## 5. The two `SCRATCH_REG32` lines

They are part of the assert mechanism and carry no information about the address.
`_bsp_assert_fail`'s callers all start with `bl 0xa56bc` (ours: `0x20a24`):

```
a56d0: adrp x19, 0x21a000 / add x19, x19, #0x7b8     ; base pointer table 0x21a7b8
a56dc: mov  w21, #0x34 / movk w21, #0x105, lsl #16   ; offset 0x1050034
a56ec: add  x8, x8, x21
a56f0: adr  x0, 0xce17b   ; "SCRATCH_REG32_RD: addr %016llx value %08x\n"
a56f4: ldr  w22, [x8]
a570c: orr  w9, w22, #0x40000000                     ; set bit 30
a5710: adr  x0, 0xce1a6   ; "SCRATCH_REG32_WR: addr %016llx value %08x\n"
a571c: str  w9, [x8]
```

So: read `mmio_base + 0x1050034`, OR in `0x40000000`, write back, logging both.
`0xfffffffff5050034` is that register as the coprocessor addresses it, i.e.
`[0x21a7b8] = 0xfffffffff4000000`. The pre-existing value `1` is some other
firmware-set bit. A sibling routine at `0xa5740` sets bit 31 instead (used by a
different fatal path). **Confirmed.** Bit 30 of that scratch word is simply
"the AVE firmware asserted" — a useful thing for the driver to poll, but not
related to the missing address.

---

## 6. Recommended change (description, not an edit)

`driver/` was not modified (this document is analysis only).

### `driver/ave_abi.h`

1. `struct ave_start_avc_layout`: add

   ```c
   u32 param_sets_addr;   /* u64 IOVA: firmware writes SPS+PPS bytes here */
   u32 param_sets_size;   /* u32 bytes */
   ```

2. `ave_cmd_abi_13_5.start_avc`: `.param_sets_addr = 0xfb30`,
   `.param_sets_size = 0xfb38`, with the citations
   `/* p_ParameterSetsBuffer: kext str x0,[x20] 0xfffffe0008eaee10 (VP+0xFAD0),
      size str w0,[x20,#8] 0xfffffe0008eaee1c; fw ldr x20,[x23,#880] 0x5df28,
      ldr w2,[x23,#888] 0x5de44, x23 = cmd+0xF7C0 (0x5cdd4) */`

3. `ave_cmd_abi_26_6.start_avc`: both `AVE_OFF_NONE` (counterpart not located).

### `driver/ave_cmd.h`

`struct ave_avc_session`: add `u64 param_sets_addr; u32 param_sets_size;`,
documented as "13.5: mandatory, non-zero; the firmware memcpys the generated
SPS+PPS into it with **no bound check** — size it at least a page".

### `driver/ave_cmd.c` (`ave_cmd_build_start_avc`)

* Validate alongside the existing buffer checks (around line 286):
  `if (l->param_sets_addr != AVE_OFF_NONE && (!s->param_sets_addr || !s->param_sets_size)) return -EINVAL;`
* Emit:
  `wr64(&w, l->param_sets_addr, s->param_sets_addr);`
  `wr32(&w, l->param_sets_size, s->param_sets_size);`
  guarded on `!= AVE_OFF_NONE`, next to the coded/recon table writes.

### `driver/ave_session.c` (`ave_session_start_avc`)

* Add one more `ave_sess_dma_alloc(bufs, AVE_SESS_PARAMSETS_SIZE, &ps_iova)`
  with `#define AVE_SESS_PARAMSETS_SIZE 0x1000`. This makes 6 DMA buffers in
  the Start path plus 1 for Config = 7, still within `AVE_SESS_MAX_DMA` (8).
* `s.param_sets_addr = ps_iova; s.param_sets_size = AVE_SESS_PARAMSETS_SIZE;`
* Add `paramsets %pad/%#x` to the existing buffer log line.
* Drop (or downgrade to `dev_dbg`) the
  `"Config reg-DART addr is 0 (unknown); pass session_reg_dart=…"` warning and
  the `session_reg_dart` module parameter's "the firmware faults in
  ProcessInitStage2" claim — §4 shows it does not. Keep the parameter itself if
  26.6.2 support may need it.

---

## 7. What this analysis does not settle

* **Whether `param_sets_size` has an upper bound.** No compare against a
  constant was found; only the stale-length `b.cs` at `0x5de4c`. **Unknown**;
  4 KiB is safe by inspection of the copy lengths.
* **The 26.6.2 equivalent field.** Not located (§3.1). **Unknown.**
* **Whether the firmware wants the parameter sets read back by the host.**
  The buffer is write-only from the firmware's side on this path; whether the
  host is expected to consume it (for `avcC` / extradata) is **inferred** from
  the name, not read.
* **What comes next.** Fixing `0xFB30`/`0xFB38` gets past this assert; the next
  asserts on the same function, in source order, are (`CAVCController_H13C.cpp`)
  line 3745 `EncCommParams.cycleBuffer0` (`0x5de58`), line 3746
  `EncCommParams.cycleBuffer1` (`0x5e24c`), `clientBuffer != NULL` on lines
  3901/3763/3915 (`0x5e294`, `0x5e2dc`, `0x5e324`) and
  `clientBufferAvailableSize >= …` on lines 3902/3765/3916 (`0x5e36c`,
  `0x5e3b4`, `0x5e3fc`). The latter
  group is about `cmd+0x40`'s size, which our run already sets to the firmware's
  own `GetClientBufferSize()` (`0xb4000`); the `cycleBuffer` pair is about
  further sub-allocations and may need its own reading. Also possible is
  `MappedMemory.cpp:57` `"status == RTK_ST_OK && aligned_vaddr"` (`0x20b50`) if
  the IOVA we supply cannot be mapped by the coprocessor's own MMU.
* **No hardware experiment is proposed beyond the obvious one**: re-run the
  existing `session_selftest=1` path with the `0xFB30`/`0xFB38` pair filled in.
  That is a driver change, not a new probe, and it is the operator's call
  ([AGENTS.md](../AGENTS.md)).
