/* SPDX-License-Identifier: MIT */
/*
 * A burst (la.h): one PRU's channels every 8 ns with no gap, whatever the
 * signal, for as long as the burst buffer holds. Raw mode's converter
 * writes a record at each change, so a signal faster than it keeps up with
 * is cut into bursts with holes between (la_conv.asm). In dump mode the
 * other PRU copies each block of samples into the buffer as it is
 * (la_dump.asm), at the same cost whatever the pins do, and this finds the
 * changes after: ai64 la burst, the web page's Burst, and the MCP server's
 * la_capture take one.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

static void noop_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
{
	(void)ctx;
	(void)st;
	(void)t;
	(void)state;
}

/* Samples a word: one, two, or four (ai64_la.h's lanes). */
static int per_word(uint32_t lane)
{
	return lane == AI64_LA_LANE_ALL ? 1 : lane <= AI64_LA_LANE_MID16 ? 2 : 4;
}

static uint64_t block_ns(uint32_t lane)
{
	return 152u * (uint64_t)per_word(lane);
}

double la_burst_max(int pru, uint32_t mask)
{
	return AI64_LA_BURST_SLOTS * block_ns(la_lane_burst(mask & AI64_LA_MASK(pru))) / 1e9;
}

/* Sample i of a block, the pins as the PRU numbers them. */
static uint32_t sample(const uint32_t *w, uint32_t lane, int i)
{
	switch (lane) {
	case AI64_LA_LANE_ALL:
		return w[i];
	case AI64_LA_LANE_LO16:
	case AI64_LA_LANE_MID16:
		return (i & 1 ? w[i / 2] >> 16 : w[i / 2] & 0xffffu) << (lane == AI64_LA_LANE_MID16 ? 8 : 0);
	default:
		return (w[i / 4] >> (8 * (i & 3)) & 0xffu) << (8 * (lane - AI64_LA_LANE_B0));
	}
}

