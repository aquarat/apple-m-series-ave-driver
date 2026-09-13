savedcmd_apple-ave.mod := printf '%s\n'   ave_drv.o ave_ipc.o ave_fw.o ave_version.o ave_cmd.o ave_dapf.o | awk '!x[$$0]++ { print("./"$$0) }' > apple-ave.mod
