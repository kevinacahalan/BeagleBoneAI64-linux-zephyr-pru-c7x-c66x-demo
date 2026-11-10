/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's setup and reader (la.h). PRU0 writes blocks of
 * samples into the ring in DDR at a fixed pace (ai64_la.h), so which blocks
 * are due is a matter of IEP0's count, read here directly: there is no
 * write count to follow.
 *
 * The ring is mapped uncached, which one thread reads at about 250 MB/s.
 * So one pump thread a process copies each block, once it is due, out of
 * the ring into a cached copy of it, and every reader in the process finds
 * the changes in that copy, each at its own pace: the web page's viewers
 * share one pass over DDR. A setup that writes faster than the pump reads
 * (LA_LIVE_BPS) still has every sample in the ring, but the pump falls
 * behind and skips ahead, and what it skipped is a hole.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

#define LA_DT "/proc/device-tree/reserved-memory/ai64-la@ac000000"
#define PRU0  "g1pru0"

const struct la_chan la_chans[AI64_LA_NCHAN] = {
#define X(c, pin, ball, pad, bit) [c] = { pin, ball, pad, bit },
	AI64_LA_CHANNELS(X)
#undef X
};

/* What the demo's jumpers bring to each channel (pru/logic_analyzer/README.md). */
const struct la_wired la_wired[AI64_LA_NCHAN] = {
	[0] = { "ECAP", "P8.15" },
	[1] = { "SPI7_CS", "P9.28" },
	[2] = { "SPI7_CLK", "P9.31" },
	[3] = { "SPI7_MOSI", "P9.30" },
	[4] = { "SQUARE", "P8.18" },
	[5] = { "S0_A", "P8.21" },
	[6] = { "S0_B", "P8.12" },
	[7] = { "EHRPWM4", "P9.25" },
	[8] = { "S1_A", "P9.12" },
	[9] = { "S1_B", "P9.15" },
};

const struct la_rate la_rates[] = {
#define X(hz, period) { hz, period },
	AI64_LA_RATES(X)
#undef X
};
const size_t la_nrates = sizeof(la_rates) / sizeof(la_rates[0]);

static const volatile uint32_t *ring, *iep;
static volatile struct ai64_la_cfg *cfg;

static int map_all(char *err, size_t errlen)
{
	int fd;

	if (ring && iep && cfg) {
		return 0;
	}
	if (access(LA_DT, F_OK)) {
		snprintf(err, errlen, "%s is missing: the overlay is older than the logic analyzer; "
			 "run install-beagle.sh and reboot", LA_DT);
		return -1;
	}
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open /dev/mem (root?)");
		return -1;
	}
	ring = mmap(NULL, AI64_LA_RING_SIZE, PROT_READ, MAP_SHARED, fd, AI64_LA_RING_ADDR);
	iep = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, ICSSG1_IEP0);
	close(fd);
	cfg = map_phys(ICSSG1_DRAM0, 0x2000);
	if (ring == MAP_FAILED || iep == MAP_FAILED || !cfg) {
		ring = NULL;
		iep = NULL;
		snprintf(err, errlen, "cannot map the analyzer's memory");
		return -1;
	}
	cfg = (volatile struct ai64_la_cfg *)((volatile uint8_t *)cfg + AI64_LA_CFG);
	return 0;
}

uint64_t la_now(void)
{
	uint32_t hi, lo;

	do {
		hi = iep[0x14 / 4];
		lo = iep[0x10 / 4];
	} while (iep[0x14 / 4] != hi);
	return (uint64_t)hi << 32 | lo;
}

/* ---- the setup ---- */

static uint32_t gpi_of(uint32_t chans)
{
	uint32_t g = 0;
	int c;

	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (chans >> c & 1u) {
			g |= 1u << la_chans[c].bit;
		}
	}
	return g;
}

static uint32_t chans_of(uint32_t gpi)
{
	uint32_t m = 0;
	int c;

	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (gpi >> la_chans[c].bit & 1u) {
			m |= 1u << c;
		}
	}
	return m;
}

