/* SPDX-License-Identifier: MIT */
/*
 * The fabric: every demo core talks to every other one, with no Linux in
 * the path. Ten nodes take part: Linux (while `ai64 fabric` runs), the four
 * main-domain R5Fs, both C66x, the C7x, and the RTU of each ICSSG0 slice.
 * The RTU is its slice's link to the rest of the SoC; the slice's PRU and
 * Tx_PRU keep to their real-time work and never touch the fabric. ICSSG1
 * runs TI's multicore scheduler on all six of its cores (pru/
 * multicore_scheduler/), with no core left to talk, so it is not a node.
 * The layout has room for AI64_FAB_SLOTS nodes; the last two are spare.
 * Linux, Zephyr, both DSP compilers, and the PRU compiler all include this
 * file, so it only uses 32-bit fields at fixed offsets.
 *
 * Three pieces of SoC hardware carry it, all reachable from every core:
 *   DDR at AI64_FAB_ADDR, which the overlay reserves: one page per node
 *     holding its inbox, plus its statistics for Linux to read. Every
 *     core maps it uncached.
 *   Mailbox cluster 11 (Linux leaves it disabled): one FIFO per node, for
 *     doorbells. A sender writes its node number there after a message,
 *     so the receiver only reads DDR when something has arrived.
 *   The GTC, the 200 MHz SoC timebase Linux's arch timer also counts.
 *     Messages carry GTC stamps, so one-way latencies are real. The C66x
 *     reaches it through an MPAX window (c66x/main.c).
 * and a fourth, hardware spinlock AI64_FAB_LOCK, guards a tally that every
 * node adds to (struct ai64_fab_tally).
 *
 * Uncached reads are slow, from 0.1 to 0.5 us each, and writes are posted
 * and cheap. So a node only ever reads its own page, and writes to others:
 * a message goes into the receiver's page, and a tour's or route's hop
 * stamps go straight into its origin's page.
 *
 * The inbox: a slot for each sender and message class. Only that sender
 * writes the slot. It writes the message, then bumps its count for the
 * class in seq[me] (or rseq[me] for routes) in the receiver's page, then
 * rings the doorbell. The receiver keeps the counts it last saw, so a
 * changed count is a new message, and nothing is ever cleared. Each class
 * has at most one message in flight per sender and receiver, so a slot is
 * never overwritten before it is read:
 *   REQ    a ping. A node pings each peer every AI64_FAB_PING_TICKS and
 *          waits for the pong before the next ping to that peer.
 *   REPLY  the pong. Its ping only comes after the last pong was read.
 *   TOUR   R5F0_0 sends one tour at a time round every node that answers
 *          its pings, and waits for it to come back.
 *   ROUTE  `ai64 fabric route` sends one, along the route it is given, and
 *          waits for it. Its counts are in rseq, not seq, so that a second
 *          Linux process can send routes while the viewer runs.
 * A node that starts again takes the counts it finds as seen. A message
 * that arrives for a stopped node is lost, and its sender times out.
 */
#ifndef AI64_FABRIC_H_
#define AI64_FABRIC_H_

#include <stdint.h>
#include "ai64_shm.h"

#define AI64_FAB_ADDR     0xA9000000u
#define AI64_FAB_SIZE      0x00020000u /* what the cores map; the overlay reserves 1 MiB */
#define AI64_FAB_PAGE      0x2000u
#define AI64_FAB_SLOTS     12u /* what the layout has room for */
#define AI64_FAB_NODES     10u /* the nodes there are */
#define AI64_FAB_MAGIC     0x46364941u /* "AI6F" */
#define AI64_FAB_TALLY_OFF (AI64_FAB_SLOTS * AI64_FAB_PAGE)

#define AI64_FAB_LINUX     0u
#define AI64_FAB_R5F0_0    1u
#define AI64_FAB_R5F0_1    2u
#define AI64_FAB_R5F1_0    3u
#define AI64_FAB_R5F1_1    4u
#define AI64_FAB_C66X0     5u
#define AI64_FAB_C66X1     6u
#define AI64_FAB_C7X       7u
#define AI64_FAB_G0RTU0    8u /* ICSSG0 RTU0: slice 0, the software PWM */
#define AI64_FAB_G0RTU1    9u /* ICSSG0 RTU1: slice 1, the ICSSG PWM */

