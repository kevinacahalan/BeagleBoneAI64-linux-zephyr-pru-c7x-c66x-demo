; SPDX-License-Identifier: BSD-3-Clause
; Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/

;************************************************************************************
;   File:     pwm_toggle.asm
;
;   Brief:	  contains Multicore scheduling Task, it's configuration macros and function
;
;   Ported from open-pru examples/multicore_scheduler (am243x-lp, icss_g0_pru0_fw)
;   to ICSSG1 of the J721E on the BeagleBone AI-64. What changed:
;     - The ICSSG1 core clock is already 250 MHz, and Linux muxes the pin
;       (P8.17, PRG1_PWM2_A0), so main.c sets neither.
;     - J721E has no PWM_EFC_EN (ICSSG_SA_MX_REG bit 16 is reserved): an IEP
;       compare's status stays set until software clears it, and the task
;       manager only sees the next event once it is clear. So each task
;       clears its own compare's status bit (r25, preloaded), and FN_MCS_CFG
;       does not set PWM_EFC_EN.
;     - FN_MCS_CFG maps every core's task manager event before it starts
;       IEP0, not after: with no auto-clear, a compare that fires before
;       its core's task manager listens would stall that task for good.
;       Two periods after the start it clears the task flags once more,
;       for a restart under running tasks.
;     - The schedule's step and period can be set from make: MCS_STEP_NS
;       (default 4, one core clock: TI's back-to-back schedule) and
;       MCS_PERIOD_NS (default 100).
;************************************************************************************
	.sect	".text:pwm_toggle"
	.clink
	.global	FN_PWM_INIT_TOGGLE_PRU0_TASK
	.global	||FN_MCS_CFG||

; File includes
    .include  "mcs_regs.inc"

; Macros

; increment value for configured clock
MS_TIMER_INC	.set	4 ; 250 Mhz, 4ns

	.if	!$isdefed("MCS_STEP_NS")
MCS_STEP_NS	.set	MS_TIMER_INC
	.endif
	.if	!$isdefed("MCS_PERIOD_NS")
MCS_PERIOD_NS	.set	100
	.endif

; set CMP0 time to desired time period in ns
	; (COUNT - increment value) is set as count starts from 0
MS_CMP0_TIME	.set	(MCS_PERIOD_NS-MS_TIMER_INC) ; cycle time
MS_CMP1_TIME	.set	2000   ; CMP1 time
MS_CMP2_TIME	.set	(2*MCS_STEP_NS)  ; CMP2 time
MS_CMP3_TIME	.set	(3*MCS_STEP_NS) ; CMP3 time
MS_CMP4_TIME	.set	(4*MCS_STEP_NS) ; CMP4 time
MS_CMP5_TIME	.set	(5*MCS_STEP_NS) ; CMP5 time
MS_CMP6_TIME	.set	(6*MCS_STEP_NS) ; CMP6 time
MS_CMP7_TIME	.set	(1*MCS_STEP_NS)  ; CMP7 time

;relevant event mappings
IEP0_CMP_0	.set	(16+0)
IEP0_CMP_1	.set	(16+1)
IEP0_CMP_2	.set	(16+2)
IEP0_CMP_3	.set	(16+3)
IEP0_CMP_4	.set	(16+4)
IEP0_CMP_5	.set	(16+5)
IEP0_CMP_6	.set	(16+6)
IEP0_CMP_7	.set	(16+7)

TRG_EVENT_PRU0				.set IEP0_CMP_7
TRG_EVENT_PRU1				.set IEP0_CMP_2
TRG_EVENT_RTU1				.set IEP0_CMP_3
TRG_EVENT_RTU0				.set IEP0_CMP_4
TRG_EVENT_TXPRU1			.set IEP0_CMP_5
TRG_EVENT_TXPRU0			.set IEP0_CMP_6

; toggling value for pwm pin (toggling sets POS_INIT and NEG_INT to 0 or 1)
; rx.b2 --> Set PIN high
; rx.b3 --> Set PIN low
TOGGLE_VAL	.set	0x050a8000


;*************************************************
;   Function : FN_MCS_CFG
;   Description:
;       One time configurations and pre-loads for Multi-core Scheduling:
;		1. Configures task manager and maps tasks in all cores to events
;		2. Configures IEP timer and compare register times, and starts it
;		3. Pre-loads r24 with value required for toggling task, and r25
;		   with the status bit of PRU0's compare
;		3. Sets address of PRU0 task
;		4. Force PWM trip mode
;
;*************************************************
.asmfunc
||FN_MCS_CFG||:
	xout PRU_SPAD_B0_XID, &r3.w2, 2	; save return PC to SPAD
;* ----------- Task Manager configuration  ----------------------- *
	; (J721E: before the IEP runs. A compare that hits before its core's
	; task manager has its event leaves its status set, and with no
	; PWM_EFC_EN nothing clears it, so that task would never run.)
	; disable task manager
	tsen 	0
	; clear event from previous debug
	ldi32 	r4, TM_REG_BASE_PRU0
	ldi		r5, 0x0fff
	sbbo 	&r5, r4, TASKS_MGR_GLOBAL_CFG, 4
    xin		TM_YIELD_XID, &r0.b3,1
    nop
    nop
	ldi		r5, 0x0000
	sbbo 	&r5, r4, 0, 4
	; configure task manager general purpose mode = 2 , TS2_S0 task enable (bit 7)
	ldi		r5, 0x0082
	sbbo 	&r5, r4, TASKS_MGR_GLOBAL_CFG, 4
	; set address of task in TS2_0
	ldi     r5.w0, $CODE(FN_PWM_INIT_TOGGLE_PRU0_TASK)
    sbbo    &r5.w0, r4, TASKS_MGR_TS2_PC_S0, 4
    ; set TS2_S0 trigger for all cores, TS2_S0 (bit 7-0)
    ;PRU0
    ldi		r5, TRG_EVENT_PRU0
    sbbo    &r5, r4, TASKS_MGR_TS2_GEN_CFG1, 4
    ;PRU1
    ldi32 	r4, TM_REG_BASE_PRU1
    ldi		r5, TRG_EVENT_PRU1
    sbbo    &r5, r4, TASKS_MGR_TS2_GEN_CFG1, 4
    ;RTU0
    ldi32 	r4, TM_REG_BASE_RTU0
    ldi		r5, TRG_EVENT_RTU0
    sbbo    &r5, r4, TASKS_MGR_TS2_GEN_CFG1, 4
    ;RTU1
    ldi32 	r4, TM_REG_BASE_RTU1
    ldi		r5, TRG_EVENT_RTU1
    sbbo    &r5, r4, TASKS_MGR_TS2_GEN_CFG1, 4
    ;TXPRU0
    ldi32 	r4, TM_REG_BASE_TXPRU0
    ldi		r5, TRG_EVENT_TXPRU0
    sbbo    &r5, r4, TASKS_MGR_TS2_GEN_CFG1, 4
    ;TXPRU1
    ldi32 	r4, TM_REG_BASE_TXPRU1
    ldi		r5, TRG_EVENT_TXPRU1
    sbbo    &r5, r4, TASKS_MGR_TS2_GEN_CFG1, 4

;* ----------- iep configuration  ----------------------- *
	; Configure ICSSG_IEPCLK_REG Register to run in sync mode
	; IEP_OCP_CLK_EN = 1 (bit 0 = 1) to set the source of the IEP Clock to ICSSGn_CORE_CLK
	ldi32 	r4, PRUx_CFG_BASE
	ldi 	r5, 0x01
	sbbo 	&r5.b0, r4, ICSS_CFG_IEPCLK, 1
	; (AM243x: ICSSG_SA_MX_REG PWM_EFC_EN. J721E has none; the tasks
	; clear their compare's status instead.)
	; reset iep0 timer
	ldi32 	r4, PRUx_IEP0_BASE
	ldi 	r5, 0x20
	sbbo 	&r5.b0, r4, 0, 1
	ldi 	r5, 0x0
	sbbo 	&r5.b0, r4, ICSS_IEP_COUNT_REG, 1
	; clear the compare status a last run left
	ldi32 	r5, 0xffff
	sbbo 	&r5, r4, ICSS_IEP_CMP_STATUS_REG, 4
	; set time period of toggling pattern
	ldi32 	r5, MS_CMP0_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP0_REG, 4
	; set PRU0 trigger time
	ldi32 	r5, MS_CMP7_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP7_REG, 4
	; set PRU1 trigger time
	ldi32 	r5, MS_CMP2_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP2_REG, 4
	; set RTU1 trigger time
	ldi32 	r5, MS_CMP3_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP3_REG, 4
	; set RTU0 trigger time
	ldi32 	r5, MS_CMP4_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP4_REG, 4
	; set TXPRU0 trigger time
	ldi32 	r5, MS_CMP5_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP5_REG, 4
	; set TXPRU1 trigger time
	ldi32 	r5, MS_CMP6_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP6_REG, 4
	; set CMP1 time to a high value(since PWM init toggle is used)
	ldi32 	r5, MS_CMP1_TIME
	sbbo 	&r5, r4, ICSS_IEP_CMP1_REG, 4
	; Enable cmp 0,2,3,4,5,6,7 and wrap around
	ldi 	r5, 0x1fb
	sbbo 	&r5, r4, ICSS_IEP_CMP_CFG_REG, 4
	; set default increment and enable count
	; set default increment to 3 for 333 MHz , 4 for 250 MHz
	ldi 	r5, 0x441
	sbbo 	&r5, r4, ICSS_IEP_GLOBAL_CFG_REG, 2
	; (J721E: a restart stops the counter under tasks that are running,
	; and a compare's flag can then be set without an event its task
	; manager sees. Two periods in, clear the six task flags once more.)
	ldi32 	r5, (MCS_PERIOD_NS/4)
mcs_wait:
	sub 	r5, r5, 1
	qbne 	mcs_wait, r5, 0
	ldi 	r5, 0xfc
	sbbo 	&r5, r4, ICSS_IEP_CMP_STATUS_REG, 4
   	; pre-load for toggling, and PRU0's compare status bit (CMP7)
   	ldi32 r24, TOGGLE_VAL
   	ldi32 r25, (1 << 7)
   	; force PWM trip mode
   	ldi32 	r4, PRUx_CFG_BASE
   	ldi32 	r5, 0x0003ff00
   	ldi32 	r6, ICSSG_PWM2
   	sbbo 	&r5, r4, r6, 4
   	ldi32 	r5, 0x0004ff00
   	sbbo 	&r5, r4, r6, 4

    xin PRU_SPAD_B0_XID, &r3.w2, 2 ; restore PC
	jmp		r3.w2
.endasmfunc

;*************************************************
;   Task : FN_PWM_INIT_TOGGLE_PRU0_TASK
;   Description : Sets PWM init to Logic High for configured PIN
;   			  => Configured PIN reads Logic High
;   Assumption : r24 preloaded with toggle value, r25 with the CMP7
;                status bit, PWM out is configured
;   Peak Cycles : 5
;
;*************************************************
FN_PWM_INIT_TOGGLE_PRU0_TASK:

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
