# Encoding more than one frame (macOS 13.5)

Everything the driver sends today encodes **frame 0 of a fresh session**:
Config (1) -> Open (2) -> Start_AVC (4) -> Process (7) -> read coded -> Halt (14).
This document is the static analysis of what a *second* and *N-th* `Process`
must look like, who owns each per-frame value, and what happens if we re-send
today's image verbatim.

Nothing here has run on hardware. Method and labels follow
[00](00-methodology.md): **C** = read out of the disassembly with the VA given,
**I** = inferred, with the chain stated, **U** = unknown.

All firmware VAs are 13.5 image VAs (`__TEXT` VA 0 = file `0x4000`); kext VAs
are 13.5 kernelcache VAs. Reproduce any line with

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x2d294 -n 0x460
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008eaa9c8 -n 0x2c0
```

`AVE_MACOS=13.5` is **not optional**.

---

## 0. The short version

1. **The DPB is the firmware's, not the host's.** `H264VideoEncoderDPB::
   ManageDPBBuffer` (fw `0x2d294`), called from `CAVCController::
   PipePrepareParam` (fw `0x483ac`) on every frame, allocates the recon slot,
   rotates the reference list, and hands a `ReferenceFrameInfoData` to
   `CAVECommonDPB::setRefPointers` (fw `0x2c314`), which then **overwrites**
   PICMGMT `0x6F8..0x838`, `0x898..0x8B8`, `0x980..0xBF8` and `0xC20..0xCA0`.
   Those ranges are firmware-owned; anything the host writes there is dead
   (§1.5). **C.**
2. **A multi-frame session does not re-send `Start_AVC`.** The Start-time
   tables are copied once into the DPB context by `CAVECommonDPB::
   ProvideReferenceFrames` (fw `0x2b530`, called from `InitEncodingParameters`
   fw `0x5dd38`) and the firmware indexes into that copy per frame. **C.**
   What a session must do instead is *allocate enough slots at Start time*:
   `max_num_ref_frames + 1` of every per-slot surface (§2.3).
3. **The host's per-frame inputs are small.** In the AVC path the only wire
   fields whose value depends on the frame index are
   `PICMGMT+0xCA8` (frameNumber), `+0xCAC` (frame type), `+0xCB0` (context
   index), `+0x38`/`+0x3C` (forceKeyFrame / forceNonRefFrame), the input plane
   addresses `+0x8C0`/`+0x8D0`, and the output triple `+0xC04`/`+0xC08`/`+0xC10`
   (§1).
4. **`PICMGMT+0xCA8` is not H.264 `frame_num`.** It is a monotone frame counter
   used for rate control and DPB bookkeeping. The syntax element `frame_num` is
   maintained *inside* the firmware at `ctrl+0x236E0` by
   `AVE_H264_PrepareSliceHeader` (fw `0x20e58`-`0x20e64`, reset to 0 at
   `0x210f8` on IDR). **C.**
5. **The coded buffer index and the command slot are the same resource.**
   `AVE_Client_AcquireOutputBuf` (kext `0xfffffe0008ed5f00`) picks the lowest
   free index `i` such that coded surface `i` is idle **and** slot `21+i` has no
   command outstanding, and `SendFwCmd_Process` then computes the header slot
   as `add w26,w22,#0x15` (kext `0xfffffe0008f0166c`). `PICMGMT+0xC04 = i`,
   header `+0x1C = 21+i`. 20 coded buffers, slots 21..40. **C.**
   (Our driver already sends index 0 with slot 21 - the right pair by
   coincidence, `AVE_SESS_PROCESS_SLOT` in `driver/ave_session.c`.)
   So macOS pipelines up to **20** `Process` commands, not one.
6. **Predicted failure of re-sending our current image verbatim** (§6):
   not an assert, not a hang - a *second IDR of the same picture into the same
   coded buffer*, with the previous frame's bytes overwritten, because
   `FrameType=3` forces IDR, `frameNumber=0` passes the only monotonicity check
   at its boundary value, and coded index 0 is reused. **I**, and it is exactly
   what the cheap experiment in the reply tests.

---

## 1. What changes between consecutive `Process` commands

### 1.1 The host-owned per-frame fields

Offsets are into `AVE_PICMGMT_PARAMS`; the wire offset is `0x9C8 +` this
([46](46-abi-13.5-commands-session.md) §10.1).