#define AI64_FAB_REQ       0u
#define AI64_FAB_REPLY     1u
#define AI64_FAB_TOUR      2u
#define AI64_FAB_ROUTE     3u
#define AI64_FAB_CLASSES   4u

/* The GTC is AI64_GTC_ADDR (ai64_shm.h). */
#define AI64_FAB_US(us)    ((uint32_t)(us) * (AI64_GTC_HZ / 1000000u))

/* Mailbox cluster 11. FIFO n is node n's doorbell; each holds 4 messages. */
#define AI64_FAB_MBOX_ADDR  0x31F8B000u
#define AI64_FAB_MBOX_DEPTH 4u

/* Hardware spinlock: reading the lock register returns 0 and takes it,
 * or returns 1 when someone else holds it. Writing 0 frees it. */
#define AI64_SPINLOCK_ADDR 0x30E00000u
#define AI64_FAB_LOCK      255u
#define AI64_FAB_LOCK_OFF  (0x800u + 4u * AI64_FAB_LOCK)

/* Timing, in GTC ticks. */
#define AI64_FAB_PING_TICKS    AI64_FAB_US(10000)   /* each peer, every 10 ms */
#define AI64_FAB_TIMEOUT_TICKS AI64_FAB_US(50000)
#define AI64_FAB_RETRY_TICKS   AI64_FAB_US(500000)  /* a peer that timed out */
#define AI64_FAB_WINDOW_TICKS  AI64_FAB_US(1000000) /* min and max windows */
#define AI64_FAB_SCAN_TICKS    AI64_FAB_US(1000)    /* full inbox scan */
#define AI64_FAB_TALLY_TICKS   AI64_FAB_US(10000)
#define AI64_FAB_TOUR_TICKS    AI64_FAB_US(10000)
#define AI64_FAB_TOUR_TIMEOUT  AI64_FAB_US(100000)
#define AI64_FAB_LOCK_TRIES    1000u

#define AI64_FAB_MAX_HOPS  16u

/*
 * What a route does at action_node on its way. Each PWM generator's
 * talking core takes them: ICSSG0 RTU0 and RTU1 all four, R5F0_0 (its
 * EHRPWM) pwm and steps.
 */
#define AI64_FAB_ACT_NONE  0u
#define AI64_FAB_ACT_PWM   1u /* arg0 period ns, arg1 duty ns, as "pwm" */
#define AI64_FAB_ACT_DEAD  2u /* arg0 dead band ns, as "dead" */
#define AI64_FAB_ACT_SWEEP 3u /* arg0 ms, 0 to stop, as "sweep" */
#define AI64_FAB_ACT_STEPS 4u /* arg0 ms a step, 0 to stop, as "steps" */

/*
 * One message, 64 bytes. A ping is its first AI64_FAB_PING_WORDS words;
 * the pong echoes them, with t1 the responder's GTC when it read the ping
 * and value mixed with the responder's number. A tour or route uses them
 * all. It goes to route node 0 first, and its last route node is the
 * origin; each node on the way mixes its number into value, and writes
 * the GTC into the origin's trip stamps when it reads the message.
 */
struct ai64_fab_msg {
	uint32_t origin;
	uint32_t id;
	uint32_t t0;
	uint32_t t1;
	uint32_t value;
	uint32_t nhops;
	uint32_t hop;
	uint32_t action;
	uint32_t action_node;
	uint32_t arg0;
	uint32_t arg1;
	uint32_t route[4];     /* node of hop h in byte h % 4 of route[h / 4] */
	uint32_t status;       /* set by the action node, 0 when it acted */
};

#define AI64_FAB_PING_WORDS 5u
#define AI64_FAB_MSG_WORDS  (sizeof(struct ai64_fab_msg) / 4u)

/*
 * What a node knows of one peer from its own pings, 32 bytes. The counts
 * and sums only grow, from zero when the node starts, so Linux takes
 * differences. rtt_min and rtt_max are over the last AI64_FAB_WINDOW_TICKS.
 * oneway_sum adds t1 - t0, from the ping's send to the peer reading it.
 */
