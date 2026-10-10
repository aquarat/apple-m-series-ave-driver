# SPDX-License-Identifier: GPL-2.0-only
#
# Userspace self-test of the live-ADT parser (driver/ave_adt.c, docs/99):
# a synthetic ADT with known answers, malformed and truncated copies of it
# (every one must be refused without reading out of bounds; ASan/UBSan
# watch), and, if present, the j314c restore ADT from data/blobs.
# Pure computation, no hardware.
#
#     make -C tools/adt_selftest check
#
DRV     := ../../driver
CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror \
           -Ishim -include kshim.h -I$(DRV) -fsanitize=address,undefined

adt_selftest: adt_selftest.c $(DRV)/ave_adt.c $(DRV)/ave_adt.h kshim.h
	rm -f $@
	$(CC) $(CFLAGS) -o $@ adt_selftest.c $(DRV)/ave_adt.c

check: adt_selftest
	./adt_selftest ../../data/blobs/macos-13.5/adt.bin

clean:
	rm -f adt_selftest

.PHONY: check clean
