# t8103 (M1) port

The driver on a MacBook Air M1 (`apple,j313`, t8103), Asahi kernel
`7.1.13-fairydust-1-ARCH`, macOS 13.5 coprocessor firmware. Brought up on
2026-10-01 following [docs/79](../docs/79-porting.md). H.264 through V4L2
works; everything below "Not tested" is exactly that.

## Use after a boot

Nothing loads at boot. Either by hand,

```sh
sudo t8103/ave-run.sh C8     # overlay + driver, then a 30-frame self-check
```

or on demand through the unit in `t8103/service/`:

```sh
sudo t8103/service/install-service.sh install   # once, and again after a kernel update
systemctl start apple-ave.service               # no sudo: a polkit rule allows the start
```

The unit runs a root-owned copy of the modules and scripts from
`/usr/local/lib/apple-ave/`, so the passwordless start, stop and restart
cannot be used to load anything else. Start loads overlay and driver, stop
unloads the driver (Halt, power off). A later start resets the core and
restores its DATA from the pristine blob.

The encoder is meant to be loaded only while it is used, because the
machine cannot suspend with the core running:

- `ipadcast` restarts the unit when a viewer connects and stops it a minute
  after the last one has left.
- `/usr/lib/systemd/system-sleep/apple-ave` unloads the driver before every
  suspend. With the driver unloaded, s2idle works and the DAPF survives it.
- The driver is loaded with `pm_sleep=1`, so if the unload fails (device in
  use) the suspend is refused rather than risked.
- An unclean unload leaves the core powered; the unload script then holds a
  sleep inhibitor until the next boot.
- The load keeps a flag while the modules go in and for 30 s after; if the
  machine resets in that window it refuses to load again until
  `/var/lib/apple-ave/loading` is removed.

Either way that leaves `/dev/videoN` (name `apple-ave-enc`) usable without root. With
ffmpeg, pad to the encoder's grid: its V4L2 encoder cannot crop, and a
1080-line frame into the 1088-line buffer makes ffmpeg read past the frame
and segfault.

```sh
ffmpeg -i in.mp4 -vf "pad=ceil(iw/64)*64:ceil(ih/16)*16" -pix_fmt nv12 \
       -c:v h264_v4l2m2m -b:v 8M out.mp4
wf-recorder -o DP-1 -c h264_v4l2m2m -x nv12 \
       -F "pad=ceil(iw/64)*64:ceil(ih/16)*16" -p b=8M -f out.mp4
```

Requirements: m1n1 stage 2 with the DAPF patch (step B2 below) and
`/lib/firmware/apple/ave_h13g.bin` (installed by `ave-run.sh` from
`data/blobs/macos-13.5-j313/`).

## Rebuilding

```sh
t8103/build-modules.sh       # apple-ave.ko, ave-overlay.ko for the running kernel
t8103/build-m1n1.sh          # boot.bin candidates in t8103/boot/
```

`build-m1n1.sh` is reproducible here: a rebuild on 2026-10-01 gave the same
four sha256 sums as the images that were booted.

After a kernel, m1n1 or uboot-asahi update the installed image carries the
old device trees, and `update-m1n1` is disabled while a candidate is
installed. Then:

```sh
sudo t8103/ave-m1n1-step.sh restore
sudo update-m1n1
t8103/build-m1n1.sh && t8103/build-modules.sh
sudo t8103/ave-m1n1-step.sh B2      # reboot
```

If Linux does not come up after an m1n1 step: hold the power button, pick
macOS or Options, and in a terminal

```sh
diskutil mount B212DF28-A010-4BC5-9532-BC39966685A4
sh "/Volumes/EFI - OMARC/m1n1/restore-m1n1.sh"     # sudo in macOS
```

## What differs from t6001

| | t6001 (M1 Max) | t8103 (M1) |
|---|---|---|
| ADT nodes | `ave0`, `dart-ave0` (+ `ave1`) | `ave`, `dart-ave` |
| Firmware | `H13C`, TEXT 0xec000, DATA 0x134000 | `H13G`, TEXT 0xcc000, DATA 0x128000; same build tag `AppleAVE2FW-6070.11.1` |
| iBoot placement | TEXT 0x10000b28000, DATA 0x10001a90000 | TEXT 0x8009f4000, DATA 0x8019b0000 (remap 0xf000cc000) |
| RVBAR | locked | locked, `0x1020008009f4001` |
| DAPF entries in the ADT | window + MMIO | window only (0xf00000000-0xfffffffff) |
| Interrupt controller | AIC2, 4 cells | AIC, 3 cells; AVE 560, DARTs 557 |
| Power domains | me1 without a phandle | me0 and me1 without a phandle, siblings under pipe4 + pipe5 |
| AVE_DPE tunables | `Castor_6000`: 1 CAT + 124 + 123 CAC | `Acis_8103`: no CAT, 1 + 39 CAC |
| Device row (13.5) | 14 / 11 / 8 | 13 / 10 / 7 |
| Handshake message 1 | 7, 0x9bc0, 0x100, 0xc0000 | the same |
| Message 5 client buffer | 0xb4000 | 0xb0000 |
| CPU_STATUS cold / running | 0x2a / 0x2c | the same |
| PMP vote | 2.4-2.6x | none (no t8103 PMP report in Asahi) |