struct ai64_fab_peer {
	uint32_t pongs;
	uint32_t timeouts;
	uint32_t errors;
	uint32_t rtt_sum;
	uint32_t oneway_sum;
	uint32_t rtt_min;
	uint32_t rtt_max;
	uint32_t up;           /* 1 after a pong, 0 after a timeout */
};

/* The owner's record, 256 bytes. Everything after boot is zeroed at start. */
struct ai64_fab_own {
	uint32_t magic;
	uint32_t node;
	uint32_t boot;         /* how many times the node started */
	uint32_t beat;         /* GTC, about once a millisecond */
	uint32_t polls;
	uint32_t sends;
	uint32_t doorbells_full; /* sends that found the receiver's FIFO full */
	uint32_t misroutes;
	uint32_t tally_ok;
	uint32_t tally_busy;   /* gave up after AI64_FAB_LOCK_TRIES reads */
	uint32_t tally_tries_max;
	uint32_t forwarded;    /* tour and route hops it passed on */
	/* R5F0_0's tours */
	uint32_t tour_laps;
	uint32_t tour_errors;
	uint32_t tour_timeouts;
	uint32_t tour_sum;
	uint32_t tour_min;
	uint32_t tour_max;
	uint32_t tour_nhops;   /* the last lap: its route, start, and stamps */
	uint32_t tour_t0;
	uint32_t tour_route[4];
	uint32_t tour_stamp[AI64_FAB_MAX_HOPS];
	uint32_t pad[24];
};

struct ai64_fab_page {
	struct ai64_fab_own own;                         /* 0x000 */
	struct ai64_fab_peer peer[AI64_FAB_SLOTS];       /* 0x100 */
	/* The hops of this node's tour (TOUR) and route (ROUTE) write the GTC
	 * here when they read it: trip[cls - AI64_FAB_TOUR][hop]. */
	uint32_t trip[2][AI64_FAB_MAX_HOPS];             /* 0x280 */
	/* Whatever the node publishes for the others to read, such as a
	 * PWM generator's struct ai64_pwm_status (ai64_shm.h). */
	uint32_t app[64];                                /* 0x300 */
	/* The inbox. seq[s] and rseq[s] are written by sender s only:
	 * byte c of seq[s] counts its class c messages, rseq[s] its routes. */
	uint32_t seq[AI64_FAB_SLOTS];                    /* 0x400 */
	uint32_t rseq[AI64_FAB_SLOTS];                   /* 0x430 */
	uint32_t pad1[(0x800u - 0x460u) / 4u];
	struct ai64_fab_msg slot[AI64_FAB_CLASSES][AI64_FAB_SLOTS]; /* 0x800 */
};

/* The spinlock tally, after the pages. Only the lock holder writes it. */
struct ai64_fab_tally {
	uint32_t magic;
	uint32_t total;
	uint32_t holder;       /* node + 1 inside the lock, else 0 */
	uint32_t count[AI64_FAB_SLOTS];
};

/* What a node does to a tour's or a route's value. */
static inline uint32_t ai64_fab_mix(uint32_t v, uint32_t node)
{
	return ((v << 7) | (v >> 25)) ^ (0x6A09E667u ^ (node << 24) ^ (node << 12) ^ node);
}

/* A ping's value, and what its pong must bring back. */
static inline uint32_t ai64_fab_ping_value(uint32_t from, uint32_t to, uint32_t id)
{
	return (id << 8) ^ (from << 4) ^ to ^ 0x5DEECE66u;
}

static inline uint32_t ai64_fab_route_node(const uint32_t *route, uint32_t hop)
{
	return (route[hop >> 2] >> (8u * (hop & 3u))) & 0xffu;
}

#ifdef AI64_FAB_NOW
/*
 * The node side, one copy per image. Before including this, an image defines:
 *   AI64_FAB_NOW()          the GTC's low word
 *   AI64_FAB_BASE           the fabric's address, as an integer
 *   AI64_FAB_MBOX           mailbox cluster 11's address, as an integer
 *   AI64_FAB_LOCKREG        the lock register's address, as an integer
 * and may define:
 *   AI64_FAB_FENCE()        a full memory barrier
 *   AI64_FAB_COPY_IN(d, s, n)  copy n words of a message out of DDR, and
 *   AI64_FAB_COPY_OUT(d, s, n) into it. The defaults move a word at a time;
 *                           a core that maps the fabric as normal memory can
 *                           use memcpy, for bursts. The RTUs use bursts too.
 *   AI64_FAB_ACTION(m)      run a route's action here; returns its status
 *   AI64_FAB_LOCK_ENTER(), AI64_FAB_LOCK_EXIT()
 *                           around the time the node holds the spinlock, so
 *                           that nothing else on the core (Zephyr's shutdown
 *                           handler, say) stops it with the lock taken
 *   AI64_FAB_TOURS          start tours from this node (R5F0_0 does)
 * Then it calls fab_init(node) once and fab_poll() from its main loop.
 */
