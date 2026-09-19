# What the 13.5 kext writes into `AVC_INIT` (id 4) and `AVC_ENCODE` (id 7), field by field, against `driver/ave_cmd.c`

An exhaustive host-side pass over the two commands the bring-up actually sends,
done because the last four fixes were all the same shape: *a command field
Apple's kext fills and our driver leaves zero.* Finding them one at a time costs
a reboot each; this enumerates them in one pass.

Static analysis only. Nothing here was run on hardware.

Conventions as [46](46-abi-13.5-commands-session.md) / [61](61-mb3083-stall.md):

- Kext VAs are 13.5 kernelcache `__TEXT_EXEC` VAs; a bare `0xeaXXXX` means
  `0xfffffe0008eaXXXX`.
- Firmware VAs are 13.5 image VAs (file offset = VA + `0x4000`).
- **wire** = byte offset in the command as it goes out on IPC channel 1.
- **VP** = offset inside `AVE_VIDEO_PARAMS`, which is the `AVC_INIT` command at
  `+0x60`, so **wire = VP + 0x60**.
- Labels per [00](00-methodology.md): **[C]** read from an instruction (VA
  cited), **[I]** inferred (chain stated), **[U]** unknown.

Reproduce with `AVE_MACOS=13.5 python3 tools/disas.py --kext|--fw --addr VA -n LEN`.

---

## 0. Verdict on the entropy/SEB size question

**The guess was right, and it is no longer a guess.** The size table is at
**Start_AVC wire `0xFA30 + 0x10·i + 4·j`, u32**, exactly where
`driver/ave_abi.h` put it by inference. It is now **confirmed from both sides**:

| side | what | VA |
|---|---|---|
| kext writes it | `AVE_CHM_SetFwBuf`: `str w0,[x8,#512]` with `x8 = (VP+0xF7D0) + 4·(j+4i)`, i.e. **VP + `0xF9D0` + `0x10·i` + `4·j`**, the value being `AVE_Surface::GetSize()` of the same entropy surface whose IOVA goes to VP+`0xF7D0`+`0x20·i`+`8·j` | `0xeaf2ec` (address at `0xeaf2dc`, loop `0xeaf2ac`–`0xeaf31c`) | 
| the struct has the slot | `AVE_Client_InitFwBuf` zeroes `VP+0xF7D0` for `0x200` bytes (= u64[16][4]) and **`VP+0xF9D0` for `0x100` bytes (= u32[16][4])** back to back | `0xec8fe4`–`0xec8ff4` and `0xec8ff8`–`0xec9008` |
| both must be non-zero | the kext bails out of `SetFwBuf` (error path `0xeafff4`) if either the IOVA or the size is 0 | `0xeaf2f0`–`0xeaf2fc` |
| **firmware consumes it** | `CAVCController::InitEncodingParameters` copies **`VP+0xF9D0` → `ctrl+0x10C0`** and `VP+0xF7D0` → `ctrl+0xEC0`, four columns per row, `4·sSVEMap.iNum` rows | fw `0x5d734`–`0x5d870` |
| `setPipe` then programs it | size ← `ctrl+0x10C0 + 0x10·ch + 4·j` → channel `+0x10`; address ← `ctrl+0xEC0 + 0x20·ch + 8·j` → channel `+0x0C`; no transformation | fw `0x5595c`–`0x55970` |

All **[C]**.

### 0.1 docs/61 §2.4 was wrong: `ctrl+0x10C0` does have a writer

[61](61-mb3083-stall.md) §2.4 concluded "**`ctrl+0x10C0` has no writer anywhere
in the image**", with the Trap-3 caveat attached. The caveat was the right one.
The writer is in `InitEncodingParameters` and it is invisible to an
immediate-offset scan because it uses **negative `stur` offsets and a
post-indexed `str` off two pre-biased bases**:

```
5d734  mov w8,#0xf7d0 ; add x28,x20,x8      ; x28 = VP + 0xF7D0   (entropy addresses)
5d73c  mov w8,#0x10cc ; add x25,x19,x8      ; x25 = ctrl + 0x10CC (sizes, pre-biased +0xC)
5d744  mov w8,#0xf9d0 ; add x24,x20,x8      ; x24 = VP + 0xF9D0   (entropy sizes)
5d754  add x26,x19,#0xed8                   ; x26 = ctrl + 0xED8  (addrs, pre-biased +0x18)
        ; col 0
5d7ac  ldr  x8,[x28]      ; 5d7b8 stur x8,[x26,#-24]   -> ctrl+0xEC0 + 0x20*row
5d7bc  ldr  w9,[x24]      ; 5d7c0 stur w9,[x25,#-12]   -> ctrl+0x10C0 + 0x10*row
        ; col 1 / 2 / 3, each gated on a replicate flag (see 0.3)
5d7dc  ldr  x8,[x28,#8]   ; 5d7e0 stur x8,[x26,#-16]   -> +0xEC8   / 5d7e4 ldr w9,[x24,#4]  -> +0x10C4
5d820  ldr  x8,[x28,#16]  ; 5d824 stur x8,[x26,#-8]    -> +0xED0   / 5d828 ldr w9,[x24,#8]  -> +0x10C8
5d760  ldr  x8,[x28,#24]  ; 5d764 str  x8,[x26]        -> +0xED8   / 5d768 ldr w9,[x24,#12] -> +0x10CC
5d774  str  w9,[x25],#16                    ; x25 += 0x10
5d78c/90/98/9c  row++ ; x28 += 0x20 ; x24 += 0x10 ; x26 += 0x20
5d794  ldr w8,[[sp,#112]] ; lsl w8,#2 ; cmp row,w8 ; b.cs exit   ; rows = 4 * sSVEMap.iNum
```