| PICMGMT off | wire off | width | name | rule for frame *n* | evidence | conf |
|---:|---:|---:|---|---|---|---|
| `0x038` | `0xA00` | u32 | `forceKeyFrame` | 1 on the frames you want IDR, else 0 | kext `0xfffffe0008eab8d8`; fw reads it as `RC_UPDATE_DATA+48` `0x2d2f8` | C |
| `0x03C` | `0xA04` | u32 | `forceNonRefFrame` | 0 for a normal reference frame | kext `0xfffffe0008eac0f8`; fw `RC_UPDATE_DATA+52` `0x2d2e8` | C |
| `0x6D8` | `0x10A0` | u8 | re-anchor flag | non-zero lets the firmware reset `m_iFirstFrameNumber` to this frame's number  (§1.4) | fw `0x2d30c`, `0x2d330` | C (mechanism), I (name) |
| `0x6D9` | `0x10A1` | u8 | — | copied into the RC frame record | fw `0x2d300` | U |
| `0x6DA` | `0x10A2` | u8 | — | copied into the RC frame record | fw `0x2d3d0` | U |
| `0x8C0` | `0x1288` | u64 | `sInput.Y` | this frame's luma IOVA | fw `setPipe` `0x5428c` | C |
| `0x8D0` | `0x1298` | u64 | `sInput.UV` | this frame's chroma IOVA | fw `0x54354` | C |
| `0xC04` | `0x15CC` | u32 | coded buffer index | allocated per frame from the 20-entry pool, §3 | kext `0xfffffe0008eb0520`; fw `ldr w23,[x21,#3076]` `0x5831c` | C |
| `0xC08` | `0x15D0` | u64 | `sOutput.Coded` | must equal Start-time `CodedData[index]` **for that index** | fw compare `0x58398` vs `ctrl+0xB28+8*idx` `0x58330` | C |
| `0xC10` | `0x15D8` | u64 | `sOutput.coded_dataHeader` | Start-time `CodedHeader[index]` | kext `0xfffffe0008eb05a0`; fw `PipePrepareParam` `ldr x11,[x26,#3088]` `0x48104` | C |
| `0xC18` | `0x15E0` | u32 | `CodedBufSize` | ignored when `+0xC00 == 0` (fw takes the size from the Start table `ctrl+0xBC8+4*idx`, `0x5838c`) | fw `0x58364`-`0x5838c` | C |
| `0xCA8` | `0x1670` | u32 | `frameInfo.frameNumber` | monotone, `>= m_iFirstFrameNumber`; §1.4 | kext `AVE_CHM_SetDataInfo_Frame` `str w8,[x20,#3240]` `0xfffffe0008eaaa1c`; fw assert name `0xbfa61` | C |
| `0xCAC` | `0x1674` | u32 | frame type | 3 IDR, 0 I, 5/9 = "firmware decides"; §1.2 | kext `0xfffffe0008eaaa50`; fw `0x145cc`, `0x20d64` | C |
| `0xCB0` | `0x1678` | u32 | context / stream index | 0 for a single-stream session; indexes `CFrameType` (stride `0x130`) and the per-slot controller arrays | kext `0xfffffe0008eaaa58`; fw `0x23d14`, `0x481..` | C (use), I (name) |
| `0xCB8` | `0x1680` | u64 | timestamp / duration pair | copied verbatim into `FrameInfo+0x10` | kext `0xfffffe0008eaaa6c` (16 bytes from `FrameInfo+0x6D0`); fw `0x48150` | C (path), U (meaning) |
| `0xCC0` | `0x1688` | u32 | — | `FrameInfo+0x18` | fw `0x48160` | U |
| `0xCCC` | `0x1694` | u32 | — | `FrameInfo+0x28` | fw `0x48168` | U |
| `0xF50` | `0x1918` | u32 | slice-map entry count | `n`, with `n*64` bytes at `+0xCD0` | kext `0xfffffe0008eaaa70`/`98` | C |

Everything else in PICMGMT is either constant across the session or written by
the firmware (§1.5).

### 1.2 Frame type

`CFlowControllerBase::SendCommandToQueue` reads the host's type at
`PICMGMT+0xCAC` (`ldr w28,[x27,#3244]`, fw `0x145cc`) and **uses it as given**
unless it is 5 or 9, in which case it calls
`CAVECommonController::GetFrameType` (fw `0x23c7c`) through `[vt+400]`
(fw `0x14628`-`0x1463c`) and lets the firmware's own GOP model decide.
**C.**

The kext's own chooser, `AVE_Client_DecideFrameType`
(kext `0xfffffe0008ec5b58`), writes `FrameInfo+0x5C4C` (which
`AVE_CHM_SetDataInfo_Frame` copies to `PICMGMT+0xCAC`):

- `client[0xDDE4] == 0` -> **5** ("firmware decides"), return 0
  (kext `0xfffffe0008ec5bdc`-`e4`). **C.**
- `client[0xD0708] == 1` -> **3** (IDR) (kext `0xfffffe0008ec5ba4`), and if two
  further client bytes are set it also pushes a DPB snapshot
  (`AVE_DPB::SetDPBSnapShot(dpb, FrameInfo+0x1C8, FrameInfo[0x34])`,
  kext `0xfffffe0008ec5bcc`) - and errors with -1000 if `FrameInfo[0x34]`
  (the frame number) is zero on that path. **C.**
- anything else -> -1000. **C.**

So on macOS the normal steady-state value is **5**, i.e. Apple hands GOP
structure to the firmware. A driver can instead drive the type explicitly;
`AVE_H264_PrepareSliceHeader` (fw `0x20d44`) switches on the value with two
8-entry byte tables, and the second one - `0xcef80`, base `0x20e38` - decodes
the whole enum (§1.3).

### 1.3 The frame-type enum, decoded

The jump table at fw `0xcef80` is 8 bytes, `target = 0x20e38 + 4*byte`
(fw `0x20e10`-`0x20e34`). Decoded, with what each arm writes into the
`H264_SLICE_HEADER_PARAMS` at `x19`:

