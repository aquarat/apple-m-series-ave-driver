# The AVE firmware and kext that actually run: macOS 13.5 (22G74)

Every static finding in docs 01–42 was read out of **macOS 26.6.2 (25G83)**
blobs. iBoot on this machine loads coprocessor firmware from the **macOS 13.5**
stub (`/proc/device-tree/chosen/asahi,os-fw-version` = `13.5`). This document
records where the matching 13.5 firmware and `AppleAVE2` kext came from, and
the evidence that they are byte-for-byte what is in DRAM.

Every claim is marked **confirmed** (read out of a file, a dump, or a command
output cited here), **inferred** (chain stated), or **unknown**, per
[00-methodology.md](00-methodology.md).

---

## 0. Summary

| Question | Answer | Confidence |
|---|---|---|
| Where is the 13.5 AVE firmware? | The Asahi stub container (`nvme0n1p3`), Preboot volume, `Restore/Firmware/ave/AppleAVE2FW_H13C.im4p` | Confirmed |
| Where is the 13.5 kernelcache? | Same volume, `Restore/kernelcache.release.mac13j` (also as the personalized boot kernelcache) | Confirmed |
| Is it the IPSW's copy? | Yes. Both files are SHA-256-identical to the members of `UniversalMac_13.5_22G74_Restore.ipsw`, and SHA-384-identical to the digests in the stub's `BuildManifest.plist` | Confirmed |
| RTKit version | `RTKit-2062.141.1.release`, as in the dump | Confirmed — but **not a discriminator** (§3.1) |
| Is 13.5 H13C what is in DRAM? | `__TEXT`: **100.0000 %** of the image's non-zero bytes identical at dump offset 0, 4 bytes differ in total. 26.6.2 H13C: **1.93 %**. 13.5 H13D (M1 Ultra): **22.5 %** | Confirmed |
| Is `H13C` the right variant? | Yes; H13D/H13G/H13S from the same build all fail the `__TEXT` test | Confirmed |
| Do the tools work on 13.5 blobs? | `disas.py --macos 13.5` (or `AVE_MACOS=13.5`); `kext_extract.py --symbols` new; `fw_dump_compare.py` new | Confirmed |

**The practical consequence: firmware-side findings from 26.6.2 describe code
that is not running.** They need re-reading on the 13.5 image before a driver
relies on them. Kext-side findings likewise (§5).

---

## 1. Where the files came from

### 1.1 Route A — the stub container (primary)

`nvme0n1p3` (2.3 G APFS, no partition label) was copied **read-only** into a
scratch image and parsed with `libfsapfs-python` (`pyfsapfs` 20240429) in a
scratch venv. No partition was mounted. **Confirmed.**

The container has four volumes: `fedrat - Data` (locked, not needed),
`fedrat`, `Preboot`, `Recovery`. `fedrat/System/Library/CoreServices/SystemVersion.plist`
says `13.5 (stub)`, build `22G74`. **Confirmed.**

Everything needed is in `Preboot`, under volume group
`6A46C6D9-3F1C-481C-BAEB-B113BC9CCAAD`:

| File (relative to `/<vgid>/`) | Size | SHA-256 |
|---|---:|---|
| `Restore/Firmware/ave/AppleAVE2FW_H13C.im4p` | 1465833 | `e6efcb2ce64eb4e80c6a0b281383378d92470201bd6fee15ef0d32c518359d8f` |
| `Restore/kernelcache.release.mac13j` | 25847891 | `5b251a816ed2725d4529660a810b09ba099b71c7b25de1e411dce97138a7b28f` |
| `Restore/BuildManifest.plist` (j314cap identity only) | 72070 | `aaf47afdda9601112043a38c445113d28ca057ae0a71d624a1cdd9637b96e8c1` |
| `Restore/Firmware/all_flash/DeviceTree.j314cap.im4p` | 54294 | `1f3dbd9f829120dbdddc4632c360b01b3bc79ffdea69c972712dbbf922f09b59` |
| `Restore/SystemVersion.plist` (`13.5`, `22G74`) | 599 | `8445ef71e69451907d4b0bea9bda4ca20d4a401e7823b5b6bc8c9e651127a9e6` |
| `boot/<active>/usr/standalone/firmware/FUD/AVE.img4` (not copied) | 1471428 | `bd804255677390b4a63521074f1b3ff0be3f0c7a7e49ca5dcd9bf3bf449586a9` |

