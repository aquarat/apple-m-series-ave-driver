# `AVE_PICMGMT_PARAMS` — the per-frame encode parameters

*The `0x5118`-byte block at `sCAveCmdAvcProcess + 0x12C0`. Offsets, types and
names for the per-frame controls: frame type, input surface, output buffer,
reference lists, and where the QP actually comes from.*

Every row is marked **confirmed** (read out of an instruction, VA cited),
**inferred** (a chain of reasoning over confirmed facts) or **unknown**, per
[00-methodology.md](00-methodology.md). Reproduce any line with:

```sh
python3 tools/disas.py --fw   --addr 0x85a20  -n 0x460
python3 tools/disas.py --kext --addr 0xfffffe0008b69110 -n 0x920
```

Firmware VAs in this document are **image virtual addresses**
(`__TEXT` vmaddr 0, fileoff `0x4000`). String addresses quoted from
`strings -t x data/blobs/ave_h13c.bin` are **file offsets** and are
`VA + 0x4000`; both forms are given where a string is cited.

This document supersedes [20-command-structs.md](20-command-structs.md) §4.3
("input surface, output buffer, frame type and per-frame QP: not located") and
corrects one guess in [21-buffer-publication.md](21-buffer-publication.md)
§5.2 — see §7.

---

## 0. The result that shrinks the problem

**For AVC the firmware only ever reads four sub-ranges of the `0x5118` byte
block.** `CFlowControllerBase::ProcessCmd_Process_AVC` copies the wire command
into a pooled internal command buffer *slice by slice* and then queues **that
copy**; every controller that later takes an `AVE_PICMGMT_PARAMS*` is handed
the pooled copy, so anything not in a slice is dropped before the encoder ever
sees it.

| PICMGMT range | bytes | copy VA | what lives there |
|---|---:|---|---|
| `0x0000 .. 0x03CF` | 976 | `0x2b318`–`0x2b328` (`mov w2,#0x3d0`) | picture-parameter head (feature words, bit budget, throughput mode) |
| `0x1738 .. 0x1757` or `.. 0x175F` | 32 or 40 | `0x2b33c`–`0x2b358`, width chosen by `tbnz w26,#29` at `0x2b348` | `sRCUpdateData` (+ the `_S_AVE_DistributedRCParams` header when 40) |
| `0x1760 .. 0x1760 + 8*N` | `8*N` | `0x2b364`–`0x2b374`, `N = (int32)PICMGMT[0x175C]` (`ldrsw x8,[x21,#10780]` at `0x2b35c`) | `_S_AVE_DistributedRCParams` entries |
| `0x4228 .. 0x5117` | 3824 | `0x2b378`–`0x2b388` (`mov w2,#0xef0`) | `sRef`, `sRecon`, input planes, extra buffers, `sOutput`, `sLowResOutput`, `sFrameInfo` |

**Confirmed.** `0x12C0 + 0x3D0 = 0x1690`, `0x12C0 + 0x4228 = 0x54E8`,
`0x54E8 + 0xEF0 = 0x63D8`. Independent cross-check: *every* offset confirmed
below falls inside one of these four ranges and nothing confirmed falls
outside them. The `0x6C8`-byte rate-control block the host writes at PICMGMT
`+0x3B60` (`0xfffffe0008b69930`) is **not** copied — it is HEVC-only or dead
for AVC.

---

## 1. Minimal field set for a fixed-QP, I-frame-only AVC encode

Offsets are into `AVE_PICMGMT_PARAMS`; add `0x12C0` for the offset inside
`sCAveCmdAvcProcess`. Everything here is **confirmed** unless the row says
otherwise.

