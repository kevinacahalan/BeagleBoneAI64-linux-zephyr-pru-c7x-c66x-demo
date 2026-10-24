/* SPDX-License-Identifier: MIT */
/*
 * The Tx_PRU of each PWM slice: the modulator. It turns the RTU's request
 * (pwm_ctl) into the parameters of the coming periods (pwm_next), which
 * the slice's PRU reads once a period. Three ways, by what was asked:
 *   - a fixed duty: passed through
 *   - steps_ms: each frequency of AI64_PWM_STEP_HZ in turn, and at each
 *     one each duty of AI64_PWM_STEP_PCT, steps_ms a step, on the GTC's
 *     time (ai64_shm.h), so both slices and P9.25 change step together
 *   - sweep_ms: from the minimum duty to the maximum and back every
 *     sweep_ms, a step per period
 * Once a period it writes the next period's parameters. Both slices build
 * this file; the PRUs that read pwm_next make their edges in different
 * ways (pwm_ipc.h).
 *
 * Slice 0's also runs ICSSG0's eCAP0 in APWM mode, on P8.15: a copy of A,
 * from the eCAP's own counter, which the Tx_PRU only gives a new period
 * and duty. Each takes effect at the eCAP period end nearest the time A
 * changes, or, in steps, the step's GTC time.
 *
 * Besides its slice's data RAM and the eCAP it only reads the GTC.
 */

#include <stdint.h>
#include "icssg.h"
#include "pwm_ipc.h"

#define AI64_TRACE_SIZE 192
#define AI64_TRACE_NAME "trace:pwm-txpru"
#include "ai64_rsc_trace.h"

static const uint32_t step_hz[AI64_PWM_STEP_FREQS] = AI64_PWM_STEP_HZ;
static const uint32_t step_pct[AI64_PWM_STEPS] = AI64_PWM_STEP_PCT;
static uint32_t next_seq;
static uint32_t step_period[AI64_PWM_STEP_FREQS];
static uint32_t step_duty[AI64_PWM_STEP_FREQS][AI64_PWM_STEPS];

static void write_next(uint32_t period, uint32_t duty, uint32_t dead)
{
	volatile struct pwm_next *n = PWM_NEXT;
	volatile struct pwm_mod *mod = PWM_MOD;

	duty = PWM_TICKS(duty);
	next_seq += 2u;
	n->seq = next_seq - 1u;
	n->period_ns = period;
	n->duty_ns = duty;
	n->dead_ns = dead;
	n->seq = next_seq;
	mod->updates = mod->updates + 1u;
	mod->duty_ns = duty;
}

#define GTC_LO (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))
#define GTC_HI (*(volatile uint32_t *)(AI64_GTC_ADDR + 12u))
/* GTC ticks in ns nanoseconds */
#define GTC_TICKS(ns) ((ns) / (1000000000u / AI64_GTC_HZ))

#if AI64_SLICE == 0
#define ECAP ICSSG_ECAP0
/* Asked for, to start at GTC want_at; in the shadows, loading; running */
static uint32_t want_period, want_duty, want_at, wanted;
static uint32_t load_period, load_duty, loading;
static uint32_t run_period, ecap_periods;

/*
 * Stopped, in capture mode, so P8.15 is not driven until the first
 * request, and counting from 0: the count and flags survive a reboot. As
 * with next_seq, nothing zeroes this core's statics at start.
 */
static void ecap_init(void)
{
	wanted = 0;
	loading = 0;
	run_period = 0;
	ecap_periods = 0;
	ICSSG_REG16(ECAP_ECCTL2(ECAP)) = ECAP_SYNCO_OFF;
	ICSSG_REG(ECAP_TSCTR(ECAP)) = 0;
	ICSSG_REG16(ECAP_ECCLR(ECAP)) = 0xffffu;
}

static void ecap_want(uint32_t period, uint32_t duty, uint32_t at)
{
	want_period = period;
	want_duty = PWM_TICKS(duty);
	want_at = at;
	wanted = 1;
}

