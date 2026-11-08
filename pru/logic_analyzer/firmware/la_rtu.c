/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1 RTU0 and RTU1: each slice's mover. The RTU copies its PRU's edge
 * records from the ring in shared RAM (la_map.h) to the PRU's ring in DDR,
 * in bursts (la_copy.asm), adds a tick every millisecond, and keeps the
 * stream's header up to date (ai64_la.h). The PRU never waits for it: if
 * the RTU falls a ring behind, it skips to where the PRU is and writes a
 * gap record.
 *
 * RTU0 also starts IEP0, the analyzer's clock, before either PRU runs:
 * nanoseconds on the 250 MHz core clock, with no compare resetting it.
 *
 * Each RTU is a fabric node too (ai64_fabric.h), RTU0 node 10 and RTU1
 * node 11: it answers pings, adds to the tally, publishes its stream's
 * numbers in its page, and puts a mark record in its stream for each route
 * that brings it the mark action.
 *
 * And it is the Tx_PRU's link to Linux: it passes on the trigger Linux sets
 * in the stream's header, and puts each hit the Tx_PRU finds in the stream
 * as a trig record (la_ipc.h).
 */

#include <stdint.h>
#include "icssg.h"
#include "ai64_shm.h"
#include "ai64_la.h"
#include "la_map.h"
#include "la_tm.h"
#include "la_ipc.h"

#define AI64_TRACE_SIZE 512
#if AI64_SLICE == 0
#define AI64_TRACE_NAME "trace:la-rtu0"
#else
#define AI64_TRACE_NAME "trace:la-rtu1"
#endif
#include "ai64_rsc_trace.h"

#define IEP       ICSSG_IEP0
#define SREC(rd)  LA_SREC(AI64_SLICE, rd)
#define RING      ((volatile uint32_t *)AI64_LA_RING_ADDR(AI64_SLICE))
#define HDR       (&((volatile struct ai64_la *)AI64_LA_ADDR)->s[AI64_SLICE])
#define GTC_LO()  (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))
#define GTC_HI()  (*(volatile uint32_t *)(AI64_GTC_ADDR + 12u))
#define IEP_LO()  ICSSG_REG(IEP_COUNT_LO(IEP))
#define IEP_HI()  ICSSG_REG(IEP_COUNT_HI(IEP))

extern void la_pack(uint32_t dst, uint32_t src, uint32_t n);

/* Bytes, free-running: rd in the shared ring, wr in the DDR ring. */
static uint32_t rd, wr;
/* The trigger seq last passed on, the Tx_PRU's hits taken, the mode's seq. */
static uint32_t trig_seq, hits_rd, cfg_seq;

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

struct ai64_fab_msg;
static uint32_t fab_action(struct ai64_fab_msg *m);

#define FAB_NODE          (AI64_FAB_G1RTU0 + AI64_SLICE)
#define AI64_FAB_NOW()    GTC_LO()
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_ACTION(m) fab_action(m)
#include "ai64_fabric.h"

#define STATUS_TICKS AI64_FAB_US(10000)
#define FAB_EVERY    AI64_FAB_US(100)

/* A route's mark: when this RTU read it, who sent it, and its value. */
static uint32_t fab_action(struct ai64_fab_msg *m)
{
	if (m->action != AI64_FAB_ACT_MARK) {
		return 1;
	}
	put(IEP_LO(), (uint32_t)AI64_LA_MARK << 24 | (m->origin & 0xffu) << 16 | (m->arg0 & 0xffffu));
	HDR->marks++;
	HDR->wr = wr;
	return 0;
}

/* The stream's numbers, in this node's fabric page. */
static void publish(uint32_t state)
{
	volatile struct ai64_la_stream *h = HDR;
	volatile struct ai64_la_status *st = (volatile struct ai64_la_status *)FAB_PAGE(FAB_NODE)->app;

	st->edges = h->edges;
	st->gaps = h->gaps;
	st->lost = h->lost;
	st->marks = h->marks;
	st->state = state;
	st->mask = h->mask;
	st->ticks = h->ticks;
	st->trigs = h->trigs;
	st->magic = AI64_LA_MAGIC;
}

/* A new trigger from Linux, to the Tx_PRU. */
static void pass_trigger(void)
{
	volatile struct ai64_la_stream *h = HDR;
	volatile struct la_trig *cfg = LA_TRIG_REC;
	uint32_t seq = h->trig_seq;

	if (seq == trig_seq) {
		return;
	}
	trig_seq = seq;
	cfg->mode = h->trig_mode;
	cfg->mask = h->trig_mask;
	cfg->value = h->trig_value;
	cfg->seq = seq;
}

/*
 * A new mode from Linux, for the PRU and Tx_PRU when Linux next starts
 * them (la_map.h); cfg_ack says it is passed on.
 */
static void pass_mode(void)
{
	volatile struct ai64_la_stream *h = HDR;
	uint32_t seq = h->cfg_seq;

	if (seq == cfg_seq) {
		return;
	}
	cfg_seq = seq;
	LA_CFG_REC->mode = h->cfg_mode;
	LA_CFG_REC->mask = h->cfg_mask;
	h->cfg_ack = seq;
}

/* The Tx_PRU's hits since the last call, into the stream. */
static void take_hits(void)
{
	volatile struct la_hits *hits = LA_HITS_REC;
	uint32_t hw = hits->wr;

	if (hw == hits_rd) {
		return;
	}
	if (hw - hits_rd > LA_HIT_RING) {
		hits_rd = hw - LA_HIT_RING;
	}
	while (hits_rd != hw) {
		volatile uint32_t *hit = hits->hit[hits_rd % LA_HIT_RING];

		put(hit[0], (uint32_t)AI64_LA_TRIG << 24 | (hit[1] & 0xffffffu));
		HDR->trigs++;
		hits_rd++;
	}
	HDR->wr = wr;
}