```
/* --- what frame this is ------------------------------------------ */
0x4F68  u64     sFrameInfo.FrameNum          frame counter
0x4F78  int     sFrameInfo.FrameType         3 = IDR, 0 = I   (see §3)
0x4F80  int     sFrameInfo.PicOrderCntVal    POC
0x4FA4  u32     context index                indexes the fw's per-context
                                             arrays; keep 0 for one client
0x4FA8  double  frame rate (fps)

/* --- force a key frame ------------------------------------------- */
0x1738  int     sRCUpdateData.forceKeyFrame
0x174E  u8      sRCUpdateData.bInputCompressed   = 0 for a plain NV12 input
0x175C  int32   DistributedRCParams entry count  = 0 if unused

/* --- input surface ----------------------------------------------- */
0x4570  u64     cPicMgmtParams[Luma].saIBuf[Data].iAddr    64-byte aligned
0x4578  u32     ... .iSize
0x4590  u64     cPicMgmtParams[Chroma].saIBuf[Data].iAddr  64-byte aligned
0x4598  u32     ... .iSize

/* --- output bitstream -------------------------------------------- */
0x4EF8  u64     sOutput.Coded            must equal the Start-time
                                         CodedData[slot] IOVA (fw asserts)
0x4F00  u64     sOutput.coded_dataHeader CodedHeader[slot]
0x4F08  u32     sOutput.CodedBufSize     must be > (3*W*H)/4

/* --- reconstruction target (the DPB slot this frame writes) ------- */
0x4548  u64     sRecon.Y_MSB     128-byte aligned
0x4550  u64     sRecon.Y_LSB     128-byte aligned   (inferred pairing)
0x4558  u64     sRecon.UV_MSB    128-byte aligned
0x4560  u64     sRecon.UV_LSB    128-byte aligned   (inferred pairing)
0x4568  u64     sRecon/sRef colocated MV store, same DPB slot

/* --- per-frame scratch, from the Start-time pools ----------------- */
0x45F8  u64     surface IOVA from _S_AVE_CmdInfo[40]
0x4608  u64     surface IOVA from the slot-indexed pool at SurfaceSet+0x9B0
0x4618  u64     surface IOVA from _S_AVE_CmdInfo[48]
0x4670  u64     SrcNeighborInfo
0x4690  u64     SrcNeighborPixel
0x46B0  u64     SrcNeighborData
0x46D0  u64     SrcNeighborFwData
0x4F10  u64     sLowResOutput.LowResSrcLumaScaled
0x4F48  u64     sLowResOutput.LowResResults   (+0x4F50 size)
0x4F58  u64     low-res RC results            (+0x4F60 size)

/* --- reference lists: all zero for I-frame-only ------------------- */
0x4228  u64[4]  sRef.Y_L0_MSB[]
0x4248  u64[4]  sRef.Y_L0_LSB[]        (inferred)
0x4268  u64[4]  sRef.UV_L0_MSB[]
0x4288  u64[4]  sRef.UV_L0_LSB[]       (inferred)
0x42A8  u64[4]  sRef.Low_Res_Y_L0[]    (inferred)
0x4388  u64[4]  sRef.Y_L1_MSB[]
0x43A8  u64[4]  sRef.Y_L1_LSB[]        (inferred)
0x43C8  u64[4]  sRef.UV_L1_MSB[]
0x43E8  u64[4]  sRef.UV_L1_LSB[]       (inferred)
0x4408  u64[4]  sRef.Low_Res_Y_L1[]    (inferred)
0x44E8  u64     sRef.Colocated_L1[0]
```

### The QP is **not** per-frame

For constant-QP the QP comes from the **Start** command, not from
`AVE_PICMGMT_PARAMS`. `ConstantQpRateControl::processRateControl` (`0x8bd4`)
selects one of three session QPs by slice type and returns it:

```
 8c18:  ldr  w9, [x19, #36]         ; slice type of this frame
 8c1c:  mov  w10, #0x50             ;   default -> this+0x50
 8c20:  mov  w11, #0x4c             ;   type 0  -> this+0x4C
 8c28:  csel x10, x11, x10, eq
 8c2c:  cmp  w9, #0x2
 8c30:  mov  w9, #0x48              ;   type 2  -> this+0x48
 8c34:  csel x9, x9, x10, eq
 8c38:  ldr  w9, [x0, x9]
```

Those three words are the `QP[I]/QP[P]/QP[B]` triple carried in
`sCAveCmdAvcStart + 0x240/0x244/0x248` ([20-command-structs.md](20-command-structs.md)
§3.3). The same three-way selection appears again in
`AVE_H264_Update_POClsb_SliceType` (`0x50f94`), which picks
`sCommonParams + 6120 / 6124 / 6128` for frame types `{0,3}` / `{1}` / `{2,6}`
(`0x512f0`, `0x51048`, `0x51060`). **Confirmed.**

So a fixed-QP driver sets `sRC.RCMode` and `sRC.QP[3]` once at Start and only
varies `sFrameInfo.FrameType` per frame. The per-frame block carries no QP
field that was found; `sRCUpdateData` carries *rate-control updates* (bitrate
change, parameter-set refresh, force key frame), not a QP.

> **Sharpened by [37](37-start-avc-session.md).** A per-frame QP field does
> exist, at `+0x364`, inside copied slice 0 — so it does reach the firmware —
> and the AVC path simply never reads it. The conclusion above is unaffected,
> but "absent" and "accepted then silently ignored" are different failure
> modes, and only the second one looks like the hardware disobeying you.

---

## 2. Who writes the block on the host

`AVE_CHM_PrepareDataInfo(_S_AVE_CHM*, _S_AVE_CmdInfo*, _S_AVE_FrameInfo*,
_S_AVE_DPB_Set*, AVE_PICMGMT_PARAMS*)` at `0xfffffe0008b69a28` calls four
fillers, in this order (**confirmed**, `bl` sites cited):

| order | function | VA | call site | fills |
|---|---|---|---|---|
| 1 | `AVE_CHM_SetDataInfo_FwBuf` | `0xfffffe0008b71fd8` | `0xfffffe0008b69a60` | every DART address (`0x4548`…`0x5108`) |
| 2 | `AVE_CHM_SetDataInfo_Frame` | `0xfffffe0008b682bc` | `0xfffffe0008b69af4` | `sFrameInfo` (`0x4F68`…`0x50F3`) |
| 3 | `AVE_CHM_SetDataInfo_Header` | `0xfffffe0008b68858` | `0xfffffe0008b69bf4` | the SPS/PPS/slice-header block *in `_S_AVE_FrameInfo+0x30`*, **not** in PICMGMT |
| 4 | `AVE_CHM_SetDataInfo_RC` | `0xfffffe0008b69110` | `0xfffffe0008b69ce8` | `0x0000`…`0x1737` (bulk copy), `sRCUpdateData`, `DistributedRCParams` |

