# Teardown: how macOS stops a session and releases AVE, and why our `rmmod` resets the machine

Everything here is read from the **macOS 13.5** blobs ([43](43-macos-13.5-firmware.md)),
the build whose firmware this machine runs. Reproduce any line with:

```sh
AVE_MACOS=13.5 python3 tools/disas.py --kext --addr <VA> -n <LEN>
AVE_MACOS=13.5 python3 tools/disas.py --fw   --addr <VA> -n <LEN>
```

Kext VAs are kernelcache VAs (`0xfffffe0008…`); firmware VAs are image VAs
(`__TEXT` VA 0 = file `0x4000`). Per [00](00-methodology.md) every claim is
**[C] confirmed** (read from an instruction, VA cited), **[I] inferred** (chain
stated) or **[U] unknown**.

**Nothing in §7 has been run on hardware.** It is a proposal, and AGENTS.md
applies: the operator runs it, not an agent.

Companion documents: [55](55-halt-command.md) (the Halt command itself, and
the hardware runs H1–R4 that proved it), [46](46-abi-13.5-commands-session.md)
(the command table and header layout), [56](56-datapath-dart.md) (the DART /
SMMU block), [57](57-pipe-hang.md) (PMGR domain indices, SVE `+0x38`).

---

## 0. Summary

| Question | Answer | Conf |
|---|---|---|
| Does macOS ever tear down with a client open? | **No.** `AVE_Drv::TryPowerOff` refuses unless the client list is empty (`0xfffffe0008ef092c`–`34`); `PowerOff` queues **Stop then Close** for every live client and drains the queues before it touches power | **C** |
| The two commands we never send | **Stop** = wire id **6** `UNINIT`, size `0x40`, slot 7; **Close** = wire id **12** `STOP`, size `0x48`, slot 4 | **C** |
| What Stop does in the firmware | cancels the client's outstanding slots, then **defers its `UNINIT_DONE` reply until the client's produced/consumed counters match** — i.e. until in-flight work has drained (`0xfadc`–`0xfb10`) | **C** |
| What Close does in the firmware | `CAVEPriorityQueue::Enqueue(cid, …, 12, cmd)` (`0x107e8`) — it is **queued behind the client's pending frames**; `ProcessQueue` later runs `DestroyClient` and only then sends `STOP_DONE` `0xE0A` (`0x12a64`, `0x12ad8`) | **C** |
| Does the firmware free any buffer? | **No.** Nothing in `ProcessUninit`/`ProcessStop`/`ProcessQueue` releases host memory; the addresses are simply forgotten once the client is destroyed | **C** |
| Does macOS unmap anything at power-off? | **No.** `AVE_HwC::PowerOff` flushes the DART TLB, marks the DART **inactive**, and gates power. Surfaces stay mapped; they are released on the next `PowerOn` (`AVE_Drv::PowerOn` `0xfffffe0008eef618`) or at `IOService::stop` | **C** |
| The host's definition of "quiesced" | `AVE_HwC::CheckStopped(chm)` = `GetCmdCntInInput + InReady + InRun + InOutput == 0` over mask `0x1FFFFF` (`0xfffffe0008f1864c`–`88`) | **C** |
| The wait | `AVE_Client_Close` polls `CheckStopped` every **10 ms** for `(timeout_a + timeout_b)/10` iterations (`0xfffffe0008ecf438`–`60`) | **C** |
| What our driver gets wrong, in one line | **it drops the power reference while the coprocessor and the datapath may still be live**, and it unmaps and frees DMA memory afterwards | **C** (driver source) |
| Most likely cause of the post-unload reset | a **VENC power domain gated under an active bus master** — see §6 | **I**, ranked |
| Is the DART at risk of being gated under an unmap? | **No** on this machine: both `40d0[34]0000.iommu` sit in `venc_sys` and are runtime-**active**, so they hold `venc_sys` on for as long as they are bound | **C** (live genpd, §8) |

The single most important structural difference: **macOS never powers the block
down while anything can still issue a transaction, and it never unmaps
anything as part of powering down.** Our `ave_remove()` does both.

---

## 1. The macOS teardown, end to end

### 1.1 Entry points

| function | VA | what gates it |
|---|---|---|
| `AppleAVE2UserClient::Stop` | `0xfffffe0008e9c5c8` | userspace `IO_Stop` selector; calls `AVE_Client_Stop` (`0xfffffe0008e9c6dc`) |
| `AVE_Client_Stop(client, timeout, int)` | `0xfffffe0008ed22c4` | per-frame loop, then `AppleAVE2Driver::StopClient` (`0xfffffe0008ed24ec`) |
| `AVE_Client_Close(client, timeout)` | `0xfffffe0008ecf2e8` | the **close** path; stops first if `client[224] == 2`, then waits |
| `AVE_Drv::TryPowerOff` | `0xfffffe0008ef082c` | `drv[176] == 1` **and** `AVE_DLList_Empty(&drv->clients)` (`0xef0920`–`34`) |
| `AVE_Drv::ForcePowerOff` | `0xfffffe0008ef0438` | none — logs a transition and calls `PowerOff` |
| `AVE_Drv::PowerOff` | `0xfffffe0008eef70c` | the real sequence (§1.3) |
| `AVE_Drv::IO_stop(idx)` | `0xfffffe0008eee410` | `IOService::stop`, the analogue of our `rmmod` (§1.6) |

**`TryPowerOff` is the normal path and it cannot run with a client open.**
`ForcePowerOff` exists for the abnormal one, and it still goes through
`PowerOff`, which itself sends Stop+Close for every live client. So there is
no macOS path that reaches the hardware teardown with an un-stopped client.
**[C]**

### 1.2 Stopping and closing one client

`AppleAVE2Driver::StopClient(client, timeout, int)` (`0xfffffe0008e955cc`) is a
thin `IOCommandGate::runAction` wrapper (`0xfffffe0008e95748`) around
`AVE_Drv::StopClient` (`0xfffffe0008ef2758`). The gated body, in order — **[C]**:

| VA | what |
|---|---|
| `ef28c4`–`d4` | `KernelFrameQueue::getRequestedSpot(frame)` — the frame slot being stopped |
| `ef2b88` | `client_slot[…] = -1` |
| `ef2b94` | `AVE_Drv::MarkFrameNonDrop(client)` (`0xfffffe0008eefc0c`) |
| `ef2bc4` | **`AVE_Client_DropGroupCmds(client)`** (`0xfffffe0008ed54a0`) |
| `ef2bd4` | **`AVE_Client_DropAllCmds(client, 0x188d01)`** (`0xfffffe0008ed52b8`) — drop the queued host commands whose kinds are in that mask |
| `ef2bf4`–`c1c` | for every CHM of the client (`client[304]` of them, stride `0x33c38`): **`AVE_HwC::StopCHM(hwc, chm)`** (`0xfffffe0008f18010`) |
| `ef2c38`–`40` | require `chm[56] == 1`, else return **-1026** |
| `ef2c60` | *(debug-gated on `client[0xD059A]`)* `AVE_Client_AppendCmd(client, 15, …)` — host enum 15 is out of `AVE_Cmd2FwCmd`'s range (`0xfffffe0008ed6c70` `cmp #0xa`), so **nothing goes on the wire**; a host-side marker |
| `ef2c78` | **`AVE_Client_AppendCmd(client, 7 /* Stop */, 0, timeout, NULL)`** (`0xfffffe0008ed4348`) |
| `ef2c90` | **`AVE_Client_AppendCmd(client, 4 /* Close */, 0, timeout, frameinfo)`** |
| `ef2c98` | `chm[56] = 2` |
| `ef2cac` | `AVE_Timer_Start(timer, 1)` (`0xfffffe0008f42e94`) |