The command ABI, the start sequence (`AVE_IOP_Start_Acis` is
instruction-for-instruction `_Castor`'s) and the session code needed no
change. What did:

- `ave_soc_t8103` in `driver/ave_soc.c`, with the iBoot placement read from
  the live ADT through the m1n1 probe patch.
- `me0_node`: the driver powers `venc_me0` through a second holder device.
- `ave_soc.dpe`: the AVE_DPE tunables are per SoC. Applying `Castor_6000` on
  the M1 failed read-back on 69 entries (run C2, first try).
- `ave_soc.pipe_diag`: the session's ~120 diagnostic reads of pipe registers
  use H13C offsets and are off on t8103. They have not been checked against
  the M1's register map; a read outside a real block is a reset (f38).
- `test/ave-overlay-t8103.dts`, overlay `variant=8`.
- m1n1: `0001` exports `/arm-io/ave` `segment-ranges` and `pre-loaded` to
  `/chosen` (this machine has no m1n1 console: no internal display, and
  m1n1 does not drive USB-C DP). `0002` is `tools/m1n1/0001-...` plus the
  entry `{"/arm-io/dart-ave", 3, "/arm-io/ave"}`.

## Runs

Logs in `results/t8103-*` (firewall and audit lines removed). No run reset
the machine.

| run | what | result |
|---|---|---|
| A, B1, B2 | m1n1 self-built, + probe, + DAPF | all booted; B1: `pre-loaded` = 1, two segments |
| C1 | overlay, stages 1-6 | first try refused (no phandle on `venc_me0`); then all domains on, ME0/ME1 PS 0x3ff |
| C2 | stages 7-8, DAPF dump | first try -EIO (Castor tunables); then OK, DAPF [0] TEXT r0 0x11, [1] window r0 0x33 |
| C3 | stages 9-12 | placement as expected, DATA mapped at DVA 0xcc000 |
| C4 | core start | message 1 after 3.7 ms |
| C5 | handshake, Config | 7 channels, firmware log over TERMINAL, Config accepted |
| C6 | Open, Start_AVC | accepted |
| C7 | one I-frame 1280x720 | 4351 bytes, `check_frame.py` MATCH, 27.7 dB |
| C8 | 30 frames via v4l2-ctl | IPPP, High/CABAC, 43.8 dB, 3.1 ms per frame |
| same boot | ffmpeg, RC 8 Mbit/s | 720p 52.0 dB; 1080p60 (padded) 40.9 dB, 6.4 ms per frame |
| same boot | wf-recorder, DP-1, 5 s | 295 frames, picture checked by eye |
| D1 | load, 30 frames, `rmmod` | Halt reaches wfi, CPU_STATUS 0x2e, domains off |
| D2 | load again in the same boot | reset pulse stops the core (0x22), DAPF survives, DATA restored (290120 bytes had changed), 30 frames identical |
| D2 after s2idle | suspend with the driver unloaded, wake, load | two suspend/resume cycles, DAPF unchanged, 30 frames identical |

Power, measured in `~/Projects/m1-power` (2 + 2 alternating runs, mirroring
DP-1 to an iPad while a 1080p60 video plays): 5.164 W with this encoder
against 5.729 W with libx264, -0.565 W (95 % CI -0.667 to -0.473). No
detectable difference on a still screen.

## Not tested

HEVC, P010, B-frames, more than a few hundred frames in one session, system
suspend with the driver loaded (`pm_sleep=2`), two sessions at once, the pipe
diagnostics (`pipe_diag`), recovering a hung firmware.

## Rules on this machine

From `~/Projects/m1-power/CLAUDE.md`, and they apply here: never load the
modules automatically, never touch m1n1 or the ESP without the owner's
explicit go, one step per boot for anything new, and audit every default-on
register write for t6001 assumptions before running a new stage.
[AGENTS.md](../AGENTS.md) applies as written.
