# Phase 6 — one encoded frame (macOS 13.5)

`Start_AVC` is accepted and a 1280x720 fixed-QP I-only AVC session is open
([31](31-bringup-state.md), 2026-09-13 21:13). This document is the analysis
behind the `Process` command that should produce the first bitstream, the wire
layout it sends, **how the host learns where the frame is and how long it is**,
the operator procedure, and an honest list of what will probably break.

Nothing here has run on hardware.

All firmware VAs are 13.5 image VAs (`__TEXT` VA 0 = file `0x4000`); kext VAs
are 13.5 kernelcache VAs. Reproduce any line with

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x5be0c -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ec4e38 -n 0x200
```

**The `AVE_MACOS=13.5` is not optional.** Without it `disas.py` reads the
26.6.2 blobs and every address in this document is wrong.

Each claim below is marked **C** (read out of the disassembly, VA cited),
**I** (inferred), or **U** (unknown).

---

## 0. The short version

- The encoded frame lands at the **start of the coded buffer** whose address
  `Process` carries at `PICMGMT + 0xC08` — which must be byte-for-byte the
  address published at `Start_AVC` for the same index, or the firmware asserts.
- **The completion carries no length.** The `ENCODE_DONE` message is `0x48`
  bytes instead of `0x40`; its extra word at `+0x40` is the *slice number*, not
  a byte count.
- **The length comes out of the coded-header buffer**, as
  `sum(slice[i].ui32BytesWritten) - sum(slice[i].ui32BytesToRemove…)` over the
  slice records until the first one with a zero byte count. That is exactly
  what Apple's `AVE_RetrieveRCStats` does.
- **The bitstream is slice NALs only.** SPS and PPS were written once into the
  parameter-sets buffer at `Start_AVC`; to get a decodable `.h264` you
  concatenate `paramsets || coded`. Everything is **Annex B with 4-byte start
  codes** — `WriteBits::nal_header` writes `00 00 00 01` itself.
- The first `Process` needs **four buffer tables the accepted `Start_AVC` left
  zero** (`SrcNeighbor*`). Start does not check them; `setPipe` and
  `SetTranscode` assert on them.

---

## 1. `Process_AVC` on 13.5 — the container

| what | value | evidence | conf |
|---|---|---|---|
| command id | 7 (`CAVE_CMD_AVC_ENCODE`) | fw jump table `cmp w22,#0x1940` `0xda9c` | C |
| command size | `0x1940` | same; kext bzero `mov w2,#0x1940` `0xfffffe0008eac878` | C |
| header | the common `0x40` header ([46](46-abi-13.5-commands-session.md) §2) | — | C |
| `+0x40`, `0x984` bytes | a copy of an `AVC_Slice` object (the slice map) | kext `0xfffffe0008eac9a4`–`9b8` | C (that it is copied), **U** (contents) |
| `+0x9C8`, `0xF68` bytes | `AVE_PICMGMT_PARAMS`, with its own size in its first word | kext `0xfffffe0008eac99c`/`9bc`–`9c8`; fw `add x27,x21,#0x9c8` `0x145c8` | C |
| `+0x1930`, `0x10` | not written by Apple's builder; `StartPipe` passes `&cmd[0x1930]` | fw `0x49314`–`1c` | U |
| slot | caller's, `< 41`; Apple's client uses 21..40 | kext `0xfffffe0008eac860`, `0xfffffe0008f0166c` | C |
| reply | id `0x0E06`, `0x48` bytes | fw `NotificationToHost` `0x13784`–`0x137d0` | C |

The firmware copies the **whole** `0x1940` (`memcpy(pool, cmd, 0x1940)`, fw
`0xf04c`–`0xf05c`) — there is no 13.5 counterpart of 26.6.2's "only four
PICMGMT slices reach the encoder" rule.

`driver/ave_cmd.c` leaves `+0x40` zero. See §8.

---

## 2. The fields the driver sets, and why

Offsets are into `AVE_PICMGMT_PARAMS`; add `0x9C8` for the command offset. All
of these are in `ave_cmd_abi_13_5.process_avc` with their evidence VAs.

### 2.1 Input surface

| off | field | value | evidence | conf |
|---|---|---|---|---|
| `0x6F3` | `bInputCompressed` | 0 | kext `0xfffffe0008eabb98`; fw `setPipe` `0x54854` | C |
| `0x8C0` | `sInput.Y` | luma IOVA | fw asserts `!= 0` (`0x5428c`, setPipe:6440) and `& 63 == 0` (`0x54244`, :6441) | C |
| `0x8C8` | `sInput.Y` stride | `ALIGN(coded_width, 64)` | kext `0xfffffe0008eb0908`, `& 0x3f` check `0xeb0768` | C |
| `0x8D0` | `sInput.UV` | chroma IOVA | fw `0x546fc` (:6454), `0x54354` (:6455) | C |
| `0x8D8` | `sInput.UV` stride | same as luma | kext `0xfffffe0008eb0914` | C |

13.5 has **no** per-plane size fields (`in_luma_size` / `in_chroma_size` are
`AVE_OFF_NONE`): the complete set of stores in `AVE_CHM_SetDataInfo_FwBuf`
(`0xeb04bc`–`0xeb2520`) writes none. ([47](47-abi-13.5-frame-rc-surfaces.md) §1.2.)

**Surface layout for 8-bit 4:2:0 (NV12), two planes:**

- luma: `stride x 16*ceil(H/16)` bytes. The hardware fetches
  `16*ceil(H/16)` rows *regardless of the declared height*
  ([38](38-dimension-convention.md) §5), so an allocation sized to the display
  height runs off the end of the DART mapping. The driver allocates
  `ave_src_luma_rows()` rows.
- chroma: interleaved Cb/Cr, `stride x 8*ceil(H/16)` bytes, **same stride as
  luma** — the two interleaved components cancel the horizontal decimation
  ([39](39-input-format.md) §1.2).
- stride: non-zero and a multiple of 64, enforced on both sides.
- both plane addresses: non-zero and 64-byte aligned, asserted by the firmware.
- plane alignment beyond 64 is not required; `dma_alloc_coherent` gives page
  alignment anyway.

For the default 1280x720 session: stride 1280, luma `1280*720 = 0xE1000`,
chroma `1280*360 = 0x70800`.

### 2.2 Output

| off | field | value | evidence | conf |
|---|---|---|---|---|
| `0xC00` | `sOutput` mode byte | **0** | kext `strb wzr,[x3,#3072]` `0xeb051c`; fw `ldrb [x21,#3072]` `0x58350` | C |
| `0xC04` | buffer index | 0 | kext `0xeb0520`; fw `0x5831c` | C |
| `0xC08` | `sOutput.Coded` | the Start-time coded address | kext `0xeb0548`; fw `0x58320` | C |
| `0xC10` | `sOutput.coded_dataHeader` | the Start-time coded-header address | kext `0xeb05a0`; fw `PipePrepareParam` `0x48104` | C |
| `0xC18` | `sOutput.CodedBufSize` | the coded buffer size | kext `0xeb0554`; fw `0x58364` | C |

The mode byte picks which of two checks `ProcessTranscodeStart` runs
(fw `0x58350`):

- **mode == 0** (what we send, and what the kext sends): the firmware asserts
  `pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]`
  (fw `0x58404`, line 2649, string `0xc4a07`) and takes the buffer *size* from
  the table published at `Start_AVC` (`ldr w9,[x8,#3016]` `0x5838c`), ignoring
  `+0xC18`.
- mode != 0: it instead requires `CodedBufSize > 3*W*H/4` (fw `0x583bc`,
  line 2655).

So the coded address must be repeated exactly. `ave_session.c` keeps the
`Start_AVC` addresses in `struct ave_sess_bufs` for that reason.

### 2.3 Frame type and context

| off | field | value | evidence | conf |
|---|---|---|---|---|
| `0xCAC` | `FrameType` | 3 = IDR (default) or 0 = I | kext `0xfffffe0008eaaa50`; fw `cmp w28,#5` `0x145d4`, slice-type table `0x20d44` | C |
| `0xCB0` | context index | 0 | kext `0xfffffe0008eaaa58`; fw `ldr w27,[x23,#3248]` `0x23d14`, `strb w12,[x22]` `0x5823c` | C (index), I (name) |
| `0x038` | `forceKeyFrame` | 1 for IDR | kext `0xfffffe0008eab8d8`–`8e4`; fw `GetFrameType` `ldr w10,[x23,#56]` `0x23cf0` | C |
| `0x03C` | `forceNonRefFrame` | 0 | kext `ldrb [x19,#60]` `0xfffffe0008eac0f8` | C |
| `0x6F1` | `bUpdateParameterSets` | 0 | kext log `0xfffffe00071e51d6` | C |
| `0x6F4` | `scalingMatrixMode` | 0 | same log | C |

`FrameType = 3` is used as given: `SendCommandToQueue` only calls
`GetFrameType` when the host value is 5 or 9 (fw `0x145dc`–`0x14600`), and the
slice-header writer maps 3 to `slice_type = 2`, `IdrPicFlag = 1`, `frame_num =
0` (fw `0x210e8`–`0x210f8`).

### 2.4 Reconstruction planes

The encoder always reconstructs, even for a single I-frame.

| off | field | assert | evidence |
|---|---|---|---|
| `0x898` | `sRecon.Y_MSB` | `!= 0`, `& 127 == 0` | fw `0x55358` / `0x55310` (setPipe:6791/6792) |
| `0x8A0` | `sRecon.Y_LSB` | `!= 0`, `& 127 == 0`, **gated** | fw `0x550e4` / `0x54f94` (:6777/6778); gate `ldrb w8,[x24,#1312]; cbz` `0x54f7c` |
| `0x8A8` | `sRecon.UV_MSB` | `!= 0`, `& 127 == 0` | fw `0x580b0` / `0x58068` (:6818/6819) |
| `0x8B0` | `sRecon.UV_LSB` | `!= 0`, `& 127 == 0`, **gated** | fw `0x55978` / `0x5541c` (:6805/6806); gate `ldr w12,[x19,#2692]; cbz` `0x55404` |
| `0x8B8` | colocated MV store | written by the firmware from the DPB too | fw `setRefPointers` `str x17,[x1,#2232]` `0x2c4b0` |

The two LSB planes only matter for the 10-bit split-plane layout, which we
never select — but the loads sit behind a gate with no null check, so the
driver fills all four out of one 128-byte-aligned arena. It costs one
allocation and removes two ways to assert. **C** for the offsets and asserts;
**I** that filling them is harmless.

`encoder_addr_dst_colo` (setPipe:6889/6890, fw `0x55554`/`0x55504`) is **not** a
risk: the code path is skipped entirely when the address is zero
(`cbz x10, 0x5554c` at `0x554f0`). **C.**

### 2.5 The `SrcNeighbor` tables — new, and required

This is the part `Start_AVC` got away with and `Process` will not.

`CAVCController::setPipe` asserts, unconditionally on the path a normal frame
takes:

```
0x55618  setPipe:6990  EncCommParams.encoder_addr_src_nbr_info   != 0
0x555d0  setPipe:6991  (… & 63) == 0
0x556b8  setPipe:6993  EncCommParams.encoder_addr_src_nbr_pixels != 0
0x55670  setPipe:6994  (… & 63) == 0
```

and `CAVCController::SetTranscode`:

```
0x5869c  SetTranscode:7928  EncCommParams.encoder_addr_src_nbr_data != 0
0x58654  SetTranscode:7929  (… & 63) == 0
```

Where those three controller fields come from:

| controller field | set at | from | evidence | conf |
|---|---|---|---|---|
| `[ctrl+4544]` `_src_nbr_info` | `InitEncodingParameters` | `[x23+16]` = **`Start_AVC` wire `0xF7D0`** | fw `0x5d88c`; `x23 = cmd+0xF7C0` ([52](52-start-avc-assert.md) §2.1) | C |
| `[ctrl+4560]` `_src_nbr_pixels` | `InitEncodingParameters` | `[x23+48]` = **wire `0xF7F0`** | fw `0x5d89c` | C |
| `[ctrl+4576]` `_src_nbr_data` | `InitEncodingParameters`, then **overwritten per frame** | `VP+0xF7B0+8k` = wire `0xF810`, then `PICMGMT + 0x9C0 + 8k` | fw `0x5d8bc`; `ProcessTranscodeStart` `ldr x8,[x9,#2496]` `0x583ac` → `str x8,[x19,#4576]` `0x583b4` | C |

Apple's host fills them in both commands:

- `AVE_CHM_SetFwBuf` writes four `u64` at `VP+0xF770`, `0xF790`, `0xF7B0`,
  `0xF7D0` (wire `0xF7D0`, `0xF7F0`, `0xF810`, `0xF830`) — kext
  `0xfffffe0008eaf174`, `…f1b0`, `…f1ec`, `…f2c8`, each loop `cmp x27,#0x4`.
- `AVE_CHM_SetDataInfo_FwBuf` writes four `u64` at `PICMGMT + 0x980`, `0x9A0`,
  `0x9C0`, `0x9E0` — kext `0xfffffe0008eb0be8`, `…0c20`, `…0c58`, `…0c90`, each
  `cmp x22,#0x4`.

**Confirmed:** the third group in each set (`0xF810` / `0x9C0`) is
`SrcNeighborData`, because the firmware loads exactly those and asserts on the
result. **Inferred:** that the first, second and fourth groups are
`SrcNeighbor{Info,Pixel,FwData}` in that order — the mapping comes from the
identical shape on 26.6.2, where the four `0x20`-apart `u64[4]` tables at
PICMGMT `0x4670/0x4690/0x46B0/0x46D0` already carry those names
([20](20-command-structs.md), [32](32-picmgmt-params.md)).

**Unknown: how big each buffer must be.** Nothing in either binary sizes them;
the kext takes them from pre-allocated `AVE_Surface`s whose InfoSet entry we
have not decoded, and the 13.5 `_S_AVE_SurfaceInfoSet` is a different shape
from 26.6.2's ([46](46-abi-13.5-commands-session.md) §10.3). The driver
allocates 16 slots of `session_nbr_kb` KiB (default 256 KiB, 4 MiB total) out
of one coherent arena. If the first run faults inside one of them, raise it.

The 26.6.2 counterparts of the `Start_AVC`-side tables were **not located**;
`ave_cmd_abi_26_6.start_avc.src_nbr_set` is `AVE_OFF_NONE` and the builder
writes nothing there.

### 2.6 Loose per-frame scratch

`PICMGMT + 0x8E0 / 0x8E8 / 0x8F0 / 0x900` are four more IOVAs Apple's host
publishes (kext `0xfffffe0008eb0b10`, `…0b44`, `…0b68`, `…0b84`); a `double` at
`0x8F8` goes with them. **U** — no firmware read was matched to them. The
driver's table has the offsets and the builder can write them, but
`ave_session.c` sends `n_scratch = 0`: publishing an address whose meaning is
unknown is worse than publishing zero, because a wrong pointer turns a clean
assert into a DMA fault.

---

## 3. How the host learns the size, and where the bitstream is

**This is the crux and it is settled from the binaries.**

### 3.1 The completion does not carry it

`ENCODE_DONE` (`0x0E06`) is one of the two ids that get a `0x48` message
instead of `0x40` (`tst w8, #0x60` at `0x13784`). The extra word is written at
`0x137d0`:

```
13784:  tst  w8, #0x60             ; ids 0xE06, 0xE07
13798:  mov  w2, #0x48             ; message size
137ac:  str  w20, [x22, #16]       ; client id      (arg 2)
137b0:  str  w23, [x22, #56]       ; status         (arg 3)
137b4:  ldr  w8, [x25]             ; arg4[0]
137c0:  str  w8, [x22, #28]        ;   -> +0x1C
137c4:  ldr  w8, [x25, #4]         ; arg4[1]
137d0:  str  w8, [x22, #64]        ;   -> +0x40
```

The caller is `CFlowControllerBase::ProcessEncDone` (fw `0x14c50`), which builds
that two-word block at `0x14e4c`:

```
14e28:  ldr  x8, [x21, #48]
14e2c:  ldr  w10, [x22, #12]
14e34:  ldr  w9, [x8, #28]
14e4c:  stp  w9, w10, [sp, #48]    ; <- arg4
14e70:  mov  w1, #0xe06
14e74:  bl   0x13684               ; NotificationToHost
```

and logs them with the format string at `0xbdc14`:

> `%s::%s currEncCmd %p pCmd %p PlaneNumber %d, slice # = %d`

So reply `+0x1C` is `PlaneNumber` and reply `+0x40` is the **slice number**.
Neither is a byte count. **C.**

### 3.2 `CODED_DATA_HDR`, and the length formula

The coded-header buffer (`Start_AVC` `CodedHeader` table, `0x23000` bytes per
`AVE_CalcBufSizeOfCodedHeader`, kext `0xfffffe0008ea4fb8`) is written by the
firmware. `ProcessTranscodeDone` maps it and stores three fields directly:

```
5bdd8:  ldr  w8, [x19, #4736]          ; context index
5bde4:  mov  w2, #0x22c60              ; map length
5bde8:  ldr  x1, [x26, x8, lsl #3]     ; coded-header IOVA for this context
5bdf8:  bl   0x20bc0                   ; MappedMemory(…)
5be08:  bl   0x20c00                   ; HwToTarget -> x0 = CPU pointer
5be0c:  ldr  w8, [x19, #2740]
5be18:  str  w8, [x0, #152]            ; ui32_SPSPPSHeaderBits
5be30:  str  w10, [x0, #268]           ; FrameNumberFromDriverReturned
5be3c:  str  w9,  [x0, #272]           ; FrameTypeReturned
```

`CollectDataFromCpus` writes the same three at `0x5a7d4`/`0x5a7e8`/`0x5a7f0`.

The host side names every field. `AVE_PrintCodedHeader(CODED_DATA_HDR*, …)` at
kext `0xfffffe0008eb6584` prints, with the load for each immediately before its
format string:

| offset | name | load |
|---:|---|---|
| `0x00` | `ui32_I_MbCnt[4]` | `ldr w8,[x21,x28,lsl#2]` `0xeb6728` |
| `0x10` | `ui32_P_MbCnt[4]` | `ldr w8,[x26,#16]` `0xeb681c` |
| `0x20` | `ui32_Skip_MbCnt[4]` | `ldr w8,[x26,#32]` `0xeb6918` |
| `0x70` | `ui32_B_MbCnt` | `ldr w9,[x21,#112]` `0xeb6a1c` |
| `0x98` | `ui32_SPSPPSHeaderBits` | `ldr w9,[x21,#152]` `0xeb6b24` |
| `0x10C` | `FrameNumberFromDriverReturned` | `ldr w9,[x21,#268]` `0xeb6c2c` |
| `0x110` | `FrameTypeReturned` | `ldr w9,[x21,#272]` `0xeb6d34` |
| `+0x180` of record *i* | `ui32BytesWritten` | `ldr w9,[x21,#384]` `0xeb6ef4` |
| `+0x38C` of record *i* | `ui32BytesToRemoveAtTheEndOfTheSliceForContextSwitch` | `ldrb w8,[x21,#908]` `0xeb6ef0` |

with `add x21, x21, #0x220` at `0xeb6f2c` — the per-slice record stride is
`0x220`. **C** (the three fields the firmware writes are confirmed twice, from
both sides).

`AVE_RetrieveRCStats(_E_AVE_CodecType, CODED_DATA_HDR*, S_AVE_DRC_FrameStats*)`
(kext `0xfffffe0008ec4e38`) is the consumer, and it is where the byte count
comes from:

```c
/* x20 = hdr, x19 = stats, x25 = hdr, x0 = total, x8 = trimmed */
stats->frame_type = hdr[272];                       /* 0xec4e94-98 */
for (i = 0; i < 0x100; i++) {                       /* 0xec4f00      */
    n = *(u32 *)(x25 + 384);                        /* 0xec4ec0      */
    if (n == 0) break;                              /* 0xec4ec4      */
    stats->slices++;                                /* 0xec4ecc-d0   */
    total += n;                                     /* 0xec4ed4      */
    if (codec == 1 /* HEVC */) total += *(u32 *)(x25 + 920);  /* 0xec4ee0 */
    trim = *(s8 *)(x25 + 908);                      /* 0xec4ee8      */
    if (trim < 0) { log; return 0; }                /* 0xec4eec      */
    trimmed += trim;                                /* 0xec4ef4      */
    x25 += 0x220;                                   /* 0xec4efc      */
}
stats->bytes = total - trimmed;                     /* 0xec4f08-10   */
```

**So: `frame length = Σ ui32BytesWritten − Σ ui32BytesToRemove…`, over the
slice records, stopping at the first zero byte count.** The AVC path skips the
`+920` term (it is taken only when the codec argument is 1). **C.**

`driver/ave_cmd.c:ave_cmd_coded_length()` is a transcription of that loop, with
the bounds checks the kext does not need, and
`tools/session_selftest` exercises it with negative controls.

### 3.3 Where the bytes are

`ProcessTranscodeStart` stores the coded address as
`EncCommParams.curr_bitstream_addr_dst` (`str x8,[x19,#8328]` `0x583a4`) and
`SetTranscode` programs it straight into the hardware (`0x592f0`–`0x592fc`,
with the buffer size at `0x59300`–`0x59310`). There is no offset: for the first
frame of a session the bitstream starts at **byte 0 of the coded buffer**.

The only thing that would shift it is the restart path (`ProcessTranscodeStart`
`0x58244`–`0x58264`), which rounds the address up to 64 and records the skipped
bytes in `[ctrl+8356]`. That path is gated on a per-context byte that is zero on
a fresh session (`cbz w10, 0x582f0` at `0x58240`), and the normal path
explicitly stores zero there (`str wzr,[x19,#8356]` `0x58340`). **C.**

`EncCommParams.curr_bitstream_addr_dst != 0` is asserted at `0x593d4`
(SetTranscode:8053) and there is an overflow check
`[8312] + [8316] + [8356] <= [8360]` in `ProcessTranscodeDone` (`0x5bf7c`) whose
failure logs `"bitstream size overflow, buffer size: %d, bitstream size: %d"`
(`0x5bf84`) — useful: if the coded buffer is too small, the firmware says so in
plain text rather than corrupting memory.

---

## 4. SPS/PPS, and assembling a decodable `.h264`

### 4.1 The parameter sets come from the parameter-sets buffer, at Start

`CAVCController::InitEncodingParameters`, after generating the SPS and PPS with
`SPS::seq_parameter_set_rbsp` (`0x194a0`) and `PPS::pic_parameter_set_rbsp`
(`0x1997c`):

```
5df28:  ldr  x20, [x23, #880]     ; = Start_AVC wire 0xFB30, p_ParameterSetsBuffer
5df38:  bl   0x20bc0              ; MappedMemory(paddr = that)
5df44:  bl   0x20c00              ; HwToTarget -> x0
5df48:  ldr  w8, [x24, #1196]     ; SPS length in BITS
5df5c:  bl   0x55e8               ; memcpy(mapped, ctrl+0x2c6a8, sps_bits >> 3)
5df60:  ldr  w8, [x24, #1196]
5df6c:  add  x0, x20, x8, lsr #3
5df70:  ldr  w8, [x24, #1840]     ; PPS length in BITS
5df78:  bl   0x55e8               ; memcpy(mapped + sps_bytes, ctrl+0x2c92c, pps_bits >> 3)
5df9c:  str  w8, [x19, #2740]     ; ui32_SPSPPSHeaderBits = sps_bits + pps_bits
```

So the buffer holds **SPS bytes immediately followed by PPS bytes**, written
once at `Start_AVC`. **C.**

### 4.2 They are Annex B with 4-byte start codes

`WriteBits::nal_header(unsigned, unsigned)` at `0x155c0` writes, into the bit
buffer at `obj+0x16+pos`:

```
155d4:  strb wzr, [x11, w10]   ; 0x00
155e8:  strb wzr, [x11, w10]   ; 0x00
155fc:  strb wzr, [x11, w10]   ; 0x00
15610:  mov  w13, #0x1
15618:  strb w13, [x11, w10]   ; 0x01
15628:  add  w9, w9, #0x4      ; pos += 4
```

`00 00 00 01`. **C.** The same writer produces the slice headers that go into
the coded buffer (`CAVCController::GetSliceHeaderForFW` `0x5fdd0` memcpy's the
assembled header bytes at `0x5fe8c`), so the coded buffer is Annex B too.

**Not length-prefixed.** Do not insert AVCC lengths.

### 4.3 Are the parameter sets also in the coded buffer?

**No — inferred, not confirmed.** The reasoning:

- `ui32_SPSPPSHeaderBits` in the coded header is copied verbatim from the
  controller field the *Start-time* SPS/PPS generation set (`[ctrl+2740]`), not
  from anything measured during this frame. It is rate-control accounting: on an
  IDR, `CollectDataFromTranscode` adds it into a bit counter
  (`0x5c4f8`–`0x5c508`) for the RC, guarded by the frame type being 3.
- No path was found that copies `ctrl+0x2c6a8` / `ctrl+0x2c92c` (the SPS and
  PPS bytes) into the bitstream buffer. The only reads of those addresses in the
  image are the two `Start_AVC` memcpy's above (`add x1,x9,#0x6a8` `0x5df50`,
  `add x1,x9,#0x92c` `0x5df68`; a full-image scan for `#0x6a8` finds no other
  use on the AVC encode path).
- `SetTranscode` never passes the PS bit count to the hardware.
- It matches how this is used on macOS: the parameter sets become the
  `CMVideoFormatDescription` (`avcC`), separate from the sample data.

This is trap 2 territory — an absence argument. It is marked **I** and the
driver does not bet on it: `ave_session.c` checks whether the coded buffer
already starts with a `00 00 00 01` + `nal_unit_type == 7`, prepends the
parameter sets only if it does not, and logs which branch it took.

### 4.4 How long are the parameter sets?

The firmware records the total only as a bit count in `[ctrl+2740]`, which
never reaches the host, and the `Start_AVC` reply does not carry it either.
**U.**

The driver recovers it by construction: the parameter-sets buffer is memset to
zero before `Start_AVC`, every H.264 NAL ends with the `rbsp_stop_one_bit` (so
the last byte is non-zero), and emulation prevention forbids three consecutive
zero bytes inside a NAL — so the last non-zero byte in the buffer is the last
byte of the PPS. The driver scans back from the end
(`ave_session_psets_len()`), and cross-checks the result against
`ui32_SPSPPSHeaderBits / 8` from the coded header, logging both. If they
disagree, the log says so and the raw buffer is published anyway.

### 4.5 The assembled file

```
frame.h264 = paramsets.bin || coded.bin
           = 00 00 00 01 <SPS> 00 00 00 01 <PPS> 00 00 00 01 <IDR slice>
```

which `ffmpeg -f h264` accepts directly.

---

## 5. What the driver does

All of it is behind `session_frame=1`, which implies `session_selftest`.

`ave_session_selftest()` now runs **Config → Open → Start_AVC → Process**:

1. Before `Start_AVC`, `ave_session_alloc_nbr()` carves 16 `SrcNeighbor` slots
   (4 groups x 4) of `session_nbr_kb` KiB from one coherent arena and hands the
   addresses to both commands. With `session_nbr=0` they are left zero — a
   deliberate bisect: the run should then die at `setPipe:6990`, which would
   confirm §2.5 on hardware.
2. `Start_AVC` is unchanged apart from those tables and from recording the
   coded / coded-header / parameter-sets addresses for step 4. With
   `session_frame=0` the command is byte-identical to the one the firmware
   accepted on 2026-09-13 21:13.
3. The input surface: `stride x 16*ceil(H/16)` luma plus
   `stride x 8*ceil(H/16)` chroma, filled with a horizontal luma ramp that
   steps every macroblock row and near-flat chroma with a slow horizontal Cr
   drift. Deterministic, legal, and not flat — a flat frame would compress to
   almost nothing and make "the output is 30 bytes" ambiguous.
4. `Process` is built with the fields in §2, sent on IO, and the completion is
   captured on IO_T2H by the same hook the other three commands use.
5. On acceptance the coded header is hex-dumped (first `0x40` bytes, plus the
   first slice record), decoded with `ave_cmd_coded_length()`, and the derived
   length, slice count, trim, `FrameTypeReturned`, `frame_num` and
   `ui32_SPSPPSHeaderBits` are logged. The first 64 bytes of the bitstream and
   of the parameter sets are hex-dumped.
6. The result is published under **`/sys/kernel/debug/apple_ave/`**:

   | file | contents |
   |---|---|
   | `frame.h264` | the assembled elementary stream (§4.5) — this is the one to give to ffmpeg |
   | `coded.bin` | the coded buffer truncated to the derived length |
   | `coded_hdr.bin` | the whole `0x23000` coded-header buffer, for offline decoding |
   | `paramsets.bin` | the SPS+PPS the firmware wrote at `Start_AVC` |

   The directory is created **only after a frame actually came back**, so its
   presence is itself the success signal. It is removed in
   `ave_session_release()`, before the buffers behind the blobs are freed.

Nothing is freed early. The session buffers still belong to
`ave->session_bufs` and are released by `ave_remove()` after `ave_power_off()`,
exactly as before — the firmware holds every one of these addresses.

### Integrator wiring

**None.** `ave_drv.c` already calls `ave_session_selftest()` and
`ave_session_release()`, the frame step lives inside `ave_session_selftest()`,
and `driver/Makefile` already builds `ave_session.o`. No new file, no new
symbol, no change to any file this work does not own.

---

## 6. Operator procedure

Read [AGENTS.md](../AGENTS.md) and
[24-incident-2026-09-07.md](24-incident-2026-09-07.md) first. This loads a
kernel module that talks to the video encoder; only the operator runs it.

**Fresh boot required** — `venc_sys` is not gated on module unload, so the
firmware cannot be restarted without one ([31](31-bringup-state.md)).

```sh
# 1. fresh boot, patched m1n1, overlay variant=3, as for the cmd2 run
sudo insmod test/ave-overlay.ko variant=3

# 2. the encoder, with the frame step
sudo insmod driver/apple-ave.ko \
    stop_after=16 fw_map_data=1 fw_map_text=2 \
    session_selftest=1 session_frame=1

# 3. watch (probe is asynchronous - a clean insmod return proves nothing)
sudo dmesg -w
```

Expect, in order:

```
session: Config: ... ACCEPTED, status 0xee0000
session: Open: ... ACCEPTED, status 0xee0000
session: Start_AVC: SrcNeighbor 4 entries/group at 0x... (+256 KiB each)
session: Start_AVC: ... ACCEPTED, status 0xee0000
session: Process: 1280x720 coded, stride 1280, luma ... chroma ...
session: Process: ... ACCEPTED, status 0xee0000
session: coded_hdr: 00000000: ...
session: frame: NNNN bytes in 1 slice(s) (written NNNN - trimmed 0), FrameTypeReturned 3, ...
session: coded: 00000000: 00 00 00 01 65 ...
session: frame: parameter sets NN bytes (firmware said 376 bits = 47 bytes), first NAL type 7
session: frame: /sys/kernel/debug/apple_ave/frame.h264 = NNNN bytes (SPS+PPS prepended ...)
session: self-test encoded one frame OK (0)
```

Then, **in the same boot, before unloading the module** (the blobs vanish with
it):

```sh
sudo cp /sys/kernel/debug/apple_ave/frame.h264    ~/out.h264
sudo cp /sys/kernel/debug/apple_ave/coded.bin     ~/coded.bin
sudo cp /sys/kernel/debug/apple_ave/coded_hdr.bin ~/coded_hdr.bin
sudo cp /sys/kernel/debug/apple_ave/paramsets.bin ~/paramsets.bin
sudo chown $USER: ~/out.h264 ~/coded.bin ~/coded_hdr.bin ~/paramsets.bin

# the acceptance test
ffmpeg -v error -i ~/out.h264 -f null -    # silence = pass

# and what it thinks it is
ffprobe -v error -show_streams ~/out.h264
# expect: codec_name=h264, width=1280, height=720, pix_fmt=yuv420p
# and, from the SPS: profile Constrained Baseline / Baseline, level 4.0

# eyeball the picture (should be a horizontal grey ramp)
ffmpeg -v error -i ~/out.h264 -frames:v 1 ~/frame.png
```

Also capture the kernel log the usual way (`tools/kmsg_capture.py`) into
`results/`, and copy the four blobs next to it — the coded header is the only
record of what the firmware thought it produced.

If it asserts instead, the firmware names the file and line; §7 maps the likely
ones. Capture the log and stop — do not re-`insmod` without a reboot.

### Bisect knobs

