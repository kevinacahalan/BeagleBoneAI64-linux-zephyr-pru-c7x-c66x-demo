/* SPDX-License-Identifier: MIT */
/*
 * A recording of both streams (la.h): ai64 la capture and measure (la.c),
 * and the MCP server's tools (mcp.c), take one with record() and read it
 * back merged by time with rec_next().
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

void keep_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
{
	struct events *ev = (struct events *)ctx + st->s;

	if (ev->n == ev->cap) {
		ev->cap = ev->cap ? ev->cap * 2 : 65536;
		ev->e = realloc(ev->e, ev->cap * sizeof(*ev->e));
		if (!ev->e) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	ev->e[ev->n].t = t;
	ev->e[ev->n].state = state;
	ev->n++;
}



/* The first trigger hit since the reader started, or 0. */
uint64_t first_fresh_trig(struct recording *r)
{
	uint64_t t = 0;
	size_t i;
	int s;

	for (s = 0; s < 2; s++) {
		for (i = 0; i < r->st[s].nmark; i++) {
			struct mark *m = &r->st[s].mark[i];

			if (m->trig && m->fresh && (!t || m->t < t)) {
				t = m->t;
				break;
			}
		}
	}
	return t;
}

/*
 * secs of both streams from now; or, with trigger, around the next trigger
 * hit: a tenth of secs before it, from the ring's history, and the rest
 * after. A trigger recording starts HISTORY_BYTES back in each ring, about
 * 5 s of the demo's signals on PRU1.
 */
#define HISTORY_BYTES (8u << 20)

int record(struct recording *r, double secs, int trigger)
{
	double start;
	uint64_t hit = 0;
	int s;

	memset(r, 0, sizeof(*r));
	if (la_map(1)) {
		return -1;
	}
	for (s = 0; s < 2; s++) {
		if (stream_init(&r->st[s], s)) {
			return -1;
		}
		r->st[s].keep_marks = 1;
		if (trigger) {
			/* Back into the ring for the time before the hit, but no further
			 * than the RTU has written since it started (wr counts from 0). */
			uint32_t back = r->st[s].rd < HISTORY_BYTES ? r->st[s].rd : HISTORY_BYTES;

			r->st[s].rd -= back & ~(AI64_LA_REC_SIZE - 1u);
		}
	}
	if (wait_ticks(r->st, keep_edge, r->ev)) {
		return -1;
	}
	start = now_us();
	for (;;) {
		for (s = 0; s < 2; s++) {
			stream_poll(&r->st[s], keep_edge, r->ev);
		}
		if (!trigger) {
			if (now_us() - start >= secs * 1e6) {
				break;
			}
		} else if (!hit) {
			hit = first_fresh_trig(r);
			if (!hit && now_us() - start > 30e6) {
				fprintf(stderr, "no trigger hit in 30 s (ai64 la trigger sets one)\n");
				return -1;
			}
		} else if (r->st[0].base >= hit + (uint64_t)(secs * 0.9e9) &&
			   r->st[1].base >= hit + (uint64_t)(secs * 0.9e9)) {
			break;
		}
		usleep(2000);
	}
	for (s = 0; s < 2; s++) {
		stream_poll(&r->st[s], keep_edge, r->ev);
		if (r->st[s].missed || r->st[s].gaps) {
			fprintf(stderr, "PRU%d: %llu records missed here, %llu gaps in the stream: "
				"the recording has holes\n", s, (unsigned long long)r->st[s].missed,
				(unsigned long long)r->st[s].gaps);
		}
	}
	r->begin = r->st[0].first > r->st[1].first ? r->st[0].first : r->st[1].first;
	r->finish = r->st[0].base < r->st[1].base ? r->st[0].base : r->st[1].base;
	if (hit) {
		uint64_t pre = (uint64_t)(secs * 0.1e9), post = (uint64_t)(secs * 0.9e9);

		if (hit - r->begin > pre) {
			r->begin = hit - pre;
		}
		if (r->finish > hit + post) {
			r->finish = hit + post;
		}
		r->hit = hit;
	}
	/* Each stream's state at begin: its first tick's, then its edges before begin. */
	for (s = 0; s < 2; s++) {
		r->state[s] = r->st[s].first_state;
		while (r->i[s] < r->ev[s].n && r->ev[s].e[r->i[s]].t <= r->begin) {
			r->state[s] = r->ev[s].e[r->i[s]++].state;
		}
	}
	return 0;
}

