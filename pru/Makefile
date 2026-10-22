# Build the ICSSG images against TI's PRU Software Support Package. PRU_CGT
# and PSSP are set by beagle-linux/scripts/build.sh.
# The three ICSSG0 slice-0 cores run the demo: pru/icssg0/<core> each have
# their own main.c and linker script. In the other three slices (ICSSG0
# slice 1 and both ICSSG1 slices) the PRU and Tx_PRU run the ring,
# pru/ring/main.c, and the RTU runs pru/ring/rtu.c, each built once per core.
# ICSSG1 RTU0's build has RPMsg; the other two ring RTUs have none.
# -DPRU_CORE only hides the host CRC in ai64_shm.h. It does not pick a source.
#
# TRACE=0 builds the trace out of the real-time cores (the PRUs and
# Tx_PRUs). The RTUs always keep theirs.

ifeq ($(PRU_CGT),)
$(error PRU_CGT is not set)
endif
ifeq ($(PSSP),)
$(error PSSP is not set)
endif

TRACE ?= 1
RT_DEFS := -DAI64_TRACE=$(TRACE)

ROOT := $(abspath ..)
BUILD := build
SHM_H := $(ROOT)/zephyr/include/ai64_shm.h
COMMON_H := $(wildcard include/*.h) $(SHM_H) $(ROOT)/zephyr/include/ai64_fabric.h
CFLAGS := -v3 -O2 --display_error_number --endian=little --hardware_mac=on
LFLAGS := --reread_libs --warn_sections --stack_size=0x200 --heap_size=0
INCLUDE := --include_path=$(PRU_CGT)/include \
	--include_path=$(PSSP)/include \
	--include_path=$(PSSP)/include/j721e \
	--include_path=$(ROOT)/zephyr/include \
	--include_path=$(abspath include)

# ai64-icssg<ICSSG>-<role><slice>.elf
RING_CORES := 0-pru-1 0-txpru-1 1-pru-0 1-txpru-0 1-pru-1 1-txpru-1
RTU_CORES := 0-rtu-1 1-rtu-0 1-rtu-1
ring_elf = $(BUILD)/ai64-icssg$(word 1,$(subst -, ,$(1)))-$(word 2,$(subst -, ,$(1)))$(word 3,$(subst -, ,$(1))).elf
RING_ELFS := $(foreach c,$(RING_CORES) $(RTU_CORES),$(call ring_elf,$(c)))

.PHONY: all clean core ring
all: $(BUILD)/ai64-icssg0-pru0.elf $(BUILD)/ai64-icssg0-rtu0.elf $(BUILD)/ai64-icssg0-txpru0.elf \
	$(RING_ELFS)

$(BUILD)/ai64-icssg0-pru0.elf: icssg0/pru0/main.c icssg0/pru0/slip_tx.asm icssg0/pru0/J721E_PRU0.cmd $(COMMON_H)
	$(MAKE) core NAME=pru0 OUT=$@ ASM=icssg0/pru0/slip_tx.asm \
		LINK=icssg0/pru0/J721E_PRU0.cmd EXTRA_LIBS= DEFS="$(RT_DEFS)"

$(BUILD)/ai64-icssg0-rtu0.elf: icssg0/rtu0/main.c icssg0/rtu0/J721E_RTU0.cmd $(COMMON_H)
	$(MAKE) core NAME=rtu0 OUT=$@ ASM= \
		LINK=icssg0/rtu0/J721E_RTU0.cmd \
		EXTRA_LIBS=--library=$(PSSP)/lib/rpmsg_lib.lib

# Tx_PRU0 has 2 KiB of data RAM, so its stack is smaller.
$(BUILD)/ai64-icssg0-txpru0.elf: icssg0/txpru0/main.c icssg0/txpru0/J721E_TX_PRU0.cmd $(COMMON_H)
	$(MAKE) core NAME=txpru0 OUT=$@ ASM= \
		LINK=icssg0/txpru0/J721E_TX_PRU0.cmd EXTRA_LIBS= DEFS="$(RT_DEFS)" \
		LFLAGS="--reread_libs --warn_sections --stack_size=0x100 --heap_size=0"

core:
	mkdir -p $(BUILD)/$(NAME)
	$(PRU_CGT)/bin/clpru $(INCLUDE) --include_path=$(abspath icssg0/$(NAME)) \
		$(CFLAGS) -DPRU_CORE=1 $(DEFS) \
		--obj_directory=$(BUILD)/$(NAME) \
		-fe $(BUILD)/$(NAME)/main.object icssg0/$(NAME)/main.c
	$(if $(ASM),$(PRU_CGT)/bin/clpru $(CFLAGS) \
		-fe $(BUILD)/$(NAME)/asm.object $(ASM),)
	$(PRU_CGT)/bin/clpru $(CFLAGS) -z -i$(PRU_CGT)/lib -i$(PRU_CGT)/include \
		$(LFLAGS) -o $(OUT) $(BUILD)/$(NAME)/main.object \
		$(if $(ASM),$(BUILD)/$(NAME)/asm.object,) \
		-m$(OUT:.elf=.map) $(LINK) --library=libc.a $(EXTRA_LIBS)

# One ring image: G is the ICSSG, ROLE pru, txpru, or rtu, S the slice.
# Each core has 1 or 2 KiB of data RAM, so all three use a small stack,
# except ICSSG1 RTU0: it has RPMsg, which needs more, and room for it.
define RING_RULE
$(call ring_elf,$(1)): ring/main.c ring/rtu.c ring/ring_$(word 2,$(subst -, ,$(1))).cmd \
		ring/ring_rtu_rpmsg.cmd $(COMMON_H)
	$$(MAKE) ring OUT=$$@ G=$(word 1,$(subst -, ,$(1))) ROLE=$(word 2,$(subst -, ,$(1))) \
		S=$(word 3,$(subst -, ,$(1)))
endef
$(foreach c,$(RING_CORES) $(RTU_CORES),$(eval $(call RING_RULE,$(c))))

RING_NAME = icssg$(G)-$(ROLE)$(S)
RING_RPMSG = $(if $(filter rtu-1-0,$(ROLE)-$(G)-$(S)),1)
RING_SRC = $(if $(filter rtu,$(ROLE)),ring/rtu.c,ring/main.c)
RING_DEFS = $(if $(filter rtu,$(ROLE)),$(if $(RING_RPMSG),-DAI64_RPMSG=1),$(RT_DEFS) \
	-DAI64_ROLE_$(shell echo $(ROLE) | tr a-z A-Z)=1)
RING_LINK = ring/ring_$(ROLE)$(if $(RING_RPMSG),_rpmsg).cmd
RING_STACK = $(if $(RING_RPMSG),0x200,0x100)
ring:
	mkdir -p $(BUILD)/$(RING_NAME)
	$(PRU_CGT)/bin/clpru $(INCLUDE) $(CFLAGS) -DPRU_CORE=1 \
		-DAI64_ICSSG=$(G) -DAI64_SLICE=$(S) $(RING_DEFS) \
		--obj_directory=$(BUILD)/$(RING_NAME) \
		-fe $(BUILD)/$(RING_NAME)/main.object $(RING_SRC)
	$(PRU_CGT)/bin/clpru $(CFLAGS) -z -i$(PRU_CGT)/lib -i$(PRU_CGT)/include \
		--reread_libs --warn_sections --stack_size=$(RING_STACK) --heap_size=0 \
		-o $(OUT) $(BUILD)/$(RING_NAME)/main.object \
		-m$(OUT:.elf=.map) $(RING_LINK) --library=libc.a \
		$(if $(RING_RPMSG),--library=$(PSSP)/lib/rpmsg_lib.lib)

clean:
	rm -rf $(BUILD)
