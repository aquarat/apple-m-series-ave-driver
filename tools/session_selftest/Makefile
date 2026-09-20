# SPDX-License-Identifier: GPL-2.0-only
#
# Userspace self-test of the opening command sequence (Config -> Open ->
# Start_AVC) that driver/ave_session.c sends. It rebuilds those three
# commands with the same parameters, for both firmware ABIs, against the pure
# builders in driver/ave_cmd.c, and checks sizes, ids and reply decoding.
# Pure computation, no hardware.
#
#     make -C tools/session_selftest check
#
DRV     := ../../driver
CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror \
           -Ishim -include kshim.h -I$(DRV) -fsanitize=address,undefined

session_selftest: session_selftest.c $(DRV)/ave_cmd.c $(DRV)/ave_cmd.h $(DRV)/ave_abi.h kshim.h
	rm -f $@
	$(CC) $(CFLAGS) -o $@ session_selftest.c $(DRV)/ave_cmd.c

check: session_selftest
	./session_selftest

clean:
	rm -f session_selftest

.PHONY: check clean