/* The fewest bytes a sample the GPI bits fit, at that period. */
static uint32_t lane_of(uint32_t gpi, uint32_t period)
{
	if (period > AI64_LA_LANE_PERIOD_MAX) {
		return AI64_LA_LANE_ALL;
	}
	if (!(gpi & ~0xffu)) {
		return AI64_LA_LANE_B0;
	}
	if (!(gpi & ~0xff00u)) {
		return AI64_LA_LANE_B1;
	}
	if (!(gpi & ~0xffffu)) {
		return AI64_LA_LANE_LO16;
	}
	if (!(gpi & ~0xffff00u)) {
		return AI64_LA_LANE_MID16;
	}
	return AI64_LA_LANE_ALL;
}

int la_mode_of(struct la_mode *m, uint32_t chans, uint32_t hz)
{
	size_t i;

	memset(m, 0, sizeof(*m));
	chans &= (1u << AI64_LA_NCHAN) - 1u;
	for (i = 0; i < la_nrates && la_rates[i].hz != hz; i++) {
	}
	if (!chans || i == la_nrates) {
		return -1;
	}
	m->chans = chans;
	m->hz = hz;
	m->period = la_rates[i].period;
	m->gpi = gpi_of(chans);
	m->lane = lane_of(m->gpi, m->period);
	m->sample_ns = (uint64_t)m->period * AI64_LA_NS_PER_CYCLE;
	m->spb = AI64_LA_WORDS * AI64_LA_PER_WORD(m->lane);
	m->block_ns = m->spb * m->sample_ns;
	m->bps = AI64_LA_SLOT * 1e9 / (double)m->block_ns;
	m->span = AI64_LA_SLOTS * (double)m->block_ns / 1e9;
	m->live = m->bps <= LA_LIVE_BPS;
	return 0;
}

int la_get_mode(struct la_mode *m, char *err, size_t errlen)
{
	const struct ai64_core *c = core_find(PRU0);
	uint32_t period, key;
	size_t i;

	if (map_all(err, errlen)) {
		return -1;
	}
	if (!c || !rproc_running(c->rproc, NULL, 0) || cfg->run != AI64_LA_RUN_MAGIC) {
		snprintf(err, errlen, "ICSSG1 PRU0 is not running the logic analyzer "
			 "(ai64-demo --icssg1 la starts it)");
		return -1;
	}
	period = cfg->period;
	key = cfg->key;
	for (i = 0; i < la_nrates && la_rates[i].period != period; i++) {
	}
	if (i == la_nrates || la_mode_of(m, chans_of(cfg->mask), la_rates[i].hz) ||
	    m->lane != cfg->lane) {
		snprintf(err, errlen, "PRU0 runs a setup this ai64 does not know: an older ai64?");
		return -1;
	}
	m->key = key;
	return 0;
}

int la_set_mode(uint32_t chans, uint32_t hz, char *err, size_t errlen)
{
	const struct ai64_core *c = core_find(PRU0);
	struct la_mode m;
	uint32_t key;
	double give_up;

	if (map_all(err, errlen)) {
		return -1;
	}
	if (la_mode_of(&m, chans, hz)) {
		snprintf(err, errlen, "no channels, or not one of the rates");
		return -1;
	}
	if (!c) {
		snprintf(err, errlen, "no core %s", PRU0);
		return -1;
	}
	if (rproc_running(c->rproc, NULL, 0) && rproc_write(c->rproc, "state", "stop")) {
		snprintf(err, errlen, "could not stop ICSSG1 PRU0 (root?)");
		return -1;
	}
	/* A key of its own, so no block of an earlier run reads as one of this one. */
	if (getrandom(&key, sizeof(key), 0) != sizeof(key)) {
		key = (uint32_t)now_us() * 2654435761u;
	}
	cfg->run = 0;
	cfg->mask = m.gpi;
	cfg->lane = m.lane;
	cfg->period = m.period;
	cfg->key = key;
	__sync_synchronize();
	cfg->magic = AI64_LA_CFG_MAGIC;
	__sync_synchronize();
	if (rproc_write(c->rproc, "state", "start")) {
		snprintf(err, errlen, "could not start ICSSG1 PRU0");
		return -1;
	}
	give_up = now_us() + 500000;
	while (cfg->run != AI64_LA_RUN_MAGIC) {
		if (now_us() > give_up) {
			snprintf(err, errlen, "ICSSG1 PRU0 did not start sampling");
			return -1;
		}
		usleep(1000);
	}
	return 0;
}