| type | arm | `[x19+4]` | `[x19+8]` | `[x19+16]` | `[x19+34]` | frame counter | meaning |
|---:|---|---|---|---:|---:|---|---|
| 0 | `0x20ff0` | 1 | 1 | 2 | 0 | ++ | **I**, non-IDR |
| 1 | `0x21040` | `ref^1` | 1 | 0 | 0 | ++ | **P** |
| 2 | `0x20e38` | `ref^1` | 1 | 1 | 0 | ++ | **B** |
| 3 | `0x210d4` | 1 | **5** | 2 | **1** | **= 0** | **IDR** |
| 4,5,6 | `0x21130` | — | — | — | — | — | `"AVE ERROR: prepareSliceHeaderForFW frametype not recognized"` (string `0xbe8f9`) - a log, not an assert |
| 7 | `0x20e38` | same as 2 | | | | ++ | B (second encoding) |

`[x19+8]` is `nal_unit_type` (5 = IDR slice, 1 = non-IDR slice: the IDR arm
loads the constant `0x500000001` from `0xcef70` at fw `0x210d8`), `[x19+16]`
is `slice_type` in H.264's own numbering (0 P, 1 B, 2 I), `[x19+34]` is
`IdrPicFlag`, `[x19+28]` is `frame_num`. **C.**

`ref` in the table is `ctrl + 0xA70 + 0x1A8*ctxidx + 0x1980`, byte 0 of the
`ReferenceFrameInfoData` - which `ManageDPBBuffer` sets from
`CRateControl::getIsThisFrameMarkedAsNonReference` (`strb w0,[x22]`
fw `0x2d44c`). So **`nal_ref_idc` follows `forceNonRefFrame`
(PICMGMT `+0x3C`) through the rate controller**, and a driver never writes it
directly. **C.**

**So: `frame_type = 1` is a P frame.** That is the value a second frame needs,
and the whole reason frames 2..N differ from frame 1.

Types 5 and 9 never reach this table: `SendCommandToQueue` intercepts them
first and substitutes the firmware's own decision (fw `0x145d4`-`0x14604`).
**C.**

### 1.4 `frameNumber` - the one hard per-frame rule we have found

`ManageDPBBuffer` opens with (fw `0x2d2dc`-`0x2d3c8`):

```
w8 = frameInfo.frameNumber           ; = PICMGMT+0xCA8
if (PICMGMT[0x6D8] != 0 && w8 % this[32] == 0)
        this[48] = w8                ; m_iFirstFrameNumber = frameNumber
w9 = this[48]
w8 = w8 - w9
if (borrow) assert "frameInfo.frameNumber >= m_iFirstFrameNumber"
            (CAVEDPB.cpp:963, string 0xbfa61, file string 0xbfa36)
w8 = (frameNumber - m_iFirstFrameNumber) % arg6      ; arg6 = ctrl[4628]
```

`this[32]` is set to **1** and `this[48]` to **0** by the
`H264VideoEncoderDPB` constructor (fw `0x2d1c4`, `0x2d1c8`), so on a fresh
session `m_iFirstFrameNumber == 0` and *any* non-negative `frameNumber`
passes. **C.**

The assert is `_bsp_assert_fail` + `b .` shaped ([46](46-abi-13.5-commands-session.md)
§0): a violation wedges the coprocessor, it does not return an error.

**Rule for the driver: `frameNumber` must be monotone non-decreasing across the
session.** Using `n` (0,1,2,...) satisfies it. **C** for the check; **I** that
plain `n` is also what the rest of the RC/DPB code expects - `frameNumber` is
divided by the frame-rate denominator in `GetFrameType`
(`udiv w8,w8,w9` fw `0x23cf8`) and used as the RC time base.

**`frameNumber` is not the H.264 `frame_num` syntax element.**
`AVE_H264_PrepareSliceHeader` keeps its own counter at `ctrl+0x236E0`:
on the reference-picture arm it does `w15 = [x14]; w15++; [x14] = w15;
[slicehdr+28] = w15` (fw `0x20e58`-`0x20e64`), and on the IDR arm it writes
`[slicehdr+28] = 0` and `str wzr,[x14]` (fw `0x210dc`, `0x210f8`). It also
alternates `idr_pic_id` at `[slicehdr+36]` (fw `0x21104`-`0x2111c`) and saves
the pair to `ctrl+0x82F4` (fw `0x211a4`). **C.**
So the driver never writes `frame_num` or `idr_pic_id` on the wire.

### 1.5 The fields the firmware overwrites every frame

`CAVECommonDPB::setRefPointers(PICMGMT*, ReferenceFrameInfoData*)`
(fw `0x2c314`) is called at the end of `ManageDPBBuffer` (fw `0x2d6e8`) - i.e.
inside `PipePrepareParam`, *after* the command has been copied into the
firmware's pool and *before* `setPipe` reads it. Every `str` it makes into its
`x1` (the PICMGMT pointer), extracted mechanically from the listing:

| PICMGMT range | count | what | 
|---|---:|---|
| `0x6F8`, `0x700`, `0x708`, `0x710` ... `0x838` | 41 x u64 | reference-picture plane addresses (L0/L1 Y/UV, MSB/LSB) |
| `0x898`, `0x8A0`, `0x8A8`, `0x8B0` | 4 x u64 | `sRecon` Y_MSB / Y_LSB / UV_MSB / UV_LSB |
| `0x8B8` | u64 | colocated MV store for this frame |
| `0x980` .. `0xBF8` | 64 x u64 | the SrcNeighbor groups **and** the `encoder_addr_entropy` table ([53](53-first-frame.md) §26) |
| `0xC20` .. `0xCA0` | 17 x u64 | — (**U**) |