Host enum → wire id comes from `AVE_Cmd2FwCmd` (`0xfffffe0008ed6c68`, jump table
at `0xfffffe0008ed6d30` relative to `0xed6c8c`) — **[C]**:

| host enum | wire id | name |
|---:|---:|---|
| 1 | 1 | Config |
| 2 | **14** | Halt / `POWERDOWN` |
| 3 | 2 | Open / `START` |
| **4** | **12** | **Close / `STOP`** |
| 6 | 4 / 5 | Start AVC / HEVC |
| **7** | **6** | **Stop / `UNINIT`** |
| 8 | 7 / 8 / 9 | Process |
| 9 | 13 | Complete |
| 10 | 11 | Flush |
| 11 | 3 | Reset |

So macOS's teardown of a client is exactly **Stop (wire 6) then Close (wire 12),
in that order, on the IO ring**, queued behind whatever the client already had
outstanding.

### 1.3 `AVE_Drv::PowerOff` (`0xfffffe0008eef70c`)

| step | VA | what |
|---:|---|---|
| 1 | `eef810` | per client: `AVE_Client_DropGroupCmds` |
| 2 | `eef838` | per HwC: `Prepare_First` → `StopFwHeartBeatTimer` (`0xfffffe0008f1129c`) |
| 3 | `eef85c`–`8b8` | **drain loop**: `AVE_Drv::Drain_First` + per-HwC `AVE_HwC::Drain_First` (count commands still in input/ready), `IODelay(10)`, bounded |
| 4 | `eef924` | per client: `AVE_Drv::MarkFrameNonDrop` |
| 5 | `eef938`/`950` | `Finish_First` → `AVE_CHMList_SkipAllCmds_First` |
| 6 | `eef96c` | `AVE_ClientList_SetPowerState(list, false)` |
| 7 | `eef984` | per HwC: **`AVE_HwC::Prepare_Second`** — for each CHM with `(state | 2) == 3` (`0xfffffe0008f16dc0`–`c8`): `AVE_CHM_AppendCmd(chm, 7 /*Stop*/, …)` (`f16dd4`) then `AVE_CHM_AppendCmd(chm, 4 /*Close*/, …)` (`f16df0`), then set the CHM state |
| 8 | `eef9cc` | **drain loop**: per-HwC `Drain_Second` until empty or timeout |
| 9 | `eefad0` | per HwC: **`AVE_HwC::ShutDown()`** — the Halt (§1.4) |
| 10 | `eefaf8` | per HwC: `Finish_Second` |
| 11 | `eefb20` | per HwC: **`AVE_HwC::PowerOff()`** — the DART and power work (§1.5) |

Steps 7 and 8 are the ones we have no equivalent of at all. **[C]**

### 1.4 `AVE_HwC::ShutDown` / `ShutDownIOP`

`AVE_HwC::ShutDown` (`0xfffffe0008f14c00`): require `m_state == 3` (`f14cdc`);
`StopFwHeartBeatTimer` (`f14cec`); `ShutDownIOP()` (`f14cf4`); `SetMCC(-1)`
(`f14de4`); `[this+240] = 0`; `m_state = 2` (`f14df0`). **[C]**

`AVE_HwC::ShutDownIOP` (`0xfffffe0008f14438`) — re-read for this document:

| VA | what |
|---|---|
| `f14534` | `AVE_PMGR::SetPS(pd = 3, ps = 2, wait = 1)` (`0xfffffe0008f2ca14`) |
| `f14548` | `SetPS(pd = 4, ps = 2, wait = 1)` |
| `f1455c` | `SetPS(pd = 5, ps = 2, wait = 1)` |
| `f14570` | `SetPS(pd = 6, ps = 2, wait = 1)` |
| `f1457c` | **`AVE_PMGR::SetClockGating(false)`** (`0xfffffe0008f2cc84`, `w1 = 0`) → `AVE_SVECtrl::SetIdle(0)` → SVE **`+0x38 = 0`** ([57](57-pipe-hang.md) §3.2) |
| `f14584` | **`SendFwCmd_Halt()`** (`0xfffffe0008efc600`) — see [55](55-halt-command.md) |
| `f14614` | `AVE_IOP::Stop()` — poll `CPU_STATUS` (ASC `+0x400048`) until `(v & 3) != 0` three times, 50 µs apart |
| `f14618`–`744` | poll `AVE_SVECtrl::ReadScratch(0)` for `0x08042006`, `IODelay(100)` per iteration |
| `f14744` | `IODelay(100)`, return 0 |

PD indices on 13.5: 0 IOP, 1 IOP_Mid, 2 DCS, 3 Pipe4, 4 Pipe5, 5 ME0, 6 ME1,
7 DMA, 8 IOP_Max, 9 IOP_Mid2, 10 FAB ([10](10-power.md) name table
`0xfffffe0007bc3ca8`). So **3–6 = venc_pipe4, venc_pipe5, venc_me0, venc_me1**:
the four domains the AVE datapath lives in are driven *up* to ClockOn and the
clock gate is *released* immediately before the Halt is sent, so the firmware's
`ProcessChipReset` (§2.3) runs against a fully clocked block. **[C]** for the
calls; **[I]** for the purpose.

### 1.5 `AVE_HwC::PowerOff` (`0xfffffe0008f15794`)

Re-read for this document — **[C]**:

| VA | what |
|---|---|
| `f15874`–`80` | `m_state == 1` → already off, return 0 (`f1587c` `b.eq f158fc`); `m_state == 0` → return **-1011** (`f15880` falls through to the log path, `f15964`) |
| `f1590c` | `AVE_SVECtrl::GetIntr(&v)` (`0xfffffe0008f417d0`) |
| `f15918` | **`AVE_SVECtrl::ClearIntr(v)`** (`0xfffffe0008f4180c`) — the pending SVE interrupt is read and acknowledged *before* anything is gated |
| `f1592c` | **`AVE_SurfaceMgr::DARTFlushAll(mask = 0x10000 << hwc[64])`** (`0xfffffe0008f408f0`) → per surface class `AVE_DART::Flush()` (`0xfffffe0008edd030`) = `callPlatformFunction("cacheFlushInactive", …)` on the DART service (`0xedd14c` string `0xfffffe00071f4739`), gated on the DART still being **active** (`ldr w8,[x19,#1596]; cmp #1` `0xedd11c`) |
| `f1593c` | `AVE_SurfaceMgr::DisableOp(mask)` (`0xfffffe0008f409dc`) — asserts the surface list is empty, bookkeeping |
| `f15948` | **`AVE_DART::SetActive(false)`** (`0xfffffe0008edcc00`) = `callPlatformFunction("setActive", false, 0, …)` (`0xedcd30` string `0xfffffe00071f41fb`), vtable `+0x6f0` on the DART `IOService` |
| `f15950` | **`AVE_DPM_PowerOff(&hwc[0x108])`** (`0xfffffe0008eea1e0`) |
| `f1595c` | `m_state = 1` |

