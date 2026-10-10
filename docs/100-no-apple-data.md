# 100. No Apple data in the driver: install-time generation and capture

Status 2026-10-10: **implemented, compiled (`W=1`, no warnings), the
generator checked byte for byte; nothing here has run on hardware.** The
runs are in §6.

The driver source must carry no data taken from Apple's binaries, so that
a package built from it can be distributed (Fedora COPR). What the driver
needs from Apple's files is made on the user's machine, from the user's
copy, and loaded at run time with `request_firmware()`; the DKMS build
needs no Apple data at all.

## 1. Inventory

Every Apple-derived item in `driver/` and `test/` (the packaged overlay),
classified: **(a)** a data table or blob, which must move to install-time
generation; **(b)** an interface fact the driver needs to talk to the
hardware (register offsets, bit layouts, command formats), which stays;
**(c)** unclear, with the reasoning.

| item | where | class | why, and what was done |
|---|---|---|---|
| AVE_DPE tunables: `Castor_6000` (1 + 124 + 123 entries), `Acis_8103` (0 + 1 + 39), `Atlas_8112` (1 + 124 + 123), each `{offset, clear, set}` | `driver/ave_dpe_tables.h` (removed) | (a) | Register values copied out of AppleAVE2.kext's `gs_sAVE_DPE_CfgSet_*`. Now generated from the user's 13.5 kernelcache by `tools/ave_fwgen.py` into `apple/ave-13.5-dpe-<set>.bin` and loaded at probe (§2). |
| iBoot's ASC tunables and fill values inside the pristine DATA blob | the blob files; `tools/data_blob_from_image.py` (research tool, not packaged) | (a) | Values iBoot writes into the firmware's DATA (21-23 `{offset, mask, value}` per SoC). Not in the driver. The blob now comes from the machine itself (§3); t8103 uses the image's own empty table. |
| `STKG` values | `tools/data_blob_from_image.py`; the t8103 blob recipe | (c) | An 8-byte stack-guard cookie iBoot picks at random each boot, recorded from one dump. It holds no Apple content and any value works (docs/51 §2.3). Kept only in t8103's built blob, so that the blob stays byte-identical to the one that was tested. |
| pinned sha256 of the reference pristine dumps; sha256 of three TEXT windows per image | `driver/ave_soc.c` | (c) → kept | Digests: they identify a file and contain none of its bytes, like the packaging's `known-hashes.txt`. The pristine pins are no longer required (§3); the TEXT windows still prove the image in DRAM is the 13.5 build. |
| `AVE_DevInfo` row per SoC (DevID, DevType, ChipType) | `driver/ave_soc.c` | (b) | Three small identifiers the firmware checks in the boot handshake; protocol values, not a table copied whole. |
| MMIO, DART, DAPF and PMP addresses; interrupts; stream IDs; power-domain paths; the overlay's nodes | `driver/ave_soc.c`, `driver/ave_hw.h`, `test/*.dts`, `test/ave_overlay_mod.c` | (b) | A description of the hardware, read from the ADT, as Asahi's own device trees are. |
| iBoot placement (TEXT/DATA physical addresses) | `driver/ave_soc.c` | (b) | Addresses, and since docs/99 read from the live ADT at run time. |
| firmware layout facts: segment sizes, the DATA literal at TEXT+0x423c, the STKG offset, the patchbay tag names | `driver/ave_soc.c`, `driver/ave_fw.c` | (b) | Where to find things in the user's own image, to check it. |
| command ids, reply ids, sizes, priorities, slot numbers, struct field offsets, endpoint and channel numbers, bit layouts | `driver/ave_abi.h`, `ave_abi_boot.h`, `ave_cmd.c/.h`, `ave_ipc.c`, `ave_session.c` | (b) | The wire protocol: what any reverse-engineered driver has to state to interoperate. Each value has its meaning and source cited beside it. |
| macOS's values for 43 Start_AVC and 9 per-frame fields (`ave_macos_*_13_5`) | `driver/ave_abi.h` | (c), leaning (b) | Settings of individual protocol fields (flags, "auto" = 0xffffffff, a 0xcd fill, 1.0f), each named and explained, not a block copied whole. They configure the interface the way macOS does. Flagged for the owner's judgement. |
| the per-QP λ table `lam[52]` | `driver/ave_cmd.c` | (b) | Equals max(1, round(2^((QP-12)/6))), the standard √λ curve: arithmetic, not data. |
| H.264 `level_idc` lists, HEVC `general_level_idc` and MaxLumaPs | `ave_cmd.c`, `ave_session.c`, `ave_v4l2.c` | (b) | ITU-T H.264 Annex A and H.265 Table A.8. |
| buffer-size formulas (`AVE_CalcBufSize*`) | `ave_session.c`, `ave_cmd.h` | (b) | Formulas the firmware enforces; interop facts. |
| register address lists for diagnostics (`ch[]`, `ie[]`, `rl[]`, `mcpu[]`) | `ave_session.c` | (b) | Register addresses. |
| strings `"AppleAVE2FW/Firmware"`, `"CmdProcessor"` | `ave_fw.c` (`probe_diag` only) | (c) | Two names used to find landmarks in diagnostics; not data in any useful sense. |
| symbol tables (`kext-symbols`, `symbols`, `types`, `commands`, ...) | `data/derived/` (repository, never packaged) | (a) | Removed from the tree by the licensing merge (b5ef678); the tools write them to `data/blobs/derived/`. |