`boot/active` names `7216E8D5…9BDF9D` as the active boot-object set.
Its `FUD/AVE.img4` is the **personalized** object iBoot verifies and loads: its
IM4P payload is SHA-256-identical to `Restore/Firmware/ave/AppleAVE2FW_H13C.im4p`,
and its IM4M contains the BuildManifest `AVE` digest. The personalized boot
kernelcache (`boot/<active>/System/Library/Caches/com.apple.kernelcaches/kernelcache`)
decodes to the same `kc.macho` as `Restore/kernelcache.release.mac13j`
(SHA-256 `3569563909b1…78a3` for both). The `kernelcache.custom.*` beside it
(1.1 MB) is m1n1 stage 1, not a macOS kernel. **Confirmed.**

The BuildManifest's `AVE` and `KernelCache` digests are the SHA-384 of the
exact im4p files above. **Confirmed.**

The Recovery (`nvme0n1p7`) and iBootSystemContainer (`nvme0n1p1`) containers
were not needed and were not read.

### 1.2 Route B — the IPSW (cross-check)

`api.ipsw.me` for `MacBookPro18,4` (`J314cAP`, `t6001`) lists 13.5 as build
`22G74`:

```
https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw
```

Range-request fetches of `Firmware/ave/AppleAVE2FW_H13C.im4p` and
`kernelcache.release.mac13j` are **SHA-256-identical** to the stub copies
(the ZIP CRCs `d3998c79` / `f3b726be` also match). **Confirmed.**

The IPSW's full `BuildManifest.plist` maps AVE variants to boards:
H13C → `j314c`, `j316c`, `j375c` (M1 Max); H13D → `j375d` (M1 Ultra);
H13S → `j314s`, `j316s` (M1 Pro); H13G → M1 boards. The 13.5 IPSW has
eight AVE images (H13C/D/G/S, H14C/D/G/S), not the nineteen of 26.6.2.
**Confirmed.** H13D, H13G and H13S were fetched as negative controls (§3.3).

### 1.3 What was produced

All under `data/blobs/macos-13.5/` (gitignored), with a `PROVENANCE.txt`
listing source path and SHA-256 of every file:

| File | SHA-256 | How |
|---|---|---|
| `AppleAVE2FW_H13C.im4p` | `e6efcb2c…359d8f` | stub, §1.1 |
| `ave_h13c.bin` (decoded Mach-O, 1465808 bytes) | `fd615e0ba05725c961a074bebf0a2b768c8ed300ff40a5d06e37bdddb18dc0b2` | `pyimg4 im4p extract` (payload is not compressed) |
| `kernelcache.release.mac13j` | `5b251a81…a7b28f` | stub, §1.1 |
| `kc.macho` (96763904 bytes, `MH_FILESET`, 357 entries) | `3569563909b1d416c7407e4959df1621b2db5ae9789eaf87aa9a0280538878a3` | `pyimg4 im4p extract` (LZFSE) |
| `AppleAVE2.macho` (379056 bytes at kc offset `0x1ddf40`) | `2375ee29ece8d4d96c192ac7e22b49c6e0f31836518b8085ca01b540116c998a` | `tools/kext_extract.py --extract` |
| `derived/kext-symbols.txt` (5248 symbols) | `570ae4668f55c96a5ba2c0a66501e6d012437d020b4c5e7f3204c89bc8ccf052` | `tools/kext_extract.py --symbols` |
| `derived/symbols.txt` etc. (1390 fw symbols) | see PROVENANCE | `tools/extract_protocol.py --out` |
| `DeviceTree.j314cap.im4p`, `adt.bin` | `1f3dbd9f…09b59`, `26eed825…23d43` | stub, `pyimg4` |
| `ave_all/` H13D/H13G/S im4p + decoded | see PROVENANCE | IPSW, controls |

Version strings: firmware `HEAD_c86a10b71_AppleAVE2FW-6070.11.1.debug`
(26.6.2: `AppleAVE2FW-9003.78.0.debug`); kernel
`Darwin Kernel Version 22.6.0 … xnu-8796.141.3~6/RELEASE_ARM64_T6000`.
**Confirmed.**