#ifndef AI64_FAB_FENCE
#define AI64_FAB_FENCE()
#endif
#ifndef AI64_FAB_LOCK_ENTER
#define AI64_FAB_LOCK_ENTER()
#define AI64_FAB_LOCK_EXIT()
#endif

#define FAB_PAGE(n)  ((volatile struct ai64_fab_page *)(uintptr_t)(AI64_FAB_BASE + (n) * AI64_FAB_PAGE))
#define FAB_TALLY    ((volatile struct ai64_fab_tally *)(uintptr_t)(AI64_FAB_BASE + AI64_FAB_TALLY_OFF))
#define FAB_MBOX(o)  (*(volatile uint32_t *)(uintptr_t)(AI64_FAB_MBOX + (o)))
#define FAB_MBOX_MSG(f)    FAB_MBOX(0x40u + 4u * (f))
#define FAB_MBOX_STATUS(f) FAB_MBOX(0xc0u + 4u * (f))
#define FAB_LOCK     (*(volatile uint32_t *)(uintptr_t)(AI64_FAB_LOCKREG))
#define FAB_AFTER(now, t) ((int32_t)((now) - (t)) >= 0)

#ifdef __PRU__
/*
 * A struct copy is one LBBO or SBBO burst of up to 44 bytes, where a
 * volatile loop is a bus access a word. Not inlined, so the compiler cannot
 * drop or move the accesses to memory it does not know is shared.
 */
struct fab_ping_words { uint32_t w[AI64_FAB_PING_WORDS]; };
#pragma FUNC_CANNOT_INLINE(fab_burst_ping)
static void fab_burst_ping(void *d, const void *s)
{
	*(struct fab_ping_words *)d = *(const struct fab_ping_words *)s;
}
#pragma FUNC_CANNOT_INLINE(fab_burst_msg)
static void fab_burst_msg(void *d, const void *s)
{
	*(struct ai64_fab_msg *)d = *(const struct ai64_fab_msg *)s;
}
#define FAB_BURST(d, s, n) ((n) == AI64_FAB_PING_WORDS ? fab_burst_ping((void *)(d), (const void *)(s)) \
							: fab_burst_msg((void *)(d), (const void *)(s)))
#define AI64_FAB_COPY_IN(d, s, n)  FAB_BURST(d, s, n)
#define AI64_FAB_COPY_OUT(d, s, n) FAB_BURST(d, s, n)
#endif

#ifndef AI64_FAB_COPY_IN
#define AI64_FAB_COPY_IN(d, s, n) fab_copy_in((d), (s), (n))
static void fab_copy_in(struct ai64_fab_msg *d, volatile const struct ai64_fab_msg *s, uint32_t n)
{
	uint32_t *dw = (uint32_t *)d;
	volatile const uint32_t *sw = (volatile const uint32_t *)s;
	uint32_t i;

	for (i = 0; i < n; i++) {
		dw[i] = sw[i];
	}
}
#endif

#ifndef AI64_FAB_COPY_OUT
#define AI64_FAB_COPY_OUT(d, s, n) fab_copy_out((d), (s), (n))
static void fab_copy_out(volatile struct ai64_fab_msg *d, const struct ai64_fab_msg *s, uint32_t n)
{
	volatile uint32_t *dw = (volatile uint32_t *)d;
	const uint32_t *sw = (const uint32_t *)s;
	uint32_t i;

	for (i = 0; i < n; i++) {
		dw[i] = sw[i];
	}
}
#endif

