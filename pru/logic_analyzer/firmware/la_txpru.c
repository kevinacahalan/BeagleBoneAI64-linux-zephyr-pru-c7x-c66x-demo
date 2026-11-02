/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 Tx_PRU0 and Tx_PRU1: each slice's watcher. It reads its PRU's edge
 * records from the ring in shared RAM as they come, as the RTU does, and
 * matches the trigger the RTU passes on from Linux (ai64_la.h): the masked
 * pins coming to a value, or any masked pin changing. Each hit, the edge
 * record that met it, goes into its hit ring, and the RTU puts it in the
 * stream. Like the PRU, it touches nothing outside ICSSG1.
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_la.h"
#include "la_map.h"
#include "la_ipc.h"
#include "la_tm.h"

#define AI64_TRACE_SIZE 128
#if AI64_SLICE == 0
#define AI64_TRACE_NAME "trace:la-txpru0"
#else
#define AI64_TRACE_NAME "trace:la-txpru1"
#endif
#include "ai64_rsc_trace.h"

#define SRING ((volatile uint32_t *)LA_SRING(AI64_SLICE))
#define SWR   (*(volatile uint32_t *)LA_SCTL(AI64_SLICE))

void main(void)
{
	volatile struct la_trig *cfg = LA_TRIG_REC(AI64_SLICE);
	volatile struct la_hits *hits = LA_HITS_REC(AI64_SLICE);
	uint32_t rd, seq = ~0u, mode = AI64_LA_TRIG_OFF, mask = 0, value = 0, hw = 0;
	uint32_t prev = 0, have_prev = 0;

	tm_clear(AI64_SLICE ? ICSSG_TM_TXPRU1 : ICSSG_TM_TXPRU0);
	trace_init();
	hits->wr = 0;
	rd = SWR;
	trace_puts(AI64_SLICE ? "ICSSG1 Tx_PRU1 up: matching PRU1's trigger\n" :
				"ICSSG1 Tx_PRU0 up: matching PRU0's trigger\n");
	for (;;) {
		uint32_t w = SWR;

		if (cfg->seq != seq) {
			seq = cfg->seq;
			mode = cfg->mode;
			mask = cfg->mask;
			value = cfg->value;
		}
		if (w - rd > LA_SRING_SIZE - 64u) {
			/* Behind by a ring: take up from where the PRU is. */
			rd = w;
			have_prev = 0;
			continue;
		}
		while (rd != w) {
			volatile uint32_t *r = SRING + (rd & (LA_SRING_SIZE - 1u)) / 4u;
			uint32_t t = r[0], v = r[1];

			rd += AI64_LA_REC_SIZE;
			if (have_prev && mode != AI64_LA_TRIG_OFF &&
			    (mode == AI64_LA_TRIG_ANY ? ((prev ^ v) & mask) != 0 :
			     (v & mask) == value && (prev & mask) != value)) {
				volatile uint32_t *h = hits->hit[hw % LA_HIT_RING];

				h[0] = t;
				h[1] = v;
				hw++;
				hits->wr = hw;
			}
			prev = v;
			have_prev = 1;
		}
	}
}