---

## 2. Reproducing

Nothing here writes to a disk or touches hardware. The image copy is the only
privileged step, and it only reads.

```sh
S=<scratch dir>
sudo dd if=/dev/nvme0n1p3 of=$S/p3.img bs=4M status=none    # read-only copy, 2.3 G
sudo chown $USER $S/p3.img && chmod 0400 $S/p3.img
python3 -m venv $S/venv && $S/venv/bin/pip install libfsapfs-python
cd $S && $S/venv/bin/python - <<'EOF'
import pyfsapfs
c = pyfsapfs.container(); c.open("p3.img")
v = next(c.get_volume(i) for i in range(c.number_of_volumes)
         if c.get_volume(i).name == "Preboot")
G = "/6A46C6D9-3F1C-481C-BAEB-B113BC9CCAAD/Restore/"
for p in ("Firmware/ave/AppleAVE2FW_H13C.im4p", "kernelcache.release.mac13j",
          "BuildManifest.plist", "SystemVersion.plist",
          "Firmware/all_flash/DeviceTree.j314cap.im4p"):
    e = v.get_file_entry_by_path(G + p)
    open(p.split("/")[-1], "wb").write(e.read_buffer(e.size))
EOF
```

(The volume-group UUID is specific to this install; list `Preboot`'s root to
find yours.) Then, from the repo root with the files in
`data/blobs/macos-13.5/`:

```sh
B=data/blobs/macos-13.5
./.venv/bin/pyimg4 im4p extract -i $B/AppleAVE2FW_H13C.im4p -o $B/ave_h13c.bin
./.venv/bin/pyimg4 im4p extract -i $B/kernelcache.release.mac13j -o $B/kc.macho
python3 tools/kext_extract.py $B/kc.macho --extract com.apple.driver.AppleAVE2 -o $B/AppleAVE2.macho
python3 tools/kext_extract.py $B/kc.macho --symbols com.apple.driver.AppleAVE2 -o $B/derived/kext-symbols.txt
python3 tools/extract_protocol.py $B/ave_h13c.bin --out $B/derived
python3 tools/fw_dump_compare.py $B/ave_h13c.bin data/blobs/iboot-window-16m.bin
python3 tools/fw_dump_compare.py data/blobs/ave_h13c.bin data/blobs/iboot-window-16m.bin   # control
```

`kext_extract.py --symbols` on the 26.6.2 `kc.macho` reproduces the tracked
`data/derived/kext-symbols.txt` **byte-for-byte** (checked with `cmp`), so the
13.5 file is generated the same way the old one was. **Confirmed.**

Route B instead of A:

```sh
./.venv/bin/python tools/fetch_firmware.py --board j314c --variant H13C --outdir <dir> \
  --url https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw
```

(`fetch_firmware.py` pulls the DeviceTree and AVE image only; the kernelcache
was pulled with the same `RemoteZip(url).extract("kernelcache.release.mac13j")`
call.)

---

## 3. Verification against the dump

The dump is `data/blobs/iboot-window-16m.bin`, 16 MiB of DRAM from physical
`0x10000b28000`. `tools/fw_dump_compare.py` compares a candidate image's
`__TEXT` file contents at dump offset 0 and its `__DATA` file contents at dump
offset `0xf68000` (physical `0x10001a90000`), and separately searches for
unique 32-byte `__TEXT` samples anywhere in the dump.

"Non-zero" identity counts only bytes that are non-zero in the candidate, so
zero-filled regions that match any sparse dump do not inflate the score.

### 3.1 The RTKit string cannot tell builds or variants apart

The 13.5 H13C image contains `RTKit-2062.141.1.release` once (file offset
`0xef5dc`); the dump contains it five times. **Confirmed.** It matches — but so
do **H13D, H13G and H13S** from the same build, and every other RTKit
coprocessor in the dump. It only says "some 13.x-era RTKit". This is
[trap 2](00-methodology.md): it is recorded as a necessary condition, not as
evidence.

### 3.2 Results