`AVE_CHM_MakeFwCmd_Process_AVC` then `memcpy`s the finished block to
`cmd + 0x12C0` (`0xfffffe0008b6a560`, docs/20 §4.1). In `SetDataInfo_FwBuf`
the argument registers are `x20 = AVE_PICMGMT_PARAMS*` (`0xfffffe0008b71ffc`)
and `x27 = x20 + 0x4000` (`0xfffffe0008b72020`), which is why size fields
appear as `[x27, #small]`.

### The `0x1738`-byte head is a straight copy

```
b69184:  bl 0xfffffe000b6e8bd0     ; memcpy(PICMGMT+0, FrameInfo+0x5CB8, 0x1738)
         x0 = x19 (PICMGMT), x1 = x21 + 0x5CB8, w2 = 0x1738
```

(`x19` = arg 4 = `AVE_PICMGMT_PARAMS*`, `x21` = arg 3 = `_S_AVE_FrameInfo*`;
register captures at `0xfffffe0008b69134`–`0xfffffe0008b69140`.) **Confirmed.**
Only the first `0x3D0` of that copy reaches the firmware (§0), and only three
fields inside it have been named (§4).

---

## 3. `sFrameInfo` — `0x4F68 .. 0x4FBF`

Filled by `AVE_CHM_SetDataInfo_Frame` from `_S_AVE_FrameInfo + 0x988`:

```
b68314:  add x9, x20, #0x988
b68318:  ldp q0, q1, [x9]          ; 32 bytes
b6831c:  ldr q2, [x9, #32]         ; 16 more
b68328:  stp q1, q2, [x8, #16]     ; x8 = PICMGMT + 0x4F68
b6832c:  str q0, [x8]
b683c0:  str x8, [x21, #20328]     ; then overwrite +0x4F68 with a u64
```

so `PICMGMT[0x4F68 .. 0x4F97] = FrameInfo[0x988 .. 0x9B7]`. **Confirmed.**

| off | type | name | evidence | status |
|---:|---|---|---|---|
| `0x4F68` | u64 | `FrameNum` | fw `0xb3b50` `ldr x8,[x28,#20328]` printed as `FrameNum %lld`; also `0x6895c` | confirmed |
| `0x4F70` | u64 | — | from `FrameInfo+0x990` | unknown |
| `0x4F78` | int | `FrameType` (`IMG_FRAME_TYPE`) | fw `0xb3b5c` `ldr w10,[x10,#3960]` (`x10 = pPicParams+0x4000`) printed as `FrameType %d`, format `0x12d467` (file `0x131467`); host source `FrameInfo+0x998` | confirmed |
| `0x4F80` | int | `PicOrderCntVal` | fw `0xb3b58` `ldr w9,[x10,#3968]` printed as `POC %d`; name from `pPicParams->sFrameInfo.PicOrderCntVal` (file `0x12fae5`) | confirmed |
| `0x4F98` | u64 | — | host `0xfffffe0008b68324`, from `FrameInfo+2488` | unknown |
| `0x4FA4` | u32 | context index | host `0xfffffe0008b683c8` from `FrameInfo+44`; fw `0x73658` `and x22,x10,#0xff` then `ldr x8,[x26,x22,lsl#3]` and `umaddl` by 264 / 528 into per-context arrays | confirmed as an index, name inferred |
| `0x4FA8` | double | frame rate | host `0xfffffe0008b683d0` from `FrameInfo+32`, logged `"%s FPS %f"` (`0xfffffe000728161d`) | confirmed |
| `0x4FB0` | 16 B | — | host `0xfffffe0008b684cc`, `q0` from `FrameInfo+4608`, only when a client bit is set | unknown |
| `0x4FC4` | u32 | — | host `0xfffffe0008b68310` from `FrameInfo+6112` | unknown |
| `0x4FD0` | u8 | — | host `0xfffffe0008b684e0`, bit 2 of `FrameInfo+0x5CB8` | unknown |
| `0x4FD8` | double | — | host `0xfffffe0008b684f0` from `FrameInfo+0x17C4` | unknown |
| `0x4FE0` | double | — | host `0xfffffe0008b684f8` from `FrameInfo+6128` | unknown |
| `0x4FE8` | u32 | — | host `0xfffffe0008b68500` from `FrameInfo+6136` | unknown |
| `0x50F0` | u16 | intra-refresh height (div 64) | host `0xfffffe0008b68600`; the assert on the failing path is `pClient->sIntraRefreshInfo.iIntraRefreshHeightDiv64 <= iFrameHeightDiv64` (`0xfffffe000728214b`) | confirmed |
| `0x50F2` | u16 | intra-refresh rolling position | host `0xfffffe0008b68604` | inferred |

### `IMG_FRAME_TYPE` enumerators

