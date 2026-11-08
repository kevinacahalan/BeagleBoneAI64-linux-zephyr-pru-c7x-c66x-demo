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

#include <pthread.h>
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

/* ---- following the copier: dump mode, continuous ---- */

/* ICSSG1's data RAM, slice 0 then 1 (pru/common/icssg.h), and IEP0 (la.c). */
#define ICSSG1_DRAM   0x0b100000u
#define ICSSG1_IEP0   0x0b12e000u
#define LA_CFG_WR     (0x1f00u + 16u) /* struct la_cfg's wr (la_ipc.h) */

/* IEP0's count, all 64 bits: high, low, high again until they agree. */
static uint64_t iep_now(const volatile uint32_t *iep)
{
	uint32_t hi, lo;

	do {
		hi = iep[0x14 / 4];
		lo = iep[0x10 / 4];
	} while (iep[0x14 / 4] != hi);
	return (uint64_t)hi << 32 | lo;
}

/*
 * The pump: copies each block, as the copier's count says it is there, out
 * of the burst buffer, which Linux maps uncached, into a cached copy laid
 * out the same, so the decoder never waits on DDR: one thread reads about
 * 250 MB/s, the copier writes up to 132 MB/s at four samples a word, and
 * reading and decoding in turn on one thread took 120.
 */
static void *pump(void *arg)
{
	struct la_follow *f = arg;
	uint32_t prd = f->rd;

	while (!f->stop) {
		uint32_t wr = *f->wr - 4u, slot, n, addr, from, len;

		if ((int32_t)(wr - prd) <= 0) {
			usleep(50);
			continue;
		}
		if (wr - prd > AI64_LA_BURST_SLOTS - 16384u) {
			prd = wr - 1024u;
		}
		slot = prd % AI64_LA_BURST_SLOTS;
		n = wr - prd;
		if (n > AI64_LA_BURST_SLOTS - slot) {
			n = AI64_LA_BURST_SLOTS - slot;
		}
		if (n > 4096u) {
			n = 4096u;
		}
		addr = AI64_LA_BURST_HEAD + slot * AI64_LA_BURST_SLOT;
		from = addr & ~63u;
		len = (addr + n * AI64_LA_BURST_SLOT - from + 63u) & ~63u;
		la_read_res((uint8_t *)f->mirror + from, AI64_LA_BURST_ADDR + from, len);
		prd += n;
		__atomic_store_n(&f->pumped, prd, __ATOMIC_RELEASE);
	}
	return NULL;
}

