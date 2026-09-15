# The pipe start: MCPUs, the "go", the DMA words, and the host-side writes we skip

Static analysis prompted by F7-F9 (docs/53 §18-§20): Config / Open / Start_AVC /
Process accepted, recon writer programmed, all five VENC sub-domains on, zero
DART/SMMU/AXI faults, LRMEFS and LRMERC complete, **the Pipe starts and never
raises its done interrupt** (`0x40D110140` bit 2 clear at the hang).

Direct successor of [57](57-pipe-hang.md). All firmware VAs are **macOS 13.5**
image VAs (`AppleAVE2FW-6070.11.1`, file offset = VA + `0x4000`); kext VAs are
13.5 kernelcache VAs. `base` = the firmware's register window `[0x21a7b8]` =
AP `0x40C000000` (docs/40, docs/56), so firmware `base+0x1110128` = AP
`0x40D110128`.

```sh
AVE_MACOS=13.5 python3 tools/disas.py --fw   --addr <VA> -n <LEN>
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr <VA> -n <LEN>
```

Labels per [00](00-methodology.md): **[C]** read from an instruction or table
(VA cited), **[I]** inferred (chain stated), **[U]** unknown. Nothing here was
run on hardware.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| What are the MCPUs | Seven small **ARM Cortex-M class** cores inside the encoder, one per pipe stage: MbInput, IntraEst, MotionEst, ModeDecision, ReconLuma, ReconChroma, CAVLC (HEVC uses MbInput, MotionEst, Md5, Rcr5, Bef5, Seb5) | C |
| Do they run code | Yes. Each image is a Thumb vector table + code (`SP = 0x10004000`, reset `0x125`, `wfi` idle loop, NVIC at `0xE000E100`) | C |
| Where the code comes from | **Embedded in the ASC firmware image** (`__TEXT.__const`, e.g. MbInput `0xdadc0`, IntraEst `0xe3350`, CAVLC `0xe5650`), copied by the ASC over MMIO into each MCPU's instruction memory at `base+0x14x0000` (`StartUnit` `0x91d14` → `WriteIMem` `0x91f6c`) | C |
| Host (kext) involvement | **None found.** The 13.5 AppleAVE2 kext has no `mcpu` string and does not contain the MCPU images; the same search finds 109-123 hits in every AVE firmware image (negative control) | C |
| Does the firmware check the MCPU start | Only an ID-register compare at object creation (Start_AVC). `StartUnit` writes `+8 = 1` and returns 1 unconditionally; nothing waits for the MCPU | C |
| Could a dead MCPU stall the pipe silently | Yes: the ASC waits only for pipe-done (src 1); an MCPU that never runs its per-MB IRQ handler would leave the hardware pipeline waiting with no assert and no fault | I |
| `session_skip_mcpu=1` | **Not a clean discriminator.** With `bSkipMcpu=1` the firmware never creates the MCPU objects and never loads their code, but still writes `1` to all seven MCPU run registers — releasing cores whose memory holds no image. Expect the same hang (uninformative) or something worse; only "the frame completes" would be informative | C (paths) / I (outcome) |
| The pipe "go" | `setPipeGo` `0x61fec` sets bit 0 of `0x40D110128`, which the firmware itself calls **`SRCDMAGO`** (log `0xc448d`) — the source-DMA go. No precondition checked. Nothing in the AVC firmware clears it afterwards, so `0` at the hang means the hardware consumed the pulse | C / I |
| `0x40D120000 = 0x80034045` | **Not an error latch.** It is exactly the configuration word `setPipe` writes: `0x80034024 + 0x21` (`0x54804`, `0x5489c`, `0x549c0`). Reading it back unchanged says the source-DMA config landed | C |
| `0x40D120004 = 0xc0` | No firmware writer or reader found by immediate scan | U |
| The AXI-error print | **docs/56 §5.1 misread the addresses.** `AxiErrorHandler` reads `0x40D124000`, `0x40D124004`, `0x40D134000`, `0x40D12C000` (`mov w10,#0x4000; movk #0x112` `0x38b20/0x38b30`), not `0x40D120000/4`. The F3/F4 `0x3 0x1` and F9's `0x80034045 0xc0` are **different registers** | C |
| Host-side writes we skip | **`AVE_DPE`**: a kext-owned block at `0x40D1DC000` / `0x40D1DC400` (124 + 1 tunable writes and two enable bits, applied at PowerOn and per client) that the firmware never touches and our driver never programs | C (writes) / I (role) |

---

## 1. Q1 — the MCPUs

### 1.1 Creation (Start_AVC)

`CFlowControllerBase::ProcessConfig` creates the controller when Config `+0x41`
(`this[1358]`, bCreateMcpu) is set: `vt+112` = `CFlowController::
McpuControllerCreate("McpuController", …)` → stored at `this+31248`
(`0xe824..0xe84c`; vtable slot `0xed040`). **[C]**

