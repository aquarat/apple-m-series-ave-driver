# Bitstream retrieval — where the coded bytes are, how many, and how they assemble (macOS 13.5)

Everything here is read out of the macOS 13.5 pair
(`AppleAVE2FW-6070.11.1` + the 13.5 kernelcache). Bare VAs are 13.5 **firmware**
image VAs; `0xfffffe…` are 13.5 **kernelcache** VAs. Labels are
[00-methodology.md](00-methodology.md)'s: **C** read out of the disassembly,
**I** inferred, **U** unknown.

Sibling documents: [53](53-first-frame.md) §3–§4 first established the length
formula and the parameter-set prepend; [66](66-ratecontrol-sizing.md) §3 covers
`CODED_DATA_HDR` as the *rate-control* feedback channel and §5 its size. This
document owns it as the **bitstream retrieval contract**. Where the two
disagree, the disagreements are called out in §8.

---

## 0. The short version

1. **Offset.** For a fresh frame the bitstream starts at **byte 0** of the
   buffer whose IOVA the host put in `PICMGMT+0xC08` (`sOutput.Coded`).
   `ProcessTranscodeStart` stores that address verbatim into
   `EncCommParams.curr_bitstream_addr_dst` (`ctrl+8328`,
   `str x8,[x19,#8328]` `0x583a4`) and the hardware writes from there. **C.**
2. **Format.** Annex B, 4-byte start codes, emulation prevention already
   applied by the hardware's bit writer. Not length-prefixed. **C.**
3. **Slices are contiguous, back to back.** Slice *s* begins at
   `Σ_{t<s} ui32BytesWritten(t)` and is `ui32BytesWritten(s) −
   bytesToRemove(s)` bytes long. The firmware advances one write pointer
   across slices (`str x10,[x19,#8328]` `0x5c874`) and *differences* a
   hardware register array to fill the per-slice records. **C.**
4. **The length formula in `ave_cmd_coded_length()` is right but incomplete.**
   It is missing `numCABACzeroWordInserted` at `CODED_DATA_HDR+0xF0`: the
   firmware computes how many `cabac_zero_word`s H.264 requires, writes the
   count there, and **does not write them into the buffer**. The host must
   append `3 × count` bytes. **C.**
5. **The parameter sets are written once, at `Start_AVC`, and never again.**
   `InitEncodingParameters` is the only code in the image that generates or
   copies them, and it is reached only from `CAVCController::Init` and
   `ProcessInit`. `update_param_sets` (`PICMGMT+0x6F1`) has **no reader** in
   the firmware. **C.**
6. **There is an exact host-visible parameter-set length and we are not using
   it:** `CODED_DATA_HDR+0x98` `ui32_SPSPPSHeaderBits` is `sps_bits +
   pps_bits`, the same two numbers the firmware used as `memcpy` lengths
   (`>>3` each). `psets_len = sps_pps_bits / 8`, exactly. The "scan back for
   the last non-zero byte" can go. **C.**
7. **"The output was clipped" has a name.** `ProcessTranscodeDone` checks
   `[8312] + [8316] + [8356] ≤ [8360]` (`0x5bf7c`), logs *"bitstream size
   overflow, buffer size: %d, bitstream size: %d"*, and sets the completion
   status to **`0xEE0004`**. A hardware transcode error sets **`0xEE0007`**.
   Both arrive in the `ENCODE_DONE` reply's status word at `+0x38`, which
   `ave_cmd_check_reply()` already reads — we just never say what the values
   mean. **C.**
8. **The host must zero the coded header before every frame.** The MB-count
   arrays and the bit/byte accumulators in the header are written
   **read-modify-write** by `CollectDataFromCpus` (`ldr`/`add`/`str` at
   `0x5a55c`–`0x5a7cc`), and the slice records past the current slice count
   are simply left stale. `ave_session.c` already memsets; this is the reason,
   and it is not optional. **C.**
9. **No extra cache maintenance on the host.** The firmware's own unmap path
   does `dc civac` over the whole `0x22C60` mapping followed by `dsb sy`
   (`0x20bc4` → `0xb6f00` → `0xac49c` → `0xac474`/`0xac494`) before the
   completion is posted. With `dma_alloc_coherent` a `dma_rmb()` is all the
   host owes. **C.**

---

## 1. Where the coded bytes actually are

### 1.1 One write pointer, reset per frame

`CAVCController::ProcessTranscodeStart` (`0x581a8`) is called at the start of
every transcode. `x21 = pPicParams` = the `AVE_PICMGMT_PARAMS` block of the
`AVC_ENCODE` command (`ldr x21,[x20,#16]` `0x581f8`; a null there asserts
`"pPicParams"`, `0xc4440`, line 2615).

The **fresh-start** arm (`0x582f0`, taken when the per-context restart byte
`ctrl+0x2108+2·ctx` is zero, `cbz w10, 0x582f0` at `0x58240`):

```
582f0:  add  x10, x19, #0xa70
582f4:  movi v0.2d, #0
5830c:  str  wzr, [x20, #3708]     ; ctrl+8368  prev cumulative  = 0
58310:  str  d0,  [x19, #8312]     ; ctrl+8312  this-chunk bytes = 0
                                   ; ctrl+8316  earlier-chunk bytes = 0
58318:  str  w11, [x20, #3704]     ; ctrl+8364  slice count = 0
5833c:  str  wzr, [x19, #8376]     ; ctrl+8376  CABAC bin count = 0
58340:  str  wzr, [x19, #8356]     ; ctrl+8356  bytes skipped = 0
58364/58394: str w9, [x19, #8360]  ; ctrl+8360  = pPicParams->sOutput.CodedBufSize
                                   ;             (PICMGMT+0xC18, wire offset 3096)
583a4:  str  x8, [x19, #8328]      ; ctrl+8328  = pPicParams->sOutput.Coded
                                   ;             (PICMGMT+0xC08, wire offset 3080)
```

**C** for every line. The two `PICMGMT` offsets match `ave_abi.h`'s
`.out_coded = 0xc08` / `.out_coded_size = 0xc18` exactly, which is an
independent confirmation of both from the firmware side.

Two asserts guard it:

| assert | string | VA | meaning |
|---|---|---|---|
| `pPicParams->sOutput.Coded` | `0xc4a7e` | `0x5844c` (line 2656) | the IOVA may not be 0 |
| `pPicParams->sOutput.Coded == EncCommParams.bitstream_addr_dst[index]` | `0xc4a07` | `0x58404` (line 2649) | the per-frame IOVA must equal the `Start_AVC` `CodedData[out_index]` table entry |
| `pPicParams->sOutput.CodedBufSize > (minBufSize/2)` | `0xc4a4c` | `0x583bc` (line 2655) | `size > (3·ctrl[2700]·ctrl[2704]) >> 2` (`0x58358`–`0x58370`); with `ctrl+2700/2704` = width/height in pixels (**I**; `ctrl+2708/2712` are the same pair in MBs) that is `CodedBufSize > 3·W·H/4`, i.e. bigger than one 4:2:0 frame |

**C.** The middle one is the one worth remembering: **the per-frame
`out_coded` must be byte-identical to the entry the `Start_AVC` `CodedData`
table published at `out_index`**, or the firmware spins. `ave_session.c`
already satisfies it (`f.coded_addr = bufs->coded[n].iova`,
`f.coded_index = n`).

`SetTranscode` then hands the pointer and the size to the hardware:

```
592f0:  ldr  x10, [x19, #8328]
592f4:  cbz  x10, 0x593d4         ; SetTranscode:8053 assert
592fc:  str  w10, [x12]           ; regs - 0x134  <- low 32 bits of the write ptr
59300:  ldr  w10, [x19, #8360]
59310:  str  w10, [x11]           ; regs - 0x130  <- buffer size
59320:  str  w10, [x9]            ; regs - 0x140  <- 0x80030001 (enable)
```

**C.** Note the register takes only the **low 32 bits** of the IOVA — the same
32-bit truncation [53](53-first-frame.md) §28 noted for the source reader. A
coded buffer mapped above 4 GiB would silently write to the wrong place.

### 1.2 So: no offset, for a fresh frame

There is no header, no reserved prefix and no per-slice base table. The first
byte the hardware emits lands at `sOutput.Coded + 0`. **C.**

The **only** thing that shifts it is the restart arm of
`ProcessTranscodeStart` (`0x58244`), taken when the per-context restart byte
is set — a *context switch*, i.e. the encoder was pre-empted mid-frame and is
resuming:

```
58244:  ldr  x8,  [x19, #8328]        ; current write pointer
58250:  add  x10, x8, #0x3f
58254:  and  x10, x10, #~0x3f         ; round UP to 64
58258:  sub  x8,  x10, x8             ; how many bytes were skipped
58260:  str  w8,  [x19, #8356]        ; ctrl+8356 = bytes_to_be_removed
58264:  str  x10, [x19, #8328]        ; write pointer := aligned
```

logged as *"restart transcode frame_id %d bitstreamAddr %016llx,
bytes_to_be_removed: %d"* (`0xc4a98`). **C.** This is the *only* producer of a
non-zero `bytesToRemove`, and it explains the field's full name
(`ui32BytesToRemoveAtTheEndOfTheSliceForContextSwitch`) — see §2.2.

### 1.3 The firmware writes Annex B start codes; it does not write lengths

`WriteBits::nal_header` (`0x155c0`) emits `00 00 00 01` byte by byte
(`0x155d4`, `0x155e8`, `0x155fc`, `0x15610`/`0x15618`, `pos += 4` at
`0x15628`). **C** — this was already in [53](53-first-frame.md) §4.2.

The slice header goes to the hardware, not into the buffer by hand:
`CAVCController::GetSliceHeaderForFW(u8 *dst, u32 *outLen, u32 max)`
(`0x5fdd0`) `memcpy`s `(header_bits+7)/8` bytes from the controller's slice
header staging area (`ctx+0x580`) into the caller's buffer
(`bl 0x55e8` at `0x5fe8c`), optionally bit-reversing each byte through the
table at `0xced6d`. Its single call site is
`CAVCController::ConfigureMCPUs(AVE_PICMGMT_PARAMS*)` at `0x60588`, which is
programming the entropy MCPUs — so the *hardware* concatenates
`start code + slice header + slice data` into the coded buffer. **C** for the
call graph, **I** for "therefore the buffer holds complete NAL units" — but
[53](53-first-frame.md) §28 confirmed it empirically: F16/F17's `coded.bin`
began with a start code and `ffmpeg` decoded
`paramsets.bin || coded.bin` as Baseline 1280x720.

Emulation prevention is applied by the same writer (a NAL that ends in the
`rbsp_stop_one_bit` and contains no `00 00 00`/`00 00 01` is what a decoder
got in F16). **I**, on the strength of the decode.

**Do not insert AVCC lengths, and do not insert start codes of your own.**

### 1.4 The firmware tells you when it overflowed

At the end of `ProcessTranscodeDone`:

```
5bf64:  ldr  w9,  [x19, #8312]     ; bytes this chunk (from the HW counter)
5bf68:  ldr  w10, [x19, #8316]     ; bytes in earlier chunks
5bf6c:  ldr  w11, [x19, #8356]     ; bytes skipped for alignment
5bf70:  ldr  w8,  [x19, #8360]     ; CodedBufSize
5bf74:  add  w9, w10, w9
5bf78:  add  w9, w9, w11
5bf7c:  cmp  w9, w8
5bf80:  b.ls 0x5bf9c               ; fits
5bf84:  adr  x0, 0xc4ffa           ; "bitstream size overflow, buffer size: %d,
5bf88:  mov  w23, #0x4             ;  bitstream size: %d"
5bf90:  movk w23, #0xee, lsl #16   ; status := 0xEE0004
```

**C.** `w23` is the `AVE_FW_ERROR` that ends up in the completion (§6.2). The
firmware does **not** truncate or fault — it writes what it writes and tells
you afterwards, so a too-small buffer is memory corruption inside the
allocation plus a status code, not a fault.

---

## 2. The exact length

### 2.1 The two fields, and their real addresses

`AVE_RetrieveRCStats` reads them as `hdr[384 + 0x220·s]` and
`hdr[908 + 0x220·s]`. The *firmware* writes them as `record[0]` and
`record[524]` with `record = hdr + 0x180 + 0x220·s`
(`add x23, x20, #0x180` `0x5c294`, `umaddl x9, w9, w10(0x220), x23`
`0x5c2ac`, `str w10,[x9]` `0x5c2bc`, `strb w8,[x9,#524]` `0x5c2b4`). The two
descriptions are the same addresses:

| what | where | type | host evidence | firmware evidence |
|---|---|---|---|---|
| slice record base | `hdr + 0x180 + 0x220·s` | — | `add x22,x8,#0x180` after `+0x22000` `0xfffffe0008ec4e70`; step `0x220` `0xec4efc` | `add x23,x20,#0x180` `0x5c294`; step `0x220` `0x5c488` |
| `ui32BytesWritten` | `record + 0x000` | u32 | `ldr w9,[x25,#384]` `0xec4ec0` | `str w9,[x0]` `0x5c734`, `0x5c75c`, `0x5c780`, `0x5c7b8`, `0x5c7f8`, `0x5c830`, `0x5c85c`, `0x5c434` |
| `ui32BytesToRemoveAtTheEndOfTheSliceForContextSwitch` | `record + 0x20C` | **s8** | `ldrsb w9,[x25,#908]` `0xec4ee8`, sign-tested `tbnz w9,#31` `0xec4eec` | `strb w8,[x9,#524]` `0x5c2b4`; `strb wzr,[x…,#524]` on every normal record |
| HEVC-only extra | `record + 0x218` | u32 | `ldr w9,[x25,#920]` `0xec4ee0`, added **only** when `codec == 1` | — |

**C** for all four. `ave_abi.h`'s `slice_bytes_written = 0x180` /
`slice_bytes_removed = 0x38c` are these offsets expressed from `hdr`, and they
are correct.

### 2.2 What the trim actually is

Not trailing padding, not `cabac_zero_words`, not alignment of the frame. It
is the **gap the encoder skipped when it resumed after a context switch**, and
it is charged to the *previous* slice:

```
5c290:  ldr  w8, [x19, #8356]      ; bytes skipped by the restart arm (§1.2)
5c298:  cbz  w8, 0x5c2c4           ; nothing skipped -> nothing to remove
5c29c:  ldr  w9, [x19, #8364]      ; slice count so far
5c2a0:  cbz  w9, 0x5c36c           ; assert "EncCommParams.slice_count >= 1"
5c2ac:  x9 = hdr+0x180 + (count-1)*0x220     ; the LAST slice record
5c2b0:  ldr  w10, [x9]             ; its ui32BytesWritten
5c2b4:  strb w8, [x9, #524]        ; bytesToRemove := skipped
5c2b8:  add  w10, w10, w8
5c2bc:  str  w10, [x9]             ; ui32BytesWritten += skipped
5c2c0:  str  wzr, [x19, #8356]
```