int la_follow_open(struct la_follow *f, char *err, size_t errlen)
{
	static const volatile uint32_t *dram, *iep;
	uint32_t head[16];
	int s;

	memset(f, 0, sizeof(*f));
	if (la_map(0)) {
		snprintf(err, errlen, "the logic analyzer does not run");
		return -1;
	}
	f->pru = -1;
	for (s = 0; s < 2; s++) {
		if (AI64_LA_MODE_OF(la_hdr->s[s].cfg_mode) == AI64_LA_MODE_RAW &&
		    AI64_LA_MODE_OF(la_hdr->s[1 - s].cfg_mode) == AI64_LA_MODE_DUMP) {
			f->pru = s;
		}
	}
	if (f->pru < 0) {
		snprintf(err, errlen, "not in dump mode");
		return -1;
	}
	if (!dram) {
		dram = map_phys(ICSSG1_DRAM, 0x4000);
		iep = map_phys(ICSSG1_IEP0, 4096);
	}
	if (!dram || !iep) {
		snprintf(err, errlen, "cannot map ICSSG1 (root?)");
		return -1;
	}
	la_read_res(head, AI64_LA_BURST_ADDR, sizeof(head));
	if (head[0] != AI64_LA_BURST_MAGIC || head[3] != AI64_LA_BURST_SLOTS) {
		snprintf(err, errlen, "the copier has not started");
		return -1;
	}
	f->lane = AI64_LA_LANE_OF(la_hdr->s[f->pru].cfg_mode);
	f->ns = block_ns(f->lane);
	f->n = 19 * per_word(f->lane);
	f->t0_32 = head[1];
	f->first = head[2];
	f->now = iep_now(iep);
	f->t0 = f->now + (uint64_t)(int64_t)(int32_t)(head[1] - (uint32_t)f->now);
	f->wr = dram + ((1u - (uint32_t)f->pru) * 0x2000u + LA_CFG_WR) / 4u;
	f->cfg_seq[0] = la_hdr->s[0].cfg_seq;
	f->cfg_seq[1] = la_hdr->s[1].cfg_seq;
	f->rd = *f->wr - 4u;
	f->pumped = f->rd;
	f->mirror = aligned_alloc(64, AI64_LA_BURST_SIZE);
	if (!f->mirror || pthread_create(&f->pump, NULL, pump, f)) {
		free(f->mirror);
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	f->open = 1;
	return 0;
}

int la_follow_stale(const struct la_follow *f)
{
	return la_hdr->s[0].cfg_seq != f->cfg_seq[0] || la_hdr->s[1].cfg_seq != f->cfg_seq[1];
}

void la_follow_close(struct la_follow *f)
{
	if (f->open) {
		f->stop = 1;
		pthread_join(f->pump, NULL);
		free(f->mirror);
	}
	f->mirror = NULL;
	f->open = 0;
}

/* cur's sample in each of a word's places, as the lane packs them. */
static uint32_t replicate(uint32_t lane, uint32_t cur)
{
	switch (lane) {
	case AI64_LA_LANE_ALL:
		return cur;
	case AI64_LA_LANE_LO16:
	case AI64_LA_LANE_MID16:
		return (cur >> (lane == AI64_LA_LANE_MID16 ? 8 : 0) & 0xffffu) * 0x10001u;
	default:
		return (cur >> (8 * (lane - AI64_LA_LANE_B0)) & 0xffu) * 0x01010101u;
	}
}

/*
 * The blocks copied since the last call, as edges of stream st (its s is
 * the raw PRU), to fn, and holes, where the copier missed blocks or this
 * reader fell a buffer behind, to st->on_hole if set. It stays a few blocks
 * behind the copier's count: a block's store into DDR lands after the
 * count's into ICSSG1's own RAM.
 */
void la_follow_poll(struct la_follow *f, struct stream *st, edge_fn fn, void *ctx)
{
	uint32_t wr = __atomic_load_n(&f->pumped, __ATOMIC_ACQUIRE);

	if ((int32_t)(wr - f->rd) <= 0) {
		return;
	}
	if (wr - f->rd > AI64_LA_BURST_SLOTS - 16384u) {
		/* A buffer behind: on from near the newest, the jump a hole. */
		f->rd = wr - 1024u;
		f->behind++;
	}
	/* At most 40 ms of the signal a call, so the caller gets its turn. */
	if (wr - f->rd > 65536u) {
		wr = f->rd + 65536u;
	}
	while (f->rd != wr) {
		uint32_t slot = f->rd % AI64_LA_BURST_SLOTS, n = wr - f->rd, i;
		const uint32_t *base;

		if (n > AI64_LA_BURST_SLOTS - slot) {
			n = AI64_LA_BURST_SLOTS - slot;
		}
		base = f->mirror + (AI64_LA_BURST_HEAD + slot * AI64_LA_BURST_SLOT) / 4u;
		for (i = 0; i < n; i++) {
			const uint32_t *w = base + i * AI64_LA_BURST_SLOT / 4u;
			uint64_t d = (uint32_t)((w[19] ^ f->t0_32) - f->first), tb;
			int k = 0;

			if (f->have && (d <= f->last || d > f->last + AI64_LA_BURST_SLOTS)) {
				continue; /* written over meanwhile, or not this run's */
			}
			tb = f->t0 + 24u + (f->first + d) * f->ns;
			if (!f->have || d != f->last + 1) {
				uint32_t v = sample(w, f->lane, 0);

				if (f->have) {
					f->lost += d - f->last - 1;
					if (st->on_hole) {
						st->on_hole(ctx, st, f->t0 + 24u + (f->first + f->last + 1) * f->ns, tb, v);
					} else {
						fn(ctx, st, tb, v);
					}
				} else {
					fn(ctx, st, tb, v);
				}
				f->cur = v;
				f->have = 1;
				k = 1;
			}
			/* A word holding cur in every place has no change: on to the next. */
			{
				int per = f->n / 19, wi;
				uint32_t r = replicate(f->lane, f->cur);

				for (wi = k / per; wi < 19; wi++) {
					if (w[wi] == r && k <= wi * per) {
						continue;
					}
					for (k = k > wi * per ? k : wi * per; k < (wi + 1) * per; k++) {
						uint32_t v = sample(w, f->lane, k);

						if (v != f->cur) {
							fn(ctx, st, tb + 8u * (uint64_t)k, v);
							f->cur = v;
							f->edges++;
						}
					}
					r = replicate(f->lane, f->cur);
				}
			}
			f->last = d;
			f->blocks++;
			f->now = tb + f->ns;
		}
		f->rd += n;
	}
}
