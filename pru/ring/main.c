/* SPDX-License-Identifier: MIT */
/*
 * Ring firmware for the ICSSG slices the demo does not otherwise use:
 * ICSSG0 slice 1, ICSSG1 slice 0, and ICSSG1 slice 1. The Makefile builds
 * it once per core, with AI64_ICSSG and AI64_SLICE set, and one of
 * AI64_ROLE_PRU, AI64_ROLE_RTU, or AI64_ROLE_TXPRU.
 *
 * The slice's three cores pass a token round through the ring record in
 * their ICSSG's shared RAM (struct ai64_ring in ai64_shm.h): PRU, RTU,
 * Tx_PRU, and back to the PRU. Each one changes the value on the way, and
 * the PRU checks what comes back. The PRU times each round trip with its
 * cycle counter and logs the lap rate to its trace every 10 s or so. The PRU
 * also toggles one header pin every AI64_RING_WINDOW laps, so the pin's
 * frequency times 2 * 65536 is the lap rate.
 *
 * Between laps, every FAB_LAPS laps, the PRU is also a fabric node
 * (ai64_fabric.h): it answers and sends pings, passes tours and routes on,
 * and adds to the spinlock tally. That is the only time a ring core reaches
 * outside its ICSSG.
 *
 * Start the PRU last. It starts a run by taking the token, which is only
 * safe while the other two are waiting for it.
 */

#include <stdint.h>
#include "ai64_shm.h"

#if defined(AI64_ROLE_PRU)
#define AI64_TRACE_SIZE 512
#define ROLE_NAME       "pru"
#define ROLE_STAGE      AI64_RING_PRU
#elif defined(AI64_ROLE_RTU)
#define AI64_TRACE_SIZE 256
#define ROLE_NAME       "rtu"
#define ROLE_STAGE      AI64_RING_RTU
#elif defined(AI64_ROLE_TXPRU)
#define AI64_TRACE_SIZE 256
#define ROLE_NAME       "txpru"
#define ROLE_STAGE      AI64_RING_TXPRU
#else
#error "define AI64_ROLE_PRU, AI64_ROLE_RTU, or AI64_ROLE_TXPRU"
#endif
#define AI64_TRACE_NAME "trace:ring"
#include "ai64_rsc_trace.h"

#define NEXT_STAGE ((ROLE_STAGE + 1u) % 3u)

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

/* Its fabric node: 9 for ICSSG0 slice 1, 10 and 11 for ICSSG1. */
#define FAB_NODE (AI64_FAB_G0PRU1 + 2u * AI64_ICSSG + AI64_SLICE - 1u)
/* A pass takes a microsecond or two; 64 laps take about 30. */
#define FAB_LAPS 64u
#define AI64_FAB_NOW()    (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#include "ai64_fabric.h"

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

void main(void)
{
	volatile struct ai64_ring *ring = (volatile struct ai64_ring *)AI64_RING_LOCAL(AI64_SLICE);
	uint32_t lap = 0, lap_min = 0xffffffffu, lap_max = 0, window = 0;
	uint32_t elapsed = 0, logged = 0, start;

	trace_init();
	__R30 &= ~RING_PIN;
	/*
	 * Take the token, then start the run. The other two only write while
	 * the token is theirs, so nothing else writes the record from here.
	 */
	ring->stage = AI64_RING_PRU;
	ring->magic = 0;
	ring->run = ring->run + 1u;
	ring->laps = 0;
	ring->rtu_hops = 0;
	ring->txpru_hops = 0;
	ring->errors = 0;
	ring->lap_last = 0;
	ring->lap_min = 0;
	ring->lap_max = 0;
	ring->magic = AI64_RING_MAGIC;
	trace_name();
	trace_puts(" up, run ");
	trace_u32(ring->run);
	trace_puts(", passing the token\n");

	fab_init(FAB_NODE);
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
		ring->stage = AI64_RING_RTU;
		while (ring->stage != AI64_RING_PRU) {
		}
		cycles = PRU_CYCLE - start;
		want = ai64_ring_txpru(ai64_ring_rtu(lap));
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
		if (++window == AI64_RING_WINDOW) {
			ring->lap_min = lap_min;
			ring->lap_max = lap_max;
			lap_min = 0xffffffffu;
			lap_max = 0;
			window = 0;
			__R30 ^= RING_PIN;
		}
		if ((lap & (FAB_LAPS - 1u)) == 0) {
			fab_poll();
		}
		/* 10 s, short by a restart's few cycles every 8 s or so. */
		if (elapsed >= 10u * AI64_PRU_HZ) {
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
		while (ring->magic != AI64_RING_MAGIC || ring->stage != ROLE_STAGE) {
		}
		/* The counts live in the record: the PRU zeroes them each run. */
#ifdef AI64_ROLE_RTU
		ring->value = ai64_ring_rtu(ring->value);
		ring->rtu_hops = ring->rtu_hops + 1u;
#else
		ring->value = ai64_ring_txpru(ring->value);
		ring->txpru_hops = ring->txpru_hops + 1u;
#endif
		if (ring->run != run) {
			run = ring->run;
			trace_name();
			trace_puts(" has the token, run ");
			trace_u32(run);
			trace_putc('\n');
		}
		ring->stage = NEXT_STAGE;
	}
}
#endif