**C.** So the skipped bytes are *included* in `ui32BytesWritten` (to keep the
running offsets correct) and *flagged* by `bytesToRemove` (so the payload
length is right). Two consequences a driver must not miss:

- `Σ written − Σ removed` is the correct **total payload length**, which is
  what `ave_cmd_coded_length()` computes. **C.**
- The payload is **not contiguous** when any `bytesToRemove` is non-zero:
  there is a hole of `removed(s)` junk bytes at the end of slice *s*. A driver
  that `memcpy`s `coded[0].cpu` for `info.bytes` bytes emits the junk and
  truncates the tail. **C.** This cannot happen in a single-context,
  single-slice session (`ctrl+8356` is written to zero by the fresh-start arm
  and never set), which is why our first frame works.

`bytesToRemove < 0` is the firmware's corruption signal; the kext bails and
logs (`0xec5078`), and `ave_cmd_coded_length()` returns `-EPROTO`. **C.**

### 2.3 Which records are valid, and how many

The firmware maintains the count in `EncCommParams.slice_count` =
`ctrl+8364`, which is **zeroed at `ProcessTranscodeStart`** (`0x58318`) and
incremented once per record as `CollectDataFromTranscode` fills it
(`str w1,[x19,#8364]` and friends, `0x5c73c`/`0x5c764`/`0x5c788`/`0x5c7c0`/
`0x5c804`/`0x5c83c`/`0x5c86c`). It is asserted non-zero at `0x5c3c8`
(`"EncCommParams.slice_count > 0"`, `0xc7292`, line 8736).

**`ctrl+8364` is not host visible.** So:

- **The first valid record is record 0**, at `hdr+0x180`, always. **C.**
- **The number of valid records is not carried in the header.** Apple's own
  host code recovers it exactly the way we do — walk until
  `ui32BytesWritten == 0` — and reports it as
  `S_AVE_DRC_FrameStats.nSlices` (`0xec4ecc`–`0xec4ed0`), printed as
  *"NumberOfSlicesEncoded %d"*. **C.**
- That walk is only sound because **the buffer was zero before the frame**.
  The firmware overwrites records `0 … count-1` and leaves the rest alone, so
  a frame with fewer slices than its predecessor reads the predecessor's
  records. `ave_session.c`'s memset is therefore **required**, not insurance.
  **C** — this closes the "could not prove it is needed" note in
  [64](64-multiframe.md) §8.
- A cheap independent cross-check exists: the `ENCODE_DONE` completion's
  word at `+0x40` is logged by the firmware as `slice # = %d`
  (`ProcessEncDone` `ldr w10,[x22,#12]` `0x14e2c`, format `0xbdc14`). **C**
  for the string; **U** for whether it is the count or the index of the last
  slice. Worth logging and comparing.

### 2.4 Is there a total-length field? No, and the ones that look like it are not host visible

| candidate | where | why not |
|---|---|---|
| `ctrl+8312` | this chunk's byte count, read straight from the hardware counter register (`ldr w8,[x8,#12]` at `0x5bea8`, base `0x12EA08C`) and stored at `0x5beb8` | controller memory; never copied into the coded header |
| `ctrl+8316` | running total over earlier chunks (`0x5bea4`) | same |
| `ctrl+8328` | the write pointer, advanced by the sum (`str x10,[x19,#8328]` `0x5c874`) | same |

The firmware logs the number a driver wants — *"CAVCController::
ProcessTranscodeDone:: Encoder %d frame_num %d size %d"* (`0xc4e97`,
`0x5bec4`) — and then keeps it. **C.** The summation over slice records is
unavoidable.

### 2.5 The missing term: `numCABACzeroWordInserted` at `hdr+0xF0`

This is the one real bug in the current length computation.

H.264 §7.4.2.10 bounds the CABAC bin count of a picture by
`(32/3)·NumBytesInVclNALunits + (RawMbBits·PicSizeInMbs)/32`, and requires the
encoder to pad with `cabac_zero_word`s (each three bytes, `00 00 03`, in the
byte stream) until the bound holds. **The AVE firmware computes the count and
leaves the insertion to the host.**

`ProcessTranscodeDone`, `0x5c108`–`0x5c20c`:

```
5c114:  ldr  w8,  [x8, #16]        ; HW bin counter, base 0x12EA08C
5c12c:  str  w8,  [x19, #8376]     ; ctrl+8376 += bins
5c130:  cbz  w9, 0x5bf58           ; only at frame_done
5c13c:  bl   0x24774               ; MBChromaStride -> raw MB bits
5c140:  w11 = ctrl[8312] + ctrl[8316]           ; NumBytesInVclNALunits
5c158:  w8  = pic_width_in_mbs * pic_height_in_mbs      ; ctrl+2708, ctrl+2712
5c15c:  w10 = (rawMbBits << 4) + 0x800
5c160:  w12 = (w11 << 5) / 3                    ; (32/3)*numBytes
5c17c:  w12 = w12 + (w8 * w10) >> 5             ; + RawMbBits*PicSizeInMbs/32
5c180:  subs w14, ctrl[8376], w12
5c184:  b.ls 0x5bf58                            ; within the bound, nothing to do
5c1a0:  w24 = (w14 / 32) + 1                    ; == ceil(excess / 32)
5c18c:  log 0xc4f6d  "*** frame %d bin count violation %d max %d rawMbBits %d
                      picSizeInMbs %d numBytes %d ***"
5c1b8:  log 0xc4fc6  "*** bin count violation insert %d CABAZ zero words"
5c1cc:  map the coded-header buffer again
5c200:  str  w24, [x0, #240]       ; CODED_DATA_HDR + 0xF0
```

**C** for every line. Each `cabac_zero_word` buys 32 bins (3 bytes × 32/3), so
`(excess/32)+1` is exactly the spec's minimum.

The host side names it. `AVE_RetrieveRCStats`:

```
fffffe0008ec4fa8:  ldr w8, [x20, #240]     ; hdr + 0xF0
fffffe0008ec4fac:  add w8, w8, w8, lsl #1  ; × 3  -> bytes
fffffe0008ec4fb0:  str w8, [x19, #400]     ; S_AVE_DRC_FrameStats + 400
```

and `AVE_PrintRCStats` prints `S_AVE_DRC_FrameStats+400` as
*"numCABACzeroWordInserted %d"* (`ldr w8,[x20,#400]` `0xfffffe0008ec4530`,
strings `0xfffffe00071ef743` / `0x…760`). **C.**

Three things follow:

1. `frameBytes` (`FrameStats+0`) is `Σ written − Σ removed` and **excludes**
   the zero words (`str w9,[x8],#60` `0xec4f10`, separate from the `+400`
   store). So the encoded frame the host must emit is
   `Σ written − Σ removed + 3 × hdr[0xF0]` bytes. **C** for the arithmetic;
   **I** for "and the host is the one that must write them" — the inference
   rests on the hardware byte counter (`ctrl+8312`) being sampled *before* the
   cabac computation and the firmware then *adding* `3 × count` to it for the
   rate-control bit total (`add w9,w9,w9,lsl#1; add w9,w9,w12` `0x5c4c4`–
   `0x5c4cc`), which only makes sense if the bytes are not in the buffer.
2. It is zero for CAVLC, so our Baseline I-frame is unaffected today. A
   CABAC (Main/High) encode at low QP on small pictures is exactly where it
   bites.