`CFlowControllerBase::ProcessAvcInit` (Start_AVC): **if `this[1357]` (bSkipMcpu)
== 0 and `this[1358]` != 0**, call `McpuController->vt+200` = `Init(cmd[24]==0)`
(`0xee4c..0xee74`; slot `0xecfa8` → `CAVEPipeMcpuController::Init` `0x393e0`)
→ `McpuInit(w1)` (`0x3942c`). **[C]**

`McpuInit(1)` (`0x39454`) news seven `CAvePipeMcpu` objects and calls `Init()`
on each (`0x394ac..0x395b4`): unit 0 `MCPU_MbInput`, 1 `IntraEst`,
2 `MotionEst`, 3 `ModeDecision`, 4 `ReconLuma`, 5 `ReconChroma`, 6 `CAVLC`
(names `0xbfe26..0xbfebc`). `McpuInit(0)` builds units 0, 2, 7 `Md5`, 8 `Rcr5`,
9 `Bef5`, 10 `Seb5` instead (`0x395bc..0x396a8`). **[C]** That `w1 = 1` is AVC
is **[I]** (the HEVC-named units are built only on the other arm).

### 1.2 Per-unit register map (from the constructor `0x912d0`)

The constructor switches on the unit index (`0x91300..0x91320`, table
`0x91a74`) and loads per-unit constants from literal pools. Decoded:

| unit | IMem (`[156]`) | DMem (`[160]`) | control block `[260..]` | host-interface block `[272..]` | ASC irq src `[336]` | image(s) `[176]/[200]/[224]` |
|---|---|---|---|---|---|---|
| 0 MbInput | `0x1400000` | `0x1408000` | `0x1410000` | `0x1168000` | 4 | `0xdadc0` (0x18ec), `0xdc6b0`, `0xde780` |
| 1 IntraEst | `0x1440000` | `0x1448000` | `0x1450000` | `0x1242000` | 5 | `0xe3350` (0x446) |
| 2 MotionEst | `0x1420000` | `0x1428000` | `0x1430000` | `0x1188000` | 6 | `0xe0dd0`, `0xe14c0`, `0xe2d40` |
| 3 ModeDecision | `0x1460000` | `0x1468000` | `0x1470000` | `0x1262000` | 7 | `0xe37a0` (0xcde) |
| 4 ReconLuma | `0x1480000` | `0x1488000` | `0x1490000` | `0x1282000` | 8 | `0xe4480` (0x606) |
| 5 ReconChroma | `0x14a0000` | `0x14a8000` | `0x14b0000` | `0x12a2000` | 9 | `0xe4a90` (0xbb8) |
| 6 CAVLC | `0x14c0000` | `0x14c8000` | `0x14d0000` | `0x12c8000` | 13 | `0xe5650` (0x1500) |

(literal pools `0xda9b0..0xdadb0`; `[172]` is the image size). **[C]**

Control block `+0x4` is an **ID register**: the constructor reads
`base + ctrl + 4` and asserts `== unit + 4`
(`0x919c8..0x919dc`, `"ERROR checking mcpu_id…"` `0xcbc9b`, assert line 771).
**[C]** No such assert appeared in F5-F9, so on our Start_AVC all seven ID
registers read correctly — the MCPU blocks were powered, clocked and inside
the firmware's `IOSZ = 0x2000000` window. **[I]**, strong (depends on 1.1's
arm being taken, which our Config selects).

Control block `+0x8` is the run control (`[268]`):

| writer | value | VA |
|---|---|---|
| `StartUnit` (after loading code) | `= 1` | `0x91dd4..0x91de4` |
| `CAvePipeMcpu::Start` | `= 0xe` | `0x91c10..0x91c20` |
| `CAvePipeMcpu::Reset` | `\|= 0xe` | `0x91cf8..0x91d0c` |
| `CAvePipeMcpu::Stop` | `vt+72` then `&= ~1` | `0x91c34..0x91c60` |
| `setPipe`, bSkipMcpu arm | `= 1` on all seven | `0x57e50..0x57e90` |

**[C]**; "bit 0 = run" is **[I]** from `StartUnit` writing it last, after the
code copy, and `Stop` clearing it.

Each unit also builds a `CAvePipeMcpuISRManager(reg = host-if+8, src)`
(`0x919e0..0x91a08`), which writes `0xffffffff` to that register, registers the
ASC interrupt source (4..9, 13) and unmasks it (`0x920c0..0x920fc`). **[C]**

### 1.3 The code is Cortex-M Thumb, embedded in the ASC image

First words of every image are a Cortex-M vector table: MbInput
`10004000 00000125 00000179 0000017d 00000181 …`, IntraEst `10001000 00000125 …`,
CAVLC `10001800 00000125 …`. **[C]** (bytes at file `VA+0x4000`). The initial SP
equals `0x10000000 + DMem size` (`[244]` = `0x4000`, `0x1000`, `0x1800`), so the
MCPU sees IMem at 0 and DMem at `0x10000000`. **[C]** for the numbers, **[I]**
for the map.