| parameter | use |
|---|---|
| `session_frame=0` | back to the known-good Config/Open/Start_AVC run |
| `session_nbr=0` | leave the `SrcNeighbor` tables zero; should assert at `setPipe:6990`, which confirms §2.5 |
| `session_nbr_kb=1024` | if a DART fault points inside the `SrcNeighbor` arena |
| `session_frame_type=0` | send I (non-IDR) instead of IDR |
| `session_width` / `session_height` | e.g. 192x96, the documented minimum, to shrink every buffer |
| `session_qp` | 0..51; a low QP makes a bigger, more obviously non-trivial frame |

---

## 7. What will probably go wrong, ranked

1. **The `AVC_Slice` object at `cmd+0x40` (`0x984` bytes) is zero.** Apple
   copies a live `AVC_Slice` there (kext `0xfffffe0008eac9a4`–`9b8`,
   built by `AVE_Client_GenerateSlicesMap` `0xfffffe0008ec12c8`) and we send
   zeros. **U** — no firmware read of that range was matched, but the firmware
   copies the whole command and `CAVCController::prepSlicHeaderForCavlc`
   asserts `uSlcHdrCnt>0 && uSlcHdrCnt<=AVE_MAX_SLICEHEADERS_WALKAROUND`
   (fw `0x60250`, line 4794), and `ConfigureMCPUs` has `len >= 4` / `0`
   asserts at `0x61a78` / `0x61a30` / `0x61adc`. If any of those fire, this is
   the first thing to reconstruct. The `Start_AVC` slice map (`iNum = 1` at
   wire `0xFDAC`) is separate and *is* sent.
2. **`SrcNeighbor` buffer sizes are guesses.** 256 KiB per slot is a number
   with no evidence behind it. Too small shows up as a DART translation fault
   at an address inside the arena (the driver logs each base, so the fault
   address identifies the group) or as corrupted output rather than an assert.
3. **`setPipe` asserts on a controller-derived address we cannot see.**
   `encoder_addr_fw_data` (`0x54c0c`, :6632), `mbAddressCPUFWData` (`0x55054`
   /`0x5500c`, :6659/:6727), `stats_DMA_addr` (`0x54bc4`, :6697) all come from
   the firmware's own sub-allocator inside `iFwClientMemAddr`. We hand it
   1 MiB (`AVE_SESS_FWCLIENTMEM_SIZE`); if that is short, the sub-allocator
   returns zero and one of these fires. Raising it is cheap.
4. **`SetTranscode` entropy/bin buffers.** `wrDmaBinAddr[...]` (`0x594c8`,
   :7992) and `encoder_addr_entropy[...]` (`0x59558`, :8020) — same origin as
   3, same fix.
5. **The frame comes back with zero coded bytes.** Either the header layout is
   right and the encode produced nothing, or `slice_stride` is wrong for this
   firmware build. `coded_hdr.bin` is published precisely so this can be
   settled offline: search it for the byte count at a plausible `0x220`
   multiple plus `0x180`.
6. **The parameter-set length scan disagrees with `ui32_SPSPPSHeaderBits/8`.**
   Then one of the two is wrong; the log prints both and `paramsets.bin` has
   the raw bytes.
7. **The bitstream already contains the SPS/PPS** (§4.3 is wrong). The driver
   detects this (`nal_unit_type == 7` at offset 0) and emits the coded buffer
   unchanged; the log line says which branch ran.
8. **`ffmpeg` decodes it but the picture is wrong.** That is a *good* failure —
   it means the command path works and only the surface layout or a SPS field
   is off. Compare the decoded PNG against the generator in
   `ave_session_fill_input()`.

---

## 8. Known unknowns

- `cmd + 0x40` (`0x984` bytes): contents. **U.**
- `cmd + 0x1930` (`0x10` bytes): `StartPipe` passes its address (fw `0x49314`).
  **U.**
- `SrcNeighbor` buffer sizes and alignment beyond 64 bytes. **U.**
- `PICMGMT + 0x8E0/0x8E8/0x8F0/0x900` and the `double` at `0x8F8`: meaning.
  **U.** Sent as zero.
- `PICMGMT + 0xCA8`: the firmware copies `+0xCA8..+0xCAF` as eight bytes
  (fw `0x23cf4`), i.e. together with `FrameType`. Apple sources it from
  `FrameInfo+52`. **U.** Sent as zero.
- `PICMGMT + 0xCD0` (64-byte stride blob) with its count at `+0xF50`, and
  `+0xF60/0xF62` (intra-refresh height/position), `+0xF64/0xF65`. **U.** Zero.
- 26.6.2's `CODED_DATA_HDR`: **not read at all.** The 26.6.2 kext has the same
  `AVE_PrintCodedHeader` format strings, so the field *names* carry over, but
  its coded-header buffer is `0xC000` — too small for 256 `0x220` records — so
  the layout must differ. `ave_cmd_abi_26_6.coded_hdr.slice_stride` is 0 and
  `ave_cmd_coded_length()` refuses rather than guessing.
- 26.6.2's `Start_AVC` `SrcNeighbor` tables: not located. **U.**
- Whether `FrameTypeReturned` for a host-supplied 3 comes back as 3. **U** —
  it is read from a controller field (`[ctrl+0xa70+idx*96+5808]`, fw
  `0x5be34`), not echoed from `PICMGMT`.

---

## 9. Reproduce the analysis

```sh
# the Process container and PICMGMT
AVE_MACOS=13.5 python3 tools/disas.py --kext 'AVE_CHM_MakeFwCmd_Process_AVC' -n 0x260
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x145c8 -n 0x40

# the completion carries no length
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x13784 -n 0x60
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x14e14 -n 0x70

# CODED_DATA_HDR: the names, then the length formula
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb6584 -n 0x9c0
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ec4e38 -n 0xe0
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x5bdd8 -n 0x70

# Annex B start codes
AVE_MACOS=13.5 python3 tools/disas.py --fw 'WriteBits10nal_header' -n 0x70

# SPS/PPS into the parameter-sets buffer
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x5df28 -n 0x80

# the SrcNeighbor asserts and where the addresses come from
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x555c0 -n 0xe0
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x5d874 -n 0x50
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eaf160 -n 0x1c0
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eb0bd0 -n 0x100
```

The full assert inventory used for §7 — every `bl 0xa56bc` site in the AVC
encode path, with its expression string and source line — is reproducible by
disassembling `0x20000`–`0x62000` and reading the four `adr` instructions and
the `mov w2, #line` that follow each call.

---

## 10. Harness

`tools/session_selftest` now also builds `Process` for both ABIs and checks
every field lands at the table offset, with negative controls for a zero input
address, a misaligned input address, a stride that is not a multiple of 64, a
zero chroma address, a recon plane aligned to 64 but not 128, a zero coded or
coded-header address, a non-I frame type, an out-of-range slot, and zero or
misaligned `SrcNeighbor` entries. It also drives `ave_cmd_coded_length()`
against a synthetic two-slice header and four negative controls (all-zero
header, negative trim, undersized buffer, absurd byte count).

```
tools/abi_selftest      698 checks, 0 failures
tools/session_selftest  141 checks, 0 failures   (was 29)
tools/ipc_selftest    32120 checks, 0 failures
```

(Superseded by §11.8: `session_selftest` is 167 checks after the
`sLowResOutput` work.)

## Review of 19b9d93 (Fable) — changes applied

Verdict was **SAFE TO RUN**; no path handed the firmware unowned memory, and
the retrieval chain (length formula, offset-0 bitstream, Annex B start codes,
parameter-set scan-back) was re-derived independently and confirmed. Applied:

1. **Completion filtering (finding 1).** The capture hook completed on the
   first IO_T2H message of any id, and the encode path has seven
   `NotificationToHost` sites - `LRME_DONE` (`0xE07`) is raised in the same
   `ProcessEncDone` as `ENCODE_DONE` (fw `0x14d38` vs `0x14e70`). A frame that
   actually succeeded could have been reported as `-EPROTO` with the real
   completion going to the restored hook. The hook now waits for the id this
   command expects and logs how many others it skipped.
2. **SrcNeighbor sizes are not unknown (finding 2).** docs/47 line 302 already
   has them from the kext (`0xea5970`, `59e8`, `5a70`, `5adc`): per macroblock
   column, Info 256, Pixel 1024, Data 56, FwData 64 bytes, 16 KiB floor. The
   driver now sizes the slot from that formula (80 KiB at 1280 wide);
   `session_nbr_kb` remains as an override. The failure mode description was
   also wrong: the 16 slots are one contiguous mapping, so an undersized slot
   **overruns into the next slot silently** - wrong output, not a DART fault.
3. **Unload under live DMA (finding 4).** After a Process timeout the core may
   still be writing, and `venc_sys` is not gated on this m1n1, so unmapping
   would give a fault storm and cost a reboot. `ave_remove()` now reads
   `CPU_STATUS` **before** dropping power - a read after gating would hang the
   fabric (docs/24) - and leaks the session buffers rather than unmapping
   under a running core.
4. **4 GiB guard (finding 5).** `SetTranscode` programs only the low 32 bits
   of the coded address and size (fw `0x592fc`/`0x59310`). Allocation now
   refuses any buffer crossing 4 GiB instead of relying on the aperture.

Left as-is, with the reasoning recorded: the Process recon pointers are
overwritten by `setRefPointers` before `setPipe` reads them (finding 3), so
they are inert either way; and the debugfs directory is a weaker success
signal than the `encoded one frame OK` log line (finding 6).

---

## 11. The first hardware `Process` — `setLRME` asserts on `sLowResOutput` (2026-09-13)

`Process` (id 7, `0x1940` bytes) was accepted by the dispatcher and reached the
AVC per-frame setup, then died in `CAVCController::setLRME`
(`results/frame1-1789333891.kmsg`, docs/31 2026-09-13 22:11). This section is
the analysis behind the fix.

### 11.1 The full assert predicate

The kmsg clamps at ~120 characters; the binary does not. The two strings are
at file `0xc9b85` and `0xc9be6` in `data/blobs/macos-13.5/ave_h13c.bin`
(image VA = file − `0x4000`), and the code that evaluates them is
`setLRME + 0xf68` (fw `0x523b4`–`0x523e0`):

```
523b4:  ldr  w14, [x19, #2700]       ; EncComm width  (coded, pixels)
523b8:  ldr  w16, [x19, #5176]       ; EncCommParams.encode_row_init
523bc:  ldr  x15, [x21, #3104]       ; pPicParams->sLowResOutput.LowResSrcLumaScaled
523c0:  lsl  w14, w14, #2            ; \
523c4:  add  w14, w14, #0xfc         ;  > lr_stride = ALIGN(4*width, 256)
523c8:  and  w14, w14, #0xffffff00   ; /
523cc:  lsr  w16, w16, #2            ; encode_row_init / 4
523d0:  mul  w16, w16, w14           ; (encode_row_init/4) * lr_stride
523d4:  adds x16, x15, x16
523d8:  b.eq 0x5242c                 ; -> ASSERT line 5782   (the one we hit)
523dc:  tst  x16, #0x3f
523e0:  b.eq 0x52474                 ; else ASSERT line 5783
```

so, in full:

```
CAVCController_H13C.cpp:5782
  (pPicParams->sLowResOutput.LowResSrcLumaScaled
   + EncCommParams.encode_row_init/4*lr_stride) != 0
CAVCController_H13C.cpp:5783
  ((pPicParams->sLowResOutput.LowResSrcLumaScaled
    + EncCommParams.encode_row_init/4*lr_stride) & 63) == 0
```

with `lr_stride = ALIGN(4 * EncComm_width, 256)`. **C** (fw `0x523b4`–`0x523e0`,
assert emitters `0x52430` line 5782 and `0x523e8` line 5783).

`encode_row_init` is `0` for a whole-frame encode (it is written only by
`ProcessPipeDone` / `ProcessPipeStatsMcore` / `ConfigureMCPUs`, fw `0x59d30`,
`0x59dc8`, `0x5ba64`, `0x615f4` — **C**), so on the first frame the predicate
reduces to *`LowResSrcLumaScaled != 0` and 64-byte aligned*. We sent zero.

`x21` is `pPicParams` (`mov x21, x2` at fw `0x51494`, and the signature is
`setLRME(CAVEControllerAvcEncodeCmd*, AVE_PICMGMT_PARAMS*, unsigned)`). **C.**

### 11.2 The `sLowResOutput` group on 13.5

Offsets are into `AVE_PICMGMT_PARAMS` (add `0x9C8` for the wire offset). The
group sits immediately after `sOutput`, which ends at `0xC18`.

| off | field | how it is read | assert | conf |
|---|---|---|---|---|
| `0xC20` | `LowResSrcLumaScaled` | `ldr x15,[x21,#3104]` fw `0x523bc` | **`!= 0` and `& 63 == 0`, unconditional** (5782 / 5783) | C |
| `0xC28` | `LowResResults[0]` | `ldr x11,[x21,#3112]` fw `0x51e84` | `cbz`-skipped; only `& 63 == 0` (line 5654) when non-zero | C |
| `0xC30` | `LowResResults[1]` | `ldr x11,[x21,#3120]` fw `0x51ed4` | same | C |
| `0xC38` | `LowResResults[2]` | `ldr x11,[x21,#3128]` fw `0x5204c` | same | C |
| `0xC40` | `LowResResults[3]` | `ldr x11,[x21,#3136]` fw `0x520a4` | same | C |

The per-reference loop reads the same array as `LowResResults[me_ref_index]`
(`add x15,x21,x9,lsl#3` + `ldr x13,[x15,#3112]`, fw `0x51a2c`/`0x51a40`), which
fixes the element stride at **8 bytes** — not 26.6.2's `{addr,size}` pairs
`0x10` apart (docs/32 §6.5). **C.**

`sRef.Low_Res_Y_L0[i]` is at `0x778 + 8i` (fw `0x519e8` for `i = 0`, look-ahead
`ldr x13,[x15,#1920]` at `0x51a7c`) and `Low_Res_Y_L1[]` follows the same shape
— matching docs/47's `0x778`. **C.**

**What each points at.** `LowResSrcLumaScaled` is the *write* target of the LRME
front end: the scaled-down copy of this frame's source luma, which becomes next
frame's `Low_Res_Y_L0[]` entry. This is the surface the kext calls **`LowResRef`**
(`AVE_Client_CalcSurfaceInfo_LRME`, kext `0xfffffe0008ec6fe4`, fills
`LowResRef`/`LowResResult`/`LowResRCResult` — docs/17 version note). **I** for
the name mapping; **C** that the size formula below is the one Apple uses for it.

**Required size.** `AVE_CalcBufSizeOfLowResRef(DevType, ClientType, CodecType,
width, height, CHROMA_FORMAT, bool)` at kext `0xfffffe0008ea560c`, AVC arm
(`CodecType == 0`, `ClientType != 2`, DevType 12 < `0x13` for t6001 — docs/17):

```
fffffe0008ea5668:  lsl  w8, w3, #2            ; 4*width
fffffe0008ea56a4:  add  w8, w8, #0xfc         ; \
fffffe0008ea56a8:  and  w8, w8, #0xffffff00   ;  > ALIGN(4*width, 256)  == lr_stride
fffffe0008ea56ac:  lsr  w10, w10, #4          ; (height + 63) >> 4
fffffe0008ea56b0:  madd w8, w8, w10, w12      ; * that, + 0x1ff
fffffe0008ea56b4:  and  w8, w8, #0xfffffe00   ; ALIGN(..., 512)
```

```
size = ALIGN( ALIGN(4*W, 256) * ((H + 63) >> 4), 512 )
```

**The stride term is byte-for-byte the firmware's own `lr_stride`**, which is
what makes this the right function and not a plausible one. At 1280x720:
`lr_stride = 5120`, 48 rows, `size = 0x3C000` (245,760 bytes). Alignment: 64
(the assert); `dma_alloc_coherent` gives page alignment anyway. **C** for the
formula, **I** that the engine writes exactly `(H+63)>>4` rows.

The caller passes the pair at client`+1936`/`+1940` as `(width, height)`
(kext `0xfffffe0008ec7074` → `0xec71e4` → `0xec72a4`/`a8`). Whether that pair is
the display or the MB-aligned size is **U**; the driver passes the MB-aligned
size, which is the larger of the two, so it cannot be short.

### 11.3 Can the low-res / LRME pass be switched off? **No.**

