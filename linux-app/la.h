/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's channels and its stream reader (la_stream.c), for
 * ai64 la (la.c) and the web page's stream (la_ws.c).
 */
#ifndef LA_H_
#define LA_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ai64_la.h"

struct la_chan {
	const char *pin;
	const char *ball;
	uint32_t pad;
	int pru;
	int bit;
};

/* What the demo's jumpers bring to a channel, if anything. */
struct la_wired {
	const char *name;
	const char *from;
};

extern const struct la_chan la_chans[AI64_LA_NCHAN];
extern const struct la_wired la_wired[AI64_LA_NCHAN];
extern volatile struct ai64_la *la_hdr;

/* Bytes a reader copies out of a ring at a time. */
#define CHUNK 65536u

struct stream {
	int s;
	volatile struct ai64_la_stream *h;
	const volatile uint32_t *ring;
	uint32_t rd;        /* bytes read, free-running, as the RTU's wr */
	int ticked;         /* seen a tick since starting or skipping */
	uint64_t base;      /* IEP0, ns, at the newest tick */
	uint64_t first;     /* and at the first */
	uint32_t first_state;
	uint32_t state;
	uint64_t edges;
	uint64_t gaps;      /* gap and hole records: the RTU or raw mode's converter fell behind */
	uint64_t missed;    /* records this reader fell a ring behind on */
	uint64_t late;      /* edges with AI64_LA_LATE: edge mode overloaded */
	uint64_t last;      /* the time of the newest edge or tick */
	uint64_t hole_from; /* with hole, where the open hole starts */
	int hole;           /* a hole is open: the pins are unknown until the next record */
	uint64_t marks;
	uint64_t trigs;
	uint32_t wr0;       /* wr when the reader started */
	uint32_t *chunk;    /* the part of the ring being decoded (la_stream.c) */
	struct mark *mark;  /* the marks and hits, if keep_marks */
	size_t nmark, capmark;
	int keep_marks;
	/* If set, each hole goes here, and not as an edge (la_stream.c). */
	void (*on_hole)(void *ctx, struct stream *st, uint64_t from, uint64_t to, uint32_t state);
};

typedef void (*edge_fn)(void *ctx, struct stream *st, uint64_t t, uint32_t state);

/*
 * A mark or a trigger hit (ai64_la.h): when; for a mark, which fabric node
 * sent it and its value; for a hit, the PRU's pins after the edge. fresh is
 * whether it came after the reader started (not from the ring's history).
 */
struct mark {
	uint64_t t;
	uint32_t trig;
	uint32_t from;
	uint32_t value;
	uint32_t fresh;
};

/* Maps the headers and rings once; whether both RTUs run the analyzer. */
int la_map(int verbose);
/* The headers, writable, for ai64 la trigger; NULL if the analyzer does not run. */
volatile struct ai64_la *la_map_trigger(void);
int stream_init(struct stream *st, int s);
void stream_free(struct stream *st);
void stream_poll(struct stream *st, edge_fn fn, void *ctx);
/* Until both streams have ticked; -1 if one does not within 2 s. */
int wait_ticks(struct stream st[2], edge_fn fn, void *ctx);

/* Channel c's level, from both PRUs' states. */
static inline int level(const uint32_t state[2], int c)
{
	return (int)(state[la_chans[c].pru] >> la_chans[c].bit & 1u);
}


struct event {
	uint64_t t;
	uint32_t state;
};

struct events {
	struct event *e;
	size_t n, cap;
};

/* A PRU's holes, in time order: from a[i] to b[i] its pins are unknown. */
struct holes {
	uint64_t *a, *b;
	size_t n, cap;
};

/* Whether a hole overlaps [from, to): one ending at from does, one starting at to does not. */
int holes_cross(const struct holes *h, uint64_t from, uint64_t to);

/*
 * A recording: both streams' edges over a span both cover, from the later
 * first tick to the earlier last one, and every pin's state at its start.
 * rec_next() hands the edges back merged by time. Each hole's end is an
 * event too, with the pins there, which may be no change.
 */
struct recording {
	struct stream st[2];
	struct events ev[2];
	struct holes holes[2];
	uint32_t state[2];
	uint64_t begin, finish;
	uint64_t hit;       /* the trigger hit it was taken around, or 0 */
	size_t i[2];
};

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
 * nearest change, leaving out any across a hole of a's PRU (ha) or b's (hb).
 */
size_t nearest(const struct changes *a, const struct changes *b, int up, int64_t *out,
	       const struct holes *ha, const struct holes *hb);
void rec_changes(struct recording *r, struct changes ch[AI64_LA_NCHAN]);
void changes_free(struct changes ch[AI64_LA_NCHAN]);
void spread_of(int64_t *v, size_t n, struct spread *sp);
/* Its periods and high times leave out any across one of holes, its PRU's. */
void chan_stats(const struct changes *c, int level0, uint64_t begin, uint64_t finish, int64_t *v,
		struct chan_stats *out, const struct holes *holes);
/* The recording as a VCD file (la.c); each PRU's edges in nedge, and the marks and hits. */
size_t rec_write_vcd(struct recording *r, FILE *f, size_t nedge[2]);
/*
 * The analyzer's mode: raw -1 for edge mode on both PRUs, each recording
 * the pins in its mask; or raw 0 or 1, that PRU sampling the pins in its
 * mask every 8 ns, and the other converting them, recording none of its
 * own (pru/logic_analyzer/firmware/la_map.h). Passes the modes to the RTUs
 * and restarts both PRUs, a gap of some 50 ms in the streams. 0, or -1
 * with why in err. Needs root.
 */
int la_set_mode(int raw, const uint32_t mask[2], char *err, size_t errlen);
/* Raw mode's lane for a PRU's pins (AI64_LA_LANE_*): 16 of them, if they fit. */
uint32_t la_lane(uint32_t mask);
/* The trigger, on PRU pru (the other's goes off); -1 if the analyzer does not run. */
int la_set_trigger(int pru, uint32_t mode, uint32_t mask, uint32_t value);

/* secs of both streams from now; or, with trigger, around the next hit. */
int record(struct recording *r, double secs, int trigger);
/*
 * The next event in time, up to finish: its time, and the state before it
 * in was; 1 + its PRU, or 0 at the end.
 */
int rec_next(struct recording *r, uint64_t *t, uint32_t was[2]);
/* How many of a PRU's holes overlap [begin, finish], and how long they are in it. */
size_t rec_holes(const struct recording *r, int s, uint64_t *ns);
void rec_free(struct recording *r);
/* Both streams' marks and hits in [begin, finish], merged by time, into *out. */
size_t rec_marks(struct recording *r, struct mark **out);

#endif