| value | name | evidence |
|---:|---|---|
| `0` | `IMG_INTRA_FRAME` (non-IDR I) | maps to H.264 `slice_type = 2` (`0x50f34`→`0x50f50`); *not* the IDR value (below) |
| `1` | `IMG_INTER_P` | `slice_type = 0` (`0x50f38`→`0x50f40`) |
| `2` | `IMG_INTER_B` | `slice_type = 1` (`0x50f18`→`0x50f48`) |
| `3` | `IMG_INTRA_IDR` | `slice_type = 2` (`0x50f28`→`0x50f50`), **and** the kext DPB gates its `"DPBBuffer (%d): IDR -> IsALongTermFrame %d"` log on exactly this value: `0xfffffe0008b48050 ldr w19,[x24,#2456]` / `0xfffffe0008b48060 cmp w19,#3` / `b.ne` to the `"NON IDR"` log at `0xfffffe0008b48110` |
| `5` | `IMG_UNDECIDED` | `AVE_MD_SVE::DecideFrameType` writes literal 5 to `FrameInfo+2456` as its default (`0xfffffe0008c90188`–`0xfffffe0008c9018c`); `AVE_CHM_SetDataInfo_Header` asserts `pFrameInfo->sGOPInfo.iFrameType == IMG_UNDECIDED` with `cmp w8,#5` (`0xfffffe0008b68a24`–`0xfffffe0008b68a2c`, string `0xfffffe000728175b`) |
| `6` | `IMG_INTER_BREF` | `slice_type = 1` (`0x50f20`→`0x50f48`) |

`4` was not observed. Three-way cross-check on the intra pair: the kext also
does `cmp w21,#3; ccmp w21,#0,#4,ne; cinc x28,x20,ne` at
`0xfffffe0008c9592c`–`0xfffffe0008c95934`, i.e. `frame_num` is *not*
incremented for `0` or `3` — both intra — and the assert guarding it,
`iFrameNum >= 0 && (iFrameType == IMG_INTRA_IDR || iFrameType ==
IMG_INTRA_FRAME || iFrameType == IMG_INTER_P)` (`0xfffffe00072aff6d`), is
compiled as "accept `{0,1,3}`, reject `2` and anything above `3`"
(`0xfffffe0008c9591c`–`0xfffffe0008c95928`). **Confirmed.**

`FrameNum` (`0x4F68`) is host-supplied, so a driver controls it directly: reset
it to 0 on each IDR.

---

## 4. `0x0000 .. 0x03CF` — the picture-parameter head

The only part of the `0x1738`-byte head the firmware receives. Three fields
have names; the rest is a copy of `_S_AVE_FrameInfo + 0x5CB8` and is unmapped.

| off | type | name | evidence | status |
|---:|---|---|---|---|
| `0x0000` | u64 | feature / flag word. Host ORs bit 0 in from `chm[212] < client[15316]` (`0xfffffe0008b69554`–`0xfffffe0008b6956c`). Firmware reads bit 0 and bit 1 (`0x73640`, `0x73648`) into the frame-type decision struct, and passes bit 0 as argument 2 of the virtual rate-control call (`0xb3b10`, `0xb3b2c`) | confirmed as a bitfield; individual bits unknown |
| `0x0360` | u32 | a bit budget. Host: `(double)(FrameInfo[0x6018]) * ratio`, `0xfffffe0008b69454`–`0xfffffe0008b69464`. Firmware divides it by `this[+0x1CDF0]` (`0x73610`, `0x73620`) | confirmed as computed-from-frame-rate; name unknown |
| `0x03A0` | u8 | firmware reads bit 1 (`0x73624` `ldrb w10,[x20,#928]`, `0x73630` `ubfx w8,w10,#1,#1`) | unknown |
| `0x03C0` | int | `eThroughputMode` | host logs `[x19,#960]` as `"eThroughputMode %d"`, format `0xfffffe0007281b87`, load `0xfffffe0008b69684` | confirmed |

---

## 5. `sRCUpdateData` — `0x1738 .. 0x1757`

This is `AVE_PICMGMT_RC_UPDATE_DATA` (the type appears in the firmware symbol
`H264VideoEncoderDPB::ManageDPBBuffer(..., AVE_PICMGMT_RC_UPDATE_DATA*, ...)`
at `0x1b0f8`). Written by `AVE_CHM_SetDataInfo_RC`; `x19` is the block, `x23 =
x19 + 0x1000`, `x24 = FrameInfo + 0x1274`.

