/* SPDX-License-Identifier: MIT */
/*
 * The RTU of each ring slice: ICSSG0 slice 1, ICSSG1 slice 0, and ICSSG1
 * slice 1. The Makefile builds it once per slice, with AI64_ICSSG and
 * AI64_SLICE set.
 *
 * The slice's PRU and Tx_PRU pass the ring's token (ring/main.c) and touch
 * nothing outside their ICSSG. This core is the slice's only link to the
 * rest of the SoC, and does the talking for them:
 *   - It is the slice's fabric node (ai64_fabric.h): 9, 10, or 11. About
 *     every 10 ms it copies the ring record into the app block of its
 *     fabric page (struct ai64_slice_status), where any core can read it.
 *   - It writes the ring's control record: how many laps make a window.
 *     The PRU reads it once a window, and toggles its header pin once a
 *     window, so the pin's frequency follows.
 *   - It watches the ring's error count and says when it grows, and says
 *     when the PRU has applied a new window.
 * A window request comes from a fabric route ("ai64 fabric route g1rtu1
 * window 4096"), so any core can make one. Built with AI64_RPMSG (ICSSG1
 * RTU0), it also has an RPMsg channel to Linux, which takes "status" and
 * "window <laps>"; anything else is echoed. The other two have no RPMsg.
 */

#include <stdint.h>
#include "ai64_shm.h"

#ifdef AI64_RPMSG
#define AI64_TRACE_SIZE 512
#define AI64_TRACE_NAME "trace:ring-rtu"
#include "ai64_rsc_rpmsg.h"
#include "ai64_rtu_rpmsg.h"
#else
#define AI64_TRACE_SIZE 384
#define AI64_TRACE_NAME "trace:ring-rtu"
#include "ai64_rsc_trace.h"
#endif
#include "ai64_text.h"

/* Its fabric node: 9 for ICSSG0 slice 1, 10 and 11 for ICSSG1. */
#define FAB_NODE (AI64_FAB_G0RTU0 + 2u * AI64_ICSSG + AI64_SLICE)

/* The header pin the slice's PRU toggles (the overlay muxes it). */
#if AI64_ICSSG == 0
#define RING_PIN "P8.44"
#elif AI64_SLICE == 0
#define RING_PIN "P8.18"
#else
#define RING_PIN "P8.21"
#endif

#define STATUS_TICKS AI64_FAB_US(10000)
#define RATE_TICKS   AI64_FAB_US(1000000)

static volatile struct ai64_ring *const ring = (volatile struct ai64_ring *)AI64_RING_LOCAL(AI64_SLICE);
static volatile struct ai64_ring_ctl *const ctl = (volatile struct ai64_ring_ctl *)AI64_RING_CTL_LOCAL(AI64_SLICE);
static uint32_t ctl_seq;

static char tx[160];
#ifdef AI64_RPMSG
static char cmd[56];
#endif

static uint16_t say_who(char *dst, uint16_t n, uint16_t max)
{
	n = append_str(dst, n, max, "ICSSG");
	n = append_u32(dst, n, max, AI64_ICSSG);
	return append_str(dst, n, max, AI64_SLICE ? " RTU1" : " RTU0");
}

static uint16_t say_pru(char *dst, uint16_t n, uint16_t max)
{
	n = append_str(dst, n, max, "ICSSG");
	n = append_u32(dst, n, max, AI64_ICSSG);
	return append_str(dst, n, max, AI64_SLICE ? " PRU1" : " PRU0");
}

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

static uint32_t set_window(uint32_t laps)
{
	if (laps < AI64_RING_WINDOW_MIN) {
		laps = AI64_RING_WINDOW_MIN;
	}
	if (laps > AI64_RING_WINDOW_MAX) {
		laps = AI64_RING_WINDOW_MAX;
	}
	ctl->window = laps;
	ctl_seq++;
	ctl->seq = ctl_seq;
	return laps;
}

static uint16_t reply_window(const char *who, uint32_t laps)
{
	uint16_t n = 0, max = sizeof(tx);

	n = append_str(tx, n, max, who);
	n = append_str(tx, n, max, " asked ");
	n = say_who(tx, n, max);
	n = append_str(tx, n, max, " for a ");
	n = append_u32(tx, n, max, laps);
	n = append_str(tx, n, max, "-lap window, and it wrote control seq ");
	n = append_u32(tx, n, max, ctl_seq);
	n = append_str(tx, n, max, " for ");
	return say_pru(tx, n, max);
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
	if (m->action != AI64_FAB_ACT_WINDOW) {
		return 1;
	}
	tell(reply_window("A fabric route", set_window(m->arg0)));
	return 0;
}

/* The ring record, as the slice status in this node's fabric page. */
struct status_state {
	uint32_t run, laps, errors, ack;
	uint32_t rate_at, rate_laps;
	uint32_t rate;
	uint32_t errors_seen;
};
static struct status_state st;