/*
 * Every pass of the loop: count the eCAP's periods, and give it what was
 * asked for during the period whose end is nearest want_at. A period ends
 * when the count reaches CAP1, where the shadows load.
 */
static void ecap_poll(void)
{
	volatile struct pwm_mod *mod = PWM_MOD;
	uint32_t end;

	if (ICSSG_REG16(ECAP_ECFLG(ECAP)) & ECAP_CTR_PRD) {
		ICSSG_REG16(ECAP_ECCLR(ECAP)) = ECAP_CTR_PRD;
		mod->ecap_periods = ++ecap_periods;
		if (loading) {
			loading = 0;
			run_period = load_period;
			mod->ecap_period_ns = load_period;
			mod->ecap_duty_ns = load_duty;
		}
	}
	if (!wanted || loading) {
		return;
	}
	wanted = 0;
	if (run_period == 0u) {
		/*
		 * The first: APWM mode with the count stopped, then straight into
		 * the active registers, then count. In capture mode CAP1 and CAP2
		 * are capture registers, and a write to them does not stay.
		 */
		ICSSG_REG16(ECAP_ECCTL2(ECAP)) = ECAP_APWM | ECAP_SYNCO_OFF;
		ICSSG_REG(ECAP_CAP(ECAP, 1)) = want_period / ICSSG_NS_PER_CYCLE - 1u;
		ICSSG_REG(ECAP_CAP(ECAP, 2)) = want_duty / ICSSG_NS_PER_CYCLE;
		ICSSG_REG16(ECAP_ECCTL2(ECAP)) = ECAP_APWM | ECAP_SYNCO_OFF | ECAP_TSCTRSTOP;
		run_period = want_period;
		mod->ecap_period_ns = want_period;
		mod->ecap_duty_ns = want_duty;
		return;
	}
	end = GTC_LO + GTC_TICKS((ICSSG_REG(ECAP_CAP(ECAP, 1)) - ICSSG_REG(ECAP_TSCTR(ECAP))) *
				 ICSSG_NS_PER_CYCLE);
	if ((int32_t)(end + GTC_TICKS(run_period / 2u) - want_at) < 0) {
		wanted = 1;
		return;
	}
	ICSSG_REG(ECAP_CAP(ECAP, 3)) = want_period / ICSSG_NS_PER_CYCLE - 1u;
	ICSSG_REG(ECAP_CAP(ECAP, 4)) = want_duty / ICSSG_NS_PER_CYCLE;
	load_period = want_period;
	load_duty = want_duty;
	loading = 1;
}
#define ECAP_INIT() ecap_init()
#define ECAP_WANT(period, duty, at) ecap_want(period, duty, at)
#define ECAP_POLL() ecap_poll()
#else
#define ECAP_INIT() ((void)0)
#define ECAP_WANT(period, duty, at) ((void)0)
#define ECAP_POLL() ((void)0)
#endif

static uint64_t gtc(void)
{
	uint32_t hi, lo;

	do {
		hi = GTC_HI;
		lo = GTC_LO;
	} while (GTC_HI != hi);
	return ((uint64_t)hi << 32) | lo;
}

/* periods of period_ns in ms milliseconds, at least 1 */
static uint32_t periods_in(uint32_t ms, uint32_t period)
{
	uint32_t n = (uint32_t)((uint64_t)ms * 1000000u / period);

	return n ? n : 1u;
}