`setPipe` calls `setLRME(cmd = NULL, pPicParams, trigger = 0)` at fw `0x57d30`,
behind exactly two gates (fw `0x57c8c`–`0x57cac`):

```
57c90:  ldrb w9, [x9, #1204]     ; ctrl + 0x23FEC
57c94:  cbnz w9, 0x57d40         ; non-zero -> setPipe does NOT call setLRME
57ca4:  ldr  w10, [x10]          ; *(u32 *)(ctrl + 0x13A3C)
57cac:  cbz  w10, 0x57d20        ; zero -> call setLRME
```

Both are **firmware-internal state, not host fields**:

- `CAVCController::CAVCController` clears both — `strb wzr,[x10]` (`ctrl+0x23FEC`)
  at fw `0x46100` and `str wzr,[x11]` (`ctrl+0x13A3C`) at fw `0x46104`. **C.**
- The only code that computes `ctrl+0x23FEC` is the ctor, `ResetBetweenPasses`,
  `ProcessLRMEDone` and `ProcessLRMEStart`; the only code that computes
  `ctrl+0x13A3C` is the ctor, `ProcessPipeReset`, `ProcessLRMEStart` and
  `setPipe` itself (immediate scan for `add xD,xS,#0x23/0x13,lsl#12` followed by
  `#0xfec`/`#0xa3c`). No `Process` or `Start_AVC` field reaches either. **C**,
  with the caveat that an immediate scan cannot see table-driven writes
  (methodology trap 3).
- And switching the byte on would not help anyway: `ProcessLRMEStart` calls the
  same `setLRME` (fw `0x513e0`) precisely when `ctrl+0x23FEC != 0`
  (`ldrb w8,[x24]; cbz w8, 0x513e4` at fw `0x51398`). The assert moves, it does
  not go away. **C.**
- `CAVE_CMD_LRME_STANDALONE` (13.5 command id 9, docs/46 §1) is a *separate*
  submission for running LRME on its own; it does not disable the in-pipe pass.
  `pVideoParams->low_res_pipe_sync_mode` (pVideoParams`+0`, logged at fw
  `0x143d8` with `xc_pipe_sync_mode` at pVideoParams`+483`) is read in
  `CFlowControllerBase::ProcessInitStage2`, not in either gate. **C** that it is
  not one of the two gates; **U** what it does control.

The path from `0x52304` to the assert at `0x523b4` is straight-line: the only
branches in between are the `sInput.Y` asserts (lines 5719/5720) and the
`LowResResults` alignment assert (5654). **C.**

**With `num_ref_idx_l0_active_minus1 < 0` — which is what an I-frame sends — the
whole reference half of `setLRME` is skipped** (`tbnz w13,#31, 0x51ad4` at fw
`0x519e4`, and the L1 equivalent `tbnz w9,#31, 0x51cbc` at `0x51ae0`), so
`Low_Res_Y_L0/L1[]` and the `LowResResults[me_ref_index]` asserts (lines
5575/5576/5581/5582/5605/5606/5612/5613) are unreachable. The complete assert
inventory for `setLRME` is 14 sites; after this change **none** of them is
reachable for an I-only frame. **C.**

### 11.4 Decision

**Supply the buffer.** It is the only route: there is no flag (11.3), the size
is confirmed rather than guessed (11.2), it costs one 240 KiB coherent
allocation, and `LowResResults[]` can stay zero because every read of them is
`cbz`-skipped.

Implemented in `ave_abi.h` (`process_avc.low_res_src` = `0xC20` on 13.5,
`0x4F10` on 26.6.2), `ave_cmd.c` (writes it, rejects a misaligned address,
allows zero), and `ave_session.c`:

- `session_lowres` (default **on**) — `0` leaves the field zero and reproduces
  `ASSERT ... 5782` exactly. That is the negative control for this change.
- `session_lowres_kb` (default `0` = the formula) — an override, because the
  *row count* is inferred even though the formula is confirmed. The log line
  says which of the two produced the size.

`LowResResults[]` are deliberately left zero and the self-test asserts they stay
zero.

### 11.5 `Uncompress Ref is not supported` — informational, ignore it

Emitted from `CAVCController::setPipe + 0x20c4` (fw `0x54ffc`):

```
54ce4:  ldr  x8, [sp, #112]        ; ctrl + 0x13A3C
54cec:  cbz  w8, 0x54f7c
54f7c:  ldrb w8, [x24, #1312]      ; ctrl + 0x24058  ("refs are compressed")
54f80:  cbz  w8, 0x54ffc
54ffc:  adr  x0, 0xc6530           ; "Uncompress Ref is not supported"
55004:  bl   0x94920               ; plain log - NOT bl 0xa56bc + 0x949d8 + panic
55008:  b    0x54cf0               ; and execution continues
```

It is a one-argument log call with an unconditional branch back into the normal
flow — not an assert, no `AVE_Panic`. The hardware run proves it: the line
appeared *before* the line-5782 assert, i.e. `setPipe` ran on past it into
`setLRME`. **C.**

What it means: with `ctrl+0x24058` zero the firmware skips the branch that
programs the second (`_LSB`) reference/recon plane — the same gate docs/53 §2.4
already records for `sRecon.Y_LSB` (fw `0x54f7c`) — and takes the default
programming at `0x54cf0`. **We do not need a compression flag or a
differently-formatted recon buffer for the first frame:** an I-frame reads no
reference at all, so the reference format is moot, and the recon *write* format
only matters the moment a later P-frame tries to use it. **I.**

Provenance of `ctrl+0x24058` is **U** — a scan for `strb Wt,[Xn,#1312]` across
`__TEXT` finds only `CHEVCController::PipePrepareParam` writing the *neighbouring*
byte `+1313` (fw `0x65e64`), and that scan **fails its negative control**: it also
finds no writer for `ctrl+0x23FEC`, which demonstrably *is* written (via a
different base register). So "nothing writes it" is not established; only "no
host field for it was found" is. (Methodology trap 2.)

### 11.6 Reproduce

```sh
# the assert, its predicate and both emitters
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x523b4 -n 0xd0
AVE_MACOS=13.5 python3 tools/disas.py --fw 'CAVCController7setLRME' -n 0x1400

# the two gates in front of the setLRME call, and the ctor that clears them
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57c80 -n 0xc0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x460d4 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x51390 -n 0x60

# the size formula, and the call that fixes its arguments
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ea560c -n 0xd8
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ec7264 -n 0x60

# "Uncompress Ref is not supported": a log, not an assert
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54ce4 -n 0x40
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54f7c -n 0x90
```

### 11.7 Ranked: what most likely fails next

1. **`SetTranscode` / `setPipe` `SrcNeighbor` sizes.** The tables are now
   published, so the `!= 0` asserts are satisfied and the next failure mode is
   silent: the 16 slots are one contiguous mapping, so a slot that is too small
   overruns the next one rather than faulting. 13.5's kext *does* have
   `AVE_CalcBufSizeOfSrcNeighbor{Info,Pixel,Data,FwData}`
   (`0xfffffe0008ea5960` / `59d8` / `5a60` / `5acc`) — docs/47's per-MB-column
   numbers come from them, but nothing has checked the **floor and the row
   multiplier** against the 13.5 arms. Cheapest next read.
2. **Another unconditional per-frame assert further down `setPipe` /
   `SetTranscode`.** `setLRME` is clean now (11.3), but the assert inventory
   past `0x57d30` has not been walked with the L0 < 0 short-circuit applied.
   Expect a named field, which is the cheap failure.
3. **The LRME engine writing outside the scaled-luma surface.** The formula is
   Apple's, but the row count is inferred; an over-run would be a DART fault,
   loud and attributable. `session_lowres_kb` raises it without a rebuild.
4. **`Recon` layout.** The driver hands `setPipe` four flat planes out of one
   arena sized by the docs/38 §7 over-estimate. If the recon surface is tiled or
   compressed (11.5 hints the reference path expects compression), the write
   lands inside our mapping but in the wrong shape — no fault, wrong output, and
   only visible as a garbage second frame. Not a first-frame risk.
5. **No completion at all.** `ENCODE_DONE` (`0xE06`) and `LRME_DONE` (`0xE07`)
   come from the same `ProcessEncDone`; the capture hook now filters on the id,
   so a 2 s timeout with "0 other completions" again means the pipe never
   started, not that we missed it.
6. **The length formula / parameter-set assembly** (§3, §4). Lowest risk: both
   sides of the length computation were read out of the binaries, and the
   parameter-set scan-back cross-checks itself against
   `ui32_SPSPPSHeaderBits / 8` and logs a disagreement instead of hiding it.

### 11.8 Harness after this change

```
tools/abi_selftest      698 checks, 0 failures
tools/session_selftest  167 checks, 0 failures   (was 141)
tools/ipc_selftest    32120 checks, 0 failures
```

The new `session_selftest` checks: `LowResSrcLumaScaled` lands at
`PICMGMT + low_res_src` and inside both the PICMGMT block and the command; every
`LowResResults[i]` stays zero and none of them overlaps it; and the negative
controls — a `+1` address, a 32-but-not-64-aligned address, and the deliberate
zero (which must still build, because it is the `session_lowres=0` bisect and
must leave the field zero).

## 12. SrcNeighbor sizes checked against the 13.5 kext (risk 1 closed)

§11.7 ranked "the SrcNeighbor slot sizes were never checked against the 13.5
arms" first, because an undersized slot corrupts silently. Read directly
(`AVE_MACOS=13.5`):

`AVE_CalcBufSizeOfSrcNeighborInfo(_E_AVE_CodecType codec, uint w, uint h)`
(`0xfffffe0008ea5960`) branches on the **codec**, not the DevType:

```
ea5964: cmp w0,#1 ; b.eq 0x5980     -> codec 1 (HEVC): 2-D block formula
ea596c: cbnz w0,  0x59b8            -> anything else: return 0
ea5970: lsl w8,w1,#4 ; add #0xf0 ; and #0xffffff00   -> codec 0 (AVC)
ea59a8: max(w8, 0x4000)
```

So for **AVC** (13.5 codec id 0, docs/46):

| group | size | at 1280 wide |
|---|---|---|
| Info | `ALIGN(16·W, 256)` (`0xea5970`) | 20 KiB |
| Pixel | `ALIGN(64·W, 1024)` (`0xea59e8`) | 80 KiB |
| floor | `0x4000` (`0xea59a8`, `0xea5a20`) | 16 KiB |

**Confirmed:** these match docs/47 line 302's per-macroblock-column numbers
(256·mbW = 16·W, 1024·mbW = 64·W), so the driver's computed slot - the
maximum across the four groups, 80 KiB at 1280 - is right for AVC. The
two-dimensional formula the §11 analysis found (`(((H+31)>>5)+1)/2-1` by
`(W+31)>>5`, ×3, ×64 or ×256) is the **HEVC** arm and does not apply to this
session; it would demand 330 KiB for Pixel, so it matters for HEVC later.

---

## 13. The `sLowResOutput` chain, completed — it comes from `Start_AVC`, not `Process` (2026-09-13)

§11 supplied a 240 KiB 64-aligned buffer at `PICMGMT + 0xC20` and the hardware
returned the **identical** `setLRME:5782` assert, because the firmware
overwrites that field before `setLRME` reads it. This section is the rest of
the chain, read end to end, and the change that follows from it.

All VAs are 13.5 image VAs (firmware) / 13.5 kernelcache VAs (kext).

### 13.1 The chain, host field to assert

| # | who | what it does | evidence |
|---|---|---|---|
| 1 | **host** `AVE_CHM_SetFwBuf` | writes 2 sets x 17 slots of `u64` at `AVE_VIDEO_PARAMS + 0x248` — i.e. **`Start_AVC` wire `0x2A8`**, since VideoParams is `cmd+0x60` ([46](46-abi-13.5-commands-session.md) §9.1) | kext `add x24,x25,#0x9a8` `0xfffffe0008eaefc0`, `add x20,x22,#0x248` `0xeaefcc`, `str x0,[x20,x28,lsl#3]` `0xeaf004`; loop bounds `cmp x28,#0x11` `0xeaf010`, `cmp x27,#2` `0xeaf024`, `cmp x23,#2` `0xeaf034` |
| 2 | fw `CAVECommonDPB::ProvideReferenceFrames(numRefs, AVE_VIDEO_PARAMS*)` | copies it to `DPBContext + 0x10A0 + set*0x80 + slot*8` | `ldr x23,[x21,#584]` `0x2b780` (x21 = VP + set*`0x88` + slot*8), `str x23,[x20,#4256]` `0x2b788` |
| 3 | fw `CAVECommonDPB::InitPointerAndVariables(AVECommonDPBContext*)` | copies element `[set][slot]` into **DPB entry + 64** (entry = ctx + set*`0x500` + `0x680` + slot*`0x50`) | head (slot 0) `ldr x0,[x0,#4256]` `0x2bd48` -> `str x0,[x17,#1728]` `0x2bd54`; loop (slot >= 1) `ldr x6,[x1,x2]` `0x2bddc` -> `str x6,[x5,#1808]` `0x2bde8` |
| 4 | fw `H264VideoEncoderDPB::ManageDPBBuffer` | DPB entry + 64 -> `ReferenceFrameInfoData + 224` | `ldur q0,[x11,#56]` `0x2d544`, `str x10,[x22,#224]` `0x2d55c` |
| 5 | fw `CAVECommonDPB::setRefPointers` | `ReferenceFrameInfoData + 224` -> `AVE_PICMGMT_PARAMS + 0xC20` | `ldp x11,x8,[x2,#216]` `0x2c318`, `str x8,[x1,#3104]` `0x2c320` |
| 6 | fw `CAVCController::setLRME` | asserts it is non-zero (5782) and 64-aligned (5783) | `ldr x15,[x21,#3104]` `0x523bc`, §11.1 |

**C** at every step. The same pass carries the recon planes: the recon table's
first `u64` (wire `0x88 + slot*0x10`) becomes DPB entry + 48 -> RefFrameInfo +
72 -> `sRecon.Y_MSB` at `PICMGMT + 0x898` (fw `ldr x21,[x20,#40]` `0x2b75c`,
`str x21,[x19]` `0x2b764`; `ldr x10,[x11,#48]` `0x2d53c`, `str x10,[x22,#72]`
`0x2d540`; `ldr x12,[x2,#72]` `0x2c314`, `str x12,[x1,#2200]` `0x2c338`), and
the entry's second `u64` (wire `0x88 + slot*0x10 + 8`) becomes `sRecon.Y_LSB`
at `PICMGMT + 0x8A0` (`0x2d550` / `0x2c328`).

**That recon half is already confirmed on hardware.** The 2026-09-13 run got
past `setPipe`'s `sRecon.Y_MSB != 0` / `& 127 == 0` asserts (fw `0x55358` /
`0x55310`) *and* reached `setLRME`, which is only possible if `setRefPointers`
ran — and if it ran, `PICMGMT + 0xC20` was necessarily overwritten, which is
exactly why our per-frame value did not help. One mechanism, two fields: the
one we filled at Start worked, the one we left zero asserted.

### 13.2 The rest of the table, and the neighbouring ones

`AVE_VIDEO_PARAMS` (wire = block + `0x60`) carries three DPB-indexed tables
back to back, all `[2 sets][17 slots]`:

| block | wire | shape | contents | evidence |
|---:|---:|---|---|---|
| `+0x28` | `0x88` | 2 x 17 x `0x10` | recon: `{luma, luma_LSB}` | kext `0xfffffe0008eaef08`–`0xeaef58`; fw `0x2b75c`–`0x2b770` |
| `+0x248` | **`0x2A8`** | 2 x 17 x `8` | **LowResRef** (LRME scaled luma) | kext `0xeaefcc`/`0xeaf004`; fw `0x2b780`/`0x2b788` |
| `+0xF650` | `0xF6B0` | 2 x 17 x `8` | Colocated | kext `0xeaef74`–`0xeaefb8`; fw `ldr x21,[x21,x3]` `0x2b78c` (x3 = `0xf650`), `str x21,[x20,#512]` `0x2b790` |

`0x28 + 2*0x110 = 0x248` and `0x248 + 2*0x88 = 0x358` (the LowResResult table,
wire `0x3B8`) — the three are contiguous, which is the cross-check that the
stride and count readings are right. This also resolves docs/46 §12's open
"16 versus 2 x 17" question: the host writes 2 x 17 and the firmware's
`InitEncodingParameters` copy loop (`0x5d6ac`–`0x5d728`) separately takes the
**first `u64` of 16 of them** into `ctrl+3256` for `p_apsReconPictures`. Two
different consumers of the same table, not a contradiction.

