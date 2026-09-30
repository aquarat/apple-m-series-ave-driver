PMP experiment on the M1 Pro (MacBookPro18,3, j314s) - 2026-09-29, docs/87 §7
============================================================================

boot.bin now carries a t6000-j314s device tree built with APPLE_USE_PMP,
so m1n1 starts the PMP coprocessor. Everything else in boot.bin (m1n1
1.6.1, the other device trees, u-boot, config) is stock Fedora.

boot.bin.pre-pmp is the stock stage 2 that was live before
(sha256 6522d07f53617809d090341795bdadb20256e9c24788bfad146257e6ec3e634e,
5019314 bytes). restore-pre-pmp-j314s.sh checks that hash, keeps the
current boot.bin as boot.bin.failed-pmp, and puts boot.bin.pre-pmp back.

If Linux still boots:
     sudo sh /boot/efi/m1n1/restore-pre-pmp-j314s.sh     then reboot
  or simply:  sudo update-m1n1   (rebuilds a stock boot.bin)

If Linux no longer boots (black screen, hang after the logo, or a reboot
loop before U-Boot/GRUB):

1. Shut down. Hold the power button until "Loading startup options"
   appears. Choose macOS if installed, or Options (recoveryOS). In
   recoveryOS: Utilities -> Terminal (root, no sudo needed).

2. Find and mount this partition: the ~500 MB EFI partition labelled
   "EFI - <name>" (on Linux: lsblk -o NAME,LABEL,PARTUUID shows the label
   and PARTUUID; write them into the copy of this file on the ESP), normally
   the 4th partition on the internal disk (disk0s4):
     diskutil list
     diskutil mount <PARTUUID>
     diskutil info  <PARTUUID> | grep "Mount Point"

3. Restore (prefix with sudo in macOS):
     sh "/Volumes/EFI - <name>/m1n1/restore-pre-pmp-j314s.sh"
   (use the Mount Point shown in step 2 if it differs).
   Manual equivalent:  cd "/Volumes/EFI - <name>/m1n1" && cp boot.bin.pre-pmp boot.bin

4. diskutil unmount <PARTUUID>, shut down,
   hold the power button and choose Fedora/Asahi.

Second fallback: boot.bin.old in the same directory is the stage 2 from
before the last update-m1n1 run. Since 2026-09-30 11:3x that is the
source-built APPLE_USE_PMP stage 2 (sha256 134f8b0b...), which booted twice.
The current boot.bin (6dc0383e...) carries the same DTB made by
asahi-fleet-kernel's dtb-fixup instead of a kernel build.