/*
 * How many records from rd on the PRU has written, up to max (at most
 * 512): it writes them in order, so a binary search on their seqs.
 */
static uint32_t ready(uint32_t max)
{
	uint32_t n = 1, step;

	for (step = 256; step; step >>= 1) {
		uint32_t at = rd + (n + step - 1u) * LA_SREC_SIZE;

		if (n + step <= max && SREC(at)->seq == at) {
			n += step;
		}
	}
	return n;
}

/* The ring before the PRU starts: each slot as if written the lap before the first. */
static void ring_clear(void)
{
	uint32_t at;

	for (at = 0; at < LA_SRING_SIZE; at += LA_SREC_SIZE) {
		SREC(at)->seq = at - LA_SRING_SIZE;
	}
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
	uint32_t state = 0, last_tick, status_at, fab_at;

	/* The PRU waits for this word, the last thing set here. */
	LA_READY_WORD = 0;
	tm_clear(AI64_SLICE ? ICSSG_TM_RTU1 : ICSSG_TM_RTU0);
	trace_init();
	if (AI64_SLICE == 0) {
		iep_start();
	}
	rd = 0;
	ring_clear();
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
	h->marks = 0;
	h->trigs = 0;
	/* No trigger at start: the header's may be an old run's, or garbage. */
	h->trig_mode = AI64_LA_TRIG_OFF;
	h->trig_mask = 0;
	h->trig_value = 0;
	trig_seq = h->trig_seq - 1u;
	hits_rd = 0;
	LA_HITS_REC->wr = 0;
	pass_trigger();
	/* Edge mode and every pin, whatever the header held. */
	h->cfg_mode = AI64_LA_MODE_EDGE;
	h->cfg_mask = AI64_LA_MASK(AI64_SLICE);
	cfg_seq = h->cfg_seq;
	h->cfg_ack = cfg_seq;
	LA_CFG_REC->mode = AI64_LA_MODE_EDGE;
	LA_CFG_REC->mask = AI64_LA_MASK(AI64_SLICE);
	LA_CFG_REC->resume = 0;
	h->magic = AI64_LA_MAGIC;
	LA_READY_WORD = LA_READY_MAGIC;
	trace_puts(AI64_SLICE ? "ICSSG1 RTU1 up: PRU1's edges to DDR\n" :
				"ICSSG1 RTU0 up: IEP0 counting ns; PRU0's edges to DDR\n");
	fab_init(FAB_NODE);
	trace_puts(AI64_SLICE ? "ICSSG1 RTU1 is fabric node " : "ICSSG1 RTU0 is fabric node ");
	trace_u32(FAB_NODE);
	trace_putc('\n');
	last_tick = GTC_LO();
	status_at = last_tick;
	fab_at = last_tick;

	for (;;) {
		uint32_t seq = SREC(rd)->seq;

		if ((int32_t)(seq - rd) > 0) {
			/* The PRU has gone a lap past: skip to the record it left here. */
			uint32_t skip = (seq - rd) / LA_SREC_SIZE;

			rd = seq;
			put(IEP_LO(), (uint32_t)AI64_LA_GAP << 24 | (skip & 0xffffffu));
			h->gaps++;
			h->lost += skip;
			h->wr = wr;
		} else if (seq == rd) {
			/*
			 * A burst, up to where either ring wraps; at most 512
			 * records a pass, so the ticks keep up.
			 */
			uint32_t so = rd & (LA_SRING_SIZE - 1u), dof = wr & (AI64_LA_RING_SIZE - 1u);
			uint32_t max = 512u, n;

			if (max > (LA_SRING_SIZE - so) / LA_SREC_SIZE) {
				max = (LA_SRING_SIZE - so) / LA_SREC_SIZE;
			}
			if (max > (AI64_LA_RING_SIZE - dof) / AI64_LA_REC_SIZE) {
				max = (AI64_LA_RING_SIZE - dof) / AI64_LA_REC_SIZE;
			}
			n = ready(max);
			la_pack(AI64_LA_RING_ADDR(AI64_SLICE) + dof, LA_SRING(AI64_SLICE) + so, n);
			rd += n * LA_SREC_SIZE;
			wr += n * AI64_LA_REC_SIZE;
			/* The newest pins: a raw-mode hole record holds none. */
			if (!AI64_LA_TYPE(SREC(rd - LA_SREC_SIZE)->state)) {
				state = SREC(rd - LA_SREC_SIZE)->state & ~AI64_LA_LATE;
			}
			LA_CFG_REC->resume = rd;
			h->edges += n;
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
			pass_trigger();
			pass_mode();
		}
		take_hits();
		/*
		 * The fabric costs a few us of uncached reads a pass; the ring
		 * holds 80 us of edges at 25 million a second. So the records
		 * first: the fabric when the ring is nearly drained, or every
		 * FAB_EVERY at the latest.
		 */
		if (SREC(rd + 64u * LA_SREC_SIZE)->seq != rd + 64u * LA_SREC_SIZE ||
		    FAB_AFTER(GTC_LO(), fab_at)) {
			fab_poll();
			fab_at = GTC_LO() + FAB_EVERY;
		}
		if (FAB_AFTER(GTC_LO(), status_at)) {
			status_at = GTC_LO() + STATUS_TICKS;
			publish(state);
		}
	}
}