| off | type | name | write VA | source | status |
|---:|---|---|---|---|---|
| `0x1738` | int | **`forceKeyFrame`** | `0xfffffe0008b69550` | `FrameInfo[4720]`, with `3 -> 0` (`0xfffffe0008b69544`–`4c`) | confirmed |
| `0x173C` | int | — set to `-1` | `0xfffffe0008b69580` | literal `0xffffffff` | confirmed as always `-1` from this builder |
| `0x1740` | u8 | — | `0xfffffe0008b69578` | bit 0 of `FrameInfo[0x1274]` | unknown |
| `0x1741` | u8 | — | `0xfffffe0008b6958c` | bit 0 of `FrameInfo[0x17BD]` | unknown |
| `0x1742` | u8 | `bEnableUserSAOControl` | `0xfffffe0008b69598` | bit 0 of `FrameInfo[0x17BC]` | confirmed |
| `0x1743` | u8 | — | `0xfffffe0008b695e0` | a temporal-layer test | unknown |
| `0x1744` | u8 | `bChangeBitrateAtNextIDR` | `0xfffffe0008b695ec` | bit 0 of `FrameInfo[0x17BF]` | confirmed |
| `0x1748` | u32 | — | `0xfffffe0008b695f4` | `FrameInfo[4740]` | unknown |
| `0x174C` | u8 | — | `0xfffffe0008b69600` | bit 0 of `FrameInfo[0x17C0]` | unknown |
| `0x174D` | u8 | `bUpdateParameterSets` | `0xfffffe0008b6960c` | bit 0 of `FrameInfo[0x17C1]` | confirmed |
| `0x174E` | u8 | **`bInputCompressed`** | `0xfffffe0008b69618` | bit 0 of `FrameInfo[0x17C2]` | confirmed |
| `0x1750` | int | `scalingMatrixMode` | `0xfffffe0008b69654` | `FrameInfo[0x17CC]` | confirmed |

Names come from the two exit logs, whose argument order fixes the mapping:

* `"eThroughputMode %d bChangeBitrateAtNextIDR %d"` (`0xfffffe0007281b87`) —
  args loaded at `0xfffffe0008b69684`/`88`, stored `0xfffffe0008b69694`.
* `"bEnableUserSAOControl %d"` (`0xfffffe0007281bc6`) — `0xfffffe0008b69710`.
* `"bUpdateParameterSets %d scalingMatrixMode %d forceKeyFrame %d
  bInputCompressed %d"` (`0xfffffe0007281bf0`) — the four loads at
  `0xfffffe0008b69790`–`0xfffffe0008b697a4`, stored in order at
  `0xfffffe0008b697ac`/`b0`.

`bInputCompressed` is independently confirmed on the firmware side: it is the
first thing `CAVCController::setPipe` reads (`0xb52d4` `ldrb w22,[x1,#0x174e]`),
`CDMAController::ConfigLRMERdDMA` reads it (`0x871ac`), and
`CLRMEFSController::ConfigRdDMALowResSrc` reads it (`0x8a48c`) under the assert
`!(pPicParams->sRCUpdateData.bInputCompressed && m_psSequenceInits.bScaledSrcEn)`
(string file `0x12f18f`, VA `0x12b18f`, loaded at `0x8a4b8`). **Confirmed.**

`forceKeyFrame` is read by
`CAVECommonController::GetFrameType(AVE_PICMGMT_PARAMS*, ...)` at
`0x7360c` (`ldr w8,[x20,#5944]`), guarded by the function's `bool` argument
(`0x73600` `tbz w26,#0`) which forces it to 0. **Confirmed.**

### `_S_AVE_DistributedRCParams` — `0x1758 ..`

`AVE_CHM_SetDataInfo_RC` calls
`AVE_SVERC::ProcessRCParams(int, long, int, int, _S_AVE_DistributedRCParams*)`
(`0xfffffe0008ca2f1c`) with the out-pointer `x5 = PICMGMT + 0x1758`
(`0xfffffe0008b69478`–`0xfffffe0008b69484`). The firmware's copy loop reads a
count at `0x175C` and copies `count` 8-byte entries from `0x1760`
(`0x2b35c`–`0x2b374`). **Confirmed** for the shape `{u32 @0x1758; int32 count
@0x175C; u64 entries[] @0x1760}`; field meanings unknown. Set `count = 0` if
you do not use distributed rate control.

---

## 6. `0x4228 .. 0x5117` — buffers and frame identity

### 6.1 `sRef` — the reference lists

The names come from `CHEVCController::DebugEncode(unsigned,
AVE_PICMGMT_PARAMS*)` at `0x84a28`, which dumps them one per log line. Its
`x19` is argument 2 (`0x84a5c` `mov x19, x2`; reloaded `0x85a70`). Each row
below is a `ldr x8,[x19,#N]` immediately followed by the named format string.
**All confirmed.**

| off | type | name | load VA |
|---:|---|---|---|
| `0x4228` | u64 | `sRef.Y_L0_MSB[0]` | `0x85b84` |
| `0x4230` | u64 | `sRef.Y_L0_MSB[1]` | `0x85bb0` |
| `0x4238` | u64 | `sRef.Y_L0_MSB[2]` | `0x85bdc` |
| `0x4240` | u64 | `sRef.Y_L0_MSB[3]` | `0x85c08` |
| `0x4268` | u64 | `sRef.UV_L0_MSB[0]` | `0x85ce4` |
| `0x4270`/`0x4278`/`0x4280` | u64 | `sRef.UV_L0_MSB[1..3]` | `0x85d10`/`0x85d3c`/`0x85d68` |
| `0x4388` | u64 | `sRef.Y_L1_MSB[0]` | `0x85c34` |
| `0x4390`/`0x4398`/`0x43A0` | u64 | `sRef.Y_L1_MSB[1..3]` | `0x85c60`/`0x85c8c`/`0x85cb8` |
| `0x43C8` | u64 | `sRef.UV_L1_MSB[0]` | `0x85d94` |
| `0x43D0`/`0x43D8`/`0x43E0` | u64 | `sRef.UV_L1_MSB[1..3]` | `0x85dc0`/`0x85dec`/`0x85e18` |
| `0x44E8` | u64 | `sRef.Colocated_L1` | `0x85b2c` |

