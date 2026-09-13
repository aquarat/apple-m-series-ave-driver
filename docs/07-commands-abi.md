# The wire command ABI

Numeric command ids, command struct sizes, and the shared header layout,
recovered from the firmware dispatcher and the kext builders. Both sides were
read independently and agree on every id and every size that both sides state.

Everything in the tables below is read directly out of the disassembly and is
cited with the VA of the instruction it came from. Anything not read out of an
instruction is marked **inferred** or **unknown**.

> **Version note (2026-09-13).** Everything in this document was read from
> **macOS 26.6.2** (25G83) and stands for that build. On **macOS 13.5**
> (22G74, the firmware this machine actually runs, [43](43-macos-13.5-firmware.md))
> the wire command ABI **differs by version** in ids, sizes and header layout.
> On 13.5: the dispatcher is at firmware `0xd614` and accepts ids **1..14**
> (`cmp w8,#0xd` at `0xd6e4`, jump table `0xe2e8`); names come from the
> firmware's own `CAVE_CMD_*` table at `0xed060`: 1 `CONFIG` `0x70`,
> 2 `START` (host "Open") `0x40`, 3 `RESET` `0x32DB0`, 4 `AVC_INIT` (host
> "Start_AVC") `0x10E10`, 5 `HEVC_INIT` `0x32DC8`, 6 `UNINIT` (host "Stop")
> `0x40`, 7 `AVC_ENCODE` `0x1940`, 8 `HEVC_ENCODE` `0x6838`,
> 9 `LRME_STANDALONE` `0x6838`, 10 `MCTF_PROCESS` (no handler), 11 `FLUSH`
> `0x40`, 12 `STOP` (host "Close") `0x48`, 13 `COMPLETE` `0x40`,
> 14 `POWERDOWN` (host "Halt") `0x40`; there is no Priority command. The
> 13.5 header is `+0x10` **u32** client id, `+0x18` codec (**0 = AVC,
> 1 = HEVC**), `+0x1C` slot, `+0x20` priority, `+0x28` timeout; host slots
> are bounded `<= 40`. Full table and VAs: [46](46-abi-13.5-commands-session.md).
> A 26.6.2-shaped command sent to 13.5 firmware hits the `insize` assert and
> the firmware spins (e.g. Config: `cmp w22,#0x70` at `0xd704` → `_bsp_assert_fail`
> and `b .` at `0xde8c`).

Reproduce any line with:

```sh
python3 tools/disas.py --fw   --addr 0x28134 -n 0xa20      # firmware dispatcher
python3 tools/disas.py --kext --addr 0xfffffe0008b668c0    # a host builder
```

## 1. The dispatcher

`CFlowControllerBase::CmdProcessor(void* cmd, uint32_t insize, uint32_t* out)`
at firmware `0x28134`.

The `Enter` log at `0x28184` uses format `"%s::%s Enter %p %d %p"` with the
stack varargs built at `0x28190`/`0x28194`, which confirms the argument roles:
`cmd` pointer, `insize`, `out` pointer.

The dispatch is a compiler-emitted jump table, not a chain of compares:

```
28244:  ldrh  w8, [x19]            ; x19 = cmd  -> id is a u16 at offset 0
2824c:  sub   w16, w8, #0x1
28250:  cmp   w16, #0xb
28254:  b.hi  0x28b24              ; default: ignore, no Ack, return 0
28260:  adrp  x17, 0x28000
28264:  add   x17, x17, #0xc6c     ; table base 0x28c6c
28268:  ldrsw x16, [x17, x16, lsl #2]
2826c:  adr   x17, 0x2826c         ; offsets are relative to 0x2826c
28274:  br    x16
```

Twelve `int32` entries at `0x28c6c`, index = `id - 1`, so **valid ids are 1..12**
and id 0 is rejected.

## 2. Command ids

The id → handler mapping is the jump-table target; the size column is the
`insize` the firmware asserts for that id, and the assert string names the
struct. Both are read from the entry block for each id.

