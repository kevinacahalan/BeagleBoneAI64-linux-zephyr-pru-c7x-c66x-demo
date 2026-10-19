# Cross-compiled on the PC by ./beagle-linux/scripts/build.sh kmod
obj-m += ti_k3_r5_remoteproc.o
ccflags-y += -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
