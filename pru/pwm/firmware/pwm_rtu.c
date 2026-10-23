/* SPDX-License-Identifier: MIT */
/*
 * The RTU of each PWM slice: the slice's only link to the rest of the SoC.
 * Both slices build this file, with AI64_SLICE set; slice 0's build also
 * has AI64_RPMSG.
 *
 *   - It is the slice's fabric node (ai64_fabric.h), 8 or 9. A route can
 *     carry a pwm, dead, sweep, or steps action for it, so any core can
 *     change the slice's PWM: `ai64 fabric route c7x g0rtu1 pwm 20000 5000`.
 *   - Slice 0's also has an RPMsg channel to Linux ("rpmsg-raw", port 30),
 *     which takes status, pwm <period_ns> <duty_ns> (a fixed duty),
 *     dead <ns>, steps <ms>, and sweep <ms> (0 stops either). Anything
 *     else is echoed.
 *   - It starts the slice stepping through AI64_PWM_STEP_PCT every
 *     AI64_PWM_DEFAULT_STEPS_MS.
 *   - It writes what it was asked, clamped, to the control record for the
 *     slice's Tx_PRU (pwm_ipc.h), and about every 10 ms publishes the
 *     slice's struct ai64_pwm_status (ai64_shm.h) in its fabric page.
 *   - It says when the outputs follow a new request, and when the PRU
 *     reports late edges.
 */

#include <stdint.h>
#include "pwm_ipc.h"

#ifdef AI64_RPMSG
#define AI64_TRACE_SIZE 512
#define AI64_TRACE_NAME "trace:pwm-rtu0"
#include "ai64_rsc_rpmsg.h"
#include "ai64_rtu_rpmsg.h"
#else
#define AI64_TRACE_SIZE 384
#define AI64_TRACE_NAME "trace:pwm-rtu1"
#include "ai64_rsc_trace.h"
#endif
#include "ai64_text.h"

#define FAB_NODE (AI64_FAB_G0RTU0 + AI64_SLICE)

#if AI64_SLICE == 0
#define WHO     "ICSSG0 RTU0"
#define ENGINE  AI64_PWM_SOFT
#define OUTPUTS "PRU0's software PWM on P8.11 and P8.12"
#else
#define WHO     "ICSSG0 RTU1"
#define ENGINE  AI64_PWM_ICSSG
#define OUTPUTS "PRU1's ICSSG PWM on P9.12 and P9.15"
#endif

#define STATUS_TICKS AI64_FAB_US(10000)
#define LATE_TICKS   AI64_FAB_US(1000000)

static uint32_t ctl_seq, ack_seq, late_told, late_at;
static char tx[176];
#ifdef AI64_RPMSG
static char cmd[56];
#endif

/* Trace a line, and send it to Linux if this one has RPMsg. */
static void tell(uint16_t n)
{
	trace_puts(tx);
	trace_putc('\n');
#ifdef AI64_RPMSG
	rtu_rpmsg_send(tx, n);
#else
	(void)n;
#endif
}

static uint16_t say_ns(uint16_t n, const char *what, uint32_t v)
{
	n = append_str(tx, n, sizeof(tx), what);
	n = append_u32(tx, n, sizeof(tx), v);
	return append_str(tx, n, sizeof(tx), " ns");
}

/* The control record, fields first, seq last. */
static void ask(uint32_t period, uint32_t duty, uint32_t dead, uint32_t sweep, uint32_t steps)
{
	volatile struct pwm_ctl *ctl = PWM_CTL;

	pwm_clamp(&period, &duty, &dead);
	if (sweep > AI64_PWM_SWEEP_MAX_MS) {
		sweep = AI64_PWM_SWEEP_MAX_MS;
	}
	if (steps > AI64_PWM_STEPS_MAX_MS) {
		steps = AI64_PWM_STEPS_MAX_MS;
	}
	ctl->period_ns = period;
	ctl->duty_ns = duty;
	ctl->dead_ns = dead;
	ctl->sweep_ms = sweep;
	ctl->steps_ms = steps;
	ctl_seq++;
	ctl->seq = ctl_seq;
}