| id | handler (firmware symbol) | entry block | asserted `insize` | assert string |
|---:|---|---|---:|---|
| 1  | `ProcessCmd_Config`   | `0x28278` | `0x78` (`0x28278`) | `insize == sizeof(struct sCAveCmdConfig)` |
| 2  | `ProcessCmd_Halt`     | `0x28334` | `0x48` (`0x28334`) | `insize == sizeof(struct sCAveCmdHalt)` |
| 3  | `ProcessCmd_Open`     | `0x28318` | `0x48` (`0x28318`) | `insize == sizeof(struct sCAveCmdOpen)` |
| 4  | `ProcessCmd_Close`    | `0x282fc` | `0x48` (`0x282fc`) | `insize == sizeof(struct sCAveCmdClose)` |
| 5  | — | *(table entry points at the default block `0x28b24`)* | — | — |
| 6  | `Start_AVC` / `Start_HEVC` | `0x282b8` | *(none)* | — |
| 7  | `ProcessCmd_Stop`     | `0x282e0` | `0x48` (`0x282e0`) | `insize == sizeof(struct sCAveCmdStop)` |
| 8  | `Process_{AVC,HEVC,LRME,MCTF,DMV}` | `0x2834c` | *(none)* | — |
| 9  | `ProcessCmd_Complete` | `0x28384` | `0x48` (`0x28384`) | `insize == sizeof(struct sCAveCmdComplete)` |
| 10 | `ProcessCmd_Priority` | `0x283bc` | `0x48` (`0x283bc`) | `insize == sizeof(struct sCAveCmdPriority)` |
| 11 | `ProcessCmd_Flush`    | `0x283a0` | `0x48` (`0x283a0`) | `insize == sizeof(struct sCAveCmdFlush)` |
| 12 | `ProcessCmd_Reset`    | `0x28294` | `0x13F08` (`0x28294`+`0x28298`) | `insize == sizeof(struct sCAveCmdReset)` |

Eleven live ids, matching the eleven `AVE_HwC::SendFwCmd_*` wrappers exactly.
Id 5 is a hole in the enum.

The assert strings live in `__cstring` at `0x1216c4` (Config), `0x121824`
(Halt), `0x1216ec` (Open), `0x1217a9` (Close), `0x12175c` (Stop), `0x1217d0`
(Complete), `0x1217fa` (Priority), `0x121782` (Flush), `0x12169d` (Reset), and
are loaded by the mismatch blocks at `0x285a4`, `0x286cc`, `0x28638`,
`0x28514`, `0x2847c`, `0x28760`, `0x28888`, `0x287f8`, `0x283e8` respectively.
Each mismatch block calls `_bsp_assert_fail` (`0xe08fc`) and then spins.

### Host confirmation

Each `MakeFwCmd_*` builder writes the same id into offset 0 of the struct it
fills. Independent confirmation of every value above.

| command | id | `mov` VA | store VA | store form |
|---|---:|---|---|---|
| Config      | 1  | `0xfffffe0008c04718` | `0xfffffe0008c0471c` | `str w8, [x19]` |
| Halt        | 2  | `0xfffffe0008c04a94` | `0xfffffe0008c04a98` | `str w8, [x21]` |
| Open        | 3  | `0xfffffe0008b669a0` | `0xfffffe0008b669a4` | `strh w8, [x23]` |
| Close       | 4  | `0xfffffe0008b66d3c` | `0xfffffe0008b66d40` | `strh w8, [x23]` |
| Start_AVC   | 6  | `0xfffffe0008b670e8` | `0xfffffe0008b670ec` | `strh w8, [x23]` |
| Start_HEVC  | 6  | `0xfffffe0008b67838` | `0xfffffe0008b6783c` | `strh w8, [x23]` |
| Stop        | 7  | `0xfffffe0008b68000` | `0xfffffe0008b68004` | `strh w8, [x23]` |
| Process_AVC | 8  | `0xfffffe0008b6a158` | `0xfffffe0008b6a15c` | `strh w8, [x23]` |
| Process_HEVC| 8  | `0xfffffe0008b6a908` | `0xfffffe0008b6a90c` | `strh w8, [x23]` |
| Complete    | 9  | `0xfffffe0008b6afc4` | `0xfffffe0008b6b0bc` | `strh w8, [x23]` |
| Priority    | 10 | `0xfffffe0008b6b36c` | `0xfffffe0008b6b398` | `strh w8, [x23]` |
| Flush       | 11 | `0xfffffe0008b6b700` | `0xfffffe0008b6b7f8` | `strh w8, [x23]` |
| Reset       | 12 | `0xfffffe0008b6baa4` | `0xfffffe0008b6bb9c` | `strh w8, [x23]` |