| Candidate | `__TEXT` identical bytes | `__TEXT` non-zero identical | `__TEXT` 32-byte windows | Unique samples found anywhere (dominant offset) | `__DATA` non-zero identical |
|---|---|---|---|---|---|
| **13.5 H13C** | 966652 / 966656 = **99.9996 %** | 857455 / 857455 = **100.0000 %** | 30206 / 30208 = **99.99 %** | 3536 / 3536 = **100 %** (3535 at delta `0x0`) | 77497 / 77511 = **99.98 %** |
| 26.6.2 H13C (control) | 61015 / 1261568 = 4.84 % | 21500 / 1113731 = **1.93 %** | 521 / 39424 = 1.32 % | 216 / 3553 = **6.08 %** (no dominant offset; top 19 at `-0x31fb0`) | 68957 / 81710 = 84.39 % |
| 13.5 H13D (M1 Ultra) | 253389 / 966656 = 26.21 % | 193145 / 857387 = **22.53 %** | 4173 / 30208 = 13.81 % | 3073 / 3527 = 87.13 % (2351 at `+0x54`) | 76451 / 77502 = 98.64 % |
| 13.5 H13S (M1 Pro) | 6.24 % | 2.94 % | 2.57 % | 40.53 % | 93.59 % |
| 13.5 H13G (M1) | 6.18 % | 3.05 % | 2.62 % | 33.51 % | 93.60 % |

All **confirmed** (command output of `fw_dump_compare.py`). Reading the table:

- **Aligned `__TEXT` identity is the discriminator.** It returns 100 % for one
  candidate and ≤ 22.5 % for every other, including the same-build sibling
  H13D whose image is exactly the same size. **Confirmed.**
- The position-free sample test separates 13.5 from 26.6.2 (100 % vs 6 %,
  consistent with the ~5 % earlier work measured) but **not** H13C from H13D
  (87 %): the sibling shares most functions, shifted by `0x54`. It is only
  useful in combination with the aligned test.
- **The `__DATA` comparison is weak on its own.** The 26.6.2 control still
  scores 84 % and H13D 98.6 %, because RTKit fills `_rtk_init_stack` with a
  repeating `RTKSTACK` pattern (non-zero, identical in every build) and most
  `__DATA` is that stack. It is reported for the diff locations (§3.4), not as
  evidence of identity.
- Alignment: 13.5 H13C `__TEXT` file offset `0x4000` (VA `0x0`) sits at dump
  offset **`0x0`** exactly; the first word `0x14000081` is at both. **Confirmed.**

### 3.3 Variant

H13C is the variant in DRAM (table above). This agrees with the manifest
(`j314cap` → `AppleAVE2FW_H13C.im4p`), which is how [00](00-methodology.md)
settled the question for 26.6.2. The per-SoC *kext* variant for `ave0`
(`soc-id = t6000` → Castor) is a separate question; the firmware image has
no per-instance variant. **Confirmed** (firmware); the kext chain is in
[42](42-asc-firmware-ownership.md) §1.4 and was read on 26.6.2 only.

### 3.4 What iBoot changed

`__TEXT`: exactly **4 bytes** differ, in two u32 fields of a small header that
follows the `uuid` record at VA `0x4204`:

```
VA      image       dump
0x422c  0x000efa30  0x000efa30   = _rtk_patchbay VA
0x4230  0x000001c8  0x000001c8   = _rtk_patchbay size
0x4234  0x000ec000  0x000ec000   = __DATA vmaddr
0x423c  0x00000000  0x000ec000   <- written
0x4240  0x00000000  0x000001f0   <- written
```

The values are confirmed; the neighbouring fields are confirmed to equal the
patchbay VA/size and the `__DATA` vmaddr. That the two written fields are a
loader-filled "(DATA base, used patchbay length)" pair is **inferred** from
the layout only; who wrote them (iBoot or the firmware at run time) is
**unknown**, iBoot being the likelier since `__TEXT` is presumably read-only
to the firmware (**inferred**).

`__DATA` (file size `0x64000`): 147 bytes differ, in four ranges:

| Image VA | Section | What |
|---|---|---|
| `0xefa38–0xefa3f` | `_rtk_patchbay` | `STKG` value (`0xaff` → `0x816ea533007323bc`) |
| `0xefba3–0xefbcf` | `_rtk_patchbay` | `SOC_` `0xffffffff`→`0x6001`, `SOCR` `0xffffffff`→`0x11`, `CpAd` 0→`0x40d800000`, `WrAd` 0→`0x40dc00000` |
| `0xefbe7–0xefbe8` | `_rtk_patchbay` | `IOBA` 0→`0x40c000000` |
| `0x14c36b–0x14c504` | `_rtk_tunables` | tunable values (the region `TUNS`/`TUNZ` point at) |