`AVE_DPM_PowerOff`, exact — **[C]**:

```
eea2a0  AVE_DPM_Stop(dpm)                    ; bookkeeping only, dpm[24] = 0
eea2b4  SetPS(pd 3  Pipe4, ps 0 PowerOff, wait)
eea2c8  SetPS(pd 4  Pipe5, ps 0 PowerOff, wait)
eea2d4  SetClockGating(true)                 ; SVE +0x38 = 1
eea2e8  SetPS(pd 0  IOP,   ps 0 PowerOff, wait)
eea2fc  SetPS(pd 2  DCS,   ps 0 PowerOff, wait)
eea310  SetPS(pd 10 FAB,   ps 0 PowerOff, wait)
```

ME0 (5) and ME1 (6) are not gated explicitly; `AVE_PMGR`'s dependency graph
pulls them down with their parents ([10](10-power.md)). **[I]**

Note the shape: **flush the IOMMU, deactivate the IOMMU, gate the datapath
domains, gate the clock, gate the IOP, and only then the fabric domain.** Every
`SetPS` has `wait = 1`.

### 1.6 `AVE_Drv::IO_stop` — the analogue of `rmmod`

`AVE_Drv::IO_stop(u32 idx)` (`0xfffffe0008eee410`), per HwC index — **[C]**:

```
eee4f0  if (idx >= 8) -> log + bail
eee508  AVE_SurfaceMgr::DARTUnmapSurface(mgr, drv[88], idx)   ; 0xfffffe0008f4062c
eee518  AVE_HwC::Uninit()                                     ; 0xfffffe0008f10998
eee544  operator delete(hwc)                                  ; virtual +40
eee5d4  drv[180]--
```

and `AVE_HwC::Uninit` itself does `AVE_DPM_Uninit` (`f10b18`),
`DARTUnmapSurface` (`f10b38`), `DestroySurface` (`f10b48`), `AVE_IPC::Uninit`
(`f10b5c`), timer teardown, `AVE_IOP::Uninit` (`f10b94`).

**Every unmap happens here, after `PowerOff` has completed.** There is no unmap
anywhere in `AVE_HwC::PowerOff` or `AVE_DPM_PowerOff`. **[C]**

---

## 2. The wire

### 2.1 Stop — wire id 6 `CAVE_CMD_UNINIT`, size `0x40`

Builder `AVE_CHM_MakeFwCmd_Stop(chm, u64 cnt, u32, _S_AVE_TimeOut*, sCAveCmdUninit*)`
at **`0xfffffe0008eaa5e4`**. The stores, all **[C]**:

```
eaa6dc  ldr x26, [x19, #16]        ; x26 = chm->client
eaa6ec  memset(cmd, 0, 0x40)       ; w2 = #0x40 at eaa6e8
eaa6f0  strh wzr, [x23, #2]        ; +0x02 = 0
eaa6f4  str  x20, [x23, #8]        ; +0x08 = CNT (u64)
eaa6f8  ldr  w8, [x26, #80]        ; client id
eaa6fc  ldr  w9, [x26, #212]
eaa700  stp  w8, w9, [x23, #16]    ; +0x10 = client id, +0x14 = client[212]
eaa704  ldr  w8, [x26, #220]       ; codec
eaa708  str  w8, [x23, #24]        ; +0x18 = codec (0 = AVC)
eaa710  ldr  d0, [x8, #552]        ; literal 0xfffffe000722f228 = {7, 200}
eaa714  stur d0, [x23, #28]        ; +0x1C = slot 7, +0x20 = priority 200
eaa718  ldr  q0, [x21]             ; _S_AVE_TimeOut
eaa71c  stur q0, [x23, #40]        ; +0x28..+0x37
eaa720  mov  w8, #0x6
eaa724  strh w8, [x23]             ; +0x00 = 6
```

Byte table for one plain AVC client:

| off | sz | value | source |
|---|---|---|---|
| `+0x00` | u16 | **6** | `eaa724` |
| `+0x02` | u16 | 0 | `eaa6f0` (firmware rewrites it, fw `0xd69c`) |
| `+0x08` | u64 | command count | `eaa6f4` |
| `+0x10` | u32 | **client id** | `eaa700` |
| `+0x14` | u32 | `client[212]` — no firmware reader found ([46](46-abi-13.5-commands-session.md) §2) | `eaa700` |
| `+0x18` | u32 | **codec**, 0 = AVC | `eaa708` |
| `+0x1C` | u32 | **slot 7** | literal `0xfffffe000722f228` |
| `+0x20` | u32 | priority **200** | same literal |
| `+0x28` | 16 | `_S_AVE_TimeOut` | `eaa71c` |

Firmware size assert: `cmp w22, #0x40` fw `0xda0c` → `_bsp_assert_fail` on
`"sizeof(struct sCAveCmdUninit)"` (`0xbcc2e`) and `b .`. Reply:
**`UNINIT_DONE` = `0xE05`**, 0x40 bytes, status `0xEE0000` on success. **[C]**

### 2.2 Close — wire id 12 `CAVE_CMD_STOP`, size `0x48`

Builder `AVE_CHM_MakeFwCmd_Close(chm, u64, u32, _S_AVE_TimeOut*, sCAveCmdStop*)`
at **`0xfffffe0008ea942c`**. Identical to Stop except:

```
ea9530  memset(cmd, 0, 0x48)                 ; w2 = #0x48
ea9558  ldr  d0, [x8, #536]                  ; literal 0xfffffe000722f218 = {4, 200}
ea955c  stur d0, [x23, #28]                  ; +0x1C = slot 4, +0x20 = prio 200
ea9568  mov  w8, #0xc
ea956c  strh w8, [x23]                       ; +0x00 = 12
ea9570  mov  w8, #0xc95 ; movk #0xe, lsl#16  ; 0xE0C95
ea9578  ldrb w8, [x26, x8]                   ; client[0xE0C95]
ea957c  strb w8, [x23, #64]                  ; +0x40 = that byte
```

`client[0xE0C95]` is the client's **LRME / low-resolution flag**: the only other
readers select `AVE_Client_CalcSurfaceInfo_LRME` over the normal variant
(`AVE_Client_Prepare` `0xfffffe0008ed00e4`–`104`, `AVE_Client_Start`
`0xfffffe0008ed166c`). For a plain AVC session it is **0**. **[C]** for the
provenance, **[U]** for whether the firmware reads `+0x40` at all — nothing in
`ProcessStop` (`0x10690`) touches it.

Firmware size assert: `cmp w22, #0x48` fw `0xdce8` → `"sizeof(struct
sCAveCmdStop)"` (`0xbcc7d`). Reply: **`STOP_DONE` = `0xE0A`**, 0x40 bytes,
status `0xEE0000`. **[C]**

### 2.3 What the firmware does with each

**`ProcessUninit` (Stop, fw `0xf480`)** — **[C]**:

1. `x20 = this[1440]` = the command; `CAVEPriorityQueue::GetClientIndexFromID(cmd+0x10)`
   (`0xf510` → `0x175b4`). `-1` → error branch `0xf664`; `>= 0x80` → assert.