#ifdef __PRU__
/* A PRU image's linker script places this, where its data RAM has room. */
#pragma DATA_SECTION(fab, ".ai64_fab")
#endif
static struct {
	uint32_t me;
	uint32_t seen[AI64_FAB_NODES];   /* each sender's seq, as last read */
	uint32_t rseen[AI64_FAB_NODES];
	uint32_t sent[AI64_FAB_NODES];   /* my seq word in each peer's inbox */
	uint32_t ping_id[AI64_FAB_NODES];
	uint32_t ping_at[AI64_FAB_NODES]; /* in flight: sent then; else: next ping */
	uint32_t rtt_min[AI64_FAB_NODES];
	uint32_t rtt_max[AI64_FAB_NODES];
	uint32_t in_flight;               /* a bit per peer */
	uint32_t next_peer;
	uint32_t polls, sends;
	uint32_t window_at, beat_at, scan_at, tally_at;
#ifdef AI64_FAB_TOURS
	uint32_t tour_busy, tour_id, tour_t0, tour_at, tour_want;
	uint32_t tour_min, tour_max;
#endif
	struct ai64_fab_msg msg;
} fab;

static void fab_send(uint32_t to, uint32_t cls, const struct ai64_fab_msg *m, uint32_t words)
{
	volatile struct ai64_fab_page *p = FAB_PAGE(to);
	uint32_t i, n;

	AI64_FAB_COPY_OUT(&p->slot[cls][fab.me], m, words);
	AI64_FAB_FENCE();
	if (cls == AI64_FAB_ROUTE) {
		/* On Linux, ai64 fabric route also sends as node 0, so count
		 * from what is there. */
		p->rseq[fab.me] = p->rseq[fab.me] + 1u;
	} else {
		n = fab.sent[to];
		i = ((n >> (8u * cls)) + 1u) & 0xffu;
		n = (n & ~(0xffu << (8u * cls))) | (i << (8u * cls));
		fab.sent[to] = n;
		p->seq[fab.me] = n;
	}
	fab.sends++;
	AI64_FAB_FENCE();
	/* A full FIFO already has a doorbell waiting, and the receiver scans
	 * its whole inbox every AI64_FAB_SCAN_TICKS besides. */
	if ((FAB_MBOX_STATUS(to) & 0x3fu) < AI64_FAB_MBOX_DEPTH) {
		FAB_MBOX_MSG(to) = fab.me;
	} else {
		FAB_PAGE(fab.me)->own.doorbells_full++;
	}
}

static void fab_hop(uint32_t cls, uint32_t now)
{
	struct ai64_fab_msg *m = &fab.msg;
	volatile struct ai64_fab_page *mine = FAB_PAGE(fab.me);
	volatile struct ai64_fab_own *own = &mine->own;
	uint32_t hop = m->hop;

	/* Never write outside the pages, whatever a message says. */
	if (hop >= m->nhops || m->nhops > AI64_FAB_MAX_HOPS || m->origin >= AI64_FAB_NODES ||
	    ai64_fab_route_node(m->route, hop) != fab.me ||
	    (hop + 1u < m->nhops && ai64_fab_route_node(m->route, hop + 1u) >= AI64_FAB_NODES)) {
		own->misroutes++;
		return;
	}
	FAB_PAGE(m->origin)->trip[cls - AI64_FAB_TOUR][hop] = now;
	m->value = ai64_fab_mix(m->value, fab.me);
#ifdef AI64_FAB_ACTION
	if (m->action != AI64_FAB_ACT_NONE && m->action_node == fab.me) {
		m->status = AI64_FAB_ACTION(m);
	}
#endif
	if (hop + 1u < m->nhops) {
		m->hop = hop + 1u;
		own->forwarded++;
		fab_send(ai64_fab_route_node(m->route, hop + 1u), cls, m, AI64_FAB_MSG_WORDS);
		return;
	}
	/* Home: this node is the origin. A route's origin is Linux, which
	 * reads the stamps itself. */
#ifdef AI64_FAB_TOURS
	if (cls == AI64_FAB_TOUR && fab.tour_busy && m->id == fab.tour_id) {
		uint32_t lap = now - m->t0, i;

		fab.tour_busy = 0;
		if (m->value != fab.tour_want) {
			own->tour_errors++;
			return;
		}
		own->tour_laps++;
		own->tour_sum += lap;
		if (lap < fab.tour_min) {
			fab.tour_min = lap;
		}
		if (lap > fab.tour_max) {
			fab.tour_max = lap;
		}
		own->tour_nhops = 0;
		own->tour_t0 = m->t0;
		for (i = 0; i < 4u; i++) {
			own->tour_route[i] = m->route[i];
		}
		for (i = 0; i + 1u < m->nhops; i++) {
			own->tour_stamp[i] = mine->trip[0][i];
		}
		own->tour_stamp[i] = now;
		own->tour_nhops = m->nhops;
	}
#endif
}