Reproduce:

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2c314 -n 0xd00 \
  | grep -oE '\[x1, #[0-9]+\]' | sort -u
```

Two consequences.

- The four recon planes are *derived*, not published: setRefPointers writes
  `Y_MSB = entry[48]`, `Y_LSB = refdata[216]`,
  `UV_MSB = Y_MSB + dpb[20]`, `UV_LSB = Y_LSB + dpb[24]`
  (fw `0x2c314`-`0x2c33c`), where `dpb[20]` and `dpb[24]` are the luma plane
  sizes the `H264VideoEncoderDPB` constructor computed from width/height
  (fw `0x2d14c`-`0x2d1a4`). **The firmware decides where chroma lives.** **C.**
- The gaps matter. `0x840..0x890`, the input planes `0x8C0..0x8D8`, the output
  block `0xC00..0xC18` and the frame block `0xCA8..0xCCC` are **outside** every
  written range, which is exactly why the host's values for those survive.
  **C** (by exhaustion of the store list above).

### 1.6 Independent confirmation of `0x1670` / `0x1674`

`CAVEClient::CommandQueue::Enqueue(u32 id, u64, u64, void* cmd, FrameInfo*)`
(fw `0x1664c`) reads two words straight out of the wire command for encode
commands (`sub w8,w1,#7; cmp w8,#1; b.hi` — ids 7 and 8 only, fw `0x166a8`):

```
16 6d8:  ldr  w8, [x19, #4]                  ; codec
16 6dc:  mov  w9, #0x1670 ; mov w10, #0x1674 ; AVC pair
16 6e8:  mov  w8, #0x6258 ; mov w8, #0x625c  ; HEVC pair
16 6f8:  ldr  w8, [x21, x8]                  ; x21 = the command image
16 6fc:  ldr  w9, [x21, x9]
16 704:  stp  w9, w8, [x25, #88]             ; into the queue entry
```

`0x1670` and `0x1674` are exactly `0x9C8 + 0xCA8` and `0x9C8 + 0xCAC`, read
from the *wire image* rather than from PICMGMT — a second, independent
derivation of both offsets, and of which is which (the HEVC pair
`0x6258`/`0x625c` is `0x55B0 + 0xCA8`/`0xCAC`, the HEVC PICMGMT base from
[46](46-abi-13.5-commands-session.md) §10.1). **C.**

