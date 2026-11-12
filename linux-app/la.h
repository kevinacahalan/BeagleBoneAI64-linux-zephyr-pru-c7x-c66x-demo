/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer, as Linux sees it: its channels and setup, the reader
 * that follows PRU0's ring and finds the changes (la_src.c), and
 * recordings and measurements (la_rec.c), for ai64 la (la.c), the web
 * page's stream (la_ws.c), and the MCP server (mcp.c).
 *
 * A state is the channels' levels, bit c channel c's.
 */
#ifndef LA_H_
#define LA_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ai64_la.h"

#define ICSSG1_DRAM0 0x0b100000u
#define ICSSG1_IEP0  0x0b12e000u
#define ICSSG1_TM    0x0b12a000u

struct la_chan {
	const char *pin;
	const char *ball;
	uint32_t pad;
	int bit;            /* PRU0's GPI bit */
};

/* What the demo's jumpers bring to a channel, if anything. */
struct la_wired {
	const char *name;
	const char *from;
};

extern const struct la_chan la_chans[AI64_LA_NCHAN];
extern const struct la_wired la_wired[AI64_LA_NCHAN];

/* Channel c's level in a state. */
static inline int level(uint32_t state, int c)
{
	return (int)(state >> c & 1u);
}

/* ---- the setup (la_src.c) ---- */

/* The rates, fastest first: samples a second, and PRU cycles a sample. */
struct la_rate {
	uint32_t hz;
	uint32_t period;
};
extern const struct la_rate la_rates[];
extern const size_t la_nrates;

/*
 * The bytes a second the reader here takes out of the ring and keeps up
 * with (la_src.c): through /dev/mem, uncached, about 170 MB/s while PRU0
 * writes; through /dev/ai64-la, cached, about 350 MB/s, where finding
 * the changes takes most of a core. A setup that writes more
 * keeps every sample in PRU0 and the ring, but a live reader falls behind,
 * which is a hole. la_live_bps() is the one that applies here.
 */
#define LA_LIVE_BPS        150000000u
#define LA_LIVE_BPS_CACHED 300000000u
uint32_t la_live_bps(void);

/* What PRU0 runs: the channels (bit c channel c) and the rate, and what follows from them. */
struct la_mode {
	uint32_t chans;
	uint32_t hz;
	uint32_t gpi;       /* PRU0's GPI bits for the channels */
	uint32_t lane;      /* AI64_LA_LANE_*, which they fit at that rate */
	uint32_t period;    /* PRU cycles a sample */
	uint32_t key;       /* the run's first tag: a new run, a new key */
	uint64_t sample_ns; /* 4 ns a cycle */
	uint64_t block_ns;
	uint32_t spb;       /* samples a block */
	double bps;         /* bytes a second into the ring */
	double span;        /* seconds the ring holds */
	int live;           /* whether a live reader keeps up: bps <= la_live_bps() */
};

/* hz: one of la_rates; chans: at least one. Fills in the rest; -1 if not a setup. */
int la_mode_of(struct la_mode *m, uint32_t chans, uint32_t hz);
/* What runs now; -1 with why in err, if nothing does. Needs root. */
int la_get_mode(struct la_mode *m, char *err, size_t errlen);
/*
 * Sets the channels and rate: stops PRU0, writes the setup, starts it
 * again, a gap of some 30 ms in the stream. -1 with why in err. Needs root.
 */
int la_set_mode(uint32_t chans, uint32_t hz, char *err, size_t errlen);
/* "125", "62.5M", "25MS/s", "1.25m", "2500k": one of la_rates, into *hz; -1 if not one. */
int la_parse_rate(const char *s, uint32_t *hz);
/* "125 MS/s", "62.5 MS/s", "2.5 MS/s" into buf. */
const char *la_rate_name(uint32_t hz, char *buf, size_t len);
/* IEP0's count, ns. */
uint64_t la_now(void);

/* ---- the reader (la_src.c) ---- */

typedef void (*la_edge_fn)(void *ctx, uint64_t t, uint32_t state);
/* A hole: from from to to the levels are unknown; state is the levels at to. */
typedef void (*la_hole_fn)(void *ctx, uint64_t from, uint64_t to, uint32_t state);

/*
 * Following PRU0's ring: a pump thread copies each block, as it is due,
 * out of the ring (uncached) into a copy here, and la_src_poll() finds the
 * changes in that copy. A block PRU0 dropped, or one this reader fell a
 * ring behind on, is a hole.
 */
struct la_src {
	int open;
	struct la_mode mode;
	uint64_t t0;        /* IEP0's count, ns, at block 0's first sample */
	uint64_t start;     /* the first block taken */
	uint64_t next;      /* the next block to decode */
	uint64_t pumped;    /* blocks the pump has copied, from block 0 */
	int have;           /* a sample seen: cur and last are set */
	uint32_t cur;       /* the GPI bits at the newest sample */
	uint64_t last;      /* the time of the newest sample */
	uint64_t now;       /* the time decoded up to */
	uint64_t blocks, lost, edges, behind;
	uint16_t lut[3][256]; /* GPI bits, a byte at a time, to channel bits */
	uint32_t *copy;     /* the ring as the pump copied it */
	volatile int stop;
	pthread_t pump;
};

