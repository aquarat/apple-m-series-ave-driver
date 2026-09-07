# SPDX-License-Identifier: GPL-2.0-only
# Throwaway bring-up scaffolding. See ave_overlay_mod.c.
obj-m += ave-overlay.o
ave-overlay-y := ave_overlay_mod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: ave_overlay_dtbo.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ave_overlay_dtbo.h: ave-overlay.dtbo
	python3 mkhdr.py $< ave_overlay_dtbo $@

ave-overlay.dtbo: ave-overlay.dts
	dtc -I dts -O dtb -o $@ $< 2>/dev/null

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ave_overlay_dtbo.h ave-overlay.dtbo
