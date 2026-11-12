# SPDX-License-Identifier: GPL-2.0-or-later
ifneq ($(KERNELRELEASE),)
obj-m := h868.o
h868-y := h868_usb.o h868_frontend.o
ccflags-y += -I$(srctree)/drivers/media/tuners
else
KDIR ?= /lib/modules/$(shell uname -r)/build
-include $(KDIR)/include/config/auto.conf
ifeq ($(CONFIG_CC_IS_CLANG),y)
KBUILD_TOOLCHAIN := LLVM=1
endif

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_TOOLCHAIN) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