2. `SetClientPriority(cid, cmd+0x20)` (`0xf53c` → `0x17f14`).
3. For each of `client[196]` entries: `CAVEPriorityQueue::Complete(cid, base+i, 0)`
   (`0xf588` → `0x18a40`) — **cancel the client's outstanding queue slots**.
4. If `client[480] == 1`: `CFlowControllerBase::FlushQueueForClient(cid, cmd+8)`
   (`0xf5cc` → `0x13148`). Otherwise a long scan (`0xf6b4`–`0xf938`) over a
   0x40-stride slot table, matching `cid` and the command count, cancelling
   matches.
5. **The reply is conditional** (`0xfadc`–`0xfb10`):
   ```
   fadc  w21 = client[520]        ; produced
   fae0  w8  = client[524]        ; consumed
   fae4  if (w21 != w8)  -> defer
   faec  ldp w10, w9, [client, #180]
   faf0  if (w9 != w10) -> defer
   fafc  NotificationToHost(0xE05 UNINIT_DONE, cid, 0xEE0000, sp+0x18)
   ```
   and the *defer* path (`0xfb14`–`0xfb54`) copies the pending 24-byte
   notification into a ring at `client+0x208` (8 entries, `w21 & 7`), bumps
   `client[520]`, and returns without replying; if the backlog reaches 8 it
   logs and drops.

   **So `UNINIT_DONE` arriving is the firmware's statement that the client has
   no work left in flight.** That is the signal our driver needs and does not
   currently have.

**`ProcessStop` (Close, fw `0x10690`)** — **[C]**:

1. `IsClientRegistered(cid)` (`0x10704` → `0x1785c`); not registered →
   `NotificationToHost(0xE0A, cid, **0xEE0002**, cmd+0x1C)` (`0x10878`) and
   return 1.
2. `GetClientIndexFromID`, fetch the client object from `this[31816 + idx*8]`;
   NULL or index > 0x7F → `UnregisterClient(cid)` (`0x1088c`) and reply
   `0xE0A` / `0xEE0000` immediately (`0x108a4`).
3. `AVE_History_Add(…, 0x80, 3234, …)`, `SetClientPriority`, then the same
   `Complete()` cancel loop as Stop (`0x107bc`).
4. **`CAVEPriorityQueue::Enqueue(queue, cid, this[1344], cmd+8, /*kind*/ 12,
   cmd, NULL)`** (`0x107e8` → `0x180e4`). It must return true, else
   `_bsp_assert_fail` + `b .` (`0x108cc`). **No reply is sent here.**
5. The reply comes out of **`CFlowControllerBase::ProcessQueue` (`0x10d94`)**
   when the queued item is finally dequeued (`0x12a1c`
   `CAVEPriorityQueue::Dequeue`): `CFlowControllerBase::DestroyClient(cid)`
   (`0x12a64` → `0x13a08`), clear `this[3264]` and the per-codec current-client
   word if they name this client (`0x12a78`, `0x12a90`), `GetNumClientsPerCodec`
   (`0x12aa0`), a virtual call on the McpuController `[this+31248]` vtable `+216`
   (`0x12ac0`), then **`NotificationToHost(0xE0A STOP_DONE, cid, 0xEE0000, …)`**
   (`0x12ad8`).

   `DestroyClient` (`0x13a08`) is pure bookkeeping: it compacts the client
   pointer array and calls `0x17d34` on the queue. **It frees no host memory
   and unmaps nothing.** **[C]**

**`ProcessPowerDown` (Halt, fw `0x10ca8`)** — unchanged from
[55](55-halt-command.md) §4, repeated here because it is the datapath quiesce:
`CAVEPipeISRManager::MaskAll` (`0x10cfc`), `CAVEPriorityQueue::FlushAll`
(`0x10d40`), a vtable call on the McpuController (`0x10d50`),
**`CAVECommonController::ProcessChipReset(true)`** (`0x10d58` → `0x2328c`),
`str wzr, [regbase + 0x1050000]` (`0x10d74`), then
`CPlatformEnvironment::Shutdown` → scratch 0 = `0x08042006` → `wfi` forever.

`ProcessChipReset` (`0x2328c`) is a pulse sequence on the AVE datapath register
`base + 0x1050000` — set-then-clear of bits `0x800`, `0x100`, `0x200`, `0x400`,
`0x2000`, then `0x80`, `0x8`, `0x10` (each written three times then cleared),
then `0x4` — followed by **setting and leaving set** bits `0x1 | 0x1000 |
0x2000 | 0x4000 | 0x8000` in `base + 0x111013C` (`0x233c4`–`0x23404`). **[C]**
for the instructions; the meaning of individual bits is **[U]**, but the shape
is a block-level soft reset followed by a latch of five disable/idle bits.

For comparison, `CAVECommonController::Reset(0)` (`0x2316c`) does
`ProcessChipReset(false)` **plus** the virtual `ProcessPipeReset()`
(vtable `+472`, `CAVCController::ProcessPipeReset` `0x4ed60`) **plus**
`ProcessTranscodeReset()` (vtable `+480`, `0x4f404`). `ProcessPowerDown` calls
only `ProcessChipReset`. **[C]**

**Consequence.** A successful Halt is itself the datapath quiesce: the pipe
interrupts are masked, the queues are flushed and the AVE block is soft-reset
*by the firmware, from inside the powered block*, before the core parks. That
is why F4's Halt "succeeded" (`0x08042006`, `CPU_STATUS 0x2e`) — but F4's pipe
was already hung with 100 059 SMMU faults outstanding, and a soft reset of a
block whose AXI master is stuck mid-transaction is not the same as a quiesced
one. **[I]**

---

## 3. Who owns the buffers after a completed frame

The question is whether `ENCODE_DONE` (`0x0E06`, status `0xEE0000`) means the
hardware is finished with everything we handed it. It does not.

| region | published by | firmware still holds it after ENCODE_DONE? | evidence |
|---|---|---|---|
| **FwIPC shared region** (`Config` `shmem_addr`/`shmem_size`) | Config, once per boot | **Yes, for the life of the firmware.** `ProcessConfig` hands it to `PlatformIOPIPCManager::AddSharedMemory` and carves it into four; the rings live there permanently | **C** ([46](46-abi-13.5-commands-session.md) §3, docs/36) |
| **Firmware log ring** (`log_addr`/`log_size` in the boot config) | the boot handshake, `driver/ave_ipc.c:606` | **Yes, and it is written continuously while the core runs** — the heartbeat task emits lines with no host command in flight. F4's capture shows `fw[0]| Controller Heart Beat ERROR: PIPE HANG` arriving after the last command | **C** (log + `ave_boot_fwlog_alloc`) |
| **recon / DPB, colocated, entropy, SrcNeighbor tables** | `Start_AVC` (id 4), stored in the client object | **Yes, until the client is destroyed.** They are per-session tables, re-read by every `Process`; nothing in `ProcessAvcEncode` releases them | **I**, strong: the whole point of publishing them at Start rather than per-frame |
| **coded / coded-header / source planes** | each `Process` (id 7) | Finished *for that frame* when `ENCODE_DONE` arrives — but only if the frame really completed. `ProcessUninit`'s deferred-reply logic exists precisely because the counters can disagree | **I** |
| **iBoot TEXT/DATA mapping** | the boot path, IOVA from the `fw-base` register | **Yes, always** — it is the core's instruction and data memory. Unmapping it while the core executes is unmapping the code under it | **C** |

