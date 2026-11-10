/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 PRU0: the logic analyzer. It samples its GPI pins in direct mode
 * at the rate and lane in its setup (ai64_la.h), which Linux writes before
 * it starts it (ai64 la mode), and writes the samples to the ring in DDR
 * through XFR2VBUS (la_cap.asm), on its own: no other ICSSG1 core takes
 * part. It starts IEP0, the analyzer's clock, if nothing has: nanoseconds
 * on the 250 MHz core clock, with no compare resetting it.
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_la.h"
#include "la_map.h"
#include "la_tm.h"

#define AI64_TRACE_SIZE 256
#define AI64_TRACE_NAME "trace:la-pru0"
#include "ai64_rsc_trace.h"

/* ICSSG_GPCFG0_REG: 0 is direct input and output. */
#define CFG_GPCFG0 (ICSSG_CFG + 0x08u)
#define IEP        ICSSG_IEP0
#define CFG        ((volatile struct ai64_la_cfg *)AI64_LA_CFG)

typedef void cap_fn(uint32_t mask, uint32_t key, uint32_t t0, uint32_t k);
extern cap_fn la_cap_0all, la_cap_0lo16, la_cap_0mid16, la_cap_0b0, la_cap_0b1;
extern cap_fn la_cap_1all, la_cap_1lo16, la_cap_1mid16, la_cap_1b0, la_cap_1b1;
extern cap_fn la_cap_2all;

static cap_fn *const fast[2][AI64_LA_LANES] = {
	{ la_cap_0all, la_cap_0lo16, la_cap_0mid16, la_cap_0b0, la_cap_0b1 },
	{ la_cap_1all, la_cap_1lo16, la_cap_1mid16, la_cap_1b0, la_cap_1b1 },
};

/* IEP0 counting ns from 0, unless it already does, with no compare. */
static void iep_start(void)
{
	if ((ICSSG_REG(IEP_GLOBAL_CFG(IEP)) & IEP_CNT_ENABLE) && ICSSG_REG(IEP_CMP_CFG(IEP)) == 0) {
		return;
	}
	ICSSG_REG(IEP_GLOBAL_CFG(IEP)) = IEP_DEFAULT_INC(ICSSG_NS_PER_CYCLE) |
					 IEP_CMP_INC(ICSSG_NS_PER_CYCLE);
	ICSSG_REG(IEP_CMP_CFG(IEP)) = 0;
	ICSSG_REG(IEP_CMP_STATUS(IEP)) = 0xffffffffu;
	ICSSG_REG(ICSSG_CFG_IEPCLK) |= ICSSG_IEPCLK_SYNC;
	ICSSG_REG(IEP_COUNT_LO(IEP)) = 0;
	ICSSG_REG(IEP_COUNT_HI(IEP)) = 0;
	ICSSG_REG(IEP_GLOBAL_CFG(IEP)) |= IEP_CNT_ENABLE;
}

/* Whether the setup is one la_cap.asm has a loop for. */
static int valid(volatile struct ai64_la_cfg *c)
{
	uint32_t p = c->period;

	if (c->magic != AI64_LA_CFG_MAGIC || !(c->mask & AI64_LA_MASK) || c->lane >= AI64_LA_LANES) {
		return 0;
	}
	if (p == 2u || p == 4u) {
		return 1;
	}
	return p >= 10u && !(p & 1u) && p <= 1000u && c->lane == AI64_LA_LANE_ALL;
}

void main(void)
{
	volatile struct ai64_la_cfg *c = CFG;
	uint32_t mask;

	tm_clear(ICSSG_TM_PRU0);
	trace_init();
	ICSSG_REG(CFG_GPCFG0) = 0;
	iep_start();
	c->run = 0;
	if (!valid(c)) {
		c->mask = AI64_LA_MASK;
		c->lane = AI64_LA_LANE_ALL;
		c->period = AI64_LA_DEFAULT_PERIOD;
		c->key = 0;
		c->magic = AI64_LA_CFG_MAGIC;
		trace_puts("ICSSG1 PRU0: no setup, every pin at 25 MS/s\n");
	}
	mask = c->mask & AI64_LA_MASK;
	trace_puts("ICSSG1 PRU0 up: sampling every ");
	trace_u32(c->period);
	trace_puts(" cycles, lane ");
	trace_u32(c->lane);
	trace_putc('\n');
	if (c->period <= 4u) {
		fast[c->period == 4u][c->lane](mask, c->key, (uint32_t)&c->t0, 0);
	}
	la_cap_2all(mask, c->key, (uint32_t)&c->t0, (c->period - 4u) / 2u);
}
