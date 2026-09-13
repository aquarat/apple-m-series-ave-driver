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