IntraEst disassembled as Thumb (`objdump -b binary -m arm -M force-thumb`):
reset `0x124` copies `.data` to `0x10000768`, zeroes `.bss`, writes VTOR
(`0xE000ED08`), calls `main` `0x184`: `NVIC_ISER = 0x08000000` (IRQ 27), sets
bit 13 of `0x41242000`, then `wfi` forever. IRQ 27's handler `0x1a8` reads
`0x41243000`, writes `0x80000025` to `0x41242014` and polls its bit 31. **[C]**
`0x41242000` is `0x40000000 + 0x1242000`, i.e. IntraEst's host-interface block
as the ASC addresses it — **the MCPU sees the encoder register space at
`0x40000000 + (base offset)`**. **[I]**, strong (the offset matches the ASC-side
constant exactly).

So an MCPU is idle until the hardware stage raises its IRQ, then does per-MB
control and handshakes on its host-interface block. It has no DMA path of its
own on the evidence read. **[I]**

### 1.4 Start (per pipe frame)

`CAVCController::ProcessPipeStart` (`0x52d50`), `x22 = this+0x23FE4`:
`setPipe` (`0x52e40`), then **if `[x22]` (bSkipMcpu) == 0 and `[x22+1]`
(bCreateMcpu) != 0**: `[this+1040]->vt+208(1, 0, [x22+55], 0, 0)`
(`0x52ebc..0x52ee8`) = `CAVEPipeMcpuController::Start` `0x396d0` →
`StartAvePipe` `0x3974c` → `StartUnit(1, 0, byte)` for units 0..5, then unit 6
(`[x20+0x1d8]`) (`0x397a0..0x39894`). **[C]** (slot `0xecfb0` = `0x396d0`).
`this+1040` is set in `ProcessInit` only when bSkipMcpu == 0 (`0x4de50..0x4de94`).
**[C]** (docs/57 §2.2 wrote `[this+0x24019]`; the byte is `this+0x2401B`.)

`StartUnit` (`0x91d14`): zero host-if `+0` and `+4`; if the image selection
changed (or `Init` set `[256]`) copy the image with `vt+104` = `WriteIMem`
(`0x91f6c` → word copy helper `0x61a04`) to `IMem + [168]`, size `[172]`
rounded to 4; then `ctrl+8 = 1`; `return 1`. **[C]** No read-back, no wait, no
timeout.

`ConfigureMCPUs` (`0x60298`, called from `setPipe` before the start) writes
parameter blocks into the MCPU DMem windows (`0x142xxxx`, `0x140xxxx`,
`0x14axxxx`, … e.g. `0x60788`, `0x61588`) and, when bSkipMcpu == 0, writes `1`
to ten per-stage registers `0x1170290, 0x11903bc, 0x124a394, 0x126a114,
0x127a098, 0x128a498, 0x129a0d8, 0x12aa724, 0x12ba0cc, 0x12da10c`
(`0x60300..0x6038c`). **[C]** Its two `_RTK_semaphore_wait`s (`0x609f4`,
`0x60a24`) sit on a QP-modulation arm gated by `this+0x23FC5` (= Start_AVC
VP `+0xFF10`). **[C]** They cannot be blocking us: the flow controller is one
task (`CController::Task` `0xa19e8`: one `_RTK_semaphore_wait_multiple`
dispatching command, queue, heartbeat `vt+88` and signals `vt+72`,
`0xa1ad0..0xa1b84`), and the heartbeat keeps printing. **[C]** loop, **[I]**
conclusion.

### 1.5 What the firmware waits for; interrupts from the MCPUs

Nothing MCPU-specific. Pipe completion is source 1 only (docs/57 §2.3). The
MCPU ISR (`CAvePipeMcpuISRManager::ServiceRoutine` `0x921ac`) acks bits 8 and
12 of host-if `+8` and dispatches `HandleSource(8/12)` (`0x92210..0x92258`), which
**asserts** if no callback is registered (`0x92410`, line 321; `0x92394`, line
340). **[C]** No assert was seen, so either no MCPU raised an interrupt or a
handler exists. **[U]** which.

### 1.6 `bSkipMcpu = 1` on this silicon

With skip=1, create=1:

- `ProcessAvcInit` skips `McpuController::Init` (`0xee4c` `cbnz`) → no
  `CAvePipeMcpu` objects, no ID check, no ISR managers, **no code copied**. **[C]**
- `ProcessInit` leaves `this+1040` unset (`0x4de58`). **[C]**
- `setPipe`: `ConfigureMCPUs` (`0x57df4`), `SetTunable(true)` (`0x57e08`),
  then `ctrl+8 = 1` on all seven (`0x57e50..0x57e90`). Inside
  `ConfigureMCPUs` the ten per-stage `= 1` writes are skipped (`0x60300`
  `cbnz`). **[C]**
