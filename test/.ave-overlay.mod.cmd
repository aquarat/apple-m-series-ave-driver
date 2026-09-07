savedcmd_ave-overlay.mod := printf '%s\n'   ave_overlay_mod.o | awk '!x[$$0]++ { print("./"$$0) }' > ave-overlay.mod
