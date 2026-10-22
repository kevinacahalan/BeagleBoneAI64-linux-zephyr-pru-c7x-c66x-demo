/* SPDX-License-Identifier: MIT */
/*
 * Ring firmware for the PRU and Tx_PRU of the ICSSG slices the demo does
 * not otherwise use: ICSSG0 slice 1, ICSSG1 slice 0, and ICSSG1 slice 1.
 * The Makefile builds it once per core, with AI64_ICSSG and AI64_SLICE set,
 * and AI64_ROLE_PRU or AI64_ROLE_TXPRU. The slice's RTU runs ring/rtu.c.
 *
 * The two cores pass a token back and forth through the ring record in
 * their ICSSG's shared RAM (struct ai64_ring in ai64_shm.h). The Tx_PRU
 * changes the value on the way, and the PRU checks what comes back. The PRU
 * times each round trip with its cycle counter, and toggles one header pin
 * every window: AI64_RING_WINDOW laps, unless the RTU asked for another
 * number. So the pin's frequency times 2 * window is the lap rate.
 *
 * These are the slice's real-time cores. They only read and write their
 * ICSSG's own RAM, never DDR, a mailbox, or a spinlock, so nothing outside
 * the ICSSG can stretch a lap. The RTU does all the talking: it reads the
 * ring record, and writes the control record the PRU reads once a window.
 * The trace is the one exception, and it is in the core's own data RAM;
 * make TRACE=0 builds it out.
 *
 * Start the PRU last. It starts a run by taking the token, which is only
 * safe while the Tx_PRU is waiting for it.
 */

#include <stdint.h>
#include "ai64_shm.h"

#if defined(AI64_ROLE_PRU)
#define AI64_TRACE_SIZE 384
#define ROLE_NAME       "pru"
#elif defined(AI64_ROLE_TXPRU)
#define AI64_TRACE_SIZE 256
#define ROLE_NAME       "txpru"
#else
#error "define AI64_ROLE_PRU or AI64_ROLE_TXPRU"
#endif
#define AI64_TRACE_NAME "trace:ring"
#include "ai64_rsc_trace.h"

static void trace_name(void)
{
	trace_puts("icssg");
	trace_u32(AI64_ICSSG);
	trace_putc(' ');
	trace_puts(ROLE_NAME);
	trace_u32(AI64_SLICE);
}

#ifdef AI64_ROLE_PRU
/*
 * The R30 bit whose pad the overlay muxes to this PRU (mode 0, PRGn_PRUm_GPOk):
 * ICSSG0 PRU1 GPO3 on P8.44, ICSSG1 PRU0 GPO3 on P8.18, ICSSG1 PRU1 GPO9 on
 * P8.21.
 */
#if AI64_ICSSG == 1 && AI64_SLICE == 1
#define RING_PIN (1u << 9)
#else
#define RING_PIN (1u << 3)
#endif
volatile register uint32_t __R30;

/* PRU0 and PRU1 control registers, local address: CTRL, then CYCLE at +0xc. */
#define PRU_CTRL_BASE (AI64_SLICE ? 0x00024000u : 0x00022000u)
#define PRU_CTRL      (*(volatile uint32_t *)PRU_CTRL_BASE)
#define PRU_CYCLE     (*(volatile uint32_t *)(PRU_CTRL_BASE + 0xcu))
#define CTRL_CTR_EN   (1u << 3)

/*
 * The counter only clears while it is stopped, and stops for good at
 * 0xffffffff. Restarting it costs tens of cycles, so the PRU lets it run and
 * takes differences, and restarts it only when it is halfway up.
 */
static void cycle_restart(void)
{
	PRU_CTRL &= ~CTRL_CTR_EN;
	PRU_CYCLE = 0;
	PRU_CTRL |= CTRL_CTR_EN;
}

/* The window the control record asks for, if it is a valid one. */
static uint32_t ctl_window(volatile struct ai64_ring_ctl *ctl, uint32_t window)
{
	uint32_t w = ctl->window;

	if (ctl->magic != AI64_RING_CTL_MAGIC || w < AI64_RING_WINDOW_MIN ||
	    w > AI64_RING_WINDOW_MAX) {
		return window;
	}
	return w;
}

