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
 *   - It starts the slice stepping through AI64_PWM_STEP_HZ and
 *     AI64_PWM_STEP_PCT, a step every AI64_PWM_DEFAULT_STEPS_MS.
 *   - It writes what it was asked, clamped, to the control record for the
 *     slice's Tx_PRU (pwm_ipc.h), and about every 10 ms publishes the
 *     slice's struct ai64_pwm_status (ai64_shm.h) in its fabric page.
 *     Slice 0's Tx_PRU also runs eCAP0 on P8.15, a copy of A from the same
 *     request, and RTU0 publishes its record too, AI64_PWM_STATUS2_WORDS
 *     into the app block.
 *   - It says when the outputs follow a new request, and when the PRU
 *     reports late edges.
 *   - Slice 1's stamps each window of P8.31 samples its PRU fills
 *     (ai64_pipe.h) with the GTC, and while the pipeline runs rings
 *     R5F0_0's mailbox with it. The PRU has no time to read the GTC: a
 *     read from it stalls the core for longer than an edge can wait.
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
#define OUTPUTS "software PWM on P8.11 (A) and P8.12 (B)"
#define ECAP_OUT "hardware PWM on P8.15 (eCAP0, a copy of A)"
#define ASKED   OUTPUTS ", and its copy on P8.15"
#else
#define WHO     "ICSSG0 RTU1"
#define ENGINE  AI64_PWM_ICSSG
#define OUTPUTS "hardware PWM on P9.12 (A) and P9.15 (B)"
#define ASKED   OUTPUTS
#endif

#define STATUS_TICKS AI64_FAB_US(10000)
#define LATE_TICKS   AI64_FAB_US(1000000)

static uint32_t ctl_seq, ack_seq, late_told, late_at;
static char tx[224];
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
	n = append_str(tx, n, sizeof(tx), " asked " WHO " for " ASKED ": ");
	if (ctl->steps_ms) {
		n = append_str(tx, n, sizeof(tx), "10, 15, 20 kHz x 10-90% duty, a step every ");
		n = append_u32(tx, n, sizeof(tx), ctl->steps_ms);
		n = append_str(tx, n, sizeof(tx), " ms");
	} else {
		n = say_ns(n, "period ", ctl->period_ns);
		n = say_ns(n, ", A high ", ctl->duty_ns);
		if (ctl->sweep_ms) {
			n = append_str(tx, n, sizeof(tx), " sweeping every ");
			n = append_u32(tx, n, sizeof(tx), ctl->sweep_ms);
			n = append_str(tx, n, sizeof(tx), " ms");
		}
	}
	n = say_ns(n, ", dead band ", ctl->dead_ns);
	n = append_str(tx, n, sizeof(tx), " (request ");
	n = append_u32(tx, n, sizeof(tx), ctl_seq);
	return append_str(tx, n, sizeof(tx), ")");
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

#if AI64_SLICE == 1
#include "ai64_pipe.h"

#define PIPE      ((volatile struct ai64_pipe *)AI64_PIPE_ADDR)
#define PIPE_PRU  ((volatile struct ai64_pipe_pru *)(AI64_PIPE_SMEM_LOCAL + AI64_PIPE_PRU_OFF))
#define PIPE_SLOT(w) \
	((volatile struct ai64_pipe_slot *)(AI64_PIPE_SMEM_LOCAL + \
					    ((w) % AI64_PIPE_SLOTS) * AI64_PIPE_SLOT_SIZE))
#define GTC_HI()  (*(volatile uint32_t *)(AI64_GTC_ADDR + 12u))

static uint32_t pipe_seen, pipe_synced, pipe_weights;

static void pipe_init(void)
{
	ai64_pipe_stage_reset(&PIPE->stage[AI64_PIPE_RTU]);
	pipe_seen = 0;
	pipe_synced = 0;
	pipe_weights = PIPE->ctl.weights_seq;
}

/*
 * The windows PRU1 filled since the last look: each one's end is where its
 * last chunk came (window w is halves w and w + 1), counted back from the
 * chunk PRU1 is on now, at 8 us a chunk. So the stamp is within a chunk of the truth however late this
 * core comes round.
 */