So the honest answer to "does anything still own the buffers after a completed
frame" is: **the firmware owns the FwIPC region, the log ring, the iBoot
mapping and the whole Start-time table set until the client is closed and the
core is halted.** A completed `Process` releases nothing but that frame's
coded output.

**The quiesce the host must perform**, in macOS's own terms:

1. **Stop (id 6)** — cancels the client's outstanding queue slots and replies
   only when produced == consumed. This is the "wait for in-flight work" step.
2. **Close (id 12)** — queued behind the remaining work; when it runs,
   `DestroyClient` drops the firmware's last reference to the client's tables,
   and `STOP_DONE` says so.
3. **`CheckStopped` == 0** on the host side — no commands in input, ready, run
   or output.
4. **Halt (id 14)** — masks the pipe interrupts, flushes the queues, soft-resets
   the AVE datapath (`ProcessChipReset`) and parks the core in `wfi`. After
   this the coprocessor cannot issue a transaction at all.
5. **DART TLB flush + DART inactive** — `AVE_HwC::PowerOff`.
6. **Only then** gate the power domains.

There is no separate "DMA disable" register the host writes: steps 1–4 are the
disable, and step 4 is the one that is architecturally final.

`ProcessPipeReset` is **not** part of the teardown. It is reached only from
`CAVECommonController::Reset(0)` (`0x2316c`) and
`CAVECommonController::CmdProcessor` (`0x23fc0`) — i.e. the `RESET` command
(wire id 3) and the per-frame error recovery, not from `ProcessPowerDown`.
**[C]** (the only two `bl 0x2328c` sites outside `ProcessConfig` and
`ProcessPowerDown`, and the only two loads of vtable `+472`.)

---

## 4. What our driver does today, and what is wrong with it

`ave_remove()` (`driver/ave_drv.c:1393`), in order:

| # | call | problem |
|---:|---|---|
| 1 | `ave_session_halt(ave)` — **only if `fw_halt=1`** | default off. F16 ran without it (`results/f16-1789855975.kmsg` header), so the core was **never stopped** |
| 2 | `ave_stop(ave)` → `ave_asc_stop()` | sets `ave->running = false`. That is all it does (`driver/ave_drv.c:204`) |
| 3 | read `CPU_STATUS`; if not `STOPPED`, `ave->session_bufs = NULL` (leak) | protects the *session* buffers only. The FwIPC region, the log ring and the iBoot mapping are freed regardless |
| 4 | `ave_power_off(ave, "remove")` → `ave_smmu_quiesce`, `ave_power_me1_off`, `disable_irq`, `CPU_CONTROL = 0`, `pm_runtime_put_sync` | **this is the gate-under-a-live-master step** |
| 5 | `ave_session_release(ave)` → `dma_free_coherent` × N | after power-off |
| 6 | `ave_fw_unload(ave)` → `iommu_unmap` of iBoot TEXT/DATA, `dma_free_attrs` of our image | after power-off; this is the `iboot: DATA unmapped` line |
| 7 | `ave_ipc_fini(ave)` → `dma_free_coherent` of the FwIPC region, the log ring and the fw heap | after power-off |

Then devres unwinds **after `ave_remove()` returns**: `devm_free_irq` for the
AVE line and (if `smmu_watch=1`) the SMMU line, `devm_pm_runtime_disable`,
`devm_pm_domain_detach_list`, `devm_iounmap`.

Measured against §1, the defects are:

1. **No Stop, no Close.** The client opened with `Open` (id 2) is abandoned.
   The firmware still has it registered in `CAVEPriorityQueue`, still holds its
   Start-time tables, and still has whatever the priority queue holds for it.
   macOS has no code path that does this. **[C]**
2. **No wait for in-flight work.** Our only synchronisation is the per-command
   reply wait in `ave_session_cmd` plus `synchronize_irq`. There is no
   equivalent of `CheckStopped`, and the firmware's own "produced == consumed"
   gate (`0xfadc`) is never consulted because we never send the command that
   evaluates it. **[C]**
3. **Halt is optional and off by default.** With `fw_halt=0` the core is
   running — heartbeat task, log ring writes, IPC ring polling — when step 4
   gates its power domains. **[C]**
4. **We gate power before we unmap, then unmap and free after.** macOS does
   the opposite order and, at `PowerOff`, does not unmap at all (§1.5, §1.6).
   **[C]**
5. **The SVE interrupt is never read and acknowledged.** `ave_power_off` calls
   `disable_irq()`; macOS calls `GetIntr` + **`ClearIntr`** (`0xfffffe0008f1590c`,
   `f15918`) before gating. A level-held source into a block that is about to
   lose power. The driver's own comment already records the symptom: the shared
   DART line stays asserted for ~9 s until the kernel disables IRQ 129.
   **[C]**
6. **No DART flush, no DART "inactive".** macOS does
   `DARTFlushAll` → `cacheFlushInactive` and then `setActive(false)`
   (§1.5). We do neither. There is no Linux API for the second; the first has
   an approximation (see §8). **[C]** that macOS does it; **[U]** what it costs
   us.
7. **`SetClockGating(false)` is not done before Halt.** macOS ungates the AVE
   clock (SVE `+0x38 = 0`) and raises PD 3–6 to ClockOn immediately before
   `SendFwCmd_Halt` (`0xfffffe0008f1457c`). Our `ave_session_halt` sends Halt
   with SVE `+0x38` back at 1 (the session path sets it to 1 after Process,
   `driver/ave_session.c:1933`). The firmware's `ProcessChipReset` then runs
   against a clock-gated block. **[C]** for both sides; the consequence is
   **[U]**, and it is a plausible reason a Halt can report success while
   leaving the datapath in a bad state — which is exactly F4's shape.
8. **`venc_me1` is released first, at the top of `ave_power_off`,** before the
   IRQ is quiesced and before the core is known to be stopped. macOS gates ME1
   last, inside `AVE_DPM_PowerOff`, after `SetClockGating(true)`. Minor, but it
   is the deepest leaf of the datapath and it goes away while the datapath may
   still be running. **[C]**

---

## 5. The two incidents, side by side

| | **F4** (`results/f4-1789387926.kmsg`) | **F16** (`results/f16-1789855975.kmsg`) |
|---|---|---|
| params | `… core_reset=2 fw_restore_data=1 session_selftest=1 **fw_halt=1** smmu_watch=1 session_frame=1` | `… dapf_dump=1 smmu_watch=1 session_selftest=1 session_frame=1 session_lsb=1 power_me1=1 dpe_tunables=1 session_coloc=1 session_entropy_size=1` — **no `fw_halt`** |
| encode | failed: `Controller Heart Beat ERROR: PIPE HANG: 1, 1`, **100 059** SMMU fault interrupts | succeeded: `0x0E06` / `0xEE0000`, 2709 bytes, every stage counter at 3845 |
| Halt | sent; `scratch 0 = 0x08042006`, `CPU_STATUS 0x2e STOPPED` | **not sent** |
| session buffers | freed (core STOPPED) | **leaked** (core not STOPPED) |
| last lines | `powered off (remove)` → `iboot: DATA unmapped` → `rc=0` | same shape (reported; the capture ends at the load) |
| outcome | machine reset seconds later | machine reset ~2 s later |