**[C]** for every line. Row stride `0x20`/`0x10` and column stride `8`/`4`
match `setPipe`'s read arithmetic exactly, which is the cross-check.

### 0.2 Why F14 saw the address arrive and the size stay zero

Two different paths, and only one of them was being fed:

- The **address** reaches `ctrl+0xEC0` twice: once at Start_AVC by the loop
  above, and again **every frame** by `PipePrepareParam` copying PICMGMT
  `+0xA00` (fw `0x48800`), which `setRefPointers` has refilled from the
  Start_AVC DPB record ([61](61-mb3083-stall.md) §10). F14 wrote wire `0xF830`,
  so both paths carried it. **[C]**
- The **size** reaches `ctrl+0x10C0` **only** from the Start_AVC loop above.
  There is no per-frame counterpart: `AVE_CHM_SetDataInfo_FwBuf` writes
  PICMGMT `+0xA00 + 0x20i + 8j` addresses only, 16×4, with **no size array**
  (`0xeb0cb8`–`0xeb0d0c`). **[C]**

F14 ran a driver build that did not yet write wire `0xFA30`: its log line is
`"Start_AVC: entropy 4 x 4 at wire 0xf830, slot 0 0xfe000000"`, while the
current `ave_session.c:1055` format also prints `", size %#x at wire %#x"`
(`results/f14-1789852532.kmsg` line 975 vs `driver/ave_session.c:1053`-`1059`).
So **the size table has never actually been on the wire**; there is no negative
result to explain. **[C]**

### 0.3 The gate, and which column matters

Two conditions decide what the loop does. One is host-supplied, one is not.

**The gate is host-supplied.** `InitEncodingParameters` is reached with
`x20 = cmd + 0x60` (so a VP offset and a wire offset differ by `0x60`
throughout, the convention [47](47-abi-13.5-frame-rc-surfaces.md) §2.3 had to
infer — here it is read directly at fw `0x463e8`, `add x10, x8, #0x60`, where
`x8` is the command). The loop's gate and trip count both come from
`ctrl+0x1240`, which is `memcpy(ctrl+0x1240, cmd+0x10DE8, 0x24)` — the
`sSVEMap` block (fw `0x5ca80`–`0x5cad8`, `x1 = x20 + 0x10D88` = wire
`0x10DE8`):

| what | where | our value | effect |
|---|---|---|---|
| `sSVEMap.iNum`, wire `0x10DE8` u32 → `ctrl[4672]` | gate `tst w9,#0x3fffffff ; b.eq 0x5d874` (fw `0x5d4cc`, `0x5d72c`) | **1** (`ave_abi.h` `.sve_num = 0x10de8`, written by `ave_cmd.c:390`) | non-zero ⇒ **the copy runs**; rows copied = `4·iNum` = **4**, which is exactly `num_encoder_addr_entropy` (fw `0x5d074`). **Zero would skip the whole copy**, leaving the sizes at 0 while the per-frame path still delivers the addresses — i.e. exactly the F14 symptom |

**[C].** The same word also selects the single-core arm:
`ctrl[4664] = (iNum <= 1) ? 1 : 2` (fw `0x5ca88`–`0x5caa0`), and
`ctrl[4664] != 2` is what makes `num_encoder_addr_entropy = 4`.

**The column-replicate flag is *not* host data.** The `ldrb w8,[x8,#34]`
at fw `0x5d7d0` / `0x5d814` / `0x5d858` reads **byte 34 of the descriptor
`InitEncodingParameters` is called with**, not of the command. That descriptor
is a stack record the caller builds at fw `0x463cc`–`0x46410`:

```
463cc  ldp x8,x9,[x21]       ; x8 = the AvcInit command
463e8  add x10, x8, #0x60    ; VP
463ec  stp x10, x9, [sp,#24] ; desc+0x08 = VP  (this is InitEncodingParameters' x20)
463e0  ldrb w11,[x21,#18]
46408  strb w11,[sp,#50]     ; desc+0x22 = byte 34   <- the replicate flag
46400  str  w8,[sp,#64]      ; desc+0x30 = cmd+0x58 (FwClientMem size)
463fc  ldr  x9,[x8,#80]      ; 4640c str x9,[sp,#56] ; desc+0x28 = cmd+0x50
46410  blr  [vtable+488]     ; -> 0x5c9c8 InitEncodingParameters, x1 = sp+0x10
```

**[C]** for the construction; the origin of `x21[18]` is **[U]**.