What the public history holds: `driver/ave_dpe_tables.h` from 0ef449f
(Castor_6000), 916938a (Acis_8103) and becb4d0 (Atlas_8112) until this
change, and the eight `data/derived/` symbol tables until b5ef678. No
firmware image, IPSW member or RAM dump was ever committed (`*.bin`,
`*.im4p` are ignored). The history is not rewritten here.

## 2. The AVE_DPE tunables, from the user's kernelcache

`tools/ave_fwgen.py dpe KERNELCACHE --set <set>`:

1. the kernelcache: `kernelcache.release.mac13j` (t600x), `.mac13g`
   (t8103) or `.mac14g` (t8112) of the macOS 13.5 IPSW, or the copy the
   Asahi installer put on the EFI partition (`asahi/kernelcache.release.*`).
   An IM4P, LZFSE-compressed: decompressed by Python's `lzfse` module or
   the `lzfse` command (Apple's reference code, BSD-3-Clause);
2. the MH_FILESET entry `com.apple.driver.AppleAVE2`, its own symbol
   table, and the one symbol ending `gs_sAVE_DPE_CfgSet_<set>`;
3. the CfgSet: four words (the CAT window must start at 0xDC000 and the
   CAC window at 0xDC400, the driver's bases, or the tool refuses), then
   six `{pointer, count}` pairs; slots 0, 3 and 4 are CAT Default, CAC
   Default and CAC 8-bit (docs/58 §5.1). Pointers are kernel-cache
   chained fixups (30-bit offset from the cache base, cache level 0);
4. each entry `{u32 offset, u32 width, u32 clear, u32 set}`; the width
   must be 4 and the offset a word inside the 0x400-byte window;
5. written as `"AVEDPE01"`, three counts and a zero, then `{offset,
   clear, set}` per entry, little-endian.

The driver (`ave_dpe_load`, `driver/ave_drv.c`) loads `soc->dpe_fw` once,
checks the same shape, and applies it as before; a missing or malformed
file fails the probe with a message naming the fetch tool.

**Identity check.** The three sets were generated from each of the three
13.5 kernelcaches (mac13j, mac13g, mac14g) and compared with the tables in
`driver/ave_dpe_tables.h` as of 6efeb76, encoded the same way: all nine are
byte-identical to the header (sha256 Castor_6000 `4995da8c…`, Acis_8103
`6fea4436…`, Atlas_8112 `43bef746…`). The AppleAVE2 kext is the same in
every 13.5 kernelcache, so the machine's own kernelcache always suffices.

## 3. The pristine DATA blob, from the machine itself

The blob is DATA as iBoot left it: the image's `__DATA` plus iBoot's
fills. The fills (tunables especially) come only from iBoot, whose images
are encrypted, so no file the user has holds them. The running machine
does, in iBoot's DATA before the first start of each boot.

`ave_fw_capture_cold()` (`driver/ave_fw.c`), at stage 13, before the core
starts and before any restore, where the core runs on iBoot's DATA in
place:

- copies DATA through a cacheable mapping (memory Linux does not own; no
  register is touched);
- keeps the copy only if `ave_fw_blob_by_image()` accepts it: every byte
  outside iBoot's fill set equal to the user's image file, the tunables
  table well formed, `CpAd`/`WrAd`/`IOBA` this encoder's banks and `SOC_`
  the SoC. A core that has run rewrites ~300 000 bytes (docs/84 §4), so a
  second load in a boot is never taken for a cold one;
- uses it as the pristine DATA for the module's life (hang recovery,
  resume), and exposes it read-only as
  `/sys/kernel/debug/apple_ave[N]_iboot_data` (`fw_capture=0` turns it
  off).

`apple-ave-fetch-firmware --capture` (run by `ave-load` after a load)
checks the copy the same way in Python and installs it as the SoC's
pristine blob, for reloads in later boots. Those blobs carry the STKG of
the boot they were taken in; a stale STKG is harmless (docs/51 §2.3,
seen on t6000 and t6001).

The driver no longer requires the reference dump: a blob whose sha256 is
not the pinned one is checked against the image, in place and owned
alike (it was owned only before). t6001's ave1 builds its DATA from ave0's
blob, so its row names the tags that blob carries (`blob_tag_*`). Rows
whose DATA is in place now carry their tags (t6000, t6001, t8112, t6002's
ave0).

Per SoC:

| SoC | pristine blob |
|---|---|
| t6000, t8112, t6002 (all four) | captured at the first start of a boot; not needed for that boot's first load |
| t6001 | captured from ave0; ave1 (driver-owned DATA) needs it, so ave1 comes up from the boot after the capture |
| t8103 | iBoot's DATA lies in Linux's RAM on stock m1n1 (docs/89 §6), so nothing can be captured; built from the image with the empty tunables table, as before (`544d57d7…`) |

**Identity check, offline.** `check_pristine` (Python) accepts the t6001
reference dump as a capture of ave0 and refuses it for ave1's own tags,
another SoC, and a copy with one byte changed outside the fill set. On
hardware: §6, c1.

## 4. What the driver build needs

Nothing from Apple. `make -C driver` builds without any generated header;
the module asks for `apple/ave_<variant>.bin`,
`apple/ave-13.5-dpe-<set>.bin` and (for reloads) the pristine blob at run
time, all made by the fetch tool on the user's machine.

## 5. Unchanged

The firmware image itself was already a run-time file. The overlay has no
Apple data beyond the hardware description.

## 6. Hardware runs needed (docs/53)

On the M1 Max, after docs/99's d1-d3 or together with them, each from a
fresh boot:

- **c1** the first load with the generated DPE file and no pristine blob
  installed: `dpe: apple/ave-13.5-dpe-castor_6000.bin ... read back OK`;
  `capture: this boot's cold DATA kept`; the captured copy equals the
  reference dump `ave-13.5-data-pristine.bin` in every byte but STKG
  (compare on the host); 720p H.264 at the known PSNR. A "no": any read-back
  error, no capture, or any other byte different.
- **c2** = c1 (repeat).
- **c3** after `--capture` installed the copy: unload and reload in the
  same boot; the reload restores from the captured blob and encodes
  identically (docs/84 R5/R6 with this boot's own blob).
- **c4** a forced hang and recovery (docs/84 R7) restores from the
  in-memory capture without any file.
