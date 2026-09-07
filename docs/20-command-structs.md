> ## Verification note
>
> - **`+0x24` is client priority — confirmed.** Firmware `ProcessCmd_Start_AVC`
>   does `ldr w2, [x21, #36]` at `0x29834` then `bl 0x401c8`, which the symbol
>   table gives as `CAVEPriorityQueue::SetClientPriority(unsigned long long, int)`.
>   So it is not command-specific and not a timeout. `driver/ave_abi.h` updated.
> - The `RCMode` polarity catch is recorded as found: `CRateControl::Check_RCMode`
>   *rejects* 20 and 100, and the `csel` polarity inverts the obvious reading.
>   Worth keeping in mind for any future reading of that function.
> - `RCMode = 3` for constant QP remains **inferred**, not read. Nothing in
>   either binary names the `_E_AVE_RCMode` enumerators.

# Command struct field layouts

[docs/07-commands-abi.md](07-commands-abi.md) established the wire command ids,
the struct sizes, and the common `0x40`-byte header. This document goes inside
the structs.

Every row is marked **confirmed** (read out of an instruction, VA cited),
**inferred** (a chain of reasoning over confirmed facts, stated as such), or
**unknown**. Reproduce any line with:

```sh
python3 tools/disas.py --kext --addr 0xfffffe0008b668c0 -n 0x180
python3 tools/disas.py --fw   --addr 0x29578 -n 0x1d0
```

Two coverage caveats up front:

* `sCAveCmdAvcStart` (0x3180) and `sCAveCmdAvcProcess` (0x63D8) are mapped at
  **block** granularity completely, and at **field** granularity only for the
  fields a minimal encode needs. §6 says exactly which regions are unmapped.
* `sCAveCmdHevcStart`, `sCAveCmdHevcProcess` and `sCAveCmdReset` were not
  looked at at all.

## 0. Corrections and additions to the common header

Two header fields that docs/07 left open are now settled.

### `+0x24` is a **client priority**, not a timeout

docs/07 recorded `+0x24` as "command-specific" and said a timeout reading was
*inferred* from the constant 200. It is a priority, for all commands, and the
firmware proves it in two independent handlers:

| firmware | instructions | meaning |
|---|---|---|
| `ProcessCmd_Start_AVC` | `0x29834` `ldr w2,[x21,#36]` → `0x29840` `bl 0x401c8` | `CAVEPriorityQueue::SetClientPriority(cid, prio)` |
| `ProcessCmd_Process_AVC` | `0x2aee8` `ldr w2,[x19,#36]` → `0x2aef0` `bl 0x401c8` | same |
| `ProcessCmd_Priority` | `0x2d67c` (docs/07) → `0x401c8` | same |

`0x401c8` is `__ZN17CAVEPriorityQueue17SetClientPriorityEyi`
(`CAVEPriorityQueue::SetClientPriority(unsigned long long, int)`), from
`data/derived/symbols.txt`. In `Process_AVC` the call is guarded by a compare
against the queue's current priority (`0x2ae7c`–`0x2ae88`), i.e. it is only
re-applied when it changed. **Confirmed.** The default 200 that
Open/Close/Start/Stop carry is simply a default priority.

### `+0x10` is the client id, and `Open` is what registers it

`ProcessCmd_Open` passes `cmd[0x10]` to
`CAVEPriorityQueue::RegisterClient(unsigned long long)` (`0x3f660`), reached at
`0x29650` `bl 0x3f660` with `x1 = x20 = [x23,#16]` loaded at `0x295a0`.
`RegisterClient` linearly scans an array of `u64` ids at `queue+0x18` whose
count is at `queue+0x10` (`0x3f674`–`0x3f6a0`), rejects a duplicate with
`"Warning : Client %llu already registered"` (string `0x123a28`), and rejects a
129th client with `"No more space for new client:%llu"` (`0x123a5b`) once the
count reaches `0x80` (`0x3f710` `cmp w8,#0x80`). **So the firmware supports at
most 128 open clients.** Confirmed.

---

## 1. `sCAveCmdOpen` — 0x48 bytes

**The eight bytes past the header are unused.** The struct is the common header
plus a zeroed 8-byte tail; a driver has nothing extra to fill in.

Host side, `AVE_CHM_MakeFwCmd_Open` (`0xfffffe0008b668c0`). The complete set of
stores into the command buffer `x23` is:

| off | size | value | store VA |
|---:|---:|---|---|
| `0x00`–`0x3F` | 64 | zeroed (`stp q0,q0` ×2) | `0xfffffe0008b66968`, `0xfffffe0008b6696c` |
| `0x40` | 8 | zero | `0xfffffe0008b66964` (`str xzr,[x23,#64]`) |
| `0x00` | u16 | id = 3 | `0xfffffe0008b669a4` |
| `0x08` | u64 | `CNT` (arg 2) | `0xfffffe0008b66974` |
| `0x10` | u64 | `CID` = `chm->[0x38]` | `0xfffffe0008b66974` |
| `0x18` | u32 | client type = `chm->[0x34]` | `0xfffffe0008b66980` |
| `0x1c` | u32 | enc type = `chm->[0x18]->[0x270]` | `0xfffffe0008b66980` |
| `0x20`/`0x24` | u32×2 | literal `{3, 200}` | `0xfffffe0008b6698c` |
| `0x28` | u32 | `chm->[0x18]->[0x1564]` | `0xfffffe0008b66994` |
| `0x30`–`0x3F` | 16 | `_S_AVE_TimeOut` copy | `0xfffffe0008b6699c` |

There is no store at any offset ≥ `0x41`. **Confirmed** — the function is only
`0x180` bytes of straight-line code before the logging tails, and the whole of
it was read.

Firmware side, `CFlowControllerBase::ProcessCmd_Open` (`0x29578`). With
`x23 = this->[1472]` (the command pointer, loaded at `0x29594`) the *only*
loads from the command in the entire function are:

| read | VA | use |
|---|---|---|
| `[x23,#8]` (`CNT`) | `0x295e0`, `0x29690` | trace line, and arg 2 of the Ack |
| `[x23,#16]` (`CID`) | `0x295a0`, `0x295e0`, `0x296d0` | `RegisterClient`, Ack |
| `[x23,#32]` (slot) | `0x29698`, `0x296d4` | arg 5 of the Ack |

Nothing at `+0x40` or beyond. **Confirmed.** *(Negative-result check per
docs/00 trap 2: the same grep over `ProcessCmd_Config` returns positives at
`+72/+73/+88/+92/+96/+104/+112`, and over `ProcessCmd_Start_AVC` at
`+72/+80`, so the test does discriminate.)*

Return path: on success `ProcessCmd_Open` calls
`CFlowControllerBase::NotificationToHost(u16 cmdid, u64 cnt, u64 cid, int
status, u32 slot, u32)` (`0x311a8`) as
`(3, cmd[8], cmd[0x10], 0, cmd[0x20], 0)` at `0x296d0`–`0x296e8`, and returns 0.
If `RegisterClient` fails it sends status `-1004` and returns `-1015`
(`0x29698`–`0x296b4`). **Confirmed.**

### Complete `sCAveCmdOpen`

```c
struct sCAveCmdOpen {          /* 0x48 */
    /* 0x00 */ struct ave_cmd_hdr hdr;   /* 0x40, see docs/07 §4 */
    /* 0x40 */ uint64_t reserved;        /* written 0, never read */
};
```

---

## 2. `sCAveCmdConfig` — 0x78 bytes

Writer: `AVE_HwC::MakeFwCmd_Config(this, u64 cnt, u32, _S_AVE_TimeOut*,
sCAveCmdConfig*)` at `0xfffffe0008c046b8`, called from
`AVE_HwC::SendFwCmd_Config()` (`0xfffffe0008c05948`) as
`(this, 0, 0, &timeout, buf)` (`0xfffffe0008c05a4c`–`0xfffffe0008c05a60`).
Reader: `CFlowControllerBase::ProcessCmd_Config` (`0x29080`).

The builder zeroes `+4 .. +119` (`0xfffffe0008c046f8`–`0xfffffe0008c04714`)
and writes only the fields below.

| off | size | name / meaning | evidence | status |
|---:|---:|---|---|---|
| `0x00` | u32 | id = 1 (32-bit store; upper half is header `+0x02`) | `0xfffffe0008c0471c` | confirmed |
| `0x08` | u64 | `CNT` | `0xfffffe0008c04720` | confirmed |
| `0x18` | u32 | client type = 0 | `0xfffffe0008c04724` | confirmed |
| `0x20` | u32 | slot = `0xFFFFFFFF` | `0xfffffe0008c0472c` | confirmed |
| `0x24` | u32 | priority = 0 | same store (`movi d0,#0xffffffff`) | confirmed |
| `0x30` | 16 | `_S_AVE_TimeOut` | `0xfffffe0008c04734` | confirmed |
| `0x48` | u8 | **bool**, `= (this->[32]->[0] == 3)`. Gates the McpuController: if bit 0 is set the firmware skips creating it | host `0xfffffe0008c04748`; fw `0x29118` → `this->[1393]`, tested `0x29178` `tbnz w8,#0` | confirmed (semantics of the source `==3` unknown) |
| `0x49` | u8 | **bool**, always 1 from this builder. Firmware creates `McpuController` only when this == 1 | host `0xfffffe0008c0474c`; fw `0x2910c` → `this->[1394]`, `0x29180` `cmp w8,#1` | confirmed |
| `0x4A` | u8 | bit 17 of `this->[32]->[40]` | host `0xfffffe0008c04754`+`58` | written but **never read** by `ProcessCmd_Config` |
| `0x50` | u64 | **not written** by this builder; it *is* a real field — the exit log prints it as `%llu` | host log load `0xfffffe0008c048d0` | unknown |
| `0x58` | u32 | **doorbell cadence**, channel manager `this->[1488]` | fw `0x29128` `ldr w1,[x20,#88]` → `0x2912c` `bl 0xdc460` = `CChannelManager::DoorBellCadenceSet(unsigned)` | confirmed |
| `0x5C` | u32 | doorbell cadence, channel manager `this->[1504]` | fw `0x29134`+`0x29138` | confirmed |
| `0x60` | u32 | **MCC DSID**, `AVE_MCC::GetDSID(this->[152], _E_AVE_MCC_DSType(0), &cmd[0x60])` | host `0xfffffe0008c04764`–`6c`; fw `0x29120` `ldr w8,[x20,#96]` → `this->[1396]` | confirmed |
| `0x68` | u64 | **shared-memory base**, `AVE_Surface::GetDARTAddr(this->[80], this->[72], 0)` — i.e. the IOVA the coprocessor will use | host `0xfffffe0008c04874`+`78`; fw `0x291d8` `ldr x1,[x20,#104]` | confirmed |
| `0x70` | u32 | **shared-memory size**, `AVE_Surface::GetSize(this->[80])` | host `0xfffffe0008c04880`+`84`; fw `0x291dc` `ldrsw x2,[x20,#112]` | confirmed |

The exit trace at `0xfffffe0008c048b4`–`0xfffffe0008c04924` prints exactly
`{+0x48&1, +0x49&1, +0x4A&1, +0x50, +0x60, +0x68, +0x70}` with format
`"… | %p %d | %p %d %d %d %llu | %d | 0x%llx %d"` (`0xfffffe00072e34f8`),
which is an independent confirmation of the field set and of the widths.

### What the firmware does with it

`ProcessCmd_Config` (`0x29080`), in order:

1. `this->[1394] = cmd[0x49]`, `this->[1393] = cmd[0x48]`,
   `this->[1396] = cmd[0x60]` (`0x2910c`–`0x29124`).
2. `DoorBellCadenceSet` on two channel managers from `cmd[0x58]` / `cmd[0x5C]`
   (`0x29128`–`0x29138`).
