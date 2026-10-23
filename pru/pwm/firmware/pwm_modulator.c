/* SPDX-License-Identifier: MIT */
/*
 * The Tx_PRU of each PWM slice: the modulator. It turns the RTU's request
 * (pwm_ctl) into the parameters of the coming periods (pwm_next), which
 * the slice's PRU reads once a period. With sweep_ms set it moves the duty
 * from its minimum to its maximum and back every sweep_ms, a step per
 * period, so its pace is the PWM's own; otherwise it passes the requested
 * duty through. Both slices build this file; the PRUs that read pwm_next
 * make their edges in different ways (pwm_ipc.h).
 *
 * Like the PRU, it only touches its slice's data RAM.
 */

#include <stdint.h>
#include "pwm_ipc.h"

#define AI64_TRACE_SIZE 192
#define AI64_TRACE_NAME "trace:pwm-txpru"
#include "ai64_rsc_trace.h"

static uint32_t next_seq;

static void write_next(uint32_t period, uint32_t duty, uint32_t dead)
{
	volatile struct pwm_next *n = PWM_NEXT;
	volatile struct pwm_mod *mod = PWM_MOD;

	next_seq += 2u;
	n->seq = next_seq - 1u;
	n->period_ns = period;
	n->duty_ns = duty;
	n->dead_ns = dead;
	n->seq = next_seq;
	mod->updates = mod->updates + 1u;
	mod->duty_ns = duty;
}

void main(void)
{
	volatile struct pwm_ctl *ctl = PWM_CTL;
	volatile struct pwm_out *out = PWM_OUT;
	volatile struct pwm_mod *mod = PWM_MOD;
	uint32_t ctl_seen = 0, periods_seen = 0;
	uint32_t period = 0, duty = 0, dead = 0, sweep = 0;
	/* The sweep: lo to hi in half periods, step ns a period plus rem/half. */
	uint32_t lo = 0, hi = 0, half = 1, step = 0, rem = 0, acc = 0, up = 1;

	trace_init();
	/* The PRU waits for a valid record; none until the RTU asks. */
	next_seq = 0;
	PWM_NEXT->seq = 0;
	mod->magic = 0;
	mod->ctl_seq = 0;
	mod->updates = 0;
	mod->duty_ns = 0;
	mod->magic = PWM_MOD_MAGIC;
	trace_puts("ICSSG0 Tx_PRU");
	trace_u32(AI64_SLICE);
	trace_puts(" up: the modulator, waiting for the RTU\n");

	for (;;) {
		uint32_t s = ctl->seq;

		if (ctl->magic == PWM_CTL_MAGIC && s != ctl_seen) {
			period = ctl->period_ns;
			duty = ctl->duty_ns;
			dead = ctl->dead_ns;
			sweep = ctl->sweep_ms;
			if (ctl->seq != s) {
				continue;
			}
			ctl_seen = s;
			if (sweep) {
				lo = AI64_PWM_EDGE_MIN_NS;
				hi = pwm_duty_max(period, dead);
				half = (uint32_t)((uint64_t)sweep * 500000u / period);
				if (half == 0) {
					half = 1;
				}
				step = (hi - lo) / half;
				rem = (hi - lo) % half;
				acc = 0;
				up = 1;
				duty = lo;
			}
			write_next(period, duty, dead);
			mod->ctl_seq = s;
			periods_seen = out->periods;
			trace_puts("control seq ");
			trace_u32(s);
			trace_puts(sweep ? ": sweeping\n" : ": fixed duty\n");
		}
		if (sweep && out->periods != periods_seen) {
			uint32_t d = step;

			periods_seen = out->periods;
			acc += rem;
			if (acc >= half) {
				acc -= half;
				d++;
			}
			if (up && duty + d >= hi) {
				duty = hi;
				up = 0;
			} else if (up) {
				duty += d;
			} else if (duty <= lo + d) {
				duty = lo;
				up = 1;
			} else {
				duty -= d;
			}
			write_next(period, duty, dead);
		}
	}
}