All other sections of the file-backed `__DATA` are 100 % identical, including
`__const` and `__data`. **Confirmed** (tag values from `tools/rtkit_tags.py`
on the image and on the dump).

Tags iBoot did **not** change because the image already carries them:
`RTSZ = 0x220000`, `GptS = 0x4000`, `TUNS = 0x14c368`, `TUNZ = 0x1e8`,
`McRA = 0x4000`. **Confirmed.**

---

## 4. Segment layout of the 13.5 firmware

`python3 tools/macho_info.py data/blobs/macos-13.5/ave_h13c.bin` — `MH_PRELOAD`,
arm64e, 5 load commands, **1390 symbols** (26.6.2: 1558). **Confirmed.**

| | 13.5 (22G74) | 26.6.2 (25G83) |
|---|---|---|
| `__TEXT` | vm `0x0` + **`0xec000`**, file `0x4000` + `0xec000` | vm `0x0` + `0x134000`, file `0x4000` + `0x134000` |
| `__DATA` | vm **`0xec000`** + `0x134000`, file `0xf0000` + **`0x64000`** | vm `0x134000` + `0x134000`, file `0x138000` + `0x134000` |
| `__DATA_CONST` | none | vm `0x268000` + 0 |
| TEXT+DATA vmsize | **`0x220000`** | `0x268000` |
| `_rtk_mtab` | in `__TEXT` at `0xeb640` | in `__DATA` at `0x135268` |
| `_rtk_patchbay` (tag list) | `__DATA+0x3a30` = VA **`0xefa30`**, size `0x1c8` | `__DATA+0x0` = VA `0x134000`, size `0x211` |
| `_rtk_init_stack` | `0xefc00` + `0x10000` | `0x138eb0` + `0x10000` |
| `_rtk_boot` | `0x104000` + `0x8000` | `0x14c000` + `0x8000` |
| `_rtk_page_tables` | `0x10c000` + `0x40000` | `0x154000` + `0x40000` |
| `_rtk_power`, `_rtk_tunables` | `0x14c000` + `0x368`, `0x14c368` + `0x1e8` | `0x135520` + `0x3b8`, `0x138cc8` + `0x1e8` |
| `__zerofill` | `0x14c580` + `0xcfd08` | `0x194000` + `0xd2680` |

**Confirmed** (section table). `__TEXT.__text` is `0xbb8d8` bytes
(26.6.2: `0xfd710`).

### 4.1 `RTSZ` and where `__DATA` sits

`RTSZ = 0x220000` equals `__TEXT` vmsize + `__DATA` vmsize of the 13.5 image
exactly (`0xec000 + 0x134000`); for 26.6.2 it would be `0x268000`.
**Confirmed** (arithmetic on the section table), and one more independent
match to the dump.

In firmware VA, `__DATA` starts at **`0xec000`**, immediately after `__TEXT`.
Physically it does not: `__TEXT` occupies `0x10000b28000–0x10000c14000`, and
`__DATA` VA `0xec000` corresponds to physical **`0x10001a90000`** (dump offset
`0xf68000`), `0xf68000` above the TEXT base. **Confirmed** by three
independent landmarks landing where the 13.5 section table predicts:

- the tag list at physical `0x10001a93a30` = `__DATA+0x3a30` = `_rtk_patchbay`;
- `TUNS = 0x14c368` = `_rtk_tunables`, which is `__DATA+0x60368` → physical
  `0x10001af0368`, inside the lone non-zero 16K page at dump offset `0xfc8000`;
- the zero run between (`__DATA+0x18000–0x60000`) is `_rtk_boot` +
  `_rtk_page_tables`, which are zero-filled in the image too.

Under 26.6.2's layout the tag list would have to sit at `__DATA+0`
(physical `0x10001a90000`); it does not. That the ASC's MMU maps VA `0xec000`
to that physical page is **inferred** (the firmware's page tables are zero in
the dump region captured, so the mapping itself was not read).

---

