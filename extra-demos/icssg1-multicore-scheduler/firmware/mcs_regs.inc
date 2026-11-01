; SPDX-License-Identifier: BSD-3-Clause
; Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/
;
; The registers the multicore scheduler uses, at the local addresses every
; ICSSG core sees (J721E PRU user's guide, SPRUJ61). open-pru takes these
; from its icss_regs.inc, icss_cfg_regs.inc, icss_iep_regs.inc and
; icss_xfer_defines.inc; this port defines only what it needs.

PRUx_CFG_BASE		.set	0x00026000
ICSS_CFG_IEPCLK		.set	0x30
PRUx_IEP0_BASE		.set	0x0002E000
ICSS_IEP_GLOBAL_CFG_REG	.set	0x00
ICSS_IEP_COUNT_REG	.set	0x10
ICSS_IEP_CMP_CFG_REG	.set	0x70
ICSS_IEP_CMP_STATUS_REG	.set	0x74
ICSS_IEP_CMP0_REG	.set	0x78
ICSS_IEP_CMP1_REG	.set	0x80
ICSS_IEP_CMP2_REG	.set	0x88
ICSS_IEP_CMP3_REG	.set	0x90
ICSS_IEP_CMP4_REG	.set	0x98
ICSS_IEP_CMP5_REG	.set	0xA0
ICSS_IEP_CMP6_REG	.set	0xA8
ICSS_IEP_CMP7_REG	.set	0xB0

; The task manager of each core
TM_REG_BASE_PRU0	.set	0x0002A000
TM_REG_BASE_RTU0	.set	0x0002A100
TM_REG_BASE_PRU1	.set	0x0002A200
TM_REG_BASE_RTU1	.set	0x0002A300
TM_REG_BASE_TXPRU0	.set	0x0002A400
TM_REG_BASE_TXPRU1	.set	0x0002A500
TASKS_MGR_GLOBAL_CFG	.set	0x00
TASKS_MGR_TS2_PC_S0	.set	0x1C
TASKS_MGR_TS2_GEN_CFG1	.set	0x40

; XFR IDs: scratchpad bank 0, and the task manager's yield
PRU_SPAD_B0_XID		.set	10
TM_YIELD_XID		.set	252

; CFG: the PWM2 set's trip register, and ICSSG_PWM2_0 from constant C5
; (CFG + 0x100), the state register whose initial values the tasks write.
ICSSG_PWM2		.set	0x138
ICSSG_PWM2_0_C5		.set	0x58
ICSSG_IEP_C26_STATUS	.set	ICSS_IEP_CMP_STATUS_REG