3. `hdr+0xF0` is only written **when a violation occurred**, plus a
   `str wzr,[x20,#240]` on the LRME collection path (`0x268e0`). Zero the
   buffer and read it; do not assume the firmware initialised it.

---

## 3. Multi-slice layout — one buffer, contiguous, offsets are a prefix sum

`CAVCController::CollectDataFromTranscode(CAVEControllerTranscodeDoneCmd*)`
(`0x5c210`) is where the per-slice records are produced, and it answers the
question [66](66-ratecontrol-sizing.md) §6.1 left open ("so where does slice 2
go?").

The hardware publishes an array of **cumulative byte offsets** in its own
register file and the firmware differences it:

```
5c2ec:  x9  = *(0x21a7b8)                  ; MMIO window base
5c2f0:  x10 = x9 + 0x12EA0B4
5c2f4:  ldr  w10, [x10, #68]               ; 0x12EA0F8 = number of entries
5c2f8:  str  w10, [x21, #4]                ; cmd->count
5c304:  loop: x8[i] = *(u32*)(x9 + 0x12EA0B4 + 4·i)     ; copy them out
5c32c:  ldr  w9, [x8]                      ; entry[0] = start offset (0 on a
5c334:  str  w9, [x19, #8368]              ;            fresh frame)
5c708:  for k = 1 .. n-1:
          record[count + k - 1].ui32BytesWritten = entry[k] - entry[k-1]
          record[…].bytesToRemove = 0
          ctrl+8368 = entry[k]             ; prev
          ctrl+8364 = index + 2            ; slice count
          x10 += difference                ; the running write pointer
5c874:  str  x10, [x19, #8328]
5c41c:  record[count-1].ui32BytesWritten = ctrl[8312] - entry[n-1]
5c438:  record[count-1].bytesToRemove = 0
5c44c:  ctrl+8328 += that
```

**C.** Then a debug loop prints every record as *"slice %d offset %d"*
(`0xc72c3`, `0x5c46c`–`0x5c490`).

So, for a driver:

| question | answer | conf |
|---|---|---|
| one buffer or several? | **one** — the single `sOutput.Coded` buffer; there is exactly one write pointer (`ctrl+8328`) and it only moves forward | C |
| where does slice *s* start? | `Σ_{t<s} ui32BytesWritten(t)` from the buffer base | C |
| how long is slice *s*? | `ui32BytesWritten(s) − bytesToRemove(s)` | C |
| gaps? | only `bytesToRemove(s)` junk bytes at the end of slice *s*, and only after a context-switch restart (§2.2) | C |
| does anything in the size table change with `iNum`? | no — [66](66-ratecontrol-sizing.md) §6 and §6.1, re-confirmed: no `AVE_CalcBufSizeOf*` takes a slice count and `AVE_CalcBufNumOfSliceHeader` returns 0 for AVC | C |
| how many records can there be? | 256 (`cmp x24,#0x100` `0xfffffe0008ec4f00`); the kext refuses `iNum > 32` (`0x1ecfaa`) | C |

**Therefore the whole-frame bitstream is `coded[0..Σwritten)` with the
`bytesToRemove` holes elided — which for every session we have run so far is
simply `coded[0 .. Σwritten)`.** The concatenation the driver does today is
correct for `bytesToRemove == 0` and wrong otherwise.

What is **not** established: whether `CollectDataFromCpusMultiCore`
(`0x49b38`) / `UpdateTranscodeDataMultiCore` (`0x494c4`) and the
`TranscodedData` ×2 surfaces ([66](66-ratecontrol-sizing.md) §5.2) put slices
somewhere else on a multi-SVE-core split. They are a distinct mechanism from
`sSliceMap.iNum`; no path from them into `sOutput.Coded` was traced. **U.**

---

## 4. SPS and PPS: written once, at Start; the length is in the coded header

### 4.1 One producer, reachable only from Init

The whole image contains exactly one call to `SPS::seq_parameter_set_rbsp`
(`0x194a0`, called at `0x5dda4`) and one to `PPS::pic_parameter_set_rbsp`
(`0x1997c`, called at `0x5ddb0`). Both are inside
`CAVCController::InitEncodingParameters` (`0x5c9c8`). **C** — a whole-image
`bl` scan.

`InitEncodingParameters` is virtual, slot `+0x218` of the `CAVCController`
vtable (entry at `0xed520`, vtable base `0xed308`). It has exactly two call
sites in `CAVCController`, both `ldr x8,[x8,#536]; blr x8` (a whole-image scan
for that immediate finds only these two plus the `CHEVCController` mirror at
`0x63c3c`/`0x681ac` and a handful of unrelated stack loads):

| VA | caller | when |
|---|---|---|
| `0x466f4` | `CAVCController::ResetBetweenPasses` (`0x46468`) | multi-pass restart |
| `0x4de40` | `CAVCController::ProcessInit` (`0x4dde8`) | the `AVC_INIT` (`Start_AVC`) command |

**C.** There is no per-frame and no per-IDR call. **The parameter-sets buffer
is written exactly once per `Start_AVC`.**

### 4.2 `update_param_sets` does nothing on 13.5

`PICMGMT+0x6F1` is recorded in `ave_abi.h` as `update_param_sets`, sourced
from the kext's log line
*"bUpdateParameterSets %d scalingMatrixMode %d forceNonRefFrame %d
forceKeyFrame %d bInputCompressed %d"* (`0xfffffe00071e91d3`) — i.e. from a
name list, not from a store. A scan of the firmware for any load at that
offset (`ldrb wN,[xM,#1777]`) finds **none**; the only `#0x6f1` immediates in
the image are two assert line numbers (`0x9448`, `0x123c0`). **C** for the
absence of a reader *at that offset*; **I** for "the flag is inert", because
`PICMGMT` is copied into the controller and a read could be at a shifted
offset — but §4.1 already settles the behavioural question: nothing
regenerates the parameter sets per frame, so there is nothing for the flag to
trigger.

Set it to 0 and forget it. If a driver ever wants new parameter sets
(resolution change, profile change), the mechanism is a new `Start_AVC`.

### 4.3 The buffer, and its one size check

```
5df28:  ldr  x20, [x23, #880]       ; Start_AVC wire 0xFB30, p_ParameterSetsBuffer
5df38:  bl   0x20bc0                ; MappedMemory
5df44:  bl   0x20c00                ; HwToTarget -> CPU pointer
5df48:  ldr  w8,  [x24, #1196]      ; SPS length in BITS
5df5c:  bl   0x55e8                 ; memcpy(base,               ctrl+0x2c6a8, sps_bits>>3)
5df6c:  add  x0,  x20, x8, lsr #3
5df70:  ldr  w8,  [x24, #1840]      ; PPS length in BITS
5df78:  bl   0x55e8                 ; memcpy(base + sps_bytes,   ctrl+0x2c92c, pps_bits>>3)
5df7c:  bl   0x20bc4                ; unmap (clean+invalidate, dsb)
5df84:  w8 = [x24,#1196] + [x24,#1840]
5df9c:  str  w8, [x19, #2740]       ; ui32_SPSPPSHeaderBits
```

**C.** SPS bytes immediately followed by PPS bytes, at offset 0. Both are
Annex B with 4-byte start codes (§1.3).

The gate in front of it, which [66](66-ratecontrol-sizing.md) §7 noticed in
passing, is worth stating properly:

```
5de40:  ldr  w8, [x19, #2740]       ; the PREVIOUS ui32_SPSPPSHeaderBits
5de44:  ldr  w2, [x23, #888]        ; Start_AVC wire 0xFB38, buffer SIZE
5de48:  cmp  w2, w8, lsr #3
5de4c:  b.hs 0x5df28                ; big enough -> copy
5de50:  sub  w27, w27, #0x2         ; else status 0xEE0003, no copy
```

**C.** On the *first* `Start_AVC` of a session `ctrl+2740` is 0, so the check
is vacuous and a zero `ParameterSetsBufferSize` is accepted — which is why we
have got away with over-allocating and never setting it carefully. On a
**second** `Start_AVC` in the same session the check is live against the
previous frame's value. Send a real size (Apple's `AVE_CalcBufSizeOfParameterSet`
returns **512** for AVC, `0xfffffe0008ea4bb4`).