The two runs share almost nothing except the last five lines of
`ave_remove()`. The common factor is not the fault storm and not the Halt: it
is **`ave_power_off()` followed by unmapping and freeing DMA memory**, with the
AVE block in an unknown state in one case and with the coprocessor *provably
still running* in the other.

F16 is the more informative of the two, because it removes every
"something-was-already-broken" explanation:

* the frame completed with status `0xEE0000` and zero DART/SMMU/AXI faults
  ([53](53-first-frame.md) §28);
* the core was healthy and, with no Halt, **still executing** — the heartbeat
  task alone keeps it writing the log ring;
* `ave_power_off()` then dropped the runtime-PM reference, which releases
  `venc_me1`, `venc_me0`, `venc_pipe4`, `venc_pipe5` and `venc_dma`;
* `ave_fw_unload()` then **unmapped the iBoot TEXT/DATA mapping the running
  core fetches from**, and `ave_ipc_fini()` freed the FwIPC region and the log
  ring it was writing to.

---

## 6. What resets the machine ~2 s after the last register access — ranked

No fault is reported in either case, which is itself the strongest clue: on
Apple silicon a transaction to a gated or unresponsive block **does not fault,
it stalls the fabric**, and the SoC's watchdog resets the machine a short time
later ([00](00-methodology.md) Trap 6, [24](24-incident-2026-09-07.md)). A
~2 s delay between the last successful access and the reset is the signature of
a watchdog, not of a synchronous abort.

### Rank 1 — a VENC power domain gated while the block still has an active master. **[I], high confidence**

Evidence for:

* It is the one thing macOS structurally refuses to do. Every `SetPS(… , 0)` in
  `AVE_DPM_PowerOff` happens after Halt has been acknowledged twice (scratch 0
  **and** `CPU_STATUS`), and `AVE_HwC::ShutDown` will not even start unless
  `m_state == 3`.
* F16 gated with the core **running** (no `fw_halt`), so there was certainly an
  active master: the RTKit heartbeat task, the IPC ring poller and the log
  writer.
* F4 gated after a Halt that reported success, but the AVE datapath had
  100 059 outstanding SMMU faults and a reported `PIPE HANG` — a stuck AXI
  master that `ProcessChipReset` pulsing `base+0x1050000` may not have
  recovered, especially with the clock gated (§4 defect 7).
* Both resets came seconds *after* `rmmod` returned 0. `pm_runtime_put_sync()`
  suspends `ave->dev`, but the five `genpd:N:` virtual devices behind
  `devm_pm_domain_attach_list()` are *suppliers* reached through device links,
  and supplier runtime-PM puts are asynchronous; genpd can also defer the
  actual gate to `genpd_power_off_work_fn`. So the domains plausibly gate after
  `ave_remove()` has returned and after devres has freed the interrupt
  handlers. **[I]** — this is Linux-side reasoning, not read from a
  disassembly, and it is worth confirming with a `pm_genpd_summary` read
  immediately after a (surviving) unload.
* It explains the absence of any log line: the CPU that would print it is
  waiting on the fabric.

Evidence against: none found. The alternative that the *core* itself is the
stuck master in F4 is excluded by `CPU_STATUS 0x2e` (STOPPED) — but the
datapath is a separate master from the ASC.

### Rank 2 — DMA landing on memory we already freed or unmapped. **[I], plausible, and certainly a bug regardless**

In F16 `ave_fw_unload()` unmapped the iBoot TEXT/DATA range out from under a
running core, and `ave_ipc_fini()` freed the log ring it writes. The *first*
such access should produce a DART translation fault, which the driver logs and
which did not appear — but the fault handler lives on a line whose device is in
`venc_sys`, and by then the AVE's own path to that DART runs through the
already-gated `venc_dma`. A fault that cannot be delivered and a transaction
that cannot complete look identical from the outside. This is really rank 1
with a different first domino; it is listed separately because the fix is
different (order, not synchronisation).

### Rank 3 — the shared interrupt line left asserted with no one to ack it. **[I], contributory at most**

macOS reads and clears the SVE interrupt (`GetIntr`/`ClearIntr`,
`0xfffffe0008f1590c`/`f15918`) before gating; we only `disable_irq()`, and the
SMMU/DART handlers are freed by devres *after* `ave_remove()` returns. The
observed consequence of this in earlier runs was a 9 s "nobody cared" storm and
a disabled IRQ 129 — annoying, not fatal. It becomes dangerous only once the
handler is reading a **gated** block, which is rank 1 again.

### Rank 4 — DART teardown while a channel is still enabled. **[I], unlikely on this machine**

Both `40d030000.iommu` and `40d040000.iommu` are in `venc_sys` and are
runtime-**active** (§8), so they hold `venc_sys` on and cannot be gated by our
`pm_runtime_put_sync`. We never write DART registers directly. The `setActive`
/ `cacheFlushInactive` platform functions macOS uses have no Linux caller, so
we simply skip a flush — stale TLB entries pointing at freed pages are a
correctness problem for the *next* load, not a reset mechanism for this one.

### Rank 5 — an `AVE_DPE` disable we skip. **[U] → folded into rank 1**

There is no host-side DPE disable in the 13.5 kext teardown path: the only
datapath reset in the teardown is the firmware's own `ProcessChipReset` inside
`ProcessPowerDown`. So "we skip a DPE disable" is really "we skip the Halt that
performs it" (F16) or "the Halt performed it against a clock-gated, wedged
block" (F4).

### Rank 6 — the PMGR ordering itself. **[I], low**

macOS gates Pipe4, Pipe5, then clock, then IOP, DCS, FAB, each with `wait = 1`.
genpd gates children before parents (`venc_me1` → `venc_me0` → `venc_pipe4/5` →
`venc_dma` → `venc_sys`), which is a superset of macOS's ordering constraints
and not obviously wrong. It would only matter if some domain must be gated
*before* its child, which nothing in the kext suggests.

---

## 7. The minimal correct sequence for our driver

This is a **proposal**. AGENTS.md: the operator runs it.

### 7.1 Order

Everything below happens inside `ave_remove()`, before any `dma_free_*`,
`iommu_unmap` or `pm_runtime_put_sync`.

```
 0. stop accepting new work; install ave_session_ipc_rx (see below)
 1. for each open client (we have one, AVE_SESS_CLIENT_ID):
      send Stop  (wire id 6,  0x40 bytes) -> wait for UNINIT_DONE 0xE05
      send Close (wire id 12, 0x48 bytes) -> wait for STOP_DONE   0xE0A
 2. SVE +0x38 = 0                       (macOS SetClockGating(false), f1457c)
 3. scratch[0] = 0, read back, abort if non-zero
 4. send Halt (wire id 14, 0x40 bytes) on IO; do NOT wait for a reply
 5. poll scratch[0] == 0x08042006
    poll CPU_STATUS & (RUNNING|STOPPED) != 0, three consecutive samples
 6. restore ave->ipc_rx; synchronize_irq()
 7. read SVE INTR_STATUS, write it back (w1c), i.e. ack whatever is pending
 8. unmap and free EVERYTHING, while the block is still powered:
      session buffers, FwIPC region, log ring, fw heap, iBoot TEXT/DATA
 9. SVE +0x38 = 1                       (macOS SetClockGating(true), eea2d4)
10. release venc_me1, then pm_runtime_put_sync()  -> genpd gates the domains
```