**And it is the key the firmware's queue uses.** `CommandQueue::Complete(u32
frameNumber)` (fw `0x16bf0`) scans the ring backwards matching
`entry[88] == frameNumber` (fw `0x16c78`-`0x16c80`), and
`CommandQueue::Dequeue(cid, id, frameNumber, sCmdInformation*)` (fw `0x167d0`)
matches the same field (fw `0x16858`-`0x1685c`). **A session that sends the
same `frameNumber` for every frame gives the firmware's queue no way to tell
its frames apart.** **C** for the matching; **I** for the consequence.

---

## 2. Slot and buffer management

### 2.1 Who chooses what

| resource | chosen by | how |
|---|---|---|
| DPB / recon slot for this frame | **firmware** | `ctx[4228]`, maintained by `RemoveDPBEntry` (fw `0x2c1b4`) / `InsertBeforeDPBEntry` (fw `0x2c280`), reset to 0 by `InitPointerAndVariables` (fw `0x2bd1c`) on every IDR |
| which slots are referenced | **firmware** | `DPBSanityCheck` (fw `0x2be3c`) + the entry linked list walked in `ManageDPBBuffer` (fw `0x2d660`, `0x2d79c`, `0x2d800`) |
| colocated MV buffer for this frame | **firmware** | `ctx+0x200+8*slot`, written into PICMGMT `+0x8B8` by `setRefPointers` (fw `0x2c4ac`-`0x2c4b0`) |
| low-res (LRME) surface | **firmware** | DPB entry `+64` -> RefFrameInfo `+224` -> PICMGMT `+0xC20` ([53](53-first-frame.md) §13.1) |
| entropy / SEB channel table | **firmware** | rebuilt into PICMGMT `+0x980..0xBF8` from the Start-time record ([53](53-first-frame.md) §26) |
| **coded buffer index** | **host** | `AVE_Client_AcquireOutputBuf`, §3 |
| **command slot** (header `+0x1C`) | **host** | `21 + coded index`, §3 |
| **frameNumber, frame type, context** | **host** | §1 |

The firmware never returns a slot index to the host. The `ENCODE_DONE` reply
carries only the command's own slot at `+0x1C` and one extra word at `+0x40`
([46](46-abi-13.5-commands-session.md) §2.1). **C.**

### 2.2 The per-frame rotation, read out of `ManageDPBBuffer`

`H264VideoEncoderDPB::ManageDPBBuffer` (fw `0x2d294`) is called once per frame
from `CAVCController::PipePrepareParam` (fw `0x483ac`) with, among others,
`x3 = _sAVEFrameInfo*` (= `ctrl+0xA70 + 0x60*ctxidx + 0x16A8`, the struct
`PipePrepareParam` has just filled from PICMGMT `0xCA8/0xCAC/0xCB8/0xCC0/0xCCC`
at fw `0x4813c`-`0x4816c`), `x4 = PICMGMT + 8` (the
`AVE_PICMGMT_RC_UPDATE_DATA`, fw `0x48178` / `0x4837c`) and
`x5 = ReferenceFrameInfoData*` (= `ctrl+0xA70 + 0x1A8*ctxidx + 0x1980`,
fw `0x48370`/`0x4838c`). **C.**

Its two arms:

- **frame type 3 (IDR)** — `InitPointerAndVariables(ctx)` (fw `0x2d4e8`)
  **resets the whole DPB**: `ctx[4228] = 0`, every entry invalidated. The
  current entry and the next one are read out of the freshly reset array
  (fw `0x2d4f8`-`0x2d55c`) and the reference addresses in
  `ReferenceFrameInfoData` at `+16` and `+40` are set to **zero**
  (fw `0x2d52c`, `0x2d534`), so the P/B reference slots in PICMGMT come back
  as 0. **C.**
- **any other type** — the L0/L1 lists are copied out of the context arrays at
  `ctx + 64*parity + {4512,1536,...}` (fw `0x2d5a8`-`0x2d62c`), the parity bit
  `ctx[4640]` is flipped when `dpb[44]` is set (fw `0x2d638`-`0x2d640`), and
  the entry linked list is walked *k* links from `ctx[4228]` to find each
  reference (fw `0x2d660`, `0x2d79c`, `0x2d800`). A walk that runs off the end
  logs `DPB ERROR` via `DPBGetFrameRequested` (string `0xbfb9b`) and the frame
  is dropped rather than asserted. **C.**

So an all-IDR stream never exercises the reference path at all: every frame
resets the DPB, writes recon slot 0, and carries no references.

### 2.3 How many of each a session must allocate

Settled in [53](53-first-frame.md) §13.2-13.3 and unchanged by this reading:

| table | Start_AVC wire off | shape | count that must be valid |
|---|---:|---|---|
| recon `{Y_MSB, Y_LSB}` | `0x88` | 2 sets x 17 x `0x10` | `max_num_ref_frames + 1`, set 0 only |
| LowResRef | `0x2A8` | 2 x 17 x `8` | same |
| Colocated | `0xF6B0` | 2 x 17 x `8` | same |
| entropy addresses | `0xF830` | 4 x 4 x `8` | all 16 ([53](53-first-frame.md) §26) |
| entropy sizes | `0xFA30` | 4 x 4 x `4` | all 16, **I** ([53](53-first-frame.md) §27) |
| CodedData addr / size | `0x4B8` / `0x558` | 20 x `8` / 20 x `4` | as many as you intend to keep in flight |
| CodedHeader | `0x5C0` | 20 x `8` | same |

`CAVECommonDPB::ProvideReferenceFrames(numRefs, VIDEO_PARAMS*)` (fw `0x2b530`)
rejects `numRefs > dpb[8]` (the level's max DPB size, `0x2b558`) and
`numRefs >= 17` (`0x2b5b8`), then copies `numRefs + 1` slots of all three
DPB-indexed tables into the DPB context, `dpb[32]` (= **1**) sets deep.
**C.**

**Therefore: a multi-frame session does not re-send `Start_AVC`.** The
firmware holds its own copy of the tables from Start time and indexes into it
per frame. The only reason to re-send Start would be to change
`max_num_ref_frames`, the resolution or the parameter sets — and `AVC_INIT`
runs `CreateClient`, so it is a session restart, not a reconfiguration.
**C** for the copy; **I** for "therefore".

**The consequence for our driver:** with `max_num_ref_frames = 1` we allocate
two recon, two low-res and two colocated slots already
([53](53-first-frame.md) §13.4), which is enough for an IPPP stream with one
reference. Nothing in the slot model has to change to go from one frame to N;
what has to change is that the driver must stop believing the per-frame
PICMGMT recon/colocated/entropy writes do anything (§1.5).

---

## 3. Coded output rotation

`AVE_Client_AcquireOutputBuf(client, cmd)` (kext `0xfffffe0008ed5f00`):

```
n = min(client[0xCF2D4], client[0xCF2E8])                    ; 0xed5f24-30
for i in 0..n-1:
    surf = client[0xCF670 + 8*i]                             ; 0xed5f4c
    if surface_is_idle(surf) and surf->[384] == 0            ; 0xed5f54-60
        and client[808 + 8*(i + 0x15)] == 0:                 ; 0xed5f64-70
            return i
return -1                                                    ; 0xed5f80
```

`0x15 = 21`, and `client[0x328 + 8*(i+21)]` is the per-slot in-flight command
pointer.

**`slot = coded index + 21` is read directly, not inferred.**
`AVE_HwC::SendFwCmd_Process` bounds-checks its index argument
(`cmp w22,#0x13; b.hi` kext `0xfffffe0008f01188`, so `index <= 19`) and then
computes the slot from it:

```
fffffe0008f0166c:  add w26, w22, #0x15        ; slot = index + 21
fffffe0008f01694:  mov x5, x26                ; -> AVE_CHM_MakeFwCmd_Process_AVC slot arg
fffffe0008f0169c:  bl  0xfffffe0008eac7b4
```

So coded buffer *i* and command slot *21 + i* are **one resource**, and Apple's
client uses slots 21..40 for exactly the 20 coded buffers. **C.**

The index is latched into `FrameInfo+448` by `AVE_HwC::PrepareDataResource`
(kext `0xfffffe0008f01ba0`-`a4`) only when it is still negative, i.e. once per
frame, and passed as `SendFwCmd_Process`'s `u32` argument
(kext `0xfffffe0008f07770`-`84`), which becomes `PICMGMT+0xC04`.