`CAVECommonDPB::setRefPointers(AVE_PICMGMT_PARAMS* x1, ReferenceFrameInfoData*
x2)` (`0x1a2e4`) writes the same offsets *plus* four more arrays at the same
4-entry, 8-byte stride, which pins the rest of the group by construction:

```
1a48c:  ldr x8,[x2,#8]     ; str x8,[x1,#16936]   -> 0x4228  Y_L0_MSB[0]
1a4a4:  ldr x8,[x2,#184]   ; str x8,[x1,#16968]   -> 0x4248  (Y_L0_LSB[0])
1a498:  ldr w9,[x0,#16]    ; str 0x4228+w9 ->  0x4268  UV_L0_MSB[0]
1a4ac:  ldr w9,[x0,#20]    ; str 0x4248+w9 ->  0x4288  (UV_L0_LSB[0])
1a4b8:  ldr x8,[x2,#264]   ; str x8,[x1,#17064]   -> 0x42A8  (Low_Res_Y_L0[0])
```

with the L1 group at `0x4388 / 0x43A8 / 0x43C8 / 0x43E8 / 0x4408`
(`0x1a55c`–`0x1a5e0`). The `_LSB` and `Low_Res_` labels are **inferred** — the
offsets and the `+lumaSize` / `+chromaOffset` relationships are read, and the
assert strings `pPicParams->sRef.Y_L0_LSB[me_ref_index]` (file `0x131967`) and
`pPicParams->sRef.Low_Res_Y_L0[me_ref_index]` (file `0x1314e6`) prove those
fields exist, but no instruction was found that joins a name to those exact
offsets. The `_MSB` and `Colocated_L1` rows are read directly.

`0x42C8 .. 0x4387` (192 bytes) is written by nothing found and is **unknown**.

### 6.2 `sRecon` — this frame's reconstruction target

| off | type | name | evidence | status |
|---:|---|---|---|---|
| `0x4548` | u64 | `sRecon.Y_MSB` | `0x85aa8` `ldr x8,[x19,#17736]` → `"pInPicParams.sRecon.Y_MSB %016llx"` | confirmed |
| `0x4550` | u64 | `sRecon.Y_LSB` | written `0x1a310` from `RefInfo[248]`; host `0xfffffe0008b72a7c`ff | inferred (by elimination) |
| `0x4558` | u64 | `sRecon.UV_MSB` | `0x85ad4` `ldr x8,[x19,#17752]` → `"pInPicParams.sRecon.UV_MSB"`; `= Y_MSB + [x0,#16]` at `0x1a300`–`0x1a308` | confirmed |
| `0x4560` | u64 | `sRecon.UV_LSB` | `= Y_LSB + [x0,#20]` at `0x1a314`–`0x1a31c` | inferred |
| `0x4568` | u64 | colocated MV store for this recon slot | `0x1a474` `str x10,[x1,#17768]` after the pointer search described in docs/21 §5.2 | confirmed |

All four `sRecon` planes are asserted **128-byte** aligned and non-zero
(strings file `0x131c57`–`0x131d44`).

**This corrects [21-buffer-publication.md](21-buffer-publication.md) §5.2**,
which listed the quad as `{Y_LSB, Y_MSB, UV_LSB, UV_MSB}` at
`0x4548/0x4550/0x4558/0x4560`. The dumper reads `0x4548` as `Y_MSB` and
`0x4558` as `UV_MSB`, so the order is `{Y_MSB, Y_LSB, UV_MSB, UV_LSB}`.

### 6.3 Input planes — `cPicMgmtParams[]`

| off | type | name | evidence | status |
|---:|---|---|---|---|
| `0x4570` | u64 | `cPicMgmtParams[AVE_BufIdx_Luma].saIBuf[AVE_BufIdx_Data].iAddr` | `0x85b58` `ldr x8,[x19,#17776]` → that exact string (file `0x12ec6e`); asserted non-zero and `& 63 == 0` at `0xb6b74`–`0xb6b7c` (`setPipe`) and `0x87228`–`0x87234` (`ConfigLRMERdDMA`) | confirmed |
| `0x4578` | u32 | `... .iSize` | host `str w,[x27,#1400]` in `SetDataInfo_FwBuf` | confirmed |
| `0x4590` | u64 | `cPicMgmtParams[AVE_BufIdx_Chroma].saIBuf[AVE_BufIdx_Data].iAddr` | `setPipe` `0xb6cec`–`0xb6cf8`: `csel x8, #0x45b0, #0x4590` then `ldr x8,[x27,x8]`, guarded by the chroma asserts (files `0x12edac`, `0x12edf1`) | confirmed |
| `0x4598` | u32 | `... .iSize` | host `str w,[x27,#1432]` | confirmed |
| `0x45B0` | u64 | alternate chroma `iAddr`, selected when `[sp+100] != 0` in `setPipe` | `0xb6cf0`, `0xb6cf4` | confirmed as an alternative; the selector is unknown |

