/* SPDX-License-Identifier: MIT */
/*
 * ICSSG0 PRU0: PWM in software. The core drives A on R30 bit 17 (P8.11,
 * PRG0_PRU0_GPO17) and B on bit 16 (P8.12, PRG0_PRU0_GPO16), and times
 * every edge by polling IEP0, which counts nanoseconds. Its slice partner,
 * PRU1, makes the same waveform with the ICSSG's PWM module instead.
 *
 * Each period it reads the next record (pwm_ipc.h) right after A's rising
 * edge, when the next edge is at least AI64_PWM_EDGE_MIN_NS away, and
 * reports in the out record. A poll of IEP0 takes a few cycles, so an edge
 * lands up to a few tens of nanoseconds after its time; the core counts
 * the ones more than AI64_PWM_LATE_NS late. It touches nothing outside
 * ICSSG0: no DDR, no mailbox, so nothing outside can delay an edge.
 */

#include <stdint.h>
#include "icssg.h"
#include "pwm_ipc.h"

#define AI64_TRACE_SIZE 256
#define AI64_TRACE_NAME "trace:pwm-pru0"
#include "ai64_rsc_trace.h"

volatile register uint32_t __R30;

#define PIN_A (1u << 17)
#define PIN_B (1u << 16)

#define NOW() ICSSG_REG(IEP_COUNT_LO(ICSSG_IEP0))

static uint32_t late, late_max;

/* Spin until IEP0 reaches t; how late that was, in nanoseconds. */
static inline uint32_t wait_until(uint32_t t)
{
	uint32_t now;

	do {
		now = NOW();
	} while ((int32_t)(now - t) < 0);
	return now - t;
}

static inline void account(uint32_t l)
{
	if (l > AI64_PWM_LATE_NS) {
		late++;
	}
	if (l > late_max) {
		late_max = l;
	}
}

/* The next record, if the Tx_PRU wrote a new one. */
static void read_next(uint32_t *seen, uint32_t *period, uint32_t *duty, uint32_t *dead)
{
	volatile struct pwm_next *n = PWM_NEXT;
	uint32_t s = n->seq, p, d, dt;

	if (s == *seen || (s & 1u)) {
		return;
	}
	p = n->period_ns;
	d = n->duty_ns;
	dt = n->dead_ns;
	if (n->seq != s) {
		return;
	}
	*seen = s;
	*period = p;
	*duty = d;
	*dead = dt;
}

void main(void)
{
	volatile struct pwm_out *out = PWM_OUT;
	uint32_t seen = 0, period = 0, duty = 0, dead = 0, periods = 0;
	uint32_t t, l;

	trace_init();
	__R30 &= ~(PIN_A | PIN_B);

	/*
	 * IEP0 is this core's clock: on the core clock, counting nanoseconds,
	 * free-running, with no compare resetting it.
	 */
	ICSSG_REG(IEP_GLOBAL_CFG(ICSSG_IEP0)) = IEP_DEFAULT_INC(ICSSG_NS_PER_CYCLE) |
						IEP_CMP_INC(ICSSG_NS_PER_CYCLE);
	ICSSG_REG(ICSSG_CFG_IEPCLK) |= ICSSG_IEPCLK_SYNC;
	ICSSG_REG(IEP_CMP_CFG(ICSSG_IEP0)) = 0;
	ICSSG_REG(IEP_GLOBAL_CFG(ICSSG_IEP0)) |= IEP_CNT_ENABLE;

	out->magic = 0;
	out->periods = 0;
	out->late = 0;
	out->late_max_ns = 0;
	out->magic = PWM_OUT_MAGIC;
	trace_puts("ICSSG0 PRU0 up: software PWM, A on P8.11, B on P8.12, timed by IEP0\n");

	/* The modulator writes the first record when its slice's RTU starts it. */
	while (seen == 0) {
		read_next(&seen, &period, &duty, &dead);
	}
	trace_puts("first period: ");
	trace_u32(period);
	trace_puts(" ns, duty ");
	trace_u32(duty);
	trace_puts(" ns\n");

	t = NOW() + 1000u;
	for (;;) {
		l = wait_until(t + PWM_T0_NS);
		__R30 |= PIN_A;
		account(l);

		/* The 400 ns before A falls: this period's parameters, and the report. */
		read_next(&seen, &period, &duty, &dead);
		periods++;
		out->periods = periods;
		out->next_seq = seen;
		out->period_ns = period;
		out->duty_ns = duty;
		out->dead_ns = dead;
		out->late = late;
		out->late_max_ns = late_max;

		l = wait_until(t + PWM_T0_NS + duty);
		__R30 &= ~PIN_A;
		account(l);
		l = wait_until(t + PWM_T0_NS + duty + dead);
		__R30 |= PIN_B;
		account(l);
		l = wait_until(t + period - dead);
		__R30 &= ~PIN_B;
		account(l);
		t += period;
	}
}