static void fab_pong(uint32_t from, uint32_t now)
{
	struct ai64_fab_msg *m = &fab.msg;
	volatile struct ai64_fab_peer *p = &FAB_PAGE(fab.me)->peer[from];
	uint32_t rtt;

	if (!(fab.in_flight & (1u << from)) || m->id != fab.ping_id[from] ||
	    m->origin != fab.me) {
		return; /* one that timed out, or from before this node started */
	}
	fab.in_flight &= ~(1u << from);
	fab.ping_at[from] = fab.ping_at[from] + AI64_FAB_PING_TICKS;
	if (m->value != ai64_fab_mix(ai64_fab_ping_value(fab.me, from, m->id), from)) {
		p->errors++;
		return;
	}
	rtt = now - m->t0;
	p->pongs++;
	p->rtt_sum += rtt;
	p->oneway_sum += m->t1 - m->t0;
	p->up = 1;
	if (rtt < fab.rtt_min[from]) {
		fab.rtt_min[from] = rtt;
	}
	if (rtt > fab.rtt_max[from]) {
		fab.rtt_max[from] = rtt;
	}
}

/*
 * Everything sender `from` has sent since the last look. Each message is
 * stamped when it has been read, so a latency includes reading it.
 */
static void fab_read(uint32_t from)
{
	volatile struct ai64_fab_page *me = FAB_PAGE(fab.me);
	uint32_t seq = me->seq[from];
	uint32_t rseq = me->rseq[from];
	uint32_t changed = seq ^ fab.seen[from];

	if (changed) {
		AI64_FAB_FENCE();
		fab.seen[from] = seq;
		if (changed & 0xffu) {
			AI64_FAB_COPY_IN(&fab.msg, &me->slot[AI64_FAB_REQ][from], AI64_FAB_PING_WORDS);
			fab.msg.t1 = AI64_FAB_NOW();
			fab.msg.value = ai64_fab_mix(fab.msg.value, fab.me);
			fab_send(from, AI64_FAB_REPLY, &fab.msg, AI64_FAB_PING_WORDS);
		}
		if (changed & 0xff00u) {
			AI64_FAB_COPY_IN(&fab.msg, &me->slot[AI64_FAB_REPLY][from], AI64_FAB_PING_WORDS);
			fab_pong(from, AI64_FAB_NOW());
		}
		if (changed & 0xff0000u) {
			AI64_FAB_COPY_IN(&fab.msg, &me->slot[AI64_FAB_TOUR][from], AI64_FAB_MSG_WORDS);
			fab_hop(AI64_FAB_TOUR, AI64_FAB_NOW());
		}
	}
	if (rseq != fab.rseen[from]) {
		AI64_FAB_FENCE();
		fab.rseen[from] = rseq;
		AI64_FAB_COPY_IN(&fab.msg, &me->slot[AI64_FAB_ROUTE][from], AI64_FAB_MSG_WORDS);
		fab_hop(AI64_FAB_ROUTE, AI64_FAB_NOW());
	}
}

static void fab_ping(uint32_t to, uint32_t now)
{
	struct ai64_fab_msg *m = &fab.msg;
	volatile struct ai64_fab_peer *p = &FAB_PAGE(fab.me)->peer[to];

	if (fab.in_flight & (1u << to)) {
		if (FAB_AFTER(now, fab.ping_at[to] + AI64_FAB_TIMEOUT_TICKS)) {
			fab.in_flight &= ~(1u << to);
			fab.ping_at[to] = now + AI64_FAB_RETRY_TICKS;
			p->timeouts++;
			p->up = 0;
		}
		return;
	}
	if (!FAB_AFTER(now, fab.ping_at[to])) {
		return;
	}
	/* Late by more than a period: start the schedule again from now. */
	if (FAB_AFTER(now, fab.ping_at[to] + AI64_FAB_PING_TICKS)) {
		fab.ping_at[to] = now;
	}
	m->origin = fab.me;
	m->id = ++fab.ping_id[to];
	m->t0 = now;
	m->t1 = 0;
	m->value = ai64_fab_ping_value(fab.me, to, m->id);
	fab.in_flight |= 1u << to;
	fab_send(to, AI64_FAB_REQ, m, AI64_FAB_PING_WORDS);
}

