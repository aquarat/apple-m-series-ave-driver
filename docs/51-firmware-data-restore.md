# Restoring the firmware's DATA segment before every start

The AVE firmware boots and completes the full macOS 13.5 handshake — but only on
the **first** start after a reboot ([31](31-bringup-state.md), 2026-09-13 19:25 /
19:36). A second start in the same boot is silent: no message 1, no page of the
16 MiB window changes. The first run modifies 19 pages of the firmware's `__DATA`
segment in place, and the second start runs on that dirtied data.

macOS never hits this, because it copies a pristine `__DATA` back over the
physical segment before **every** start. This document records exactly what 13.5
does, how the pristine bytes are reconstructed offline, what the driver does with
them, and how the first write Linux has ever made to that DRAM is isolated.

Every claim is **confirmed** (read out of a binary, a dump, or command output
cited here), **inferred** (chain stated), or **unknown**, per
[00-methodology.md](00-methodology.md).

---

## 0. Summary

| Question | Answer | Confidence |
|---|---|---|
| What does 13.5 restore? | The whole `__DATA` vmsize, `0x134000` bytes, bss included | Confirmed |
| From where? | A kalloc'd snapshot taken **once**, in `AVE_Firmware::Init`, before the core has ever run in that boot | Confirmed |
| Is `__TEXT` ever rewritten? | No | Confirmed |
| When? | First call in `AVE_HwC::StartUpIOP`, before the IPC allocation, the scratch writes, `IOP::Config` and `IOP::Start` | Confirmed |
| Can we reconstruct that snapshot offline? | Yes: the pre-boot dump's DATA window is exactly it | Confirmed |
| Does the stale per-boot stack guard matter? | No (§2.3) | Inferred |
| Has the write been performed on hardware? | **No.** Nothing below has run | — |

---

## 1. What macOS 13.5 does

All VAs are in the 13.5 `AppleAVE2` kext (`AVE_MACOS=13.5 python3 tools/disas.py
--kext --addr <va>`). 26.6.2's `AVE_FwImg::UpdateImage` → `RestoreCTRRData`
([42](42-asc-firmware-ownership.md) §4–5) was **not** assumed; this was read on
13.5, where the class is `AVE_Firmware` and there is no `AVE_FwImg`
([43](43-macos-13.5-firmware.md) §5).

### 1.1 The call chain

```
AVE_HwC::StartUpIOP        0xfffffe0008f119e0
  0xfffffe0008f11ae4  bl AVE_Firmware::UpdateImage      <- first call in the function
AVE_Firmware::UpdateImage  0xfffffe0008ef7670
  0xfffffe0008ef774c  ldrb w8, [x19]        ; the iBoot-loaded flag, AVE_Firmware+0
  0xfffffe0008ef7758  bl  RestoreCTRRData   ; iBoot-loaded path, returns 0
  0xfffffe0008ef7768  bl  UpdateBufImage    ; the other path (host-loaded image)
AVE_Firmware::RestoreCTRRData  0xfffffe0008ef5fe4
  0xfffffe0008ef60b0  ldr x0, [x19, #120]   ; destination: kernel VA of physical DATA
  0xfffffe0008ef60b8  ldr x1, [x19, #112]   ; source: the snapshot
  0xfffffe0008ef60c0  ldr x2, [x19, #72]    ; length: segment 1 size
  0xfffffe0008ef60c4  bl  0xfffffe00083e4a30 ; memcpy
```

**Confirmed.** The two `cbz`s before the copy mean a missing snapshot silently
skips the restore; there is no error path.

### 1.2 Where the three operands come from

`AVE_Firmware::RetrieveInfo` (`0xfffffe0008ef4968`) parses the ADT
`segment-ranges` into an array at `AVE_Firmware+0x30`, 16 bytes per entry,
`{phys, size}` (`0xfffffe0008ef4d30`–`4d40`), count at `+80`; for two segments it
requires both sizes non-zero and their sum ≤ `0x400000` (`0xfffffe0008ef4d5c`–
`4d74`). So `+48/+56` are TEXT's phys/size and `+64/+72` are DATA's.
**Confirmed.**

