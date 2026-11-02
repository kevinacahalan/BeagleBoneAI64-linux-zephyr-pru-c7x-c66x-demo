/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 RTU0 and RTU1: each slice's mover. The RTU copies its PRU's edge
 * records from the ring in shared RAM (la_map.h) to the PRU's ring in DDR,
 * adds a tick every millisecond, and keeps the stream's header up to date
 * (ai64_la.h). The PRU never waits for it: if the RTU falls a ring behind,
 * it skips to where the PRU is and writes a gap record.
 *
 * RTU0 also starts IEP0, the analyzer's clock, before either PRU runs:
 * nanoseconds on the 250 MHz core clock, with no compare resetting it.
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_shm.h"
#include "ai64_la.h"
#include "la_map.h"
#include "la_tm.h"

#define AI64_TRACE_SIZE 512
#if AI64_SLICE == 0
#define AI64_TRACE_NAME "trace:la-rtu0"
#else
#define AI64_TRACE_NAME "trace:la-rtu1"
#endif
#include "ai64_rsc_trace.h"

#define IEP       ICSSG_IEP0
#define SRING     ((volatile uint32_t *)LA_SRING(AI64_SLICE))
#define SWR       (*(volatile uint32_t *)LA_SCTL(AI64_SLICE))
#define RING      ((volatile uint32_t *)AI64_LA_RING_ADDR(AI64_SLICE))
#define HDR       (&((volatile struct ai64_la *)AI64_LA_ADDR)->s[AI64_SLICE])
#define GTC_LO()  (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))
#define GTC_HI()  (*(volatile uint32_t *)(AI64_GTC_ADDR + 12u))
#define IEP_LO()  ICSSG_REG(IEP_COUNT_LO(IEP))
#define IEP_HI()  ICSSG_REG(IEP_COUNT_HI(IEP))

/* Bytes, free-running: rd in the shared ring, wr in the DDR ring. */
static uint32_t rd, wr;

static void iep_start(void)
{
	ICSSG_REG(IEP_GLOBAL_CFG(IEP)) = IEP_DEFAULT_INC(ICSSG_NS_PER_CYCLE) |
					 IEP_CMP_INC(ICSSG_NS_PER_CYCLE);
	ICSSG_REG(IEP_CMP_CFG(IEP)) = 0;
	ICSSG_REG(IEP_CMP_STATUS(IEP)) = 0xffffffffu;
	ICSSG_REG(ICSSG_CFG_IEPCLK) |= ICSSG_IEPCLK_SYNC;
	ICSSG_REG(IEP_COUNT_LO(IEP)) = 0;
	ICSSG_REG(IEP_COUNT_HI(IEP)) = 0;
	ICSSG_REG(IEP_GLOBAL_CFG(IEP)) |= IEP_CNT_ENABLE;
}

static inline void put(uint32_t time, uint32_t state)
{
	volatile uint32_t *r = RING + (wr & (AI64_LA_RING_SIZE - 1u)) / 4u;

	r[0] = time;
	r[1] = state;
	wr += AI64_LA_REC_SIZE;
}

/* A tick: IEP0's and the GTC's counts together, and the state now. */
static void tick(uint32_t state)
{
	uint32_t ihi, ilo, ghi, glo;

	do {
		ihi = IEP_HI();
		ilo = IEP_LO();
	} while (IEP_HI() != ihi);
	do {
		ghi = GTC_HI();
		glo = GTC_LO();
	} while (GTC_HI() != ghi);
	put(ilo, (uint32_t)AI64_LA_TICK << 24 | state);
	put(glo, (ghi & 0xffffu) << 16 | (ihi & 0xffffu));
	HDR->ticks++;
}

void main(void)
{
	volatile struct ai64_la_stream *h = HDR;
	uint32_t state = 0, last_tick;

	tm_clear(AI64_SLICE ? ICSSG_TM_RTU1 : ICSSG_TM_RTU0);
	trace_init();
	if (AI64_SLICE == 0) {
		iep_start();
	}
	SWR = 0;
	rd = 0;
	wr = 0;
	h->magic = 0;
	h->wr = 0;
	h->edges = 0;
	h->gaps = 0;
	h->lost = 0;
	h->state = 0;
	h->ticks = 0;
	h->mask = AI64_LA_MASK(AI64_SLICE);
	h->ring = AI64_LA_RING_ADDR(AI64_SLICE);
	h->ring_size = AI64_LA_RING_SIZE;
	h->magic = AI64_LA_MAGIC;
	trace_puts(AI64_SLICE ? "ICSSG1 RTU1 up: PRU1's edges to DDR\n" :
				"ICSSG1 RTU0 up: IEP0 counting ns; PRU0's edges to DDR\n");
	last_tick = GTC_LO();

	for (;;) {
		uint32_t n = SWR - rd;

		if (n > LA_SRING_SIZE - 64u) {
			/* The PRU is about to write over what is left: skip to it. */
			uint32_t skip = n / AI64_LA_REC_SIZE;

			rd += n;
			put(IEP_LO(), (uint32_t)AI64_LA_GAP << 24 | (skip & 0xffffffu));
			h->gaps++;
			h->lost += skip;
			h->wr = wr;
		} else if (n) {
			volatile uint32_t *s;
			uint32_t i;

			/* At most 32 records a pass, so the ticks keep up. */
			if (n > 32u * AI64_LA_REC_SIZE) {
				n = 32u * AI64_LA_REC_SIZE;
			}
			for (i = 0; i < n; i += AI64_LA_REC_SIZE) {
				s = SRING + ((rd + i) & (LA_SRING_SIZE - 1u)) / 4u;
				state = s[1];
				put(s[0], state);
			}
			rd += n;
			h->edges += n / AI64_LA_REC_SIZE;
			h->state = state;
			h->wr = wr;
		}
		if (GTC_LO() - last_tick >= AI64_LA_TICK_GTC) {
			last_tick += AI64_LA_TICK_GTC;
			if (GTC_LO() - last_tick >= AI64_LA_TICK_GTC) {
				last_tick = GTC_LO();
			}
			tick(state);
			h->wr = wr;
		}
	}
}
