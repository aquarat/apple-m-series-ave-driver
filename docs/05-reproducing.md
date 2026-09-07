# Reproducing this from scratch

Everything in `data/derived/` can be regenerated on any Linux machine with
network access. Hardware access is **not** required for the static phase — the
M1 Max in question was used only to confirm what a booted Asahi system does and
does not expose.

## Setup

```sh
python3 -m venv .venv && ./.venv/bin/pip install -r requirements.txt
git clone --depth 1 https://github.com/AsahiLinux/m1n1 m1n1-src
```

`m1n1-src` is needed only for `proxyclient/m1n1/adt.py`, the ADT parser.

## 1. Why the ADT must come from Apple

On a booted Asahi system:

```sh
ls /proc/device-tree/soc/ | grep -iE 'ave|avd'   # -> avd@286000000 only
```

`/proc/device-tree` is the hand-written kernel DTS patched by m1n1 at boot, so
it contains only nodes with existing bindings. There is no runtime export of
the real ADT — nothing under `/sys/firmware`, no reserved-memory carveout, no
`chosen` property. AVE is absent there because no driver exists, not because
the hardware is missing.

The ADT therefore has to be read out of Apple's own DeviceTree image.

## 2. Fetching without downloading 20 GB

A restore IPSW is a plain ZIP, so HTTP range requests can pull individual
members. The DeviceTree is ~56 KB and the firmware ~2.6 MB, out of ~19.8 GB.

```sh
./.venv/bin/python tools/fetch_firmware.py --list
./.venv/bin/python tools/fetch_firmware.py --board j314c --variant H13C
```

The IPSW URL comes from `api.ipsw.me` for the Mac identifier (M1 Max 14" is
`MacBookPro18,3`). Apple's CDN resets long-lived range sessions, so the tool
retries per member.

Board id comes from the machine itself:

```sh
tr '\0' '\n' < /proc/device-tree/compatible    # apple,j314c / apple,t6001
```

## 3. Unwrapping Image4

Both payloads are LZFSE-compressed inside an Image4 container:

```sh
./.venv/bin/pyimg4 im4p extract \
  -i data/blobs/Firmware/all_flash/DeviceTree.j314cap.im4p -o data/blobs/adt.bin
./.venv/bin/pyimg4 im4p extract \
  -i data/blobs/Firmware/ave/AppleAVE2FW_H13C.im4p -o data/blobs/ave_h13c.bin
```

The ADT is ~341 KB of raw Apple device tree; the firmware is a 2.6 MB Mach-O.

## 4. Analysis

```sh
./.venv/bin/python tools/adt_dump.py data/blobs/adt.bin --grep 'ave|avd'
python3 tools/macho_info.py data/blobs/ave_h13c.bin
python3 tools/extract_protocol.py data/blobs/ave_h13c.bin
```

`file` will misidentify the raw ADT as thermal-camera data; that is a magic-byte
collision and harmless.

For disassembly, `llvm-objdump` reads the `MH_PRELOAD` image directly:

```sh
llvm-objdump -d --start-address=0xe66a4 --stop-address=0xe6860 data/blobs/ave_h13c.bin
```

## 5. The host side

The kernelcache from the same IPSW carries `AppleAVE2.kext`. See
[06-kext.md](06-kext.md) for the walkthrough:

```sh
./.venv/bin/pyimg4 im4p extract -i data/blobs/kernelcache.release.mac13j -o data/blobs/kc.macho
python3 tools/kext_extract.py data/blobs/kc.macho --list --grep ave
python3 tools/kext_classmap.py data/derived/kext-symbols.txt
```

For raw disassembly of kext code, `llvm-objdump` will not resolve
`__TEXT_EXEC` addresses in the fileset; compute the file offset by hand
(`va - 0xfffffe0008b34bb0 + 0x1b30bb0` for this kernelcache) and use
`objdump -D -b binary -m aarch64`.

## Provenance

- IPSW: `UniversalMac_26.6.2_25G83_Restore.ipsw` (macOS 26.6.2, build 25G83)
- Machine: MacBook Pro 14" M1 Max, `apple,j314c` / `apple,t6001`
- Asahi kernel at time of writing: `7.1.6-400.asahi.fc44.aarch64+16k`
- Firmware build string: `RTKit-3255.160.4.release`
- Kernelcache: `kernelcache.release.mac13j` (same IPSW)