- `ProcessPipeStart` does not call `Start` (`0x52e44` `cbz`). **[C]**

The kext sets skip only when the `ave-platform` boot-arg is 3
(`0xfffffe0008efadd0..ade0`, docs/57 §3.4) — a platform where the MCPUs are
pre-loaded, presumably. **[I]** On our hardware it releases cores with no image
(on a cold boot their IMem is whatever the SRAM powers up with) and removes the
ten stage enables. Prediction: **same PIPE HANG** or a different hang; an MCPU
executing garbage sees only register space (§1.3), so the blast radius is the
encoder, not host memory. **[I]** A completed frame would be the only
informative outcome.

---

## 2. Q2 — is the pipe kicked?

`CAVCController::setPipeGo` (`0x61fec`, end of `setPipe`):

```
62034  mov w10,#0x128 ; movk #0x111      ; base+0x1110128
6203c  ldr w11,[x9,x10] ; orr #1 ; str   ; bit 0 set
62064  … str 3 → base+0x11E0100, +0x11E4100, +0x11E8100
6209c  … if this[0x2401A]: 3 → +0x11F0100, +0x11F4100, +0x11F8100
```

**[C]** The only precondition is the multicore QP sync on `[this+2828]`
(`0x61ffc`). **[C]**

The register's name comes from `CAVCController::ProcessLRMEStart`
(`0x5122c`): it reads `base+0x1110128` and prints
`"ProcessLRMEStart read: SRCDMAGO 0x%x async %d-%d"` (`0x512e4..0x51308`,
string `0xc448d`), then rewrites it with bit 3 replaced
(`and #0xfffffff7`, `orr` of `this[0x13A3C] ? 8 : this[0x23FEC] << 3`,
`0x51310..0x51380`) — bit 0 preserved. **[C]** So `0x40D110128` is
**SRCDMAGO**, the source-DMA go, and bit 0 is its go.

AVC writers of `0x1110128`: only `ProcessLRMEStart` (`0x512e0`, bit 3) and
`setPipeGo` (`0x62038`, bit 0) (immediate scan over `mov/movk`, which also finds
the known `0x1110140`/`0x111013c` users). **[C]** for the scan, **[I]** complete
(Trap 3). Neither clears bit 0, so reading `0` after the frame started means
**the bit self-clears when the source reader takes it** — the pipe was kicked.
**[I]** Hardware corroboration from F4: with DMA unmapped the datapath DART
faulted walking the input luma buffer (docs/53 §15), i.e. the source reader
does run. **[I]** (LRME also reads the source, so F4 alone does not prove it
was the pipe's reader.)

Source-reader progress is readable: `ProcessPipeDone` (`0x595e8`) reads
`base+0x112002C`, takes `>> 16` as **currMbRow** and compares `+1` with the
picture height in MBs (`0x59690..0x596c4`, log
`"ProcessAvcPipeDone currMbRow %d pic_height_in_mbs %d …"` `0xc4bef`). **[C]**
Reading `0x40D12002C` at the hang tells how far the pipeline got (§7).

---

## 3. Q3 — `0x40D120000` / `0x40D120004`

### 3.1 `0x40D120000 = 0x80034045` is the value the firmware wrote

`setPipe`: `w22 = 0x80034024` (`mov #0x4024; movk #0x8003,lsl#16`, `0x54804`).
On the arm where `[x27+1779] == 0` it forms `w12 = w22 + 0x21 = 0x80034045`,
`w10 = w22 + 0x31 = 0x80034055` (`0x5489c`, `0x548a0`); on the other arm
`w12 = w22 + 0x23` (`0x549a4`). It stores `w12` to `base+0x1120000`
(`mov w9,#0x1120000` `0x549bc`, `str` `0x549c0`) and, if `[this+2692]`, `w10`
to `base+0x1120080` (`x25 - 0xf24`, `x25 = 0x1120fa4`, `0x549cc..0x549d4`). **[C]**

`ProcessPipeReset` (`0x4ed60`) first writes the reset value `0x4004` to
`+0x1120000` and `+0x1120080`, `0x2005` to `+0x1120018`/`+0x1120098`
(`0x4ee08..0x4ee40`). **[C]**

So `0x40D120000` is the **source-luma DMA control word** and `0x40D120080` the
chroma one **[I]** (luma/chroma from the `[this+2692]` chroma gate). F9's read
of exactly `0x80034045` says the reset and the configuration both landed and
the register is not a status latch. Bit 31 is part of a constant the firmware
writes (also in `0x800314B1` for the recon writer, docs/57 §4.3) — an enable,
not an error. **[I]**

### 3.2 `0x40D120004 = 0xc0`

No `mov/movk` constant `0x1120004` exists in the image and no `x25-0xfa0`
style offset was found. **[U]** Most likely a read-only status register of
the same DMA channel. **[I]**, weak.

### 3.3 The AXI-error registers are elsewhere (correction to docs/56 §5.1)

`CAVEPipeISRManager::AxiErrorHandler` (`0x38b04`):

```
38b20  mov  w10,#0x4000
38b30  movk w10,#0x112,lsl#16        ; 0x1124000
38b3c  add  x9, base, x10
38b50  ldr  w13,[x9]                 ; 0x40D124000
38b54  ldr  w14,[x9,#4]              ; 0x40D124004
38b58  ldr  w15,[x9,#0x10000]        ; 0x40D134000
38b5c  ldr  w9, [x9,#0x8000]         ; 0x40D12C000
```

**[C]** docs/56 recorded `0x40d120000, 0x40d120004, 0x40d130000, 0x40d128000`,
dropping the `0x4000`. The F3/F4 `AXI Error: 0x3 0x1 0x0 0x0` values are
`0x40D124000 = 3`, `0x40D124004 = 1`, and **cannot be compared** with F9's
`0x40D120000/4`. The driver's diag line reads the wrong pair for that purpose.

---

## 4. Q4 — pipe interrupt enables

### 4.1 The ASC side: every source is unmasked through RTKit, not the host

`CAVEPipeISRManager`'s constructor unmasks all its sources (docs/57 §2.3,
`UnmaskAll` `0x381a0`) via `CISRManager::instance` (`0x1545b8`) `vt+48`. The
runtime class is `CPlatformISRManager`: `Unmask(src)` for `src <= 0x68` tail-calls
the RTKit interrupt-enable routine with `src | 0x10000` (`0xa7174..0xa7188`,
`b 0xb5fd4`). **[C]** That is the coprocessor's own interrupt controller. No
host register is involved, and LRME (src 15) and LRMERC (src 32) arrive through
the same manager, so delivery works. **[I]**, strong.

