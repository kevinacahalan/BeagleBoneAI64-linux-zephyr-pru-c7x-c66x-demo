/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 PRU0 and PRU1: the logic analyzer's samplers, one per slice. Each
 * watches its own GPI pins (ai64_la.h has which) in direct mode, in the
 * mode the RTU set (la_map.h): in edge mode it writes an edge record into
 * its ring in shared RAM at every change (la_sample.asm); in raw mode it
 * samples every 8 ns and passes the samples to the other PRU (la_raw.asm);
 * in conv mode it is that other PRU, and writes the records for them into
 * the raw PRU's ring (la_conv.asm); in dump mode it copies them into the
 * burst buffer in DDR (la_dump.asm). Except in dump mode it touches nothing
 * outside ICSSG1, so nothing outside can slow the loop: each slice's RTU
 * takes the records from its ring.
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_la.h"
#include "la_map.h"
#include "la_ipc.h"
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

extern void la_sample(uint32_t ring, uint32_t mask, uint32_t seq);
extern void la_raw(uint32_t mask, uint32_t t0);
extern void la_raw16_0(uint32_t mask, uint32_t t0);
extern void la_raw16_8(uint32_t mask, uint32_t t0);
extern void la_raw8_0(uint32_t mask, uint32_t t0);
extern void la_raw8_8(uint32_t mask, uint32_t t0);
extern void la_conv(uint32_t ring, uint32_t seq, uint32_t t0);
extern void la_conv16(uint32_t ring, uint32_t seq, uint32_t t0, uint32_t shift);
extern void la_dump(uint32_t buf, uint32_t t0);

void main(void)
{
	uint32_t mask, mode, lane;

	tm_clear(AI64_SLICE ? ICSSG_TM_PRU1 : ICSSG_TM_PRU0);
	trace_init();
	ICSSG_REG(CFG_GPCFG(AI64_SLICE)) = 0;
	/*
	 * The slice's RTU readies the ring (la_map.h), and RTU0 starts IEP0
	 * counting nanoseconds, with no compare resetting it.
	 */
	while (LA_READY_WORD != LA_READY_MAGIC ||
	       !(ICSSG_REG(IEP_GLOBAL_CFG(ICSSG_IEP0)) & IEP_CNT_ENABLE) ||
	       ICSSG_REG(IEP_CMP_CFG(ICSSG_IEP0)) != 0) {
	}
	mask = LA_CFG_REC->mask & AI64_LA_MASK(AI64_SLICE);
	mode = AI64_LA_MODE_OF(LA_CFG_REC->mode);
	if (mode == AI64_LA_MODE_RAW) {
		trace_puts(AI64_SLICE ? "ICSSG1 PRU1 up: raw, every 8 ns, to PRU0\n" :
					"ICSSG1 PRU0 up: raw, every 8 ns, to PRU1\n");
		lane = AI64_LA_LANE_OF(LA_CFG_REC->mode);
		if (lane == AI64_LA_LANE_LO16) {
			la_raw16_0(mask, (uint32_t)&LA_CFG_REC->t0);
		}
		if (lane == AI64_LA_LANE_MID16) {
			la_raw16_8(mask, (uint32_t)&LA_CFG_REC->t0);
		}
		/* A burst's lanes, four samples a word (la_dump.asm). */
		if (lane == AI64_LA_LANE_B0) {
			la_raw8_0(mask, (uint32_t)&LA_CFG_REC->t0);
		}
		if (lane == AI64_LA_LANE_B1) {
			la_raw8_8(mask, (uint32_t)&LA_CFG_REC->t0);
		}
		la_raw(mask, (uint32_t)&LA_CFG_REC->t0);
	}
	if (mode == AI64_LA_MODE_CONV) {
		/* Started before the raw PRU, so waiting when its first block comes. */
		trace_puts(AI64_SLICE ? "ICSSG1 PRU1 up: PRU0's samples to edge records\n" :
					"ICSSG1 PRU0 up: PRU1's samples to edge records\n");
		lane = AI64_LA_LANE_OF(LA_CFG_OTHER->mode);
		if (lane != AI64_LA_LANE_ALL) {
			la_conv16(LA_SRING(1 - AI64_SLICE), LA_CFG_OTHER->resume,
				  (uint32_t)&LA_CFG_OTHER->t0, lane == AI64_LA_LANE_MID16 ? 8u : 0u);
		}
		la_conv(LA_SRING(1 - AI64_SLICE), LA_CFG_OTHER->resume, (uint32_t)&LA_CFG_OTHER->t0);
	}
	if (mode == AI64_LA_MODE_DUMP) {
		/* Started before the raw PRU too. */
		trace_puts(AI64_SLICE ? "ICSSG1 PRU1 up: PRU0's samples to the burst buffer\n" :
					"ICSSG1 PRU0 up: PRU1's samples to the burst buffer\n");
		la_dump(AI64_LA_BURST_ADDR, (uint32_t)&LA_CFG_OTHER->t0);
	}
	trace_puts(AI64_SLICE ? "ICSSG1 PRU1 up: an edge record at each change\n" :
				"ICSSG1 PRU0 up: an edge record at each change\n");
	la_sample(LA_SRING(AI64_SLICE), mask, LA_CFG_REC->resume);
}
