If Fedora/Asahi Linux no longer boots after the m1n1 (AVE DAPF) experiments
===========================================================================

Symptoms: black screen, a hang after the logo, or the Mac rebooting over and
over, before U-Boot/GRUB appears.

1. Shut down. Hold the power button until "Loading startup options" appears.
   Choose Macintosh HD (macOS), or Options (recoveryOS) if macOS won't start.
   In recoveryOS: Utilities -> Terminal.

2. Mount this partition (label "EFI - FEDRA", ~500 MB):

     macOS:       sudo diskutil mount 89A77CF4-32BA-4A03-8BCA-DB0F62925CA4
     recoveryOS:  diskutil mount 89A77CF4-32BA-4A03-8BCA-DB0F62925CA4

   (If that fails: diskutil list, find "EFI - FEDRA", usually disk0s4, then
    diskutil mount disk0s4.)

3. Run the restore script:

     macOS:       sudo sh "/Volumes/EFI - FEDRA/m1n1/restore-m1n1.sh"
     recoveryOS:  sh "/Volumes/EFI - FEDRA/m1n1/restore-m1n1.sh"

   (If the volume mounted elsewhere, `diskutil info disk0s4 | grep "Mount Point"`
    shows the path; use it in place of "/Volumes/EFI - FEDRA".)

   It verifies boot.bin.pre-ave (sha256 2227cf97...94f7), keeps the current
   boot.bin as boot.bin.failed, copies the known-good file over boot.bin and
   verifies the result. It changes nothing if the backup is missing or wrong.

4. Shut down, hold the power button, choose the Fedora/Asahi volume.

Manual equivalent:  cd "/Volumes/EFI - FEDRA/m1n1" && cp boot.bin.pre-ave boot.bin
Last resort:        cp boot.bin.old boot.bin   (older stage 2, Sep 3)

Details: ~/Projects/apple-ave-driver/docs/50-m1n1-ave-dapf.md