`Config` and `Halt` write a 32-bit `str` rather than a 16-bit `strh`; the
firmware reads only the low 16 bits (`ldrh` at `0x28244`), and the upper half is
the field at `+0x02` (see below), which is zero in both cases.

A second, independent host source for the same four ids: the builders for
Open/Close/Start/Stop load a literal `u32` pair into `+0x20`/`+0x24` from
`__PRELINK_TEXT`:

| literal VA | bytes | loaded at | command |
|---|---|---|---|
| `0xfffffe000723ea28` | `03 00 00 00  c8 00 00 00` | `0xfffffe0008b66988` | Open |
| `0xfffffe000723ea30` | `04 00 00 00  c8 00 00 00` | `0xfffffe0008b66d24` | Close |
| `0xfffffe000723ea38` | `06 00 00 00  c8 00 00 00` | `0xfffffe0008b670cc` | Start (both) |
| `0xfffffe000723ea40` | `07 00 00 00  c8 00 00 00` | `0xfffffe0008b67fe8` | Stop |

## 3. Command struct sizes

| struct | size | firmware evidence | host evidence |
|---|---:|---|---|
| `sCAveCmdConfig`      | `0x78` (120)   | `0x28278` | zero-fill reaches `+116..119` at `0xfffffe0008c04704` |
| `sCAveCmdHalt`        | `0x48` (72)    | `0x28334` | zero-fill reaches `+68..71` at `0xfffffe0008c04aac` |
| `sCAveCmdOpen`        | `0x48` (72)    | `0x28318` | inline zeroing of exactly 72 bytes, `0xfffffe0008b66964`–`0xfffffe0008b6696c`; `AVE_IPC::Alloc(0x48, …)` at `0xfffffe0008c0653c` |
| `sCAveCmdClose`       | `0x48` (72)    | `0x282fc` | `0xfffffe0008b66d00`–`08`; `Alloc(0x48)` at `0xfffffe0008c06df0` |
| `sCAveCmdStop`        | `0x48` (72)    | `0x282e0` | `0xfffffe0008b67fc4`–`cc`; `Alloc(0x48)` at `0xfffffe0008c08120` |
| `sCAveCmdComplete`    | `0x48` (72)    | `0x28384` | `0xfffffe0008b6afb8`–`c0`; `Alloc(0x48)` at `0xfffffe0008c089d8` |
| `sCAveCmdPriority`    | `0x48` (72)    | `0x283bc` | `0xfffffe0008b6b360`–`68`; `Alloc(0x48)` at `0xfffffe0008c09290` |
| `sCAveCmdFlush`       | `0x48` (72)    | `0x283a0` | `0xfffffe0008b6b6f4`–`fc`; `Alloc(0x48)` at `0xfffffe0008c09b4c` |
| `sCAveCmdReset`       | `0x13F08` (81672) | `0x28294`+`0x28298` | zero-fill length at `0xfffffe0008b6ba98`+`9c`; `Alloc` size at `0xfffffe0008c0a408`+`0c` |
| `sCAveCmdAvcStart`    | `0x3180` (12672)  | — | zero-fill length `0xfffffe0008b670ac`; `Alloc` size `0xfffffe0008c07818` |
| `sCAveCmdHevcStart`   | `0x13F28` (81704) | — | zero-fill length `0xfffffe0008b677f8`+`fc`; `Alloc` size `0xfffffe0008c07808`+`0c` |
| `sCAveCmdAvcProcess`  | `0x63D8` (25560)  | — | zero-fill length `0xfffffe0008b6a098`; `Alloc` size `0xfffffe0008c0acb4` |
| `sCAveCmdHevcProcess` | `0xB1C0` (45504)  | — | zero-fill length `0xfffffe0008b6a848`; `Alloc` size `0xfffffe0008c0aca8` |