static uint16_t reply_ask(const char *who)
{
	volatile struct pwm_ctl *ctl = PWM_CTL;
	uint16_t n = 0;

	n = append_str(tx, n, sizeof(tx), who);
	n = append_str(tx, n, sizeof(tx), " asked " WHO " for");
	n = say_ns(n, " period ", ctl->period_ns);
	n = say_ns(n, ", duty ", ctl->duty_ns);
	n = say_ns(n, ", dead ", ctl->dead_ns);
	if (ctl->steps_ms) {
		n = append_str(tx, n, sizeof(tx), ", duty stepping every ");
		n = append_u32(tx, n, sizeof(tx), ctl->steps_ms);
		n = append_str(tx, n, sizeof(tx), " ms");
	}
	if (ctl->sweep_ms) {
		n = append_str(tx, n, sizeof(tx), ", duty sweeping every ");
		n = append_u32(tx, n, sizeof(tx), ctl->sweep_ms);
		n = append_str(tx, n, sizeof(tx), " ms");
	}
	n = append_str(tx, n, sizeof(tx), "; it wrote control seq ");
	return append_u32(tx, n, sizeof(tx), ctl_seq);
}

/* Change one thing, keep the rest of the last request. A duty is fixed. */
static uint16_t set_pwm(const char *who, uint32_t period, uint32_t duty)
{
	ask(period, duty, PWM_CTL->dead_ns, 0, 0);
	return reply_ask(who);
}

static uint16_t set_dead(const char *who, uint32_t dead)
{
	ask(PWM_CTL->period_ns, PWM_CTL->duty_ns, dead, PWM_CTL->sweep_ms, PWM_CTL->steps_ms);
	return reply_ask(who);
}

static uint16_t set_sweep(const char *who, uint32_t ms)
{
	ask(PWM_CTL->period_ns, PWM_CTL->duty_ns, PWM_CTL->dead_ns, ms, 0);
	return reply_ask(who);
}

static uint16_t set_steps(const char *who, uint32_t ms)
{
	ask(PWM_CTL->period_ns, PWM_CTL->duty_ns, PWM_CTL->dead_ns, 0, ms);
	return reply_ask(who);
}

struct ai64_fab_msg;
static uint32_t fab_action(struct ai64_fab_msg *m);

#define AI64_FAB_NOW()    (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_ACTION(m) fab_action(m)
#include "ai64_fabric.h"

static uint32_t fab_action(struct ai64_fab_msg *m)
{
	static const char who[] = "A fabric route";

	if (m->action == AI64_FAB_ACT_PWM) {
		tell(set_pwm(who, m->arg0, m->arg1));
	} else if (m->action == AI64_FAB_ACT_DEAD) {
		tell(set_dead(who, m->arg0));
	} else if (m->action == AI64_FAB_ACT_SWEEP) {
		tell(set_sweep(who, m->arg0));
	} else if (m->action == AI64_FAB_ACT_STEPS) {
		tell(set_steps(who, m->arg0));
	} else {
		return 1;
	}
	return 0;
}

#ifdef AI64_RPMSG
static uint16_t reply_status(void)
{
	volatile struct pwm_out *out = PWM_OUT;
	uint16_t n = 0;

	n = append_str(tx, n, sizeof(tx), OUTPUTS ":");
	n = say_ns(n, " period ", out->period_ns);
	n = say_ns(n, ", duty ", out->duty_ns);
	n = say_ns(n, ", dead ", out->dead_ns);
	n = append_str(tx, n, sizeof(tx), ", ");
	n = append_u32(tx, n, sizeof(tx), out->periods);
	n = append_str(tx, n, sizeof(tx), " periods, ");
	n = append_u32(tx, n, sizeof(tx), out->late);
	return append_str(tx, n, sizeof(tx), " late edges");
}
#endif