### 13.3 How many slots must be valid: `max_num_ref_frames + 1`, set 0 only

- `InitEncodingParameters` calls `ProvideReferenceFrames` with
  `numRefs = SPS max_num_ref_frames` (`ldr w1,[x24,#1072]` fw `0x5dd14`, x24 =
  `cmd + 0x105AC`, so wire `0x109DC` = `MAX_REF`; `bl 0x2b530` at `0x5dd38`).
- Its slot loop runs `numRefs + 1` times (`add w14,w1,#1` `0x2b628`, unrolled
  `0x2b640`/`0x2b63c`, remainder `0x2b744`), and its set loop runs `[dpb+32]`
  times (`ldrb w10,[x0,#32]` `0x2b614`, `cmp x11,x10` `0x2b670`).
- `H264VideoEncoderDPB::H264VideoEncoderDPB` sets `[this+32] = 1`
  (`mov w8,#1` `0x2d1c0`, `strb w8,[x0,#32]` `0x2d1c4`). **Only set 0 exists.**
- Apple allocates exactly that many: `AVE_CalcBufNumOfLowResRef` (kext
  `0xfffffe0008ea55d8`) returns `n + 1` (`csinc w8,wzr,w0,eq` `0xea55f0`), and
  `AVE_CreateInternalSurfaces` (kext `0xfffffe0008f3a414`) creates one surface
  per slot at `SurfaceSet + 0x9A8`, `[type][layer][num]` with strides
  `0x110`/`0x88`/`8` — the same shape the wire table has.
  `AVE_CalcBufTypeNumOfLowResRef` (`0xea55b0`) returns 1 for the call site's
  `w0 = 0` (`0xec7264`), so only the first of the two wire sets is filled, and
  the second (block `+0xFAE0`) stays zero. **C.**
- The InfoSet slots behind those counts are `+196` type, `+204` layer, `+208`
  num, `+212` size (kext `0xec726c`, `0xec7278`, `0xec7290`, `0xec72bc`) —
  matching docs/47's `LowResRef` row (`0xC4`/`0xCC`/`0xD0`/`0xD4`).

We send `max_num_ref_frames = 1`, so **two slots**: slot 0 is the entry this
frame encodes into (`ManageDPBBuffer` reads the index at `ctx+4228`, which
`InitPointerAndVariables` zeroes — `str wzr,[x16,#4228]` `0x2bd1c`) and slot 1
is read as the "next" entry in the same call (`0x2d504`–`0x2d524`).

**Size per slot is unchanged from §11.2** — `AVE_CalcBufSizeOfLowResRef`
(`0xfffffe0008ea560c`), AVC arm `0xea56a4`–`0xea56b4`,
`ALIGN(ALIGN(4*W,256) * ((H+63)>>4), 512)` = `0x3C000` at 1280x720. The
function is per-surface, so the count multiplies it: 480 KiB for two slots.

### 13.4 What changed in the driver

`driver/ave_abi.h` — `struct ave_start_avc_layout` gains
`low_res_ref_set` / `_stride` / `_max`; 13.5 = `0x2A8` / `8` / `17`, 26.6.2 =
`AVE_OFF_NONE` (no counterpart located; its `ProvideReferenceFrames` was not
read).

`driver/ave_cmd.h`, `driver/ave_cmd.c` — `struct ave_avc_session` gains
`low_res_ref[AVE_DPB_MAX]` / `n_low_res_ref`, in the **same slot order** as
`recon[]`. The builder writes them into the new table, and refuses: a table for
an ABI that has none, a count that is not exactly `n_recon`, a count past
`low_res_ref_max`, a zero hole, and any entry that is not 64-aligned. Zero
entries (`n_low_res_ref == 0`) is allowed and writes nothing — that is the
control.

`driver/ave_session.c`:

- `ave_session_alloc_dpb()`, called before `Start_AVC` alongside
  `ave_session_alloc_nbr()`, carves `session_dpb` DPB slots out of **two**
  coherent arenas (one recon, one LowResRef) so the mapping count does not grow
  with the slot count, and checks the 128-/64-byte alignment rather than
  assuming it.
- **`session_dpb` (default 2)** — slots published in both tables. The number is
  derived (`max_num_ref_frames + 1`, §13.3), not guessed. `session_dpb=1`
  reproduces the recon table of the `Start_AVC` the firmware accepted on
  2026-09-13 21:13.
- **`session_lowres=0` still reproduces the assert**, and now means it: it
  leaves the Start-time table *and* the per-frame field zero. With only the
  per-frame field zeroed the run would no longer reproduce anything, because
  the per-frame field is inert.
- The per-frame `PICMGMT + 0xC20` is still written, from slot 0, and the log
  line now says it is inert and names the instruction that overwrites it.
- The old `ave_session_alloc_lowres()` is gone; `ave_session_lowres_size()`
  stays and is used per slot. `session_lowres_kb` still overrides the per-slot
  size, because the row count in the formula is inferred.

Buffers still belong to `ave->session_bufs` and are released by `ave_remove()`
after `ave_power_off()`; the 4 GiB allocation guard is unchanged and now covers
the two arenas.

### 13.5 Harness

```
tools/abi_selftest      698 checks, 0 failures
tools/session_selftest  207 checks, 0 failures   (was 167)
tools/ipc_selftest    32120 checks, 0 failures
```

New `session_selftest` checks: every recon slot lands at
`recon_set + i*recon_stride` and inside the command; every LowResRef slot lands
at `low_res_ref_set + i*low_res_ref_stride`, inside the command, and does not
overlap the recon table; the stride is a `u64` and the table is at least
`session_dpb` slots deep; the slot one past the last published one is still
zero; and the negative controls — a 32- but not 64-aligned entry, a zero hole
in the middle, fewer entries than DPB slots, more entries than the table holds,
a table offered to the 26.6.2 ABI (which has none), and the deliberate
`n_low_res_ref = 0` control, which must build **and** leave all 17 slots zero.

Both halves were validated against a negative control ([00](00-methodology.md)
trap 2): deleting the builder's write makes exactly the two "not at wire
`0x2a8`/`0x2b0`" checks fail, and disabling the validation makes exactly the
five rejection checks fail.

### 13.6 Reproduce

```sh
# host: the LowResRef table, and the two tables around it
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eaefbc -n 0x70
AVE_MACOS=13.5 python3 tools/disas.py --kext 'AVE_CHM_SetFwBuf' -n 0x400

# firmware: VideoParams -> DPB context -> DPB entry -> PICMGMT
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2b614 -n 0x1a0   # ProvideReferenceFrames
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2bcb0 -n 0x18c   # InitPointerAndVariables
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2d4d8 -n 0x90    # ManageDPBBuffer
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2c314 -n 0x40    # setRefPointers

# how many slots: numRefs from the SPS, one set only
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x5dd0c -n 0x30
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2d1a8 -n 0x28

# Apple's own count and size for the same surface
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ea55b0 -n 0x5c
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ea560c -n 0xd4
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008f3a408 -n 0x94
```

### 13.7 Ranked: what most likely fails next

1. **Another unconditional per-frame assert further down `setPipe` /
   `SetTranscode`.** `setLRME` is now clean for an I-only frame (§11.3 plus the
   buffer), so the next stop is the assert inventory past fw `0x57d30` walked
   with the `num_ref_idx_l0_active_minus1 < 0` short-circuit applied. Expect a
   named controller field — the cheap failure.
2. **A DPB field we still send zero.** `ManageDPBBuffer` also fills
   `ReferenceFrameInfoData + 8 / +152 / +232` from the *next* slot
   (fw `0x2d510`–`0x2d524`); with `session_dpb=2` those are now real addresses,
   but `RefFrameInfo + 296 / +360` are filled from the LowResResult and
   LowResRCResult tables (wire `0x3B8` / `0x438`, fw `0x2b870`–`0x2b884`) which
   we leave zero. That copy is gated on the DPB constructor's 8th argument
   (`[dpb+44]`, `strb w7,[x0,#44]` fw `0x2d1b8`) and, for `FrameType = 3`, on
   the `cmp w9,#3` branch at fw `0x2d4dc` taking the *IDR* path that does not
   read them. **`session_frame_type=0` (non-IDR I) takes the other branch
   (fw `0x2d5a8`), which does read them — that path is untraced, so leave the
   default at 3.**
3. **The firmware's own sub-allocator inside `iFwClientMemAddr`**
   (`encoder_addr_fw_data`, `mbAddressCPUFWData`, `stats_DMA_addr`,
   `wrDmaBinAddr[]`, `encoder_addr_entropy[]` — §7 items 3 and 4). Unchanged
   risk; raising `AVE_SESS_FWCLIENTMEM_SIZE` is cheap.
4. **The LRME engine writing outside the scaled-luma surface.** The formula is
   Apple's and is now applied per slot, but the row count is still inferred; an
   over-run is a DART fault, loud and attributable, and the two slots are
   separate arena entries so the fault address names the slot.
   `session_lowres_kb` raises it without a rebuild.
5. **The recon surface layout.** Each slot is `cw*ch*2` and flat.
   `setRefPointers` derives `sRecon.UV_MSB` as `luma + [dpb+20]`, a
   firmware-computed offset (fw `0x2c324`, value built in the ctor at
   `0x2d14c`–`0x2d168`), so the chroma plane lands wherever the firmware thinks
   it should — inside our mapping, but at an offset we have not checked against
   the slot size. Worth computing before blaming the picture.
6. **The colocated table at wire `0xF6B0`.** Left zero. `setRefPointers`
   publishes it to `PICMGMT + 0x8B8` (fw `str x17,[x1,#2232]` `0x2c4b0`) and
   `setPipe` skips the whole block when it is zero (fw `cbz x10` `0x554f0`), so
   it is not a first-frame risk — but it is the same shape as the two tables
   this section added, and if a later assert names `colo`, the fix is the same
   three lines.
7. **Everything in §11.7 from item 5 down** (no completion, the length formula,
   the parameter-set assembly) — unchanged.

---

## 14. F3 (2026-09-14 12:20): no assert - the hardware runs, and its DMA fails

`results/f3-1789384805.kmsg`, commit `19d669d`, same-boot restart
(`core_reset=2 fw_restore_data=1 ... fw_halt=1`), `session_frame=1`, with the
docs/54 entropy table (4 x 960 KiB) and `sSVEMap.iNum = 1`.

- Config, Open, Start_AVC **ACCEPTED** (`0xee0000`); Process sent (6464 bytes) and
  **acked** - and for the first time **no assert**. docs/54's static pass was
  right: nothing left on the path asserted.
- The firmware **started the encoder hardware**: heartbeat shows `LRMEFS`,
  `LRMERC`, `Pipe`, `xcode` all `StartCount 1-0-1-0`. Its history log records
  Config / Open / Start AVC / `AVC` frame 0 type 3 (IDR).
- It then printed `Uncompress Ref is not supported` once, `AXI Error: 0x0 0x1 0x0
  0x0` x12 and `AXI Error: 0x3 0x1 0x0 0x0` x7, and from 2 s on
  `Controller Heart Beat ERROR: LRME HANG` / `PIPE HANG` every second. No
  completion; Process timed out after 2 s.
- Linux: IRQ 130 (AIC 1028, shared by both AVE DART nodes) fired ~29 000 times
  with both `apple_dart_irq` handlers returning IRQ_NONE -> `nobody cared`,
  **disabled**. apple-dart printed no translation fault.
- **Halt with a client open works** (docs/55 §6 [U] -> C): scratch 0
  `0x08042006`, `0x2e`. Load 2 recovered with reset + restore, Config accepted,
  clean halt.

**Reading (I):** the ASC CPU reaches our buffers through the CPUDART
(40d040000), the only DART in `iommus`. The encoder's own bus-master DMA does
not go through that instance. With nothing mapped where it looks, its AXI
transactions fail, the pipeline hangs, and an instance Linux does not service
holds the shared DART interrupt. Which instance - the `DART` at 40d030000, the
`SMMU` at 40d020000, or both - is docs/56's question. Linux's ISP node attaches
all its DART instances (`iommus = <&dart_isp0 0>, <&dart_isp1 0>, <&dart_isp2
0>`); overlay `variant=4` does the same for AVE.

IRQ 130 being disabled removes DART fault reporting for the rest of the boot,
so the next hardware run needs a reboot regardless.

---

## 15. F4 (2026-09-14 13:12): the datapath translates through DART1 - which had no TTBR - and the machine reset after unload

`results/f4-1789387926.kmsg`, commit `0b45961`, fresh boot, overlay
`variant=4` (both DARTs, SIDs 0 and 1), `core_reset=2 ... smmu_watch=1
session_frame=1`.

- Everything up to Process as in F3; no assert; hardware started.
- **`apple-dart 40d030000.iommu: translation fault: status 0x80000001 stream 0
  code 0x1 (NO TTBR FOR IOVA) at 0xfe000000`**, then `0xfe03c000`, `0xfe054000`,
  `0xfe070000`, ... - stepping through the **input luma buffer** (IOVA
  `0xfe000000`, `0xe1000` bytes). **C: the encoder's DMA goes through the
  `DART` instance 0x40d030000, stream 0**, as docs/56 inferred.
- On that DART stream 0 had **no valid TTBR**, although the device is attached
  to it. At the same time the CPUDART read `TCR[0] = TCR[1] = 0x80 TRANSLATE,
  TTBR[0][0] = TTBR[1][0] = 0x901c0584 VALID`, before and after the stage-7
  pulse - so apple-dart programs every SID in `iommus`, and DART1 should have
  held the same TTBR.
- AXI errors `0x3 0x1 0x0 0x0` x18. SMMU watch: baseline `+0x40 = 0x09000000`;
  on fault `+0x40 = 0x80000040`, `+0x48 = 0x300`, **`+0x50 = 0xfe000000`**
  (the same IOVA), `+0x58 = 0x180`, `+0x60 = 8`; **100 059** fault interrupts,
  IRQ 127 disabled. **C** that the SMMU raises the shared line and records the
  faulting address at `+0x50`.
- Halt worked (`0x08042006`, `0x2e`), unload completed, **then the machine reset
  within ~2 s**, before load 2's first marker. Nothing was running; cause
  **U**. Candidates: the teardown touching a datapath DART / SMMU still in
  error (runtime suspend of the newly linked DART1 reads its registers), or a
  leaf domain gating under a hung bus master.

**Open question that decides the next step:** why DART1's TTBR is invalid.
Either the stage-7 block reset wipes DART1 (only the CPUDART was ever checked
across it - R1, R4), or apple-dart never programmed it. The driver now dumps
DART1's TCR/TTBR next to the CPUDART's and **refuses to send Process unless
DART1's SID-0 TCR/TTBR match the CPUDART's** (`session_ignore_dart=1`
overrides), so a known-bad run stops before it starts the hardware.

**Next run (F5), fresh boot, no pulse, no unload:**

```sh
sudo insmod test/ave-overlay.ko variant=4
HOLD=15 tools/e3-run.sh f5 stop_after=16 fw_map_data=1 fw_map_text=2 \
    dapf_dump=1 smmu_watch=1 session_selftest=1 session_frame=1
```

No `core_reset` (a cold core is already STOPPED), so if DART1 matches at
"before Process" the pulse was the culprit; if it does not, the attach is.
`e3-run.sh` does not unload, avoiding F4's post-unload reset; reboot afterwards.

---

## 16. F5 (2026-09-14 14:21): datapath mapped, zero faults, LRME finishes, the Pipe hangs

`results/f5-1789392104.kmsg`, commit `74ba0ff`, fresh boot, overlay `variant=4`,
**no `core_reset`** (cold core), `dapf_dump=1 smmu_watch=1 session_frame=1`,
held loaded (`e3-run.sh`, no unload).

- Stage-8 dump: **DART1 (0x40d030000) SIDs 0 and 1 `TCR 0x80 TRANSLATE`, TTBR
  `0x901c2044 VALID` - identical to the CPUDART**. Before Process:
  `dart: SID0 CPUDART TCR 0x80 TTBR 0x901c2044 | DART1 TCR 0x80 TTBR 0x901c2044 ->
  MATCH`; Process sent.
- **Zero** DART translation faults, **zero** SMMU faults, **zero** AXI errors,
  no IRQ trouble.