Firmware side, `CAVCController::ProcessTranscodeStart` (fw `0x581a8`):

```
58 31c:  ldr w23, [x21, #3076]        ; index  = PICMGMT+0xC04
58 320:  ldr x11, [x21, #3080]        ; addr   = PICMGMT+0xC08
58 330:  ldr x9,  [x22, #184]!        ; x22 = ctrl+0xA70 + 8*index + 184
58 350:  ldrb w8, [x21, #3072]        ; mode   = PICMGMT+0xC00
         mode == 0:
58 38c:    ldr w9, [x19 + 4*index + 3016]   ; the Start-time SIZE for that index
58 398:    cmp addr, [x22]                  ; the Start-time ADDRESS for that index
58 39c:    b.ne -> assert (bitstream_addr_dst[index], string 0xc4a07)
```

**Both halves of the output are indexed by `+0xC04`.** Sending index *i* with
the address of buffer *j* asserts. **C.**

**How the host knows which buffer a completion refers to:** it does not read it
out of the reply. The reply echoes the command's slot at `+0x1C`
([46](46-abi-13.5-commands-session.md) §2.1); the host looks the command up in
`chm[0xA8 + slot*8]` (kext `0xfffffe0008efbb48`) and the command already
carries the index in `FrameInfo+448`. `slot = 21 + index` makes the two
equivalent. **C** for the mechanism; **I** that Apple relies on the slot
rather than the extra reply word.

**When a buffer may be reused:** `AVE_Client_ReleaseOutputBuf`
(kext `0xfffffe0008ed6094`) is called from `AVE_CHM_HandleCmds`
(kext `0xfffffe0008eb4584`), the completion handler — i.e. **on completion of
the Process command that used it**. The `AVE_CHM_ReleaseOutputBuf` calls in
`ProcessInputCmd_Process` / `ProcessReadyCmd_Process`
(kext `0xfffffe0008f02fb0`, `…f06c8c`, `…f06f1c`, `…f07260`, `…f07490`) are all
on submit-failure paths, undoing the acquire. **C.**

There is no ring index and no producer/consumer pointer anywhere in the
`Process` wire image or the reply: the rotation is entirely the host's
free-list. **C** (by exhaustion of the fields above).

---

## 4. The commands we have never sent

Ids and sizes are [46](46-abi-13.5-commands-session.md) §1.1 and are not
re-derived here. What follows is what each handler *does*.

| id | name | wire image | what the firmware does | needed between frames? |
|---:|---|---|---|---|
| 3 | `RESET` | `0x32DB0`: header, `+0x40` u8 = `client[0xE1078]`, payload from `+0x48` | not mapped | no (**U**) |
| 6 | `UNINIT` | `0x40`, header only (kext `0xfffffe0008eaa6e8`-`724`) | `ProcessUninit` fw `0xf480` | no — it is the counterpart of `AVC_INIT`, i.e. session teardown ([63](63-teardown.md) owns this path) |
| 9 | `LRME_STANDALONE` | `0x6838` (the HEVC encode struct) | `ProcessLrmeStandAlone` fw `0xe9c8` | no; a standalone motion-estimation pass, not part of encode |
| 10 | `MCTF_PROCESS` | — | **no handler**: the jump-table entry is the default `0xe218`, which returns 0 without doing anything | never send |
| 11 | `AVE_FLUSH` | `0x40`, header only | see below | no |
| 12 | `STOP` | `0x48`, `+0x40` u8 | `UnregisterClient` (fw `0x1088c`) | no — session teardown |
| 13 | `COMPLETE` | `0x40`, header only | see below | **no** (see below) |

### 4.1 `AVE_FLUSH` (11), `ProcessFlush` fw `0xff7c`

```
ff dc:  x20 = this[1440]                      ; the command image
10 018: x21 = this + 0x7A38                   ; CAVEPriorityQueue
10 028: GetClientIndexFromID(cmd[0x10])        ; == -1 -> error exit
10 054: SetClientPriority(cid, cmd[0x20])
10 058: w27 = client[196]                      ; number of queued frames
        for i = w27-1 .. 0:
10 074:   log "i %d Complete called for client->FrameNumberFromDriver() %d"
10 0a0:   CAVEPriorityQueue::Complete(cid, client[188] + i, mode = 0)
10 0cc: reply id 0xE09 FLUSH_DONE
```

So **Flush = "force-complete every frame this client still has queued, then
reply"**. It takes no per-frame argument on the wire; the set is whatever the
firmware's own queue holds. **C.**

### 4.2 `COMPLETE` (13), `ProcessComplete` fw `0x10918`

Same shape, with two differences (fw `0x109d8`-`0x10a8c`):

- the mode argument to `CAVEPriorityQueue::Complete` is **1**, not 0
  (`mov w3,#1` fw `0x10a80` vs `mov w3,#0` fw `0x1009c`);
- when `client[200] != 0` and the controller's once-only byte `this[3258]` is
  clear, it first calls `CFlowControllerBase::EnqueueDummyFrameForMCTF(cid,
  frameNo+1)` and `(cid, frameNo+2)` (fw `0x10a54`, `0x10a6c`), sets
  `this[3258] = 1` and logs `"------ RampDown(%d) -------"` (string
  `0xbdaa2`). That is the MCTF (temporal-filter) pipeline drain: MCTF needs
  two look-ahead frames, so end-of-stream has to push two dummies through it.
  **C.**