void main(void)
{
	volatile struct pwm_ctl *ctl = PWM_CTL;
	volatile struct pwm_out *out = PWM_OUT;
	volatile struct pwm_mod *mod = PWM_MOD;
	uint32_t ctl_seen = 0, periods_seen = 0;
	uint32_t period = 0, duty = 0, dead = 0, sweep = 0, steps = 0;
	/* The sweep: lo to hi in half periods, step ns a period plus rem/half. */
	uint32_t lo = 0, hi = 0, half = 1, step = 0, rem = 0, acc = 0, up = 1;
	/* The steps: step n is at frequency f = n / 5 (step_period), duty at = n % 5; the next at GTC next. */
	uint32_t f = 0, at = 0, next = 0, len = 0;
	uint32_t i, j;

	trace_init();
	/* The PRU waits for a valid record; none until the RTU asks. */
	next_seq = 0;
	PWM_NEXT->seq = 0;
	mod->magic = 0;
	mod->ctl_seq = 0;
	mod->updates = 0;
	mod->duty_ns = 0;
	mod->ecap_period_ns = 0;
	mod->ecap_duty_ns = 0;
	mod->ecap_periods = 0;
	mod->magic = PWM_MOD_MAGIC;
	ECAP_INIT();
	trace_puts("ICSSG0 Tx_PRU");
	trace_u32(AI64_SLICE);
	trace_puts(" up: the modulator, waiting for the RTU\n");

	for (;;) {
		uint32_t s = ctl->seq;

		ECAP_POLL();
		if (ctl->magic == PWM_CTL_MAGIC && s != ctl_seen) {
			period = ctl->period_ns;
			duty = ctl->duty_ns;
			dead = ctl->dead_ns;
			sweep = ctl->sweep_ms;
			steps = ctl->steps_ms;
			if (ctl->seq != s) {
				continue;
			}
			ctl_seen = s;
			lo = AI64_PWM_EDGE_MIN_NS;
			hi = pwm_duty_max(period, dead);
			if (steps) {
				for (i = 0; i < AI64_PWM_STEP_FREQS; i++) {
					uint32_t p = PWM_TICKS(1000000000u / step_hz[i]);
					uint32_t top = pwm_duty_max(p, dead);

					step_period[i] = p;
					for (j = 0; j < AI64_PWM_STEPS; j++) {
						uint32_t d = (uint32_t)((uint64_t)p * step_pct[j] / 100u);

						step_duty[i][j] = d < lo ? lo : d > top ? top : d;
					}
				}
				len = AI64_PWM_STEP_GTC(steps);
				i = ai64_pwm_step_at(gtc(), steps, &next);
				f = i / AI64_PWM_STEPS;
				at = i % AI64_PWM_STEPS;
				period = step_period[f];
				duty = step_duty[f][at];
			} else if (sweep) {
				half = periods_in(sweep, period) / 2u;
				half = half ? half : 1u;
				step = (hi - lo) / half;
				rem = (hi - lo) % half;
				acc = 0;
				up = 1;
				duty = lo;
			}
			write_next(period, duty, dead);
			ECAP_WANT(period, duty, GTC_LO);
			mod->ctl_seq = s;
			periods_seen = out->periods;
			trace_puts("control seq ");
			trace_u32(s);
			trace_puts(steps ? ": steps\n" : sweep ? ": sweeping\n" : ": fixed duty\n");
		}
		if ((steps || sweep) && out->periods != periods_seen) {
			periods_seen = out->periods;
			if (steps) {
				/*
				 * The PRU has just begun a period with what it last read;
				 * this write is for the one after, which starts a period
				 * from now. It takes the next step when that start is
				 * past the step's GTC time less half a period, so the
				 * change lands on the period end nearest it.
				 */
				uint32_t start = GTC_LO + GTC_TICKS(period);

				if ((int32_t)(start + GTC_TICKS(period / 2u) - next) < 0) {
					continue;
				}
				if (++at == AI64_PWM_STEPS) {
					at = 0;
					f = f + 1u == AI64_PWM_STEP_FREQS ? 0 : f + 1u;
					period = step_period[f];
				}
				duty = step_duty[f][at];
				ECAP_WANT(period, duty, next);
				next += len;
			} else {
				uint32_t d = step;

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
				ECAP_WANT(period, duty, GTC_LO + GTC_TICKS(period));
			}
			write_next(period, duty, dead);
		}
	}
}