`AVE_Firmware::InitCTRRImage` (`0xfffffe0008ef5124`) builds one
`IOMemoryDescriptor` over both physical ranges (`0xfffffe0008ef5224`–`523c`,
options `0x23`), `prepare`s it, and maps it into the DART. `AcquireCTRRData`
(`0xfffffe0008ef5b68`) then maps it into the kernel and computes

```
0xfffffe0008ef5cb0  ldr x8, [x19, #56]     ; TEXT size = 0xec000
0xfffffe0008ef5cb4  add x8, x8, x0         ; + the mapping's base VA
0xfffffe0008ef5cb8  str x8, [x19, #120]    ; = kernel VA of DATA
0xfffffe0008ef5ccc  ... kalloc([x19,#72])  ; snapshot buffer -> +112
0xfffffe0008ef5ce8  bl  memcpy(snapshot, DATA, [x19,#72])
```

**Confirmed.** So the restore length is DATA's **vmsize**, `0x134000` — not the
file-backed `0x64000` — and the destination is the physical carve-out, reached
through a kernel mapping, not through the DART.

### 1.3 When the snapshot is taken

`AcquireCTRRData` has exactly one caller: `AVE_Firmware::Init`, at
`0xfffffe0008ef7404` (scan of the whole kext `__TEXT_EXEC` for `bl` to it).
`Init` reaches it only on the iBoot-loaded path (`ldrb w8,[x19]` at
`0xfffffe0008ef6ffc`). **Confirmed.** That is driver-attach time, before any
`StartUpIOP`, so the snapshot is DATA as iBoot left it — **inferred** only in
that nothing else in the kext is shown to have started the core before Init, and
`UpdateImage` is what starts every subsequent one.

In firmware VA terms: TEXT `0x0 + 0xec000`, DATA `0xec000 + 0x134000`
([43](43-macos-13.5-firmware.md) §4); physically TEXT `0x10000b28000`, DATA
`0x10001a90000`. The restore writes `[0x10001a90000, +0x134000)` and nothing
else.

---

## 2. The blob

`tools/make_ave_data_blob.py` → `data/blobs/ave-13.5-data-pristine.bin`,
`0x134000` bytes, **sha256
`f1af1ef42be04a7d607b0bc1a27dfda57268b476fecf1d92c8c13c760c71a103`**.
Apple-derived: it lives in `data/blobs/` (gitignored) and is never committed.

### 2.1 Provenance

| DATA range | Source | Why |
|---|---|---|
| `0x0 – 0x64000` | `data/blobs/iboot-window-16m.bin` at dump offset `0xf68000` | the file-backed part **as iBoot filled it in** — this is what macOS snapshots |
| `0x64000 – 0x98000` | dump, verified all-zero | bss the firmware initialises itself |
| `0x98000 – 0x134000` | zero fill | beyond the dump; `__zerofill`, and everything the dump does cover is zero — **inferred** |

The dump, not the image, is the source: the 13.5 Mach-O's `__DATA` lacks the 147
bytes iBoot writes (`STKG`, `SOC_`, `SOCR`, `CpAd`, `WrAd`, `IOBA`, and the
tunables region), and a firmware restored from the file alone would run with
`IOBA = 0` — the exact symptom [40](40-firmware-io-base.md) chased. The image is
still used, as a check: the tool builds the blob **both** ways (dump as it
stands, and image + only the iBoot bytes lifted from the dump) and refuses to
emit anything unless the two constructions are byte-identical. They are.

