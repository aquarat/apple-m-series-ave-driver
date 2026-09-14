# The per-frame assert map (macOS 13.5, AVC, 1280x720, I-only)

Two hardware attempts have each bought exactly one firmware assertion
([52](52-start-avc-assert.md), [53](53-first-frame.md) §11/§13). This document
walks the whole `Process` (id 7) path statically and enumerates **every** check
that can stop us, so the remaining ones can be satisfied in one reboot.

Nothing here has run on hardware. Static analysis only.

All firmware VAs are 13.5 image VAs (`__TEXT` VA 0 = file `0x4000`); kext VAs
are 13.5 kernelcache VAs. Reproduce any line with

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x54b6c -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb0cc4 -n 0x60
```

**`AVE_MACOS=13.5` is not optional.** Without it `disas.py` reads the 26.6.2
blobs and every address below is wrong.

Each claim is marked **C** (read out of the disassembly, VA cited), **I**
(inferred), or **U** (unknown), per [00](00-methodology.md).

---

## 0. The short version

- The firmware image has **774** `bl 0xa56bc` assert sites. **133** of them lie
  in the functions a `Process` traverses; **31** of those are both reachable
  with our configuration *and* decided by something the host supplies. The rest
  are gated off (mostly by the I-frame short-circuit and by the single-core
  arm), or are internal null/status checks.
- **Exactly one reachable assert is still unsatisfied:**
  `CAVCController_H13C.cpp:8020` in `SetTranscode` —
  `EncCommParams.encoder_addr_entropy[i][transcode_buffer_id] != 0`, fw
  `0x59558`. It is fed from a **per-frame host table at `AVE_PICMGMT_PARAMS +
  0xA00`** (wire `0x13C8` inside the `Process` command) that we currently leave
  zero. Four entries are required.
- Two PICMGMT fields that [53](53-first-frame.md) §2.6 listed as **U** are now
  identified: `+0x8E0` is `EncCommParams.encoder_addr_fw_data` and `+0x8E8` is
  `stats_DMA_addr`. **Both are safely zero for us** — the code that reads them
  is gated on `AVE_VIDEO_PARAMS+12` (wire `0x6C`) and on a firmware-internal
  flag, and both are zero. `+0x900` is `sInput.MultiPassStatsInBuffer`, also
  gated off. `+0x8F0` has no reader anywhere in the image.
- **The `iFwClientMemAddr` risk ([53](53-first-frame.md) §7 item 3) is closed.**
  The firmware maps and sub-allocates that buffer **only when
  `EncCommParams.sSVEMap.iNum > 1`** (fw `0x5cb50`). We send `iNum = 0`, so the
  buffer is never touched, the `iFwClientMemAddr != 0` assert (line 3396) is
  never reached, and `encoder_addr_fw_data` / `mbAddressCPUFWData` /
  `stats_DMA_addr` / `wrDmaBinAddr[]` do not come from it at all — three of
  those four are per-frame PICMGMT fields and the fourth is multi-core only.
- **The client buffer (`client_buf_size` = `0xB4000`) is comfortably large.**
  The firmware's own sub-allocator needs about `0x16000` (≈88 KiB) for AVC:
  `CAVCController::kClientBufferSize` = `0xFF08` plus DPB `0x2700` plus rate
  control `0x49A0` plus a 0x27-byte LRME DPB. Confirmed from the four size
  checks. That `Open` was accepted on hardware already proves the outer
  `CAVEClient` check.

---

## 1. How the inventory was produced

Every assert in this firmware is the same six-instruction idiom:

```
5242c:  bl   0xa56bc              ; assert prologue
52430:  adr  x8, 0xc5b85          ; the expression, as text
52434:  adr  x0, 0xce2b1          ; "ASSERT: "
52438:  adr  x1, 0xc36b1          ; source file
5243c:  adr  x3, 0xce2ef          ; function
52450:  mov  w2, #0x1696          ; #5782 - the source line
52458:  bl   0x949d8              ; log, then AVE_Panic, then a spin loop
```

So the complete inventory is mechanical: disassemble `0x0`-`0xa8000`, find every
`bl 0xa56bc`, and read the four `adr` targets (string VA + `0x4000` = file
offset) and the `mov w2, #line` that follow. 774 sites. **C.**

The reachability column is *not* mechanical and is the part worth re-checking:
each one was resolved by reading the branch that guards the block.

---

## 2. The gates — read these first

Most of the per-frame assert surface is switched off by seven values. Getting
these wrong is what makes an assert map useless, so each is given with its
provenance and whether the *host* can influence it.

| gate | where | value for us | host field | conf |
|---|---|---|---|---|
| `EncCommParams.sSVEMap.iNum` = `ctrl+4672` | copied 36 bytes from `VideoParams+0x10D88` by `memcpy` fw `0x5cad8`; base `add x1,x8,#0xd88` `0x5ca84` | **0** | **wire `0x10DE8`**, u32 | C |
| `ctrl+4664` (multi-core select) | `= (sSVEMap.iNum > 1) ? 2 : 1`, fw `0x5ca88`-`0x5caa0` | **1** | same | C |
| `ctrl+0x23FE7` (FW-data / MB-input-ctrl enable) | `strb w8,[x22,#35]` fw `0x5cf64`, from `[x20,#12]` `0x5cf60`; x22 = `ctrl+0x23FC4`, x20 = `AVE_VIDEO_PARAMS` | **0** | **wire `0x6C`** (`bEnableFwOverride` or `bEnableMBInputCtrl`; the pair is `0x6C`/`0x6D`, assert 3358 fw `0x5ca28` forbids both being set) | C |
| `pPicParams->[0x6E8]` (per-frame FW-data enable) | `ldrb w11,[x11,#1760]` fw `0x54b14`, where `ctrl[9144+8*ctx] = pPicParams+8` (fw `str x11,[x13,#6472]` `0x4817c`, `0xa70+6472 = 9144`) | **0** | **PICMGMT `+0x6E8`** | C |
| `ctrl+0x23FCF` (mbAddressCPUFWData enable) | `strb w8,[x22,#11]` fw `0x5d198`, from `[x25,#70]`, x25 = `VideoParams+0xFED0` (`str x15,[sp,#104]` `0x5ce00`) | **0** | **wire `0xFF76`** (byte) | C |
| `ctrl+0x23FD4` (colocated-data enable) | `strb w11,[x22,#16]` fw `0x5d1d4`, `= (pic_width_in_mbs > 11)` (`cmp w11,#0xb; cset hi` `0x5d1cc`, w11 = `pic_width_in_mbs_minus1 + 1`) | **1** at 1280 (80 MBs) | derived from wire `0x109E4` | C |
| `ctrl[0x2108 + 2*ctx]` (row-restart / second-pass) | zero on a fresh session; written only by `SetTranscode` fw `0x596f0`; same byte [53](53-first-frame.md) §3.3 already records at fw `0x58240` | **0** on frame 1 | none | C |
| `ctrl+0x24D4C` (multi-pass RC) | inside the ctor memset `memset(ctrl+0xa70, 0, 0x244b0)` fw `0x467d8`; no writer found | **0** | none found | I |

