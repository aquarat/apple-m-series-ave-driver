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
  behaviour for every other caller;
- **guarded** (Fable review F1): the AVE writes run with
  `exc_guard = GUARD_MARK`, then `dsb sy; isb; udelay(300 ms)`, then the guard
  is restored. Unguarded, an SError here makes `exc_serr()` reboot, and since
  stage 1 chainloads the same file every time, that would be a boot loop.
  Guarded, `exc_serr()` logs it and returns, m1n1 prints
  `dapf: /arm-io/dart-ave0: N exception(s) while programming; continuing
  boot`, and Linux boots with the DAPF unprogrammed. An SError arriving later
  than 300 ms would still escape the guard.

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
| `boot.bin.ave-dapf` | `73577b9903d8b2735bb439e42787341821ae83a5e2c5fd016778a4dbfdd049fd` | patched + guard, version tag `v1.6.1-1-g1ae6361` |

m1n1 finds appended payloads at the linker symbol `_payload_start`; in the
patched ELF it is `0x10c000` = 1 097 728 = the binary's length, so the logo,
DTBs and u-boot stay aligned (verified with `nm`, not assumed).

Differences from Fedora's own build, besides the patch (109 492 bytes of the
unpatched self-built image differ from the package): compiler version and
the console font (Fedora regenerates `font.bin` from Source Code Pro; this
build uses the font committed in the m1n1 tree). The boot logo is Fedora's,
byte for byte.

## 3. Install

Step A is **mandatory** (review F7): it proves a self-built v1.6.1 boots
here before the patch is added. Each step is one reboot. Copies go to a
`.new` file and are renamed into place, as `update-m1n1` does, so a crash
mid-copy cannot leave a truncated `boot.bin` (F3).

```sh
# 0. stop package updates from silently replacing boot.bin while testing (F5)
#    (update-m1n1 runs on every kernel install/removal and on m1n1/u-boot
#    updates; it would also rotate the custom file into boot.bin.old)
echo 'M1N1_UPDATE_DISABLED=1' | sudo tee -a /etc/sysconfig/update-m1n1

# 1. back up the known-good file on the ESP itself
sudo cp -p /boot/efi/m1n1/boot.bin /boot/efi/m1n1/boot.bin.pre-ave && sync
sudo sha256sum /boot/efi/m1n1/boot.bin.pre-ave   # must be 2227cf97...94f7

# 2. step A: unpatched self-built
sudo cp data/blobs/m1n1/boot.bin.stock-src /boot/efi/m1n1/boot.bin.new && sync
sudo mv -f /boot/efi/m1n1/boot.bin.new /boot/efi/m1n1/boot.bin && sync
sudo sha256sum /boot/efi/m1n1/boot.bin           # must be bd040b64...75da8
# reboot; then: tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version  -> v1.6.1

# 3. step B: patched
sudo cp data/blobs/m1n1/boot.bin.ave-dapf /boot/efi/m1n1/boot.bin.new && sync
sudo mv -f /boot/efi/m1n1/boot.bin.new /boot/efi/m1n1/boot.bin && sync
sudo sha256sum /boot/efi/m1n1/boot.bin           # must be 73577b99...49fd
# reboot; then: ... asahi,m1n1-stage2-version -> v1.6.1-1-g1ae6361
```

Verification after booting the patched m1n1:

