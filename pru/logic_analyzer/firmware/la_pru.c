/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 PRU0 and PRU1: the logic analyzer's samplers, one per slice. Each
 * watches its own GPI pins (ai64_la.h has which) in direct mode, and writes
 * an edge record into its ring in shared RAM at every change
 * (la_sample.asm). It touches nothing outside ICSSG1, so nothing outside
 * can slow the loop: its slice's RTU takes the records from there.
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_la.h"
#include "la_map.h"
#include "la_tm.h"

#define AI64_TRACE_SIZE 256
#if AI64_SLICE == 0
#define AI64_TRACE_NAME "trace:la-pru0"
#else
#define AI64_TRACE_NAME "trace:la-pru1"
#endif
#include "ai64_rsc_trace.h"

/* ICSSG_GPCFG0_REG and ICSSG_GPCFG1_REG: 0 is direct input and output. */
#define CFG_GPCFG(s) (ICSSG_CFG + 0x08u + 4u * (s))

extern void la_sample(uint32_t ring, uint32_t ctl, uint32_t mask);

void main(void)
{
	tm_clear(AI64_SLICE ? ICSSG_TM_PRU1 : ICSSG_TM_PRU0);
	trace_init();
	ICSSG_REG(CFG_GPCFG(AI64_SLICE)) = 0;
	/* RTU0 starts IEP0 counting nanoseconds, with no compare resetting it. */
	while (!(ICSSG_REG(IEP_GLOBAL_CFG(ICSSG_IEP0)) & IEP_CNT_ENABLE) ||
	       ICSSG_REG(IEP_CMP_CFG(ICSSG_IEP0)) != 0) {
	}
	trace_puts(AI64_SLICE ? "ICSSG1 PRU1 up: sampling its 9 channels\n" :
				"ICSSG1 PRU0 up: sampling its 10 channels\n");
	la_sample(LA_SRING(AI64_SLICE), LA_SCTL(AI64_SLICE), AI64_LA_MASK(AI64_SLICE));
}