`CAVEPriorityQueue::Complete(cid, frameNumber, mode)` (fw `0x18a40`) resolves
to `CAVEClient::CommandQueue::Complete(frameNumber)` (fw `0x16bf0`), which
scans the 64-entry ring backwards and sets `entry[72] = 1` on entries whose
`entry[88]` (= the wire `frameNumber` from `0x1670`) matches. **C.**

### 4.3 Is `COMPLETE` required between frames? No.

The per-frame ring is drained by `CAVEClient::CommandQueue::Dequeue(
sCmdInformation*)` (fw `0x16af4`), which advances the tail itself
(`add w8,w22,#1; str w8,[x0,#24]` fw `0x16b3c`-`0x16b40`) when the pipe picks
work up. `Complete` only *marks* entries; it does not free ring space.
`ProcessAvcEncode` never calls it. **C.**

On the host side `AVE_Client_Complete` (kext `0xfffffe0008ed333c`) is reached
only from `AppleAVE2UserClient::Complete` (kext `0xfffffe0008e9d30c`), i.e. it
is a **user-space API call** — VideoToolbox's "give me the frames that have
finished" — not something the kext emits on its own. **C.**

**Conclusion:** a driver that sends one `Process` and waits for its
`ENCODE_DONE` before sending the next needs neither `COMPLETE` nor `FLUSH`.
A driver that pipelines should send `FLUSH` at end of stream (it is the
cheaper of the two and needs no frame number), and `COMPLETE` **only** if MCTF
is enabled, because the `RampDown` path is the only drain for the two-frame
MCTF look-ahead. We do not enable MCTF (id 10 has no handler at all on 13.5),
so `COMPLETE` is dead code for us. **I**, from the call graph above.

---

## 5. Pipelining

**macOS does not run one at a time.** Three independent limits:

| limit | value | evidence |
|---|---:|---|
| coded buffers / command slots the host will use | **20** (slots 21..40, indices 0..19) | `AVE_Client_AcquireOutputBuf` kext `0xfffffe0008ed5f64`-`70`; `cmp w22,#0x13` kext `0xfffffe0008f01188` |
| entries in the firmware's per-client ring | **64** | `subs w23,w8,#0x40; b.mi` fw `0x16670`, else `"Enqueue failed: queue is full"` (string `0xbe014`) and the enqueue is dropped |
| CHM slot arrays in the kext | 41 (`chm[0xA8+8*slot]`, `chm[0x1F0+8*slot]`) | [46](46-abi-13.5-commands-session.md) §2 |

The binding one is **20**: `AcquireOutputBuf` returns -1 when every coded
buffer is busy, and the host then has to wait. **C.**

**The contract.**

- Submission is asynchronous: `AppleAVE2UserClient::Process` returns after
  `AVE_Client_Process` queues the command; results are collected by a separate
  `IO_Complete` call. **C** (two distinct user-client selectors, 7 and 6,
  [46](46-abi-13.5-commands-session.md) §6).
- Completion is `IO_T2H` -> `AVE_HwC::ProcessIntr_Cmd`
  (kext `0xfffffe0008f0b838`) -> `ProcessIntr_OutputData`
  (`0xfffffe0008f0af5c`) -> `AVE_CHM_HandleCmds`, which is where
  `AVE_Client_ReleaseOutputBuf` frees the coded buffer (§3). **C.**
- Ordering is **not** guaranteed to be submission order in general: the
  firmware has `CAVEClient::CommandQueue::ReorderFrames(u32, Command*)`
  (fw `0x160a8`) and `CAVEPriorityQueue::GetCommandQueueToDequeue`
  (fw `0x186c4`) picks among clients by priority. Within one client with one
  priority and no B-frames nothing reorders, but the host is expected to match
  completions by slot/frameNumber, not by arrival order. **C** that the
  reordering machinery exists; **I** that a single-client IPPP stream is FIFO.
- Every command carries its own `_S_AVE_TimeOut` at header `+0x28`
  ([46](46-abi-13.5-commands-session.md) §2), and the firmware has a
  `0xE00 CH_FRAMEDONE_TIMEOUT` reply id, so a lost frame is reported rather
  than hanging the ring. **C** (the id exists); **U** what arms it.

**For our driver the right first step is still strictly one at a time.**
Pipelining buys throughput and costs a completion-matching bug surface we
cannot debug on this machine one boot at a time.

---

## 6. What breaks on frame 2 if we change nothing

The prediction, for the current `driver/ave_cmd.c` image re-sent verbatim with
only the input planes repointed (and in fact our current image does not even
repoint those):