### 4.2 The encoder side: `0x40D11013C` is the enable for `0x40D110140`

| writer | bits set in `base+0x111013C` | VA |
|---|---|---|
| `ProcessChipReset` (from `ProcessConfig` `0xe708`) | 0, 12, 13, 14, 15 | `0x233c4..0x23404` |
| `setLRME` (`x25 = 0x1110008`, `0x51ccc`) | 1 (LRME done) | `0x52570..0x52580` |
| `SetLRMERC` (`w20 = 0x1110134`, `[+8]`) | 11 (LRMERC done) | `0x62858..0x62874` |
| **`setPipe`** (`x26 = 0x111000C`, `0x551b4`) | **2 (pipe done)**; bit 1 set or cleared on `[sp+112]` | **`0x57c7c..0x57c88`**, `0x57c5c..0x57c78` |

**[C]** for the writes. The bit numbers match the ack bits of `0x40D110140`
exactly (AXI error 0, LRME 1 `=2`, pipe 2 `=4`, LRMERC 11 `=0x800`, write-full
12/13 `=0x1000/0x2000`, docs/57 §2.3), which makes `0x40D11013C` the enable
register of `0x40D110140`. **[I]**, strong. The pipe-done enable is written by
the firmware in `setPipe`, before `setPipeGo`; the host owns none of it.
Whether `0x40D110140` shows masked or raw status is **[U]**; a read of
`0x40D11013C` at the hang (expect bit 2 set) closes the question.

### 4.3 AIC lines

The kext consumes only ADT interrupt index 0 (docs/57 §3.5, docs/11). Nothing
on the pipe path reads or writes an AP-side interrupt register. **[I]**

---

## 5. Q5 — host-side writes macOS makes that we do not

### 5.1 `AVE_DPE`: a kext-owned block at `0x40D1DC000` that nobody programs for us

**What the kext writes.** `AVE_DPE_GetTunables(DevID)` (`0xfffffe0008ee3204`)
indexes a pointer table at `0xfffffe0007bc3398` by `DevID - 1` (valid mask
`0x0FFFFC07`). DevIDs 14, 15, 16 (t6000/t6001/t6002 on 13.5, docs/45 §2.2,
docs/47) all resolve to **`gs_sAVE_DPE_CfgSet_Castor_6000`**
(`0xfffffe0007bc3018`). **[C]** (chained-fixup low 32 bits matched to symbol
file offsets, Trap 5.)

`CfgSet_Castor_6000`: `+0x08` CAT base `0xdc000`, `+0x10` CAC base `0xdc400`,
`+0x18` CAT enable reg `0xdc000`, `+0x1C` CAC enable reg `0xdc400`; tables
`+0x20/+0x30/+0x40` = CAT Default/8bit/10bit (counts 1/0/0) and
`+0x50/+0x60/+0x70` = CAC Default/8bit/10bit (counts `0x7c`/`0x7b`/`0x7b`).
**[C]**

