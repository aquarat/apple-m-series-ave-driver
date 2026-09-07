savedcmd_apple-ave.mod := printf '%s\n'   ave_drv.o ave_ipc.o | awk '!x[$$0]++ { print("./"$$0) }' > apple-ave.mod