Practical consequence, and it is the safe one: **we cannot choose which arm
runs, so fill all four columns of rows 0–3** — which is what
`ave_cmd.c:438`–`451` already does. If the flag is 0 the firmware replicates
column 0 (so column 0 must be right); if it is 1 each column is taken
literally (so all four must be right). Both hold if all four are filled with
valid, equal-or-distinct buffers. This is the Start-time twin of the
`PipePrepareParam` rule [61](61-mb3083-stall.md) §2.3 found per frame.

### 0.4 What to write

For a 1280x720 fixed-QP Baseline CAVLC I-frame, one SVE core:

| | |
|---|---|
| wire offset | **`0xFA30 + 0x10·i + 4·j`**, `i = 0..3`, `j = 0..3` |
| width | **u32** little-endian, a **byte count**, not a page count |
| value | the allocated size of the buffer whose IOVA is at wire `0xF830 + 0x20i + 8j` — the kext writes `AVE_Surface::GetSize()` of that exact surface (`0xeaf2e4`). For the driver's own arena that is `ave_session_entropy_size(1280,720)` = `align_down(64·1280+960,1024)·max(8,ceil(45/4))` = `81920·12` = **983040** (`0xF0000`) |
| rows | 0..3 (`4·iNum`); rows 4..15 are never read by `setPipe` and stay 0 |
| columns | **all four** — the host cannot choose which arm the firmware takes (§0.3) |

`driver/ave_abi.h` already has `.entropy_size_set = 0xfa30`,
`.entropy_size_stride_i = 0x10`, `.entropy_size_stride_j = 0x04`. **Those three
constants are correct; change the comment from INFERRED to confirmed and cite
kext `0xeaf2ec` / fw `0x5d768`.** No code change is needed beyond making sure
`session_entropy_size` is on.

### 0.5 If it still stalls

The "zero-length ring" reading is now *supported*, not merely plausible: the
channel's `+0x10` word is a host-supplied byte count that Apple always fills and
we never did. If a run with `0xFA30` populated still raises
`Cveseb buffer write full!`, the next things to check, in order:

1. `0x40D11303D0 + 0x40k` at the timeout — if the size word is now non-zero and
   the stall persists, the SEB is not drained by these channels and
   [61](61-mb3083-stall.md) §7.3 becomes live.
2. Confirm the wire: dump the built Start_AVC at `0xF7D0..0xFB40` in userspace
   the way the coordinator dumped the Process command for
   [61](61-mb3083-stall.md) §6.1, and confirm 16 IOVAs and 16 sizes are present
   at the strides above.
3. The `4·iNum` row count: `sSVEMap.iNum` at wire `0x10DE8` must be non-zero or
   the whole Start-time copy is skipped and *both* tables come from the
   per-frame path only — which reproduces exactly the F14 symptom. The driver
   writes 1 (`ave_cmd.c:390`), so this should already hold; worth one `dev_info`
   of the built bytes to be sure.

---

## 1. `AVC_INIT` (id 4, `0x10E10`): what the kext writes

### 1.1 The builder's own stores

`AVE_CHM_MakeFwCmd_Start_AVC(chm, count, flags, timeout, cmd)` `0xea9820`.
`x23` = cmd, `x27` = client = `[chm+16]`, `x21` = the client's CHM index.

| wire | width | contents | kext VA | source | driver |
|---:|---:|---|---|---|---|
| — | `0x10E10` | `memset(cmd, 0, 0x10E10)` | `0xea98c4`–`0xea9904` | — | yes |
| `0x00` | u16 | id = 4 | `0xea9938`–`3c` | literal | yes |
| `0x02` | u16 | 0 | `0xea9908` | — | yes |
| `0x08` | u64 | command count | `0xea990c` | caller | yes |
| `0x10` | u32 | client id | `0xea9918` | `client[80]` | yes |
| `0x14` | u32 | unknown | `0xea9918` | `client[212]` | **no** (left 0) |
| `0x18` | u32 | codec, 0 = AVC | `0xea9920` | `client[220]` | yes |
| `0x1C`/`0x20` | u32/u32 | slot 6 / priority 200 | `0xea992c` | literal `0x722f220` | yes |
| `0x28` | 16 B | `_S_AVE_TimeOut` | `0xea9934` | caller | yes |
| `0x40`/`0x48` | u64/u32 | FwClient IOVA / size | `0xea995c`, `0xea9968` | `SurfaceSet[0x1068 + 8·idx]`, `GetDARTAddr` / `GetSize` | yes |
| `0x50`/`0x58` | u64/u32 | FwClientMem IOVA / size, **only if the surface exists** (`cbz` `0xea9970`) | `0xea9980`, `0xea998c` | `client[0xD04F0]` | yes |
| `0x60` | `0xFED0` | `AVE_VIDEO_PARAMS` — see §1.2/§1.3 | `0xea9b3c` | staged copy at `chm+0x338` | partly |
| `0xFF30` | `0x680` | `AVEFWRCSettings` verbatim | `0xea9b54` | `client+0xD0688` | field-by-field |
| `0x105B0` | `0x6AC` | SPS params | `0xea9b6c` | `*(client+0xE2260) + 0xC` | field-by-field |
| `0x10C5C` | `0x184` | PPS params | `0xea9b80` | `*(client+0xE2268)` | field-by-field |
| `0x10DE0` | u32 | unknown | `0xea9b98` | `client[0xDE30]` | **no** |
| `0x10DE4` | u8 | `bTranscodeOverlap` (fw `0x13f78`) | `0xea9ba0` | `client[0xD0598]` | **no** |
| `0x10DE8` | `0x24` | `sSVEMap`, copied whole | `0xea9bb4`–`c0` | `client+0xCF218` | only the u32 at `+0` |
| `0x10E0C` | u32 | EU/SVE map index | `0xea9bc4` | the CHM index `w21` | **no** (0 is a valid index) |
| — | — | after building, the kext memcpys `cmd+0x60 .. +0x10E0F` (`0x10DB0` bytes) into the **InitParamsCopy** surface `client[0xD04F8 + 8·idx]` | `0xea9bf0` | — | n/a (host bookkeeping) |

