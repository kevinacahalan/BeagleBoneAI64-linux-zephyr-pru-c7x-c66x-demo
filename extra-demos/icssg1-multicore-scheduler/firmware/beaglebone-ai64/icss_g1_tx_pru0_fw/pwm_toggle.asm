; SPDX-License-Identifier: BSD-3-Clause
; Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/

;************************************************************************************
;   File:     pwm_toggle.asm
;
;   Brief:	  contains Multicore scheduling Task
;
;   Ported from open-pru examples/multicore_scheduler (am243x-lp,
;   icss_g0_tx_pru0_fw) to ICSSG1 of the J721E on the BeagleBone AI-64. The
;   task also clears its compare's status: J721E has no PWM_EFC_EN to do
;   it (icss_g1_pru0_fw/pwm_toggle.asm).
;************************************************************************************
	.sect	".text:pwm_toggle"
	.clink
	.global	FN_PWM_INIT_TOGGLE_TXPRU0_TASK
; File includes
    .include  "mcs_regs.inc"
;*************************************************
;   Task : FN_PWM_INIT_TOGGLE_TXPRU0_TASK
;   Description : Sets PWM init to Logic High for configured PIN
;   			  => Configured PIN reads Logic High
;   Assumption : r24 preloaded with toggle value, r25 with the CMP6
;                status bit, PWM out is configured
;   Peak Cycles : 5
;
;*************************************************
FN_PWM_INIT_TOGGLE_TXPRU0_TASK:
    ; GPO toggle using PWM init state
    ; rx.b2 --> Set PIN high
	; rx.b3 --> Set PIN low
    sbco    &r24.b2,c5,ICSSG_PWM2_0_C5, 1
    ; J721E: clear this task's compare, so the next one starts it again
    sbco    &r25, c26, ICSSG_IEP_C26_STATUS, 4

	; return from task
    xin    TM_YIELD_XID, &R0.b3,1
	nop
    nop

;*****************************************************************************
;                                     END
;*****************************************************************************