/*
 * From back seconds ago, as far as the ring holds, or now. The first call
 * to fn is the levels at the first sample, then one at each change. -1
 * with why in err. Needs root.
 */
int la_src_open(struct la_src *s, double back, char *err, size_t errlen);
/* Whether PRU0 has started a new run since: s is done; open another. */
int la_src_stale(const struct la_src *s);
void la_src_close(struct la_src *s);
/* The blocks since the last call, at most max_ns of the signal, as edges and holes. */
void la_src_poll(struct la_src *s, la_edge_fn fn, la_hole_fn hfn, void *ctx, uint64_t max_ns);
/*
 * For a setup the reader cannot follow live: stops PRU0, so the ring holds
 * still (stopped is IEP0's count after), and la_src_reread() can take a
 * span of it again, whole, if the ring still holds it, from s's first
 * block on; then la_set_mode() starts PRU0 again. -1 if not.
 */
int la_src_freeze(uint64_t *stopped, char *err, size_t errlen);
int la_src_reread(struct la_src *s, uint64_t stopped, uint64_t from, uint64_t to, la_edge_fn fn,
		  la_hole_fn hfn, void *ctx);

/* ---- recordings (la_rec.c) ---- */

struct event {
	uint64_t t;
	uint32_t state;
};

struct events {
	struct event *e;
	size_t n, cap;
};

/* Holes, in time order: from a[i] to b[i] the levels are unknown. */
struct holes {
	uint64_t *a, *b;
	size_t n, cap;
};

/* Whether a hole overlaps [from, to): one ending at from does, one starting at to does not. */
int holes_cross(const struct holes *h, uint64_t from, uint64_t to);

/*
 * A trigger: the levels of the channels in mask come to value, from any
 * other (ENTER); or any of them changes (ANY).
 */
#define LA_TRIG_ENTER 1
#define LA_TRIG_ANY   2
struct la_trig {
	int kind;           /* 0 for none */
	uint32_t mask, value;
};

/*
 * A recording: the edges over [begin, finish], and the levels at begin.
 * rec_next() hands them back in order. Each hole's end is an event too,
 * with the levels there, which may be no change.
 */
struct recording {
	struct events ev;
	struct holes holes;
	struct la_mode mode;
	uint32_t state0;    /* the levels at begin */
	uint64_t begin, finish;
	uint64_t hit;       /* the trigger hit it was taken around, or 0 */
	uint64_t blocks, lost;
	/* reading it back */
	size_t i;
	uint32_t state;
};

/*
 * secs from now; or, with trig, around its next hit: pre seconds before
 * it, from the ring, and secs after; giving up after wait seconds. -1 with
 * why in err.
 */
int record(struct recording *r, double secs, const struct la_trig *trig, double pre, double wait,
	   char *err, size_t errlen);
/* The next event up to finish: its time, the levels before it in was; 0 at the end. */
int rec_next(struct recording *r, uint64_t *t, uint32_t *was);
/* From begin again. */
void rec_rewind(struct recording *r);
/* How many holes overlap [begin, finish], and how long they are in it. */
size_t rec_holes(const struct recording *r, uint64_t *ns);
void rec_free(struct recording *r);
/* The recording as a VCD file (la.c); its edges. */
size_t rec_write_vcd(struct recording *r, FILE *f);

/* One channel's changes: times, and the level after each. */
struct changes {
	uint64_t *t;
	uint8_t *up;
	size_t n, cap;
};

struct spread {
	size_t n;
	int64_t min, med, max;
};

struct chan_stats {
	size_t edges, rises;
	uint64_t high_ns;   /* of known_ns, the time not in a hole */
	uint64_t known_ns;
	int level_end;
	struct spread period, high;
};

void changes_add(struct changes *ch, uint64_t t, int up);
int cmp_i64(const void *a, const void *b);
/*
 * For each change of a that goes up (or down), the signed time to b's
 * nearest change, leaving out any across a hole.
 */
size_t nearest(const struct changes *a, const struct changes *b, int up, int64_t *out,
	       const struct holes *h);
/* Each channel's changes over the recording, in ch; the recording can be read again after. */
void rec_changes(struct recording *r, struct changes ch[AI64_LA_NCHAN]);
void changes_free(struct changes ch[AI64_LA_NCHAN]);
void spread_of(int64_t *v, size_t n, struct spread *sp);
/* Its periods and high times leave out any across a hole. */
void chan_stats(const struct changes *c, int level0, uint64_t begin, uint64_t finish, int64_t *v,
		struct chan_stats *out, const struct holes *holes);

#endif
