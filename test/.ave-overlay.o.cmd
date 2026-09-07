savedcmd_ave-overlay.o := ld -EL  -maarch64linux -z norelro -z noexecstack --no-warn-rwx-segments   -r -o ave-overlay.o @ave-overlay.mod 