The zero-fill length is the second argument to an out-of-line kernel routine at
`0xfffffe000b6e8d80` called as `f(buffer, length)`. That it is `bzero` is
**inferred** from the two-argument form and from the fact that the 72-byte cases
are open-coded as SIMD zero stores instead. It is corroborated for Reset, where
the length `0x13F08` equals the firmware's asserted `sizeof(sCAveCmdReset)`
exactly.

Start and Process pick their size at run time from the session's codec:

```
; AVE_HwC::SendFwCmd_Start
c07804:  ldr  w8, [x26, #15532]     ; x26 = chm->[0x18]
c07808:  mov  w9, #0x13F28          ; HEVC
c07814:  csel w9, w9, wzr, eq       ;   if codec == 2
c07818:  mov  w10, #0x3180          ; AVC
c07820:  csel w23, w10, w9, eq      ;   if codec == 1
```

and likewise `0xfffffe0008c0aca4`–`0xfffffe0008c0acbc` for Process
(`0x63D8` when codec == 1, `0xB1C0` when codec == 2). Codec value 1 = AVC,
2 = HEVC — the same encoding the firmware dispatches on (§5).

### Correction to `docs/06-kext.md`

`docs/06-kext.md` states `sCAveCmdOpen` is 120 bytes "from the allocation in
`AVE_CHM_MakeFwCmd_Open`". That is wrong. The `0x78` constant appearing
throughout that function (`0xfffffe0008b668f4`, `0xfffffe0008b66904`, …) is the
first argument to `AVE_Log_CheckLevel(unsigned, char)` at
`0xfffffe0008c46348` — an `AVE_Log` subsystem id, not a size. `sCAveCmdOpen` is
`0x48` = 72 bytes. `0x78` happens to be the correct size of `sCAveCmdConfig`,
by coincidence.

## 4. The common 64-byte header

Every command struct starts with the same `0x40`-byte header. The `0x48`-byte
commands are that header plus 8 trailing bytes that no builder writes.

| off | size | contents | evidence |
|---:|---:|---|---|
| `0x00` | u16 | **command id** | fw `ldrh w8,[x19]` `0x28244`; host `strh` table §2 |
| `0x02` | u16 | zeroed by the firmware on receipt; purpose **unknown** | fw `strh wzr,[x19,#2]` `0x281e0` |
| `0x04` | 4 | never written by any builder or by the firmware | — |
| `0x08` | u64 | the `uint64_t` second argument of every `MakeFwCmd_*`. Logged by the firmware as **`CNT`** | host `stp x20,x8,[x23,#8]` `0xfffffe0008b66974`; fw log arg at `0x28208` with format `0x12166b` |
| `0x10` | u64 | **client id**. Logged by the firmware as `CID`; read by `ProcessCmd_Open` and `ProcessCmd_Priority`. Source is `chm->[0x38]`. Left 0 by Config/Halt | host `0xfffffe0008b66970`+`74`; fw `ldr x20,[x23,#16]` `0x295a0`, `ldr x1,[x21,#16]` `0x2d668` |
| `0x18` | u32 | **client type** — selects the engine/pass for `Process`. Source is `chm->[0x34]` | host `0xfffffe0008b66978`+`0xfffffe0008b66980`; fw `ldr w8,[x19,#24]` `0x28354`; error string `"Not Supported Client Type %d"` at `0x121735`, loaded `0x28a70`, value from `0x28a50` |
| `0x1c` | u32 | **enc type** — 1 = AVC, 2 = HEVC. Source is `chm->[0x18]->[0x270]` | host `0xfffffe0008b6697c`+`80`; fw `ldr w8,[x19,#28]` `0x282c0`, compares `0x282c4`/`0x282cc`; error string `"Not Supported EncType %d"` at `0x121712`, value from `0x28964` |
| `0x20` | u32 | **in-flight slot index**, must be `< 0x33` (51) | fw logs it; host bound check `ldr w26,[x21,#32]` / `cmp w26,#0x33` / `b.cs` at `0xfffffe0008c05104`–`0c`, used as the array index at `0xfffffe0008c054e8`–`0xfffffe0008c05520` |
| `0x24` | u32 | command-specific parameter (see §5) | — |
| `0x28` | u32 | source is `chm->[0x18]->[0x1564]`; logged by the firmware as `0x%x`. Meaning **unknown** | host `0xfffffe0008b66990`+`94`; fw log arg `0x28204` |
| `0x2c` | u32 | written by `AVE_DPM::RetrieveHw(dpm, &cmd[0x2c])` inside `SendFwCmd`, not by the builder. Meaning **unknown** (a power/clock state handle, **inferred**) | `add x1, x21, #0x2c` at `0xfffffe0008c05118`, call `0xfffffe0008c0511c` |
| `0x30` | 16 | verbatim 16-byte copy of the `_S_AVE_TimeOut*` argument. So `_S_AVE_TimeOut` is 16 bytes | `ldr q0,[x21]` / `str q0,[x23,#48]` at `0xfffffe0008b66998`+`9c` |
| `0x40` | 8 | zeroed, never written | `str xzr,[x23,#64]` `0xfffffe0008b66964` |

