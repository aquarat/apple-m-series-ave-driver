# N3: program AVE's DAPF from m1n1 stage 2

Linux cannot write the AVE DAPF at any point of a power session: every first
write raises a fatal asynchronous SError ([49](49-dapf-write-reset.md)). m1n1
stage 2 already programs DAPFs for aop/mtp/pmp/isp at boot, and ISP's live
DAPF (E1) shows that works. This adds `dart-ave0` to that list.

Everything here is **confirmed** unless marked otherwise.

## 1. The boot chain on this machine

| item | value |
|---|---|
| m1n1 stage 1 / stage 2 | `v1.6.1` / `v1.6.1` (`/proc/device-tree/chosen`) |
| package | `m1n1-1.6.1-1.fc44` (`/usr/lib/m1n1/m1n1.bin`, `m1n1-asahi.bin`) |
| ESP | `/dev/nvme0n1p4`, vfat, label **`EFI - FEDRA`**, PARTUUID **`89a77cf4-32ba-4a03-8bca-db0f62925ca4`** (= `/chosen/asahi,efi-system-partition`), mounted at `/boot/efi` |
| stage 2 file | `/boot/efi/m1n1/boot.bin` (5 010 668 bytes, sha256 `2227cf97…94f7`); `boot.bin.old` (Sep 3) also present |
| how it is built | `update-m1n1`: `cat m1n1.bin t6*.dtb t81*.dtb; gzip -c u-boot-nodtb.bin; m1n1.conf options` |

**Process check:** rebuilding `boot.bin` from the packaged `m1n1.bin`,
`/boot/dtb/apple/t6*.dtb t81*.dtb` and `gzip -c u-boot-nodtb.bin` is
**byte-identical** to the ESP's `boot.bin`. The packaged `m1n1.bin` is
`m1n1-asahi.bin` (1 097 728 bytes) followed by a 327 696-byte Fedora logo blob
(`m1n1_log…`).

## 2. The change

`tools/m1n1/0001-dapf-program-dart-ave0-and-append-the-AVE-TEXT-entry.patch`,
against tag `v1.6.1`, `src/dapf.c` only:

- add `{"/arm-io/dart-ave0", 3, "/arm-io/ave0"}` to `dapf_entries[]` (reg[3]
  is the DAPF, tagged `'DAPF'` in `/defaults/pmap-io-ranges`);
- program the live ADT's `filter-data-instance-0` exactly as for ISP;
- then, only if no programmed entry already covers the start of `ave0`'s
  first `segment-ranges` segment (its TEXT), append one entry: that range,
  end masked to the last 4-byte word, `r0 0x11`, `r4 1` - copied from ISP's
  live TEXT entry. If `ave0` has no `segment-ranges`, nothing is appended.
- existing entries are untouched: `dapf_init()` keeps its signature and
  behaviour for every other caller.

Build (native aarch64; needs `rust-std-static-aarch64-unknown-none-softfloat`):

```sh
git clone https://github.com/AsahiLinux/m1n1.git && cd m1n1
git checkout v1.6.1 && git submodule update --init --depth 1
git am /path/to/0001-dapf-program-dart-ave0-and-append-the-AVE-TEXT-entry.patch
make -j10 RELEASE=1 LOGO=fedora build/m1n1-asahi.bin
```

Assemble: `m1n1-asahi.bin` (patched) + the Fedora logo blob taken verbatim
from the packaged `m1n1.bin` + everything after the packaged `m1n1.bin` in
the ESP's current `boot.bin` (DTBs, gzipped u-boot, config), verbatim.

Artefacts (gitignored, `data/blobs/m1n1/`):

| file | sha256 | what |
|---|---|---|
| `esp-boot.bin` | `2227cf97eac6d5f2db08e43a81693ef96aea168532933bfec9602c8c580d94f7` | copy of the current ESP `boot.bin` |
| `boot.bin.stock-src` | `bd040b64842cbe07752312725f774dd11870c462712b6fdbb75d442d92275da8` | **unpatched** v1.6.1 built here, same assembly |
| `boot.bin.ave-dapf` | `fd7ab4e104bd88f2851894cc3da1a2ac14b3b81deba91545b0d2282d8a92441c` | patched, version tag `v1.6.1-1-g9f9850d` |