/* The next edge in time, up to finish: its time, and the state before it in was. */
int rec_next(struct recording *r, uint64_t *t, uint32_t was[2])
{
	int a = r->i[0] < r->ev[0].n && r->ev[0].e[r->i[0]].t <= r->finish;
	int b = r->i[1] < r->ev[1].n && r->ev[1].e[r->i[1]].t <= r->finish;
	int s;

	if (!a && !b) {
		return 0;
	}
	was[0] = r->state[0];
	was[1] = r->state[1];
	s = a && (!b || r->ev[0].e[r->i[0]].t <= r->ev[1].e[r->i[1]].t) ? 0 : 1;
	*t = r->ev[s].e[r->i[s]].t;
	r->state[s] = r->ev[s].e[r->i[s]++].state;
	return 1;
}

void rec_free(struct recording *r)
{
	free(r->ev[0].e);
	free(r->ev[1].e);
	stream_free(&r->st[0]);
	stream_free(&r->st[1]);
}

/* Both streams' marks in [begin, finish], merged by time, into *out; how many. */
size_t rec_marks(struct recording *r, struct mark **out)
{
	size_t n = 0, i = 0, j = 0;
	struct mark *m = malloc((r->st[0].nmark + r->st[1].nmark + 1) * sizeof(*m));

	if (!m) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	while (i < r->st[0].nmark || j < r->st[1].nmark) {
		struct mark *a = i < r->st[0].nmark ? &r->st[0].mark[i] : NULL;
		struct mark *b = j < r->st[1].nmark ? &r->st[1].mark[j] : NULL;
		struct mark *k = a && (!b || a->t <= b->t) ? (i++, a) : (j++, b);

		if (k->t >= r->begin && k->t <= r->finish) {
			m[n++] = *k;
		}
	}
	*out = m;
	return n;
}

