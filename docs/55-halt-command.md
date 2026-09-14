# The Halt command (wire id 14, `CAVE_CMD_POWERDOWN`)

Everything here is read from the **macOS 13.5** blobs — the build this machine
runs ([43](43-macos-13.5-firmware.md)). Reproduce any line with:

```sh
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr <VA> -n <LEN>
AVE_MACOS=13.5 python3 tools/disas.py --fw   --addr <VA> -n <LEN>
```

Kext VAs are kernelcache VAs (`0xfffffe0008…`). Firmware VAs are image VAs
(`__TEXT` VA 0 = file `0x4000`). Per [00](00-methodology.md), every claim is
**[C] confirmed** (read from an instruction, VA cited), **[I] inferred** (chain
stated) or **[U] unknown**.

**Nothing below has been run on hardware.** The proposal in §8 is a proposal.

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| Wire id | **14**, `CAVE_CMD_POWERDOWN`, size `0x40` | **C** |
| Channel | IO (`_E_AVE_IPC_Ch` = 1), `AVE_IPC::Send(1, cmd, 0x40)` | **C** |
| Reply on IO_T2H | **None, ever.** The handler never returns | **C** |
| Completion signal | SVE **scratch 0 reads `0x08042006`** | **C** both sides |
| Does the core stop? | Yes — `wfi` in an infinite loop at fw `0xa689c` | **C** |
| Does `CPU_STATUS` bit 1 (STOPPED) then read 1? | Almost certainly yes | **I** (§5) |
| Does anything clear `CPU_CONTROL`? | **No.** The only writes to `+0x400044` in the whole kext are the three inside `AVE_IOP_Start_*` | **C** |
| Precondition | `AVE_HwC::m_state` (`[HwC+160]`) **== 3** = "IOP started" | **C** |
| **Firmware-side precondition** (added 2026-09-14, **measured**) | **Config must have been processed with `bCreateMcpu = 1`.** `ProcessPowerDown` calls through `[this+0x7A10]` with no null check (`10d44`/`10d48`), and that object's only creator is `ProcessConfig` (`str x0,[x19,#31248]` `0xe84c`, behind `this[1358]` = `bCreateMcpu`, docs/46). Halt before Config = NULL data abort, not a halt. See §9 | **C** |
| Must clients be closed first? | macOS queues per-client Stop+Close and drains them first; the firmware does **not** check | **C** / **C** |
| Restart after Halt | Full `StartUpIOP`: **DATA restore**, scratch 0/1/2, `IOP::Config`, `IOP::Start` (4 writes incl. `CPU_CONTROL = 0`) | **C** |
| Bytes the firmware actually reads | the **u16 at `+0x00`** and the transport length. Nothing else | **C** |

The single most dangerous detail for us: **the host writes `0x08042006` into
scratch 0 during boot** ([33](33-firmware-logging.md) §, `StartUpIOP`
`0xfffffe0008f12004`) and it stays there. macOS therefore **clears scratch 0 to
0 immediately before sending Halt** (`0xfffffe0008efc704`–`710`). A driver that
skips that clear will read `0x08042006` back instantly and report success
whether or not the firmware ever saw the command. That is a discriminator that
can never say "no" — see [00](00-methodology.md) Trap 2.

---

## 1. The wire image

Built by `AVE_HwC::MakeFwCmd_Halt(u64, u32, _S_AVE_TimeOut*, sCAveCmdPowerDown*)`
at **`0xfffffe0008efb06c`**. `x0`=this, `x1`=u64 arg, `x2`=u32 arg (unused),
`x3`=`_S_AVE_TimeOut*`, `x4`=command buffer.

```
efb098  cbz x3, err          ; both pointers required, else return -1001
efb09c  cbz x20, err
efb0a8  mov w2, #0x40        ; memset(cmd, 0, 0x40)
efb0ac  bl  memset
efb0b4  mov w8, #0xe         ; 14
efb0b8  str w8, [x20]        ; +0x00
efb0bc  str x21, [x20, #8]   ; +0x08 = the u64 arg
efb0c0  adrp x8, 0xfffffe000723c000
efb0c4  ldr  q0, [x8, #3264] ; = 0xfffffe000723ccc0, 16 bytes
efb0c8  str  q0, [x20, #16]  ; +0x10..+0x1F
efb0cc  str  wzr, [x20, #32] ; +0x20
efb0d0  ldr  q0, [x19]       ; 16 bytes of _S_AVE_TimeOut
efb0d4  stur q0, [x20, #40]  ; +0x28..+0x37
efb0b0  mov w0, #0           ; return 0
```

The 16-byte literal, read straight out of the Mach-O (`kc.macho` file offset
`0x238cc0`, `__PRELINK_TEXT,__text`) — **[C]**:

```
00 00 00 00  00 00 00 00  00 00 00 00  ff ff ff ff
```

which, laid on the 13.5 header ([46](46-abi-13.5-commands-session.md) §0), is
client-id 0, `+0x14` 0, codec 0, slot `0xFFFFFFFF`. Same literal as Config.

### 1.1 Byte table — a plain halt

`SendFwCmd_Halt` passes `x1 = 0` (`0xfffffe0008efc774`), so `+0x08` is zero.