#ifdef AI64_FAB_TOURS
/* Every node that answers this one's pings, in an order that keeps
 * crossing between kinds of core, then home. */
static const uint8_t fab_tour_order[] = {
	AI64_FAB_C66X0, AI64_FAB_G0RTU0, AI64_FAB_R5F1_0, AI64_FAB_C7X,
	AI64_FAB_R5F0_1, AI64_FAB_C66X1, AI64_FAB_G0RTU1, AI64_FAB_R5F1_1,
	AI64_FAB_LINUX,
};

static void fab_tour(uint32_t now)
{
	struct ai64_fab_msg *m = &fab.msg;
	volatile struct ai64_fab_page *mine = FAB_PAGE(fab.me);
	uint32_t i, n = 0, v;

	if (fab.tour_busy) {
		if (FAB_AFTER(now, fab.tour_t0 + AI64_FAB_TOUR_TIMEOUT)) {
			fab.tour_busy = 0;
			mine->own.tour_timeouts++;
		}
		return;
	}
	if (!FAB_AFTER(now, fab.tour_at)) {
		return;
	}
	fab.tour_at = now + AI64_FAB_TOUR_TICKS;
	for (i = 0; i < 4u; i++) {
		m->route[i] = 0;
	}
	for (i = 0; i < sizeof(fab_tour_order); i++) {
		if (mine->peer[fab_tour_order[i]].up) {
			m->route[n >> 2] |= (uint32_t)fab_tour_order[i] << (8u * (n & 3u));
			n++;
		}
	}
	if (n == 0) {
		return;
	}
	m->route[n >> 2] |= fab.me << (8u * (n & 3u));
	n++;
	/* The lap starts now, not when the poll began. */
	now = AI64_FAB_NOW();
	m->origin = fab.me;
	m->id = ++fab.tour_id;
	m->t0 = now;
	m->t1 = 0;
	m->value = now;
	m->nhops = n;
	m->hop = 0;
	m->action = AI64_FAB_ACT_NONE;
	m->action_node = 0;
	m->arg0 = 0;
	m->arg1 = 0;
	m->status = 0;
	v = now;
	for (i = 0; i < n; i++) {
		v = ai64_fab_mix(v, ai64_fab_route_node(m->route, i));
	}
	fab.tour_want = v;
	fab.tour_t0 = now;
	fab.tour_busy = 1;
	fab_send(ai64_fab_route_node(m->route, 0), AI64_FAB_TOUR, m, AI64_FAB_MSG_WORDS);
}
#endif

static void fab_tally(void)
{
	volatile struct ai64_fab_tally *t = FAB_TALLY;
	volatile struct ai64_fab_own *own = &FAB_PAGE(fab.me)->own;
	uint32_t tries = 0;

	AI64_FAB_LOCK_ENTER();
	while (FAB_LOCK != 0u) {
		if (++tries == AI64_FAB_LOCK_TRIES) {
			AI64_FAB_LOCK_EXIT();
			own->tally_busy++;
			return;
		}
	}
	t->holder = fab.me + 1u;
	t->total = t->total + 1u;
	t->count[fab.me] = t->count[fab.me] + 1u;
	t->holder = 0;
	AI64_FAB_FENCE();
	FAB_LOCK = 0;
	AI64_FAB_LOCK_EXIT();
	own->tally_ok++;
	if (tries > own->tally_tries_max) {
		own->tally_tries_max = tries;
	}
}