void changes_add(struct changes *ch, uint64_t t, int up)
{
	if (ch->n == ch->cap) {
		ch->cap = ch->cap ? ch->cap * 2 : 4096;
		ch->t = realloc(ch->t, ch->cap * sizeof(*ch->t));
		ch->up = realloc(ch->up, ch->cap);
		if (!ch->t || !ch->up) {
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

/* For each change of a that goes up (or down), the signed time to b's nearest change. */
size_t nearest(const struct changes *a, const struct changes *b, int up, int64_t *out)
{
	size_t i, j = 0, n = 0;

	if (!b->n) {
		return 0;
	}
	for (i = 0; i < a->n; i++) {
		int64_t best;

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
		out[n++] = best;
	}
	return n;
}

/* Each channel's changes over the recording, in ch; the recording can be read again after. */
void rec_changes(struct recording *r, struct changes ch[AI64_LA_NCHAN])
{
	uint32_t start[2] = { r->state[0], r->state[1] }, was[2];
	size_t at[2] = { r->i[0], r->i[1] };
	uint64_t t;
	int c;

	memset(ch, 0, sizeof(struct changes) * AI64_LA_NCHAN);
	while (rec_next(r, &t, was)) {
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if (level(r->state, c) != level(was, c)) {
				changes_add(&ch[c], t, level(r->state, c));
			}
		}
	}
	r->state[0] = start[0];
	r->state[1] = start[1];
	r->i[0] = at[0];
	r->i[1] = at[1];
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
		struct chan_stats *out)
{
	uint64_t last = begin;
	size_t i, n;
	int lv = level0;

	memset(out, 0, sizeof(*out));
	out->edges = c->n;
	for (i = 0; i < c->n; i++) {
		if (lv) {
			out->high_ns += c->t[i] - last;
		}
		last = c->t[i];
		lv = c->up[i];
		out->rises += (size_t)lv;
	}
	if (lv) {
		out->high_ns += finish - last;
	}
	out->level_end = lv;
	for (i = 0, n = 0; i < c->n; i++) {
		if (c->up[i] && i + 2 < c->n) {
			v[n++] = (int64_t)(c->t[i + 2] - c->t[i]);
		}
	}
	spread_of(v, n, &out->period);
	for (i = 0, n = 0; i + 1 < c->n; i++) {
		if (c->up[i] && !c->up[i + 1]) {
			v[n++] = (int64_t)(c->t[i + 1] - c->t[i]);
		}
	}
	spread_of(v, n, &out->high);
}

uint32_t la_lane(uint32_t mask)
{
	if (!(mask & ~0xffffu)) {
		return AI64_LA_LANE_LO16;
	}
	if (!(mask & ~0xffff00u)) {
		return AI64_LA_LANE_MID16;
	}
	return AI64_LA_LANE_ALL;
}

int la_set_mode(int raw, const uint32_t mask[2], char *err, size_t errlen)
{
	static const char *const pru[2] = { "g1pru0", "g1pru1" };
	volatile struct ai64_la *hdr = la_map_trigger();
	const struct ai64_core *c[2];
	double give_up;
	int s, order[2];

	if (!hdr) {
		snprintf(err, errlen, "the logic analyzer does not run");
		return -1;
	}
	if (raw > 1 || (raw >= 0 && !(mask[raw] & AI64_LA_MASK(raw)))) {
		snprintf(err, errlen, "no such PRU, or none of its channels");
		return -1;
	}
	for (s = 0; s < 2; s++) {
		volatile struct ai64_la_stream *h = &hdr->s[s];

		c[s] = core_find(pru[s]);
		if (!c[s]) {
			snprintf(err, errlen, "no core %s", pru[s]);
			return -1;
		}
		h->cfg_mode = raw < 0 ? AI64_LA_MODE_EDGE : s != raw ? AI64_LA_MODE_CONV :
			      AI64_LA_MODE_RAW | la_lane(mask[s] & AI64_LA_MASK(s)) << 8;
		h->cfg_mask = raw >= 0 && s != raw ? 0 : mask[s] & AI64_LA_MASK(s);
		__sync_synchronize();
		h->cfg_seq++;
	}
	/* Each RTU passes its mode on within a tick, 1 ms. */
	give_up = now_us() + 200000;
	while (hdr->s[0].cfg_ack != hdr->s[0].cfg_seq || hdr->s[1].cfg_ack != hdr->s[1].cfg_seq) {
		if (now_us() > give_up) {
			snprintf(err, errlen, "ICSSG1's RTUs did not take the mode");
			return -1;
		}
		usleep(1000);
	}
	/*
	 * Stop both PRUs, let the RTUs take what is left in the rings, and
	 * start them again: in raw mode the converter first, so it is waiting
	 * when the raw PRU's first block comes. The Tx_PRUs run on.
	 */
	for (s = 0; s < 2; s++) {
		if (rproc_running(c[s]->rproc, NULL, 0) && rproc_write(c[s]->rproc, "state", "stop")) {
			snprintf(err, errlen, "could not stop %s (root?)", pru[s]);
			return -1;
		}
	}
	usleep(20000);
	order[0] = raw == 0 ? 1 : 0;
	order[1] = 1 - order[0];
	for (s = 0; s < 2; s++) {
		if (rproc_write(c[order[s]]->rproc, "state", "start")) {
			snprintf(err, errlen, "could not start %s", pru[order[s]]);
			return -1;
		}
		usleep(10000);
	}
	return 0;
}

int la_set_trigger(int pru, uint32_t mode, uint32_t mask, uint32_t value)
{
	volatile struct ai64_la *hdr = la_map_trigger();
	int s;

	if (!hdr) {
		return -1;
	}
	for (s = 0; s < 2; s++) {
		volatile struct ai64_la_stream *h = &hdr->s[s];

		h->trig_mode = s == pru ? mode : AI64_LA_TRIG_OFF;
		h->trig_mask = s == pru ? mask : 0;
		h->trig_value = s == pru ? value : 0;
		__sync_synchronize();
		h->trig_seq++;
	}
	return 0;
}