| off | sz | value | source | does the firmware read it? |
|---|---|---|---|---|
| `+0x00` | u16 | **`0x000E`** | `mov w8,#0xe` `efb0b4`, `str w8,[x20]` `efb0b8` | **yes** — `ldrh w8,[x23]` fw `0xd65c`, dispatch index `w8-1`, `cmp #0xd` `0xd6e4` |
| `+0x02` | u16 | 0 | memset | written back: `strh wzr,[x23,#2]` fw `0xd69c` |
| `+0x04` | u32 | 0 | memset (host stores id as u32) | no |
| `+0x08` | u64 | **0** | `str x21,[x20,#8]` `efb0bc`; caller passes 0 `efc774` | no |
| `+0x10` | u32 | 0 (client id) | `q0` literal `0xfffffe000723ccc0` | no |
| `+0x14` | u32 | 0 | same | no |
| `+0x18` | u32 | 0 (codec, 0=AVC) | same | **logging only** — `ldr w26,[x23,#24]` fw `0xd6b4` |
| `+0x1C` | u32 | **`0xFFFFFFFF`** (slot) | same | no |
| `+0x20` | u32 | 0 (priority) | `str wzr,[x20,#32]` `efb0cc` | no |
| `+0x24` | u32 | 0 | memset | no |
| `+0x28` | u32 | `cfg[+20] * 3000` | `efc734`–`efc744` | no |
| `+0x2C` | u32 | 0 | `stp xzr,xzr,[x29,#-64]` `efc620` | no |
| `+0x30` | u64 | firmware-clock time, ms | `efc714`–`efc76c` | no |
| `+0x38` | u64 | 0 | memset | no |

**Which bytes matter — [C].** For id 14 the firmware touches the command buffer
in exactly three places, all inside `CmdProcessor` (`0xd614`): the u16 id at
`+0x00`, a zero store to `+0x02`, and a read of `+0x18` for a log line.
`ProcessPowerDown` (`0x10ca8`) takes only `this` and never dereferences the
command at all. The **transport length must be `0x40`** — fw `0xde08`
`cmp w22,#0x40; b.ne 0xe1a8`, and `0xe1a8` is `_bsp_assert_fail` on
`"sizeof(struct sCAveCmdPowerDown)"` (`0xbcccd`, line 1439) followed by the
`b .` spin. Everything else is **advisory**: id 14 is also the **only** id the
dispatcher runs without `CController::Post` (compare id 1 at `0xd704`), so the
header fields the ring layer normally consumes are never looked at.

### 1.2 The `_S_AVE_TimeOut` fields

Identical code builds them for every command (compare `SendFwCmd_Config`
`0xfffffe0008efc078`–`0c0`), so this is a generic header, not Halt-specific.

```
efc734  ldr x9, [x19, #32]     ; AVE_HwC.cfg  (_S_AVE_Cfg*)
efc738  ldr w9, [x9, #20]
efc73c  mov w10, #0xbb8        ; 3000
efc740  mul w9, w9, w10
efc744  stur w9, [x29, #-64]   ; struct +0  -> cmd +0x28

efc714  bl  AVE_GetCurrTime          ; 0xfffffe0008f43530
efc728  smulh/asr #7                 ; / 1000
efc748  ldr x9, [x19, #240]          ; AVE_HwC.fw_clock_offset
efc75c  smulh with -0x20c49ba5e353f7cf, asr #7   ; / -1000
efc768  add x8, x9, x8
efc76c  stur x8, [x29, #-56]   ; struct +8  -> cmd +0x30
```

* `AVE_GetCurrTime` (`0xfffffe0008f43530`) is `absolutetime_to_nanoseconds(...)`
  then `(t >> 3) / 125`, i.e. **nanoseconds / 1000 = microseconds**. **[C]**
* `[AVE_HwC+240]` is set in `AVE_HwC::StartUp` to
  `AVE_GetCurrTime() - AVE_IOP::GetCurrTime()` (`0xfffffe0008f11684`–`6a4`) —
  the host-to-firmware clock **offset in µs**; zeroed again in `ShutDown`
  (`0xfffffe0008f14de8`). **[C]**
* So `+0x30` = `(host_us − offset_us) / 1000` = **the firmware's own clock in
  milliseconds** at the moment the command was built. **[C]**
* `+0x28` = `cfg[+20] × 3000`. The units are milliseconds by the same
  reasoning; `cfg[+20]` is a scale factor whose default we have not read, so
  with a scale of 1 this is **3000 ms**. Scale value: **[U]**; units: **[I]**.

**A sane value for us: `+0x28 = 3000`, `+0x30 = 0`.** Neither is read by the
firmware for this command (§1.1), so both are cosmetic. **[C]**

---

## 2. Send path and preconditions on the host

`AVE_HwC::SendFwCmd_Halt()` at **`0xfffffe0008efc600`**, in order:

| VA | what |
|---|---|
| `efc624`/`628` | `x22 = [this+224]` (command-buffer base), `w23 = [this+232]` (offset) |
| `efc6ec`–`6f4` | `ldr w8,[x19,#160]; cmp w8,#3; b.ne` — **state must be 3** |
| `efc6f8`–`6fc` | `[this+224]` must be non-null |
| `efc700` | `x21 = x22 + x23` — the 0x40-byte command lives here |
| `efc704`–`710` | **`AVE_SVECtrl::WriteScratch(0, 0)`** — clear the handshake scratch |
| `efc714`–`76c` | build `_S_AVE_TimeOut` on the stack (§1.2) |
| `efc770`–`780` | `MakeFwCmd_Halt(0, 0, &timeout, cmd)` |
| `efc968`–`978` | `AVE_IPC::Send(ch = 1 /* IO */, cmd, 0x40)` |
| `efc980` | zero return = queued |

`_E_AVE_IPC_Ch` 1 = `IO`, 2 = `IO_T2H` ([45](45-abi-13.5-boot-ipc.md) §78) —
**same channel as Config / Open / Start / Process**. **[C]**

### 2.1 The state field

`AVE_HwC::m_state` at `[AVE_HwC+160]`. All writers — **[C]**:

| value | set by | VA | meaning |
|---:|---|---|---|
| (init) | `AVE_HwC::Init` | `f10198` | constructed |
| **1** | `AVE_HwC::PowerOff` | `f1595c` | power removed |
| **2** | `AVE_HwC::PowerOn` | `f1554c` | powered, IOP **not** started |
| **2** | `AVE_HwC::ShutDown` | `f14df0` | back down from 3 |
| **3** | `AVE_HwC::StartUp` | `f11514` | **IOP started, firmware running** |