The firmware's own trace line, format string `"%s::%s:%d CMD %d %d | %d 0x%x |
CID %llu CNT %llu"` at `0x12166b`, prints in order: id (`+0x00`), a ring index,
`+0x18`, `+0x28`, `+0x10`, `+0x08`. The vararg slots are built at `0x28208`,
`0x28210`, `0x28214`, `0x28238`, `0x2823c`. This is what fixes the `CID`/`CNT`
names to `+0x10` and `+0x08` and not the other way round (Apple's arm64 ABI puts
all variadic arguments on the stack, so slot order is argument order).

### `+0x20` — the slot index

This is the one header field whose name is worth being careful about. It is not
simply a duplicate of the id, even though for eight of the eleven commands the
builder stores the id there:

* Open/Close/Start/Stop take it from the `{id, 200}` literal pair (§2).
* Complete/Priority/Flush/Reset store the id explicitly
  (`0xfffffe0008b6afc8`, `0xfffffe0008b6b384`, `0xfffffe0008b6b704`,
  `0xfffffe0008b6baa8`).
* **Process** stores a caller-supplied `int`, range-checked `<= 0x32`
  (`cmp w24,#0x32` / `b.gt` at `0xfffffe0008b6a074`, store at
  `0xfffffe0008b6a0a0`).
* **Config/Halt** store `0xFFFFFFFF` (`movi d0, #0xffffffff` /
  `str d0, [x19,#32]` at `0xfffffe0008c04728`+`2c` and
  `0xfffffe0008c04abc`+`c0`, which also puts 0 in `+0x24`).

`AVE_HwC::SendFwCmd` treats it purely as an index into two 51-entry arrays in
the `_S_AVE_CHM`:

```
c05104:  ldr  w26, [x21, #32]       ; x21 = cmd
c05108:  cmp  w26, #0x33
c0510c:  b.cs <error>
...
c054e8:  add  x8, x20, #0x108       ; x20 = chm
c054ec:  lsl  x9, x26, #3
c05504:  str  x21, [x10]            ; chm[0x108 + slot*8] = cmd buffer
c05508:  add  x8, x20, #0x2a0
c05520:  str  x23, [x10]            ; chm[0x2A0 + slot*8] = _S_AVE_Cmd*
```

So the single-instance commands each get a fixed slot that happens to equal
their id, and Process gets a per-frame slot chosen by the caller. Whether the
firmware also uses `+0x20` as an index is **not determined** — no read of
`[cmd+32]` was found in the handlers examined.

### `+0x24`

Command-specific:

* Open/Close/Start/Stop: the constant `200` (`0xc8`) from the literal pair.
* Complete/Flush/Reset/Process: `200` if bit 2 of the builder's `uint32_t`
  third argument is set, else `chm->[0x18]->[0x27c]`
  (`tbnz w22, #2` at `0xfffffe0008b6afe0`, `0xfffffe0008b6b71c`,
  `0xfffffe0008b6bac0`, `0xfffffe0008b6a0bc`; the two arms at e.g.
  `0xfffffe0008b6afe4` and `0xfffffe0008b6b0a8`).
* **Priority**: the `int` priority argument, stored directly
  (`stp w8, w24, [x23,#32]` at `0xfffffe0008b6b384`). The firmware reads it back
  at `ldr w2,[x21,#36]` (`0x2d67c`) and passes it with the client id to
  `0x401c8`.

That it is a timeout for the non-Priority commands is **inferred** from the
`200` default and is not proven.

## 5. Sub-dispatch inside ids 6 and 8

Ids 6 (Start) and 8 (Process) carry no `insize` assert; they branch on header
fields instead.

**id 6 — Start**, on `+0x1c`:

```
282c0:  ldr w8, [x19, #28]
282c4:  cmp w8, #0x2  -> 0x28928 -> ProcessCmd_Start_HEVC (0x29954)
282cc:  cmp w8, #0x1  -> 0x29744    ProcessCmd_Start_AVC
        otherwise     -> 0x28954    "Not Supported EncType %d"
```

**id 8 — Process**, on `+0x18` first:

| `+0x18` | target |
|---:|---|
| 1 | second-level switch on `+0x1c` (`0x28934`) |
| 2 | `ProcessCmd_Process_LRME` (`0x2a128`), via `0x2890c` |
| 3 | `ProcessCmd_Process_MCTF` (`0x2a720`), via `0x28a34` |
| 4 | second-level switch on `+0x1c` (`0x28934`) |
| 6 | `ProcessCmd_Process_DMV` (`0x29b84`) |
| other | `0x28a40`, `"Not Supported Client Type %d"` |

Compares at `0x28358`, `0x28360`, `0x28368`, `0x28370`, `0x2890c`, `0x28914`.
Values 0 and 5 are not handled. The second-level switch at `0x28934` reads
`+0x1c` and goes to `ProcessCmd_Process_AVC` (`0x2adf0`) for 1 and
`ProcessCmd_Process_HEVC` (`0x2b480`) for 2.

Note that `+0x18` = 1 and `+0x18` = 4 dispatch identically. What distinguishes
them is **unknown**.

Every branch except id 6 / `+0x1c` = 1 first calls `CFlowControllerBase::Ack`
(`0x28c9c`). After the handler, `CmdProcessor` writes 0 to `*out`
(`str wzr, [x20]` at `0x28b28`) and calls `ProcessQueue` (`0x2d934`).

## 6. How a command reaches the firmware

Taking `AVE_HwC::SendFwCmd_Open(_S_AVE_CHM*, _S_AVE_Cmd*)`
(`0xfffffe0008c06478`) as the worked example — the other ten wrappers have the
same shape:

1. Check `this->[192] == 3` (`0xfffffe0008c06528`). A state gate; the value 3 is
   used identically in all three layers.
2. `AVE_IPC::Alloc(0x48, &buf)` (`0xfffffe0008c0653c`+`40`). `AVE_IPC::Alloc(int,
   unsigned long*)` writes the buffer address through its out-parameter; the IPC
   object is `this->[224]` (`0xfffffe0008c06534`).
3. Build a 16-byte `_S_AVE_TimeOut` on the stack, scaling the caller's value by
   8/10 (`0xfffffe0008c06904`–`0xfffffe0008c0691c`).
4. `AVE_CHM_MakeFwCmd_Open(chm, aveCmd->[0x20], aveCmd->[0x30], &timeout, buf)`
   (`0xfffffe0008c06920`–`0xfffffe0008c06930`).
5. `AVE_HwC::SendFwCmd(chm, buf, 0x48, aveCmd)` (`0xfffffe0008c06b88`–`9c`).
   Note the true arity is **four**, not three: `(_S_AVE_CHM*, void*, int,
   _S_AVE_Cmd*)`.