void main(void)
{
	volatile struct ai64_ring *ring = (volatile struct ai64_ring *)AI64_RING_LOCAL(AI64_SLICE);
	volatile struct ai64_ring_ctl *ctl = (volatile struct ai64_ring_ctl *)AI64_RING_CTL_LOCAL(AI64_SLICE);
	uint32_t lap = 0, lap_min = 0xffffffffu, lap_max = 0, laps_in = 0;
	uint32_t window, ctl_seen;
	uint32_t elapsed = 0, logged = 0, start;

	trace_init();
	__R30 &= ~RING_PIN;
	/* Whatever the RTU wrote before this run started. */
	ctl_seen = ctl->seq;
	window = ctl_window(ctl, AI64_RING_WINDOW);
	/*
	 * Take the token, then start the run. The Tx_PRU only writes while
	 * the token is its, so nothing else writes the record from here.
	 */
	ring->stage = AI64_RING_PRU;
	ring->magic = 0;
	ring->run = ring->run + 1u;
	ring->laps = 0;
	ring->txpru_hops = 0;
	ring->errors = 0;
	ring->lap_last = 0;
	ring->lap_min = 0;
	ring->lap_max = 0;
	ring->window = window;
	ring->ctl_ack = ctl_seen;
	ring->magic = AI64_RING_MAGIC;
	trace_name();
	trace_puts(" up, run ");
	trace_u32(ring->run);
	trace_puts(", passing the token\n");

	cycle_restart();
	start = PRU_CYCLE;
	for (;;) {
		uint32_t want, cycles, now;

		/* From the last lap's start: its round trip and its bookkeeping. */
		now = PRU_CYCLE;
		elapsed += now - start;
		if (now >= 0x80000000u) {
			cycle_restart();
			now = PRU_CYCLE;
		}
		start = now;
		lap++;
		ring->value = lap;
		ring->stage = AI64_RING_TXPRU;
		while (ring->stage != AI64_RING_PRU) {
		}
		cycles = PRU_CYCLE - start;
		want = ai64_ring_txpru(lap);
		if (ring->value != want) {
			ring->errors = ring->errors + 1u;
		}
		ring->laps = lap;
		ring->lap_last = cycles;
		if (cycles < lap_min) {
			lap_min = cycles;
		}
		if (cycles > lap_max) {
			lap_max = cycles;
		}
		if (++laps_in >= window) {
			uint32_t seq;

			__R30 ^= RING_PIN;
			ring->lap_min = lap_min;
			ring->lap_max = lap_max;
			lap_min = 0xffffffffu;
			lap_max = 0;
			laps_in = 0;
			/* The RTU's control record, once a window. */
			seq = ctl->seq;
			if (seq != ctl_seen) {
				ctl_seen = seq;
				window = ctl_window(ctl, window);
				ring->window = window;
				ring->ctl_ack = seq;
			}
		}
		/* 10 s, short by a restart's few cycles every 8 s or so. */
		if (AI64_TRACE_ON && elapsed >= 10u * AI64_PRU_HZ) {
			elapsed -= 10u * AI64_PRU_HZ;
			trace_name();
			trace_puts(": ");
			trace_u32(lap - logged);
			trace_puts(" laps in 10 s, last ");
			trace_u32(cycles);
			trace_puts(" cycles, errors ");
			trace_u32(ring->errors);
			trace_putc('\n');
			logged = lap;
		}
	}
}
#else
void main(void)
{
	volatile struct ai64_ring *ring = (volatile struct ai64_ring *)AI64_RING_LOCAL(AI64_SLICE);
	uint32_t run = 0;

	trace_init();
	trace_name();
	trace_puts(" up, waiting for the token\n");

	for (;;) {
		while (ring->magic != AI64_RING_MAGIC || ring->stage != AI64_RING_TXPRU) {
		}
		/* The counts live in the record: the PRU zeroes them each run. */
		ring->value = ai64_ring_txpru(ring->value);
		ring->txpru_hops = ring->txpru_hops + 1u;
		if (ring->run != run) {
			run = ring->run;
			trace_name();
			trace_puts(" has the token, run ");
			trace_u32(run);
			trace_putc('\n');
		}
		ring->stage = AI64_RING_PRU;
	}
}
#endif