Differences from Fedora's own build, besides the patch: compiler version and
the console font (Fedora regenerates `font.bin` from Source Code Pro; this
build uses the font committed in the m1n1 tree). The boot logo is Fedora's,
byte for byte.

## 3. Install

Two steps are possible; step A isolates "a self-built m1n1 boots" from "the
patch is safe". Each is one reboot.

```sh
# once: back up the known-good file on the ESP itself, under a new name
sudo cp -p /boot/efi/m1n1/boot.bin /boot/efi/m1n1/boot.bin.pre-ave
sudo sha256sum /boot/efi/m1n1/boot.bin.pre-ave   # must be 2227cf97...94f7

# optional step A: unpatched self-built
sudo cp data/blobs/m1n1/boot.bin.stock-src /boot/efi/m1n1/boot.bin && sync
# reboot; check: tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version  -> v1.6.1

# step B: patched
sudo cp data/blobs/m1n1/boot.bin.ave-dapf /boot/efi/m1n1/boot.bin && sync
# reboot; check: ... asahi,m1n1-stage2-version -> v1.6.1-1-g9f9850d
```

Verification after booting the patched m1n1 (read-only, no core start):

```sh
sudo insmod test/ave-overlay.ko variant=3
HOLD=10 tools/e3-run.sh n3-dump stop_after=8 dapf_dump=1
```

Expect the DAPF slots to hold the ADT entries (`0x1f0` window r0 `0x33`, the
MMIO entry r0 `0x31`) and, if appended, TEXT `0x10000b28000 -
0x10000c13ffc` r0 `0x11`, instead of E2's uninitialised contents.

**Note:** `update-m1n1` runs on m1n1/u-boot/kernel package updates and will
replace the custom `boot.bin` with a stock one (moving the custom one to
`boot.bin.old`). That is safe, but it silently removes the AVE entry.

## 4. Fallback - if Linux no longer boots

Symptoms of a bad stage 2: black screen or a hang after the Asahi/m1n1 logo,
or m1n1 printing an exception, before U-Boot/GRUB appears. The fix is to put
`boot.bin.pre-ave` back as `boot.bin` on the ESP from another OS.

### From macOS (preferred)

1. Shut down. Press and **hold** the power button until "Loading startup
   options" appears. Select **Macintosh HD** (macOS) and boot it.
2. Open Terminal and find the ESP:
   ```sh
   diskutil list
   ```
   It is the ~500 MB `EFI` partition labelled **`EFI - FEDRA`** on the
   internal disk (on this machine the 4th partition, normally `disk0s4`).
   Confirm with `diskutil info disk0s4 | grep -i "uuid"` — the partition UUID
   must be `89A77CF4-32BA-4A03-8BCA-DB0F62925CA4`.
3. Mount it and restore:
   ```sh
   sudo diskutil mount disk0s4          # mounts at /Volumes/EFI - FEDRA (name may vary)
   cd "/Volumes/EFI - FEDRA/m1n1"
   ls -l                                 # boot.bin, boot.bin.pre-ave, boot.bin.old
   shasum -a 256 boot.bin.pre-ave       # must be 2227cf97...94f7
   cp boot.bin.pre-ave boot.bin
   cd / && sudo diskutil unmount disk0s4
   ```
4. Shut down, hold the power button, choose the Fedora/Asahi volume.

### From recoveryOS (if macOS does not start)

1. Shut down, hold the power button until startup options appear, choose
   **Options** (recoveryOS), log in if asked.
2. Utilities → **Terminal**, then the same `diskutil list`,
   `diskutil mount disk0s4`, `cd "/Volumes/EFI - FEDRA/m1n1"`,
   `cp boot.bin.pre-ave boot.bin`, `diskutil unmount disk0s4`.
3. Restart and choose Fedora/Asahi.

Second fallback if `boot.bin.pre-ave` is somehow missing: `boot.bin.old` on
the same ESP (the Sep 3 stage 2) also boots this machine, and a stock file can
be regenerated from Fedora with `sudo update-m1n1` once booted.