Steps 8 and 10 are the swap that matters: **unmap while powered, gate last.**
The DARTs are in `venc_sys` and hold it on themselves (§8), so an unmap at
step 8 is safe; an unmap at today's position is an unmap into a block whose
datapath domains are already gone.

### 7.2 The commands, concretely

Both already exist in `driver/ave_abi.h`'s 13.5 table, with the right ids,
sizes, slots and reply ids:

```c
[AVE_OP_STOP]  = { 6,  0xE05, 0x40, /*slot*/ 7, 200, /*reply*/ 0x40 }
[AVE_OP_CLOSE] = { 12, 0xE0A, 0x48, /*slot*/ 4, 200, /*reply*/ 0x40 }
```

and `ave_cmd_build_simple()` already handles both (`driver/ave_cmd.c:172`–`180`),
including leaving Close's `+0x40` at zero, which is correct for a non-LRME
client. So the new code is:

```c
static int ave_session_stop_close(struct ave_device *ave,
                                  const struct ave_cmd_abi *abi,
                                  struct ave_sess_bufs *bufs, u64 client_id)
{
        /* Stop: id 6, 0x40, slot 7. Reply UNINIT_DONE 0xE05 arrives only
         * once the firmware's produced == consumed (fw 0xfadc). */
        ret = ave_session_simple(ave, abi, AVE_OP_STOP,  "Stop",  client_id);
        if (ret)
                return ret;
        /* Close: id 12, 0x48, slot 4. The firmware queues it behind the
         * client's remaining work (fw 0x107e8) and replies STOP_DONE 0xE0A
         * from ProcessQueue after DestroyClient (fw 0x12a64, 0x12ad8). */
        return ave_session_simple(ave, abi, AVE_OP_CLOSE, "Close", client_id);
}
```

where `ave_session_simple()` is the existing `ave_session_cmd()` wrapper used
for Open (`driver/ave_session.c:802`–`826`): allocate `cmd_len` from the FwIPC
pool, build, `ave_ipc_send(AVE_CH_IO, …)`, wait on `rx->done`.

**Implementation trap.** `ave_session_cmd()` reads the static `ave_sess_rx`,
which is only filled while `ave->ipc_rx == ave_session_ipc_rx`, and
`ave_session_selftest()` restores the previous hook on its way out
(`driver/ave_session.c:2166`). At `rmmod` time there is therefore **no rx hook
installed**, so a naive Stop/Close from `ave_remove()` would time out on a
successful command — the same class of never-says-no error as polling scratch 0
without clearing it. The teardown must re-install `ave_session_ipc_rx` before
step 1 and restore it at step 6, and the command buffers must come from
`ave_ipc_alloc()` (as `ave_session_halt()` does) rather than from
`bufs->ipc[]`, because `bufs` may legitimately be NULL if the self-test never
ran.

Wire fields, for a single AVC client with id 1 and no LRME:

| off | Stop (0x40) | Close (0x48) |
|---|---|---|
| `+0x00` u16 | `0x0006` | `0x000C` |
| `+0x02` u16 | 0 | 0 |
| `+0x08` u64 | command count | command count |
| `+0x10` u32 | client id (1) | client id (1) |
| `+0x14` u32 | 0 | 0 |
| `+0x18` u32 | 0 (AVC) | 0 (AVC) |
| `+0x1C` u32 | **7** | **4** |
| `+0x20` u32 | 200 | 200 |
| `+0x28` 16 B | `{3000, 0}` timeout | `{3000, 0}` timeout |
| `+0x40` u8 | — | **0** (client LRME flag) |

### 7.3 Timeouts and polls