All **[C]**. The `0x10DE0` and `0x10DE4` values and the meaning of `0x14` remain
**[U]** (unchanged from [46](46-abi-13.5-commands-session.md) §12).

### 1.2 `AVE_VIDEO_PARAMS` is a pass-through from user space

This is the structural fact that decides how to read the rest of the command:

```
AVE_Client_Config(client, bool, AVE_SessionSettings_UserKernel_Data *pInfo):
  0xecc1bc  memcpy(client + 0xD0E38, pInfo + 0x7B0, 0xFED0)   <- the whole VIDEO_PARAMS
  0xecc1d8  memcpy(client + 0xE0D08, pInfo + 0x10680, 0x3A0)
  0xecc1ec  memcpy(client + 0xD0688, pInfo + 0,       0x680)  <- AVEFWRCSettings
  0xecc200  memcpy(client + 0xD0D08, pInfo + 0x680,   0x128)
  0xecc224  AVE_Client_InitFwBuf(client + 0xD0E38)            <- re-zero the buffer tables
  (codec 0) memcpy(client + 0xE10AC, pInfo + 0x10A20, 0x6AC)  <- SPS
            memcpy(client + 0xE1758, pInfo + 0x110CC, 0x184)  <- PPS
            memcpy(client + 0xE18DC, pInfo + 0x11250, 0x984)
```

**[C]**. So the kext **does not synthesise** the scalar `VIDEO_PARAMS` fields:
they arrive from the user-space AVE library and are passed through byte for
byte. `MakeFwCmd_Start_AVC` then copies that template to `chm+0x338`
(`0xea99a4`), lets `AVE_CHM_SetFwBuf` patch the buffer tables into it
(`0xea99bc`), and copies the result to wire `0x60` (`0xea9b3c`).

Consequence for this exercise: for the scalar half of `VIDEO_PARAMS` there is
no "what the kext writes" to read — only "what the firmware reads and we leave
zero" (§1.4). For the buffer half, `AVE_CHM_SetFwBuf` *is* the answer (§1.3).

### 1.3 `AVE_CHM_SetFwBuf` — every buffer table it writes into `VIDEO_PARAMS`

`AVE_CHM_SetFwBuf(chm, SurfaceSet, SurfaceInfoSet, AVE_VIDEO_PARAMS*)`
`0xeaed44`; `x22` = VP, `x25`/`[sp,#112]` = SurfaceSet. Every entry is
`AVE_Surface::GetDARTAddr` (`0xf35fa0`) except where noted, and every entry is
skipped when the surface pointer is null.

The **extent** column is corroborated independently by
`AVE_Client_InitFwBuf` (`0xec8e98`), which memsets exactly these ranges — a
second reading of the same layout, and the control on the decode.