Also gated: `[x24,#1192]` (SPS `fw_creates_header`) and `[x24,#1836]` (PPS
`fw_creates_header`) must **agree**; `0` + `1` or `1` + `0` jumps to the error
exit at `0x5ed4c` (`cbz w10, 0x5de3c` / `cbnz w8, 0x5ed4c`). **C.**

### 4.4 There is no host-visible length for that buffer, because macOS never reads it

The kext builds the parameter sets **itself**. `AVC_SPS::seq_parameter_set_rbsp
(AVE_SyntaxWriter*)` is at kext `0xfffffe0008f45bd8` and
`AVC_PPS::pic_parameter_set_rbsp(AVC_SPS*, AVE_SyntaxWriter*, int)` at
`0xfffffe0008f4671c` — the same H.264 writer that is statically linked into
the firmware, linked into the kext too. `AVE_Client_UpdateParameterSet
(_S_AVE_Client*, u8 *out, u32 *outLen, _S_AVE_PSInfo*)`
(`0xfffffe0008ec0c0c`) serves userspace from those host-side objects:

```
fffffe0008ec0c7c:  ldr  x8,  [x24]          ; the client's AVC_SPS object
fffffe0008ec0c80:  ldr  w20, [x8, #1204]    ; SPS length in BITS
fffffe0008ec0c8c:  cmp  w9, #0x201          ; >>3 must be < 513 bytes
fffffe0008ec0c94:  add  x1, x8, #0x4b8      ; the SPS byte buffer
fffffe0008ec0ca0:  bl   memcpy
fffffe0008ec0ca8:  stp  w23, w8, [x21, #16] ; PSInfo: length, type 3
fffffe0008ec0cb0:  ldr  x8, [x24, #8]       ; the client's AVC_PPS object
fffffe0008ec0cb4:  ldr  w9, [x8, #128]      ; PPS length in BITS (< 0x808)
fffffe0008ec0cd0:  add  x1, x8, #0x84
fffffe0008ec0cd8:  bl   memcpy
fffffe0008ec0cdc:  str  w24, [x21, #32]
```

**C.** So `p_ParameterSetsBuffer` is the *firmware's* copy, for the firmware's
own use, and Apple's driver has an exact length because it generated the bytes.
We do not. **U** for "a host-visible length of that DMA buffer" — there is
none.

### 4.5 …but `ui32_SPSPPSHeaderBits` is exact, and we already have it

`ctrl+2740` is set to `sps_bits + pps_bits` (`0x5df9c`) — literally the sum of
the two `memcpy` lengths shifted back up by 3 — and is copied verbatim into
every frame's coded header at `+0x98`, by both
`CollectDataFromCpus` (`str w8,[x20,#152]` `0x5a7d4`) and
`ProcessTranscodeDone` (`str w8,[x0,#152]` `0x5be18`). **C.**

Both NAL units are byte aligned by construction (`rbsp_trailing_bits`), so
each bit count is a multiple of 8 and

```
psets_len = coded_hdr.sps_pps_bits / 8          (exact)
```

**C** for the arithmetic, **I** for the byte-alignment premise — which is
cheap to assert (`sps_pps_bits % 8 == 0`) and was true in F16 (21 bytes,
168 bits).

`ave_session_psets_len()`'s back-scan can be demoted to a cross-check. It is
also *not* safe in general: it depends on the buffer being zero before Start
and on nothing shorter ever being written into it afterwards. §4.1 says a
second `Start_AVC` in the same session writes the buffer again, and a shorter
SPS+PPS (a smaller `pic_width_in_mbs_minus1`, say) would leave the old tail in
place and the scan would over-report. **C.**

---

## 5. `CODED_DATA_HDR`, field by field

`0x23000` bytes (`AVE_CalcBufSizeOfCodedHeader` `0xfffffe0008ea4fb8`), of which
the firmware maps and touches `0x22C60` (`mov w2,#0x2c60; movk w2,#0x2,lsl#16`
at `0x59f50`, `0x5bde4`, `0x5c240`, `0x5c8fc`, `0x26828`). Layout:
`0x000` header block, `0x180 + 256·0x220` slice records, `0x22180` trailer.

Names in the first column are Apple's own where a format string supplies one;
`—` means the field is written and/or consumed but never printed by name.

