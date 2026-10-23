# SPDX-License-Identifier: MIT
# ICSSG0 RTU0: slice 0's talking core, with RPMsg (pwm_rtu.c).
NAME := ai64-icssg0-rtu0
SRCS := ../../../pwm_rtu.c
DFLAGS := -DAI64_SLICE=0 -DAI64_RPMSG=1
LIBS := --library=$(PSSP)/lib/rpmsg_lib.lib
# RPMsg needs the bigger stack.
STACK_SIZE := 0x200

include ../../../../../pru_rules.mak