| VP | **wire** | shape | kext writer | InitFwBuf zero | surface kind | driver writes? |
|---:|---:|---|---|---|---|---|
| `0x28` | `0x88` | 2 sets × 17 × 16 B `{MSB,LSB}` (`AVE_CHM_GetFwDPBBuf`) | `0xeaef38` | `+0x28`, `0x220` | Recon / DPB | **set 0 only**, `n_recon` entries |
| `0x248` | `0x2A8` | 2 × 2 × 17 u64 (two arms, see note) | `0xeaf004` | `+0x248`, `0x110` | LowResRef | set 0 only |
| `0x358` | `0x3B8` | 2 × 2 × 8 u64 | `0xeaf094` | `+0x358`, `0x80` | LowResResult **[I]** | **no** |
| `0x3D8` | `0x438` | 2 × 2 × 8 u64 | `0xeaf124` | `+0x3D8`, `0x80` | LowResRCResult **[I]** | **no** |
| `0x458` | `0x4B8` | 20 × u64 | `0xeaee74` | `+0x458`, `0xA0` | CodedData addr | yes |
| `0x4F8` | `0x558` | 20 × u32 | `0xeaee84` | `+0x4F8`, `0x50` | CodedData size | yes |
| `0x548` | `0x5A8` | 2 × u64 | `0xeaf280` | `+0x548`, `0x10` | TranscodedData addr **[I]** | **no** |
| `0x558` | `0x5B8` | u32 (one size for the pair) | `0xeaf28c` | `+0x558`, 4 | TranscodedData size **[I]** | **no** |
| `0x560` | `0x5C0` | 20 × u64 | `0xeaeec4` | `+0x560`, `0xA0` | CodedHeader addr | yes |
| `0x600` | `0x660` | 20 × u32 | `0xeaeed4` | `+0x600`, `0x50` | CodedHeader size | yes |
| `0xF650` | `0xF6B0` | 2 sets × 17 u64 (set stride `0x88`) | `0xeaef94` | `+0xF650`, `0x110` | Colocated | set 0 only |
| `0xF760` | `0xF7C0` | 2 × u64 | `0xeaf350` | `+0xF760`, `0x10` | CrcQPMod **[I]** | **no** |
| `0xF770` | `0xF7D0` | 4 × u64 | `0xeaf188` | `+0xF770`, `0x20` | SrcNeighborInfo | yes |
| `0xF790` | `0xF7F0` | 4 × u64 | `0xeaf1c4` | `+0xF790`, `0x20` | SrcNeighborPixel | yes |
| `0xF7B0` | `0xF810` | 4 × u64 | `0xeaf200` | `+0xF7B0`, `0x20` | SrcNeighborData | yes |
| **`0xF7D0`** | **`0xF830`** | **16 × 4 u64**, `[i][j]` at `+0x20i+8j` | `0xeaf2dc` | `+0xF7D0`, `0x200` | **EntropyCoding addr** | yes (since F14) |
| **`0xF9D0`** | **`0xFA30`** | **16 × 4 u32**, `[i][j]` at `+0x10i+4j` | `0xeaf2ec` | `+0xF9D0`, `0x100` | **EntropyCoding size** | new, untested |
| `0xFAD0` | `0xFB30` | u64 + u32 | `0xeaee10`, `0xeaee1c` | `str xzr/wzr` | ParameterSet | yes |
| `0xFAE0` | `0xFB40` | 17 × u64 | `0xeaeff8` | — | LowResRef, second arm **[U]** | **no** |
| `0xFB08` | `0xFB68` | 8 × u64 | `0xeaf088` | — | LowResResult, second arm **[U]** | **no** |
| `0xFB88` | `0xFBE8` | 8 × u64 | `0xeaf118` | — | LowResRCResult, second arm **[U]** | **no** |
| `0xFC08` | `0xFC68` | 6 × 16 B (`AVE_CHM_GetMCTFOutBuf`) | `0xeaf3f4` | — | MCTFOutput **[C]** (helper name) | **no** |
| `0xFC68` | `0xFCC8` | 2 × u64 | `0xeaf394` | `+0xFC68`, `0x10` | MBInputCtrl **[I]** | **no** |
| `0xFE70` | `0xFED0` | 4 × u64 | `0xeaf23c` | `+0xFE70`, `0x20` | SrcNeighborFwData | yes (`src_nbr_set[3]`) |

Offsets, shapes and writer VAs: **[C]**. Surface-kind names marked **[I]** come
from matching the SurfaceSet source offsets to the `_S_AVE_SurfaceInfoSet`
member order in [47](47-abi-13.5-frame-rc-surfaces.md) §4 — the two
anchors that make that ordering solid are `EntropyCoding` (source
`SurfaceSet+0xE58`, a 4×16 u64 array = `0x200`, so the two-entry array at
`+0xE48` immediately before it is TranscodedData and the two-entry array at
`+0x1058` immediately after it is CrcQPMod, with FwClient at `+0x1068` as
independently proved by `0xea994c`) and `MCTFOutput` (named by the helper).

Note on the "second arm": the LowRes trio each have an outer loop over a flag
`x23 ∈ {0,1}`; arm 0 writes the low table, arm 1 writes a second table up in
the `0xFAE0`/`0xFB08`/`0xFB88` region. The arm-1 destinations are not advanced
per set, and `0xFAE0 + 17·8` overruns `0xFB08`. Either a latent Apple bug or
these counts are never simultaneously large. **[U]**; not chased, because
[47](47-abi-13.5-frame-rc-surfaces.md) §4 puts these kinds at count 0 or inert
for our configuration.

### 1.4 `VIDEO_PARAMS` scalars the firmware reads and we leave zero

Not "the kext writes them" (§1.2 — user space does), but the same actionable
class. Every `[x20, #imm]` load in `InitEncodingParameters`
(`0x5c9c8`..`0x5e000`, `x20` = VP, anchored by `ldr x8,[x20,#1112]` = CodedData
addr[0] at `0x5d4c8`) with `imm < 0x28`:

| VP | wire | width | firmware use | VA | our value |
|---:|---:|---|---|---|---|
| `0x00`/`0x04` | `0x60`/`0x64` | u32/u32 | width / height | `0x5ced0` | written |
| `0x08` | `0x68` | u32 bitfield | bits 0..4 and 27 become six encoder flag bytes at `params+17..22` | `0x5cf70`–`0x5cfb8` | **0** |
| `0x0C`/`0x0D` | `0x6C`/`0x6D` | u8/u8 | copied to `params+35/36`; **both non-zero trips an assert** (`0x5ca38`) | `0x5ca28`, `0x5cf60` | **0** (safe) |
| `0x0E` | `0x6E` | u8 | `params+42` | `0x5d1dc` | **0** |
| `0x0F` | `0x6F` | u8 | `params+38` | `0x5d128` | **0** |
| `0x10` | `0x70` | u32 | if non-zero, `params+15 = (v < 0x11)`; zero skips | `0x5d3c0` | **0** (skips) |
| `0x14` | `0x74` | u32 | stored to `ctrl+0x2C1F8` | `0x5cf20` | **0** |
| `0x18` | `0x78` | u32 | into `sCRCInitParams+0x20` and an RC helper arg (`0x3cc3c`) | `0x5dab4`, `0x5dc38`, `0x5dc98` | **0** |
| `0x1D` | `0x7D` | u8 | `sCRCInitParams+0x24`, same helper | `0x5cf94`, `0x5da6c`, `0x5dca0` | **0** |
| `0x20` | `0x80` | u32 | `sCRCInitParams+0x38` | `0x5da64`, `0x5dc3c` | **0** |

**[C]** for the loads; the field *names* are **[U]**. None of them is on the
SEB path — `0x18`/`0x1D`/`0x20` are rate-control inputs that a FIXQP session
ignores, and `0x0C`/`0x0D`/`0x10` are safe at zero by construction. Recorded so
the next "we leave it zero" hunt does not have to redo the scan.

---

## 2. `AVC_ENCODE` (id 7, `0x1940`): what the kext writes

`AVE_CHM_MakeFwCmd_Process_AVC(chm, count, flags, timeout, cmd, slot, PICMGMT)`
`0xeac7b4`. The command is only three things: a header, a `0x984` object, and
the caller's PICMGMT.

| wire | len | contents | kext VA | driver |
|---:|---:|---|---|---|
| — | `0x1940` | `memset` | `0xeac878` | yes |
| `0x00` | u16 | id 7 | `0xeac998` | yes |
| `0x02`/`0x08`/`0x10`/`0x14`/`0x18`/`0x1C` | | 0 / count / cid / `client[212]` / codec / slot (`slot <= 0x28`, `0xeac860`) | `0xeac880`–`98` | all but `0x14` |
| `0x20` | u32 | priority: 200 if bit 1 of arg 3, else `client[228]` | `0xeac984`–`88` | yes (200) |
| `0x28` | 16 B | timeout | `0xeac990` | yes |
| **`0x40`** | **`0x984`** | **`memcpy(cmd+0x40, *(u64*)(client+0xE2270), 0x984)`** — a slice-map / `AVC_Slice` object the client keeps a pointer to; `AVE_Client_Config` seeds a same-sized block from `pInfo+0x11250` (`0xecc268`–`0xecc27c`, `bl` at `0xecc35c`) and `AVE_Client_GenerateSlicesMap` (`0xec1284`) maintains it | `0xeac9a4`–`b8` | **no — 2436 bytes of zero** |
| `0x9C4` | 4 | zero | — | yes |
| `0x9C8` | `0xF68` | `AVE_PICMGMT_PARAMS`, with `[0] = 0xF68` | `0xeac99c`, `0xeac9bc`–`c8` | yes |
| `0x1930` | `0x10` | not written by the builder either | — | matched |

**[C]**. No firmware read of `cmd+0x40` was located on the pipe path:
`ProcessAvcEncode` memmoves the whole `0x1940` (`0xf050`) and the only offsets
its callees take from the pooled copy are `+0x1670`/`+0x1674` (= PICMGMT
`+0xCA8`/`+0xCAC`, `0xf094`/`0xf098`) and PICMGMT at `+0x9C8` (`0x145c8`).
Whether anything reads `+0x40` is **[U]**; CAVLC produced 3067 MBs in F12/F13,
so slice setup is evidently not broken by the zeros.

### 2.1 PICMGMT

The per-frame block is already mapped in [47](47-abi-13.5-frame-rc-surfaces.md)
§1.2 and encoded in `ave_abi.h`; this pass re-read only the parts relevant to
the stall and found no new gaps. Three points worth recording:

- `AVE_CHM_SetDataInfo_FwBuf` writes the per-frame entropy table at
  **PICMGMT `+0xA00 + 0x20i + 8j`, i = 0..15, j = 0..3, addresses only**
  (`0xeb0cb8`–`0xeb0d0c`, source `client+0xD02C0 + 0x80j + 8i`). **There is no
  per-frame size table.** The driver matches this (it writes addresses only).
  **[C]**
- The four `SrcNbr` groups at PICMGMT `+0x980/+0x9A0/+0x9C0/+0x9E0` are written
  four entries each (`0xeb0be4`/`0c1c`/`0c54`/`0c8c`); the driver matches.