| step | what to poll | interval | budget | on expiry |
|---|---|---|---|---|
| Stop | `UNINIT_DONE` `0xE05` on IO_T2H | IRQ-driven `wait_for_completion_timeout` | **3 s** (macOS's `cfg[+20] × 3000` ms, [55](55-halt-command.md) §1.2) | §7.4 |
| Close | `STOP_DONE` `0xE0A` on IO_T2H | same | **3 s** | §7.4 |
| Halt | SVE `scratch[0] == 0x08042006` | 100 µs | 1 s (macOS: `IODelay(100)` × `cfg[+24] × 10000`) | §7.4 |
| Halt | `CPU_STATUS & (RUNNING\|STOPPED) != 0`, ×3 | 50 µs | 10 ms (`AVE_IOP::Stop`'s own criterion) | log, continue |

macOS's own wait for "everything drained" is `AVE_Client_Close`'s loop:
`CheckStopped` every **10 ms** for `(timeout_a + timeout_b)/10` iterations
(`0xfffffe0008ecf438`–`60`, `IOSleep(10)` at `0xecf450` — **[I]** that
`0xfffffe0008a6a720` is `IOSleep`, from the argument 10 and the /10 budget
arithmetic). We have no `CheckStopped` equivalent because we have no CHM
queues; the two replies are our version of it.

### 7.4 When a frame is in flight, or the firmware is wedged

Today's fallback is "leak the session buffers". That is better than freeing
them, but it is not the right shape, because it leaves the *other* live
regions — FwIPC, the log ring, the iBoot mapping — to be freed anyway, and it
still gates the power domains. Replace it with a state machine:

| state | detection | action |
|---|---|---|
| **clean** | Stop and Close both replied `0xEE0000`, Halt reached `0x08042006`, `CPU_STATUS` STOPPED | full teardown: unmap and free everything (step 8), then gate |
| **stop/close timed out, core alive** | no `0xE05`/`0xE0A` within 3 s, `CPU_STATUS & STOPPED == 0` | **still send Halt** — `ProcessPowerDown` takes no client argument and the dispatcher runs it without `CController::Post` ([55](55-halt-command.md) §3), so it does not need the client to be gone. If Halt then reaches `0x08042006`, treat as clean |
| **Halt did not land** (scratch 0 stayed 0) | poll expiry, with the readback in step 3 proving the clear stuck | **do not unmap, do not free, do not gate.** Leak every DMA region *and* keep the runtime-PM reference (`ave->powered` stays true, no `pm_runtime_put_sync`). Log loudly and return. A module that unloads leaving VENC powered and a firmware running is recoverable by the next load's `core_reset=2` + `fw_restore_data=1` (proven in [55](55-halt-command.md) §14); a machine that resets is not |
| **core crashed** (`CPU_STATUS 0x28`) | read before anything | same as "Halt did not land": the core is not executing, but the datapath state is unknown. Leak, keep power, let the next load's block reset clean up |

The rule behind the table: **never drop the power reference on a state we
cannot prove is quiescent.** Leaking a few MiB and leaving a domain on costs a
reboot at worst; gating under a live master costs a reboot *always*, and
loses the log.

This also means `fw_halt` should stop being an opt-in. Halt is the only
mechanism the hardware offers, it is proven on this machine
([55](55-halt-command.md) §10, §14), and its failure mode is now a refusal to
gate rather than a reset. Keep a `fw_halt=0` escape hatch for bisecting, but
make it imply "leak and keep power".

---

## 8. DART / IOMMU and power-domain ordering

### 8.1 What the live machine says

Read-only, today, with the driver loaded (`/sys/kernel/debug/pm_genpd/pm_genpd_summary`)
— **[C]**:

```
venc_me1     on   | 40d100000.video-encoder-venc_me1   active  SW
venc_me0     on   | genpd:2:40d100000.video-encoder    active  SW
venc_pipe5   on   | genpd:1:40d100000.video-encoder    active  SW
venc_pipe4   on   | genpd:3:40d100000.video-encoder    active  SW
venc_dma     on   | (no devices; parent of pipe4/pipe5)
venc_sys     on   | 40d040000.iommu   active  SW
                  | 40d030000.iommu   active  SW
                  | genpd:0:40d100000.video-encoder  active  SW
```

and from `/proc/device-tree` — **[C]**:

```
iommu@40d030000  apple,t6000-dart  power-domains = <0x1d>          ; venc_sys
iommu@40d040000  apple,t6000-dart  power-domains = <0x1d>          ; venc_sys
video-encoder@40d100000            power-domains = <0x1d 0xc2 0xc4 0xc3 0xc5>
                                   iommus = <0x11b 0 0x11b 1 0x11c 0 0x11c 1>
```

Three consequences:

1. **The DARTs keep `venc_sys` alive by themselves.** Both DART devices are
   attached to `venc_sys` and are runtime-**active**, so `venc_sys` cannot gate
   while they are bound. This is a better explanation for
   [31](31-bringup-state.md)'s "venc_sys no longer gates off" than the m1n1
   patch, and it is the reason an `iommu_unmap` after `pm_runtime_put_sync`
   has not yet hung on the DART itself. **[C]** for the data, **[I]** for the
   causal reading.
2. **`venc_dma`, `venc_pipe4`, `venc_pipe5`, `venc_me0` and `venc_me1` *do*
   gate** when we drop the reference — and those are the domains the encoder
   datapath and (per macOS's PD 0/2/10 gating in `AVE_DPM_PowerOff`) the IOP
   side live in. That is the exposure.
3. **`venc_dma` has no device of its own.** Nothing in Linux holds it except
   the encoder's links through pipe4/pipe5. It is the DMA domain; it is exactly
   the domain you do not want to gate under an outstanding transaction.

### 8.2 The ordering constraint we get wrong

macOS's invariant, stated as an ordering: **unmap is always later than
power-off, and power-off is always later than "the coprocessor is provably
stopped".**

```
macOS:  Stop+Close -> drain -> Halt -> IOP::Stop -> DART flush -> DART inactive
        -> gate PDs        ................ (much later, at IOService::stop)
        -> DARTUnmapSurface -> HwC::Uninit -> DestroySurface
ours:   (nothing) -> (optional Halt) -> gate PDs -> unmap -> free
```

Ours has the unmap on the wrong side of the gate **and** has nothing on the
left of it. Two separate fixes, and the cheap one (move the unmaps up) does not
help without the expensive one (actually stop the device).

### 8.3 `venc_me1` specifically

`ave_power_me1_off()` runs first inside `ave_power_off()`
(`driver/ave_drv.c:465`). Its comment says "child of venc_me0: release it
first", which is right for genpd refcounting but wrong relative to the
hardware: ME1 is PD 6, the deepest stage of the motion-estimation datapath,
and macOS is still raising it to ClockOn (`SetPS(6, 2, wait)`,
`0xfffffe0008f14570`) at the moment it sends Halt, and only lets it fall at the
very end of `AVE_DPM_PowerOff` via the dependency graph. Move
`ave_power_me1_off()` to immediately before `pm_runtime_put_sync()`, after the
core is stopped and after the unmaps. **[I]** — no instruction says ME1 must
outlive the unmaps; the argument is that it is part of the datapath and the
datapath must be dead before anything is torn down.

### 8.4 What we cannot reproduce

`setActive(false)` and `cacheFlushInactive` are AppleDART platform functions
with no Linux equivalent, and AGENTS.md forbids writing DART registers
directly. The nearest honest approximation is to do the unmaps themselves while
powered — `iommu_unmap`/`dma_free_coherent` already issue the DART TLB
invalidations through `apple-dart`, which is the part of `cacheFlushInactive`
that matters for correctness. Do not add a DART driver, and do not write the
DAPF ([49](49-dapf-write-reset.md)).

---

## 9. What success and failure look like on hardware

Proposal for the operator. One fresh boot, `tools/e3-run.sh`-style capture, the
driver built with §7.

**Before the first unload, record:** `CPU_STATUS`, `scratch[0..3]`, SVE
`INTR_STATUS`, the SMMU fault count, and `pm_genpd_summary` for the venc tree.

**A run that works** produces, in one boot, on every cycle:

```
session: Stop:  reply 64 bytes flags 0x40: id=0x0e05 cid=0x1 slot=0x7 status=0xee0000
session: Close: reply 64 bytes flags 0x40: id=0x0e0a cid=0x1 slot=0x4 status=0xee0000
halt: scratch 0 = 0x08042006, the firmware reached its wfi
halt: CPU_STATUS 0x0000002e STOPPED after 3 sample(s)
... unmap/free lines ...
powered off (remove)
```

and then `insmod` again, `core_reset=2 fw_restore_data=1`, and gets a clean
handshake — **repeatedly, in the same boot, with no reboot between
experiments.** That is the whole deliverable: three or more load/unload cycles
in one boot, `pm_genpd_summary` showing the venc leaves returning to `off-0`
after each unload and back to `on` after each load.

**Discriminators that can say "no"** — each of these is a specific, expected
failure, not a hang:

| observation | reading |
|---|---|
| Stop times out but Close replies | the deferred-reply path (`fw 0xfb14`) is holding `UNINIT_DONE` because produced != consumed — i.e. there really was work in flight. Raise the budget; do not proceed to free |
| Stop replies, Close times out | the Close item is stuck in the priority queue behind something. Send Halt anyway (§7.4) and record `CPU_STATUS` |
| Close replies `0xEE0002` | `IsClientRegistered` said no (`fw 0x10850`) — we closed a client the firmware had already destroyed. Harmless; means our Open/Close bookkeeping is off |
| `scratch[0]` stays 0 after Halt | the command never dispatched. **Do not gate.** Leak and keep power (§7.4); the next load's `core_reset=2` recovers |
| everything passes and the machine still resets | rank 1 is wrong, or the gate is not the trigger. Next discriminator: unload with `pm_runtime_put_sync()` skipped entirely (leave VENC powered). If that stops the resets, it is the gate; if it does not, it is the unmap or the IRQ |
| the machine resets with the *unmaps* moved before the gate but the gate still present | narrows it to the gate alone |

**The negative control for the whole exercise**: a load that does
`session_config_only=1` (Config, no Open, no frame) and then unloads. There is
no client to Stop/Close and no datapath work, so a reset there would mean the
cause is in the power/unmap path and nothing to do with the session. That case
already ran clean in H2/R4 ([55](55-halt-command.md) §10, §14), which is itself
evidence that the session — a *running datapath* — is what makes the unload
dangerous.

**Do not** try to shortcut this by sending Halt only. F4 did exactly that, with
a successful Halt, and still reset. The Stop/Close pair and the unmap ordering
are not decoration.