`AVE_DPE::ApplyTunables(base, table, n)` (`0xfffffe0008ee3e4c`): for each
16-byte entry `{off, 4, clear, set}`, bank-0 read, `bic clear`, `orr set`,
write at `base + off` (`0xfffffe0008ee3f8c..0x3fbc`). **[C]** Bank 0 is
`0x40D100000` (docs/12), so the targets are:

- CAT Default: `0x40D1DC004 = 0x1000` (full mask). **[C]**
- CAC Default: `0x40D1DC400` field `[24:16] = 3`; `0x40D1DC404..0x40D1DC4A0`
  bit 0 = 1 (40 entries); `0x40D1DC4A4` bit 0 = **0**; `0x40D1DC4A8..0x40D1DC5EC`
  9-bit values in pairs (e.g. `+0x1B0 = 0x22`, `+0x1B4 = 0x12`,
  `+0x1D0 = 0x3a`, `+0x1D4 = 0x22`). The 8-bit table repeats `+0x4..+0x1EC`
  with different pair values; 10-bit equals Default. **[C]**
- `AVE_DPE::Enable` (`0xfffffe0008ee4754`, only for DevType >= 8,
  `0xfffffe0008ee4848`): `0x40D1DC400 |= 3` (`0x48e0..0x4908`),
  `0x40D1DC000 |= 1` (`0x4b50..0x4b78`). `Disable` clears bit 0 of each
  (`0x3ab4..0x3ae0`, `0x3d24..0x3d50`). **[C]**

**When.** `AVE_DPE::Reset` (`0xfffffe0008ee4c74`) applies type 0 (Default)
(`0x4f50`), later type 1 (`0x50c4`) and `Enable` (`0x50cc`); the branch
between them on the client count `[this+32]` was not traced. **[C]** calls /
**[U]** branch. Callers: `AVE_HwC::Init` (`0xfffffe0008f0f480`) and
`AVE_HwC::ResetDPE` (`0xfffffe0008f156a8`) ← **`AVE_HwC::PowerOn`**
(`0xfffffe0008f150c8`). Per client, `AVE_HwC::RegisterCHM` → `SetDPE(chm, 1)`
(`0xfffffe0008f173c0`) → `AVE_RegCfgList_Apply` and `AVE_DPE::Add`
(`0xfffffe0008f17968`), which applies type 2 or type 1 and bumps the client
count (`0xfffffe0008ee55d4`, `0x5640`). **[C]** (kext bl scan). Which
condition picks 8-bit vs 10-bit is **[I]** from the table names.

**The firmware never touches it.** An immediate scan of every `mov/movk`
register constant in the image finds nothing in `0x11D8000..0x11E0000`, while
the same scan finds 35 hits in `0x11E0000..0x11E0200` and both known
`0x1110128` writers. **[C]** scan with positive control; **[I]** that no
table-driven access exists (Trap 3).

**The driver never touches it** (no `dpe`/`tunable` writer in `driver/`;
bank 0 is only read, `ave_session.c:1486..1515`). **[C]**

**What it is.** Not named anywhere. 40 enable bits, one explicit disable, and
41 pairs of small per-stage counts whose values change with bit depth read like
a per-stage clock-gating / pipeline-latency configuration. **[I]**, weak. The
LRME stages ran without it, so if it matters it matters to the pipe stages
only. **[I]**

### 5.2 Checked and not a gap

| host step | why not a gap | Conf |
|---|---|---|
| `AVE_AXI2AF::ApplyTunables` (`0xfffffe0008e84180`, from `AVE_HwC::PowerOn` `0xfffffe0008f1522c`) | `AVE_AXI2AF_GetTunables` indexes by `DevID - 3` (`0xfffffe0008e84b30`); DevIDs 14-16 → `gs_sAVE_AXI2AF_Cfg_Castor_6000`, whose RegCfg pointer `+144` and count `+152` are both 0, so the loop at `0xfffffe0008e84270..0x4284` writes nothing | C |
| `AVE_DPM_TuneUpPipe` → SVE `+0x38 = 0` | tested F8, no change (docs/53 §19) | C (hw) |
| PMGR PS 3/4/5/6 ClockOn | all five VENC sub-domains read `0x3ff` in F9 | C (hw) |
| MCPU anything | no kext reference (§0) | C |
| `SetTunable` (register table `0xcf630`, `0x1C00040`, `0xd04c0`) | firmware-owned; called from `InitEncodingParameters` `0x5e858` and `setLRME` `0x5255c` | C |
| SVE `+0x00` reset pulses, SVE `+0x04` bit 5 | firmware-owned (`ProcessChipReset` `0x2328c`, `ProcessPipeReset` `0x4edb8..0x4eddc`) | C |
| `AVE_MCC::Enable` (DSIDs) | memory-controller data-stream IDs, docs/27 §5; a DMA problem would show as faults, and there are none | I |
| `AVE_HwC::SetRegCfg` | driven by a user-client call (`AppleAVE2Driver::SetRegCfg`), not by the encode path | I |

---

