/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/
 *
 * Ported from open-pru examples/multicore_scheduler (am243x-lp,
 * icss_g0_pru0_fw) to ICSSG1 PRU0 of the J721E on the BeagleBone AI-64.
 * Linux already runs the ICSSG1 core clock at 250 MHz and muxes P8.17 to
 * PRG1_PWM2_A0 (the overlay), so the clock and pin-mux steps of the
 * original are gone; it logs to its remoteproc trace instead, and clears
 * its compare's status once its task manager is on.
 */

/**
 *  \file   main.c
 *
 *  \brief  Multi-core scheduling configuration and code for PRU0
 */

/* ========================================================================== */
/*                             Include Files                                  */
/* ========================================================================== */
#include <stdint.h>

#define AI64_TRACE_SIZE 256
#define AI64_TRACE_NAME "trace:mcs-pru0"
#include "ai64_rsc_trace.h"

/* ========================================================================== */
/*                          External Function Declarations                    */
/* ========================================================================== */
void FN_MCS_CFG();
/*
 * void main()
 */
void main(void)
{
    trace_init();
    trace_puts("ICSSG1 PRU0 up: configuring IEP0 and the six task managers\n");
    trace_puts("its task, on IEP0 CMP7, sets P8.17 high\n");

    // disable task manager
    asm("   tsen 0");

/* ---------- PRU clock and IEP clock configuration -------------- */
    /* J721E: the ICSSG1 core clock is 250 MHz already, and FN_MCS_CFG()
     * puts IEP0 in sync mode, on that clock. */
/* ---------- pin-mux configuration --------------------- */
    /* J721E: the overlay muxes P8.17 to PRG1_PWM2_A0. */

/* ---------- additional configuration ------------------- */
    /*asssembly routine to configure IEP timer, compare registers and task manager*/
    FN_MCS_CFG();

/* ----------- end of configuration  ----------------------- */
    /* enable task manager */
    asm("   tsen 1");
    /* J721E: a compare that hit before this core armed its task manager
     * left its status set, and no new event comes until it is clear. */
    asm("   sbco &r25, c26, 0x74, 4");

    /* idle loop */
    while(1)
    {

    }
}