static void publish(uint32_t now)
{
	volatile struct ai64_pwm_status *s =
		(volatile struct ai64_pwm_status *)FAB_PAGE(FAB_NODE)->app;
	volatile struct pwm_ctl *ctl = PWM_CTL;
	volatile struct pwm_out *out = PWM_OUT;
	volatile struct pwm_mod *mod = PWM_MOD;
	uint32_t late = out->late;
	uint16_t n;

	if (out->magic != PWM_OUT_MAGIC || mod->magic != PWM_MOD_MAGIC) {
		return;
	}
	if (mod->ctl_seq == ctl_seq && ack_seq != ctl_seq && out->next_seq) {
		ack_seq = ctl_seq;
		n = append_str(tx, 0, sizeof(tx), OUTPUTS " follows control seq ");
		n = append_u32(tx, n, sizeof(tx), ack_seq);
		n = say_ns(n, ": period ", out->period_ns);
		n = say_ns(n, ", duty ", out->duty_ns);
		tell(n);
	}
	/* At most a line a second about late edges. */
	if (late != late_told && FAB_AFTER(now, late_at)) {
		late_at = now + LATE_TICKS;
		n = append_str(tx, 0, sizeof(tx), OUTPUTS " has ");
		n = append_u32(tx, n, sizeof(tx), late);
		n = append_str(tx, n, sizeof(tx), " late edges, up from ");
		n = append_u32(tx, n, sizeof(tx), late_told);
		tell(n);
		late_told = late;
	}
	s->magic = 0;
	s->engine = ENGINE;
	s->unit = 0;
	s->channel = AI64_SLICE;
#ifdef AI64_RPMSG
	s->rpmsg = 1;
#else
	s->rpmsg = 0;
#endif
	s->ask_period_ns = ctl->period_ns;
	s->ask_duty_ns = ctl->duty_ns;
	s->ask_dead_ns = ctl->dead_ns;
	s->sweep_ms = ctl->sweep_ms;
	s->steps_ms = ctl->steps_ms;
	s->run_period_ns = out->period_ns;
	s->run_duty_ns = out->duty_ns;
	s->run_dead_ns = out->dead_ns;
	s->periods = out->periods;
	s->late = late;
	s->late_max_ns = out->late_max_ns;
	s->ctl_seq = ctl_seq;
	s->ctl_ack = mod->ctl_seq;
	s->at = now;
	s->magic = AI64_PWM_MAGIC;
}

#ifdef AI64_RPMSG
static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void run_cmd(void)
{
	static const char who[] = "Linux";
	uint32_t a, b;
	uint16_t n;

	trace_puts("Linux: ");
	trace_puts(cmd);
	trace_putc('\n');
	if (parse_two(cmd, "pwm", &a, &b)) {
		tell(set_pwm(who, a, b));
	} else if (parse_one(cmd, "dead", &a)) {
		tell(set_dead(who, a));
	} else if (parse_one(cmd, "sweep", &a)) {
		tell(set_sweep(who, a));
	} else if (parse_one(cmd, "steps", &a)) {
		tell(set_steps(who, a));
	} else if (same(cmd, "status")) {
		rtu_rpmsg_send(tx, reply_status());
	} else {
		n = append_str(tx, 0, sizeof(tx), "Linux sent \"");
		n = append_str(tx, n, sizeof(tx), cmd);
		n = append_str(tx, n, sizeof(tx), "\" to " WHO ", and it echoed it");
		rtu_rpmsg_send(tx, n);
	}
}
#endif

void main(void)
{
	volatile struct pwm_ctl *ctl = PWM_CTL;
	uint32_t now, status_at;
	uint16_t n;

	trace_init();
	trace_puts(WHO " up\n");
#ifdef AI64_RPMSG
	rtu_rpmsg_up();
	trace_puts("channel rpmsg-raw port 30 is up\n");
#endif

	/*
	 * A fresh control record with the defaults, stepping. Start this core first:
	 * the Tx_PRU waits for it, and the PRU for the Tx_PRU.
	 */
	ctl_seq = ctl->magic == PWM_CTL_MAGIC ? ctl->seq : 0;
	ctl->magic = 0;
	ask(AI64_PWM_DEFAULT_PERIOD_NS, AI64_PWM_DEFAULT_DUTY_NS, AI64_PWM_DEFAULT_DEAD_NS, 0,
	    AI64_PWM_DEFAULT_STEPS_MS);
	ctl->magic = PWM_CTL_MAGIC;
	late_told = 0;
	late_at = AI64_FAB_NOW();

	fab_init(FAB_NODE);
	n = append_str(tx, 0, sizeof(tx), WHO " is fabric node ");
	append_u32(tx, n, sizeof(tx), FAB_NODE);
	trace_puts(tx);
	trace_putc('\n');

	status_at = AI64_FAB_NOW();
	for (;;) {
		fab_poll();
#ifdef AI64_RPMSG
		if (rtu_rpmsg_receive(cmd, sizeof(cmd))) {
			run_cmd();
		}
#endif
		now = AI64_FAB_NOW();
		if (FAB_AFTER(now, status_at)) {
			status_at = now + STATUS_TICKS;
			publish(now);
		}
	}
}
