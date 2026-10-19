/* SPDX-License-Identifier: MIT */
/*
 * Tx_PRU0 firmware (ICSSG0 slice 0). This core sends pulse trains.
 *
 * It polls the arm block RTU0 publishes in ICSSG0 shared RAM. For each new
 * block it sends `periods` pulses on P9.25, one every `period_cycles` ICSSG
 * cycles, high for the first half of each period. A train always finishes.
 * A block that arrives during a train starts when the train ends; if more
 * than one arrives, only the newest runs (the arm block is one slot).
 *
 * Tx_PRU0's R30 does not reach the pads, so it drives P9.25 through the main
 * GPIO0 module: one store to SET_DATA, one to CLR_DATA. The overlay already
 * made GPIO0_104 an output. The half periods are timed with this core's
 * cycle counter, so the time a bus store takes does not add to the period.
 */

#include <stdint.h>
#include "ai64_shm.h"

#define AI64_TRACE_SIZE 256
#define AI64_TRACE_NAME "trace:txpru0"
#include "ai64_rsc_trace.h"

/* Tx_PRU0 control registers, local address (constant table entry 11). */
#define TXPRU_CTRL   (*(volatile uint32_t *)0x00025000u)
#define TXPRU_CYCLE  (*(volatile uint32_t *)0x0002500Cu)
#define CTRL_CTR_EN  (1u << 3)

#define GPIO_SET (*(volatile uint32_t *)AI64_PULSE_GPIO_SET)
#define GPIO_CLR (*(volatile uint32_t *)AI64_PULSE_GPIO_CLR)

/* The counter only clears while it is stopped. */
static void cycle_restart(void)
{
	TXPRU_CTRL &= ~CTRL_CTR_EN;
	TXPRU_CYCLE = 0;
	TXPRU_CTRL |= CTRL_CTR_EN;
}

static void cycle_wait(uint32_t until)
{
	while (TXPRU_CYCLE < until) {
	}
}

/* A new arm block is waiting. */
static int arm_pending(volatile struct ai64_arm *arm, uint32_t seen)
{
	return arm->magic == AI64_ARM_MAGIC && arm->seq != seen;
}

void main(void)
{
	volatile struct ai64_arm *arm = (volatile struct ai64_arm *)AI64_ARM_LOCAL;
	uint32_t seen;

	trace_init();
	GPIO_CLR = AI64_PULSE_GPIO_MASK;
	seen = arm->seq;
	trace_puts("txpru0 up, pulses on P9.25\n");

	while (1) {
		uint32_t seq, period, count, half, n;

		if (!arm_pending(arm, seen)) {
			continue;
		}
		seq = arm->seq;
		period = arm->period_cycles;
		count = arm->periods;
		if (arm->seq != seq || arm->magic != AI64_ARM_MAGIC) {
			continue;
		}
		seen = seq;
		if (period < AI64_ARM_PERIOD_MIN) {
			period = AI64_ARM_PERIOD_MIN;
		}
		if (period > AI64_ARM_PERIOD_MAX) {
			period = AI64_ARM_PERIOD_MAX;
		}
		half = period / 2u;
		arm->sent = 0;
		arm->applied_period = period;
		arm->applied_seq = seq;
		trace_puts("txpru0 arm seq=");
		trace_u32(seq);
		trace_puts(" period=");
		trace_u32(period);
		trace_puts(" count=");
		trace_u32(count);
		trace_putc('\n');

		for (n = 0; n < count; n++) {
			cycle_restart();
			GPIO_SET = AI64_PULSE_GPIO_MASK;
			cycle_wait(half);
			GPIO_CLR = AI64_PULSE_GPIO_MASK;
			cycle_wait(period);
			arm->sent = n + 1u;
		}
		arm->done_seq = seq;
		trace_puts("txpru0 sent ");
		trace_u32(n);
		trace_putc('\n');
	}
}