int la_burst(int pru, uint32_t mask, double secs, struct recording *r, struct burst_info *info,
	     char *err, size_t errlen)
{
	static const char *const prus[2] = { "g1pru0", "g1pru1" };
	uint32_t m[2] = { 0, 0 }, *buf = NULL, t0_32, first, slots, cur = 0;
	volatile uint32_t *head;
	const struct ai64_core *copier;
	struct stream st;
	uint64_t ns, t0, want, maxb, d, dmax = 0, dmin, prev = 0;
	double started;
	int32_t *slot_of = NULL;
	double give_up;
	int have = 0, i, n, rc = -1;

	memset(r, 0, sizeof(*r));
	memset(info, 0, sizeof(*info));
	mask &= AI64_LA_MASK(pru);
	if (la_map(1) || !mask) {
		snprintf(err, errlen, mask ? "the logic analyzer does not run" :
			 "a burst takes channels of one PRU");
		return -1;
	}
	info->pru = pru;
	info->lane = la_lane_burst(mask);
	ns = block_ns(info->lane);
	n = 19 * per_word(info->lane);
	if (secs <= 0 || secs > la_burst_max(pru, mask)) {
		secs = la_burst_max(pru, mask);
	}
	copier = core_find(prus[1 - pru]);
	head = map_phys(AI64_LA_BURST_ADDR, 4096);
	if (!copier || !head || stream_init(&st, pru)) {
		snprintf(err, errlen, "no %s, or no burst buffer", prus[1 - pru]);
		return -1;
	}
	/* The stream's last tick before: IEP0's top bits, for t0's. */
	give_up = now_us() + 100000;
	while (!st.ticked && now_us() < give_up) {
		stream_poll(&st, noop_edge, NULL);
		usleep(1000);
	}
	head[0] = 0;
	__sync_synchronize();
	m[pru] = mask;
	if (la_set_mode_ex(pru, m, 1, err, errlen)) {
		goto out;
	}
	give_up = now_us() + 300000;
	while (head[0] != AI64_LA_BURST_MAGIC) {
		if (now_us() > give_up) {
			snprintf(err, errlen, "%s did not start copying", prus[1 - pru]);
			goto out;
		}
		usleep(1000);
	}
	started = now_us();
	t0_32 = head[1];
	first = head[2];
	slots = head[3];
	usleep((useconds_t)(secs * 1e6) + 2000);
	/* Stopped, it leaves the buffer as it was; the raw PRU samples on. */
	if (rproc_write(copier->rproc, "state", "stop")) {
		snprintf(err, errlen, "could not stop %s (root?)", prus[1 - pru]);
		goto out;
	}
	stream_poll(&st, noop_edge, NULL);
	t0 = st.base + (uint64_t)(int64_t)(int32_t)(t0_32 - (uint32_t)st.base);

	buf = malloc(AI64_LA_BURST_SIZE);
	if (!buf || slots != AI64_LA_BURST_SLOTS) {
		snprintf(err, errlen, buf ? "the burst buffer's header is wrong" : "out of memory");
		goto out;
	}
	la_read_res(buf, AI64_LA_BURST_ADDR, AI64_LA_BURST_SIZE);
	/*
	 * Each slot's block, from the first: no more than could have come since
	 * the copier started, by the clock here (the RTUs keep quiet through a
	 * burst, so the stream has no tick to go by). A slot an earlier burst
	 * left reads as a number far out of that.
	 */
	maxb = (uint64_t)((now_us() - started) * 1e3) / ns + 4096u;
	slot_of = malloc(maxb * sizeof(*slot_of));
	if (!slot_of) {
		snprintf(err, errlen, "out of memory");
		goto out;
	}
	memset(slot_of, 0xff, maxb * sizeof(*slot_of));
	for (i = 0; i < (int)slots; i++) {
		const uint32_t *w = buf + (AI64_LA_BURST_HEAD + (size_t)i * AI64_LA_BURST_SLOT) / 4u;

		d = (uint32_t)((w[19] ^ t0_32) - first);
		if (d < maxb) {
			slot_of[d] = i;
			dmax = d > dmax ? d : dmax;
		}
	}
	/* The newest secs of them. */
	want = (uint64_t)(secs * 1e9) / ns;
	dmin = dmax + 1 > want ? dmax + 1 - want : 0;
	for (d = dmin; d <= dmax; d++) {
		const uint32_t *w;
		uint64_t tb = t0 + 24u + (first + d) * ns;
		int k = 0;

		if (slot_of[d] < 0) {
			continue;
		}
		w = buf + (AI64_LA_BURST_HEAD + (size_t)slot_of[d] * AI64_LA_BURST_SLOT) / 4u;
		if (!have) {
			cur = sample(w, info->lane, 0);
			r->state[pru] = cur;
			r->begin = tb;
			have = 1;
			k = 1;
		} else if (d != prev + 1) {
			/* Blocks lost: from the first of them to this one's first sample. */
			cur = sample(w, info->lane, 0);
			rec_add_hole(r, pru, t0 + 24u + (first + prev + 1) * ns, tb, cur);
			info->lost += d - prev - 1;
			k = 1;
		}
		for (; k < n; k++) {
			uint32_t v = sample(w, info->lane, k);

			if (v != cur) {
				rec_add_edge(r, pru, tb + 8u * (uint64_t)k, v);
				cur = v;
				info->edges++;
			}
		}
		info->blocks++;
		prev = d;
	}
	if (!have) {
		snprintf(err, errlen, "the burst buffer holds no block");
		goto out;
	}
	r->finish = t0 + 24u + (first + dmax + 1) * ns;
	info->blocks += info->lost;
	/* The other PRU's channels were not recorded. */
	rec_add_hole(r, 1 - pru, r->begin, r->finish, 0);
	rc = 0;
out:
	free(buf);
	free(slot_of);
	stream_free(&st);
	return rc;
}
