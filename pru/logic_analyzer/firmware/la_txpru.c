/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 Tx_PRU0 and Tx_PRU1: each slice's watcher. For now it only says
 * it is up; the triggers on its PRU's channels come next.
 */

#include <stdint.h>
#include "icssg.h"
#include "la_tm.h"

#define AI64_TRACE_SIZE 128
#if AI64_SLICE == 0
#define AI64_TRACE_NAME "trace:la-txpru0"
#else
#define AI64_TRACE_NAME "trace:la-txpru1"
#endif
#include "ai64_rsc_trace.h"

void main(void)
{
	tm_clear(AI64_SLICE ? ICSSG_TM_TXPRU1 : ICSSG_TM_TXPRU0);
	trace_init();
	trace_puts(AI64_SLICE ? "ICSSG1 Tx_PRU1 up\n" : "ICSSG1 Tx_PRU0 up\n");
	for (;;) {
	}
}