## 5. The 13.5 kext differs substantially from 26.6.2

Recorded because docs 06–42 cite 26.6.2 kext VAs and structures. Not analysed
further here; each point is a symbol-table or string observation.

| | 13.5 | 26.6.2 |
|---|---|---|
| `AppleAVE2` fileset entry | kc offset `0x1ddf40`, vm `0xfffffe00071e1f40` | `0x230b00`, `0xfffffe0007234b00` |
| `__TEXT_EXEC` | vm `0xfffffe0008e83b10` + `0xd2008`, kc file `0x1e7fb10` | vm `0xfffffe0008b34bb0` + `0x1a059c`, file `0x1b30bb0` |
| symbols | 5248 | 10844 |
| `AVE_IOP_Config_*` variants | 13 (Acis, Atlas, Castor, Cronus, Hera, Hypnos, Nyx, Panda, Rhea, Tethys, Thanatos, Themis, Tyche) | 21 |
| `AVE_IOP::Config` | `Config(unsigned long long)` at `0xfffffe0008f21544` | `Config(void)` at `0xfffffe0008c4073c` |
| `AVE_FwImg` class | **absent** | present (`RetrieveInfo`, `m_bIBootLoaded`) |
| `AVE_IOP_If` table | `gs_saAVE_IOP_If` at `0xfffffe0007bc39c8` | `gsc_saAVE_IOP_If` at `0xfffffe0007ee07f0` |
| `"pre-loaded"`, `"segment-ranges"` strings | present in kext `__TEXT` | present |

**Confirmed** (symbol and string presence). `AVE_IOP_Config_Castor` is at
`0xfffffe0008f1d76c`. **Unknown**, and material to
[42](42-asc-firmware-ownership.md) §1.4: whether the 13.5 `AVE_IOP::Config`
has the same iBoot-loaded early return, since it takes a 64-bit argument and
there is no `AVE_FwImg` to hold `m_bIBootLoaded`.

The 13.5 firmware also has **no `ProcessCmd_*` symbols**, so
`extract_protocol.py` recovers 0 command handlers from it (26.6.2: 16);
`CFlowControllerBase::CmdProcessor` is at VA `0xd614`. Whether the wire command
set in [07](07-commands-abi.md) is unchanged is **unknown**.

The 13.5 ADT (`DeviceTree.j314cap.im4p`) `ave0`/`ave1` nodes, as printed by
`tools/adt_dump.py --grep ave`, differ from 26.6.2's only in phandle numbering
(`iommu-parent`, `interrupt-parent`, `AAPL,phandle`): `reg`, `interrupts`,
`power-gates`, `soc-id` are identical. **Confirmed** for those nodes only;
other nodes were not compared.

---

## 6. Tool changes

- **`tools/disas.py`**: new `--macos {26.6.2,13.5}` (default `26.6.2`, also
  settable as `AVE_MACOS=13.5`). `13.5` reads `data/blobs/macos-13.5/kc.macho`,
  `ave_h13c.bin`, and `derived/{kext-symbols,symbols}.txt`. Existing
  invocations are unchanged.

  ```sh
  python3 tools/disas.py --macos 13.5 --fw 'CFlowControllerBase12CmdProcessor' -n 0x40
  python3 tools/disas.py --macos 13.5 --kext 'AVE_IOP_Config_Castor' -n 0x40
  ```
- **`tools/kext_extract.py`**: new `--symbols <entry-id> -o <file>` dumps a
  kext's `LC_SYMTAB` in the `kext-symbols.txt` format (previously produced
  ad hoc; reproduces the tracked 26.6.2 file byte-for-byte).
- **`tools/fw_dump_compare.py`** (new): the §3 comparison.
- `macho_info.py`, `rtkit_tags.py`, `extract_protocol.py`, `adt_dump.py`,
  `kext_extract.py --list/--extract` already take paths; no change needed.
  `check_addrs.py` still reads the 26.6.2 `data/blobs/adt.bin`. Everything it
  reads — `/arm-io` `ranges` and `reg` of `avd0`, `isp0`, `dart-avd0`, `ave0`,
  `dart-ave0`, `ave1` — compares equal between the 26.6.2 and 13.5 ADTs
  (m1n1 `load_adt`, `==` on the parsed properties). **Confirmed.**
