# SPDX-License-Identifier: GPL-2.0-only
# Throwaway bring-up scaffolding. See ave_overlay_mod.c.
obj-m += ave-overlay.o
ave-overlay-y := ave_overlay_mod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: ave_overlay_dtbo.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Build from the no-DART overlay. Building from ave-overlay.dts silently
# reintroduced the DART node for four bring-up attempts and was only caught by
# decoding the committed header - see docs/30-address-translation-bug.md.
ave_overlay_dtbo.h: ave-overlay-nodart.dtbo
	python3 mkhdr.py $< ave_overlay_dtbo $@

ave-overlay-nodart.dtbo: ave-overlay-nodart.dts
	dtc -I dts -O dtb -o $@ $< 2>/dev/null

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ave_overlay_dtbo.h ave-overlay-nodart.dtbo ave-overlay.dtbo
