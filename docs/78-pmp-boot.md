# 78. Booting with the PMP running (APPLE_USE_PMP)

2026-09-24. Operator-approved. docs/75 showed that the encoder's clock is set
by a vote to the PMP (power-management coprocessor), and f81 showed that the
PMP was not running. This document records how it was turned on and how to
turn it off again.

## 1. Why it was off

Asahi added the PMP (`apple,t6000-pmp-v2`, `drivers/soc/apple/pmp.rs`,
`CONFIG_APPLE_PMP=m`) in February 2026. It is opt-in at device-tree build
time:

- `pmp@28e700000` is `status = "disabled"` in `t600x-die0.dtsi`.
- m1n1 (`dt_set_pmp`, `src/kboot.c`, v1.6.1) starts it. It copies board-id,
  dram-vendor-id, dram-capacity and every `/arm-io/pmp/iop-pmp-nub` property
  from the ADT as `apple,tunable-*`, then sets `status = "okay"`. **It does
  that only if the FDT has a `pmp` alias.** Otherwise it prints "pmp not found
  in devtree" and returns 0.
- The alias exists only when the DT is built with `-DAPPLE_USE_PMP`, and
  Fedora's DTBs are not.

The commit that added it (`49d2918e946d`, "arm64: dts: apple: Add PMP nodes
and hook up power reporting") gives no reason for the opt-in.

## 2. What the flag changes (this kernel, t6001-j314c)

Built from the running kernel's own source
(`kernel-7.1.13-401.asahi.fc44.src.rpm`: `linux-7.1.13.tar.xz` plus the
`arch/arm64/boot/dts/apple`, `include/dt-bindings` and `scripts/dtc` hunks of
`patch-7.1-redhat.patch`). It uses the kernel's own `dtc`, because Asahi's
DT has float literals (`apple,avg-power-kp = <1.5>`) that Fedora's `dtc`
rejects. **Built without the flag, the result is byte-identical to the
installed `/boot/dtb/apple/t6001-j314c.dtb`** (sha256 `6467c77f…f146`), which
validates the build. With the flag, the source-level diff (labels resolved,
phandle renumbering ignored) is exactly:

| node | stock | with the flag |
|---|---|---|
| `/aliases` | none | `pmp = "/soc/pmp@28e700000"` |
| `dcp@38bc00000` (disp0) | `ps_disp0_cpu0` | `pmp_report_disp0` |
| `dcp@289c00000` (dispext0) | `ps_dispext0_cpu0` | `pmp_report_dispext0` |
| `dcp@28cc00000` (dispext1, disabled) | `ps_dispext1_cpu0` | `pmp_report_dispext1` |
| `isp@384000000` | `ps_isp_sys` first | `pmp_report_isp_sys` first |
| `avd@287080000` | `ps_avd_sys` | `pmp_report_avd_sys` |

Each report entry is itself a child of the power domain it replaces. In
this kernel `report@b` (isp) is enabled; on the current `asahi` branch tip it
is disabled, and there the flag would leave the camera without a power
domain. `report@10` (`pmp-venc-sys`) stays disabled either way.

Before installing, `dt_set_pmp`'s hard requirements were checked against
the ADT (the restore ADT, `data/blobs/adt.bin`): `/chosen` has `board-id` and
`dram-vendor-id`, and `/arm-io/pmp/iop-pmp-nub` exists with 75 properties.
A failure there makes m1n1 refuse to boot, so this was the check that
mattered.

## 3. Installation

`boot.bin` = m1n1 + 41 DTBs + gzip(u-boot) + config. The new file is the
live one (`73577b99…49fd`, the DAPF-patched m1n1 of docs/50) with **only
the t6001-j314c DTB blob replaced**. Everything before and after it was
checked to be byte-identical.

| file on the ESP (`/boot/efi/m1n1/`) | sha256 | what |
|---|---|---|
| `boot.bin` | `111066a1…4e38` | DAPF m1n1 + `APPLE_USE_PMP` j314c DTB |
| `boot.bin.pre-pmp` | `73577b99…49fd` | the stage 2 before this (DAPF m1n1, stock DTBs) |
| `boot.bin.pre-ave` | `2227cf97…94f7` | stock Fedora stage 2 (docs/50) |
| `restore-pre-pmp.sh` | | puts `boot.bin.pre-pmp` back, verified (`tools/m1n1/`) |
| `RESTORE-README.txt` | | recovery from macOS/recoveryOS; PMP section first |

`M1N1_UPDATE_DISABLED=1` (docs/50) still protects this from `update-m1n1`.
A kernel update does not rebuild `boot.bin`. **To undo:**
`sudo sh /boot/efi/m1n1/restore-pre-pmp.sh`, then reboot.

The working files are on the target in `~/pmp-dt/`: the DTBs, `kdtc`,
`boot-pmp.bin` and `baseline-pre-pmp.txt`.

## 4. First boot (boot `630e75f6…`)

- `pmp@28e700000`: `okay`, 67 `apple,tunable-*` properties, board-id 8,
  dram-vendor-id 6. The `pmp` module is bound. `[1.25] apple_pmp: RTKit:
  syslog message: io.cpp:41: PMP started`. One warning: `unknown property
  "apple,tunable-fast-die-ctrl-ce-map"`.
- Against `baseline-pre-pmp.txt`: both DCPs bound, eDP connected and
  enabled, ISP bound (`/dev/video0`). AVD fails its firmware load exactly as
  before. Nothing new at `err` level.
- genpd: `pmp-disp0` and `pmp-dispext0` on; `pmp-isp-sys` and `pmp-avd-sys`
  off.
- R1b (`perf_dump=1`): PMP-STATUS 1, PS-ACK = PS-REQ = `0x60003000`,
  DVFS-STATE non-zero, AVE0 DVFS 0.
- Encoder: 18-23% **slower** than without the PMP (docs/53 f89).

## 5. Next

docs/75 §7: R3 (enable `report@10` and put it in ave0's power-domain chain,
so the PMP is told when VENC_SYS is on), then R4 (write the VENC DVFS vote
into the PTD, and clear it at teardown).
