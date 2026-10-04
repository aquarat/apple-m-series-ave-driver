# tools/ave2pass

Turns the firmware's pass-1 multi-pass records into the pass-2 input,
the way macOS 13.5's user space does it (docs/95 §2.4, §2.5).

| file | what |
|---|---|
| `ave2pass.py` | the command line: `build`, `frames`, `dump`, `synth` |
| `mpport.py` | pure-Python port of the user-space MP code (no dependencies) |
| `mpemu.py` | the same code run from Apple's binary under Unicorn (needs `unicorn` and the binary) |
| `recfmt.py` | record and header layout, decoder, pass-2 buffers, synthetic records |
| `selftest.py` | invariants on synthetic clips; port against emulation when the binary is there |

The port is the default backend. It matches the emulation byte for byte on
every record set tried (selftest: synthetic clips with cuts, varied frame
bits, and random records including NaNs, infinities and subnormals).

## Input

`RECS.bin`: the pass-1 records, 0x626 bytes each, packed, in the order the
frames completed. For an IPPP stream that is display order, and record *i*
is display frame *i*: the bytes at CodedHeader+0x22638 of that frame.
`rec+0x2C` (display order) must hold 0..N-1, each once; the tool refuses
anything else, because the reorder heap would never release a missing
frame. `--renumber` writes 0..N-1 if a dump lacks them. For per-frame dumps
of whole coded headers, `--stride 0x22C60 --offset 0x22638` picks the
records out. Several input files are read in order.

`build` writes the frame's PTS (CMTime *i*/fps) over rec+0x04..0x1B, as
SendFrame does; the MP code itself never reads it. `--keep-pts` leaves it.

## Output

`TABLE.bin` = `header[0x108] + rec[N][0x626]`, records in display order.
This is also the layout of macOS's debug dump (`DBUG_DumpMultiPassStats`,
UA 0x950e0, writes each record at file offset 0x108 + display_order × 0x626).

`frames TABLE.bin OUTDIR` writes `frame-0000.bin`, ...: what PICMGMT+0x900
must point at for each pass-2 frame. Frame 0: header + records 0..10,
0x44AA bytes (= 0x108 + 11 × 0x626; there is no padding). Frame *k* ≥ 1:
header + record *k*+10, 0x72E bytes. Past the end of the clip the last
record is repeated.

## Commands

```sh
P=.venv/bin/python; A=tools/ave2pass/ave2pass.py
$P $A synth 40 -o recs.bin --cuts 12                 # synthetic pass-1 records
$P $A build recs.bin 40 -o table.bin                  # port (default)
$P $A build recs.bin 40 -o table.bin --backend both   # port and emulation, must agree
$P $A build recs.bin 40 -o table.bin --backend emu --trace   # Apple's MP: log lines on stderr
$P $A dump table.bin                                  # per record and header fields
$P $A frames table.bin out/                           # pass-2 buffers
$P tools/ave2pass/selftest.py                         # 50 checks + emulation cross-check
```

The emulation needs `data/blobs/macos-13.5-userspace/.../AppleVideoEncoder`
(SHA-256 `b1f8fd38…834da803`, fetched by `tools/fetch_userspace.py`) or
`AVE_USERSPACE_BIN=path`. Everything else runs on a plain `python3`.

## What `dump` prints

Per record: `fn` rec+0x2C display order, `sl` rec+0x30 slice type, `cl`
rec+0x34 class (2 = intra), `bits` rec+0x40, `hdr` rec+0x44, `corr`
rec+0x48, `fk` rec+0x50 bit 0 (forceKeyFrame), `sc` rec+0x4B0 scene start,
`scn` rec+0x4C4 frames in the scene, `hdiff`/`hdiff_mx` rec+0x4B8/0x4BC,
`act` rec+0x4C0, `qscale` rec+0x614, `cplx0/1` rec+0x618/0x61C, `lc`
rec+0x624. Then the header fields (docs/95 §2.4.6).