Readers that gate on it: `SendFwCmd_Halt` `== 3` (`f14c…`, `efc6ec`);
`AVE_HwC::ShutDown` `== 3` (`f14cdc`–`e4`); `AVE_HwC::StartUp` `== 2` → do
`StartUpIOP`, `== 3` → already up, return 0 (`f113e8`–`400`);
`AVE_HwC::PowerOff` `== 1` → already off (`f15874`–`80`).

So **state 3 = "the boot handshake completed and the heartbeat timer is
running"**. For our driver the equivalent precondition is: the core was started
in *this* module load and messages 1–5 completed.

---

## 3. Does the firmware reply? No.

Dispatcher case for id 14 — fw `0xde08`:

```
de08  cmp  w22, #0x40        ; insize
de0c  b.ne 0xe1a8            ; -> _bsp_assert_fail + b .
de10  ldr  x8, [x19]         ; vtable
de18  mov  w1, #0xe
de1c  ldr  x8, [x8, #40]     ; name lookup for the log line
de24  blr  x8
de38  bl   0xbe08            ; CFlowControllerBase::Print
de40  bl   0x10ca8           ; ProcessPowerDown   <-- never returns
de44  b    0xe214            ; unreachable
```

All other ids converge on `0xe218`: `str wzr,[x20]` (`*out = 0`) then
`ProcessQueue` (`0x10d94`), which is where replies are emitted. Id 14 cannot
reach it, because `ProcessPowerDown` tail-branches into an infinite `wfi` loop
(§4). `NotificationToHost` (`0x13684`) is **not** called anywhere in
`ProcessPowerDown`; the only `bl`s in `0x10ca8`–`0x10d94` are
`GetCurrentTaskID`, `Print`, `MaskAll`, `readTimeBase`, `AVE_History_Add`,
`FlushAll`, `ProcessChipReset`. **[C]**

**Consequence for our timeout handling:** do **not** wait on IO_T2H. There is
no reply id, no `0xEE0000` status, nothing. The only completion signal is
scratch 0. A driver that blocks on a reply will time out on a *successful*
halt. **[C]**

---

## 4. Firmware-side trace

`CFlowControllerBase::ProcessPowerDown()` — **`0x10ca8`**:

| VA | call | meaning |
|---|---|---|
| `10cd0` | `CTaskPool::GetCurrentTaskID` (`0x99064`) | log arg |
| `10cf0` | `CFlowControllerBase::Print` (`0xbe08`) | log |
| `10cfc` | `CAVEPipeISRManager::MaskAll` (`0x38218`) on `[this+31256]`, if non-null | mask every pipe interrupt |
| `10d34` | `AVE_History_Add(hist, readTimeBase(), 0, 0x80, 3430, -1, -1, str)` (`0x22d08`) | trace record |
| `10d40` | `CAVEPriorityQueue::FlushAll` (`0x189e8`) on `this+0x7A38` | drop queued work |
| `10d50` | virtual `vptr+184` on `[this+0x7A10]` (a controller created only in `ProcessConfig`, `0xe84c`, behind `bCreateMcpu`) | **[U]** which method. **Not null-checked** (unlike `+0x7A18` at `10cf8`): with no Config, `10d48` faults — measured, §9 |
| `10d58` | `CAVECommonController::ProcessChipReset(true)` (`0x2328c`) | long RMW sequence over the AVE datapath register block (same shape as the Config handler's, [46](46-abi-13.5-commands-session.md) §) |
| `10d74` | `str wzr, [regbase + 0x1050000]` | one 32-bit zero write into the AVE register block (base from the runtime table at fw `0x21a7b8`, the same base `ProcessChipReset` uses). Host-side meaning **[U]** |
| `10d90` | `br [[CEnvironment*]+48]` | tail call — see below |

The global loaded at `10d78` is the `CEnvironment*` stored by
`CEnvironment::CEnvironment` at `0x9ea88` (fw BSS `0x154638`). Its vtable
pointer is `vtable_sym + 16`, so `+48` is the entry at `__ZTV12CEnvironment`
`0xedfe0 + 0x40` = **`Shutdown`**. The concrete object is a
`CPlatformEnvironment`, whose `Shutdown` is `0xa67b8`. **[C]**

`CPlatformEnvironment::Shutdown()` — **`0xa67b8`**:

```
a67c8  [env+264] ? virtual +8                  ; tear down one object
a67f8  loop over [env+280][0 .. [env+288])  bl 0x953ec
a6810  bl 0xb70b4
a6814..a686c  three "if (x) { 0xb6f00(x,1); 0xb7108(x); }" teardowns
a6870  bl 0xba638(1)
a6874  bl 0xac4d8
a687c  bl 0xb9488(1)
a6880  ldr x0, [x19, #312]                     ; CPlatformGPIOManager*
a6884  mov w2, #0x2006 ; movk w2, #0x804,lsl#16 ; 0x08042006
a6888  mov w1, #0
a6894  ldr x8, [x8, #48]                       ; vptr+48
a6898  blr x8                                  ; GPIOManager::Write(0, 0x08042006)
a689c  wfi
a68a0  nop
a68a4  nop
a68a8  b   0xa689c                             ; forever
```

`vptr+48` on `__ZTV20CPlatformGPIOManager` (`0xee320`, vptr `0xee330`) is
`CPlatformGPIOManager::Write(u32, u32)` at `0xa69a8`. **[C]** Index 0 is the
register the host calls SVE **scratch 0** — the firmware reads the same index
at boot to decide standalone mode (`GPIO::Read(0) != 0x08042006`,
[36](36-ipc-implementation.md) §), and the host writes scratch 0 through
`AVE_SVECtrl::WriteScratch(0, …)`. **[C]** both sides.

So the sequence is: **mask interrupts → flush queues → chip reset → write
`0x08042006` into scratch 0 → `wfi` forever.** It is not a software teardown
that leaves the core spinning on work: it is an architectural `wfi` with no
wake path back into the dispatcher. There is no SMC/PMGR handshake and no ASC
power-state write on the firmware side; the PMGR work is all on the host
(§6). **[C]**

---

## 5. Does `CPU_STATUS` read STOPPED afterwards?

This is the question that costs a reset if answered wrong, so the evidence is
laid out separately from the conclusion.

**What is confirmed.**

1. `AVE_HwC::ShutDownIOP` calls `AVE_IOP::Stop()` immediately after
   `SendFwCmd_Halt` returns — `0xfffffe0008f14614`.
2. `AVE_IOP::Stop()` (`0xfffffe0008f21b14`) does **no writes**. It dispatches
   through the per-SoC table at `0xfffffe0007bc39c8` (stride `0x28`, index
   `ChipType − 1`, entry `+24`) and polls. **[C]**
3. Entry `+24` is `AVE_IOP_CheckIdle_<variant>`. Decoding the table:
   ChipType 9 → `_Nyx`. The 13.5 device table (`0xfffffe0007bc2a38`, stride
   `0x28`) gives `t6001` → ChipType **9**, DevType 12, DevID 15 — so **`_Nyx`
   is our variant on 13.5**. (Note: [00](00-methodology.md) records
   "ChipType 7 → `_Nyx`"; that is the *26.6.2* numbering. On 13.5, ChipType 7
   is `_Acis`. The function bodies are identical, so nothing downstream
   changes.) **[C]**
4. `AVE_IOP_CheckIdle_Nyx` (`0xfffffe0008f1f2e4`):
   ```
   f1f3a4  mov x0, x19
   f1f3a8  mov w1, #1               ; bank 1 (ASC)
   f1f3ac  mov w2, #0x48 ; movk #0x40,lsl#16   ; 0x400048 = CPU_STATUS
   f1f3b4  bl  AVE_Reg::Read32
   f1f3bc  tst w0, #3
   f1f3c0  csel w20, w0, wzr, eq    ; return status if (status & 3)==0 else 0
   ```
   **[C]**
5. `AVE_IOP::Stop` succeeds when `CheckIdle` returns **0 three times running**
   (`f21c40`–`f21c54`, exit `f21f10` with return 0), polling every 50 µs
   (`IODelay(50)`, `f21c6c`) up to `cfg[+24] × 10000` iterations. Returning 0
   means **`(CPU_STATUS & 3) != 0`**. **[C]**
6. Nothing in the entire 13.5 kext writes `+0x400044` except the three writes
   inside each `AVE_IOP_Start_*` (exhaustive scan of the `AppleAVE2`
   `__TEXT_EXEC` range for `mov #0x44 / movk #0x40,lsl#16` immediates).
   macOS never clears `CPU_CONTROL` to stop the core. **[C]**
7. macOS does **not** poll `CPU_STATUS` after `AVE_IOP::Start`
   (`StartUpIOP` `0xfffffe0008f122dc` just checks the return code). The only
   consumer of `CheckIdle` in the kext is `AVE_IOP::Stop`. **[C]** — our
   driver's post-start idle poll in `ave_asc_start` is an Asahi addition, not
   Apple's.

**What is inferred.** `(CPU_STATUS & 3) != 0` names bit 0 (`RUNNING`) or bit 1
(`STOPPED`) — m1n1 `proxyclient/m1n1/hw/asc.py`. On this machine the value read
before the core has ever been started is **`0x2a`** = bits 1, 3, 5, i.e.
`STOPPED | FIQ_NOT_PEND | IDLE`, so `& 3 == 2`
([22](22-driver-plan.md) stage 8); after the start sequence the same poll saw
`& 3 == 0` (stage 9), so **bit 1 is the bit that clears on start**. macOS's
post-Halt wait is the exact inverse condition. The bit that must come back is
therefore bit 1.

> **Verdict: after a successful Halt, `CPU_STATUS` (ASC `+0x400048`) bit 1
> `STOPPED` reads 1, and the register is expected to read `0x2a` again.**
> Confidence: **high, but [I] not [C]** — what is confirmed is `(v & 3) != 0`;
> that the responsible bit is bit 1 rests on the 0x2a/start-clears-it pair.
> The failure mode if this is wrong is benign: the core is provably in `wfi`
> either way (§4), and the restart path writes `CPU_CONTROL = 0` before
> `CPU_CONTROL = RUN` regardless (`AVE_IOP_Start_Nyx` `0xfffffe0008f1f1f0`).

---

## 6. Preconditions and the full macOS teardown order

Entry points, all **[C]**:

* `AVE_Drv::TryPowerOff` (`0xfffffe0008ef082c`) — gates on `[drv+176] == 1`
  **and** `AVE_DLList_Empty(&drv->clients)` (`0xef092c`–`34`); only then calls
  `PowerOff`. This is the "all clients closed" gate.
* `AVE_Drv::ForcePowerOff` (`0xfffffe0008ef0438`) — logs a power-state
  transition and calls `PowerOff` unconditionally.
* `AVE_Drv::PowerOff` (`0xfffffe0008eef70c`) — the real sequence:

| step | VA | what |
|---|---|---|
| 1 | `eef810` | per client: `AVE_Client_DropGroupCmds` |
| 2 | `eef838` | per HwC: `AVE_HwC::Prepare_First` → `StopFwHeartBeatTimer` |
| 3 | `eef85c`–`8b8` | drain loop: `AVE_Drv::Drain_First` + per-HwC `AVE_HwC::Drain_First` (counts commands still in input/ready), `IODelay(10)`, bounded |
| 4 | `eef924` | per client: `AVE_Drv::MarkFrameNonDrop` |
| 5 | `eef938`/`950` | `Finish_First` (Drv, then each HwC → `AVE_CHMList_SkipAllCmds_First`) |
| 6 | `eef96c` | `AVE_ClientList_SetPowerState(list, false)` |
| 7 | `eef984` | per HwC: `AVE_HwC::Prepare_Second` — for **each client whose state == 3**, `AVE_CHM_AppendCmd(chm, 7, …)` then `AVE_CHM_AppendCmd(chm, 4, …)` (`f16dd4`, `f16df0`). Host enum 7 = **Stop**, 4 = **Close** ([46](46-abi-13.5-commands-session.md) §1.2 — on the wire these become 13.5 ids **6 `UNINIT`** and **12 `STOP`**) |
| 8 | `eef9cc` | drain loop: per-HwC `Drain_Second` until empty or timeout |
| 9 | `eefad0` | per HwC: **`AVE_HwC::ShutDown()`** |
| 10 | `eefaf8` | per HwC: `Finish_Second` |
| 11 | `eefb20` | per HwC: **`AVE_HwC::PowerOff()`** |

`AVE_HwC::ShutDown` (`0xfffffe0008f14c00`): requires state == 3 (`f14cdc`);
`StopFwHeartBeatTimer` (`f14cec`); **`ShutDownIOP()`** (`f14cf4`);
`SetMCC(-1)` (`f14de4`); `[this+240] = 0`; state = **2**.

`AVE_HwC::ShutDownIOP` (`0xfffffe0008f14438`) — the halt proper:

| VA | what |
|---|---|
| `f14534`,`548`,`55c`,`570` | `AVE_PMGR::SetPS(PD = 3, 4, 5, 6 → PS = 2, wait = true)` |
| `f1457c` | `AVE_PMGR::SetClockGating(false)` |
| `f14584` | **`SendFwCmd_Halt()`** |
| `f14614` | `AVE_IOP::Stop()` — poll `CPU_STATUS` (§5) |
| `f14618`–`744` | poll `AVE_SVECtrl::ReadScratch(0)` until it equals `0x08042006`; `IODelay(100)` per iteration; bound `cfg[+24] × 10000` |
| `f14744` | `IODelay(100)`, return 0 |

`AVE_HwC::PowerOff` (`0xfffffe0008f15794`): requires state != 0/1;
`SVECtrl::GetIntr` + `ClearIntr`; `SurfaceMgr::DARTFlushAll`; `DisableOp`;
`AVE_DART::SetActive(false)`; **`AVE_DPM_PowerOff`** (`0xfffffe0008eea1e0`);
state = **1**.

`AVE_DPM_PowerOff`: `AVE_DPM_Stop`, then `SetPS(PD 3 → 0)`, `SetPS(… → 0)`,
`SetClockGating(true)`, `SetPS(PD 0 → 0)`, `SetPS(PD 2 → 0)`,
`SetPS(PD 10 → 0)` (`eea2a0`–`eea310`). PS 0 is the real gate-off; the PS 2
used before Halt is a lighter state whose meaning is **[U]**.

**Answer to "must clients be closed first":** the *firmware* does not check —
`ProcessPowerDown` takes no client argument and the dispatcher runs it without
`CController::Post`. But macOS always arrives with every client stopped and
closed and every queue drained (steps 1–8). Sending Halt with live clients is
**[U]**: untested on either side, and it is the kind of untested ordering that
has cost this project reboots before.

---

## 7. What happens on the way back up

`AVE_HwC::StartUp` (`0xfffffe0008f1130c`): state 3 → return 0 (already up);
state 2 → `StartUpIOP()` then state = 3 and `StartFwHeartBeatTimer`; anything
else → error. So a Halt leaves the HwC at state 2, and the restart is the
**whole** boot again, not a lighter resume. **[C]**

`AVE_HwC::StartUpIOP` (`0xfffffe0008f119e0`), in order — **[C]**:

1. `f11ae4` **`AVE_Firmware::UpdateImage()`** — the `__DATA` restore
   ([51](51-firmware-data-restore.md)). First call in the function, before
   anything else.
2. IPC surface allocation and channel memory.
3. `f12004` `WriteScratch(0, 0x08042006)`; `f1211c` `WriteScratch(2, DevID)`;
   `WriteScratch(1, instance)`.
4. `f1224c` `AVE_IOP::Config(fw_base)` (skipped when the image is iBoot-loaded
   and ChipType > 5 — [45](45-abi-13.5-boot-ipc.md) row 15).
5. `f122dc` `AVE_IOP::Start()` → `AVE_IOP_Start_Nyx`: `0x400808 = 1`,
   **`0x400044 = 0`**, `0x400400 = 0x10000`, `0x400044 = 0x10`.
6. Apple messages 1–5 and channel creation.

Note step 5: the restart path itself writes `CPU_CONTROL = 0` before setting
`RUN`. So macOS's model is "the firmware stops itself; the host then re-runs
the normal start sequence, which includes the `CPU_CONTROL = 0` write it never
uses on its own". And step 3 re-arms scratch 0 with `0x08042006` — which is
exactly why the pre-Halt clear in §2 exists.

---

## 8. What the driver should do

### 8.1 Teardown sequence to implement at `rmmod`

Minimum viable version, in this order. Steps marked *(macOS also does)* are
Apple's but are not obviously load-bearing for a bring-up driver that has one
client and no DPM.

1. Stop any frame submission and let outstanding Process commands complete or
   time out. *(macOS: steps 1–5 of §6)*
2. For each open client, send **Stop** (wire id 6, `UNINIT`, size `0x40`) then
   **Close** (wire id 12, `STOP`, size `0x48`) and wait for their replies on
   IO_T2H. *(macOS: step 7)* If we have no open client — the current state —
   skip.
3. **`scratch[0] = 0`.** Non-optional. `AVE_SVE_SCRATCH(0)` in
   `driver/ave_hw.h`.
4. Send the 0x40-byte Halt on the **IO** ring: `id = 14`, `+0x1C = 0xFFFFFFFF`,
   `+0x28 = 3000`, everything else zero.
5. **Do not wait for a reply.** Poll `scratch[0]` for `AVE_SCRATCH0_STOPPED`
   (`0x08042006`) — 100 µs interval, ~1 s budget, matching macOS.
6. Poll `CPU_STATUS` until `(v & AVE_ASC_STATUS_BUSY) != 0` for three
   consecutive 50 µs samples — Apple's own `AVE_IOP::Stop` criterion. Log the
   raw value either way.
7. Only then drop power / unmap.

On the next `insmod`, the existing path is already right: restore `__DATA`
([51](51-firmware-data-restore.md)), write scratch 0/1/2, run the four-write
start sequence (which clears `CPU_CONTROL` first), re-do messages 1–5. Nothing
extra is needed.

`driver/ave_drv.c:166` (`ave_asc_stop`) is the TODO this fills, and
`AVE_SCRATCH0_STOPPED` is already defined in `driver/ave_hw.h:176`.

### 8.2 Deviations from macOS, and what they might cost

* We cannot do `AVE_PMGR::SetPS(PD 3–6 → PS 2)` / `SetClockGating(false)`
  before the Halt: those are raw PMGR writes, which AGENTS.md forbids and
  which genpd owns. Their purpose is **[U]**. This is the most likely reason a
  halt attempt could behave differently from macOS's. It is a *pre*-halt
  operation, so if it matters at all it would most plausibly show up as the
  firmware never reaching its `wfi` — which is a timeout, not a hang.
* macOS arrives with clients closed. We would be halting an idle-but-open
  firmware. Untested. **[U]**

### 8.3 How to tell success from failure cheaply

The whole point is that a wrong guess costs a reset, so every check below is a
**read**, and the only new write is a command on a ring that already carries
Config / Open / Start_AVC / Process ([53](53-first-frame.md)). No new register
block, no new DART, no second driver — the containment argument that
[00](00-methodology.md) Trap 6 warns about is not being stretched here, because
nothing new is being mapped or powered.

**Before the halt, record:** `CPU_STATUS`, `scratch[0..3]`, SVE interrupt
status. All already read safely today.

**Then, in order:**

| observation | reading |
|---|---|
| `scratch[0]` goes `0` → `0x08042006` | the firmware executed `CPlatformEnvironment::Shutdown` to the instruction before `wfi`. This is the decisive one, and it is **[C]** on both sides. |
| `CPU_STATUS & 3` goes `0` → non-zero, and specifically `0x2a` | the core is architecturally stopped. Matches Apple's own criterion. |
| `scratch[0]` stays `0`, `CPU_STATUS` unchanged | the command never dispatched. Wrong ring, wrong size, or the doorbell never rang. **Harmless** — the firmware is where it was; reboot as usual. |
| `scratch[0] == 0x08042006` but `CPU_STATUS & 3 == 0` | the firmware halted but the ASC does not report it the way we predicted. Still worth everything: the core is in `wfi` (§4, **[C]**), so the next `insmod`'s `CPU_CONTROL = 0` write has a genuinely quiescent core to act on for the first time. Try the start sequence; if it boots, we have the no-reboot loop anyway. |
| `scratch[0] == 0x08042006` and the machine then wedges | reset. Record which of the two polls was in flight. |

**The negative control.** Because the host sets `scratch[0] = 0x08042006` at
startup, the poll in step 5 will pass trivially if the clear in step 3 is
skipped or fails. Prove the apparatus before trusting the result: read
`scratch[0]` back immediately after writing 0 and **abort the experiment if it
does not read 0**. A run where that readback was never checked measures
nothing — see [00](00-methodology.md) Trap 2 and Trap 7.

**What is not safe to try.** Writing `CPU_CONTROL = 0` on a *running* core to
provoke a stop is not part of this: macOS never does it
(§5 item 6), it is already known not to work ([31](31-bringup-state.md) stage
15), and it is not needed — the Halt path replaces it entirely.

---

## 9. First hardware run (2026-09-14): Halt before Config crashes the firmware

`results/h1-1789369083.kmsg`, commit `73de91d`. Fresh boot, patched m1n1,
overlay `variant=3`. Load 1 `stop_after=16 fw_map_data=1 fw_map_text=2
fw_halt=1` — handshake complete, **no Config sent**. `rmmod` sent the Halt.

- The scratch-0 clear stuck (read back 0), the command went out on IO, and the
  firmware **received and dispatched it** — within 0.6 ms it printed on
  TERMINAL:

  ```
  !! Exception !! crash type 4
  [Call stack] 0x10D48 0xDE40 0xA1CC8 0xA1AB4 0xA19AC 0xB3448
  pc 0x10D48  psr 0x60000004  far 0x0  esr 0x96000007  lr 0x10D44
  ```

  `esr 0x96000007` = data abort, same EL, translation fault level 3; `far 0`.
  `0xDE40` is the `bl 0x10ca8` in the id-14 dispatch arm (after the size check
  at `0xde08`), and `0x10D44`/`0x10D48` is `ldr x0,[x19,#31248]; ldr x8,[x0]`
  — a NULL object pointer at `CmdProcessor+0x7A10`.
- The only instruction in the image that stores a non-zero value there is
  `0xe84c`, in `ProcessConfig`, gated on `this[1358]` — `bCreateMcpu`, set from
  Config `+0x41` at `0xe54c` (docs/46 §row `bCreateMcpu`). Everything else is
  the zeroing at `0xb684` or reads. **C.**
- Scratch 0 stayed 0 for the full second, so the poll said "no" correctly:
  **the discriminator works in the failure direction.** Nothing wedged; unload
  completed and powered off.
- Load 2 read `CPU_STATUS 0x28` (not STOPPED, not the running `0x2c`: a
  crashed core), and `fw_restore_data=1` **refused**, as designed. That is the
  restore gate's first real "no".

The §0 claim "the firmware does **not** check" preconditions is still literally
true — it checks nothing — but it *depends* on Config having run. macOS never
reaches this state because `StartUpIOP` always sends Config before the IOP is
considered up.

**Driver change:** `ave_session_halt()` now refuses unless Config was accepted
with `create_mcpu` (`ave->mcpu_created`), and `session_config_only=1` stops the
self-test after Config so Halt can be proven with no client open.
`tools/halt-run.sh` refuses a load 1 without `session_selftest=1`.

**Next run** (needs a fresh boot — the crashed core is not STOPPED):

```sh
sudo insmod test/ave-overlay.ko variant=3
tools/halt-run.sh h2 \
  "stop_after=16 fw_map_data=1 fw_map_text=2 session_selftest=1 session_config_only=1 fw_halt=1" \
  "stop_after=16 fw_map_data=1 fw_map_text=2 session_selftest=1 session_config_only=1 fw_halt=1 fw_restore_data=1"
```

Load 2 halts again, so a success leaves the core STOPPED for further loads in
the same boot.

---

## 10. Second hardware run (2026-09-14 08:10): Halt works; restart does not

`results/h2-1789369852.kmsg`, commit `ad89724`. Fresh boot, overlay
`variant=3`, both loads `session_selftest=1 session_config_only=1 fw_halt=1`,
load 2 adds `fw_restore_data=1`.

**Load 1 / unload 1 — the Halt, confirmed on hardware:**

```
session: Config: ACCEPTED, status 0xee0000
halt: sending command 14, 64 bytes ... on IO (no reply is expected)
halt: scratch 0 = 0x08042006, the firmware reached its wfi        (+0.5 ms)
halt: CPU_STATUS 0x0000002e STOPPED after 3 sample(s)
```

§5's inference is now **C**: after Halt `CPU_STATUS` bit 1 (STOPPED) reads 1.
No DART fault storm and no IRQ trouble on unload.

**Load 2 — the first real DATA restore, then a start that does not take:**

- The restore gates all passed (`CPU_STATUS 0x2e`, blob sha256, TEXT match,
  range foreign); **301006 bytes in 167 pages had drifted**, first at
  DATA+`0x2950`; `0x134000` bytes written, **read-back verified**, 0 bytes
  differ. docs/51 §7's "whether Linux can write this DRAM at all" is answered:
  **yes. C.**
- `ave_asc_start()` then wrote the four `AVE_IOP_Start_Nyx` values and
  `CPU_STATUS` stayed `0x2e` for 100 ms (`ASC did not become idle`). On a fresh
  boot the same writes take `0x2a` to `0x2c`.

**Reading:** the core is sitting in `ProcessPowerDown`'s `wfi` loop, and
`CPU_CONTROL 0 -> RUN` does not reset it. macOS never asks it to: between the
Halt and `StartUpIOP` it runs `AVE_DPM_PowerOff` (§6), and that power cycle
is what puts the core back at its reset vector. **I** (consistent with every
observation; no instruction shows the reset explicitly).

**Why that is hard here:** `pm_genpd_summary` after the run shows the five
VENC leaves `off-0` and **only `venc_sys` on**, with both DARTs `suspended` and
no active child - and no `apple,always-on` in its DT node. Its only direct
devices are the two DARTs. The DART, and the DAPF entry m1n1 installs at boot,
live in `venc_sys`; Linux cannot write that DAPF (docs/49). So a power cycle
or reset deep enough to restart the core is expected to take the DAPF entry
with it. **I**, and exactly what `core_reset` (with its before/after DAPF
comparison) exists to measure.

**What this run buys regardless:** Halt gives a clean unload (buffers can be
freed, no fault storm), and the restore is proven. What it does not yet buy is
a second firmware start in the same boot.

---

## 11. R1 (2026-09-14 08:25): the block reset on a halted core keeps the DAPF

`results/r1-1789370719.kmsg`, commit `db259ea`, the same boot as §10 (core
halted, `CPU_STATUS 0x2e`, no fault storm). `stop_after=13 core_reset=2
core_reset_only=1` - pulse and report, never start.

- `reset_control_reset()` returned 0; **the machine did not hang.** This is the
  quiescent-core case docs/41's clean run also was; docs/25 7c (hang) was not.
- `CPU_STATUS 0x2e -> 0x22`. STOPPED still set; the IRQ/FIQ-not-pending bits
  dropped. A fresh boot reads `0x2a` before start, so `0x22` is close but not
  identical. **Whether the core is really back at its reset vector is U** -
  only starting it can say.
- **All 16 DAPF entries identical before and after**, field for field, including
  m1n1's TEXT entry in slot 0 and the `0x1f0` window in slot 1. DART `CONFIG`,
  `TCR[0] 0x80`, `TTBR[0][0] 0x901eef14 VALID`, and even the stale `ERROR`
  word were unchanged too, so the DART itself was not reset. **C.**
- **The driver's own verdict ("slots 16 -> 16: SURVIVED") was not evidence.**
  Slots 3-15 hold uninitialised junk, so the count reads 16 whatever happens -
  a check that could never say "no". The entries were diffed by hand from the
  log. Fixed after this run: the comparison is now an FNV-1a fingerprint over
  every field of all 16 slots, plus an explicit "TEXT fetch admitted" check,
  and the core is not started unless both hold.

Consequence: the reset does not take the DAPF with it, so restarting after a
Halt in the same boot is **not ruled out**. The next discriminator is a full
start after the pulse: `core_reset=2 fw_restore_data=1`, handshake, Config,
Halt.

---

## 12. R2 (2026-09-14 08:39): the reset restarts the core, and the restarted firmware dies before message 1

`results/r2-1789371544.kmsg`, commit `feec90e`, same boot as §10-§11. Both
loads: `core_reset=2 fw_restore_data=1 session_selftest=1
session_config_only=1 fw_halt=1`.

- **Load 1:** fingerprint `0x335a86dea8522727` before and after the pulse, TEXT
  fetch admitted; `CPU_STATUS 0x22`; restore wrote DATA (0 bytes had drifted -
  nothing had run since H2's restore); `ASC start` **returned OK** this time
  (unlike §10, where RUN did nothing). 9 ms later, **one** DART fault:
  `status 0x80000800 stream 0 code 0x800` (NO_DAPF_MATCH) **at
  `0x39b200000`**. No message 1 in 6 s; handshake timeout. Unload: Halt
  correctly not sent (no Config).
- **Load 2:** `CPU_STATUS 0x28` before the pulse - the same value the H1 crash
  left - and the restore found **56 462 bytes drifted**. So load 1's core
  **executed** and wrote DATA. Pulse, fingerprint unchanged, restore, start:
  the identical silent failure.

**What `0x39b200000` is:** `serial@39b200000` in the live DT - a UART. The
same u64 sits statically in the firmware (image file `0xf2e38`) and in pristine
DATA at `+0x2e38` (VA `0xeee38`), as the second word of a
`{0xeb6a8, 0x39b200000, 0x1000}` record, and in the ADT. **C** that the
firmware carries it; **U** which code reads it (no direct `adr`/`adrp` to
`0xeee38` in TEXT - it is reached through a table).

**Reading (I):** a fresh boot never touches the UART (no such fault in any
earlier run), and `0x28` is the post-exception state. The likeliest story is
that the restarted firmware **took an exception early**, and with no host IPC
up its report path went to the serial console, which the DAPF does not admit.
The UART access is then a symptom; the cause is whatever differs between a
cold iBoot start and a reset-and-restore start. Candidates, none tested: state
outside DATA that iBoot writes once; ASC/CPU state the block reset does not
clear (`0x22` vs a cold `0x2a`); or DATA beyond `+0x98000`, which the pristine
blob fills with zeros because the dump it came from stopped there (docs/51 §7).

**The cheap discriminator is the firmware's own report.** It formats crash text
on a stack inside DATA (H1's `!! Exception !! ... pc ... esr` came from that
path). `tools/crash_scan.py` diffs a DATA copy against pristine and pulls
newly-written strings and nearby code pointers, validated by a negative and a
planted positive control. `test/physdump.ko full=1` now copies through the end
of DATA (the default 16 MiB window stopped `0x9c000` short - which is also why
the pristine blob's tail is assumed zero); that range has been read and written
on hardware by the restore, so it is no longer unexplored memory.

---

## 13. R3 (2026-09-14 11:04): root cause - the reset wiped scratch 0, and the firmware booted into its UART console

`results/r3-1789380356.kmsg`, commit `b771580`. Fresh boot.

**Step 1 - cold full dump before any start** (`physdump full=1`,
`data/blobs/r3-cold-full.bin`, gitignored). Cold DATA vs the pristine blob:
**7 bytes differ, all inside the STKG word at +0x3a38**; the tail
`+0x98000..0x134000` is **all zero**. So the blob is correct and the zero tail
was never a problem. **C.**

**Step 2 - restart with this boot's exact cold DATA.** Load 1 cold start,
Config, Halt (`0x08042006`, `0x2e`). Load 2 `core_reset=2 fw_restore_data=1
fw_restore_stkg=0x6e14c23b2faec5` - the blob with the cold cookie, verified
offline to be byte-identical to this boot's cold DATA. Result identical to R2:
UART DAPF miss at +16 ms, no message 1. **STKG ruled out; all of DATA ruled
out. C.**

**Step 3 - post-failure dump** (`r3-after-restart-full.bin`) through
`tools/crash_scan.py` against the cold DATA:

- TEXT unchanged (0 words). DATA: **56 462 bytes, 82 pages - the same count as
  R2**: the failure is deterministic.
- New strings: RTKit pools, `System Thread`, `Terminator`, `MMUManager`,
  `ISRManager`, `IPIManager`, `GPTimer0`, the RTKit banner. No crash text (the
  report goes straight to the UART, which faults).
- The boot stack, symbolised with `data/blobs/macos-13.5/derived/symbols.txt`:
  `__rtk_arm_start_bootstrap_area` -> `_main` -> `CPlatformEnvironment::Create`
  -> `CPlatformEnvironment::CPlatformEnvironment+0x228` (`0xa6074`) -> the device
  dispatch at `0xaff50` -> `__rtk_arch_vectors` -> `__rtk_arch_exception` ->
  `_crashlog_get_exception_type` / `_crashlog_create_callstack_section` ->
  `_RTK_abort`.

**The mechanism (C, every step a VA):**

1. `CPlatformEnvironment` ctor reads SVE scratch 0 through
   `CPlatformGPIOManager` (`0xa5f80`-`0xa5f90`, vtable `+40` = Read, index 0) and
   compares it with `w22 = 0x08042006` (`0xa5e78`/`0xa5e84`):
   `strb (scratch0 != 0x08042006), [this, #420]` at `0xa5fa0`.
2. At `0xa6050`, if `[this+420]` is set it opens `[0xeeec0]` =
   `_RTK_dev_samsunguart_0` (`0xeee30`: `{_RTK_dev_samsunguart_dispatch,
   0x39b200000, 0x1000}`) - the UART debug console.
3. `0x39b200000` is not admitted by the AVE DAPF -> the DART fault we saw ->
   exception -> crashlog -> `_RTK_abort` -> more console output -> dead.

The per-CPU RTKit boot word (`VA 0x14c358`: `0xfeed1b00` cold, `0xcafe4b0b`
warm, `0xbaadf1ac` -> `__rtk_unexpected_reset`) was restored to the cold value,
so that path was not involved.

**Why scratch 0 was wrong:** the driver writes `0x08042006 / 0 / 0xe` to scratch
0/1/2 at stage 11, and `core_reset` pulsed at the head of stage 13. The r3 load
2 stage-15 dump reads **all scratch registers 0** afterwards. The block reset
clears the SVE registers, erasing the stage-11 writes. **C** (the writes are
logged, the zeros are logged, nothing else touches them in between).

So none of this was about the core, DATA or the firmware's reset detection -
it was the driver's own stage order.

**Fix (driver):** `ave_core_reset()` now runs inside stage 7, right after Apple's
first SVE write and before every other write to the block, and re-issues that
write if it pulsed; it logs scratch 0-2 after the pulse. Stage 13 refuses to
start the core unless scratch 0 reads `0x08042006`, so this class of failure
now reports itself in one line instead of a six-second silence.