int la_parse_rate(const char *s, uint32_t *hz)
{
	char *end;
	double v = strtod(s, &end);
	size_t i;

	if (end == s || v <= 0) {
		return -1;
	}
	if (*end == 'k' || *end == 'K') {
		v *= 1e3;
	} else if (*end == 'g' || *end == 'G') {
		v *= 1e9;
	} else if (*end == 'm' || *end == 'M' || v < 1000) {
		v *= 1e6; /* MS/s, said or not */
	}
	for (i = 0; i < la_nrates; i++) {
		if (v > la_rates[i].hz * 0.999 && v < la_rates[i].hz * 1.001) {
			*hz = la_rates[i].hz;
			return 0;
		}
	}
	return -1;
}

const char *la_rate_name(uint32_t hz, char *buf, size_t len)
{
	snprintf(buf, len, "%g MS/s", hz / 1e6);
	return buf;
}

/* ---- the pump: one a process, which every reader shares ---- */

/* How long after its time a block is surely in DDR: XFR2VBUS's write, and the SoC holding DDR. */
#define SETTLE_NS 100000u
/* Slots a reader keeps clear of the pump, which writes over the copy a lap on. */
#define GUARD (AI64_LA_SLOTS / 8u)

static struct {
	pthread_mutex_t lock;
	int refs;
	uint32_t key;
	uint64_t t0, block_ns;
	uint64_t pumped;    /* blocks copied, from block 0; atomic */
	uint64_t first;     /* the first block it copied */
	uint32_t *copy;
	volatile int stop;
	pthread_t th;
} P = { .lock = PTHREAD_MUTEX_INITIALIZER };

static uint64_t due(uint64_t t0, uint64_t block_ns)
{
	uint64_t now = la_now(), settle = SETTLE_NS + block_ns;

	return now > t0 + settle ? (now - t0 - settle) / block_ns : 0;
}

/* 64-byte loads: about twice as fast as words from an uncached mapping. */
static void read_ring(void *dst, const void *src, size_t len)
{
	size_t at;

	for (at = 0; at < len; at += 64u) {
		const uint8_t *s = (const uint8_t *)src + at;
		uint8_t *d = (uint8_t *)dst + at;
#ifdef __aarch64__
		__asm__ volatile("ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n\t"
				 "st1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%1]"
				 : : "r"(s), "r"(d) : "v0", "v1", "v2", "v3", "memory");
#else
		memcpy(d, s, 64);
#endif
	}
}

static void *pump(void *arg)
{
	uint64_t at = __atomic_load_n(&P.pumped, __ATOMIC_ACQUIRE);

	(void)arg;
	while (!P.stop) {
		uint64_t to = due(P.t0, P.block_ns), n;
		uint32_t slot;

		if (to <= at) {
			usleep(100);
			continue;
		}
		if (to - at > AI64_LA_SLOTS - GUARD) {
			/* A ring behind: on from near the newest; what it skips is a hole. */
			at = to - 4096u;
		}
		slot = (uint32_t)(at % AI64_LA_SLOTS);
		n = to - at;
		if (n > AI64_LA_SLOTS - slot) {
			n = AI64_LA_SLOTS - slot;
		}
		if (n > 4096u) {
			n = 4096u;
		}
		read_ring(P.copy + slot * (AI64_LA_SLOT / 4u),
			    (const void *)(ring + slot * (AI64_LA_SLOT / 4u)), n * AI64_LA_SLOT);
		at += n;
		__atomic_store_n(&P.pumped, at, __ATOMIC_RELEASE);
	}
	return NULL;
}

/* The pump for run key, started if none runs; -1 if one for another run still does. */
static int pump_get(const struct la_mode *m, uint64_t t0)
{
	int ret = 0;

	pthread_mutex_lock(&P.lock);
	if (P.refs && P.key != m->key) {
		ret = -1;
	} else if (!P.refs) {
		uint64_t to = due(t0, m->block_ns);

		if (!P.copy) {
			P.copy = aligned_alloc(64, AI64_LA_RING_SIZE);
		}
		P.key = m->key;
		P.t0 = t0;
		P.block_ns = m->block_ns;
		/* As much of the ring as there is of this run, short of where the
		 * pump would count itself a ring behind by the time it starts. */
		P.first = to > AI64_LA_SLOTS - 2u * GUARD ? to - (AI64_LA_SLOTS - 2u * GUARD) : 0;
		P.pumped = P.first;
		P.stop = 0;
		if (!P.copy || pthread_create(&P.th, NULL, pump, NULL)) {
			ret = -2;
		}
	}
	if (!ret) {
		P.refs++;
	}
	pthread_mutex_unlock(&P.lock);
	return ret;
}

