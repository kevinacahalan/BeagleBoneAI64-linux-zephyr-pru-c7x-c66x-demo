/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's channels and its stream reader (la_stream.c), for
 * ai64 la (la.c) and the web page's stream (la_ws.c).
 */
#ifndef LA_H_
#define LA_H_

#include <stddef.h>
#include <stdint.h>

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
	uint64_t gaps;      /* gap records: the RTU fell behind the PRU */
	uint64_t missed;    /* records this reader fell a ring behind on */
	uint64_t marks;
	uint64_t trigs;
	uint32_t wr0;       /* wr when the reader started */
	uint32_t *chunk;    /* the part of the ring being decoded (la_stream.c) */
	struct mark *mark;  /* the marks and hits, if keep_marks */
	size_t nmark, capmark;
	int keep_marks;
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

#endif