Element stride is `0x20` (`0x4570` → `0x4590` → `0x45B0`), i.e. each
`cPicMgmtParams[]` entry holds two `{u64 iAddr; u32 iSize; u32 pad}` leaves.
**Inferred** from the three offsets and the two size slots.

### 6.4 `sOutput`

`CAVCController::ProcessTranscodeStart` (`0x6870c`) loads the block as
`x22 = [x1,#8]` (`0x68728`) and `x23 = x22 + 0x4000` (`0x68748`).

| off | type | name | evidence | status |
|---:|---|---|---|---|
| `0x4EF0` | u8 | a mode bit (chooses which size check runs) | `0x688dc` `ldrb w9,[x23,#3824]` | confirmed as a bool; meaning unknown |
| `0x4EF4` | u32 | an index into the firmware's own tables | `0x68910` `ldr w8,[x23,#3828]` | confirmed as an index |
| `0x4EF8` | u64 | **`sOutput.Coded`** | `0x688d4`/`0x68908` `ldr x8,[x22,#20216]`, compared at `0x6892c` against `bitstream_addr_dst[index]` under the assert `pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]` (file `0x129a12`, loaded `0x68a70`) | confirmed |
| `0x4F00` | u64 | `sOutput.coded_dataHeader[]` base | host `SetDataInfo_FwBuf` writes `CodedHeader[slot]` here (docs/21 §5.2); dumper string `"pInPicParams.sOutput.coded_dataHeader[%d]"` (file `0x12b51b`) | confirmed offset, indexing unknown |
| `0x4F08` | u32 | `sOutput.CodedBufSize` | `0x688f0` `ldr w9,[x23,#3848]`, compared `w9 > (3*W*H)>>2` at `0x688f8` under the assert `pPicParams->sOutput.CodedBufSize > (minBufSize/2)` (file `0x129a57`, loaded `0x689dc`) | confirmed |

### 6.5 `sLowResOutput` and the extra buffers

| off | type | name | evidence | status |
|---:|---|---|---|---|
| `0x45F0` | double | — | host `0xfffffe0008b72ae4`, from `client+5272` | unknown |
| `0x45F8` | u64 | surface IOVA, `GetDARTAddr(_S_AVE_CmdInfo[40])` | host `0xfffffe0008b72af0`–`0xfffffe0008b72b04` | confirmed as an address; name unknown |
| `0x4608` | u64 | surface IOVA, `GetDARTAddr(pool[slot])` where the pool base is `x28 + 0xEF9B0` — the region docs/21 identifies as `MBStats` | host base `0xfffffe0008b72b0c`–`10`, store `0xfffffe0008b72b3c` | confirmed as an address; `MBStats` inferred |
| `0x4618` | u64 | surface IOVA, `GetDARTAddr(_S_AVE_CmdInfo[48])` | host `0xfffffe0008b73370`–`0xfffffe0008b73384` | confirmed as an address; name unknown |
| `0x4628` | double | — | host `0xfffffe0008b72448`, from `client+5280` | unknown |
| `0x4630`–`0x466F` | 4×u64 | a 64-byte quad copied wholesale (`ldp q0,q1` twice) | host `0xfffffe0008b72464`–`78`; `0x4630` is read by `ConfigLRMERdDMA` at `0x871c4` (`ldr x11,[x1,#17968]`) | confirmed as addresses; names unknown |
| `0x4670`/`0x4690`/`0x46B0`/`0x46D0` | u64 | `SrcNeighbor{Info,Pixel,Data,FwData}` | host `0xfffffe0008b73bfc`/`c3c`/`c7c`/`cc0`; firmware dumper names `sExtraBuff.SrcNbrInfo/SrcNbrPixels/SrcNbrData` (files `0x12b7ea`/`0x12b816`/`0x12b844`) but reads them from the controller, not from `pPicParams` | confirmed offsets, names inferred |
| `0x4F10` | u64 | `sLowResOutput.LowResSrcLumaScaled` | `CLRMEFSController::ConfigWrDMALowResSrcScaled` `0x8a850` `ldr x10,[x1,#20240]`; also written by `setRefPointers` `0x1a2e8` | confirmed |
| `0x4F18` | u64 | a DPB-set surface | host `0xfffffe0008b75060` | unknown |
| `0x4F48` | u64 | `sLowResOutput.LowResResults` | `CLRMEFSController::ConfigWrDMALowResFSRslts` `0x8aa10` `ldr x10,[x1,#20296]` | confirmed |
| `0x4F50` | u32 | its size | host `str w,[x27,#3920]` | confirmed |
| `0x4F58` | u64 | low-res RC results | `CLRMERSController::ConfigRdDMALowResRCPrvRslts` `0x8b08c`/`0x8b0a8` `ldr x,[x1,#20312]` | confirmed |
| `0x4F60` | u32 | its size | host `str w,[x27,#3936]` | confirmed |
| `0x50F8`, `0x5108` | u64 | two more IOVAs written by `SetDataInfo_FwBuf` and read back for logging (`0xfffffe0008b721a4`, `0xfffffe0008b73060`) | unknown |