static void publish(uint32_t now)
{
	volatile struct ai64_slice_status *s =
		(volatile struct ai64_slice_status *)FAB_PAGE(FAB_NODE)->app;
	uint32_t run = ring->run, laps = ring->laps, errors = ring->errors;
	uint32_t ack = ring->ctl_ack;
	uint16_t n;

	if (ring->magic != AI64_RING_MAGIC) {
		return;
	}
	if (run != st.run) {
		/* The PRU started a run and zeroed the counts. */
		st.run = run;
		st.errors = errors;
		st.ack = ack;
		st.rate_laps = laps;
		st.rate_at = now;
		st.rate = 0;
	}
	if (errors != st.errors) {
		st.errors_seen++;
		n = append_str(tx, 0, sizeof(tx), "the ");
		n = say_pru(tx, n, sizeof(tx));
		n = append_str(tx, n, sizeof(tx), " ring's error count went from ");
		n = append_u32(tx, n, sizeof(tx), st.errors);
		n = append_str(tx, n, sizeof(tx), " to ");
		n = append_u32(tx, n, sizeof(tx), errors);
		tell(n);
		st.errors = errors;
	}
	if (ack != st.ack) {
		st.ack = ack;
		n = say_pru(tx, 0, sizeof(tx));
		n = append_str(tx, n, sizeof(tx), " applied control seq ");
		n = append_u32(tx, n, sizeof(tx), ack);
		n = append_str(tx, n, sizeof(tx), ": its pin on " RING_PIN " now toggles every ");
		n = append_u32(tx, n, sizeof(tx), ring->window);
		n = append_str(tx, n, sizeof(tx), " laps");
		tell(n);
	}
	if (FAB_AFTER(now, st.rate_at + RATE_TICKS)) {
		/* Laps a second, scaled from however long it really was, in
		 * milliseconds; 32 bits are enough. */
		uint32_t ms = (now - st.rate_at) / (AI64_GTC_HZ / 1000u);
		uint32_t d = laps - st.rate_laps;

		st.rate = d / ms * 1000u + d % ms * 1000u / ms;
		st.rate_laps = laps;
		st.rate_at = now;
	}
	s->magic = 0;
	s->icssg = AI64_ICSSG;
	s->slice = AI64_SLICE;
#ifdef AI64_RPMSG
	s->rpmsg = 1;
#else
	s->rpmsg = 0;
#endif
	s->run = run;
	s->laps = laps;
	s->txpru_hops = ring->txpru_hops;
	s->errors = errors;
	s->lap_last = ring->lap_last;
	s->lap_min = ring->lap_min;
	s->lap_max = ring->lap_max;
	s->window = ring->window;
	s->ctl_seq = ctl_seq;
	s->ctl_ack = ack;
	s->rate = st.rate;
	s->errors_seen = st.errors_seen;
	s->at = now;
	s->magic = AI64_SLICE_MAGIC;
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

static uint16_t reply_status(void)
{
	uint16_t n = 0, max = sizeof(tx);

	n = say_who(tx, n, max);
	n = append_str(tx, n, max, ": ring run ");
	n = append_u32(tx, n, max, ring->run);
	n = append_str(tx, n, max, ", ");
	n = append_u32(tx, n, max, st.rate);
	n = append_str(tx, n, max, " laps/s, round trip ");
	n = append_u32(tx, n, max, ring->lap_last);
	n = append_str(tx, n, max, " cycles, window ");
	n = append_u32(tx, n, max, ring->window);
	n = append_str(tx, n, max, " laps, errors ");
	return append_u32(tx, n, max, ring->errors);
}

static uint16_t reply_echo(void)
{
	uint16_t n = 0, max = sizeof(tx);

	n = append_str(tx, n, max, "Linux sent \"");
	n = append_str(tx, n, max, cmd);
	n = append_str(tx, n, max, "\" to ");
	n = say_who(tx, n, max);
	return append_str(tx, n, max, ", and it echoed it");
}

static void run_cmd(void)
{
	uint32_t a;

	trace_puts("Linux: ");
	trace_puts(cmd);
	trace_putc('\n');
	if (same(cmd, "status")) {
		rtu_rpmsg_send(tx, reply_status());
		return;
	}
	if (parse_one(cmd, "window", &a)) {
		tell(reply_window("Linux", set_window(a)));
		return;
	}
	rtu_rpmsg_send(tx, reply_echo());
}
#endif

void main(void)
{
	uint32_t now, status_at;
	uint16_t n;

	trace_init();
	say_who(tx, 0, sizeof(tx));
	trace_puts(tx);
	trace_puts(" up\n");
#ifdef AI64_RPMSG
	rtu_rpmsg_up();
	trace_puts("channel rpmsg-raw port 30 is up\n");
#endif

	/*
	 * A new control record, back to the default window. The PRU applies
	 * whatever the record says when it starts, so start this core first.
	 */
	ctl_seq = ctl->magic == AI64_RING_CTL_MAGIC ? ctl->seq : 0;
	ctl->magic = 0;
	set_window(AI64_RING_WINDOW);
	ctl->magic = AI64_RING_CTL_MAGIC;

	fab_init(FAB_NODE);
	n = say_who(tx, 0, sizeof(tx));
	n = append_str(tx, n, sizeof(tx), " is fabric node ");
	append_u32(tx, n, sizeof(tx), FAB_NODE);
	trace_puts(tx);
	trace_putc('\n');

	st.run = 0xffffffffu;
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