3. Creates `AvePipeISRManager` unconditionally (`0x2916c`, virtual slot
   `[vt+104]`, stored at `this->[18488]`), then `McpuController`
   (`0x291b8`, `[vt+112]`, `this->[18480]`) only if
   `!(cmd[0x48] & 1) && cmd[0x49] == 1`.
4. `CFlowControllerBase::MapRemoteRegs()` (`0x31b14`), then
   `PlatformIOPIPCManager::AddSharedMemory(cmd[0x68], cmd[0x70])`
   (`0x291e0` `bl 0xe6cec`), then **twelve**
   `PlatformIOPIPCManager::MapSharedMemory(index, size)` calls (`0xe7050`) for
   `index = 0..3` × `size = 1176, 2048, 4` (`0x291ec`–`0x292fc`). The results
   land in `this->[5048/5056/5064/5072]`, `this->[16360..16376]` and
   `this->[16384..16416]`.
   So `+0x68`/`+0x70` describe **one host buffer out of which the firmware
   sub-allocates four sets of three regions**, and a driver must supply at
   least `4 * (1176 + 2048 + 4) = 12912` bytes for step 4 to succeed
   (**inferred** — the allocator's alignment/overhead was not read).
5. `CAVECommonController::ProcessChipReset(true)` (`0x29304`), three MMIO
   read-modify-writes through a table pointer at `0x2649c8`
   (`0x2930c`–`0x29368`), then `NotificationToHost(1, cmd[8], …, cmd[0x20])`
   (`0x2936c`–`0x29374`).

### Complete `sCAveCmdConfig`

```c
struct sCAveCmdConfig {           /* 0x78 */
    /* 0x00 */ struct ave_cmd_hdr hdr;   /* id=1, client_type=0, slot=~0 */
    /* 0x40 */ uint8_t  _pad40[8];       /* not written, not read */
    /* 0x48 */ uint8_t  bSkipMcpu;       /* 1 -> no McpuController */
    /* 0x49 */ uint8_t  bCreateMcpu;     /* must be 1 to get McpuController */
    /* 0x4A */ uint8_t  unknown4A;       /* written, never read */
    /* 0x4B */ uint8_t  _pad4B[5];
    /* 0x50 */ uint64_t unknown50;       /* logged, never written or read here */
    /* 0x58 */ uint32_t doorbell_cadence_0;
    /* 0x5C */ uint32_t doorbell_cadence_1;
    /* 0x60 */ uint32_t mcc_dsid;
    /* 0x64 */ uint32_t _pad64;
    /* 0x68 */ uint64_t shmem_iova;
    /* 0x70 */ uint32_t shmem_size;
    /* 0x74 */ uint32_t _pad74;
};
```

---

## 3. `sCAveCmdAvcStart` — 0x3180 bytes

Writer: `AVE_CHM_MakeFwCmd_Start_AVC(_S_AVE_CHM*, u64 cnt, u32,
_S_AVE_TimeOut*, sCAveCmdAvcStart*)` at `0xfffffe0008b66ff8`.
Readers: `CFlowControllerBase::ProcessCmd_Start_AVC` (`0x29744`) and, for the
payload, `CFlowControllerBase::ProcessCmd_Start(u64 cid, u32 enctype, void*
cmd)` (`0x31e44`), tail-called at `0x29858`.

In the writer, `x19 = chm`, `x27 = chm->[0x18]` (**the `_S_AVE_Client`**,
see below), `x25 = chm->[0x20]` (the surface set), `x23 = cmd`.

### 3.1 Block map — confirmed, and it accounts for all 0x3180 bytes

`bzero(cmd, 0x3180)` at `0xfffffe0008b670ac`, then:

| cmd off | len | source | store / call VA |
|---:|---:|---|---|
| `0x0000` | `0x40` | common header, id = 6, `{6,200}` at `+0x20`/`+0x24` | `0xfffffe0008b670b8`–`ec` |
| `0x0040` | `0x08` | never written | — |
| `0x0048` | `u64` | `AVE_Surface::GetDARTAddr(surf, chm->[40], 0)` where `surf = *(chm->[0x20] + 0xF1628 + chm->[44]*8)` | `0xfffffe0008b67120`+`24` |
| `0x0050` | `u32` | `AVE_Surface::GetSize(surf)` | `0xfffffe0008b67148`+`4c` |
| `0x0058` | `u64` | `GetDARTAddr` of `*(chm->[0x20] + 0xF1648)` | `0xfffffe0008b67168`+`6c` |
| `0x0060` | `u32` | `GetSize` of the same | `0xfffffe0008b67174`+`78` |
| `0x0068` | `0x300` | `memcpy` from `client + 0x1538` | `0xfffffe0008b67188` |
| `0x0368` | `0x2460` | `memcpy` from `client + 0x1838` = **`_S_AVE_Client::VideoParams`**, staged through `chm + 0x4C0` so that `AVE_CHM_SetFwBuf` can patch buffer addresses into it | stage `0xfffffe0008b6719c`, patch `0xfffffe0008b671b8`, copy `0xfffffe0008b67308` |
| `0x27C8` | `0x154` | `memcpy` from `client + 0x417C` | `0xfffffe0008b67320` |
| `0x291C` | `0x6B4` | `memcpy` from `client + 0x4AD0` (the AVC SPS/PPS block) | `0xfffffe0008b67338` |
| `0x2FD0` | `0x180` | `memcpy` from `client + 0x5184` | `0xfffffe0008b67350` |
| `0x2580` | `u16` | patched **after** the `0x368` copy: `0` or `3`, only when `chm->[52] == 4`; `3` unless `AVE_DevInfo::GetDevType(client->[56]) < 30` | `0xfffffe0008b67374` |
| `0x3150` | `u8` | bit 0 of `client + 1472` | `0xfffffe0008b67380` |
| `0x3154` | `0x28` | `AVE_MD::RetrieveEUInfo(chm->[44], &out)` → `_S_AVE_EUInfo` | `0xfffffe0008b67390`, stores `0xfffffe0008b67598`+`9c` |
| `0x317C` | `4` | never written | — |

Finally the whole payload `cmd+0x68 .. cmd+0x317F` (`0x3118` bytes) is
*also* `memcpy`'d into `AVE_Surface::GetKernelAddr(*(chm->[0x20] + 0xF1650 +
chm->[44]*8), 0)` at `0xfffffe0008b675d4`–`e0`. That is a host-side mirror,
not part of the wire struct.

`AVE_CHM_SetFwBuf(_S_AVE_CHM*, _S_AVE_SurfaceSet*, _S_AVE_SurfaceInfoSet*,
_S_AVE_Buf_Set*)` is called with 4th argument `chm + 0x4E8`
(`0xfffffe0008b671b0`), which is `(chm+0x4C0) + 0x28`. Since `chm+0x4C0` is the
staging copy of `VideoParams`, **`VideoParams + 0x28` is an `_S_AVE_Buf_Set`,
i.e. `cmd + 0x390`** is where the working-buffer DMA addresses go. Confirmed
from the call signature; the internal layout of `_S_AVE_Buf_Set` was not read.

### 3.2 How the block map was turned into names

`chm->[0x18]` is a `_S_AVE_Client`. Proof: `AVE_Client_CheckCommonInfo(
_S_AVE_Client* pClient, bool, AVE_SessionSettings_UserKernel_Data* pInfo)`
(`0xfffffe0008b8f6c8`) asserts

```
pClient->VideoParams.ui32Width  == pInfo->VideoParams.ui32Width &&
pClient->VideoParams.ui32Height == pInfo->VideoParams.ui32Height
```

(string at `0xfffffe0007289b9b`, loaded `0xfffffe0008b8f780`) and implements it
as

```
fffffe0008b8f708:  ldr w9, [x19, #6200]    ; pClient->VideoParams.ui32Width
fffffe0008b8f70c:  ldr w10,[x21, #2648]    ; pInfo->VideoParams.ui32Width
fffffe0008b8f718:  ldr w9, [x19, #6204]    ; ...ui32Height
fffffe0008b8f71c:  ldr w10,[x21, #2652]
```

so `_S_AVE_Client::VideoParams` is at `0x1838` (6200) with `ui32Width` first
and `ui32Height` at `+4`. **That is exactly the source of the `0x2460` block,
so `cmd+0x368` = width and `cmd+0x36C` = height.** Confirmed.

The other anchor is `AVE_Client_Enc_PrintAll` calling
`AVE_Alg_PrintCfg(_S_AVE_Alg_Cfg*)` with `pClient + 0x16D8`
(`0xfffffe0008b800a4`+`a8`, call `0xfffffe0008b800bc`). `0x16D8` is inside the
first block, so `sAlgCfg` lands at `cmd + 0x68 + (0x16D8 - 0x1538)` =
**`cmd + 0x208`**. `AVE_Alg_PrintCfg` (`0xfffffe0008cd3c28`) then hands its
sub-structs to named printers:

| `_S_AVE_Alg_Cfg` off | type | printer | cmd off |
|---:|---|---|---:|
| `0x00` | `sComm` (printed inline) | — | `0x208` |
| `0x20` | `_S_AVE_RC_Cfg` | `AVE_RC_PrintCfg` (`0xfffffe0008bff044`), call `0xfffffe0008cd411c` | `0x228` |
| `0x98` | `_S_AVE_GOP_Cfg` | `AVE_GOP_PrintCfg` (`0xfffffe0008bfc590`), call `0xfffffe0008cd4134` | `0x2A0` |
| `0xD0` | `_S_AVE_Ref_Cfg` | `AVE_Ref_PrintCfg` (`0xfffffe0008c035d0`), call `0xfffffe0008cd414c` | `0x2D8` |
| `0xF8` | `_S_AVE_QPMod_Cfg` | `AVE_QPMod_PrintCfg` (`0xfffffe0008c02574`), call `0xfffffe0008cd4164` | `0x300` |

Each printer's format strings name the fields and the loads give the offsets.
All of the following is **confirmed**.

### 3.3 The bring-up-critical fields

`sAlgCfg.sComm` at `cmd + 0x208` — offsets from `AVE_Alg_PrintCfg`
(`0xfffffe0008cd3dcc`, `0xfffffe0008cd3ea8`, `0xfffffe0008cd3f84`,
`0xfffffe0008cd4060`):

| cmd off | type | name |
|---:|---|---|
| `0x208` | u64 | `Feature` — bit 1 selects frame drop (`ProcessCmd_Start` `0x32518`–`24`, `ubfx x1,x8,#1,#1` → `CAVEClient::SetUseFrameDrop`) |
| `0x210` | u64 | `SEIFeature` |
| `0x218` | u64 | `VUIFeature` |
| `0x220` | u32 | `FrameRate` |

`sAlgCfg.sRC` (`_S_AVE_RC_Cfg`) at `cmd + 0x228` — offsets from
`AVE_RC_PrintCfg` (`0xfffffe0008bff1e4`, `0xfffffe0008bff2c0`,
`0xfffffe0008bff39c`, `0xfffffe0008bff478`, `0xfffffe0008bff554`+`5c`,
`0xfffffe0008bffdb4`):

| cmd off | type | name |
|---:|---|---|
| `0x228` | u64 | `Feature` |
| `0x230` | int | `LookAheadFrameCount` |
| **`0x234`** | int | **`RCMode`** |
| `0x238` | int | `Bitrate` |
| **`0x240`** | int | **`QP` (I)** |
| **`0x244`** | int | **`QP` (P)** |
| **`0x248`** | int | **`QP` (B)** |
| `0x298` | int | `RCQPRange` min |
| `0x29C` | int | `RCQPRange` max |

`RCMode` at `cmd+0x234` is cross-confirmed on the firmware side: `ProcessCmd_
Start` loads it twice, once to store into the client object
(`0x32534` `ldr w8,[x20,#564]` → `str w8,[x23,#140]`) and once to validate it:

```
32684:  ldr  w0, [x20, #564]
32688:  bl   0xf830              ; CRateControl::Check_RCMode(_E_AVE_RCMode)
```

`Check_RCMode` is a six-instruction leaf:

```
 f830:  cmp   w0, #0x14
 f834:  mov   w8, #0x64
 f838:  ccmp  w0, w8, #0x4, ne
 f83c:  mov   w8, #0xfffffc16        ; -1002
 f840:  csel  w0, wzr, w8, ne
```

so it **returns `-1002` for mode 20 and mode 100 and 0 for everything else** —
it is a rejection list, not an accept list. (Note the polarity: `csel Wd,Wn,Wm,
cond` yields `Wn` when the condition holds, so `ne` → 0 → success.) The values
the rest of the firmware branches on are 3, 4, 20 and 100
(`CAVERefManager::Init` `0x20ed8`–`0x20ef0`; `CRateControl::ProcessInit`
`0xc158`/`0xc160`; `CRateControl::ProcessAccumulate` `0xe604`/`0xe60c`).
Which of them is constant-QP is **not determined at the `_E_AVE_RCMode`
level**. What *is* determined is one level down: `RateControl::CreateInstance`
(`0x60c4`) dispatches on `RateControlParameters+12` and installs
`ConstantQpRateControl`'s vtable (`__ZTV21ConstantQpRateControl` at `0x135938`)
for the value **3** (`0x6200` `cmp w8,#0x3` → `0x6224`–`0x623c`). `3` is also
one of the values `sCRCInitParams+12` is compared against, and it is *not* on
`Check_RCMode`'s rejection list, so **`RCMode = 3` is the constant-QP mode** —
**inferred**, on the strength of the shared `+12` offset and the shared value
set, not read end-to-end.

`sAlgCfg.sGOP` (`_S_AVE_GOP_Cfg`) at `cmd + 0x2A0` — offsets from
`AVE_GOP_PrintCfg` (`0xfffffe0008bfc730`, `0xfffffe0008bfc80c`+`14`,
`0xfffffe0008bfc904`, `0xfffffe0008bfc9e0`, `0xfffffe0008bfcabc`,
`0xfffffe0008bfcb98`, `0xfffffe0008bfcce8`, `0xfffffe0008bfce38`):

| cmd off | type | name |
|---:|---|---|
| `0x2A0` | u32 | `Feature` |
| `0x2A8`/`0x2AC`/`0x2B0` | int×3 | `NumOfFrame[3]` (indexed by frame type) |
| `0x2B4` | int | `NumOfGOPLayer` |
| **`0x2B8`** | int | **`MaxKeyFrameInterval`** |
| `0x2BC` | int | `StrictKeyFrameInterval` |
| `0x2C0` | double | `MaxKeyFrameIntervalDuration` |
| `0x2C8` | double | `StrictKeyFrameIntervalDuration` |
| `0x2D0` | int | `NumOfTemporalLayer` |

Four-way cross-check: the firmware calls
`AVE_KeyFrame::Init(u64 cid, int, int, double, double, int)` (`0xe75f0`) at
`0x325c4` with exactly `(cid, cmd[0x2B8], cmd[0x2BC], cmd[0x2C0], cmd[0x2C8],
cmd[0x220])` — the two ints, the two doubles and the frame rate, in the order
and with the widths the print map predicts.

`ProcessCmd_Start` also special-cases client type 4 (`cmd[0x18] == 4`,
`0x32568`): it overwrites `cmd[0x2B8] = INT_MAX` unless it is already 1,
`cmd[0x2BC] = 0`, and both durations to `-1.0` (`0x32570`–`0x325a4`) —
i.e. "no periodic key frames".

`sAlgCfg.sRef` (`_S_AVE_Ref_Cfg`) at `cmd + 0x2D8` — offsets from
`AVE_Ref_PrintCfg` (`0xfffffe0008c03770`, `0xfffffe0008c0384c`+`54`,
`0xfffffe0008c03944`+`4c`):

| cmd off | type | name |
|---:|---|---|
| `0x2D8` | u32 | `Feature` |
| `0x2E0`/`0x2E4`/`0x2E8` | int×3 | `ReferenceNum[3]` |
| `0x2F0`/`0x2F4`/`0x2F8`/`0x2FC` | int×4 | `ReferenceGap[4]` |

`VideoParams` at `cmd + 0x368`:

| cmd off | type | name | evidence |
|---:|---|---|---|
| **`0x368`** | u32 | **`ui32Width`** | `AVE_Client_CheckCommonInfo` `0xfffffe0008b8f708`; fw bound check `0x326e4` `cmp w8,#0x1,lsl#12` (≤ 4096) |
| **`0x36C`** | u32 | **`ui32Height`** | `0xfffffe0008b8f718` |
| `0x370` | u8 | firmware-RC flag → `CAVEPriorityQueue::SetClientUseFirmwareRC(cid,bool)` (`0x402e0`) | fw `0x32690`, call `0x326a8` |
| `0x390` | — | `_S_AVE_Buf_Set` (working-buffer DMA addresses) | `AVE_CHM_SetFwBuf` 4th arg, `0xfffffe0008b671b0` |

AVC SPS/PPS block at `cmd + 0x291C` — from `AVE_Client_Enc_PrintAVC`
(`0xfffffe0008b7cc90`), which bases on `x28 = pClient + 0x4AD0`
(`0xfffffe0008b7ccd4`+`d8`):

| cmd off | type | name | load VA |
|---:|---|---|---|
| **`0x291C`** | int | **profile** (`_E_AVC_Profile`) | `0xfffffe0008b7d458` `ldr w8,[x28]` |
| **`0x2938`** | int | **level** | `0xfffffe0008b7d45c` `ldr w9,[x28,#28]` |
| `0x2950` | u8 | `LOSSLESS` (bit 0) | `0xfffffe0008b7d480` `ldrb w13,[x28,#52]` |
| `0x2D44` | int | `max_num_ref_frames` | `0xfffffe0008b7d20c` `ldr w8,[x28,#1064]` |
| `0x2FD8` | int | entropy mode; `== 1` → CABAC, else CAVLC | `0xfffffe0008b7d2ec` `ldr w8,[x28,#1724]` (`0x4AD0+1724 = 0x518C`, i.e. in the `0x2FD0` block) |

The same print confirms there is **no `nmb` field**: the log computes it as
`(width >> 4) * (height >> 4)` at `0xfffffe0008b7d464`–`78`.

### 3.4 Other scalars the firmware reads out of the payload

`ProcessCmd_Start_AVC` (`0x29744`):

* `cmd[0x48]` (u64) and `cmd[0x50]` (u32) are arguments 1 and 2 of
  `CFlowControllerBase::CreateClient(u64, u32, u64 cid, u32 enctype, u32
  clienttype)` (`0x31cc0`), read at `0x297d8`/`0x297dc`, and are forwarded
  as the `(y, j)` pair of
  `CAVEPriorityQueue::CreateClient(char const*, CObject*, u64, u32, int, u64,
  u32, u32, AVE_PIODMACtrl*)` (`0x3fae8`, call `0x31d8c`). Together with the
  writer, that makes **`+0x48`/`+0x50` the (IOVA, size) of a per-client
  firmware working buffer** and `+0x58`/`+0x60` a second such pair.
  Confirmed for the pair semantics; which buffer each one is, unknown.
* On `CreateClient` failure the handler acks `-1008` and returns `-1`
  (`0x29918`–`0x29938`).

`ProcessCmd_Start` (`0x31e44`), payload reads not already listed:

| cmd off | width | use | VA |
|---:|---|---|---|
| `0x78` | u32 | → `client+152` | `0x3254c` |
| `0x79` | u8 (bit 0) | → `client+0` and `client+9` | `0x32528`, `0x32620` |
| `0x2584` | u8 | → `client+2`; also `+0x26EA`, `+0x26EB`, `+0x25D0` are read as bytes | `0x32554`, `0x325d4`, `0x325fc`, `0x326ac` |
| `0x2700` | u32 | `== 1` → `CAVEClient::SetMCTFLowLatencyMode` (`0x3f1a4`) | `0x32504`–`14` |
| `0x2704` | u32 | → `client+172`, `+12272`, `+12328`, `+12384` | `0x320c0`–`e0` |
| `0x2708` | u32 | `== 1` gates the width ≤ 4096 path | `0x326c8` |
| `0x3150` | u8 | → `client+10` | `0x31f18` |
| `0x3154` | s32 | index into the `_S_AVE_EUInfo` array that follows; `client+0x1D0 ..+0x1F3` receives `memcpy(cmd+0x3158, 0x24)` and the selected entry drives `CAVEClient::SetWorkMode(_E_AVE_WorkMode)` (`0x3f1b4`) | `0x31f14`–`0x31f4c` |

### 3.5 A minimal fixed-QP, I-frame-only AVC Start

Everything here is confirmed except where noted. Offsets are into
`sCAveCmdAvcStart`.

```
hdr.id            = 6                       0x0000 u16
hdr.cnt           = <sequence>              0x0008 u64
hdr.cid           = <client id>             0x0010 u64
hdr.client_type   = 1                       0x0018 u32   (docs/07 §5)
hdr.enc_type      = 1  (AVC)                0x001C u32
hdr.slot          = 6                       0x0020 u32
hdr.priority      = 200                     0x0024 u32
hdr.timeout[16]                             0x0030

per-client fw buffer  iova / size           0x0048 u64 / 0x0050 u32
second buffer         iova / size           0x0058 u64 / 0x0060 u32

sAlgCfg.sComm.FrameRate                     0x0220 u32
sAlgCfg.sRC.RCMode          = 3 (const QP)  0x0234 int   <- inferred value
sAlgCfg.sRC.QP[I]                           0x0240 int
sAlgCfg.sGOP.MaxKeyFrameInterval = 1        0x02B8 int   (I-frame only)
sAlgCfg.sRef.ReferenceNum[*]     = 0        0x02E0..
VideoParams.ui32Width                       0x0368 u32
VideoParams.ui32Height                      0x036C u32
VideoParams.buf_set                         0x0390 (_S_AVE_Buf_Set)
SPS.profile                                 0x291C int
SPS.level                                   0x2938 int
SPS.max_num_ref_frames      = 0             0x2D44 int
entropy (0=CAVLC, 1=CABAC)                  0x2FD8 int
EUInfo                                      0x3154 (0x28 bytes)
```

---

## 4. `sCAveCmdAvcProcess` — 0x63D8 bytes

Writer: `AVE_CHM_MakeFwCmd_Process_AVC(_S_AVE_CHM*, u64 cnt, u32,
_S_AVE_TimeOut*, sCAveCmdAvcProcess*, int slot, AVE_PICMGMT_PARAMS*,
_S_AVE_FrameInfo*)` at `0xfffffe0008b69f74`. Argument registers are captured at
`0xfffffe0008b69f98`–`0xfffffe0008b69fb4`: `x25 = AVE_PICMGMT_PARAMS*`,
`x26 = _S_AVE_FrameInfo*`, `x23 = cmd`, `x24 = slot`.

Reader: `CFlowControllerBase::ProcessCmd_Process_AVC` (`0x2adf0`).

### 4.1 Block map — confirmed, and it tiles the struct exactly

`bzero(cmd, 0x63D8)` at `0xfffffe0008b6a09c`, header stores at
`0xfffffe0008b6a0a0`–`0xfffffe0008b6a15c` (`+0x20` = the caller's slot, range
checked `<= 0x32` at `0xfffffe0008b6a074`), then three `memcpy`s:

| cmd off | len | source | call VA |
|---:|---:|---|---|
| `0x0040` | `0x08` | never written | — |
| `0x0048` | `0x954` | `x27` = either `_S_AVE_FrameInfo + 0x30` (`0xfffffe0008b6a184`+`88`) or `client + 0x417C` (`0xfffffe0008b6a2cc`) | `0xfffffe0008b6a53c` |
| `0x099C` | `0x924` | `*(x28 + 40)` where `x28 = *(chm->[0x20] + 0xF1728)` | `0xfffffe0008b6a54c` |
| `0x12C0` | `0x5118` | the caller's `AVE_PICMGMT_PARAMS*` | `0xfffffe0008b6a560` |

`0x48 + 0x954 = 0x99C`, `0x99C + 0x924 = 0x12C0`, `0x12C0 + 0x5118 = 0x63D8`.
The blocks tile the struct with no gaps.

Consistency check with §3: `client + 0x417C` and `client + 0x4AD0` are
`0x954` apart, and `Start` copies the *first* `0x154` bytes of that same
structure. So `client+0x417C` is a `0x954`-byte per-picture parameter block of
which `Start` sends a prefix and `Process` sends the whole thing, with
`_S_AVE_FrameInfo+0x30` as a per-frame override of the same layout.
**Inferred**, from the two sizes agreeing and the two sources feeding the same
destination.

### 4.2 What the firmware reads

With `x19 = this->[1472]` = the command (`0x2ae4c`), the complete set of loads
from the command in `ProcessCmd_Process_AVC` is:

| cmd off | width | use | VA |
|---:|---|---|---|
| `0x08` | u64 | `CNT`; passed as argument 7 of `SendCommandToQueue` | `0x2aff8`, `0x2b030`, `0x2b394`, `0x2b3f8`, `0x2b448` |
| `0x10` | u64 | `CID`; used for every `CAVEPriorityQueue` lookup — `0x3f840`, `CAVEPriorityQueue::…(0x400b8)`, `SetClientPriority`, `GetClientIndexFromID` (`0x3f44c`) | `0x2ae54`, `0x2ae68`, `0x2aee4`, `0x2aef4` |
| `0x20` | u32 | slot, argument of the Ack path | `0x2b034` |
| `0x24` | u32 | **priority** → `SetClientPriority` when it differs from the queue's | `0x2ae80`, `0x2ae9c`, `0x2aee8` |
| `0x4E20` | u32 | a flag; gates a branch that reads `client+1904` and `controller+9840` | `0x2b3b4`+`b8` |
| `0x6228` | u32/u64 | passed as argument 2 of `0x55730` with `w3 = 2`, and printed as `%p`; **inferred** to be the frame/picture identifier | `0x2aea8`, `0x2b22c`+`30`, `0x2b430` |

The handler then copies three sub-ranges of the command **to the same offsets**
in the queued command object `x21`:

```
2b340:  add x0, x21, x8 ; add x1, x19, x8 ; memmove(.., 0x20 or 0x28)
2b364:  mov w9, #0x2a20 ; ... memmove(x21+0x2A20, x19+0x2A20, count*8)
2b378:  mov w8, #0x54e8 ; ... memmove(x21+0x54E8, x19+0x54E8, 0xEF0)
```

so the firmware's internal per-frame structure is offset-identical to the wire
command over at least `0x2A20…` and `0x54E8…0x63D8`. Confirmed.

Dispatch out: `CFlowControllerBase::SendCommandToQueue(CAVECommonController*,
u32, void*, u32, u32, u64, u64, u32)` at `0x32800`, called `0x2b3b0` with
`w2 = 1`, `x7 = cmd[8]`.

### 4.3 What is *not* established for Process

The task asked specifically for the input surface, the output buffer, the frame
type and the QP. Honestly:

* **Input surface and output buffer**: not located. They are inside the
  `0x5118`-byte `AVE_PICMGMT_PARAMS` block at `cmd+0x12C0`, which is filled by
  `AVE_CHM_SetDataInfo_Frame` / `_Header` / `_RC` / `_FwBuf`
  (`0xfffffe0008b682bc`, `0xfffffe0008b68858`, `0xfffffe0008b69110`,
  `0xfffffe0008b71fd8`) — those were identified but not disassembled.
* **Frame type**: not located. The firmware's
  `CAVECommonController::GetFrameType(AVE_PICMGMT_PARAMS*, u32*, u16*, u16*,
  u32*, u32*, bool)` at firmware `0x7355c` is the entry point to follow — its
  first argument is the block at `cmd+0x12C0`.
* **Per-frame QP**: not located. `AVE_CHM_SetDataInfo_RC(chm, _S_AVE_CmdInfo*,
  _S_AVE_FrameInfo*, AVE_PICMGMT_PARAMS*)` at `0xfffffe0008b69110` is the
  function that writes it.

Do not guess these. They are one disassembly session away.

---

## 5. Structures named along the way

Recorded so the next pass does not have to re-derive them.

| structure | where | fields recovered |
|---|---|---|
| `_S_AVE_Client` | `chm->[0x18]` | `+0x1538` 0x300 alg/session block; `+0x16D8` `sEnc.sAlgCfg`; `+0x1838` `VideoParams` (0x2460); `+0x417C` per-picture params (0x954); `+0x4AD0` AVC SPS/PPS (0x6B4); `+0x5184` (0x180); `+0x5C0` a bool |
| `_S_AVE_Alg_Cfg` | client `+0x16D8` | `sComm` `+0x00`, `sRC` `+0x20`, `sGOP` `+0x98`, `sRef` `+0xD0`, `sQPMod` `+0xF8` |
| `_S_AVE_RC_Cfg` | AlgCfg `+0x20` | `Feature` u64 `+0`, `LookAheadFrameCount` `+8`, `RCMode` `+0xC`, `Bitrate` `+0x10`, `QP[3]` `+0x18/1C/20`, `RCQPRange[2]` `+0x70/74`; also `DRL`, `CRFScale`, `VBVMaxBitRate`, `VBVBufferSize`, `VBVInitialDelay` (offsets not read) |
| `_S_AVE_GOP_Cfg` | AlgCfg `+0x98` | `Feature` `+0`, `NumOfFrame[3]` `+8/C/10`, `NumOfGOPLayer` `+0x14`, `MaxKeyFrameInterval` `+0x18`, `StrictKeyFrameInterval` `+0x1C`, `MaxKeyFrameIntervalDuration` (double) `+0x20`, `StrictKeyFrameIntervalDuration` (double) `+0x28`, `NumOfTemporalLayer` `+0x30` |
| `_S_AVE_Ref_Cfg` | AlgCfg `+0xD0` | `Feature` `+0`, `ReferenceNum[3]` `+8/C/10`, `ReferenceGap[4]` `+0x18/1C/20/24` |
| `AVE_SessionSettings_UserKernel_Data` | `IO_Start` input (docs/13) | `VideoParams.ui32Width` `+2648`, `ui32Height` `+2652` (`0xfffffe0008b8f70c`) |
| `_S_AVE_EUInfo` | `AVE_MD::RetrieveEUInfo` | 0x28 bytes; `+0` a count/index (s32), entries of 8 bytes from `+4` |
| `_S_AVE_TimeOut` | header `+0x30` | 16 bytes, opaque |

Useful firmware symbols recovered for these paths:

```
0x0311a8  CFlowControllerBase::NotificationToHost(u16,u64,u64,int,u32,u32)
0x031cc0  CFlowControllerBase::CreateClient(u64,u32,u64,u32,u32)
0x031e44  CFlowControllerBase::ProcessCmd_Start(u64,u32,void*)
0x031b14  CFlowControllerBase::MapRemoteRegs()
0x032800  CFlowControllerBase::SendCommandToQueue(...)
0x03f44c  CAVEPriorityQueue::GetClientIndexFromID(u64)
0x03f660  CAVEPriorityQueue::RegisterClient(u64)
0x03fae8  CAVEPriorityQueue::CreateClient(...)
0x0401c8  CAVEPriorityQueue::SetClientPriority(u64,int)
0x0402e0  CAVEPriorityQueue::SetClientUseFirmwareRC(u64,bool)
0x03f1a4  CAVEClient::SetMCTFLowLatencyMode(bool)
0x03f1b4  CAVEClient::SetWorkMode(_E_AVE_WorkMode)
0x03f1c8  CAVEClient::SetUseFrameDrop(bool)
0x00f830  CRateControl::Check_RCMode(_E_AVE_RCMode)
0x0060c4  RateControl::CreateInstance(RateControlParameters const&,...)
0x0dc460  CChannelManager::DoorBellCadenceSet(unsigned)
0x0e6cec  PlatformIOPIPCManager::AddSharedMemory(u64,unsigned long)
0x0e7050  PlatformIOPIPCManager::MapSharedMemory(u8,u32)
0x0e75f0  AVE_KeyFrame::Init(u64,int,int,double,double,int)
0x0e7a64  AVE_KeyFrame::GetInterval()
```

and kext symbols:

```
0xfffffe0008b66ff8  AVE_CHM_MakeFwCmd_Start_AVC
0xfffffe0008b69f74  AVE_CHM_MakeFwCmd_Process_AVC
0xfffffe0008b6fc6c  AVE_CHM_SetFwBuf(_S_AVE_CHM*,_S_AVE_SurfaceSet*,
                                     _S_AVE_SurfaceInfoSet*,_S_AVE_Buf_Set*)
0xfffffe0008b8f6c8  AVE_Client_CheckCommonInfo
0xfffffe0008b7cc90  AVE_Client_Enc_PrintAVC
0xfffffe0008b7fea0  AVE_Client_Enc_PrintAll   (calls AVE_Alg_PrintCfg @ +0x21c)
0xfffffe0008cd3c28  AVE_Alg_PrintCfg
0xfffffe0008bff044  AVE_RC_PrintCfg
0xfffffe0008bfc590  AVE_GOP_PrintCfg
0xfffffe0008c035d0  AVE_Ref_PrintCfg
0xfffffe0008c02574  AVE_QPMod_PrintCfg
0xfffffe0008beab24  AVE_MD::RetrieveEUInfo(int,_S_AVE_EUInfo*)
0xfffffe0008c48c68  AVE_MCC::GetDSID(_E_AVE_MCC_DSType,unsigned*)
0xfffffe0008c6da54  AVE_Surface::GetDARTAddr(unsigned,int)
0xfffffe0008c6dbbc  AVE_Surface::GetSize()
```

## 6. Not determined

* `sCAveCmdConfig` `+0x4A` (written, never read) and `+0x50` (logged, never
  written by `MakeFwCmd_Config`).
* Which object `AVE_HwC::[32]` is, and hence what `[0]==3` and bit 17 of `[40]`
  mean for Config `+0x48`/`+0x4A`.
* `sCAveCmdAvcStart`: everything in `0x68..0x207` (the part of the first block
  before `sAlgCfg`), `0x24C..0x297` and `0x2A0`'s neighbours (the DRL / CRF /
  VBV tail of `sRC`), `0x300..0x367` (`sQPMod`), the bulk of `VideoParams`
  (`0x370..0x27C7` apart from the four fields listed), `0x27C8..0x291B`, and
  the AVC SPS/PPS block apart from profile/level/lossless/max_num_ref_frames.
  That is roughly 11.5 KB of the 12.6 KB struct.
* The numeric value of `_E_AVE_RCMode` for constant QP is **inferred** as 3,
  not read. `Check_RCMode` only tells you 20 and 100 are rejected.
* `sCAveCmdAvcProcess`: the input surface, output buffer, frame type and
  per-frame QP — see §4.3 for the four functions to disassemble next.
* `sCAveCmdHevcStart` (0x13F28), `sCAveCmdHevcProcess` (0xB1C0) and
  `sCAveCmdReset` (0x13F08) — untouched. `ProcessCmd_Start` shares its body
  between AVC and HEVC and takes the HEVC path at `0x32088`, which reads a
  different tail layout; that is the entry point for the HEVC work.
* `Close`, `Stop`, `Complete`, `Flush`, `Halt`, `Priority` — all `0x48`, all
  presumed to be header-only like `Open`, but only `Open` was actually
  verified on both sides.