- **Primary evidence (F2): the m1n1 console during boot.** Look for
  `dapf: Initialized /arm-io/dart-ave0` and possibly `dapf: /arm-io/dart-ave0:
  appended TEXT entry 2: 0x10000b28000-0x10000c13ffc r0 0x11` (or `... TEXT
  ... already covered` / `no segment-ranges`, or the guard's `exception(s)
  while programming`). It scrolls fast; filming the screen during boot is the
  reliable way to catch it.
- **Secondary, read-only, as early after boot as practical:**

  ```sh
  sudo insmod test/ave-overlay.ko variant=3
  HOLD=10 tools/e3-run.sh n3-dump stop_after=8 dapf_dump=1
  ```

  Expect slot 0 `0x1f000000000 - 0x1f0fffffffc r0 0x33 r4 1`, slot 1
  `0x506000000 - 0x507c6c000 r0 0x31 r4 1`, and, if appended, slot 2 TEXT
  `0x10000b28000 - 0x10000c13ffc r0 0x11`. m1n1's power-down of VENC-DART is a
  no-op (virtual device), so `venc_sys` stays on into Linux and is gated by
  Linux later; that DAPF contents survive gating is inferred from ISP (E1),
  so garbage here would not by itself prove m1n1 failed.

After testing, remove `M1N1_UPDATE_DISABLED=1` from
`/etc/sysconfig/update-m1n1` (or keep it while the patched stage 2 is wanted).

## 4. Fallback - if Linux no longer boots

Symptoms of a bad stage 2: black screen or a hang after the Asahi/m1n1 logo,
m1n1 printing "Unhandled exception, rebooting...", or the machine rebooting
over and over, before U-Boot/GRUB appears. Holding the power button still
reaches the startup options picker in all of these cases: iBoot handles it
before any m1n1 code runs. The fix is to put
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
   sudo diskutil mount disk0s4
   diskutil info disk0s4 | grep "Mount Point"   # e.g. /Volumes/EFI - FEDRA (may have " 1" appended)
   cd "/Volumes/EFI - FEDRA/m1n1"               # use the Mount Point shown above
   ls -l                                        # boot.bin, boot.bin.pre-ave, boot.bin.old
   shasum -a 256 boot.bin.pre-ave               # must be 2227cf97...94f7
   sudo cp boot.bin.pre-ave boot.bin            # the mounted ESP is root-owned: sudo is needed
   shasum -a 256 boot.bin                       # must now be 2227cf97...94f7
   cd / && sudo diskutil unmount disk0s4
   ```
4. Shut down, hold the power button, choose the Fedora/Asahi volume.

### From recoveryOS (if macOS does not start)

1. Shut down, hold the power button until startup options appear, choose
   **Options** (recoveryOS), log in if asked.
2. Utilities → **Terminal** (runs as root, no sudo needed), then the same
   `diskutil list`, `diskutil mount disk0s4`, `diskutil info disk0s4 | grep
   "Mount Point"`, `cd` there, `cp boot.bin.pre-ave boot.bin`,
   `diskutil unmount disk0s4`.
3. Restart and choose Fedora/Asahi.

Second fallback if `boot.bin.pre-ave` is somehow missing: `boot.bin.old` on
the same ESP was the live stage 2 from Sep 3 to Sep 9 (same packaged m1n1,
older DTBs/u-boot), so it very probably boots this machine. Do not run
`update-m1n1` while the custom file is installed unless you intend to revert:
it rotates the current `boot.bin` into `boot.bin.old`. Once Linux boots
again, `sudo update-m1n1` regenerates a stock `boot.bin`.

## 5. Install log

- **2026-09-13 18:51** — steps 0-2 done. `/etc/sysconfig/update-m1n1` backed up
  to `update-m1n1.bak-ave` and `M1N1_UPDATE_DISABLED=1` appended.
  `/boot/efi/m1n1/boot.bin.pre-ave` = `2227cf97…94f7` (verified). Step A
  installed: `/boot/efi/m1n1/boot.bin` = `bd040b64…75da8` (unpatched self-built
  v1.6.1, verified on the ESP). `boot.bin.old` unchanged (`8270538d…`).
  Awaiting reboot.
- **2026-09-13 19:11** — `restore-m1n1.sh` and `RESTORE-README.txt` copied
  next to `boot.bin` on the ESP (sources in `tools/m1n1/`, ESP copies verified
  identical). From recovery: `diskutil mount 89A77CF4-32BA-4A03-8BCA-DB0F62925CA4`
  then `sh "/Volumes/EFI - FEDRA/m1n1/restore-m1n1.sh"` (prefix both with
  `sudo` in macOS). Tested on a simulated ESP with a space in the path: it
  restores and verifies, keeps the replaced file as `boot.bin.failed`, and
  refuses to change anything when `boot.bin.pre-ave` is corrupt or missing.
- **2026-09-13 19:17** — **step A booted.** With `/boot/efi/m1n1/boot.bin` =
  `bd040b64…75da8` (self-built, unpatched) the machine booted normally;
  `chosen/asahi,m1n1-stage2-version` = `v1.6.1` (identical tag to Fedora's, as
  expected for an untagged-patch build - the ESP hash is what identifies it).
  No new kernel errors versus the previous boot. The self-built toolchain is
  cleared; step B next.
- **2026-09-13** — **step B installed:** `/boot/efi/m1n1/boot.bin` =
  `73577b99…49fd` (patched, guard, tag `v1.6.1-1-g1ae6361`), via `.new` +
  rename, verified on the ESP. `boot.bin.pre-ave`, `restore-m1n1.sh` and the
  README remain alongside. Awaiting reboot.