Independent cross-check: `tools/extract_pristine_data.py` (written separately,
from the dump's first `0x98000` bytes zero-padded) produces a **byte-identical**
blob — same sha256. **Confirmed.**

### 2.2 Verification output

```
$ python3 tools/make_ave_data_blob.py
image vs dump over 0x64000 file-backed bytes: 147 bytes differ in 70 run(s)
   DATA+0x03a38..0x03a40 (VA 0xefa38)    7 byte(s) filled  STKG stack guard
   DATA+0x03ba3..0x03be9 (VA 0xefba3)   16 byte(s) filled  SOC_/SOCR/CpAd/WrAd/IOBA
   DATA+0x6036b..0x60505 (VA 0x14c36b)  124 byte(s) filled  _rtk_tunables values
   no byte outside the known iBoot ranges differs - the dump IS this image's memory
dump covers DATA+0x0..0x98000 of 0x134000; beyond the file-backed part: 0 non-zero byte(s)
tag list in the dump at DATA+0x3a30 (VA 0xefa30, physical 0x10001a93a30):
   SOC_ 0x6001 / SOCR 0x11 / CpAd 0x40d800000 / WrAd 0x40dc00000 / IOBA 0x40c000000  as expected
   STKG 0x816ea533007323bc
both constructions (dump-as-is, image+iBoot-bytes) agree byte for byte

blob vs pre-boot dump over 0x98000 covered bytes: 0 byte(s) differ          PASS
blob vs 13.5 image __DATA: 147 byte(s) differ in 70 run(s)                  PASS
   tail DATA+0x64000..0x134000: all zero                                    PASS

negative controls - each must fail the sha256 gate and score < 99 % over __const+__data:
   candidate                sha256      __const+__data    whole __DATA
   26.6.2 H13C              differs     8071/14896 =  54.18 %   98.66 %  OK (rejected)
   13.5 H13D (M1 Ultra)     differs    13840/14896 =  92.91 %   99.71 %  OK (rejected)
   13.5 H13S (M1 Pro)       differs    11309/14896 =  75.92 %   63.74 %  OK (rejected)
   13.5 H13G (M1)           differs    11309/14896 =  75.92 %   62.05 %  OK (rejected)
   13.5 H13C __TEXT         differs    10035/14896 =  67.37 %    8.73 %  OK (rejected)
```

**Read the control table carefully** ([trap 2](00-methodology.md)): whole-`__DATA`
byte identity is a **weak** discriminator — the M1 Ultra sibling H13D scores
99.7 % against this blob, because most of the file-backed part is the
build-independent `RTKSTACK` filler plus zero-filled `_rtk_boot` /
`_rtk_page_tables`. Only `__const + __data` (`DATA+0..0x3a30`) discriminates, and
even there the sibling reaches 92.9 %. The blob is therefore *not* identified by
its own content: it is pinned by an exact sha256, and the image it belongs to is
identified by `__TEXT` (100 % vs ≤ 22.5 % for every sibling,
[43](43-macos-13.5-firmware.md) §3.2), which is what the driver checks before
writing anything.

`--verify` re-runs every check against an existing blob and writes nothing;
`--c-constants` prints the sha256 and the `__TEXT` window hashes in the form
`driver/ave_fw.c` compiles in, so they can be regenerated rather than trusted.

### 2.3 The stack guard is stale on purpose

`STKG` (DATA+`0x3a38`, 8 bytes) is random per boot: this blob carries
`0x816ea533007323bc`, the value from the boot the dump was taken in, and restoring
it into a later boot overwrites that boot's value. That is deliberate:

- the firmware reads the cookie out of this same tag list at run time and
  installs it itself, so the only requirement is that it is well-formed; there is
  no second copy anywhere that has to agree with it (**inferred** from the tag
  list being the sole carrier — the firmware's use of `STKG` was not traced);
- macOS's snapshot holds the current boot's value only as an accident of being
  captured in the same boot, not because anything checks it;
- the security value of a fresh guard is irrelevant on a machine whose DRAM we
  are writing from the kernel anyway.

The other iBoot-filled tags are SoC constants and MMIO bases (`SOC_ 0x6001`,
`SOCR 0x11`, `CpAd 0x40d800000`, `WrAd 0x40dc00000`, `IOBA 0x40c000000`), not
per-boot allocations, so a stale copy is the same copy. **Inferred** — they have
been observed in exactly one boot. The driver logs the live and blob `STKG` side
by side on every restore, so a boot where any of this is wrong is visible in the
log rather than silent.

---

## 3. What the driver does

`driver/ave_fw.c`, `ave_fw_restore_data()`, module parameter `fw_restore_data`:

| value | behaviour |
|---|---|
| `0` | **default** — no-op, nothing is read and nothing is written |
| `1` | restore: verify, copy, flush, read back, verify again |
| `2` | **dry run** — everything except the copy: reports how far DATA has drifted and writes nothing |

`fw_restore_path=` overrides the blob path (default
`apple/ave-13.5-data-pristine.bin`).

Sequence, in order, each step refusing with an error that fails the probe before
the core is started:

1. **ABI gate** — `ave->fw_abi` must be `AVE_ABI_MACOS_13_5`. The blob is that
   image's DATA; under any other ABI it would put one firmware's data under
   another firmware's code.
2. **Core-not-running interlock** — `CPU_CONTROL`'s RUN bit clear and
   `CPU_STATUS` STOPPED. The block is powered at stage 6 and the core is released
   at stage 13; this call belongs in that window. A call after the start is
   refused (`-EBUSY`), which is also what would happen if the call were
   mis-wired.
3. **Blob gate** — `request_firmware()`, size exactly `0x134000`, sha256 equal to
   the compiled-in digest. A wrong file cannot be "close enough".
4. **Placement** — `ave_fw_check_iboot_placement()`: the ASC firmware-base
   register's base field is `0x10000b28000` and `TEXT+0x423c` holds
   `0x1f0000ec000` (the iBoot-written DATA base literal, [44](44-reset-fetch-path.md)
   §2.4).
5. **Image identity** — three 16 KiB windows of physical `__TEXT`
   (`+0x0`, `+0x80000`, `+0xe8000`) hashed against the 13.5 image. All three are
   byte-identical between the image and the pre-boot dump (the only four bytes
   iBoot changes in TEXT are at `0x423c`, checked separately in step 4).
6. **Destination safety** — the same refusal used by the DART mapping path:
   `region_intersects()` reports the range disjoint from System RAM, and no
   `/memory` `reg` or `/reserved-memory` child (static or dynamic) overlaps it.
7. **Evidence before the write** — the live DATA is compared with the blob and
   the number of differing bytes, differing 4 KiB pages and the first differing
   offset are logged, plus live vs blob `STKG`. **0 differing bytes on a fresh
   boot; non-zero once the firmware has run.**
8. **The write** — an `ave_step()` marker naming it as the first write to this
   DRAM, then one `memcpy()` of exactly `0x134000` bytes into a `memremap()` of
   exactly `[0x10001a90000, +0x134000)`. Nothing outside that mapping is
   writable by this code.
9. **Flush and verify** — `arch_wb_cache_pmem()` cleans the range to the point of
   coherency so the core fetches DRAM rather than our cache, then
   `arch_invalidate_pmem()` so the read-back reads DRAM rather than the lines we
   just wrote, then every byte is compared again. Any mismatch returns `-EIO`
   and the probe fails **before** the ASC start, leaving the core halted.
10. **Re-baseline** — `ave_fw_snapshot_phys()` is re-run if a snapshot had
    already been taken, so the "pages changed" liveness diff at stage 15 counts
    only what the *firmware* wrote, not what the restore wrote (§5).

Everything before step 8 is read-only, so `fw_restore_data=2` exercises the
entire apparatus — blob, hashes, refusals, drift count — without writing a byte.

---

## 4. The risk, and how the procedure isolates it

`0x10001a90000` is DRAM that Linux does not own and that no Linux code has ever
written. Reading it is established practice in this driver (the liveness snapshot
`memremap()`s 16 MiB of it on every probe and has never faulted); **writing it is
new**. What is known and what is not:

- macOS writes exactly this range, the same way, before every start
  (§1). **Confirmed** — so the memory is writable by the AP, and the carve-out is
  not read-only to the kernel on macOS.
- Whether Apple's write goes through some path with different attributes
  (the `IOMemoryDescriptor` options word `0x23`, or a PPL/monitor-mediated
  mapping) is **unknown**. If the range were AP-write-protected on a non-macOS
  boot — CTRR covers `__TEXT`, and "CTRR" is in the name of the routine — the
  write would fault or be silently dropped rather than corrupt anything else.
- The blast radius is bounded by the `memremap()` extent. There is no computed
  address here: both the base and the length are compile-time constants that
  three independent landmarks agree on ([43](43-macos-13.5-firmware.md) §4.1),
  and the range is refused if Linux claims any of it.

The procedure isolates the first write in three ways:

1. **Dry run first.** `fw_restore_data=2` runs every check and the drift count
   and writes nothing. If the blob, the hashes or the placement are wrong, that
   is found with no write at all.
2. **A first real write whose content is a no-op.** On a fresh boot the drift
   count is 0, so the `memcpy` writes the bytes that are already there. If the
   machine survives it, the write path is proven without changing the firmware's
   state; if it does not, the `ave_step()` marker immediately before it names the
   operation ([00-methodology.md](00-methodology.md), "record what ran").
3. **A read-back that cannot be satisfied by our own cache**, so "the copy
   succeeded" is a statement about DRAM, not about the CPU's store buffer — and a
   failure refuses before the core is started.

---

## 5. Where the call belongs, and how to install the blob

macOS's order inside `StartUpIOP` is: **UpdateImage → IPC ctor/Init → scratch
writes (`SetIOPFlag`, `WriteScratch 1/2`) → `IOP::Config` → `IOP::Start`**
([45](45-abi-13.5-boot-ipc.md) §2.1). Mapped onto `ave_probe_stages()`:

| macOS step | our stage |
|---|---|
| `AVE_Firmware::Init` → `InitCTRRImage` (DART map of both segments) | stage 10, `ave_fw_load()` → `ave_fw_map_iboot()` (`fw_map_data=1`) |
| `UpdateImage` → `RestoreCTRRData` | **here: after stage 10, before stage 11** |
| `AVE_IPC::Init` | stage 9 in our order (earlier; it touches nothing in DATA) |
| `SetIOPFlag(0)`, `WriteScratch(1/2)` | stage 11, `ave_boot_config()` |
| `IOP::Config` | stage 12 (skipped on 13.5) |
| `IOP::Start` | stage 13 |

So the call belongs **after the stage-10 DART DATA mapping and before the
stage-11 scratch writes** — mirroring macOS, which has the mapping in place from
`Init` and restores on top of it. It must precede stage 13 in any case.

`ave_drv.c` currently calls it at the head of stage 13 (concurrent work). That is
functionally equivalent — nothing between stages 11 and 13 writes DATA — with one
wrinkle: `ave_fw_snapshot_phys()` runs *before* stage 13, so a restore there
dirties pages the liveness diff would then attribute to the firmware.
`ave_fw_restore_data()` compensates by re-taking the snapshot itself after a
successful restore, so either call site gives correct evidence. Moving the call
to between stages 10 and 11 removes the need for that compensation:

```c
	if (ave_stage(dev, AVE_STAGE_FW_LOAD)) {
		ret = ave_fw_load(ave);
		...
	}
	/* macOS restores DATA here, before the scratch writes (docs/51 §5). */
	ret = ave_fw_restore_data(ave);
	if (ret)
		return dev_err_probe(dev, ret, "DATA restore\n");
	if (ave_stage(dev, AVE_STAGE_BOOT_CFG)) {
```

No `Makefile` change is needed: `ave_fw.c` is already built, and the blob is
loaded with `request_firmware()`, not linked in. The operator installs it once:

```sh
python3 tools/make_ave_data_blob.py            # builds + verifies, prints the sha256
sudo install -d -m755 /lib/firmware/apple
sudo install -m644 data/blobs/ave-13.5-data-pristine.bin /lib/firmware/apple/
sha256sum /lib/firmware/apple/ave-13.5-data-pristine.bin
# f1af1ef42be04a7d607b0bc1a27dfda57268b476fecf1d92c8c13c760c71a103
```

(The identical blob from `tools/extract_pristine_data.py` also works; install it
under this name, or pass `fw_restore_path=apple/ave-data-pristine.bin`.)

---

## 6. Operator test sequence

Prerequisites, unchanged from the run that worked: patched m1n1 programming the
AVE DAPF ([50](50-m1n1-ave-dapf.md)), overlay `variant=3`, and the driver
parameters `stop_after=16 fw_map_data=1 fw_map_text=2`. **The operator runs
these; an agent must not** ([AGENTS.md](../AGENTS.md)).

**(0) Dry run, fresh boot.** `fw_restore_data=2`. Expect: all gates pass, "DATA
before restore vs pristine: 0 bytes differ", "DRY RUN - nothing written", and the
handshake completing exactly as it does today. Nothing has been written, so this
costs nothing but a boot.

**(a) First write, fresh boot.** Reboot, then load with `fw_restore_data=1` as
the **first** firmware start of the boot. Expect:

- `restore: DATA before restore vs pristine: 0 bytes differ over 0x134000` — the
  write is a no-op in content;
- `restored 0x134000 bytes over phys 0x10001a90000 (0 had drifted), read-back
  verified`;
- then the usual `msg 1: nch=7 chanmem=0x9bc0 ...` through
  `macOS 13.5 handshake complete: 7 channel(s)`.

A 0 here is the point of the step: it proves the write path without changing what
the firmware sees. If the count is not 0 on a genuinely fresh boot, stop — the
blob or the machine's iBoot differs from what this document assumes.

**(b) Second start, same boot.** Without rebooting: `rmmod apple-ave`, then load
again with `fw_restore_data=1`. Expect:

- `DATA before restore vs pristine: N bytes differ in ~19 page(s), first at
  DATA+0x2000` (the first changed page in the successful run was physical
  `0x10001a92000` = DATA+`0x2000`) — the drift the first start left;
- `restored ... (N had drifted), read-back verified`;
- **and the handshake completing a second time in the same boot**, which is the
  result this whole exercise exists to produce.

If (b) restores cleanly but the firmware is still silent, the DATA drift was not
the whole story and the next suspect is state outside DATA (the ASC's own
registers, or the `0x1f0` window mapping) — record it in
[31](31-bringup-state.md) rather than iterating on hardware.

---

## 7. Status and open questions

- Nothing in §3–§6 has run. The code compiles clean (`W=1`, no new warnings) and
  is off by default. **Unknown** until the operator runs it: whether Linux can
  write this DRAM at all.
- **Unknown:** whether DATA drift is the *only* reason a second start is silent.
  (b) is the discriminating experiment.
- **Unknown:** whether the bytes beyond `0x98000` (not covered by the dump) are
  really zero at boot. If the firmware's own bss initialisation covers them, it
  does not matter; if it does not, the restore would be writing zeros where iBoot
  wrote something. A 16 MiB dump starting at DATA rather than TEXT would settle
  it, read-only.
- **Inferred, not confirmed:** that `AVE_Firmware::Init` always runs before any
  start on macOS, i.e. that Apple's snapshot is genuinely pristine. The
  reconstruction here does not depend on it — the blob comes from a dump taken
  before any start — but the claim "macOS restores pristine DATA" does.
- The per-boot `STKG` discussion is §2.3; if a future run shows the firmware
  faulting in a way consistent with a stack-guard mismatch, the cheap test is to
  preserve the live 8 bytes at DATA+`0x3a38` across the restore instead of
  overwriting them.
