# SPDX-License-Identifier: MIT
#
# The rules every core's makefile shares, in the way of open-pru's
# pru_rules.mak. A project keeps one makefile per core, in
# pru/<project>/firmware/<board>/<core>/ti-pru-cgt/, which sets:
#
#   NAME        the image: pru/build/$(NAME).elf
#   SRCS        its C and assembly sources, relative to that directory
#   DFLAGS      its defines; a real-time core adds $(RT_DEFS)
#   LIBS        more libraries, such as PSSP's rpmsg_lib.lib
#   STACK_SIZE  0x100 unless it says otherwise
#   OPT         -O3 unless it says otherwise
#
# and then includes this file. The core's linker.cmd is next to its
# makefile. Headers come from the core's directory, the project's firmware
# directory, pru/common, and zephyr/include; linker.cmd can #include the
# project's memory map from the firmware directory too.
#
# PRU_CGT and PSSP come from the command line or the environment
# (beagle-linux/scripts/build.sh sets them). TRACE=0 builds the trace out
# of the real-time cores. After linking, each image's memory use is printed
# and checked against BUDGET percent.

ifeq ($(PRU_CGT),)
$(error PRU_CGT is not set)
endif
ifeq ($(PSSP),)
$(error PSSP is not set)
endif

PRU_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT := $(abspath $(PRU_ROOT)/..)
CORE_DIR := $(abspath $(CURDIR)/..)
FW_DIR := $(abspath $(CURDIR)/../../..)
BUILD := $(PRU_ROOT)/build
OBJ := $(BUILD)/obj/$(NAME)
ELF := $(BUILD)/$(NAME).elf
MAP := $(BUILD)/$(NAME).map

STACK_SIZE ?= 0x100
OPT ?= -O3
# Silicon version, as in open-pru: 3 for PRU-ICSS, 4 for PRU_ICSSG. The
# task manager instructions (tsen) and the XFR set need 4.
PRU_VERSION ?= 4
TRACE ?= 1
BUDGET ?= 95
RT_DEFS := -DAI64_TRACE=$(TRACE)

CFLAGS := -v$(PRU_VERSION) $(OPT) --display_error_number --endian=little --hardware_mac=on -DPRU_CORE=1
INCLUDE := --include_path=$(PRU_CGT)/include \
	--include_path=$(PSSP)/include \
	--include_path=$(PSSP)/include/j721e \
	--include_path=$(ROOT)/zephyr/include \
	--include_path=$(PRU_ROOT)/common \
	--include_path=$(FW_DIR) \
	--include_path=$(CORE_DIR)
LFLAGS := --reread_libs --warn_sections --stack_size=$(STACK_SIZE) --heap_size=0

HDRS := $(wildcard $(PRU_ROOT)/common/*.h $(FW_DIR)/*.h $(CORE_DIR)/*.h $(CORE_DIR)/*.inc) \
	$(ROOT)/zephyr/include/ai64_shm.h $(ROOT)/zephyr/include/ai64_fabric.h
OBJS := $(addprefix $(OBJ)/,$(addsuffix .object,$(basename $(notdir $(SRCS)))))

vpath %.c $(sort $(dir $(SRCS)))
vpath %.asm $(sort $(dir $(SRCS)))

.PHONY: all clean
all: $(ELF)

$(OBJ):
	mkdir -p $@

$(OBJ)/%.object: %.c $(HDRS) | $(OBJ)
	$(PRU_CGT)/bin/clpru $(INCLUDE) $(CFLAGS) $(DFLAGS) --obj_directory=$(OBJ) -fe $@ $<

$(OBJ)/%.object: %.asm $(HDRS) | $(OBJ)
	$(PRU_CGT)/bin/clpru $(INCLUDE) $(CFLAGS) $(DFLAGS) --obj_directory=$(OBJ) -fe $@ $<

# The map's MEMORY CONFIGURATION: name, origin, length, used (hex). Only the
# core's own RAM counts: IMEM and DMEM*. A peripheral region such as
# PRU_INTC shows as full because its register struct is placed there.
$(ELF): $(OBJS) linker.cmd $(FW_DIR)/*.h
	$(PRU_CGT)/bin/clpru $(CFLAGS) -z -i$(PRU_CGT)/lib -i$(PRU_CGT)/include -i$(FW_DIR) \
		$(LFLAGS) -o $@ $(OBJS) -m$(MAP) linker.cmd --library=libc.a $(LIBS)
	@awk -v name=$(NAME) -v budget=$(BUDGET) ' \
		function hex(s,   i, c, v) { v = 0; s = tolower(s); \
			for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)); v = v * 16 + c - 1 } \
			return v } \
		/^MEMORY CONFIGURATION/ { on = 1 } /^SEGMENT ALLOCATION/ { on = 0 } \
		on && $$1 ~ /^(IMEM|DMEM)/ && $$4 ~ /^[0-9a-f]+$$/ { \
			len = hex($$3); used = hex($$4); pct = int(100 * used / len); \
			printf "%-22s %-10s %5d of %5d bytes, %3d%%\n", name, $$1, used, len, pct; \
			if (pct > budget) { printf "%s: %s is over the %d%% budget\n", name, $$1, budget; bad = 1 } } \
		END { exit bad }' $(MAP)

clean:
	rm -rf $(OBJ) $(ELF) $(MAP)