static void pipe_poll(void)
{
	volatile struct ai64_pipe_pru *pru = PIPE_PRU;
	volatile struct ai64_pipe_stage *st = &PIPE->stage[AI64_PIPE_RTU];
	uint32_t wins = pru->wins, chunks, hi, lo, w, lat, work;
	uint64_t now;

	if (pru->magic != AI64_PIPE_MAGIC || wins == pipe_seen) {
		return;
	}
	/*
	 * The first look (shared RAM keeps the last run's record until PRU1
	 * starts again), or PRU1 started again: from its newest window on.
	 */
	if (!pipe_synced || wins - pipe_seen > AI64_PIPE_RESYNC) {
		pipe_synced = 1;
		pipe_seen = wins - 1u;
	}
	do {
		hi = GTC_HI();
		lo = AI64_FAB_NOW();
	} while (GTC_HI() != hi);
	chunks = pru->chunks;
	now = ((uint64_t)hi << 32) | lo;
	/* PRU1 starts on window w's slot again once window w + 15 is in. */
	if (wins - pipe_seen > AI64_PIPE_SLOTS - 1u) {
		st->dropped += wins - pipe_seen - (AI64_PIPE_SLOTS - 1u);
		pipe_seen = wins - (AI64_PIPE_SLOTS - 1u);
	}
	for (w = pipe_seen; w != wins; w++) {
		volatile struct ai64_pipe_slot *sl = PIPE_SLOT(w);
		uint64_t end = now - (uint64_t)(chunks - (w + 2u) * AI64_PIPE_HOP_CHUNKS) * AI64_PIPE_CHUNK_GTC;

		sl->tag.win = w;
		sl->tag.t_end_lo = (uint32_t)end;
		sl->tag.t_end_hi = (uint32_t)(end >> 32);
		sl->tag.t_rtu = lo;
	}
	pipe_seen = wins;
	w = wins - 1u;
	lat = lo - PIPE_SLOT(w)->tag.t_end_lo;
	if (PIPE->ctl.magic == AI64_PIPE_MAGIC && PIPE->ctl.run) {
		*(volatile uint32_t *)AI64_PIPE_MBOX_MSG(AI64_PIPE_MBOX_R5F0_0) = w;
	}
	if (PIPE->ctl.weights_seq != pipe_weights) {
		pipe_weights = PIPE->ctl.weights_seq;
		ai64_pipe_stage_reset(st);
	}
	work = AI64_FAB_NOW() - lo;
	st->windows = wins;
	st->last = w;
	st->lat_last = lat;
	st->lat_sum += lat;
	if (lat < st->lat_min) {
		st->lat_min = lat;
	}
	if (lat > st->lat_max) {
		st->lat_max = lat;
	}
	st->work_sum += work;
	if (work > st->work_max) {
		st->work_max = work;
	}
}
#endif

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

	n = append_str(tx, n, sizeof(tx), OUTPUTS " now:");
	n = say_ns(n, " period ", out->period_ns);
	n = say_ns(n, ", A high ", out->duty_ns);
	n = say_ns(n, ", dead band ", out->dead_ns);
	n = append_str(tx, n, sizeof(tx), ", ");
	n = append_u32(tx, n, sizeof(tx), out->periods);
	n = append_str(tx, n, sizeof(tx), " periods, ");
	n = append_u32(tx, n, sizeof(tx), out->late);
	return append_str(tx, n, sizeof(tx), " late edges");
}

static uint16_t reply_ecap(void)
{
	volatile struct pwm_mod *mod = PWM_MOD;
	uint16_t n = 0;

	n = append_str(tx, n, sizeof(tx), ECAP_OUT " now:");
	n = say_ns(n, " period ", mod->ecap_period_ns);
	n = say_ns(n, ", high ", mod->ecap_duty_ns);
	n = append_str(tx, n, sizeof(tx), ", ");
	n = append_u32(tx, n, sizeof(tx), mod->ecap_periods);
	return append_str(tx, n, sizeof(tx), " periods");
}
#endif

#if AI64_SLICE == 0
/* The eCAP's record: the slice's request, and what eCAP0 runs. */
static void publish_ecap(uint32_t now)
{
	volatile struct ai64_pwm_status *s = (volatile struct ai64_pwm_status *)
		&FAB_PAGE(FAB_NODE)->app[AI64_PWM_STATUS2_WORDS];
	volatile struct pwm_ctl *ctl = PWM_CTL;
	volatile struct pwm_mod *mod = PWM_MOD;

	s->magic = 0;
	s->engine = AI64_PWM_ECAP;
	s->unit = 0;
	s->channel = 0;
	s->rpmsg = 1;
	s->ask_period_ns = ctl->period_ns;
	s->ask_duty_ns = ctl->duty_ns;
	s->ask_dead_ns = 0;
	s->sweep_ms = ctl->sweep_ms;
	s->steps_ms = ctl->steps_ms;
	s->run_period_ns = mod->ecap_period_ns;
	s->run_duty_ns = mod->ecap_duty_ns;
	s->run_dead_ns = 0;
	s->periods = mod->ecap_periods;
	s->late = 0;
	s->late_max_ns = 0;
	s->ctl_seq = ctl_seq;
	s->ctl_ack = mod->ctl_seq;
	s->at = now;
	s->magic = AI64_PWM_MAGIC;
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
		n = append_str(tx, 0, sizeof(tx), OUTPUTS " runs request ");
		n = append_u32(tx, n, sizeof(tx), ack_seq);
		n = say_ns(n, ", now period ", out->period_ns);
		n = say_ns(n, ", A high ", out->duty_ns);
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
#if AI64_SLICE == 0
	publish_ecap(now);
#endif
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
		rtu_rpmsg_send(tx, reply_ecap());
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

#if AI64_SLICE == 1
	pipe_init();
#endif
	status_at = AI64_FAB_NOW();
	for (;;) {
		fab_poll();
#if AI64_SLICE == 1
		pipe_poll();
#endif
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
