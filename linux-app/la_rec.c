/* SPDX-License-Identifier: MIT */
/*
 * Recordings (la.h): ai64 la capture and measure (la.c), and the MCP
 * server's tools (mcp.c), take one with record() and read it back with
 * rec_next(). The trigger is matched here, on the changes as the reader
 * finds them: the ring's history gives the time before the hit.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

/* While it waits for a hit, a recording keeps only pre seconds, and this many edges more. */
#define KEEP_MIN 65536u

struct rec_ctx {
	struct recording *r;
	const struct la_trig *trig;
	uint64_t armed;     /* hits from then on */
	uint32_t was;
	int have;
};

static void *grow(void *p, size_t *cap, size_t size, size_t first)
{
	*cap = *cap ? *cap * 2 : first;
	p = realloc(p, *cap * size);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

static void add_event(struct recording *r, uint64_t t, uint32_t state)
{
	struct events *ev = &r->ev;

	if (ev->n == ev->cap) {
		ev->e = grow(ev->e, &ev->cap, sizeof(*ev->e), 65536);
	}
	ev->e[ev->n].t = t;
	ev->e[ev->n].state = state;
	ev->n++;
}

static void add_hole(struct recording *r, uint64_t from, uint64_t to)
{
	struct holes *h = &r->holes;

	if (h->n == h->cap) {
		size_t cap = h->cap;

		h->a = grow(h->a, &cap, sizeof(*h->a), 1024);
		h->b = realloc(h->b, cap * sizeof(*h->b));
		h->cap = cap;
		if (!h->b) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	h->a[h->n] = from;
	h->b[h->n++] = to;
}

static void keep_edge(void *arg, uint64_t t, uint32_t state)
{
	struct rec_ctx *x = arg;
	const struct la_trig *g = x->trig;

	add_event(x->r, t, state);
	if (g && g->kind && x->have && !x->r->hit && t >= x->armed) {
		uint32_t was = x->was & g->mask, now = state & g->mask;

		if (g->kind == LA_TRIG_ANY ? was != now : was != g->value && now == g->value) {
			x->r->hit = t;
		}
	}
	x->was = state;
	x->have = 1;
}

/* A hole: kept, and its end an event, with the levels there; no hit across one. */
static void keep_hole(void *arg, uint64_t from, uint64_t to, uint32_t state)
{
	struct rec_ctx *x = arg;

	add_hole(x->r, from, to);
	add_event(x->r, to, state);
	x->was = state;
}

/* Waiting for a hit: drop what is more than pre before the newest, keeping the levels. */
static void trim(struct recording *r, uint64_t newest, uint64_t pre)
{
	size_t i = 0, j, k = 0;

	if (r->ev.n < 2 * KEEP_MIN || newest < pre) {
		return;
	}
	while (i + 1 < r->ev.n && r->ev.e[i + 1].t < newest - pre) {
		i++;
	}
	if (i + KEEP_MIN > r->ev.n) {
		i = r->ev.n > KEEP_MIN ? r->ev.n - KEEP_MIN : 0;
	}
	/* e[i] is the levels from its time on: it stays, as the first. */
	memmove(r->ev.e, r->ev.e + i, (r->ev.n - i) * sizeof(*r->ev.e));
	r->ev.n -= i;
	for (j = 0; j < r->holes.n; j++) {
		if (r->holes.b[j] >= r->ev.e[0].t) {
			r->holes.a[k] = r->holes.a[j];
			r->holes.b[k++] = r->holes.b[j];
		}
	}
	r->holes.n = k;
}

/* After PRU0 stops, a span of the ring read again, whole, into r; 0, or -1 if the ring no longer holds it. */
static int reread(struct recording *r, struct la_src *src)
{
	struct recording whole = { .mode = r->mode, .begin = r->begin, .finish = r->finish,
				   .hit = r->hit };
	struct rec_ctx y = { .r = &whole };
	uint64_t stopped;
	char e2[160];
	int ret = -1;

	if (la_src_freeze(&stopped, e2, sizeof(e2))) {
		fprintf(stderr, "%s\n", e2);
		return -1;
	}
	if (!la_src_reread(src, stopped, r->begin, r->finish, keep_edge, keep_hole, &y) && whole.ev.n) {
		free(r->ev.e);
		free(r->holes.a);
		free(r->holes.b);
		r->ev = whole.ev;
		r->holes = whole.holes;
		r->blocks = src->blocks;
		r->lost = src->lost;
		ret = 0;
	} else {
		free(whole.ev.e);
		free(whole.holes.a);
		free(whole.holes.b);
	}
	if (la_set_mode(src->mode.chans, src->mode.hz, e2, sizeof(e2))) {
		fprintf(stderr, "%s\n", e2);
	}
	return ret;
}

/* Until IEP0 is past t, and the blocks to t are surely in DDR. */
static void wait_until(uint64_t t)
{
	uint64_t now;

	while ((now = la_now()) < t + 300000u) {
		uint64_t ns = t + 300000u - now;

		usleep(ns > 20000000u ? 20000u : (useconds_t)(ns / 1000u + 1u));
	}
}

/*
 * A setup the reader follows live is recorded as the reader finds the
 * changes. One faster than that still has every sample in the ring, for as
 * long as it holds (span): a recording that fits waits out its time, or,
 * with a trigger, finds the hit live, then stops PRU0 and reads its span
 * of the ring after, whole (reread()). One too long for the ring is taken
 * live, with holes where the reader skipped.
 */
int record(struct recording *r, double secs, const struct la_trig *trig, double pre, double wait,
	   char *err, size_t errlen)
{
	struct la_src src;
	struct rec_ctx x = { .r = r, .trig = trig && trig->kind ? trig : NULL };
	uint64_t want = (uint64_t)(secs * 1e9), back = x.trig ? (uint64_t)(pre * 1e9) : 0;
	double start = now_us();
	int done = 0, after;

	memset(r, 0, sizeof(*r));
	if (la_src_open(&src, back / 1e9 + 0.001, err, errlen)) {
		return -1;
	}
	r->mode = src.mode;
	after = !src.mode.live && (double)(want + back) < src.mode.span * 0.7e9;
	x.armed = la_now();
	if (after && !x.trig) {
		r->begin = x.armed + 100000u;
		r->finish = r->begin + want;
		wait_until(r->finish);
		if (reread(r, &src)) {
			snprintf(err, errlen, "the ring no longer held the recording");
			la_src_close(&src);
			rec_free(r);
			return -1;
		}
		goto done;
	}
	while (!done) {
		uint64_t at = src.next;

		la_src_poll(&src, keep_edge, keep_hole, &x, 50000000u);
		if (la_src_stale(&src)) {
			snprintf(err, errlen, "the analyzer's setup changed during the recording");
			la_src_close(&src);
			rec_free(r);
			return -1;
		}
		if (!src.have) {
			/* Nothing yet. */
		} else if (!x.trig) {
			done = src.now >= r->ev.e[0].t + want;
		} else if (!r->hit) {
			trim(r, src.now, back);
			if (now_us() - start > wait * 1e6) {
				snprintf(err, errlen, "no trigger hit in %g s", wait);
				la_src_close(&src);
				rec_free(r);
				return -1;
			}
		} else {
			done = after || src.now >= r->hit + want;
		}
		if (!done && src.next == at) {
			usleep(2000);
		}
	}
	r->blocks = src.blocks;
	r->lost = src.lost;
	if (x.trig) {
		r->begin = r->hit > back && r->hit - back > r->ev.e[0].t ? r->hit - back : r->ev.e[0].t;
		r->finish = r->hit + want;
	} else {
		r->begin = r->ev.e[0].t;
		r->finish = r->begin + want;
	}
	if (after) {
		/* Found live, the rest read after: from pre before the hit, as the ring holds it. */
		r->begin = r->hit > back ? r->hit - back : r->hit;
		wait_until(r->finish);
		if (reread(r, &src)) {
			snprintf(err, errlen, "the ring no longer held the recording");
			la_src_close(&src);
			rec_free(r);
			return -1;
		}
	}
done:
	/* The levels are unknown from the last sample to the end, if it came short. */
	if (src.last + src.mode.sample_ns < r->finish) {
		add_hole(r, src.last + src.mode.sample_ns, r->finish);
	}
	la_src_close(&src);
	if (r->lost) {
		fprintf(stderr, "%llu of %llu blocks of samples lost: the recording has %zu holes, "
			"marked\n", (unsigned long long)r->lost,
			(unsigned long long)(r->lost + r->blocks), r->holes.n);
	}
	rec_rewind(r);
	return 0;
}

void rec_rewind(struct recording *r)
{
	r->i = 0;
	r->state = r->ev.n ? r->ev.e[0].state : 0;
	while (r->i < r->ev.n && r->ev.e[r->i].t <= r->begin) {
		r->state = r->ev.e[r->i++].state;
	}
	r->state0 = r->state;
}

int rec_next(struct recording *r, uint64_t *t, uint32_t *was)
{
	if (r->i >= r->ev.n || r->ev.e[r->i].t > r->finish) {
		return 0;
	}
	*was = r->state;
	*t = r->ev.e[r->i].t;
	r->state = r->ev.e[r->i++].state;
	return 1;
}

void rec_free(struct recording *r)
{
	free(r->ev.e);
	free(r->holes.a);
	free(r->holes.b);
	memset(r, 0, sizeof(*r));
}

int holes_cross(const struct holes *h, uint64_t from, uint64_t to)
{
	size_t lo = 0, hi = h ? h->n : 0;

	/* The first hole ending at or after from: an interval from a hole's end
	 * starts at no real edge. One ending where a hole starts is whole. */
	while (lo < hi) {
		size_t m = (lo + hi) / 2;

		if (h->b[m] < from) {
			lo = m + 1;
		} else {
			hi = m;
		}
	}
	return lo < (h ? h->n : 0) && h->a[lo] < to;
}

/* How much of [from, to) holes cover. */
static uint64_t holes_in(const struct holes *h, uint64_t from, uint64_t to)
{
	size_t lo = 0, hi = h ? h->n : 0;
	uint64_t ns = 0;

	while (lo < hi) {
		size_t m = (lo + hi) / 2;

		if (h->b[m] < from) {
			lo = m + 1;
		} else {
			hi = m;
		}
	}
	for (; h && lo < h->n && h->a[lo] < to; lo++) {
		uint64_t a = h->a[lo] > from ? h->a[lo] : from, b = h->b[lo] < to ? h->b[lo] : to;

		ns += b > a ? b - a : 0;
	}
	return ns;
}

size_t rec_holes(const struct recording *r, uint64_t *ns)
{
	const struct holes *h = &r->holes;
	size_t i, n = 0;

	*ns = 0;
	for (i = 0; i < h->n; i++) {
		uint64_t a = h->a[i] > r->begin ? h->a[i] : r->begin;
		uint64_t b = h->b[i] < r->finish ? h->b[i] : r->finish;

		if (a < b) {
			n++;
			*ns += b - a;
		}
	}
	return n;
}

void changes_add(struct changes *ch, uint64_t t, int up)
{
	if (ch->n == ch->cap) {
		size_t cap = ch->cap;

		ch->t = grow(ch->t, &cap, sizeof(*ch->t), 4096);
		ch->up = realloc(ch->up, cap);
		ch->cap = cap;
		if (!ch->up) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	ch->t[ch->n] = t;
	ch->up[ch->n++] = (uint8_t)up;
}

int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

size_t nearest(const struct changes *a, const struct changes *b, int up, int64_t *out,
	       const struct holes *h)
{
	size_t i, j = 0, n = 0;

	if (!b->n) {
		return 0;
	}
	for (i = 0; i < a->n; i++) {
		int64_t best;
		uint64_t lo, hi;

		if (a->up[i] != up) {
			continue;
		}
		while (j + 1 < b->n && b->t[j + 1] <= a->t[i]) {
			j++;
		}
		best = (int64_t)(b->t[j] - a->t[i]);
		if (j + 1 < b->n && llabs((int64_t)(b->t[j + 1] - a->t[i])) < llabs(best)) {
			best = (int64_t)(b->t[j + 1] - a->t[i]);
		}
		lo = best < 0 ? a->t[i] + (uint64_t)best : a->t[i];
		hi = best < 0 ? a->t[i] : a->t[i] + (uint64_t)best;
		if (!holes_cross(h, lo, hi)) {
			out[n++] = best;
		}
	}
	return n;
}

void rec_changes(struct recording *r, struct changes ch[AI64_LA_NCHAN])
{
	uint32_t was;
	uint64_t t;
	int c;

	memset(ch, 0, sizeof(struct changes) * AI64_LA_NCHAN);
	rec_rewind(r);
	while (rec_next(r, &t, &was)) {
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if (level(r->state, c) != level(was, c)) {
				changes_add(&ch[c], t, level(r->state, c));
			}
		}
	}
	rec_rewind(r);
}

void changes_free(struct changes ch[AI64_LA_NCHAN])
{
	int c;

	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		free(ch[c].t);
		free(ch[c].up);
		ch[c].t = NULL;
		ch[c].up = NULL;
	}
}

void spread_of(int64_t *v, size_t n, struct spread *sp)
{
	sp->n = n;
	sp->min = sp->med = sp->max = 0;
	if (n) {
		qsort(v, n, sizeof(*v), cmp_i64);
		sp->min = v[0];
		sp->med = v[n / 2];
		sp->max = v[n - 1];
	}
}

/*
 * One channel over the recording, with level0 its level at begin: its
 * rises, time high, and the spread of its periods (rise to rise) and high
 * times. v has room for c->n values.
 */
void chan_stats(const struct changes *c, int level0, uint64_t begin, uint64_t finish, int64_t *v,
		struct chan_stats *out, const struct holes *holes)
{
	uint64_t last = begin;
	size_t i, n;
	int lv = level0;

	memset(out, 0, sizeof(*out));
	out->edges = c->n;
	for (i = 0; i <= c->n; i++) {
		uint64_t t = i < c->n ? c->t[i] : finish;

		/* Time high, of the time known: what holes leave of each level. */
		{
			uint64_t known = t - last - holes_in(holes, last, t);

			out->known_ns += known;
			if (lv) {
				out->high_ns += known;
			}
		}
		if (i == c->n) {
			break;
		}
		last = t;
		lv = c->up[i];
		out->rises += (size_t)lv;
	}
	out->level_end = lv;
	for (i = 0, n = 0; i < c->n; i++) {
		if (c->up[i] && i + 2 < c->n && !holes_cross(holes, c->t[i], c->t[i + 2])) {
			v[n++] = (int64_t)(c->t[i + 2] - c->t[i]);
		}
	}
	spread_of(v, n, &out->period);
	for (i = 0, n = 0; i + 1 < c->n; i++) {
		if (c->up[i] && !c->up[i + 1] && !holes_cross(holes, c->t[i], c->t[i + 1])) {
			v[n++] = (int64_t)(c->t[i + 1] - c->t[i]);
		}
	}
	spread_of(v, n, &out->high);
}
