# Cross-compiled on the laptop by ./beagle-linux/scripts/build.sh kmod for
# the BeagleBoard kernel, and by Yocto's ai64-la-mod recipe, which sets
# KERNEL_SRC, for the TI one.
obj-m += ai64_la.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
KDIR ?= $(KERNEL_SRC)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