| # | what happens | conf |
|---|---|---|
| 1 | **No assert.** `frameNumber` is 0 both times and the only hard per-frame check is `frameNumber >= m_iFirstFrameNumber` with `m_iFirstFrameNumber = 0` (fw `0x2d348`, ctor `0x2d1c8`). `0 >= 0` passes. | C |
| 2 | **Frame 2 is another IDR, not a P frame.** `frame_type` is 3, so `ManageDPBBuffer` takes the IDR arm, `InitPointerAndVariables` resets the DPB and `ctx[4228]` to 0 (fw `0x2bd1c`), and `AVE_H264_PrepareSliceHeader` writes `frame_num = 0`, `IdrPicFlag = 1` again (fw `0x210dc`-`0x210f8`). The stream is a sequence of independent IDRs — decodable, but no inter prediction and ~1 keyframe-sized frame each. | C |
| 3 | **The recon slot is slot 0 again**, so the reconstruction of frame 1 is overwritten. Harmless while every frame is an IDR; fatal the moment one is not. | C |
| 4 | **The coded buffer is index 0 again**: the firmware writes frame 2's bitstream over frame 1's at the same IOVA. Our driver copies the bytes out synchronously before sending the next command, so this is survivable — but only because we are synchronous. | C |
| 5 | **The coded-data header is reused without being cleared.** `ave_cmd_coded_length()` scans slice records until the first `ui32BytesWritten == 0` ([53](53-first-frame.md) §3.2). `ave_session.c` memsets that buffer once, at allocation. If frame 2 produces *fewer* slices than frame 1, the scan runs on into frame 1's stale records and reports a length that is too large; if it produces the same number, the answer happens to be right. **This is the first thing that will silently produce a wrong answer.** | C (mechanism), I (that our 1-slice config hides it) |
| 6 | **`FrameNumberFromDriverReturned` (coded header `+0x10C`) comes back 0 for every frame**, because we never write `PICMGMT+0xCA8`. There is then no field anywhere that identifies which frame a completion belongs to. | C |
| 7 | **The firmware's queue cannot tell the frames apart.** `CommandQueue::Complete` and `Dequeue` match on the wire `frameNumber` at `0x1670` (fw `0x16c78`, `0x16858`). With every frame numbered 0, a `Flush` or a timeout would retire the wrong entry. Invisible while we are synchronous and never flush. | C (the matching), I (the consequence) |
| 8 | **Rate control sees a stalled clock.** `GetFrameType` divides `frameNumber` by the frame-rate denominator (fw `0x23cf8`) and `CRateControl::setRateControlFrameInfo` consumes the same record; with fixed QP (`ui32RCFlag = 2`) this is inert, but it will not stay inert when RC is turned on. | I |

**Ranked, the single most likely *visible* failure is #5** — a plausible but
wrong byte count on the second frame — followed by #2, which is not a failure
at all but means we will have "encoded 2 frames" without ever having exercised
inter prediction, the DPB rotation, or anything this document is about.

**The thing that will *not* happen:** a `Start_AVC` re-send is not needed, and
the per-frame PICMGMT recon / colocated / entropy writes the driver makes are
neither needed nor harmful — they are overwritten before `setPipe` reads them
(§1.5), exactly as [53](53-first-frame.md) §26 found for the entropy table.

---

## 7. Reproduce

```sh
# the per-frame DPB call and its arguments
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x480b4 -n 0x320   # PipePrepareParam
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2d294 -n 0x460   # ManageDPBBuffer

# everything setRefPointers overwrites in PICMGMT
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2c314 -n 0xd00 \
  | grep -oE '\[x1, #[0-9]+\]' | sort -u

# the monotonicity assert and its string
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x2d330 -n 0x40
python3 - <<'EOF'
d=open("data/blobs/macos-13.5/ave_h13c.bin","rb").read()
for va in (0xbfa61, 0xbfa36, 0xbe014, 0xbdaa2):
    o=va+0x4000; print(hex(va), d[o:d.index(b"\0",o)])
EOF

# frame_num / idr_pic_id are the firmware's, not the host's
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x20d44 -n 0x470

# wire 0x1670 / 0x1674 read straight out of the command image
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x1664c -n 0xc0

# the coded-buffer / slot free list
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ed5f00 -n 0x90
AVE_MACOS=13.5 python3 tools/disas.py --fw  --addr 0x582f0 -n 0xe0

# Complete and Flush
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0xff7c  -n 0x160
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x10918 -n 0x180
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x16af4 -n 0xfc
```

## 8. Known unknowns

- `PICMGMT+0xCB8` (u64), `+0xCC0`, `+0xCCC`: carried per frame into
  `_sAVEFrameInfo+0x10/+0x18/+0x28` and consumed by the rate-control frame
  record. Almost certainly presentation timestamp and duration; **not read**.
- `PICMGMT+0x6D8/0x6D9/0x6DA`: three per-frame bytes the DPB and RC read.
  `0x6D8` gates the `m_iFirstFrameNumber` re-anchor; the other two only reach
  the RC record.
- `PICMGMT+0xC20..0xCA0`, 17 u64 written by `setRefPointers`: only `0xC20`
  (`LowResSrcLumaScaled`) and `0xC28..0xC40` (the LRME result array) are named
  ([53](53-first-frame.md) §11, §13); the rest are unread.
- ~~Which frame-type indices are which.~~ **Resolved in §1.3**: 0 I, 1 P,
  2 and 7 B, 3 IDR, 4/5/6 rejected with a log. What distinguishes 2 from 7 is
  still **U** (they share an arm in the slice-header writer; they may differ
  in `ManageDPBBuffer`, which was not traced past the type-3 test).
- `AVE_Client_DecideFrameType`'s gates `client[0xDDE4]` and `client[0xD0708]`:
  read, but what sets them (a user-space session flag) is not.
- Whether the firmware ever zeroes the coded-data header between frames. No
  writer was found on the `ProcessTranscodeDone` path; absence is not proven.