## 6. Q6 — `afnc4_ioa`

Live DT (read-only): `power-management@28e680000/power-controller@1a0`,
label `afnc4_ioa`, parent phandle `0x9f`; its only child is
`power-controller@1a8` `afnc4_ls`. **[C]** docs/26's parent chains put AFNC
domains on the fabric path of the *second* encoder (`VENC1_SYS -> AFNC5_LW0 ->
AFNC5_LS -> AFNC5_IOA -> AFI`); `VENC_SYS` (ave0) goes through `AVEMSR-V -> AFR`
only. **[C]** (docs/26). So `afnc4_ioa` is an unrelated fabric adapter domain.
Holding it on costs power and cannot gate anything AVE needs. **[I]** macOS
powers nothing else for ave0 beyond `VENC_SYS`'s parents and PS 3-6
(docs/26, docs/57 §3.2). **[I]** Fix the overlay for hygiene; do not expect a
change.

---

## 7. Ranked causes, changes, and reads

Everything below is a proposal for the operator (AGENTS.md). Confidence is
relative to the others; none is high.

### 7.0 Reads to add to every run (read-only, all inside mapped bank 0)

Log these at the Process timeout, next to the existing diag line:

| AP address | what it says | expected if healthy |
|---|---|---|
| `0x40D12002C` | source DMA progress; `>> 16` = **currMbRow** (fw `0x59690..0x596ac`) | 44 (= 45 MB rows - 1) at done |
| `0x40D11013C` | enable for `0x40D110140` (§4.2) | bit 2 set |
| `0x40D124000`, `0x40D124004`, `0x40D12C000`, `0x40D134000` | the real AXI-error registers (§3.3) | 0 |
| `0x40D1DC000`, `0x40D1DC004`, `0x40D1DC400`, `0x40D1DC4A4`, `0x40D1DC5B0` | DPE reset-state vs macOS values (§5.1) | macOS: bit0, `0x1000`, `0x3xxxx\|3`, bit0 = 0, `0x22` |
| `0x40D410008`, `0x40D450008`, `0x40D430008`, `0x40D470008`, `0x40D490008`, `0x40D4B0008`, `0x40D4D0008` | MCPU run control (§1.2) | `1` |
| same blocks `+0x4` | MCPU ID | 4, 5, 6, 7, 8, 9, 10 |
| `0x40D400000`, `0x40D440000`, `0x40D4C0000` | first IMem word of MbInput / IntraEst / CAVLC | `0x10004000`, `0x10001000`, `0x10001800` (image copied) |
| `0x40D168000/4/8`, `0x40D242000/4/8`, `0x40D2C8000/4/8` | MCPU host-interface of MbInput / IntraEst / CAVLC | non-zero `+0/+4` = MCPU has spoken |
| `0x40D060004` | PIODMA flags (docs/57 §3.7) | 0 |

The MCPU memory windows are read by the firmware itself (`ReadIMem`
`0x91e70`, `ReadDMem` `0x91df4`, `ProcessPipeDone` reads CAVLC DMem
`0x14C8604` at `0x597c0`), so they are readable. **[C]** Whether reading a
host-interface `+8` (interrupt status) has a side effect is **[U]**; put it
last. currMbRow alone splits the space: `0` or a small row = the pipeline never
really started consuming; the last row = the stall is at the output/entropy
end.

### 7.1 The `AVE_DPE` block is at reset defaults (medium-low)

The only host-side register programming on the encode path that macOS always
does and we never do (§5.1), in a block the firmware never touches.

**Change.** New module parameter `dpe_tunables=1` (default 0): after the VENC
domains are powered and before Config, apply the Castor_6000 tables exactly as
`AVE_DPE::Reset` does — CAT Default (`0x40D1DC004 = 0x1000`), CAC Default
(124 read-modify-writes from `0xfffffe00072309d8`), CAC 8bit (123 from
`0xfffffe0007231198`), then `0x40D1DC400 |= 3`, `0x40D1DC000 |= 1`. Generate
the tables from the kext with the §8 script rather than hand-copying.

**Expect if cause:** pipe done (`0x40D110140` bit 2), `StartCount 1-1-1-1`,
`0x0E06`. **If not:** identical hang with the DPE reads showing the macOS
values (apparatus check that the writes landed).

### 7.2 The MCPUs are not executing (low-medium)

The firmware copies code and releases the cores with no check (§1.4). A copy
that did not land, or a core that does not leave reset, stalls the pipeline
silently.

**Change first: none — the §7.0 MCPU reads decide it.** `+8 != 1`, or IMem
word 0 not the vector-table SP, or all host-interface words zero with
currMbRow stuck → MCPU. If they look healthy, this drops to very low.
`session_skip_mcpu=1` is **not** recommended as the discriminator (§1.6): it
releases cores without loading their code and skips the ten stage enables;
the expected result is the same hang, which would teach nothing.

### 7.3 Another zero-skipped output buffer blocks a pipe stage (low-medium)