Two consequences worth stating on their own:

- `ctrl+4664 == 1` puts us on the **single-core** arm of every `cmp w9,#2` in
  `setPipe`, `SetTranscode`, `PipePrepareParam`, `ProcessPipeStart`,
  `InitEncodingParameters` and `getContextHeightQuadRow`. That is what makes
  `iFwClientMemAddr`, `wrDmaBinAddr[]`, `CollectDataFromCpusMultiCore` and
  `getContextHeightQuadRow`'s `numFullCoreGrps >= 1` all unreachable. **C.**
  (`stats_DMA_addr` is *not* in that list — its block is reachable from both
  arms and is held off by a second gate instead; see assert #19.)
- `ctrl+0x23FD4 == 1` (because we are 80 MBs wide) means the *colocated* block
  in `setPipe` and the colocated `MappedMemory` in `SetTranscode` are entered —
  they survive only because every pointer in them is `cbz`-skipped and because
  `ctrl[0x2108+2*ctx]` is zero. See §5.

---

## 3. The ordered assert map

Order is the sequence a single `Process` would hit them. "Reached" means *with
our configuration* (1280x720, fixed QP, `FrameType = 3` IDR, CAVLC,
`max_num_ref_frames = 1`, `num_ref_idx_l0_active_minus1 < 0`, `sSVEMap.iNum =
0`, one slice).

Predicates are as the image spells them; `pPicParams` is `AVE_PICMGMT_PARAMS *`
(wire = PICMGMT offset + `0x9C8`).

### 3.1 Dispatch and queueing

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 1 | `0xf0ec` | `:2857` | `pCmd` | reached, satisfied | dispatcher already copied the command |
| 2 | `0xf1a4` | `:2869` | `hClientQueue.GetClientPriority(pCmd->client_id, &p) == true` | **reached** | the client id in the `Process` header must be the one `Open` registered. Satisfied today (Open was accepted and the same id is reused). **C** |
| 3 | `0xf1ec` / `0xf234` | `:2878` / `:2880` | `client != NULL`, `hCtrl` | reached, satisfied | internal |
| 4 | `0xf27c` | `:2896` | `pEncCmdBuf` | reached | the per-client encode-command pool; exhausted only with several frames in flight. **C** |
| 5 | `0x1473c` | `:2837` | `0` | **not reached** | the `default:` of `SendCommandToQueue`'s frame-type switch. `GetFrameType` is only called for host values 5 and 9 (fw `0x145dc`-`0x14600`); we send 3. **C** |
| 6 | `0x3e9dc`, `0x3ead8`, `0x3eedc`, `0x3efe4` | `CFrameType::FrameType` `:1332/1333/1452/1481` | display-order invariants | **not reached** | same gate as #5. **C** |

### 3.2 `StartEventPipe` -> `ProcessPipeStart`

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 7 | `0x9154`/`0x93a8`/`0x9424`/`0x9538` | `:1763/1775/1777/1878` | `tmpPIPECmd`, `client != NULL`, `hCtrl`, `rt == 0` | reached, internal | — |
| 8 | `0x52e74` | `ProcessPipeStart:2536` | `pPicParams` | **reached** | `pPicParams = cmdinfo[+16]` (fw `0x52d9c`); non-null because the dispatcher points it at our PICMGMT block. **C** |
| 9 | `0x52ef0` | `ProcessPipeStart:2580` | `hMcpuCtrl` | reached, internal | — |

### 3.3 `PipePrepareParam` (fw `0x480b4`) — called only when `ctrl+4664 != 2`, i.e. **for us** (fw `0x52e10`-`0x52e28`)

This function is where three per-frame PICMGMT tables are copied into the
controller. It is the reason the `SetTranscode` entropy assert is a *host*
problem and not a firmware-internal one.

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 10 | `0x48274` | `:2215` | `EncCommParams.slices_per_frame <= AVE_SLICE_MAX_NUM` | **reached**, satisfied | `slices_per_frame = ceil(mbW*mbH / MBsPerSlice)`, asserts if `>= 0x101` (fw `0x4826c`). Whole block skipped when `MBsPerSlice == 0` (`cbz w8, 0x482c8` `0x48250`). One slice -> 1. **C** |
| 11 | `0x491cc` | `:2376` | `pPicParams->sInput.MultiPassStatsInBuffer != 0` | **not reached** | gated on `ctrl+0x24D4C != 0` and `ctrl+0x24D50 == 2` (fw `0x48cb0`-`0x48cc0`) — the multi-pass RC flags, zero for us. The field is **PICMGMT `+0x900`**; when enabled it is mapped `0x72E` bytes (fw `0x48cec`). **C** for the offset, **I** that the flags are zero |
| 12 | `0x23ad0` | `ProcessFirstPassStats:782` | same field | **not reached** | same multi-pass gate. **I** |

**Silent copies in the same function — no assert here, but they decide later
ones:**

| what | firmware | source | conf |
|---|---|---|---|
| `EncCommParams.encoder_addr_fw_data` = `ctrl+5088` | `ldr x8,[x26,#2272]; str x8,[x19,#5088]` fw `0x48c04`-`0x48c08` | **PICMGMT `+0x8E0`**, only stored when `ctrl+0x23FE7 != 0 \|\| pPicParams[0x6E8] != 0` (fw `0x48bec`-`0x48c00`) | C |
| `stats_DMA_addr` = `ctrl+5112` | `ldr x8,[x26,#2280]; str x8,[x19,#5112]` fw `0x48c8c`-`0x48c90` | **PICMGMT `+0x8E8`**, gated on two `IsPerMbStatsLumaVarEnForCurFrame` results (fw `0x48c18`/`0x48c30`) | C |
| `EncCommParams.encoder_addr_entropy[i][j]` = `ctrl[0xEC0 + 32i + 8j]` | `ldr x14,[x13,#2560]; str x14,[x12,#3776]` fw `0x48800`-`0x4881c` (and the `+2568/+2576/+2584` siblings) | **PICMGMT `+0xA00 + 32i + 8j`**, `i < ctrl+3768` | C |

`ctrl+3768` is set to **4** unconditionally on our arm (`mov w10,#4` fw
`0x5d064`, `str w10,[x19,#3768]` `0x5d074`); on the multi-core arm it is
`4 * sSVEMap.iNum`. So the firmware copies **four** `i` groups. **C.**

When the `sCmdInformation` byte `[arg1 + 52]` is zero the copy instead
replicates element `j = 0` into all four `j` (fw `0x48aa8`-`0x48ac4`). **C**
that it does; **U** what that byte is.

### 3.4 `setPipe` (fw `0x52f38`)

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 13 | `0x27aec` | `getContextHeightQuadRow:3176` | `numFullCoreGrps >= 1` | **not reached** | called unconditionally at fw `0x52fd8` with `&EncCommParams.sSVEMap`, but the assert is on the `ctrl+4664 == 2` arm only (`b.ne 0x27ab0` at `0x27a60`). **C** |
| 14 | `0x538c0` | `:6023` | `(pPicParams->sLowResOutput.LowResResults[i] & 63) == 0` | **not reached** | `cbz x8, 0x5341c` at fw `0x533c0`. **Safely zero.** C |
| 15 | `0x53e8c`-`0x5478c` (16 sites) | `:6133`-`:6333` | `sRef.{Y,UV}_{L0,L1}_{MSB,LSB}[ref] != 0`, `& 63 == 0`, and `LowResResults[me_ref_index]` (`:6184/6185/6266/6267`) | **not reached** | the reference half is skipped for `num_ref_idx_l0_active_minus1 < 0` ([53](53-first-frame.md) §11.3). **C** |
| 16 | `0x5428c` / `0x54244` | `:6440` / `:6441` | `pPicParams->sInput.Y != 0`, `& 63 == 0` | **reached, satisfied** | PICMGMT `+0x8C0` |
| 17 | `0x546fc` / `0x54354` | `:6454` / `:6455` | `pPicParams->sInput.UV != 0`, `& 63 == 0` | **reached, satisfied** | PICMGMT `+0x8D0` |
| 18 | `0x54c0c` / `0x54b7c` | `:6632` / `:6633` | `EncCommParams.encoder_addr_fw_data != 0`, `& 63 == 0` | **not reached** | `cbz w11, 0x54e34` at fw `0x54b1c` when `ctrl+0x23FE7 == 0` **and** `pPicParams[0x6E8] == 0`. Both zero. **C** for the gate, **I** that `PICMGMT+0x6E8` is zero in our command (we memset it) |
| 19 | `0x54bc4` / `0x54abc` | `:6697` / `:6698` | `stats_DMA_addr != 0`, `& 63 == 0` | **not reached** | the block at fw `0x54a20` is entered from *both* arms — directly when `ctrl+4664 == 2` (`b.ne 0x54b04` at `0x54a1c`) and via `cbz w11, 0x54a20` at fw `0x54e38` on ours — so the only thing keeping this assert away is the second gate: `[ctrl+0x1BA8] == 0 && [ctrl+0x1BA9] == 0` (`ldrb w9,[x9,#2429]`/`#2430` at fw `0x54a70`/`0x54a7c`, base `ctrl+0x122B` from `[sp,#176]` fw `0x52f74`). Both are inside the ctor memset and no writer was found. **C** for the gate, **I** that it is zero |
| 20 | `0x55054` / `0x54e98` | `:6659` / `:6660` | `mbAddressCPUFWData != 0`, `& 63 == 0` | **not reached** | `cbz w11, 0x54a20` at fw `0x54e38` when `ctrl+0x23FCF == 0` (wire `0xFF76`). **C** |
| 21 | `0x5500c` / `0x54dec` | `:6727` / `:6728` | same, other arm | **not reached** | `cbz w9, 0x54fdc` at fw `0x54d98`, same flag. **C** |
| 22 | `0x550e4` / `0x54f94` | `:6777` / `:6778` | `sRecon.Y_LSB != 0`, `& 127 == 0` | **not reached** | gate `ldrb w8,[x24,#1312]` = `ctrl+0x24058`, `cbz w8, 0x54ffc` fw `0x54f80` ([53](53-first-frame.md) §11.5). Filled anyway. **C** |
| 23 | `0x55358` / `0x55310` | `:6791` / `:6792` | `sRecon.Y_MSB != 0`, `& 127 == 0` | **reached, satisfied** | overwritten by `setRefPointers` from the Start-time recon table ([53](53-first-frame.md) §13.1) |
| 24 | `0x55978` / `0x5541c` | `:6805` / `:6806` | `sRecon.UV_LSB != 0`, `& 127 == 0` | **not reached** | gate `ldr w12,[x19,#2692]; cbz` fw `0x55404` |
| 25 | `0x580b0` / `0x58068` | `:6818` / `:6819` | `sRecon.UV_MSB != 0`, `& 127 == 0` | **reached, satisfied** | derived by `setRefPointers` as `Y_MSB + [dpb+20]` |
| 26 | `0x5509c` / `0x54f34` | `:6860` / `:6861` | `(sRef.Colocated_L1[0] + coloDataContextOffset) != 0`, `& 63 == 0` | **not reached** | `cbz x9, 0x551fc` fw `0x54f20`. **Safely zero.** C |
| 27 | `0x553a0` / `0x55260` | `:6873` / `:6874` | same minus `20 * sizeof(aveCommon_AVECoLoData_t)` | **not reached** | `cbz x11, 0x5547c` fw `0x55248`. **Safely zero.** C |
| 28 | `0x55554` / `0x55504` | `:6889` / `:6890` | `(EncCommParams.encoder_addr_dst_colo + off) != 0`, `& 63 == 0` | **not reached** | `cbz x10, 0x5554c` fw `0x554f0`. **Safely zero.** C (also in [53](53-first-frame.md) §2.4) |
| 29 | `0x55618` / `0x555d0` | `:6990` / `:6991` | `EncCommParams.encoder_addr_src_nbr_info != 0`, `& 63 == 0` | **reached, satisfied** | wire `0xF7D0` |
| 30 | `0x556b8` / `0x55670` | `:6993` / `:6994` | `EncCommParams.encoder_addr_src_nbr_pixels != 0`, `& 63 == 0` | **reached, satisfied** | wire `0xF7F0` |
| 31 | `0x58160` | `:7402` | `EncCommParams.slicePPSIdIdx < AVE_MAX_NUMBER_OF_PPS` | **conditional** | a linear search of nine PPS ids at `[sp160]+1496+4i` against the slice's id `[sp160]+20` (fw `0x56900`-`0x569d8`); the assert is the "no match" tail. Both sides are firmware-generated from the Start-time PPS, so it should match at `i = 0`. **C** for the mechanism, **I** that it matches |
| 32 | `0x60250` | `prepSlicHeaderForCavlc:4794` | `uSlcHdrCnt > 0 && uSlcHdrCnt <= AVE_MAX_SLICEHEADERS_WALKAROUND` | **conditional** | counted over MB rows (fw `0x60114`-`0x60224`), asserts if `count - 1 >= 0x10`. Called from `ConfigureMCPUs` fw `0x60424`; `setPipe` calls `ConfigureMCPUs` or `ConfigureNoMCPUs` depending on two `sCommonParams` bytes (fw `0x57ddc`-`0x57e9c`). **C** for the predicate, **U** for which branch we take |
| 33 | `0x61a78` / `0x61a30` / `0x61adc` | `:458` / `:462` / `:468` | `len >= 4`, `len & 3 == 0` | **conditional, internal** | a static MCPU copy helper at fw `0x61a04` (it is *not* part of `ConfigureMCPUs`; the symbol table just puts it there). Length is firmware-computed |

### 3.5 `setLRME` (fw `0x5144c`), called from `setPipe` fw `0x57d30`

All fourteen sites were inventoried in [53](53-first-frame.md) §11.3. With
`num_ref_idx_l0_active_minus1 < 0` the reference half is skipped, so only:

| # | VA | file:line | predicate | reached? |
|---|---|---|---|---|
| 34 | `0x5164c` | `:5493` | `EncCommParams.slices_per_frame <= AVE_SLICE_MAX_NUM` | reached, satisfied |
| 35 | `0x520bc` | `:5654` | `(LowResResults[i] & 63) == 0` | not reached (`cbz`) — **safely zero** |
| 36 | `0x522bc` / `0x52274` | `:5719` / `:5720` | `sInput.Y != 0`, `& 63 == 0` | reached, satisfied |
| 37 | `0x5242c` / `0x523e4` | `:5782` / `:5783` | `(LowResSrcLumaScaled + encode_row_init/4*lr_stride) != 0`, `& 63 == 0` | **reached** — satisfied by the `Start_AVC` LowResRef table at wire `0x2A8` ([53](53-first-frame.md) §13) |

### 3.6 `ProcessTranscodeStart` (fw `0x581a8`)

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 38 | `0x582a8` | `:2615` | `pPicParams` | reached, satisfied | |
| 39 | `0x58404` | `:2649` | `pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]` | **reached, satisfied** | `sOutput` mode byte = 0 ([53](53-first-frame.md) §2.2) |
| 40 | `0x583bc` | `:2655` | `sOutput.CodedBufSize > (minBufSize/2)` | **not reached** | mode != 0 arm only |
| 41 | `0x5844c` | `:2656` | `pPicParams->sOutput.Coded` | reached, satisfied | |

### 3.7 `SetTranscode` (fw `0x58494`), called from `ProcessTranscodeStart` fw `0x58278`

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 42 | `0x5869c` / `0x58654` | `:7928` / `:7929` | `EncCommParams.encoder_addr_src_nbr_data != 0`, `& 63 == 0` | **reached, satisfied** | PICMGMT `+0x9C0` overwrites the Start-time value (fw `0x583ac`) |
| 43 | `MappedMemory.cpp:39/40` via fw `0x597a0` | — | `paddr != 0`, `size != 0` | **not reached** | maps `mbAddressCPUFWData` (`ctrl+0x1BB0`/`0x1BB8`), `16*mbW*mbH` bytes; gated `ldrb w8,[x23]` = `ctrl+0x23FCF`, `cbz w8, 0x59800` fw `0x59768`. Same flag as #20/#21 — a useful cross-check that the flag reading is right. **C** |
| 44 | `MappedMemory.cpp:39/40` via fw `0x59838` | — | `paddr != 0` | **not reached** | maps `EncCommParams.encoder_addr_dst_colo` (`ctrl+5080`), `64*mbW*rows`; gated on `ctrl+0x23FD4` (**true** for us — width > 11 MBs) **and** on `ctrl[0x2108+2*ctx]` (`cbz w8, 0x59a34` fw `0x59818`), which is zero on frame 1. **This is the one "safe zero" that is only safe because of a second gate** — see §5 |
| 45 | `0x594c8` / `0x59510` | `:7992` / `:7993` | `EncCommParams.wrDmaBinAddr[tbid][i] != 0`, `& 63 == 0` | **not reached** | `cmp ctrl+4664,#2; b.ne 0x5918c` at fw `0x59144`. Multi-core only. **C** |
| 46 | `0x59558` / `0x595a0` | `:8020` / `:8021` | `EncCommParams.encoder_addr_entropy[i][tbid] != 0`, `& 63 == 0` | **REACHED — NOT SATISFIED** | see §4.1 |
| 47 | `0x593d4` | `:8053` | `EncCommParams.curr_bitstream_addr_dst != 0` | **reached, satisfied** | stored from `sOutput.Coded` at fw `0x583a4` |

### 3.8 After the hardware runs

| # | VA | file:line | predicate | reached? | gate / note |
|---|---|---|---|---|---|
| 48 | `0x9ee8`, `0xa048`, `0xa0c0`, `0xa108`, `0x9ff8`, `0xa1c0` | `DoneEventPipe:2160/2175/2177/2179/2266/2270` | internal pointers, `rt == 0` | reached | |
| 49 | `0x59e50` | `ProcessPipeDone:2964` | `rt == 0` | reached, internal | |
| 50 | `0xa258`, `0xa420`, `0xa4b8`, `0xa500`, `0xa5b4` | `DoneEventTranscode:2305/2318/2320/2326/2373` | internal | reached | |
| 51 | `0x5a918` | `CollectDataFromCpus:8393` | `frame_done` | **reached** | `tbz w22,#0` fw `0x5a8a0`; the hardware must report the frame complete. A failure here means the encode did not finish, not a missing field. **C** |
| 52 | `0x5ac88` | `:8473` | `0` | conditional | a `default:` arm |
| 53 | `0x5ab48` / `0x5abac` | `:8485` / `:8486` | `nRemWords > 0`, `<= BITMAP_16X16_BUF_SIZE_IN_WORDS` | reached, firmware-computed | |
| 54 | `0x5af04` | `:8508` | `coded_data_hdr->ui32_numAccumSkipMbCntNMbs <= mbH*mbW` | reached, firmware-computed | writes into the coded-header buffer we supply |
| 55 | `0x5b80c` | `:8657` | `EncCommParams.sSVEMap.iNum == 1` | **conditional — we send 0** | gated on `ctrl+0x24D4C != 0` (fw `0x5b764`, the multi-pass flag, same byte as #11) and on `([sp120][0] \| 8) == 9` (fw `0x5b774`). If the multi-pass flag is ever non-zero this fires with `iNum = 0`. **Cheap insurance: send `iNum = 1`.** See §4.2 |
| 56 | `MappedMemory.cpp:39/40/57/60` via fw `0x59f78`, `0x5bdf8`, `0x5c1ec`, `0x5c260` | — | `paddr != 0`, `size != 0`, RTK status | **reached, satisfied** | all four map the **coded-header buffer** for this context, `0x22C60` bytes. Our buffer is `0x23000` ([53](53-first-frame.md) §3.2). `HwToTarget` then asserts `:87/88/89` that the address is inside the mapping |
| 57 | `0x5c074` | `ProcessTranscodeDone:3308` | `rt == 0` | reached, internal | |
| 58 | `0x5c36c` / `0x5c3c8` | `CollectDataFromTranscode:8705` / `:8736` | `EncCommParams.slice_count >= 1`, `> 0` | **reached** | firmware-computed from the hardware's slice output; zero slices means the encode produced nothing. **C** |
| 59 | `0x14d44` / `0x14ec4` | `ProcessEncDone:3533` / `:3539` | `client != NULL`, `hCtrl` | reached, internal | then `NotificationToHost(0xE06)` |
| 60 | `0x10aa8` / `0x10af0` | `ProcessComplete:3308` / `:3326` | `pCmd`, `client != NULL` | reached, internal | |

Not an assert but worth repeating: `ProcessTranscodeDone` fw `0x5bf7c` checks
`[8312] + [8316] + [8356] <= [8360]` and logs
`"bitstream size overflow, buffer size: %d, bitstream size: %d"` (fw `0x5bf84`)
rather than asserting. **C.**

---

## 4. What to fill, all at once

### 4.1 The entropy-coding table — `AVE_PICMGMT_PARAMS + 0xA00` (required)

**This is the only unsatisfied reachable assert on the path.**

Shape, read from both sides:

- The firmware reads `ctrl[0xEC0 + 32i + 8j]` and asserts `j = transcode_buffer_id`
  (`ctrl+4748`, initialised to 0 at fw `0x5d080`), for `i < ctrl+3768` = **4**.
  Loop: base `mov w22,#0xec0` fw `0x58fc0`, stride `add x22,x22,#0x20` fw
  `0x59110`, entry `b 0x5913c` fw `0x58fcc` (so the first iteration runs
  unconditionally once `ctrl+3768 != 0`), bound `cbz w8, 0x59268` fw `0x58fa4`.
  **C.**
- `PipePrepareParam` fills `ctrl[0xEC0 + 32i + 8j]` from
  `pPicParams[0xA00 + 32i + 8j]`, fw `0x48800`-`0x4881c`. **C.**
- Apple's host writes the same shape: `AVE_CHM_SetDataInfo_FwBuf` at kext
  `0xfffffe0008eb0cc4` (`add x26, x20, #0xa00`) with an outer `j < 4`
  (`cmp x22,#0x4` `0xeb0d08`, `add x26,x26,#0x8` `0xeb0d04`) and an inner
  `i < 16` (`cmp x23,#0x10` `0xeb0cf4`, `add x27,x27,#0x20` `0xeb0cf0`),
  sourced from surfaces at `client + 0xD02C0 + j*0x80 + i*8`. **C.**

So the table is `u64 tbl[16][4]` at PICMGMT `+0xA00`, i.e. **wire `0x13C8`**
(`0x9C8 + 0xA00`), element `[i][j]` at wire `0x13C8 + 32i + 8j`.

**What to write:** four distinct, non-zero, 64-byte-aligned IOVAs at
`[0][0]`, `[1][0]`, `[2][0]`, `[3][0]` — wire `0x13C8`, `0x13E8`, `0x1408`,
`0x1428`. `[i][1..3]` are only read when `transcode_buffer_id != 0`, which
needs a second frame in flight, so they may stay zero for the first frame.
(**I** — the reasoning is that `ctrl+4748` is zeroed at Start and only
`ProcessTranscodeStart` writes it, from `cmdinfo[+44]` at fw `0x58238`.)

**Size per buffer**, from `AVE_CalcBufSizeOfEntropyCoding(_E_AVE_CodecType,
W, H, CHROMA_FORMAT, uint, bool)` kext `0xfffffe0008ea5bd0`, AVC arm
(`codec == 0`, `cbnz w0` at `0xea5bdc` returns 0 for anything else):

```
stride = ALIGN_DOWN(64*W + 960, 1024)          ; 0xea5bfc-0xea5c04
K      = arg5 ? ((H+15)/16 + 3)/4 : 8          ; 0xea5be0-0xea5bf8
size   = stride * K                            ; 0xea5c6c
```

At 1280x720: `stride = 81920`; `K = 12` or `8`; **size = `0xF0000` (960 KiB)
or `0xA0000` (640 KiB)**. `arg5` could not be pinned to a call-site constant
(it is `[sp,#112]` inside `AVE_Client_CalcSurfaceInfo_LRME`, kext
`0xfffffe0008ec6ebc`), so **use `0xF0000`** — the larger — until measured.
Four buffers = **3.75 MiB**. **C** for the formula, **U** for `arg5`.

The count is consistent with `AVE_CalcBufNumOfEntropyCoding` (kext
`0xfffffe0008ea5ba8`): AVC returns `4 * arg2` when `arg3 != 0`, zero otherwise.
**C.**

### 4.2 `sSVEMap.iNum` — `Start_AVC` wire `0x10DE8` (cheap insurance)

We send 0. `InitEncodingParameters` copies 36 bytes from `VideoParams+0x10D88`
to `ctrl+0x1240` (fw `0x5cad8`) and derives `ctrl+4664` from the first `u32`.

- `iNum = 0` and `iNum = 1` both give `ctrl+4664 = 1` (the single-core arm) —
  `cinc w8,w8,hi` on `cmp w8,#1` fw `0x5ca94`-`0x5caa0`. **C.**
- The only loop that would newly execute is at fw `0x5cafc`, and it is guarded
  by `cmp w8,#2; b.cc 0x5cb38` fw `0x5cae8`, so `iNum = 1` still skips it. **C.**
- `iNum = 1` closes assert #55 (`sSVEMap.iNum == 1`, fw `0x5b80c`), which is
  otherwise conditional on a flag we believe but cannot prove is zero.

So: write `1` at wire `0x10DE8` and leave the other 32 bytes zero. **I** that
the rest of the struct is not read on the single-core arm (the two readers
found — `getContextHeightQuadRow` fw `0x27a64` and the fw-client-mem carve fw
`0x5cafc` — are both on the `ctrl+4664 == 2` arm; **C** for those two, **I**
for "no others").

### 4.3 Nothing else

Every other reachable assert on the path is already satisfied by what
`ave_session.c` sends today, or is firmware-internal. In particular
[53](53-first-frame.md) §7 items 3 and 4 and §13.7 item 3 — `iFwClientMemAddr`,
`encoder_addr_fw_data`, `mbAddressCPUFWData`, `stats_DMA_addr`,
`wrDmaBinAddr[]` — are **all unreachable** for a single-core session. See §6.

---

## 5. Checks that are safely zero — do not fill these

| field | wire / PICMGMT | why zero is fine | conf |
|---|---|---|---|
| `sLowResOutput.LowResResults[0..3]` | PICMGMT `0xC28`-`0xC40` | every read is `cbz`-skipped: fw `0x533c0` (`:6023`), `0x51e84`/`0x51ed4`/`0x5204c`/`0x520a4` (`:5654`) | C |
| `sRef.Colocated_L1[0]` | PICMGMT `0x838` | `cbz x9, 0x551fc` fw `0x54f20`; `cbz x11, 0x5547c` fw `0x55248` | C |
| `EncCommParams.encoder_addr_dst_colo` / the Start-time colocated table | wire `0xF6B0` | `cbz x10, 0x5554c` fw `0x554f0` in `setPipe`; in `SetTranscode` the `MappedMemory` is gated on `ctrl[0x2108+2*ctx]` (`cbz w8, 0x59a34` fw `0x59818`), zero on frame 1 | C |
| `PICMGMT + 0x8E0` (`encoder_addr_fw_data`) | wire `0x12A8` | store and assert both gated on `VideoParams+12` (wire `0x6C`) and `PICMGMT+0x6E8`, both zero | C / I |
| `PICMGMT + 0x8E8` (`stats_DMA_addr`) | wire `0x12B0` | only read on the `ctrl+4664 == 2` arm, and then behind `ctrl+0x1BA8`/`0x1BA9` | C |
| `PICMGMT + 0x8F0` | wire `0x12B8` | **no reader anywhere in the image** — a scan of all `ldr x?, [x?, #2288]` finds nothing (contrast `#2272`, `#2280`, `#2304`, which all hit). The scan's negative control therefore passes | C |
| `PICMGMT + 0x900` (`sInput.MultiPassStatsInBuffer`) | wire `0x12C8` | both readers gated on the multi-pass flags `ctrl+0x24D4C`/`0x24D50` | C / I |
| `PICMGMT + 0x6E8` | wire `0x10B0` | it is itself a *gate*; leaving it zero is what disables the FW-data path | C |
| `sRecon.Y_LSB` / `UV_LSB` | PICMGMT `0x8A0` / `0x8B0` | gated ([53](53-first-frame.md) §2.4); filled anyway, harmlessly | C |
| `sRef.Y/UV_L0/L1[]`, `Low_Res_Y_L0/L1[]` | PICMGMT `0x6xx`-`0x7xx` | the whole reference half of `setPipe` and `setLRME` is skipped for an I-frame | C |
| `LowResResult` / `LowResRCResult` tables | wire `0x3B8` / `0x438` | only read on the non-IDR branch of `ManageDPBBuffer` ([53](53-first-frame.md) §13.7 item 2) | C |
| `sOutput.CodedBufSize` | PICMGMT `0xC18` | ignored when the mode byte is 0 | C |

---

## 6. The two sub-allocators

[53](53-first-frame.md) §7 listed "the firmware's own sub-allocator inside
`iFwClientMemAddr`" as an open risk. It is two separate allocators, and neither
is a risk for this session.

### 6.1 `iFwClientMemAddr` (we hand it 1 MiB) — **not used at all**

```
5cb50:  ldr  w8, [x19, #4664]
5cb58:  cmp  w8, #0x2
5cb5c:  b.ne 0x5cdd0             ; <- we take this; nothing below runs
5cb60:  ldr  x8, [x25, #104]
5cb64:  cbnz x8, 0x5cdd0         ; already mapped once
5cb68:  ldr  x8, [x27, #40]      ; iFwClientMemAddr
5cb6c:  cbz  x8, 0x5cc2c         ; ASSERT 3396  iFwClientMemAddr != 0
5cb90:  bl   0x20bc0             ; MappedMemory(paddr = it, size = [x27+48])
5cb9c:  bl   0x20c00             ; HwToTarget -> x22 = CPU base
5cbe8:  loop k = 0 .. ctrl[4672]-1:
5cbf4:      ctrl[0x30278 + 8k] = x22
5cbf0:      x22 += 0x2E5C                       ; 11868 bytes per slot
```

So when it *is* used, the carve is **`sSVEMap.iNum` slots of `0x2E5C`
(11,868) bytes each**, i.e. 11,868 bytes per SVE segment, plus whatever the
`> 3` tail at fw `0x5ccec` adds. 1 MiB covers 88 segments, so the size we hand
it was never the problem. **C** for the loop and the stride; **I** for the tail.

For us `ctrl+4664 == 1`, so `iFwClientMemAddr` is never dereferenced and
assert 3396 is unreachable. If a future session sets `sSVEMap.iNum > 1`, it
becomes reachable and the size requirement is `iNum * 0x2E5C`.

### 6.2 The client buffer (`client_buf_size` = `0xB4000`) — comfortable

The firmware bump-allocates out of it with a running `{pointer, remaining}`
pair at `[x25+88]` / `[x25+80]`, checking before each take:

| consumer | required | check | conf |
|---|---|---|---|
| `CAVCController::kClientBufferSize` | **`0xFF08`** (65,288) | `mov w8,#0xff07; cmp w19,w8; b.ls 0x46170` fw `0x460bc`-`0x460c4` (assert 138) | C |
| `hAvcDPB` | **`0x2700`** (9,984) | `lsr w8,w8,#8; cmp w8,#0x26; b.ls 0x5e36c` fw `0x5dcd0`-`0x5dcd8` (assert 3902) | C |
| `hRateControl[0]` + `CRateControl::kClientBufferSize` | **`0x49A0`** (18,848) | `lsr w8,w8,#5; cmp w8,#0x24c; b.ls 0x5e3b4` fw `0x5deac`-`0x5deb4` (assert 3765) | C |
| `hAvcLrmeDPB` | **`0x27`** (39) | `cmp w8,#0x26; b.ls 0x5e3fc` fw `0x5ddd0` (assert 3916) | C |

Total ≈ `0x15FCF` (≈88 KiB) against the `0xB4000` (720 KiB) we supply. The
outer `CAVEClient` check (`clientBufferAvailableSize >= kClientBufferSize[codec]`,
fw `0x17158`, line 654) runs at `Open`, which hardware has already accepted —
that is the firmware's own opinion that our client buffer is big enough. **C.**

### 6.3 Buffers that come from the *host* per frame, not from a sub-allocator

The naming in [53](53-first-frame.md) §7 implied these were carved by the
firmware. They are not:

| controller field | actually comes from | conf |
|---|---|---|
| `EncCommParams.encoder_addr_fw_data` | `PICMGMT + 0x8E0` (host) | C |
| `stats_DMA_addr` | `PICMGMT + 0x8E8` (host) | C |
| `EncCommParams.encoder_addr_entropy[i][j]` | `PICMGMT + 0xA00 + 32i + 8j` (host) | C |
| `mbAddressCPUFWData` | `ctrl+0x1BB0` / `ctrl+0x1BB8` — firmware-internal, selected by `ctrl+8380 & 1` (fw `0x54da0`-`0x54db8`) | C |
| `EncCommParams.wrDmaBinAddr[tbid][i]` | `ctrl+0x1CC0 + tbid*0x80 + 8i` — firmware-internal | C |

The two firmware-internal ones are only read on paths we do not take, so their
provenance stays **U** and does not matter yet.

---

## 7. Provenance check — fields the firmware overwrites before reading

[53](53-first-frame.md) §13 cost a reboot because `PICMGMT + 0xC20` is
overwritten by `setRefPointers` before `setLRME` reads it. Every field §4
proposes writing was checked the same way:

| field | overwritten before use? | evidence |
|---|---|---|
| `PICMGMT + 0xA00..0xA7F` (entropy) | **No.** The only writer of that range in the firmware is nothing; `PipePrepareParam` *reads* it (fw `0x48800`) | a scan of all stores to `[x?, #2560]`-`[x?, #2584]` and to `add x?, x?, #0xa00` bases in `__TEXT` finds only the HEVC twin at fw `0x66624`, which writes its own controller, not PICMGMT. **C**, with the trap-3 caveat that an indexed store through a computed base would be invisible |
| `wire 0x10DE8` (`sSVEMap`) | **No.** It is copied *out* of the command into `ctrl+0x1240` at Start (fw `0x5cad8`) and never written back | C |
| `PICMGMT + 0x8E0 / 0x8E8` | not applicable (we are leaving them zero), and no firmware writer exists: stores to `[x?, #2272]` / `[x?, #2280]` in the image are 32-bit stores into unrelated HEVC structs at fw `0x6b1bc` / `0x7fa8c` | C |

Contrast with the ones that *are* overwritten, so the list stays honest:
`PICMGMT + 0xC20` (fw `0x2c320`), `+0x898` (fw `0x2c338`), `+0x8A0` (fw
`0x2c328`), `+0x8A8` (derived, fw `0x2c324`), `+0x8B8` (fw `0x2c4b0`) — all by
`setRefPointers`, all fed from the `Start_AVC` DPB tables.

---

## 8. What is still unknown, and the cheapest way to settle it

| unknown | why it matters | cheapest test |
|---|---|---|
| `AVE_CalcBufSizeOfEntropyCoding` `arg5` (K = 8 or 12) | entropy buffer size: 640 KiB vs 960 KiB each | allocate 960 KiB (the larger); no test needed. To settle it properly, read `AVE_Client_CalcSurfaceInfo_LRME` kext `0xfffffe0008ec6e40` backwards for `[sp,#112]` |
| whether `[i][1..3]` of the entropy table are ever read on frame 1 | 4 buffers vs 16 | fill `[i][0]` only; if an assert names `:8020` again with a non-zero `transcode_buffer_id` in the log, fill the rest |
| `ctrl+0x23FCF` source byte = wire `0xFF76` — what it *means* | if Apple sets it, we may be disabling a needed feature rather than an optional one | not a first-frame risk: the field it enables (`mbAddressCPUFWData`) is firmware-internal, so turning it on would need nothing from us |
| `ctrl+0x1BA8` / `0x1BA9`, `ctrl+0x24D4C` — no writer found | they gate asserts #19, #11, #55 | negative-control problem: the offset scan that finds no writer for these *does* find one for `ctrl+0x23FE7`, but only because that one uses an immediate offset from a base the scan happened to cover. So "no writer" here is **I**, not **C** |
| whether `setPipe` takes `ConfigureMCPUs` or `ConfigureNoMCPUs` | decides whether `prepSlicHeaderForCavlc:4794` runs | the branch is on two `sCommonParams` bytes at `+1196`/`+1197` (fw `0x57ddc`/`0x57de4`) whose writers are in `CMultiPassControl::CollectMultiPassStats_RC` (fw `0x342d4`) — i.e. set on a later pass, so the first frame most likely takes `ConfigureNoMCPUs`. **I** |
| `cmd + 0x40` (`0x984` bytes, the `AVC_Slice` copy) | still zero | unchanged from [53](53-first-frame.md) §7 item 1; no firmware read of that range was matched on this walk either |
| `PICMGMT + 0x8F0` | nothing reads it | settled as far as it can be: leave zero |

---

## 9. Suggested changes to files this document does not own

Recorded here as proposals; nothing outside `docs/54-assert-map.md` was edited.

1. **`driver/ave_abi.h`** — add to `struct ave_process_avc_layout`:
   `entropy_set = 0xA00`, `entropy_stride_i = 0x20`, `entropy_stride_j = 0x08`,
   `entropy_max_i = 16`, `entropy_max_j = 4` for 13.5; `AVE_OFF_NONE` for
   26.6.2 (no counterpart located — the 26.6.2 `PipePrepareParam` was not read).
2. **`driver/ave_cmd.c`** — write `entropy[i]` into `[i][0]`, reject a
   misaligned or zero-holed table, allow an empty table (the bisect control).
3. **`driver/ave_session.c`** — `ave_session_alloc_entropy()`: four slots of
   `ALIGN_DOWN(64*W + 960, 1024) * 12` (960 KiB at 1280x720) out of one
   coherent arena, 64-byte aligned, with a `session_entropy_kb` override and a
   `session_entropy=0` control that reproduces `SetTranscode:8020` exactly.
   Keep the existing 4 GiB allocation guard.
4. **`driver/ave_abi.h` / `ave_cmd.c`** — a `sve_num` field at `Start_AVC` wire
   `0x10DE8`, default 1 (§4.2).
5. **`docs/53-first-frame.md`** — §2.6 can be retired: `0x8E0`, `0x8E8`,
   `0x900` are identified and all three are safely zero; §7 item 3 and §13.7
   item 3 (`iFwClientMemAddr`) can be closed.
6. **`tools/session_selftest`** — checks that each entropy entry lands at
   `entropy_set + i*0x20`, inside the PICMGMT block and inside the command,
   does not overlap `sOutput` (`0xC00`) or the `SrcNeighbor` groups
   (`0x980`-`0x9E0`), plus the negative controls (32- but not 64-aligned entry,
   zero hole, more entries than `entropy_max_i`, and the deliberate zero table).

---

## 10. Reproduce

```sh
# the whole assert inventory
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x0 -n 0xa8000 \
  | grep -B0 -A12 'bl\s*0xa56bc'

# the one unsatisfied assert, and the loop it sits in
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x58f90 -n 0x1e0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x59558 -n 0x60

# where the entropy table comes from: host -> PICMGMT -> controller
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb0cb0 -n 0x60
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x4879c -n 0x90
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ea5bd0 -n 0xa4

# the gates
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5ca80 -n 0x30    # sSVEMap -> ctrl+4664
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cf60 -n 0x10    # wire 0x6C  -> ctrl+0x23FE7
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5d190 -n 0x50    # wire 0xFF76 -> ctrl+0x23FCF
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x48bec -n 0x30    # the fw_data gate
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54b04 -n 0x70    # the same gate in setPipe
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x59760 -n 0xc0    # the two SetTranscode mappings

# the sub-allocators
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5cb50 -n 0xd0    # iFwClientMemAddr carve
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x460b0 -n 0x20    # kClientBufferSize
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5dcc4 -n 0x20    # hAvcDPB
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5dea0 -n 0x20    # hRateControl
```

---

## Addendum (2026-09-14, docs/57)

Rows #23 and #25 (recon Y/UV MSB asserts) are **not reached** with
`NEED_LSB_PLANES` = 0: they sit behind Start_AVC `0xFD7D`, which gates recon
programming. #32 is resolved: Config `+0x40`/`+0x41` select `ConfigureMCPUs`.
