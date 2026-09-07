savedcmd_psdump.mod := printf '%s\n'   psdump.o | awk '!x[$$0]++ { print("./"$$0) }' > psdump.mod