static void pump_put(void)
{
	pthread_mutex_lock(&P.lock);
	if (--P.refs == 0) {
		P.stop = 1;
		pthread_join(P.th, NULL);
	}
	pthread_mutex_unlock(&P.lock);
}

/* ---- a reader ---- */

int la_src_open(struct la_src *s, double back, char *err, size_t errlen)
{
	uint64_t now, to, want;
	uint32_t lo, hi;
	int b, v, tries;

	memset(s, 0, sizeof(*s));
	if (la_get_mode(&s->mode, err, errlen)) {
		return -1;
	}
	do {
		lo = cfg->t0;
		hi = cfg->t0_hi;
	} while (cfg->t0 != lo);
	s->t0 = (uint64_t)hi << 32 | lo;
	now = la_now();
	if (s->t0 > now) {
		snprintf(err, errlen, "PRU0's run starts after now: IEP0 was reset under it");
		return -1;
	}
	/* Another run's pump still going: its readers find their run stale and let it go. */
	for (tries = 0; (b = pump_get(&s->mode, s->t0)) == -1 && tries < 200; tries++) {
		usleep(5000);
	}
	if (b) {
		snprintf(err, errlen, b == -1 ? "the reader is still on the run before" : "out of memory");
		return -1;
	}
	s->copy = P.copy;
	to = due(s->t0, s->mode.block_ns);
	want = (uint64_t)(back * 1e9 / (double)s->mode.block_ns);
	s->start = to > want ? to - want : 0;
	if (s->start < P.first) {
		s->start = P.first;
	}
	s->next = s->start;
	for (b = 0; b < 3; b++) {
		for (v = 0; v < 256; v++) {
			s->lut[b][v] = (uint16_t)chans_of((uint32_t)v << (8 * b));
		}
	}
	s->now = s->t0 + s->start * s->mode.block_ns;
	s->open = 1;
	return 0;
}

int la_src_stale(const struct la_src *s)
{
	return cfg->key != s->mode.key || cfg->run != AI64_LA_RUN_MAGIC;
}

void la_src_close(struct la_src *s)
{
	if (s->open) {
		pump_put();
	}
	s->open = 0;
}

static inline uint32_t chans(const struct la_src *s, uint32_t g)
{
	return s->lut[0][g & 0xffu] | s->lut[1][g >> 8 & 0xffu] | s->lut[2][g >> 16 & 0xffu];
}

/* Sample k of a block, the GPI bits as PRU0 numbers them. */
static inline uint32_t sample(const uint32_t *w, uint32_t lane, int k)
{
	switch (lane) {
	case AI64_LA_LANE_ALL:
		return w[k];
	case AI64_LA_LANE_LO16:
		return k & 1 ? w[k / 2] >> 16 : w[k / 2] & 0xffffu;
	case AI64_LA_LANE_MID16:
		return (k & 1 ? w[k / 2] >> 16 : w[k / 2] & 0xffffu) << 8;
	default:
		return (w[k / 4] >> (8 * (k & 3)) & 0xffu) << AI64_LA_LANE_SHIFT(lane);
	}
}

/* cur in each of a word's places, as the lane packs it. */
static inline uint32_t replicate(uint32_t lane, uint32_t cur)
{
	switch (lane) {
	case AI64_LA_LANE_ALL:
		return cur;
	case AI64_LA_LANE_LO16:
	case AI64_LA_LANE_MID16:
		return (cur >> AI64_LA_LANE_SHIFT(lane) & 0xffffu) * 0x10001u;
	default:
		return (cur >> AI64_LA_LANE_SHIFT(lane) & 0xffu) * 0x01010101u;
	}
}