- Everything in PICMGMT `+0x980 .. +0xBFF` is overwritten by `setRefPointers`
  every frame from the Start_AVC DPB record ([61](61-mb3083-stall.md) §10.1),
  so the driver's writes there are inert but harmless.

---

## 3. "Kext writes it, we do not" — the actionable list

Ranked by expected effect on a pipe that stalls with its syntax-element buffer
full. Offsets are **wire** offsets in the named command.

| # | command | wire | width | what | value for 1280x720 fixed-QP Baseline CAVLC I, 2 DPB slots | why this rank |
|---:|---|---:|---|---|---|---|
| **1** | Start_AVC | **`0xFA30 + 0x10i + 4j`** | u32 | **EntropyCoding buffer sizes** | `983040` (`0xF0000`) in every cell `i = 0..3`, `j = 0..3` — the allocated size of the buffer at `0xF830+0x20i+8j` | §0. The only value `setPipe` puts in the SEB drain channels' `+0x10` length register, with no other source anywhere in the firmware. Exactly the "kext fills it, we zero it" shape of the previous four fixes |
| 2 | Start_AVC | `0x10DE8` | u32 | `sSVEMap.iNum` — **already written as 1**; listed because it is the gate on #1 | 1 | §0.3. If this were ever 0 the whole Start-time copy is skipped and the size table never reaches `ctrl+0x10C0`, no matter what is on the wire |
| 3 | Start_AVC | `0x10E0C` | u32 | EU/SVE map index into the `0x24` block at `0x10DE8`; fw `0x5caa4` uses it as `VP + 0x1D90 + 8·index` and stores the result to `ctrl[4668]` | 0 (the kext writes the client's CHM index; 0 is the first entry) | 0 is a legal index, so this is probably already right — listed because we write it by memset, not by decision, and because the table it indexes (wire `0x1DF0 + 8i`) is also all zero for us |
| 4 | Process | `0x40` | `0x984` | the client's slice-map object | **[U]** — no layout read. Not recommended to fabricate | Largest unwritten range in the command; no firmware reader located, and CAVLC demonstrably runs without it |
| 5 | Start_AVC | `0x5A8` (u64×2) + `0x5B8` (u32) | | TranscodedData addr/size | only if a transcode is wanted; leave 0 | Transcode never starts in our runs; `SetTranscode`'s asserts are on the entropy table, not this |
| 6 | Start_AVC | `0xF7C0` | u64×2 | CrcQPMod | 0 | no firmware read located on the pipe path |
| 7 | Start_AVC | `0xFCC8` | u64×2 | MBInputCtrl **[I]** | 0 | the MB-input ring is firmware-owned (17 records, [59](59-row1-stall.md)); publishing a host buffer is not obviously required and MbInput demonstrably runs |
| 8 | Start_AVC | `0x3B8`, `0x438`, `0xFB40`, `0xFB68`, `0xFBE8` | u64 arrays | LowResResult / LowResRCResult / the second arms | 0 | [47](47-abi-13.5-frame-rc-surfaces.md) §4: these kinds are count 0 or inert here; the per-frame `LowResResults[]` reads are all `cbz`-skipped |
| 9 | Start_AVC | `0xFC68` | 6 × 16 B | MCTFOutput | 0 | MCTF does not exist on 13.5 for this path |
| 10 | Start_AVC | `0x198` (DPB set 1), `0xF738` (Colocated set 1) | | the **second** set of the recon and colocated tables | 0 | the kext fills 2 sets; `H264VideoEncoderDPB` sets `[dpb+32] = 1` (fw `0x2d1c4`), so only set 0 is read |
| 11 | Start_AVC | `0x14`, `0x10DE0`, `0x10DE4` | u32/u32/u8 | header `+0x14`, two client scalars | 0 | no firmware read located for `0x14`; `0x10DE4` is `bTranscodeOverlap`, correctly 0 for us |
| 12 | Start_AVC | `0x68`, `0x6C`–`0x6F`, `0x70`, `0x74`, `0x78`, `0x7D`, `0x80` | see §1.4 | `VIDEO_PARAMS` scalars the firmware reads | 0 | all either skipped-on-zero or RC-only; documented so the scan is not repeated |

Only **#1** is a change this document recommends making before the next run.

---

## 4. Annotations to earlier documents

Per [00](00-methodology.md) and the standing rule, these **annotate**; nothing
is retracted.

1. **[61](61-mb3083-stall.md) §2.4, §4.2, §7.2 and the §0 summary row
   ("`ctrl+0x10C0` has no writer anywhere in the image", "no located host
   field", "a firmware that *never* writes a field cannot depend on it").**
   Superseded: the writer is `InitEncodingParameters` fw `0x5d734`–`0x5d870`,
   and the host field is Start_AVC wire `0xFA30`. The §2.4 scan was correct for
   what it searched; it could not see `stur` at negative offsets from
   `ctrl+0xED8` / `ctrl+0x10CC`, which is Trap 3 in its exact documented form —
   and §2.4 said so. **[C]**
2. **[61](61-mb3083-stall.md) §7.2's suggested follow-up** ("look for a u32
   table at Start_AVC wire `0xFA30` immediately after the address table with
   the same 4-entry rows") was right, including the strides. **[C]**
3. **[61](61-mb3083-stall.md) §10 / §7.1** ("the entropy table is a Start_AVC
   table, and the per-frame copy is overwritten"). Correct, and now stronger:
   `InitEncodingParameters` *also* loads `ctrl+0xEC0` directly from wire
   `0xF830` at Start time (fw `0x5d7b8` etc.), so the address reaches the
   channels by two independent paths, not one. **[C]**
4. **[61](61-mb3083-stall.md) §2.3 note 4** ("the pipe's four channels come from
   the four rows, all at column `ctrl[4740]`, and the firmware replicates
   column 0 when the pipe command's byte 52 is zero"). The same replicate rule
   exists at **Start** time, keyed on byte 34 of the descriptor passed to
   `InitEncodingParameters` (fw `0x5d7d0`, built at `0x46408` from `x21[18]`).
   It is a firmware-internal byte in both cases, so the host cannot choose the
   arm and must fill all four columns. **[C]**
5. **[47](47-abi-13.5-frame-rc-surfaces.md) §10.4 / [46](46-abi-13.5-commands-session.md) §10.4**
   list `AVE_CHM_SetFwBuf`'s writes as "DPB, CodedData, CodedHeader, Colocated,
   and one `{addr,size}` at `+0xFAD0`". Complete list is §1.3 above: **24**
   destination ranges, including the entropy address *and size* tables and six
   LowRes/MCTF/CrcQPMod/TranscodedData tables not previously recorded. **[C]**
6. **`driver/ave_abi.h` `.entropy_size_set` comment** ("INFERRED, never seen
   written"). It has now been seen written, on both sides: kext `0xeaf2ec`,
   firmware `0x5d768`. The three constants are unchanged. **[C]**
7. **New, not in any earlier document:** `AVE_Client_InitFwBuf` (`0xec8e98`) is
   a complete, machine-checkable table of contents for the buffer half of
   `AVE_VIDEO_PARAMS` — it memsets every buffer table in the struct (in near
   struct order; `+0xFE70` is emitted out of sequence) with its exact length,
   and the ranges tile `VP+0x28 .. VP+0xFE90` with only two gaps, both of which
   hold scalars. Any future "is there a table at X" question should start
   there. **[C]**
8. **New:** `AVE_VIDEO_PARAMS` is copied verbatim from user space by
   `AVE_Client_Config` (`0xecc1bc`, `pInfo + 0x7B0`, `0xFED0` bytes). The kext
   originates none of its scalar fields. This bounds what kext disassembly can
   ever tell us about them, and means the firmware side is the only source for
   their meaning. **[C]**

---

## 5. Reproduce

```sh
cd ~/Projects/apple-ave-driver
K="AVE_MACOS=13.5 python3 tools/disas.py --kext"
M="AVE_MACOS=13.5 python3 tools/disas.py --fw"

# --- the size table, host side ---
eval $K --addr 0xfffffe0008eaf250 -n 0xd0     # SetFwBuf entropy loop: addr +0xF7D0, size +0xF9D0
eval $K --addr 0xfffffe0008ec8e98 -n 0x1a0    # AVE_Client_InitFwBuf: every VP table and its length
eval $K --addr 0xfffffe0008ea5ba8 -n 0xd0     # CalcBufNumOfEntropyCoding (4*n) / CalcBufSizeOfEntropyCoding

# --- the size table, firmware side ---
eval $M --addr 0x5d730 -n 0x150               # InitEncodingParameters: VP+0xF9D0 -> ctrl+0x10C0
eval $M --addr 0x5cabc -n 0x30                # memcpy(ctrl+0x1240, sSVEMap, 0x24): the iNum gate
eval $M --addr 0x5d4c8 -n 0x10                # x20 = VP anchor (CodedData addr[0] at VP+0x458)
eval $M --addr 0x558f0 -n 0xa0                # setPipe channel 0: ctrl+0xEC0 -> +0xC, ctrl+0x10C0 -> +0x10
eval $M --addr 0x58fa0 -n 0x40                # the Transcode reader of the same size table

# --- the two builders ---
eval $K --addr 0xfffffe0008ea9820 -n 0x420    # MakeFwCmd_Start_AVC
eval $K --addr 0xfffffe0008eaed44 -n 0x700    # SetFwBuf, all 23 tables
eval $K --addr 0xfffffe0008eac858 -n 0x190    # MakeFwCmd_Process_AVC
eval $K --addr 0xfffffe0008ecc194 -n 0x100    # AVE_Client_Config: VIDEO_PARAMS is a pass-through
eval $K --addr 0xfffffe0008eb0cb8 -n 0x60     # SetDataInfo_FwBuf: per-frame entropy, addresses only

# --- the negative control for "no writer of ctrl+0x10C0" ---
# docs/61 9 builds $S/fw.S; the scan there finds only readers because the
# writer is  stur w9,[x25,#-12]  with x25 = ctrl+0x10CC:
grep -nE 'stur\s+w9, \[x25, #-(4|8|12)\]|str\s+w9, \[x25\], #16' $S/fw.S
```