static void fab_init(uint32_t me)
{
	volatile struct ai64_fab_page *mine = FAB_PAGE(me);
	volatile uint32_t *w;
	uint32_t i, now = AI64_FAB_NOW();

	fab.me = me;
	/* Drop the doorbells and messages that came while this node was down. */
	while (FAB_MBOX_STATUS(me) & 0x3fu) {
		(void)FAB_MBOX_MSG(me);
	}
	for (i = 0; i < AI64_FAB_NODES; i++) {
		fab.seen[i] = mine->seq[i];
		fab.rseen[i] = mine->rseq[i];
		fab.sent[i] = FAB_PAGE(i)->seq[me];
		fab.ping_id[i] = 0;
		/* Spread the first pings over a period. */
		fab.ping_at[i] = now + i * (AI64_FAB_PING_TICKS / AI64_FAB_NODES);
		fab.rtt_min[i] = 0xffffffffu;
		fab.rtt_max[i] = 0;
	}
	/* Start the record over: magic first, so Linux skips it meanwhile. */
	mine->own.boot = mine->own.magic == AI64_FAB_MAGIC && mine->own.node == me ?
			 mine->own.boot + 1u : 1u;
	mine->own.magic = 0;
	AI64_FAB_FENCE();
	w = (volatile uint32_t *)&mine->own;
	for (i = 3; i < sizeof(mine->own) / 4u; i++) {
		w[i] = 0;
	}
	w = (volatile uint32_t *)mine->peer;
	for (i = 0; i < sizeof(mine->peer) / 4u; i++) {
		w[i] = 0;
	}
	fab.in_flight = 0;
	fab.next_peer = 0;
	fab.polls = 0;
	fab.sends = 0;
	fab.window_at = now + AI64_FAB_WINDOW_TICKS;
	fab.beat_at = now;
	fab.scan_at = now;
	fab.tally_at = now;
#ifdef AI64_FAB_TOURS
	fab.tour_busy = 0;
	fab.tour_at = now + AI64_FAB_WINDOW_TICKS;
	fab.tour_min = 0xffffffffu;
	fab.tour_max = 0;
#endif
	mine->own.node = me;
	mine->own.beat = now;
	AI64_FAB_FENCE();
	mine->own.magic = AI64_FAB_MAGIC;
}

static void fab_poll(void)
{
	volatile struct ai64_fab_page *mine = FAB_PAGE(fab.me);
	uint32_t now, n, i;

	/* Doorbells name their sender. */
	n = FAB_MBOX_STATUS(fab.me) & 0x3fu;
	while (n--) {
		i = FAB_MBOX_MSG(fab.me);
		if (i < AI64_FAB_NODES) {
			fab_read(i);
		}
	}
	now = AI64_FAB_NOW();
	if (FAB_AFTER(now, fab.scan_at)) {
		fab.scan_at = now + AI64_FAB_SCAN_TICKS;
		for (i = 0; i < AI64_FAB_NODES; i++) {
			if (i != fab.me) {
				fab_read(i);
			}
		}
	}

	/* One peer's ping a pass. */
	i = fab.next_peer;
	fab.next_peer = i + 1u == AI64_FAB_NODES ? 0 : i + 1u;
	if (i != fab.me) {
		fab_ping(i, now);
	}
#ifdef AI64_FAB_TOURS
	fab_tour(now);
#endif
	if (FAB_AFTER(now, fab.tally_at)) {
		fab.tally_at = now + AI64_FAB_TALLY_TICKS;
		fab_tally();
	}
	fab.polls++;
	if (FAB_AFTER(now, fab.beat_at)) {
		fab.beat_at = now + AI64_FAB_US(1000);
		mine->own.beat = now;
		mine->own.polls = fab.polls;
		mine->own.sends = fab.sends;
	}
	if (FAB_AFTER(now, fab.window_at)) {
		fab.window_at = now + AI64_FAB_WINDOW_TICKS;
		for (i = 0; i < AI64_FAB_NODES; i++) {
			mine->peer[i].rtt_min = fab.rtt_min[i] == 0xffffffffu ? 0 : fab.rtt_min[i];
			mine->peer[i].rtt_max = fab.rtt_max[i];
			fab.rtt_min[i] = 0xffffffffu;
			fab.rtt_max[i] = 0;
		}
#ifdef AI64_FAB_TOURS
		mine->own.tour_min = fab.tour_min == 0xffffffffu ? 0 : fab.tour_min;
		mine->own.tour_max = fab.tour_max;
		fab.tour_min = 0xffffffffu;
		fab.tour_max = 0;
#endif
	}
}
#endif /* AI64_FAB_NOW */

#endif