6. On failure, `AVE_IPC::Free` (`0xfffffe0008c06818`).

`AVE_HwC::SendFwCmd(_S_AVE_CHM*, void*, int, _S_AVE_Cmd*)`
(`0xfffffe0008c05040`):

* null/length checks (`0xfffffe0008c050e0`–`ec`), state gate `this->[192] == 3`
  (`0xfffffe0008c050f0`);
* slot bound check on `cmd[0x20]` (`0xfffffe0008c05104`);
* `AVE_DPM::RetrieveHw(this->[136], &cmd[0x2c])` then
  `AVE_DPM::TuneUpHw(this->[136], chm->[0x20]->[0x68])`
  (`0xfffffe0008c05114`–`0xfffffe0008c05128`);
* `AVE_HwC::SendFwCmd(cmd, len)` (`0xfffffe0008c05138`);
* on success, records the buffer and the `_S_AVE_Cmd*` in the CHM slot arrays
  (§4).

`AVE_HwC::SendFwCmd(void*, int)` (`0xfffffe0008c04ba4`) is a thin shim:

```
c04c40:  ldr  w8, [x19, #192]
c04c44:  cmp  w8, #0x3
c04c48:  b.ne <error>
c04c4c:  ldr  x0, [x19, #224]       ; the AVE_IPC object
c04c50:  mov  w1, #0x1              ; _E_AVE_IPC_Ch = 1
c04c54:  mov  x2, x20               ; buffer
c04c58:  mov  x3, x21               ; length
c04c5c:  bl   AVE_IPC::Send(_E_AVE_IPC_Ch, void*, int)
```

`AVE_IPC::Send` (`0xfffffe0008c44e08`):

* the channel argument must be 1 or 2 (`sub w8,w20,#1` / `cmp w8,#1` / `b.hi` at
  `0xfffffe0008c44efc`–`0xfffffe0008c44f04`);
* `AVE_IPC::Kernel2FwAddr(this, buf)` (`0xfffffe0008c44f14`) converts the kernel
  pointer to the address the coprocessor sees;
* selects a channel object at `this + 0x50 + this->[0xC8 + ch*4] * 40`
  (`0xfffffe0008c45158`–`0xfffffe0008c45194`);
* `AppleAVEIOProcessorChannel::Send(chan, fw_addr, length, 0)`
  (`0xfffffe0008c45198`–`0xfffffe0008c451a4`).

**So a command is a struct written into IPC shared memory, and the doorbell
message carries only its firmware-visible address and its byte length.** That
`(address, length)` pair becomes the firmware's `(cmd, insize)` arguments is
**inferred** — it was not traced through the firmware receive path — but it is
strongly supported: every asserted `insize` equals the size the host passes to
`AVE_IPC::Alloc` for that command.

Commands are sent on IPC channel **1**. What channel 2 carries was not
determined.

## 7. Not determined

* The meaning of header fields `+0x02`, `+0x04`, `+0x28`, `+0x2c`, and of
  `+0x24` for the non-Priority commands.
* The internal layout of any command struct beyond the `0x40`-byte header. The
  large structs (`sCAveCmdHevcStart` at 81704 bytes, `sCAveCmdReset` at 81672)
  are entirely unmapped.
* Which structure `chm->[0x18]` points at, and hence what `+0x270`, `+0x27c`,
  `+0x1564` and `+0x3CAC` in it are. `+0x270` and `+0x3CAC` both hold the codec
  (1 = AVC, 2 = HEVC) and are read at different points; whether they are the
  same field is unknown.
* Why `sCAveCmdReset` is 81672 bytes. That number is confirmed from both sides
  and is not a misreading, but nothing explains it.
* The distinction between `+0x18` values 1 and 4 in the Process dispatch.
* The completion path. `ProcessIntr_CmdAck` / `ProcessIntr_CmdErr` exist on the
  host and `CFlowControllerBase::Ack` on the firmware, but nothing here traces
  how a result gets back to the host or how the CHM slot arrays are drained.
* Whether id 5 ever existed. It is simply absent.