- Firmware: `Uncompress Ref is not supported`, then from +2 s `Controller Heart
  Beat ERROR: PIPE HANG: 1, 1`, `ENC: StartCount 1-1-1-0, Idle 1-1-0-1`, per-stage
  lines only for `Pipe` and `xcode`. Compare F3/F4 (DMA unmapped): `1-0-1-0 /
  0-1-0-1`, **LRME HANG** and PIPE HANG, lines for LRMEFS, LRMERC, Pipe, xcode.
  **Mapping the datapath let the LRME stages finish; the Pipe now hangs without
  any fault.** No Process completion. What the fields encode: docs/57 (in
  progress).

**The F4 question is answered (C, by the pair):** with the pulse (F4) DART1
had no TTBR; without it (F5) DART1 matched the CPUDART exactly. The stage-7
block reset clears the datapath DART's translation and leaves the CPUDART's.
The driver now copies the CPUDART's SID 0/1 TTBRs and TCRs into DART1 right
after a pulse (`ave_dart_restore_datapath`, read back), so same-boot restarts
keep the datapath mapped; the before-Process check still guards it.

---

## 17. Corrections from docs/57 (static, 2026-09-14)

Annotations, not retractions:

- **§11.5 "we do not need a compression flag" is wrong.** Start_AVC byte
  `0xFD7D` (`NEED_LSB_PLANES`, fw `0x5d08c` -> `this+0x24058`) gates the only code
  that programs the pipe's recon writer (`0x54f90` -> `0x40D130240 =
  0x800314B1` plus the recon Y/UV addresses). With it 0 the firmware logs
  `Uncompress Ref is not supported` and never configures recon output - the
  leading explanation for F5's Pipe hang.
- **§13.1 "the recon half is confirmed on hardware" overstates it.** The recon
  MSB asserts sit behind the same flag and never ran.
- The recon table entry is `{MSB u64, LSB u64}`; the firmware derives both
  chroma planes from the two luma ones.
- Overlay `variant=3/4` power-domains are really `venc_sys, venc_pipe5,
  venc_me0, venc_pipe4, afnc4_ioa` (checked against the live DT: `0xc5` is
  `afnc4_ioa`); `venc_dma` is powered only as a parent and `venc_me1` (no
  phandle) not at all. The DTS comments mislabel `0xc2` and `0xc5`. Left as is
  for now to keep variables apart; docs/57 ranks it #3.

**Driver (untested):** `session_lsb=1` sets `0xFD7D` and publishes an LSB plane
per DPB slot (LSB at slot+0, MSB at slot+0x20000); `session_sve_ungate=1`
writes SVE `+0x38 = 0` around Process as `AVE_DPM_TuneUpPipe` does (docs/57
#2); after Process the driver reads `0x40D130240` (+`0x24c/0x25c/0x31c`) and
prints whether the recon writer was programmed.

---

## 18. F7 (2026-09-15 16:40): NEED_LSB_PLANES programs the recon writer; the Pipe still hangs

`results/f7-1789486815.kmsg`, commit `2886130`. Cold boot, overlay
`variant=4`, no pulse, no unload, `session_lsb=1`.

- Start_AVC with `NEED_LSB_PLANES=1`, slot 0 LSB `0xfe800000`, MSB `0xfe820000`:
  ACCEPTED. DART1 matched before Process.
- **`Uncompress Ref is not supported` is gone**, and after Process the recon
  writer reads **`0x40D130240 = 0x800314b1`**, `+0x24c = 0xfe820000` (MSB),
  `+0x25c = 0xfe800000` (LSB), `+0x31c = 0xfe810000` (derived by the firmware).
  docs/57 §4.3 is **C on hardware**: the flag gates recon programming, and
  it now happens.
- **Still `PIPE HANG`**, `ENC: StartCount 1-1-1-0, Idle 1-1-0-1`, no
  completion, zero DART/SMMU/AXI faults. So docs/57 #1 was a real bug but not
  the only one.

The driver was left loaded with the firmware hung; register peeks from
userspace are blocked (`CONFIG_IO_STRICT_DEVMEM=y`, regions claimed), so the
#3/#4 reads are now built into the Process timeout path (`session_diag`,
default on, read-only): VENC_DMA/PIPE4/PIPE5/ME0/ME1 power state, `0x40D110140`
(pipe done bit 2), `0x40D110128` (go bit 0), `0x40D120000/4`, scratch 7.

---

## 19. F8 (2026-09-15 16:44): SVE ungate changes nothing; ME1 is off; the pipe really did not finish

`results/f8-1789487084.kmsg`, commit `d37cbea`, cold boot, overlay
`variant=4`, no pulse, `session_lsb=1 session_sve_ungate=1`.

- SVE `+0x38 <- 0` before Process (and `<- 1` after): **no change** - same
  `PIPE HANG`, `StartCount 1-1-1-0, Idle 1-1-0-1`. docs/57 #2 ruled out (C for
  this configuration).
- Recon writer programmed as in F7 (`0x800314b1`).
- Timeout diagnostics:
  - PMGR PS **DMA `0x3ff`, PIPE4 `0x3ff`, PIPE5 `0x3ff`, ME0 `0x3ff`, ME1
    `0x300`** - every VENC sub-domain on except **ME1, off** (docs/57 #3, C on
    hardware).
  - `0x40D110140 = 0` - pipe done bit 2 **clear**: the hardware did not finish;
    not a lost interrupt (docs/57 #4 ruled out).
  - `0x40D110128 = 0` (go bit 0 clear - self-clearing or never set, **U**),
    `0x40D120000 = 0x80034045`, `0x40D120004 = 0xc0` (AXI-side, undecoded).
  - scratch 7 `0x04000003`: bit 26, the firmware's PIPE HANG flag, as docs/57
    predicted.

**Next:** power `venc_me1`. Its DT node has no phandle, so the overlay cannot
reference it; `power_me1=1` attaches a holder device to the node's genpd
provider at stage 6 (the mechanism `genpd_dev_pm_attach_by_id()` uses) and
logs ME1's PS register afterwards.

---

## 20. F9 (2026-09-15 16:50): ME1 powered, no change

`results/f9-1789487404.kmsg`, commit `ca2d391`, as F8 minus the SVE ungate,
plus `power_me1=1`.

- `me1: venc_me1 powered; PMGR PS ME1 = 0x3ff`; genpd shows the holder
  `active`; at the hang all five VENC sub-domains read `0x3ff`.
- **Identical hang**: `PIPE HANG`, `StartCount 1-1-1-0, Idle 1-1-0-1`,
  `0x40D110140 = 0` (done clear), `0x40D110128 = 0`, `0x40D120000 =
  0x80034045`, `0x40D120004 = 0xc0`, scratch 7 `0x04000003`; recon writer
  programmed; no faults.

docs/57's ranked causes #1 (fixed, not sufficient), #2, #3 and #4 are now all
tested on hardware. Remaining: #5, the MCPU path - Config asks the firmware to
create and **start** the pipe's microcontrollers, and a pipe whose MCPUs never
really run would hang exactly like this. `session_skip_mcpu=1` (Config
`bSkipMcpu = 1`, departs from macOS) is the discriminator; static analysis of
what the MCPUs need (docs/58) runs in parallel.

---

## 21. F10 (2026-09-15 17:34): AVE_DPE programmed; the pipe stalls at macroblock row 1

`results/f10-1789490043.kmsg`, commit `2f81a3a`, cold boot, overlay `variant=4`,
no pulse, `session_lsb=1 power_me1=1 dpe_tunables=1`.

- `dpe: [before] DC000 0x100 DC004 0x40 DC400 0x2900 DC4A4 0 DC5B0 0` ->
  `[after] DC000 0x101 DC004 0x1000 DC400 0x32901 DC4A4 0 DC5B0 0x1d`, 1+124+123
  tunables read back. **No change to the hang.**
- New diagnostics at the timeout (docs/58 §7.0):
  - **`0x40D12002C = 0x00010009` -> currMbRow 1.** The source reader completed
    MB row 0 and stopped at row 1 of 45.
  - Pipe-done enable `0x40D11013C = 0xf007` (bit 2 set - not masked).
  - Real AXI-error registers `0x40D124000/4`, `0x40D12C000`, `0x40D134000`: all 0.
  - MCPU run control 1 and IDs 4..10 on all seven; IMem first words
    `0x10004000 / 0x10001000 / 0x10001800` - images loaded and released.
  - MCPU host interface: MbInput `+0 0x48000, +4 0, +8 0x2201f`; IntraEst and
    CAVLC `+0 0x2000, +4 0, +8 0x1f`. MbInput shows pending bits the downstream
    stages do not.

**Reading (I):** the pipeline starts consuming the frame and stalls almost at
once, with MbInput apparently waiting on the next stage. A stall at **row 1**
is what something needed only from the second row on would produce - top
neighbours written by row 0 through the SrcNeighbor path is the obvious
candidate. The SrcNeighbor tables are published (4 entries/group, 80 KiB
slots); whether the firmware programs the neighbour DMA from them, and whether
13.5's DevType-dependent neighbour counts (docs/47: Info/Pixel count 4 only for
DevType 17/18, else 1) matter, is docs/59's question.

---

## 22. F11 (2026-09-15 18:08): the stall is inside row 0; ModeDecision and ReconLuma are stuck

`results/f11-1789492087.kmsg`, commit `2fb7c67`, as F10 plus `session_nbr_fill=1`.
Hang unchanged; docs/59's reads at the timeout:

- **MbInput produced 48, consumed 34**, lag 15, drain 0; last source event
  `0x00010008 / 0x10000000` (y 1, x 8, not last). **Consumed < 80: the encode
  chain stalled inside row 0**, around MB 34. "currMbRow 1" (unchanged,
  `0x00010009`) is the lookahead head, as docs/59 predicted. IntraEst current MB
  `0x00020001`.
- Stage host interfaces (`+0` enable, `+8` pending, `+0xc/+0x10/+0x14`):

  | stage | +0 | +8 | +0xc | +0x10 | +0x14 |
  |---|---|---|---|---|---|
  | MbInput | `0x48000` | `0x2201f` | 0 | 0 | `0x24` |
  | IntraEst | `0x2000` | `0x1f` | 0 | 0 | 0 |
  | CAVLC | `0x2000` | `0x1f` | 0 | 0 | 0 |
  | MotionEst | `0x8000` | `0x201f` | 0 | 0 | 0 |
  | **ModeDecision** | `0x2000` | **`0x201f`** | 0 | 0 | **`0x80000126`** |
  | **ReconLuma** | `0x2000` | **`0x201f`** | 0 | 0 | **`0x8000012b`** |
  | ReconChroma | `0x2000` | `0x1f` | `0x35` | `0x35` | `0x1` |

  **ModeDecision and ReconLuma have bit 13 enabled *and* pending, and bit 31 set
  in `+0x14`** (docs/59: a stuck bit 31 marks the stage holding the chain). An
  enabled pending event that is not serviced means those cores are not taking
  their per-MB interrupt - blocked or faulted (**I**). MotionEst's bit 13 is
  pending but not enabled, as expected for an I-frame.
- Neighbour DMA: readers and writers point at our buffers (Info `0xff000000`
  size `0x1400`, Pixel `0xff050000` size `0x14000`, control `0x80030001`), but
  **0 of 81 920 bytes changed in Info[0] and Pixel[0]** - the writers stored
  nothing. Consistent with neighbour output coming from the stalled stages (or
  only at row end); not necessarily a separate cause.
- No AXI, DART or SMMU faults; DPE, power, MCPU run/ID/IMem as in F10.

Next: why ModeDecision and ReconLuma stop servicing their MB interrupt
(docs/60).

---

## 23. F12 (2026-09-19 18:44): the colocated writer was the row-0 stall - 34 -> 3083 MBs

`results/f12-1789839843.kmsg`, commit `1f2f9e5`, as F11 plus `session_coloc=1`.

- Start_AVC published 2 colocated slots of `0x71000` bytes (slot 0 `0xfe100000`)
  at wire `0xF6B0`. The pipe's colocated writer came up **enabled**:
  `0x40D130380 = 0x80030001`, address `0xfe100000`, size `0x38400` - and
  **194 560 of 462 848 fill bytes were overwritten**. docs/60 #1 **confirmed**.
- **MbInput consumed 3083 of 3600** (was 34), produced 3097, last source event
  y 38 x 48; ModeDec entries 3078, ReconLuma granted 3074, CAVLC entries 3067.
  The frame now encodes almost to the end and stalls around **row 38 of 45**.
- ModeDecision and ReconLuma are again posted-and-not-granted
  (`+0x14 = 0x80000126 / 0x8000012b`).
- **The channel snapshots name the next gap.** At the timeout the four entropy
  write channels `0x1303C0`, `0x130400`, `0x130440`, `0x130480` read control
  `0x80030001` (**enabled**) with **address 0 and size 0**, while every other
  channel holds a real buffer (recon `0xfe820000`/`0xfe810000`, colocated
  `0xfe100000`, neighbours `0xff000000`/`0xff050000`).

**Why (C):** `AVE_CHM_SetDataInfo_FwBuf` fills `encoder_addr_entropy` as a
**matrix** - columns `j = 0..3` on the outside, rows `i = 0..15` inside, one
surface per entry, stopping at the first null (kext `0xfffffe0008eb0cb8..0d0c`,
entry `[i][j]` at `+0xA00 + 32i + 8j`). `PipePrepareParam` copies **all four
columns** of a row (fw `0x48800..0x4881c`). We filled column 0 only (docs/54's
`[i][transcode_buffer_id]`), so the pipe's four channels were enabled with
nothing behind them.

**Change:** the entropy table is now a matrix in the ABI, the builder and the
session: 4 rows x 4 columns, each entry its own 960 KiB buffer (16 total,
15 MiB), with the builder refusing a hole or more columns than the wire table
has (20 new harness checks).

---

## 24. F13 (2026-09-19 18:50): the entropy matrix changes nothing; the stall is deterministic at MB 3083

`results/f13-1789840219.kmsg`, commit `de4a30b`, as F12 with the entropy table
filled as the full 4 x 4 matrix (`session: entropy: 4 x 4 buffers of 960 KiB`).

**Every counter is identical to F12**: MbInput produced 3097 / consumed 3083,
last source event y 38 x 48, ModeDec 3078, ReconLuma 3074, CAVLC 3067,
colocated 194 560 of 462 848 bytes written. So the stall is deterministic and
the entropy matrix is not on this path.

**And the four channels at `0x40D1303C0 + 0x40k` still read enabled
(`0x80030001`) with address 0 and size 0**, exactly as in F12. docs/60's note
that they come from `encoder_addr_entropy` is therefore **refuted on hardware**:
filling all four columns did not change them. What programs them, and whether
"enabled with size 0" can block the stage feeding them, is docs/61's first
question.

The 4 x 4 matrix is kept: it matches what the kext does
(`AVE_CHM_SetDataInfo_FwBuf`, columns outside, rows inside) and costs only
memory.

---

## 25. The firmware named the failure: `Cveseb buffer write full!`

docs/61. `grep -c Cveseb results/f*.kmsg`: **4 in f12 and 4 in f13, 0 in every
other run** - only the two runs that got past MB 34 and actually ran CAVLC. The
string is the handler at fw `0x38ba0..0x38be0`, which W1C-acks **bit 12 of
`0x40D110140`** (`CAVEPipeISRManager.cpp`). Sibling handlers give the whole bit
map: bit 0 AXI error, bits 1/2/3/5/6/11 semaphores (bit 2 = pipe done), **bit 12
Cveseb (syntax-element buffer)**, 13 AVC XC full, 14/15 HEVC XC - and the
observed enable `0x40D11013C = 0xf007` is exactly bits 0,1,2,12,13,14,15, so
bit 12 was armed. The messages land 1.46 ms after the Process send, ~2 s before
`PIPE HANG`.

So the stall is the syntax-element buffer filling because its four drain
channels (`0x40D1303C0 + 0x40k`) are enabled with **address 0 and size 0**
(§23, §24). docs/61 §Q1: those channels are the pipe-side entropy/SEB write
DMAs (Transcode uses a different block, `0x11208C0 + 0x40i`); their control word
is written unconditionally (fw `0x55d54..0x55d6c`), the **address** comes from
`ctrl+0xEC0 + 0x20*ch + 8*j` with `j = ctrl[4740]` - `EncCommParams.
encoder_addr_entropy`, copied per frame from PICMGMT `+0xA00` by
`PipePrepareParam` (fw `0x487a4..0x48bea`) - and the **size** from `ctrl+0x10C0`,
**which no instruction in the image writes** (exhaustive immediate scan with a
positive control). CAVLC ran 3067 MBs into a staging buffer nothing drained.

**The host side is verified correct (C, offline):** building the same Process
command in userspace and dumping wire `0x1340..0x1450` shows all 16 entropy
entries at `0x13C8 + 32i + 8j` (`0xfec00000`..`0xffb00000` in the test pattern)
and the neighbour fields at `+0x9E0`, in a 6464-byte command whose declared size
is 6464. So docs/61 §Q4's "the per-frame PICMGMT block may not be reaching the
firmware" is not a host-side bug: we send the bytes. Either the firmware's
per-frame copy into `ctrl+0xEC0` does not run, or it reads a column we do not
populate - and the size at `ctrl+0x10C0` has no known source at all.

---

## 26. The entropy table belongs to Start_AVC, not Process (docs/61 §10)

`CAVECommonDPB::setRefPointers` rebuilds PICMGMT `+0x980..+0xBF8` **every frame**
from the DPB record (fw `0x2c98c..0x2ca4c`, 64 u64s from `[x9,#1024]`), so the
per-frame entropy table we filled in F13 was overwritten before `setPipe` read
it - which is exactly why F13 was byte-identical to F12. The record is filled at
**Start_AVC** by `ProvideReferenceFrames` from `VP+0xF770 + 96 + 8n`
(fw `0x2ba98..0x2bc8c`) = **wire `0xF830 + 32i + 8j`**. Same mechanism docs/60
found for the colocated pointer, over a 512-byte block.

A second thing falls out of the same listing: **SrcNeighbor group 3 (FwData) is
at wire `0xFED0`**, not adjacent to the other three. docs/59 §3 called `0xF830`
"SrcNeighborFwData"; it is row 0 of `encoder_addr_entropy`. The driver had group
3 as `AVE_OFF_NONE` and never published it - and `InitEncodingParameters`
(fw `0x5d8a0`) stores that same field to `ctrl[7896]`, the register behind
`0x40D13078C`, **which reads 0 in every run so far**. Two anomalies, one
mechanism.

Every gate on the per-frame copy was checked and is open, and the command is not
truncated (`ProcessAvcEncode` memmoves a hardcoded `0x1940` = 6464 bytes, fw
`0xf050`), consistent with §25's offline proof that the host bytes are right.

**Changes:** `start_avc.entropy_set = 0xF830` (stride 0x20/0x08, 4 rows x 4
columns) published from the same 16 buffers, and `start_avc.src_nbr_set[3] =
0xFED0`. The per-frame write stays - harmless, and correct again if a firmware
stops overwriting it. 27 new harness checks (774 total).

**`ctrl+0x10C0`, the channel size, still has no writer anywhere in the image**
(exhaustive scan with a positive control), so macOS most likely leaves it zero
too and it is not a hard length. If the address fix lands and the SEB still
fills, the fallback is a u32 `[16][4]` at Start_AVC wire `0xFA30`, immediately
after the address table - **not confirmed, not sent**.

---

## 27. F14 (2026-09-19 22:15): the address half lands; the size is still zero

`results/f14-1789852532.kmsg`, commit `2849599`, same flags as F13.

Two of the three checks from §26 passed:

- **`0x40D13078C = 0xff0f0000`** (was 0 in every earlier run). Publishing
  SrcNeighbor group 3 at wire `0xFED0` reached `ctrl[7896]`. The
  Start_AVC -> DPB record -> `setRefPointers` -> PICMGMT mechanism is
  **confirmed on hardware**, independently of the entropy fix.
- **Entropy channel `0x40D1303C0` now holds `0xfe000000`** (was 0): the table
  at wire `0xF830` is read by `setPipe`. docs/61 §10 confirmed.
- **But `+0x10` (the size) is still 0**, `Cveseb buffer write full!` still fires
  (3x), and every counter is unchanged: produced 3097, consumed 3083, ModeDec
  3078, ReconLuma 3074, CAVLC 3067. A ring with a valid base and zero length
  still cannot drain.

**The size table (inferred, now sent).** `setPipe` reads the address from
`ctrl+0xEC0 + 0x20*ch + 8*j` and the size from `ctrl+0x10C0 + 0x10*ch + 4*j`
(docs/61 Q1): a `u64[16][4]` (0x200 bytes) immediately followed by a
`u32[16][4]` (0x100). The same adjacency on the Start_AVC side puts the sizes
at `0xF830 + 0x200 = 0xFA30`, and `0xFA30 + 0x100 = 0xFB30` is exactly
`param_sets_addr` - the gap fits one `u32[16][4]` and nothing else is known to
live there. **Inferred, not confirmed:** no instruction in the firmware writes
`ctrl+0x10C0`, so the source has never been seen. `session_entropy_size=1`
(default on) writes it; `=0` is the control. 35 new harness checks (809 total),
including that `param_sets_addr` is still intact at `0xFB30`.

---

## 28. F16/F17: the pipeline completes - and encodes the wrong picture

`results/f16-1789855975.kmsg` (+ `-load1/`), `results/f17-1789857052.kmsg`
(+ `-load1/`), commits `8826a1c` / `600e365`.

**What works, end to end.** With the entropy size table at wire `0xFA30`
enabled the encode completes: **no `Cveseb buffer write full!`**, completion
**`0x0E06` status `0xee0000`**, 2709 bytes in one slice, `FrameTypeReturned 3`,
21 bytes of SPS+PPS. `ffprobe` reads `frame.h264` independently as **H.264
Baseline 1280x720 yuv420p** and ffmpeg decodes it. F17 adds the proof the pipe
ran the whole frame: MbInput produced = consumed = 3845, ModeDec 3845,
ReconLuma 3845, CAVLC 3845, `currMbRow 44` (last row), drain 1, zero
DART/SMMU/AXI faults, and the four entropy channels holding **both** address
and size `0xf0000` with non-zero progress words.

**What is wrong.** The decoded picture is **uniform luma 130**. The buffers,
compared offline:

| buffer | distinct values | content |
|---|---|---|
| `input_luma.bin` (what we wrote) | 226 | the ramp, `16,16,16,17 ... 234` |
| `recon_luma.bin` (DPB slot 0 MSB) | 2 | 8032 non-zero bytes of 262144 |

So the hardware consumed a frame's worth of macroblocks **without reading our
pixels**, and 2709 bytes is the cost of encoding nothing. The host side is not
at fault: the ramp is in the very allocation whose IOVA the Process command
publishes (`luma 0xfd300000/0xe1000`, stride 1280), written before the command
is sent.

**Apparatus note:** the in-driver "distinct values" counter used a `u8` tally
and wrapped, printing 1100 and 1025 distinct values of a *byte*. It is a
bitmap now; the offline comparison above is what the conclusion rests on.
A statistic that cannot say "impossible" is worth nothing.

**Also learned, the hard way:** `rmmod` is unsafe. The machine reset seconds
after a clean unload following F16 - core idle, no faults, unload logged
success - which is the same shape as F4's post-unload reset. Until that is
understood each experiment gets its own boot and the driver stays loaded
(`e3-run.sh`, never `halt-run.sh`).

Open: where the source-read channel (`0x40D120000 + 0x40k`) gets its base
address, and whether the input surface is a Start-time table like recon,
colocated and entropy turned out to be.

## The source path, answered statically (docs/62 §6, 2026-09-20)

The open question above is closed, and the answer is that the input is **not**
like recon, colocated and entropy. There is no Start-time source table.
`PICMGMT +0x8C0/+0x8D0` really do program the source-read DMA, every frame,
through `CAVCController::setPipe`; `setRefPointers` rebuilds only
`+0x980..+0xBF8`, safely above them, and none of `SetFwBuf`'s 24 destination
ranges is an input surface.

F17's own log already carries the proof that the programming happened:

| register | F17 read | meaning |
|---|---|---|
| `0x40D120000` | `0x80034045` | the **linear**-input config word (`...47` is the compressed arm) |
| `0x40D12002C` | `0x002c004f` | `currMbRow 44`, last MB column 79 |

`setPipe` writes that config word at fw `0x549bc`, *after* the luma address
(`0x54320`), the chroma address (`0x547dc`), the origin (`0x54818`) and both
strides - all on the same straight-line arm. Seeing it means those writes
executed. And our IOVA was `0xfd300000`: 32-bit clean, so the register, which
takes only the low 32 bits, lost nothing.

What is left on the host side is exactly two scalars, both of which we have
sent as zero since the first encode:

| wire | width | reaches | value |
|---|---|---|---|
| `0xFEC0` | u16 | `0x40D120050 = v & 3`, `0x40D1200D0 = v >> 2` | **[U]** |
| `0xFCE8` | u8 | bits 16+ of `0x40D12000C` | **[U]** |

Neither is knowable from the kext: `AVE_VIDEO_PARAMS` is a byte-for-byte
pass-through from user space, so Apple's driver never writes or validates
them. They are `session_src_mode` and `session_src_cfg`, to be swept.

Separately, `iNumViews` (wire `0xFF24`/`0xFF28`) is a field Apple's own
validator refuses to send as zero (`0 < n <= 2`, kext `0xec9078`) and we sent
zero. It is now 1 unconditionally.

### What `recon_luma.bin` actually is

Characterised by `tools/check_frame.py`, which also grades a run's bitstream
against its source and refuses to grade anything until a planted positive, a
negative and a blank control all come out right. On F17:

```
recon_luma: 262144 bytes, 2 distinct, 8032 non-zero (3.1%)
recon_luma: 256 written islands, pitch 1024, widths [(32, 216), (28, 40)]
VERDICT: BLANK  (PSNR 12.1 dB over 261120 compared bytes)
```

256 rows at a 1024-byte pitch with ~32 bytes written in each, all value 128.
The 128 is what a blank source produces (DC prediction, no residual); the
sparsity is a *layout* fact and a separate question, and `session_lsb=0` is
the cheapest way to test whether the split MSB/LSB planes explain it.

## F18 (proposed, not yet run)

One load, one variable: `session_flat_luma=200`, otherwise F17's parameters.

- decoded picture at 200 -> the source DMA does read our buffer, and the ramp
  failure is an addressing or layout problem downstream;
- decoded picture at ~130 again -> the hardware never delivered our bytes.

The same load captures the four `0x20000` register windows (added after F17,
so never yet seen) and prints its own verdict comparing `0x40D120010` with the
IOVA it just published.

Delta from F17 beyond the flat source: `iNumViews = 1` and `frameNumber = 0`
are now sent, both because Apple's driver sends them. If F18 stops completing
at all, those two are the first bisect.

## The run queue (2026-09-20)

Four static passes landed together: docs/62 (source path), docs/63 (teardown),
docs/64 (multi-frame), docs/65 (P-frames). Between them they changed what the
next few boots should ask, and in what order. One experiment per boot **until
F19 proves otherwise** - that is the point of F19.

### F18 - does the source DMA read our buffer? (one variable)

```
session_flat_luma=200      # otherwise exactly F17's parameters
```

| outcome | means | next |
|---|---|---|
| decoded picture = 200 | the DMA reads our bytes; the ramp failure is downstream addressing or layout | compare `recon_luma.bin`'s 1024-pitch layout against the expected tiling |
| decoded picture ~= 130 | the hardware never delivered our bytes | sweep `session_src_mode` (wire `0xFEC0`) and watch `0x40D120050` / `0x40D1200D0` move |

Grade it with `tools/check_frame.py results/<run>-load1`, which refuses to
grade until its planted-positive, negative and blank controls all pass.

**Then, in the same boot and only after the log and debugfs are on disk,**
attempt the new clean unload (docs/63: Stop, Close, Halt, unmap while
powered, gate last). F18's answer is already saved by then, so a reset costs
only the reboot we would have needed anyway.

### F19 - is the teardown safe?

If F18's unload survived, repeat it: three load/unload cycles in one boot,
starting with the control that has always been safe.

```
tools/halt-run.sh f19 "session_selftest=1 session_config_only=1 fw_halt=1" \
                      "session_selftest=1 session_frame=1 fw_halt=1 ..." \
                      "session_selftest=1 session_frame=1 fw_halt=1 ..."
```

Each unload should log `Stop -> UNINIT_DONE`, `Close -> STOP_DONE`,
`scratch 0 = 0x08042006`, `CPU_STATUS 0x2e STOPPED`, then the unmaps, then
`powered off (remove)`. Anything that cannot be proven quiet leaks its
buffers and keeps the power reference on purpose - the module unloads with
VENC still powered, and the next load recovers with
`core_reset=2 fw_restore_data=1`.

Discriminators that can say no: Stop times out but Close replies -> the
deferred-reply path is holding `UNINIT_DONE` because work really was in
flight. `Close` returns `0xEE0002` -> `IsClientRegistered` said no, our
Open/Close bookkeeping is wrong. Everything passes and it still resets ->
docs/63's rank 1 is wrong, and the next step is an unload that skips the
runtime-PM put entirely, separating "the gate" from "the unmap".

### F20 - two frames, IDR then P

Only once F18's picture is correct; a P frame predicted from a blank
reference would tell us nothing.

```
session_frames=2
```

docs/65's predicted first failure is an assert, not a fault, and the most
likely line is the one table nobody had filled until now:
`CAVCController_H13C.cpp:6184 LowResResults[me_ref_index] != 0`. That is
published as of this commit, so the interesting outcomes are further down
docs/65's ordered list. The quiet failure to watch for is frame 2 coming out
the same size as frame 1 with the reference channels
(`0x40D128000`, `0x40D128200`, `0x40D120F80`) still zero: that is the
firmware deriving `num_ref_idx_l0_active_minus1 = -1` and coding intra.

With four frames (`session_frames=4`) the run also answers whether
`frameNumber` reaches the firmware at the right offset and width - the coded
header's `+0x10C` should read 0,1,2,3 - and whether the frame-type enum is
right, `+0x110` reading 3,1,1,3.

## What F17's recon dump probably was (hypothesis, testable by F18)

docs/66 §5 gives the recon layout from `AVE_CalcBufSizeOfRecon`: the luma
plane is `1024 * ceil(W/32) * ceil((H+4)/32)` bytes - **1024 bytes per 32x32
tile**, which is exactly one byte per pixel, and the separate "meta" (LSB)
plane holds per-tile metadata. The recon writer is programmed with
`0x800314B1`, and the firmware refuses to run at all without
`NEED_LSB_PLANES`, logging "Uncompress Ref is not supported" - i.e. **the
reconstruction is written compressed**.

Now put that next to what `tools/check_frame.py` measured on F17's dump:

```
256 written islands, pitch 1024, widths [(32, 216), (28, 40)]
```

256 tile slots of 1024 bytes each, with ~32 bytes written at the start of
every one. That is not an empty buffer. It is the compressed payload of 256
tiles that all hold the same value - a uniform tile costing ~32 bytes.

So "the recon is flat" and "the recon is nearly empty" are the same
observation, and neither is evidence of a broken recon path: both follow from
the source being flat. **[I]**, and F18 tests it for free - if the source
fix lands, the island widths should grow with the picture's detail, and if
F18's flat-200 frame comes back at 200 the islands should stay ~32 bytes wide
while the decoded value changes.

## Where this leaves the driver (2026-09-20, all six static passes done)

docs/62 through docs/68 are in, and every actionable finding is in the code.
What they change about the plan:

**The remaining unknown is small and specific.** The source DMA is programmed
with our address, the pipe runs every macroblock, the entropy and recon paths
work, the bitstream is well-formed and decodes. Two undocumented scalars
(`0xFEC0`, `0xFCE8`) are the only host-side inputs to the source block we have
never set, and no amount of further static analysis will reveal their values -
Apple's kext passes them through from user space untouched. They have to be
swept on hardware. F18 decides whether they even matter.

**Teardown moved from a nuisance to a prerequisite.** docs/68 makes the point
sharply: a driver whose `close()` can reset the machine cannot be offered to
userspace at all, and without F19 every step of the remaining work costs a
reboot. It is now ahead of the V4L2 work, not after it.

**The interface is decided.** V4L2 stateful M2M, single-planar NV12 in /
H.264 out. Forced rather than chosen: the firmware picks the recon slot, the
reference list and the DPB rotation and never reports any of it, so userspace
cannot do what a *stateless* interface is defined by doing. Two consequences
worth knowing now:

- `ffmpeg -c:v h264_v4l2m2m` requires `-pix_fmt nv12` explicitly, or it
  refuses to open the encoder.
- ffmpeg packs the source at its own `linesize` while taking the height from
  our reported format, so the strides agree only when `width % 64 == 0` and
  `height % 16 == 0`. 1280x720 qualifies. **1080p does not**, and there is no
  correct answer for it - only a choice of which side reads out of bounds.

**Known-latent, now fixed:** the DMA mask was 42 bits while the firmware
programs only the low 32 of the coded and source addresses. The runtime check
in `ave_sess_dma_alloc()` caught it for our own allocations; an imported
dmabuf would have bypassed it.

**Open, cheap to answer, nobody has:** `dts/t6001-ave.dtsi` has no
`dma-coherent`, so the DMA API treats AVE as non-coherent. Whether the
datapath actually is has never been established, and it decides whether every
imported source frame gets a cache clean.

## F18 (2026-09-20): the source DMA is programmed with our buffer and still does not read it

One variable changed from F17: `session_flat_luma=200`, a constant luma plane
instead of the ramp.

```
session: source reader 0x40D120010 = 0xfd300000 (want 0xfd300000: OUR BUFFER)
         stride +0x14 0x500 (want 0x500) chroma +0x90 0xfd280000 (want 0xfd280000)
         fmt +0x0C 0x00001400 mode +0x50 0x0 +0xD0 0x0
session: frame 0: 2709 bytes in 1 slice(s) (written 2709 - trimmed 0),
         FrameTypeReturned 3, frame_num 0 (sent 0), SPS+PPS 168 bits, cabac_zero_words 0
session: frame 0: MB counts I 3600 P 0 skip 0 = 3600 of 3600 expected
```

Everything the host controls is right, confirmed **on the hardware** rather
than inferred:

- the source-read register holds **our** luma IOVA, our stride, our chroma
  IOVA and the expected format word;
- every macroblock is accounted for - 3600 of 3600, all intra;
- the firmware echoes the frameNumber we sent;
- the bitstream is well formed and 168 bits of SPS+PPS come back.

And the result is the same as F17's:

| | F17 source | F18 source | coded bytes | decoded luma |
|---|---|---|---:|---|
| | ramp, 226 values | constant 200 | **2709** | **130** |

**The same 2709 bytes for two completely different inputs.** That is the
discriminator, and it says the encoder did not read either of them. The
decoded value is 130 in F16, F17 and F18 alike, independent of what we put in
the buffer.

So the host side of the source path is now exhausted as an explanation: the
address is published, the register is programmed, and the fetch produces the
same thing whatever the memory holds. What remains is between that register
and the pipe:

1. the two unknown scalars - `mode +0x50` and `+0xD0` both read **0**, which
   is what wire `0xFEC0` puts there. This is the only host input left, and it
   is a sweep, not an analysis;
2. the datapath DART returning zeros for that IOVA without faulting (no
   SMMU/AXI faults were reported, which is also what a *successful* read with
   no data would look like);
3. a fetch that never starts - `SRCDMAGO 0x40D110128` reads 0 after the
   frame, though it may simply self-clear.

`tools/check_frame.py` needed a correction to grade this run: BLANK means
"the decode is flat while the source was not", which is meaningless when the
source is deliberately flat. With a flat source the question is only whether
the decode is flat at the *right value*, which PSNR answers alone. It refused
to grade until fixed, which is what it is for.

## F20a / s1 (2026-09-20): the teardown works, and one experiment per boot ends

**F20a - the full docs/63 sequence, on hardware:**

```
close: client 1 returned (Stop -> UNINIT_DONE, Close -> STOP_DONE)
halt:  scratch 0 = 0x08042006, the firmware reached its wfi
halt:  CPU_STATUS 0x0000002e STOPPED after 3 sample(s)
powered off (remove)
=== still alive 8 s after the unload ===
```

Stop (id 6, slot 7) and Close (id 12, slot 4) were both accepted with
`0xEE0000` first time - ids, slots, sizes and order all correct from static
analysis alone. Everything was unmapped **while still powered** and the
domains gated last, and the machine survived. F4 and F16 both reset within
seconds of an unload; this did not.

**Then the sweep's first load failed, and the cause was ours.** Stage 13:

```
ASC did not become idle (status 0x2e)
error -ETIMEDOUT: ASC start
```

`0x2e` has bit 1 set - STOPPED - which is exactly the state F20a's clean Halt
leaves behind. Stage 13 polls for `(CPU_STATUS & 3) == 0`, so **a cleanly
halted core can never be restarted**, and the recovery (block reset + DATA
restore) sat behind two module parameters that the sweep script passed from
its *second* value onwards, because it counted loads within the sweep rather
than within the boot.

Both are fixed: a STOPPED core is now the ordinary state of every load after
the first, so `ave_core_reset()` recognises it and recovers automatically
(`auto_recover=0` disables). `src_mode=9` was therefore never actually
tested - that run measured the harness.

**Unexplained, and recorded as such:** the machine reset roughly ten seconds
after the *failed* load's unload. Not after F20a's clean one. The difference
is that the failed probe took its own error path - `ave_power_off(ave,
"probe failed")`, `me1: venc_me1 released`, `iboot: DATA unmapped` - and that
path has **none** of the protections `ave_remove()` was just given: no
Stop/Close, no Halt, and no refusal to gate or unmap when the core cannot be
proven quiet. It gated and unmapped anyway. That is the obvious suspect and
the next thing to fix; it is also consistent with F4 and F16, which both
unloaded without a Halt.

## s2 (2026-09-20): a cold core reads STOPPED too

The sweep reset the machine **at insmod**, about 20 s after the overlay went
in, before stage 7 had logged anything. The cause was the auto-recovery added
an hour earlier.

`ave_core_reset()` treated `CPU_STATUS & AVE_ASC_ST_STOPPED` as proof that a
previous load had halted the core. It is not:

| state | CPU_STATUS | STOPPED (bit 1) |
|---|---|---|
| cold, never started (F20a's own pre-start read) | `0x2a` | **set** |
| halted by our Halt (F20a's teardown, s1-9's stage 13) | `0x2e` | set |

They differ only in bit 2, which m1n1 marks as a guess. So on the **first**
load of a fresh boot the condition was true, and the driver pulsed the block
reset where none was wanted - the same `reset_control_reset()` that hung the
fabric in experiment 7c (docs/25).

Recovery is an explicit request again (`core_reset=2 fw_restore_data=1`), and
stage 13 now names it in the error when it finds a core it cannot start. The
sweep decides from the kernel log - "has this driver started the core at all
this boot?" - which is the only source that actually knows.

Both failures in this area came from the same reflex: inferring history from
a register that does not record it. A cold core and a halted core look the
same from stage 7, and the honest answer is that the driver cannot tell.

## s3 (2026-09-20): src_mode reaches the registers and changes nothing

First real sweep data. `session_src_mode=9` on a fresh boot:

```
mode +0x50 0x1 +0xD0 0x2      coded=2709      decoded luma: 1 distinct, first 130
```

The wire-to-register mapping docs/62 derived is **confirmed on hardware**:
`0x40D120050 = v & 3` and `0x40D1200D0 = v >> 2`, exactly. And the output is
unchanged - 2709 bytes, uniform 130, the same as `src_mode=0` in F18 and the
same as the ramp in F17. So the field reaches the hardware and is not what
gates the fetch, at least at this value.

**The clean teardown is not proven safe.** The second load never started: the
machine reset in the gap before it, roughly ten seconds after an unload that
was completely clean -

```
halt: scratch 0 = 0x08042006 ... CPU_STATUS 0x0000002e STOPPED
iboot: DATA unmapped ... me1: venc_me1 released ... powered off (remove)
=== still alive 3 s after the unload ===
=== still alive 8 s after the unload ===
```

F20a's surviving unload was one data point; this contradicts it, and the
8-second check is too short to certify anything. Back to one load per boot
with no unload until the delayed reset is understood.

## The stream-id hypothesis

Blind-sweeping a 16-bit field is not a plan, and there is a better question
in the logs we already have.

Both AVE DARTs report `ENABLED_STREAMS = 0x0000ffff` - sixteen streams - and
this driver has only ever configured, restored or even *looked at* **two** of
them, SIDs 0 and 1. `ave_dart_restore_datapath()` copies those two;
`ave_dapf_dump()` printed those two.

If the source-read DMA issues its transactions under any other stream id,
its translation was never set up. A read that is dropped or returns zeros
looks exactly like every run so far: the address programmed into
`0x40D120010`, every macroblock walked, the correct MB counts, and no
pixels. It also explains the absence of faults - **no DART fault was logged
in F16, F17 or F18**, and Linux owns the datapath DART through the overlay,
so a mistranslating stream should have produced one. A *disabled* stream
need not.

The datapath DART's own error register is non-zero in every run:
`ERROR 0x0c0f0000`.

Next run (read-only): dump TCR and TTBR for all sixteen streams on both
DARTs, plus DART1's ERROR and ERROR_ADDR. One load, no unload. If some
stream other than 0/1 is enabled but not translating, that is the answer.

## F21 (2026-09-20): the fetch happens, and the output is neutral grey - not black

Read-only survey, one load, no unload.

**The stream-id hypothesis is refuted.** All sixteen streams are enabled on
both DARTs, but only SIDs 0 and 1 have any configuration, on *both*:

```
CPUDART TCR[0] = 0x80 TRANSLATE   CPUDART TCR[2..15] = 0
DART1   TCR[0] = 0x80 TRANSLATE   DART1   TCR[2..15] = 0
dart: [before Process] SID0 CPUDART TCR 0x80 TTBR 0x9003d5f4 | DART1 TCR 0x80 TTBR 0x9003d5f4 -> MATCH
```

and DART1's error register has bit 31 (`DART_T8020_ERROR_FLAG`) **clear** in
every run, so `0x0a0d0000` / `0x0c0f0000` and the address beside them are not
a latched fault at all. There is no evidence of a stream we failed to
program.

**Both source channels ran the entire picture.** At the end of the frame:

| | `+0x00` | `+0x0C` | `+0x10` | `+0x14` | `+0x2C` |
|---|---|---|---|---|---|
| luma `0x40D120000` | `80034045` | `00001400` | **`fd300000`** | **`00000500`** | `002c004f` |
| chroma `0x40D120080` | `80034055` | `00001400` | **`fd280000`** | **`00000500`** | `002c004f` |

Our IOVAs, our stride, the linear-input config word, and `0x2c004f` = MB row
44, column 79 - the last macroblock of a 45-row picture. Right after
Start_AVC the same words are zero, confirming setPipe programs them at
Process time.

**The output is exactly 128 everywhere, in both planes.**

```
luma   distinct 1  [(128, 921600)]
chroma distinct 1  [(128, 460800)]
```

This is the observation that reframes the problem. Zeroed memory would
encode to **black**; neutral 128 with no residual is what an encoder emits
when it computes **no difference at all**. The pixels are fetched and never
reach the subtraction.

**Two numbers that do not add up.** The pipe stages each handled 3845
entries while the coded header reports 3600 of 3600 macroblocks, and
MbInput's last source event is at **y 47** in a picture 45 macroblock rows
tall:

```
MbInput produced 3845 consumed 3845 drain 0x1 lag 0; last src event (y 47 x 72 last 0)
ModeDec entries 3845  ReconLuma granted 3845  CAVLC entries 3845
```

3845 = 3840 + 5, and 3840 = 80 x 48. Something in the pipe is running to 48
macroblock rows - three more than the picture has. That is the most
diagnostic number we have, and docs/69 is tracing it along with where the
fetched pixels are actually delivered.

## The answer, probably: IntraEst never ran, and stream 15 was never attached

Two facts, one from our own log and one from our own device tree, that had
both been sitting in front of us.

**1. The intra estimator never processed a macroblock.** F21, already
captured:

| MCPU | `+0xc` | `+0x10` | `+0x14` |
|---|---:|---:|---:|
| ModeDec | `0x26` | `0x26` | `0x5` |
| ReconLuma | `0x2b` | `0x2b` | `0xa` |
| ReconChroma | `0x35` | `0x35` | `0x1` |
| **IntraEst** | **0** | **0** | **0** |

plus `IntraEst curMB = 0` after 3845 macroblocks, with its enable word
`+0 = 0x2000` showing it was armed. **An I-frame whose intra estimator never
ran is a frame of default I_16x16 DC macroblocks with no coefficients**:
uniform 128 in both planes, independent of the source, and the same byte
count for any input. That is F16, F17, F18 and F21 exactly - including why
the ramp and a constant 200 both produced 2709 bytes.

**2. The ADT declares two streams and we attach one.**
`dts/t6001-ave.dtsi` records it in its own comment:

```
/* ADT: page-size 16384, sids 32769 */      32769 = 0x8001 = streams 0 and 15
```

Every overlay to date attaches SIDs 0 and 1; `ave_dart_restore_datapath()`
mirrored a hardcoded `{ 0, 1 }`. **Nothing has ever attached stream 15**, and
F21 measured `TCR[15] = 0` on both DARTs - enabled in `ENABLED_STREAMS`,
translating nothing.

The two fit together. A source reader issuing under stream 15 has its fetches
dropped with no fault latched (a *disabled* stream need not fault, where a
mistranslating one would - and no DART fault has ever been logged); the
address generator still walks the picture, which is why `+0x2C` reaches row
44 column 79 and why every stage counts macroblocks; and the estimator is
never offered pixels, so nothing is subtracted and DC prediction stands.

This also retires my own dismissal of the stream hypothesis after F21: I
checked whether an *unconfigured stream had faulted*, found no fault, and
concluded the streams were fine. The right question was which streams the
device declares, and the answer was in our own dtsi.

### F22 (proposed): overlay variant=5

`variant=5` is `variant=4` plus stream 15 on both DARTs, and
`ave_dart_restore_datapath()` now mirrors every stream the CPUDART
translates rather than a hardcoded pair - a set derived from the hardware
cannot go stale the way `{ 0, 1 }` did.

One load, one variable, and it can say no:

- **picture changes** -> that was it;
- **picture unchanged, `TCR[15]` now reads TRANSLATE with the CPUDART's
  TTBR** -> the hypothesis is dead rather than untested, and the next
  candidates are wire `0xFECC` (SRCDMAGO bits 4+) and `0xFCE9` (bit 3),
  which docs/69 newly traced and which we have always sent as zero.

## F22 (2026-09-20): stream 15 was not it

`variant=5`, one variable. The mapping genuinely took effect:

```
DART1   TCR[15] = 0x00000080 TRANSLATE   TTBR[15][0] = 0x901a0aa8 VALID
CPUDART TCR[15] = 0x00000080 TRANSLATE   TTBR[15][0] = 0x901a0aa8 VALID
```

Both DARTs translate stream 15 with the same page table as stream 0. And
nothing changed:

```
diag MbInput ... IntraEst curMB 0x00000000
diag hif IntraEst  +0 0x00002000 +4 0 +c 0 +10 0 +14 0
frame 0: 2709 bytes ... MB counts I 3600 P 0 skip 0 = 3600 of 3600
decoded distinct 1, first 130
```

So the hypothesis is **dead rather than untested**, which is what the run was
built to be able to say. Stream 15 is now attached anyway - the ADT declares
it, so it should be - but it is not what starves IntraEst.

### F23 (proposed): the third reader channel

docs/69 traced two more host bytes that have always been zero, both landing
in SRCDMAGO (`0x40D110128`):

| wire | u8 | goes to |
|---|---|---|
| `0xFCE9` | `session_src_bit3` | SRCDMAGO bit 3, **and** the gate on whether `ProcessPipeReset` initialises a THIRD reader channel at `0x40D120100` (fw `0x4edf0`) |
| `0xFECC` | `session_src_go` | SRCDMAGO bits 4 and up |

The two reader channels we know about - luma at `0x40D120000`, chroma at
`+0x80` - are both programmed and both run to the last macroblock. There is a
third, we have never enabled it, and **we have never even dumped its
window**: the channel list stopped at `0x200FF`. It does now, along with
`0x20140`.

The stage that never runs is the intra estimator, which needs source pixels
of its own. A reader channel that is never initialised because a byte we have
always sent as zero gates it is a candidate of exactly the shape this project
has hit five times already.

`session_src_bit3=1`, one variable, everything else as F22.
