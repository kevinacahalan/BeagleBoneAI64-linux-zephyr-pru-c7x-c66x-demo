/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 RTU0 and RTU1: the demo's fabric nodes 10 and 11 (ai64_fabric.h).
 * Each answers pings, adds to the tally, and joins R5F0_0's tour, so the
 * fabric spans both ICSSGs. The logic analyzer needs neither: PRU0 writes
 * its samples to DDR on its own (la_pru.c).
 *
 * A fabric poll reads DDR and the GTC, through ICSSG1's port to the SoC,
 * the one PRU0's XFR2VBUS writes go through too. PRU0 never waits on it,
 * but the RTU still polls only every 100 us, and in between spins on
 * nothing outside the RTU.
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_shm.h"
#include "la_map.h"
#include "la_tm.h"

#define AI64_TRACE_SIZE 512
#if AI64_SLICE == 0
#define AI64_TRACE_NAME "trace:la-rtu0"
#else
#define AI64_TRACE_NAME "trace:la-rtu1"
#endif
#include "ai64_rsc_trace.h"

#define GTC_LO()  (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))

struct ai64_fab_msg;
static uint32_t fab_action(struct ai64_fab_msg *m);

#define FAB_NODE          (AI64_FAB_G1RTU0 + AI64_SLICE)
#define AI64_FAB_NOW()    GTC_LO()
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_ACTION(m) fab_action(m)
#include "ai64_fabric.h"

/* 100 us at 250 MHz. */
#define POLL_CYCLES 25000u

/* A route's action: none is this node's. */
static uint32_t fab_action(struct ai64_fab_msg *m)
{
	(void)m;
	return 1;
}

void main(void)
{
	tm_clear(AI64_SLICE ? ICSSG_TM_RTU1 : ICSSG_TM_RTU0);
	trace_init();
	fab_init(FAB_NODE);
	trace_puts(AI64_SLICE ? "ICSSG1 RTU1 up: fabric node " : "ICSSG1 RTU0 up: fabric node ");
	trace_u32(FAB_NODE);
	trace_putc('\n');
	for (;;) {
		fab_poll();
		__delay_cycles(POLL_CYCLES);
	}
}
