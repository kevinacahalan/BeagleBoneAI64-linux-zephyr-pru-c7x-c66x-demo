# Build the DSP images with TI's C6000 and C7000 compilers.
# C6X_CGT and C7X_CGT are set by beagle-linux/scripts/build.sh.

ifeq ($(C6X_CGT),)
$(error C6X_CGT is not set)
endif
ifeq ($(C7X_CGT),)
$(error C7X_CGT is not set)
endif

BUILD := build
INC := include
SHARED := ../zephyr/include

.PHONY: all clean
all: $(BUILD)/ai64-c66x.elf $(BUILD)/ai64-c7x.elf

$(BUILD)/ai64-c66x.elf: c66x/main.c c66x/c66x.cmd $(INC)/ai64_dsp_trace.h $(INC)/ai64_dsp_mbox.h $(INC)/ai64_dsp_job.h $(SHARED)/ai64_fabric.h
	mkdir -p $(BUILD)/c66x
	$(C6X_CGT)/bin/cl6x -mv6600 --abi=eabi -O3 --display_error_number \
		--include_path=$(C6X_CGT)/include --include_path=$(INC) --include_path=$(SHARED) \
		--obj_directory=$(BUILD)/c66x -c c66x/main.c
	$(C6X_CGT)/bin/cl6x -mv6600 --abi=eabi -z -i$(C6X_CGT)/lib \
		--reread_libs --warn_sections -m $(BUILD)/ai64-c66x.map \
		-o $@ $(BUILD)/c66x/main.obj c66x/c66x.cmd --library=rts6600_elf.lib

# Warning 70: the resource table stores the 64-bit trace buffer pointer in a
# uint32_t. The buffer is in DDR below 4 GiB, so nothing is lost.
C7X_SRC := c7x/main.c c7x/ai64_c7x_mmu.c c7x/c7x_regs.asm
C7X_OBJ := $(BUILD)/c7x/main.obj $(BUILD)/c7x/ai64_c7x_mmu.obj $(BUILD)/c7x/c7x_regs.obj

$(BUILD)/ai64-c7x.elf: $(C7X_SRC) c7x/ai64_c7x_mmu.h c7x/c7x.cmd $(INC)/ai64_dsp_trace.h $(INC)/ai64_dsp_mbox.h $(INC)/ai64_dsp_job.h $(SHARED)/ai64_fabric.h
	mkdir -p $(BUILD)/c7x
	$(C7X_CGT)/bin/cl7x -mv7100 -O3 --display_error_number --diag_suppress=70 \
		--include_path=$(C7X_CGT)/include --include_path=$(INC) --include_path=$(SHARED) \
		--obj_directory=$(BUILD)/c7x -c $(C7X_SRC)
	$(C7X_CGT)/bin/cl7x -mv7100 -z -i$(C7X_CGT)/lib \
		--reread_libs --warn_sections -m $(BUILD)/ai64-c7x.map \
		-o $@ $(C7X_OBJ) c7x/c7x.cmd --library=libc.a

clean:
	rm -rf $(BUILD)
