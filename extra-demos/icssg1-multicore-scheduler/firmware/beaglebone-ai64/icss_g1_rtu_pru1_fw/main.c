/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/
 *
 * Ported from open-pru examples/multicore_scheduler (am243x-lp,
 * icss_g0_rtu_pru1_fw) to ICSSG1 RTU1 of the J721E on the BeagleBone AI-64.
 * It also logs one line to its remoteproc trace before the task manager
 * takes over, preloads r25 for its task (pwm_toggle.asm), and clears
 * its compare's status once its task manager is on.
 */

/**
 *  \file   main.c
 *
 *  \brief  Multi-core scheduling configuration and code for RTU1
 */
/* ========================================================================== */
/*                             Include Files                                  */
/* ========================================================================== */
#include <stdint.h>
#include "mcs_tasks.h"

#define AI64_TRACE_SIZE 128
#define AI64_TRACE_NAME "trace:mcs-rtu1"
#include "ai64_rsc_trace.h"
/* ========================================================================== */
/*                          External Function Declarations                    */
/* ========================================================================== */
void FN_PWM_INIT_TOGGLE_RTU1_TASK();
/* ========================================================================== */
/*                          Macros                                            */
/* ========================================================================== */
#define TM_REG_BASE_RTU1 ICSSG_TM_RTU1

/*
 * void main()
 */
void main(void)
{
/* Note: Event mapping and timer configuration 
*  for all cores are done from PRU0 in this project
*  to keep the configurations in one place and 
*  make any changes to the configuration easier.
*/
    trace_init();
    trace_puts("ICSSG1 RTU1 up: its task, on IEP0 CMP3, sets P8.17 high\n");
/* ---------- task manager configuration ------------------- */
    /* disable task manager */
    asm("   tsen 0");
    /* clear event from previous debug.*/
    HW_WR_REG32((TM_REG_BASE_RTU1+TASKS_MGR_GLOBAL_CFG), 0x0fff);
    asm("   xin     252, &r0.b3,1");
    asm("   nop ");
    asm("   nop ");
    HW_WR_REG32((TM_REG_BASE_RTU1+TASKS_MGR_GLOBAL_CFG), 0x0000);
    /* configure task manager, TS2 is highest priority and can pre-empt TS1
    *  general purpose mode = 2 ,
    *  TS2_S0 task enable  (bit 7)
    */
    HW_WR_REG32((TM_REG_BASE_RTU1+TASKS_MGR_GLOBAL_CFG), 0x0082);
    /* set address of tasks in TS2_0*/
    HW_WR_REG32((TM_REG_BASE_RTU1+TASKS_MGR_TS2_PC_S0), (unsigned int) FN_PWM_INIT_TOGGLE_RTU1_TASK);
    /* pre load for toggling
    *  toggling value for pwm pin (toggling sets POS_INIT and NEG_INT to 0 or 1)
    *  r24.b2 --> Set PIN high
    *  r24.b3 --> Set PIN low
    *  and r25, the status bit of this core's compare, CMP3
    */
    asm("   ldi32 r24, 0x050a8000");
    asm("   ldi32 r25, 0x8");
    /* enable task manager*/
    asm("   tsen 1");
    /* J721E: a compare that hit before this core armed its task manager
     * left its status set, and no new event comes until it is clear. */
    asm("   sbco &r25, c26, 0x74, 4");
    /* idle loop */
    while(1)
    {
    }
}
