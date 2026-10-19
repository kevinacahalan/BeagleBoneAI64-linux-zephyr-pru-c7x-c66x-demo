# Build the three ICSSG0 slice-0 images against TI's PRU Software Support
# Package. PRU_CGT and PSSP are set by beagle-linux/scripts/build.sh.
# pru/icssg0/<core> each have their own main.c and linker script.
# -DPRU_CORE only hides the host CRC in ai64_shm.h. It does not pick a source.

ifeq ($(PRU_CGT),)
$(error PRU_CGT is not set)
endif
ifeq ($(PSSP),)
$(error PSSP is not set)
endif

ROOT := $(abspath ..)
BUILD := build
SHM_H := $(ROOT)/zephyr/include/ai64_shm.h
COMMON_H := include/ai64_trace.h include/ai64_rsc_trace.h $(SHM_H)
CFLAGS := -v3 -O2 --display_error_number --endian=little --hardware_mac=on
LFLAGS := --reread_libs --warn_sections --stack_size=0x200 --heap_size=0
INCLUDE := --include_path=$(PRU_CGT)/include \
	--include_path=$(PSSP)/include \
	--include_path=$(PSSP)/include/j721e \
	--include_path=$(ROOT)/zephyr/include \
	--include_path=$(abspath include)

.PHONY: all clean core
all: $(BUILD)/ai64-icssg0-pru0.elf $(BUILD)/ai64-icssg0-rtu0.elf $(BUILD)/ai64-icssg0-txpru0.elf

$(BUILD)/ai64-icssg0-pru0.elf: icssg0/pru0/main.c icssg0/pru0/slip_tx.asm icssg0/pru0/J721E_PRU0.cmd $(COMMON_H)
	$(MAKE) core NAME=pru0 OUT=$@ ASM=icssg0/pru0/slip_tx.asm \
		LINK=icssg0/pru0/J721E_PRU0.cmd EXTRA_LIBS=

$(BUILD)/ai64-icssg0-rtu0.elf: icssg0/rtu0/main.c icssg0/rtu0/resource_table.h icssg0/rtu0/J721E_RTU0.cmd $(COMMON_H)
	$(MAKE) core NAME=rtu0 OUT=$@ ASM= \
		LINK=icssg0/rtu0/J721E_RTU0.cmd \
		EXTRA_LIBS=--library=$(PSSP)/lib/rpmsg_lib.lib

# Tx_PRU0 has 2 KiB of data RAM, so its stack is smaller.
$(BUILD)/ai64-icssg0-txpru0.elf: icssg0/txpru0/main.c icssg0/txpru0/J721E_TX_PRU0.cmd $(COMMON_H)
	$(MAKE) core NAME=txpru0 OUT=$@ ASM= \
		LINK=icssg0/txpru0/J721E_TX_PRU0.cmd EXTRA_LIBS= \
		LFLAGS="--reread_libs --warn_sections --stack_size=0x100 --heap_size=0"

core:
	mkdir -p $(BUILD)/$(NAME)
	$(PRU_CGT)/bin/clpru $(INCLUDE) --include_path=$(abspath icssg0/$(NAME)) \
		$(CFLAGS) -DPRU_CORE=1 \
		--obj_directory=$(BUILD)/$(NAME) \
		-fe $(BUILD)/$(NAME)/main.object icssg0/$(NAME)/main.c
	$(if $(ASM),$(PRU_CGT)/bin/clpru $(CFLAGS) \
		-fe $(BUILD)/$(NAME)/asm.object $(ASM),)
	$(PRU_CGT)/bin/clpru $(CFLAGS) -z -i$(PRU_CGT)/lib -i$(PRU_CGT)/include \
		$(LFLAGS) -o $(OUT) $(BUILD)/$(NAME)/main.object \
		$(if $(ASM),$(BUILD)/$(NAME)/asm.object,) \
		-m$(OUT:.elf=.map) $(LINK) --library=libc.a $(EXTRA_LIBS)

clean:
	rm -rf $(BUILD)
