# Methodology and known traps

This project's findings come from static analysis of two Apple binaries. That
is cheap and fast, but it fails in specific, repeatable ways. Every trap below
has already produced a wrong result in this repository that had to be corrected
in a later commit. Read this before adding findings.

## The standing rule

**A finding is either read out of the disassembly, or it is marked as
inferred.** There is no third category. Cite the VA of the instruction behind
every constant, so anyone can re-check it in one command:

```sh
python3 tools/disas.py --kext --addr 0xfffffe0008c2abe0 -n 0x40
```

A short document of confirmed facts is worth more than a long one padded with
plausible reconstruction. A driver written against a fabricated register offset
fails in a way that is extremely expensive to debug, because the natural
assumption will be that the hardware or the sequence is wrong, not the constant.

## Trap 1 — the logging idiom looks like an allocation

`AppleAVE2` is heavily instrumented. Almost every function opens with:

```
mov  w0, #<subsystem>      ; e.g. 0x5b, 0x78, 0x80
mov  w1, #<level>          ; e.g. 6, 7, 8
bl   <AVE_Log_CheckLevel>
cbz  w0, <skip>
```

This is visually identical to `alloc(size, type)`. It caused a real error:
`mov w0, #0x78` in `AVE_CHM_MakeFwCmd_Open` was recorded as "`sCAveCmdOpen` is
120 bytes". It is a log subsystem id. The struct is 72 bytes, as the firmware's
own size check proves. The coincidence that `0x78` *is* a real command struct
size elsewhere (`sCAveCmdConfig`) made the misreading convincing.

**Before treating a constant as a size, confirm it is used as one** — followed
to an allocator, compared against a length, or used as a copy count.

When reading these functions, filtering the logging noise helps:

```sh
python3 tools/disas.py --kext 'AVE_IOP5StartEv' -n 0x500 \
  | grep -vE '#0x5b|c46348|c46390|c463c8|cac7e4|b76a170|beab450'
```

## Trap 2 — confident negatives from tests that cannot discriminate

The worst failure in this project so far. An analysis concluded that
`AppleAVE2FW_H13C.im4p` is *not* the M1 Max firmware, because the image contains
the codename `Erebus` and no `Nyx` or `Castor`. The reasoning was sound; the
test was not. **All nineteen** firmware variants in the IPSW contain `Erebus`
and nothing else, so the string cannot distinguish anything. The correct answer
— `H13C` *is* M1 Max — came from Apple's own `BuildManifest.plist`.

**Before asserting that something is absent, verify your test returns a
different answer for a case you know differs.** If it cannot, you have measured
nothing.

## Trap 3 — absence of a call site is not absence of use

MMIO bank 2 was recorded as "no call site found — unknown" after a scan for
instructions passing bank 2 as an immediate. Bank 2 is in fact the doorbell and
interrupt block; its offsets come from a **per-SoC table**, loaded with `ldr`,
so no immediate exists to find:

```
ldr  w2, [x8]        ; offset = table[0]
mov  w1, #0x2        ; bank 2
bl   AVE_Reg::Write32
```

Table-driven and virtual-dispatch code is invisible to immediate scans. Two
analyses of the same block disagreed for exactly this reason and both were
honest.

## Trap 4 — variant labels

There are 21 per-SoC variants of many routines (`_Acis`, `_Nyx`, `_Castor`,
`_Rhea`, `_Erebus`, …). Finding *a* function named `AVE_IOP_Start_Acis` does not
mean it is the one this hardware uses. The selection chain is:

```
ADT soc-id -> DevID -> ChipType -> variant
t6001      -> 12    -> 7        -> _Nyx     (t6000 -> 6 -> _Castor; t8103 -> 5 -> _Acis)
```

An earlier revision documented the ASC start sequence as `_Acis`. The values
happened to be identical across variants so nothing downstream broke, but the
label was wrong. **Resolve the variant through the dispatch chain, then say
which one you read.**

## Trap 5 — chained fixups are not virtual addresses

Pointers inside `__DATA_CONST` tables are chained-fixup encoded. Their low 32
bits hold the **image-relative file offset**, not a VA:

```python
ptr, = struct.unpack("<Q", entry[:8])
file_offset = ptr & 0xFFFFFFFF        # not a vmaddr
```

Verified by matching all ten `IOExternalMethodDispatch` entries against known
symbol addresses. If a table decode produces garbage names, this is usually why.

## What counts as authoritative

Ranked, most to least:

1. **Apple's own manifests and device tree.** `BuildManifest.plist` settled the
   firmware variant question that disassembly had got wrong. The ADT is the
   truth about hardware wiring.
2. **The firmware's own validation.** The command struct sizes are certain
   because `CmdProcessor` rejects anything else — that is the hardware's
   opinion, not an inference about the host's.
3. **Both sides agreeing.** The command set was recovered independently from
   the kext (`SendFwCmd_*`) and the firmware (`ProcessCmd_*`) and they match.
   `IOProcessorChannel` is statically linked into *both* binaries, so ring
   semantics can be read twice.
4. **A single disassembled call site.** Fine, but cite it.
5. **Ordering, layout, or naming inference.** Weakest. The wire command ids were
   first inferred from `__text` emission order and that inference was wrong.

## Cross-checking with known-good values

Some values are useful canaries when decoding a table:

| Value | Meaning |
|---|---|
| `FwIPC` surface = index 31, size `0x1400000` | 20 MiB IPC region |
| `sCAveCmdOpen` = `0x48` | enforced by firmware |
| `t6001` -> DevID 12 -> ChipType 7 | from the 34-entry device table |
| bank 1 `+0x400044` | ASC CPU control, same in every variant checked |

If a fresh decode of a table disagrees with one of these, the decode is wrong.

## Verify before recording

Every finding in this repository from a delegated analysis was re-checked
against the binaries before being committed. That process caught, among other
things, two missing register writes in a four-write start sequence — a driver
built from the unverified report would not have started the coprocessor.