/* Blocks [from, to) out of the copy, as edges and holes. */
static void decode(struct la_src *s, uint64_t from, uint64_t to, la_edge_fn fn, la_hole_fn hfn,
		   void *ctx)
{
	const struct la_mode *m = &s->mode;
	uint32_t per = AI64_LA_PER_WORD(m->lane);
	uint64_t b;

	for (b = from; b < to; b++) {
		const uint32_t *w = s->copy + (b % AI64_LA_SLOTS) * (AI64_LA_SLOT / 4u);
		uint64_t tb = s->t0 + b * m->block_ns;
		uint32_t r;
		int k = 0, wi;

		if (w[AI64_LA_WORDS] != m->key + (uint32_t)b) {
			/* Dropped by PRU0, or skipped by the pump: the levels are unknown. */
			s->lost++;
			continue;
		}
		if (!s->have) {
			s->cur = sample(w, m->lane, 0);
			fn(ctx, tb, chans(s, s->cur));
			s->have = 1;
			k = 1;
		} else if (s->last + m->sample_ns < tb) {
			/* After a hole: it ends at this block's first sample. */
			s->cur = sample(w, m->lane, 0);
			if (hfn) {
				hfn(ctx, s->last + m->sample_ns, tb, chans(s, s->cur));
			} else {
				fn(ctx, tb, chans(s, s->cur));
			}
			k = 1;
		}
		/* A word holding cur in every place has no change: on to the next. */
		r = replicate(m->lane, s->cur);
		for (wi = k / (int)per; wi < (int)AI64_LA_WORDS; wi++) {
			if (w[wi] == r && k <= wi * (int)per) {
				continue;
			}
			for (k = k > wi * (int)per ? k : wi * (int)per; k < (wi + 1) * (int)per; k++) {
				uint32_t v = sample(w, m->lane, k);

				if (v != s->cur) {
					fn(ctx, tb + (uint64_t)k * m->sample_ns, chans(s, v));
					s->cur = v;
					s->edges++;
				}
			}
			r = replicate(m->lane, s->cur);
		}
		s->last = tb + (m->spb - 1u) * m->sample_ns;
		s->blocks++;
	}
}

void la_src_poll(struct la_src *s, la_edge_fn fn, la_hole_fn hfn, void *ctx, uint64_t max_ns)
{
	uint64_t to = __atomic_load_n(&P.pumped, __ATOMIC_ACQUIRE);
	uint64_t max = max_ns / s->mode.block_ns + 1u;

	if (to - s->next > AI64_LA_SLOTS - GUARD && to > s->next) {
		/* A ring behind the pump, which has written over the copy: on from
		 * near it. What it skips is lost too, and a hole. */
		s->lost += to - 4096u - s->next;
		s->next = to - 4096u;
		s->behind++;
	}
	if (to > s->next + max) {
		to = s->next + max;
	}
	if (to > s->next) {
		decode(s, s->next, to, fn, hfn, ctx);
		s->next = to;
	}
	s->now = s->t0 + s->next * s->mode.block_ns;
}

int la_src_freeze(uint64_t *stopped, char *err, size_t errlen)
{
	const struct ai64_core *c = core_find(PRU0);

	if (!c || rproc_write(c->rproc, "state", "stop")) {
		snprintf(err, errlen, "could not stop ICSSG1 PRU0 (root?)");
		return -1;
	}
	*stopped = la_now();
	return 0;
}

int la_src_reread(struct la_src *s, uint64_t stopped, uint64_t from, uint64_t to, la_edge_fn fn,
		  la_hole_fn hfn, void *ctx)
{
	uint64_t b0, b1, b, newest;

	if (from < s->t0 || to <= from || stopped < s->t0) {
		return -1;
	}
	b0 = (from - s->t0) / s->mode.block_ns;
	b1 = (to - s->t0) / s->mode.block_ns + 1u;
	/* The newest block PRU0 can have written before it stopped; the ring
	 * holds a lap back from it, less what it wrote while it stopped. */
	newest = (stopped - s->t0) / s->mode.block_ns;
	if (b1 + 2u > newest || newest - b0 > AI64_LA_SLOTS - 1024u) {
		return -1;
	}
	/* Copied again over the pump's copy: the ring holds still now, so the
	 * pump, if it copies the same slots, copies the same. */
	for (b = b0; b < b1;) {
		uint32_t slot = (uint32_t)(b % AI64_LA_SLOTS);
		uint64_t n = b1 - b;

		if (n > AI64_LA_SLOTS - slot) {
			n = AI64_LA_SLOTS - slot;
		}
		read_ring(s->copy + slot * (AI64_LA_SLOT / 4u),
			  (const void *)(ring + slot * (AI64_LA_SLOT / 4u)), n * AI64_LA_SLOT);
		b += n;
	}
	s->have = 0;
	s->blocks = s->lost = s->edges = 0;
	decode(s, b0, b1, fn, hfn, ctx);
	return 0;
}
