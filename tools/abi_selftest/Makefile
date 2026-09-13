# SPDX-License-Identifier: GPL-2.0-only
#
# Userspace self-test of the AVE command builders (driver/ave_cmd.c) against
# the documented per-version evidence tables. Pure computation, no hardware.
#
#     make -C tools/abi_selftest check
#
DRV     := ../../driver
CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror \
           -Ishim -include kshim.h -I$(DRV) -fsanitize=address,undefined

abi_selftest: abi_selftest.c $(DRV)/ave_cmd.c $(DRV)/ave_cmd.h $(DRV)/ave_abi.h kshim.h
	$(CC) $(CFLAGS) -o $@ abi_selftest.c $(DRV)/ave_cmd.c

check: abi_selftest
	./abi_selftest

clean:
	rm -f abi_selftest

.PHONY: check clean
