# SPDX-License-Identifier: GPL-2.0-only
# Throwaway bring-up scaffolding. See ave_overlay_mod.c.
obj-m += ave-overlay.o
ave-overlay-y := ave_overlay_mod.o
obj-m += physdump.o
# docs/44 E1: read-only ISP RVBAR/CPU/DAPF peek. See isp_peek.c.
obj-m += isp_peek.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: ave_overlay_dtbo.h ave_overlay_noiommu_dtbo.h ave_overlay_e2_dtbo.h \
     ave_overlay_e3_dtbo.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Build from the no-DART overlay. Building from ave-overlay.dts silently
# reintroduced the DART node for four bring-up attempts and was only caught by
# decoding the committed header - see docs/30-address-translation-bug.md.
ave_overlay_dtbo.h: ave-overlay-nodart.dtbo
	python3 mkhdr.py $< ave_overlay_dtbo $@

ave-overlay-nodart.dtbo: ave-overlay-nodart.dts
	dtc -I dts -O dtb -o $@ $< 2>/dev/null

# The no-IOMMU variant, selected at insmod time (variant=1) rather than at
# build time, so the choice of risk level is made when the module is loaded
# and not baked in here.
ave_overlay_noiommu_dtbo.h: ave-overlay-noiommu.dtbo
	python3 mkhdr.py $< ave_overlay_noiommu_dtbo $@

ave-overlay-noiommu.dtbo: ave-overlay-noiommu.dts
	dtc -I dts -O dtb -o $@ $< 2>/dev/null

# docs/44 E2/E3 variants (variant=2 and variant=3): the variant=1 and variant=0
# nodes with "cpudart" and "dapf" reg entries appended.
ave_overlay_e2_dtbo.h: ave-overlay-e2.dtbo
	python3 mkhdr.py $< ave_overlay_e2_dtbo $@

ave-overlay-e2.dtbo: ave-overlay-e2.dts
	dtc -I dts -O dtb -o $@ $< 2>/dev/null

ave_overlay_e3_dtbo.h: ave-overlay-e3.dtbo
	python3 mkhdr.py $< ave_overlay_e3_dtbo $@

ave-overlay-e3.dtbo: ave-overlay-e3.dts
	dtc -I dts -O dtb -o $@ $< 2>/dev/null

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ave_overlay_dtbo.h ave_overlay_noiommu_dtbo.h \
	      ave_overlay_e2_dtbo.h ave_overlay_e3_dtbo.h \
	      ave-overlay-nodart.dtbo ave-overlay-noiommu.dtbo ave-overlay.dtbo \
	      ave-overlay-e2.dtbo ave-overlay-e3.dtbo
