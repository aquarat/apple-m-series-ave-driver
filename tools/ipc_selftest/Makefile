# SPDX-License-Identifier: GPL-2.0-only
# Userspace test of driver/ave_abi_boot.h. No hardware access.
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -I../../driver
O ?= .

all: $(O)/ipc_selftest

$(O)/ipc_selftest: ipc_selftest.c ../../driver/ave_abi_boot.h ../../driver/ave_version.h
	$(CC) $(CFLAGS) -o $@ ipc_selftest.c

run: $(O)/ipc_selftest
	$(O)/ipc_selftest

clean:
	rm -f $(O)/ipc_selftest

.PHONY: all run clean