The recon writer was one (docs/57 §4). `LowResResults`, `stats_DMA_addr`,
`mbAddressCPUFWData` and colocated are still skipped when zero (docs/57 §3.6).
A stage with no DMA destination issues no AXI transaction and never finishes.

**Change:** only after currMbRow says where it stops. If it stops at the last
row, give the stats / MB-address buffers real arenas (sizes from
docs/47) one at a time. **Expect:** progress or a DART fault naming the plane.

### 7.4 Pipe-done is masked or delivered differently (low)

Written by the firmware (§4.2), so unlikely. The `0x40D11013C` read settles it:
bit 2 clear at the hang would be a real finding (`setPipe` path not taken).

### 7.5 `afnc4_ioa` (very low)

Replace `0xc5` with `venc_dma` (`0xc1`) in `test/ave-overlay-e4.dts` for
hygiene (§6). No change expected.

### Suggested next run

One reboot: the §7.0 reads plus `dpe_tunables=1`. The reads are informative
whatever the DPE outcome, and the change is the one macOS step still missing.

---

## 8. Corrections to existing documents (not edited here)

1. **docs/56 §5.1**: the AXI-error values are `0x40D124000`, `0x40D124004`,
   `0x40D134000`, `0x40D12C000` (`0x38b20..0x38b5c`), not `0x40d120000/4`,
   `0x40d130000`, `0x40d128000`. **[C]**
2. **docs/57 §2.2**: the MCPU start byte is `this+0x2401B` (`[x22,#55]`,
   `x22 = this+0x23FE4`, `0x52ed0`), not `0x24019`. "No host buffer is
   involved" stands and is now **[C]** (§1.3). **[C]**
3. **docs/57 §7 #5 / docs/53 §20**: `bSkipMcpu = 1` does not "skip
   `ConfigureMCPUs`": it still calls it (`0x57df4`) and `SetTunable(true)`,
   skips object creation and code loading, and releases the cores (§1.6). **[C]**
4. **`driver/ave_session.c` diag line** labels `0x40D120000/4` "AXI error";
   they are the source-luma DMA control word and an unknown neighbour (§3). **[C]**
5. **docs/12 §4.4 / docs/27 §6** ("no AXI2AF table for 6000/6001, likely a
   no-op"): on 13.5 a `Castor_6000` config exists but carries zero tunables, so
   the conclusion holds and is now **[C]** (§5.2).

---

## 9. Reproduce

```sh
# MCPU objects, register map, start, run control
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x39454 -n 0x27c   # McpuInit
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x912d0 -n 0x7d0   # CAvePipeMcpu ctor
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x91b54 -n 0x4a0   # Load/Start/Stop/Init/Reset/StartUnit/Mem
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x3974c -n 0x16c   # StartAvePipe
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x52d50 -n 0x1e8   # ProcessPipeStart
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57ddc -n 0xe0    # setPipe skip/create arms
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0xee4c  -n 0x30    # ProcessAvcInit -> Init
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x92054 -n 0x400   # MCPU ISR manager
objdump -D -b binary -m arm -M force-thumb <(python3 -c "import sys;d=open('data/blobs/macos-13.5/ave_h13c.bin','rb').read();sys.stdout.buffer.write(d[0xe3350+0x4000:0xe3350+0x4000+0x446])")
# go, enables, DMA word, AXI handler
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x61fec -n 0xb0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x512d4 -n 0xb0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x57c54 -n 0x38
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x54804 -n 0x1c0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x4ed60 -n 0x100
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x38b04 -n 0x9c
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0x595e8 -n 0xe0
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0xa715c -n 0x30
AVE_MACOS=13.5 python3 tools/disas.py --fw --addr 0xa19e8 -n 0x1a0   # single task loop
# kext DPE / AXI2AF
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee3204 -n 0x44
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee3f8c -n 0x38
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee4174 -n 0x140
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee48dc -n 0x30
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee4b4c -n 0x34
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008ee4f48 -n 0x190
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr 0xfffffe0008e84b28 -n 0x44
```

DPE table dump (kext data; pointer low 32 bits are file offsets, Trap 5):

```python
import sys, struct; sys.path.insert(0, 'tools'); import disas
kc = 'data/blobs/macos-13.5/kc.macho'; segs = disas.segments(kc); d = open(kc, 'rb').read()
off = lambda va: disas.va_to_off(segs, va)[0]
for name, va, n in (('CAT_Default', 0xfffffe00072309c0, 1),
                    ('CAC_Default', 0xfffffe00072309d8, 0x7c),
                    ('CAC_8bit',    0xfffffe0007231198, 0x7b)):
    for i in range(n):
        o, _, clr, st = struct.unpack_from('<4I', d, off(va) + 16 * i)
        print(name, hex((0xdc000 if name.startswith('CAT') else 0xdc400) + o), hex(clr), hex(st))
```