| offset | name | type | written by | read by | meaning | conf |
|---|---|---|---|---|---|---|
| `0x000` | `ui32_I_MbCnt[4]` | u32[4] | `CollectDataFromCpus` `0x5a56c` (**+=**) | `AVE_PrintCodedHeader` `0xeb6728`; `RetrieveRCStats` `0xec4f20` | intra MBs, by a 4-way sub-index | C |
| `0x010` | `ui32_P_MbCnt[4]` | u32[4] | `0x5a57c` (**+=**) | `0xeb681c` | inter MBs | C |
| `0x020` | `ui32_Skip_MbCnt[4]` | u32[4] | `0x5a58c` (**+=**) | `0xeb6918` | skipped MBs | C |
| `0x030` | — | u32[4] | `0x5a59c` (**+=**) | copied to `FrameStats` | further MB tallies | C offset, **U** meaning |
| `0x040` | — | u32[4] | `0x5a5ac` (**+=**) | same | | C / **U** |
| `0x050` | — | u32[4] | `0x5a5b8` (**+=**) | same | | C / **U** |
| `0x060` | — | u32[4] | the SIMD arm of the same loop (`0x5a5c0`+); not isolated to an instruction | `RetrieveRCStats` `0xec4f54` | | C (it is read) / **U** (writer, meaning) |
| `0x070` | `ui32_B_MbCnt` | u32 | — | `0xeb6a1c` | B MBs | C |
| `0x078` | — | u32 | `0x5a7a8` (**+=**) | `FrameStats+136` | | C / **U** |
| `0x07C`,`0x084`,`0x08C`,`0x090` | — | u32/u64 | `0x5a7a8`, `0x5a7c8`, `0x5a7cc` (**+=**, one is a 64-bit NEON add) | `FrameStats+140…164` | accumulated bit/complexity statistics | C / **U** |
| `0x098` | **`ui32_SPSPPSHeaderBits`** | u32 | `0x5a7d4`, `0x5be18` | `0xeb6b24`; `FrameStats+132` | `sps_bits + pps_bits` from `Start_AVC`, constant for the session | **C** |
| `0x0F0` | **`numCABACzeroWordInserted`** | u32 | `0x5c200` (only on a violation); zeroed on the LRME path `0x268e0` | `RetrieveRCStats` `0xec4fa8`, ×3 into `FrameStats+400` | `cabac_zero_word`s the **host** must append; 3 bytes each | **C** |
| `0x10C` | **`FrameNumberFromDriverReturned`** | u32 | `0x5a7e8`, `0x5be30`, `0x268b0` | `0xeb6c2c` | echo of `PICMGMT.frameNumber`; the kext asserts equality (`0x1ec86b`) | **C** |
| `0x110` | **`FrameTypeReturned`** | u32 | `0x5a7f0`, `0x5be3c`, `0x268c0`; **`= 4`** on the drop paths `0x13340` and `0x5c930` | `0xeb6d34`; `FrameStats+8`; the fw itself tests `== 4` at `0x14bbc` | frame type; **4 = the frame was dropped, there is no bitstream** | **C** |
| `0x114` | — | u8 | `0x5a7fc`, `0x5be4c`, `0x268d0` | — | copied from `ReferenceFrameInfoData[ctx]` (`ctrl+0xa70 + 424·ctx + 6528`), the same byte `AVE_H264_PrepareSliceHeader` reads at `0x20dfc` and `ManageDPBBuffer` fills | C provenance, **U** meaning (reference / IDR marker) |
| `0x118`…`0x163` | — | 7×u32 at stride 16 | `0x5a354`–`0x5a4a0` | `FrameStats+312` | per-engine counters read straight out of the pipe registers | C / **U** |
| `0x160` | — | u32[4] | `0x5a4a0` | `FrameStats+384` | | C / **U** |
| `0x170` | — | u32 | `0x5a218` | `FrameStats+300` | `(accumulated engine count) / (picW·picH)` | C / **U** |
| `0x174` | — | u32 | `0x5a220` | `FrameStats+304` | from `ctrl+4988` | C / **U** |
| `0x178` | — | u32 | `0x5a228` | `FrameStats+308` | from `ctrl+4996` | C / **U** |
| `0x180 + 0x220·s` | **`ui32BytesWritten`** | u32 | §3 | `0xec4ec0` | bytes the hardware wrote for slice *s*, **including** any restart gap | **C** |
| `0x38C + 0x220·s` | **`ui32BytesToRemove…ForContextSwitch`** | **s8** | §2.2 | `0xec4ee8` (`ldrsb`, negative = corrupt) | restart gap at the end of slice *s* | **C** |
| `0x398 + 0x220·s` | — | u32 | — | `0xec4ee0`, added to the total **only for HEVC** | | **C** |
| `0x221A4` | — | u32 (pair read as u64) | `str w9,[x23,#36]` `0x59fe8` from MMIO `0x140810C+368` | `FrameStats+440` | | C / **U** |
| `0x221A8` | — | u32 | `str w8,[x23,#40]` `0x59ff0` from `+372` | (same u64) | | C / **U** |
| `0x221AC`…`0x221B3` | — | 8 bytes, **byte-swapped** | `0x5a814`–`0x5a848` | `FrameStats+448` | written a byte at a time in reverse order — a CRC or digest | C / **U** |
| `0x221B4`,`…+8`,`+16`,`+20`,`+28` | — | u32 | `CollectDataFromLrme` `0x268d8`+ | `FrameStats+404…436` | LRME statistics | C / **U** |
| `0x221E0` | — | struct | — | `CRateControl::computeCorrCoeff` `0x48e8c` | correlation-coefficient scratch | C / **U** |

Things a driver should surface or check that it does not today:

- **`FrameTypeReturned == 4` means the frame was dropped.** Two independent
  producers write it (`CFlowControllerBase` `0x13340` and
  `CAVCController::HandleDPBForFrameDrop` `0x5c930`) and the firmware's own
  consumer tests for it (`cmp w8,#4` `0x14bc0`). A drop produces **no**
  bitstream and the slice records stay whatever they were. **C.**
- **`FrameNumberFromDriverReturned` must equal the `frameNumber` we sent.**
  Apple asserts it (`0x1ec86b`). It is the only per-frame identity check
  available and it costs one comparison. **C.**
- `ui32_I_MbCnt` + `ui32_P_MbCnt` + `ui32_Skip_MbCnt` summed over the four
  sub-indices should equal `pic_width_in_mbs · pic_height_in_mbs`. For the
  F16/F17 class of failure — "the pipeline completed and encoded nothing" —
  that is a one-line sanity check the driver can make itself: an all-I,
  all-DC frame is `I == total, P == 0, Skip == 0`. **I** (the identity is not
  asserted anywhere in the binaries).
- Per-frame QP is **not** in the coded header. No field in the table above is
  a QP, and [66](66-ratecontrol-sizing.md) §3.3 already established there is
  no per-frame QP writer in `CAVCController` at all on 13.5. **C.**

---

## 6. Completion, reuse, and cache

### 6.1 The order inside `ProcessTranscodeDone` (`0x5bd54`)

```
5bdf8   map the coded-header buffer (0x22C60)
5be18   hdr+0x98  = ui32_SPSPPSHeaderBits
5be30   hdr+0x10C = FrameNumberFromDriverReturned
5be3c   hdr+0x110 = FrameTypeReturned
5be4c   hdr+0x114 = ReferenceFrameInfoData byte
5be50   read the transcode status register (0x12EA08C); bit 24 set ->
        log "Transcode Error! code 0x%08x value 0x%08x" and status := 0xEE0007
5bec8   compute frame_done = (currMbRow == pic_height_in_mbs), store at
        ctrl+0x2109+2·ctx; log "ProcessAvcTranscodeDone currMbRow %d
        pic_height_in_mbs %d frame_done %d"
(5c108) if not frame_done: accumulate the CABAC bin count, and at frame end
        compute and store numCABACzeroWordInserted (§2.5) through its own
        mapping, which is unmapped (and cleaned) at 5c208
5bf60   CollectDataFromTranscode  -> fills the slice records through its own
                                     mapping, unmapped at 0x5c6b0
5bf7c   overflow check -> status := 0xEE0004
5c06c   SendNotification(type 1, 24 bytes {frame_num, frame_done, status})
5c0dc   unmap the outer mapping
```

**C.**

### 6.2 The completion carries the status, and the status is meaningful

`CFlowControllerBase::ProcessEncDone` (`0x14c50`) turns that notification into
`NotificationToHost(0x0E06, clientId, error, {PlaneNumber, slice#})`
(`0x14e70`–`0x14e74`), and `NotificationToHost` (`0x13684`) writes the error
word to **reply `+0x38`** (`str w23,[x22,#56]` `0x136fc`) — the offset
`ave_abi.h` already has as `.status`. **C.**

| status | set at | meaning |
|---|---|---|
| `0xEE0000` | `0x5be58` | the transcode completed cleanly |
| `0xEE0007` | `0x5be88` (`orr w23,w9,#3` over `0xEE0004`) | the hardware raised a transcode error; the code/value pair is at registers `0x12EA0AC` / `0x12EA0B0` and is logged, not returned |
| `0xEE0004` | `0x5bf88` | **the bitstream overflowed the coded buffer** — `written + skipped > CodedBufSize` |

**C.** F16/F17 observed `0x0E06` with `0xee0000`
([53](53-first-frame.md) §28), which is the success value this path sets —
independent confirmation that the reply status is this word.

`ave_cmd_check_reply()` already returns `-EIO` for anything but `0xEE0000`, so
the driver *fails* correctly today; it just cannot say why. Name the two
codes.

### 6.3 When is the buffer safe to read?

The `ENCODE_DONE` reply is sufficient, with one caveat and one nuance:

