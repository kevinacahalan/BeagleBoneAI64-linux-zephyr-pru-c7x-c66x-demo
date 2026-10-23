# SPDX-License-Identifier: BSD-3-Clause
# ICSSG1 PRU0: its multicore scheduler task.
NAME := ai64-icssg1-pru0
SRCS := ../main.c ../pwm_toggle.asm
DFLAGS := $(RT_DEFS) $(if $(MCS_STEP_NS),--asm_define=MCS_STEP_NS=$(MCS_STEP_NS)) \
	$(if $(MCS_PERIOD_NS),--asm_define=MCS_PERIOD_NS=$(MCS_PERIOD_NS))

include ../../../../../pru_rules.mak