---

## 7. Corrections to earlier documents

* **docs/20 §4.3** listed the input surface, output buffer, frame type and
  per-frame QP as "not located". All four are settled here: input `0x4570`,
  output `0x4EF8`, frame type `0x4F78`, and the QP is a *session* parameter
  (`sCAveCmdAvcStart + 0x240/0x244/0x248`), not a per-frame one.
* **docs/21 §5.2** ordered the recon quad `{Y_LSB, Y_MSB, UV_LSB, UV_MSB}`.
  The firmware dumper reads `0x4548` as `Y_MSB` and `0x4558` as `UV_MSB`
  (`0x85aa8`, `0x85ad4`), so the order is `{Y_MSB, Y_LSB, UV_MSB, UV_LSB}`.
  docs/21 flagged this as unresolved; it is now resolved.
* **docs/21 §5.2** noted that `sRef.Y_L0_MSB[]` had no known offset. It is
  `0x4228`, stride 8, four entries.
* **docs/21 §5.2** gives `0x45C8 <- MBStats[slot]` citing store
  `0xfffffe0008b72b3c`. That instruction is `str x0,[x20,#17928]`, i.e. it
  writes **`0x4608`**, not `0x45C8`; nothing in `AVE_CHM_SetDataInfo_FwBuf`
  writes `0x45C8` at all (checked by scanning every `str` in its `0x3200`
  bytes — the same scan does return the other 26 offsets, so it
  discriminates). The pool base docs/21 identifies is right; the destination
  offset was mis-transcribed.
* **docs/21 §5.2** gives `0x4618 <- SliceHeader[slot]`. The store at
  `0xfffffe0008b73384` takes `GetDARTAddr(_S_AVE_CmdInfo[48])`
  (`0xfffffe0008b73370`); the `SliceHeader` pool lookup begins *after* it, at
  `0xfffffe0008b7338c`, and its result is not stored at a fixed offset in that
  basic block. The offset `0x4618` is right; the source attribution is not.

---

## 8. Still unknown

* `0x03D0 .. 0x1737` — copied by the host from `_S_AVE_FrameInfo + 0x5CB8` but
  **discarded by `ProcessCmd_Process_AVC`** (§0), so a driver can leave it
  zero. Nothing was mapped inside it and nothing needs to be.
* `0x0004 .. 0x035F`, `0x0364 .. 0x039F`, `0x03A4 .. 0x03BF`,
  `0x03C4 .. 0x03CF` — the parts of the head the firmware *does* receive.
  Roughly 900 of the 976 bytes are unmapped. This is now the single largest
  gap and is the right target for the next pass: `CAVCController::setPipe`
  (`0xb52a8`, `0x1C00` bytes) and `CAVCController::ConfigureMCPUs`
  (`0x5b104`) both consume it through register-indexed loads, which is why an
  immediate-offset scan finds so little (docs/00 trap 3).
* `0x1760 + 8*N .. 0x4227` — not copied by the AVC handler, so out of scope.
* `0x42C8 .. 0x4387` (192 B) inside `sRef`.
* `sInput.sMultiPassStats` — the assert string exists (file `0x128acc`, VA
  `0x124acc`, referenced from `CAVCController::PipePrepareParam` `0x59458`,
  `CAVECommonController::ProcessFirstPassStats` `0x73364` and
  `CHEVCController` `0x92d60`), but in all three the base register is not the
  `AVE_PICMGMT_PARAMS*` argument, so no offset could be read. **A negative
  that is a limitation of the search, not a finding.**
* `sFrameInfo.iLayerID` — string exists (file `0x12f921`), logged from
  `CHEVCController::PipePrepareParam` `0x904e0` off a base that was not tied
  to `pPicParams`. Offset unknown.
* Which of `0x4590` / `0x45B0` is used, and what `[sp+100]` in `setPipe`
  selects on.
* Whether `AVE_BufIdx` has more than `{Luma, Chroma}` entries, and what
  `saIBuf[]` index 1 (`+0x10` within each element) holds.

---

## 9. Proposed hardware experiment (do not run — for the operator)

Nothing in this document requires hardware to verify. The one check worth
queuing behind first light: submit a single `Process` with
`sFrameInfo.FrameType = 3`, `FrameNum = 0`, `PicOrderCntVal = 0`,
`forceKeyFrame = 1`, all `sRef.*` zero, and confirm the firmware's
`"FrameType: %d FrameNum: %lld POC: %d qp: %d"` trace (`0xb3b88`) reports back
`3 / 0 / 0 / <session QP[I]>`. That single line validates §1 and §3 end to end
and needs no register access beyond a normal command submission.