- **Caveat.** Everything a driver reads out of the coded header was written
  through a mapping that was unmapped — and therefore `dc civac`'d and
  `dsb sy`'d — *before* the notification was sent: `CollectDataFromCpus`
  unmaps at `0x5b7c8`/`0x5b9fc`, the CABAC block at `0x5c208`,
  `CollectDataFromTranscode` at `0x5c6b0`, all before `0x5c06c`. The only
  stores whose clean happens *after* the notification are
  `0x5be18`/`0x5be30`/`0x5be3c`/`0x5be4c` in the outer mapping — and those
  four fields were already written and cleaned by `CollectDataFromCpus`
  (`0x5a7d4`/`0x5a7e8`/`0x5a7f0`/`0x5a7fc`). **C** for the ordering, **I**
  for "so the race is benign", which depends on `CollectDataFromCpus` running
  every frame.
- **Nuance.** `ProcessTranscodeDone` runs **once per transcode chunk**, not
  once per frame; `frame_done` is what distinguishes the last one, and the
  slice records and the write pointer accumulate across chunks. A single
  non-pre-empted frame produces one. A driver that starts sharing the
  engine must gate on something stronger than "a reply arrived": the
  available signals are `FrameNumberFromDriverReturned == our frame number`
  and `Σ written > 0`. **C** for the mechanism; **U** for whether a
  non-final chunk produces a host-visible reply at all.

### 6.4 Cache maintenance

`MappedMemory`/`HwToTarget`/unmap are `0x209d4` / `0x20c00` / `0x20bc4`. The
unmap path is the interesting one:

```
20bc4: ldr x8,[x0,#24]; cbz x8, ret      ; only if the mapping has a length
20be4: bl 0xb6f00 (mode 1)
 b6f38:   bl 0xac49c
  ac4c4:    b 0xbb3a4  -> b 0xac440 with clean=1, invalidate=1
   ac474:     dc civac, x9      ; over [base, base+len), cache-line stride
   ac494:     dsb sy
20bec: bl 0xb7108                        ; release the mapping slot
```

**C.** That is the *coprocessor's* caches. On our side the coded and
coded-header buffers come from `dma_alloc_coherent`, so nothing further is
required beyond the `dma_rmb()` `ave_session.c` already issues before reading
the header. **C** for the firmware side, **I** for the host-side conclusion.

### 6.5 Safe to reuse

A coded buffer is reusable once its frame's completion has been processed:
`ProcessTranscodeStart` re-points `ctrl+8328` at `sOutput.Coded` at the start
of the next transcode and the hardware never reads the buffer back. The
constraint that *does* bind is the one from
[64](64-multiframe.md) §3: the per-frame `out_coded` must match the
`Start_AVC` `CodedData[out_index]` entry, so reuse means reusing a slot from
that table, not handing over a fresh allocation. **C** (the assert at
`0x58404`).

---

## 7. What the driver should change

Concrete, in the order they matter.

### 7.1 `driver/ave_abi.h` — two new offsets

```c
struct ave_coded_hdr_layout {
	...
	u32	cabac_zero_words;	/* u32, count; 3 bytes each */
	...
};
```

13.5: `.cabac_zero_words = 0xF0` (fw `str w24,[x0,#240]` `0x5c200`; kext
`ldr w8,[x20,#240]` `0xfffffe0008ec4fa8`). 26.6.2: `AVE_OFF_NONE` until read.

And a named constant for the drop marker:

```c
#define AVE135_FRAME_TYPE_DROPPED	4	/* fw 0x13340, 0x5c930; tested 0x14bc0 */
```

### 7.2 `ave_cmd_coded_length()` — three corrections

1. **Report the per-slice geometry, not just a total.** The caller needs
   `offset[s]` and `len[s]`, because the payload is discontiguous whenever
   `bytesToRemove != 0`:

```c
struct ave_coded_slice { u32 off, len; };	/* off = prefix sum of written */

struct ave_coded_info {
	u32	bytes;			/* Σ (written - removed) */
	u32	span;			/* Σ written - how far the fw wrote */
	u32	slices;
	u32	bytes_removed;
	u32	cabac_zero_words;	/* append 3 bytes each */
	u32	frame_type, frame_num, sps_pps_bits;
	struct ave_coded_slice	slice[AVE_CODED_SLICES_MAX];
};
```

   with `off` accumulated as `written` (not `written - removed`) and
   `len = written - removed`.
2. **Read `cabac_zero_words`** when the ABI has the offset, and have the
   session append `3 × n` bytes of `00 00 03` after the last slice. Today a
   CABAC stream that needs them is silently non-conforming.
3. **Bound the walk by the buffer, and reject the stale-record case
   explicitly.** `hdr_len` is already checked against `min_bytes`; also refuse
   when `span > coded_size`, which is the cheap detector for "we read a
   previous frame's records".

The existing `-EPROTO` on a negative trim is right and matches Apple
(`0xec5078`). The `> 0x40000000` guards are ours and can stay.

### 7.3 `ave_session.c` — the stream assembly

`ave_session_publish()` today does

```c
memcpy(p, bufs->psets_cpu, psets_len);        /* length from a back-scan */
for each frame: memcpy(p, coded[i].cpu, coded[i].len);
```

It should do

```c
if (need_psets) memcpy(p, psets_cpu, info.sps_pps_bits / 8);
for each frame:
	for (s = 0; s < info.slices; s++)
		memcpy(p, coded[i].cpu + info.slice[s].off, info.slice[s].len);
	for (k = 0; k < info.cabac_zero_words; k++)
		memcpy(p, "\x00\x00\x03", 3);
```

and

- **replace `ave_session_psets_len()`'s result with
  `info.sps_pps_bits / 8`**, keeping the back-scan only as a cross-check that
  logs a warning on a mismatch. The back-scan is wrong by construction on the
  second `Start_AVC` of a session (§4.5);
- **warn if `info.sps_pps_bits % 8`** — that would mean the premise is wrong;
- **set `ParameterSetsBufferSize` (`Start_AVC` wire `0xFB38`) to 512** rather
  than leaving it zero, because the check at `0x5de44` becomes live on the
  second Start (§4.3).

### 7.4 Checks we do not make and should

| check | evidence | why |
|---|---|---|
| `info.frame_type == 4` → "the firmware dropped this frame" | fw `0x13340`, `0x5c930`; kext tests it at `0x14bc0` | today a dropped frame looks like "zero coded bytes", indistinguishable from a dead pipe |
| `info.frame_num == the frameNumber we sent` | Apple's own assert, kext `0x1ec86b` | the only identity check available; catches a completion matched to the wrong frame |
| reply status `0xEE0004` → "coded buffer too small", `0xEE0007` → "hardware transcode error" | fw `0x5bf88`, `0x5be88` | `ave_cmd_check_reply()` already returns `-EIO`; say which |
| `Σ I+P+Skip MbCnt == mbW·mbH` | §5 | the F16/F17 failure mode ("completed, encoded nothing") is visible here |
| `info.span <= coded[n].size` | §1.4 | the host-side mirror of the firmware's own overflow test |
| log reply `+0x40` (`slice #`) next to `info.slices` | fw log `0xbdc14` | an independent slice count, free |
| the coded buffer's IOVA must be 32-bit clean | `str w10,[x12]` `0x592fc` — the register takes 32 bits | same trap as the source reader ([53](53-first-frame.md) §28) |

### 7.5 What does **not** need changing

- `slice_stride 0x220`, `slice_max 0x100`, `slice_bytes_written 0x180`,
  `slice_bytes_removed 0x38c`, `frame_type 0x110`, `frame_num 0x10c`,
  `sps_pps_bits 0x98`, `min_bytes 0x22c60` in `ave_abi.h` are all correct and
  now confirmed from the firmware's writer side as well as the kext's reader
  side.
- The `memset` of the coded header before each `Process` — keep it, and
  upgrade the comment from "cheap insurance" to "required": the MB counters
  are read-modify-write and the slice records past the current count are
  stale (§2.3, §5).
