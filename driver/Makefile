# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree build. Requires the matching kernel-devel package:
#
#     sudo dnf install kernel-devel-$(uname -r)
#     make -C driver
#
obj-m += apple-ave.o
apple-ave-y := ave_drv.o ave_ipc.o ave_fw.o ave_version.o ave_cmd.o ave_dapf.o ave_session.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