- Prepending the parameter sets only when the coded buffer does not already
  start with a `nal_unit_type == 7` — keep. Nothing found here contradicts
  [53](53-first-frame.md) §4.3's inference that the parameter sets are not in
  the coded buffer, and the guard costs nothing.

---

## 8. Annotations owed to earlier documents

- [53](53-first-frame.md) §3.2 — the formula is right; it is **incomplete**.
  Add `+ 3 × numCABACzeroWordInserted` (§2.5). Also: the slice record's own
  base is `hdr+0x180+0x220·s` with the two fields at `+0` and `+0x20C`; the
  kext's `384`/`908` are the same addresses measured from `hdr`.
- [53](53-first-frame.md) §3.3 — "there is no offset" is confirmed, and the
  restart path is confirmed to be the only exception. Add: the restart gap is
  not merely *recorded*, it is folded into the previous slice's
  `ui32BytesWritten` and flagged by `bytesToRemove`, so the payload is
  discontiguous (§2.2).
- [53](53-first-frame.md) §4.4 — "**U**, the driver recovers it by
  construction" can be **closed**: `ui32_SPSPPSHeaderBits / 8` is the exact
  byte length. **C.** The back-scan is a cross-check, not the source.
- [66](66-ratecontrol-sizing.md) §3.1 — the field table is correct as far as
  it goes; `+0xF0`, `+0x114`, `+0x170/4/8` and the `0x118…0x163` block are
  missing, and `+0xF0` is load-bearing for bitstream conformance, not just
  for statistics.
- [66](66-ratecontrol-sizing.md) §3.1's closing line, "**no driver change is
  needed for §3** — including for multi-slice", is **wrong on two counts**:
  the CABAC term is missing, and multi-slice needs per-slice offsets whenever
  a context switch occurs.
- [64](64-multiframe.md) §8 — "could not prove the memset is needed, only
  that it might be" is now **proved needed**: the MB counters accumulate with
  `ldr`/`add`/`str` and the slice records are not cleared (§2.3).
- [62](62-kext-field-map.md) / `ave_abi.h` — `update_param_sets = 0x6f1` came
  from a log line; record that **no firmware reader exists at that offset**
  (§4.2), and that the parameter sets are Start-scoped regardless.

---

## 9. The cheapest hardware run that would confirm all of this

**One load, two frames, at a fixed QP, with the coded header published
whole.** This is F20 from [53](53-first-frame.md) §"run queue" with three
cheap additions; it needs no new command and no new surface.

Setup: `session_frames=2` (IDR then P), everything else as F18/F19.
Additions, all host-side and all free:

1. Publish the **first `0x200` bytes and the first four slice records** of
   each frame's coded header to debugfs (we publish the whole buffer already;
   just hexdump `+0x00…0x1FF`, `+0x180`, `+0x3A0`, `+0x5C0`, `+0x7E0` into
   the log so a failure is readable from the kmsg alone).
2. Log `info.span`, `info.slices`, `hdr[0xF0]`, `hdr[0x110]`, `hdr[0x10C]`,
   `hdr[0x98]`, and the reply's `+0x1C` and `+0x40` words, for both frames.
3. Compute `psets_len` **both** ways and log both.

What it proves, in one boot:

| observation | conclusion |
|---|---|
| frame 1's `hdr[0x10C] == 1` and frame 0's `== 0` | `FrameNumberFromDriverReturned` is the echo (§5) and completions are not being crossed |
| `hdr[0x98]/8 == ` the back-scan length, on both frames | §4.5's exact length, and that the parameter-sets buffer is untouched by frame 2 (§4.1) |
| `hdr[0x180+0x220]` (record 1) is **zero** on a one-slice frame after a two-slice frame, only because we memset | §2.3 — run it once with the memset disabled via a module parameter and watch record 1 survive into the next frame. This is the negative control, and it is the only part that needs a second load |
| `hdr[0xF0] == 0` | expected for CAVLC; a non-zero value on a Baseline stream would mean §2.5's reading is wrong |
| `ffmpeg` still decodes `frame.h264` when it is assembled per-slice instead of as one `memcpy` | the per-slice assembly is not a regression |

A **multi-slice** run (`sSliceMap.iNum = 2`) would be the direct test of §3,
but `iNum`'s wire offset is itself inferred ([66](66-ratecontrol-sizing.md)
§6.1) and the 256 bytes after it are **U**, so it costs a bisect of its own.
It is the *second* run, not the first.

---

## 10. Reproduce

```sh
export AVE_MACOS=13.5        # mandatory; the default is 26.6.2

# --- where the bytes are ---------------------------------------------------
python3 tools/disas.py --fw --addr 0x581a8 -n 0x2f0    # ProcessTranscodeStart:
                                                       #  8328/8356/8360/8364
python3 tools/disas.py --fw --addr 0x592c0 -n 0x80     # SetTranscode: the regs

# --- the length and the slice records -------------------------------------
python3 tools/disas.py --fw --addr 0x5c210 -n 0x300    # CollectDataFromTranscode
python3 tools/disas.py --fw --addr 0x5c708 -n 0x180    #   the differencing loop
python3 tools/disas.py --kext --addr 0xfffffe0008ec4e38 -n 0x260  # RetrieveRCStats

# --- cabac_zero_words ------------------------------------------------------
python3 tools/disas.py --fw --addr 0x5c108 -n 0x110    # the bin-count check
python3 tools/disas.py --kext --addr 0xfffffe0008ec4fa8 -n 0x10   # ×3 -> +400
python3 tools/disas.py --kext --addr 0xfffffe0008ec4530 -n 0x40   # the print

# --- status codes ----------------------------------------------------------
python3 tools/disas.py --fw --addr 0x5be50 -n 0x40     # 0xEE0007
python3 tools/disas.py --fw --addr 0x5bf64 -n 0x40     # 0xEE0004
python3 tools/disas.py --fw --addr 0x14c50 -n 0x240    # ProcessEncDone -> 0xE06
python3 tools/disas.py --fw --addr 0x13684 -n 0x160    # NotificationToHost

# --- parameter sets --------------------------------------------------------
python3 tools/disas.py --fw --addr 0x5dd50 -n 0x270    # generate + memcpy + size check
python3 tools/disas.py --kext --addr 0xfffffe0008ec0c0c -n 0x200  # the HOST builds its own
python3 tools/disas.py --kext 'seq_parameter_set' --list           # AVC_SPS is in the kext too

# vtable slot +0x218; the only two CAVCController call sites are 0x466f4 and
# 0x4de40 (0x63c3c / 0x681ac are the CHEVCController mirror, the rest are
# stack loads at the same immediate):
python3 tools/disas.py --fw --addr 0x0 -n 0xec000 | grep -nE 'ldr\s+x8, \[x8, #536\]'

# no firmware reader of PICMGMT+0x6F1:
python3 tools/disas.py --fw --addr 0x0 -n 0xec000 | grep -n '#1777\]'   # empty

# --- cache maintenance -----------------------------------------------------
python3 tools/disas.py --fw --addr 0x20bc4 -n 0x40
python3 tools/disas.py --fw --addr 0xac440 -n 0x60     # dc civac / dsb sy
```

The format strings quoted above are read straight out of `__TEXT`/
`__PRELINK_TEXT`; e.g. `0xc4ffa` in the firmware is *"bitstream size overflow,
buffer size: %d, bitstream size: %d"* and `0xfffffe00071ef743` in the kext is
*"numCABACzeroWordInserted %d"*.
