/* SPDX-License-Identifier: MIT */
/*
 * ai64 la [seconds|once|lines]
 *   The logic analyzer on ICSSG1 (pru/logic_analyzer/), the la window:
 *   every channel's level, edges, frequency, and duty over the last
 *   interval (1 s), and each PRU's totals. lines prints one line every
 *   10 s, for the merged console.
 *
 * ai64 la capture <seconds> <file.vcd|->
 *   Records all 19 channels for that long and writes a VCD file, for
 *   PulseView or GTKWave, in nanoseconds from the start.
 *
 * ai64 la quiet
 *   Before ICSSG1's cores stop: stops IEP0, zeroes its count, and maps no
 *   event to a task in any of the six task managers, so no new task
 *   starts and each core stops outside one (pru/logic_analyzer/firmware/
 *   la_tm.h has why). ai64-demo runs it between firmware sets.
 *
 * Each PRU's RTU keeps a stream in DDR (ai64_la.h): edge records stamped
 * with IEP0's low 32 bits, and a tick every millisecond with IEP0's high
 * bits and the GTC. A reader here follows one stream, unwraps each edge's
 * time against the stream's last tick, and hands the edges on; the two
 * streams are merged by time. A reader starts at a stream's first tick,
 * which also says every pin's state then.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ai64.h"
#include "ai64_la.h"

#define LA_DT "/proc/device-tree/reserved-memory/ai64-la@ac000000"

static const struct {
	const char *pin;
	const char *ball;
	uint32_t pad;
	int pru;
	int bit;
} chans[AI64_LA_NCHAN] = {
#define X(c, pin, ball, pad, pru, bit) [c] = { pin, ball, pad, pru, bit },
	AI64_LA_CHANNELS(X)
#undef X
};

/* What the demo's jumpers bring to each channel (pru/logic_analyzer/README.md). */
static const struct {
	const char *name;
	const char *from;
} wired[AI64_LA_NCHAN] = {
	[3] = { "SPI7_CS", "P9.28" },
	[4] = { "SPI7_CLK", "P9.31" },
	[5] = { "SPI7_MOSI", "P9.30" },
	[6] = { "SQUARE", "P8.18" },
	[7] = { "S0_A", "P8.11" },
	[8] = { "S0_B", "P8.12" },
	[9] = { "S1_A", "P9.12" },
	[10] = { "S1_B", "P9.15" },
	[11] = { "ECAP", "P8.15" },
	[12] = { "EHRPWM4", "P9.25" },
	[13] = { "S0_A", "P8.21" },
};

static volatile struct ai64_la *la;
static const volatile uint32_t *rings;

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
};

typedef void (*edge_fn)(void *ctx, struct stream *st, uint64_t t, uint32_t state);

static int la_map(void)
{
	int s;

	if (access(LA_DT, F_OK)) {
		fprintf(stderr, "%s is missing: the overlay is older than the logic analyzer.\n"
			"Run install-beagle.sh and reboot.\n", LA_DT);
		return -1;
	}
	la = map_phys(AI64_LA_ADDR, 4096);
	rings = map_phys(AI64_LA_RES_ADDR, AI64_LA_RES_SIZE);
	if (!la || !rings) {
		return -1;
	}
	for (s = 0; s < 2; s++) {
		if (la->s[s].magic != AI64_LA_MAGIC) {
			fprintf(stderr, "ICSSG1 RTU%d is not running the logic analyzer "
				"(ai64-demo --icssg1 la starts it)\n", s);
			return -1;
		}
	}
	return 0;
}

static void stream_init(struct stream *st, int s)
{
	memset(st, 0, sizeof(*st));
	st->s = s;
	st->h = &la->s[s];
	st->ring = rings + (AI64_LA_RING_ADDR(s) - AI64_LA_RES_ADDR) / 4u;
	st->rd = st->h->wr;
}

static inline uint32_t ring_word(const struct stream *st, uint32_t at)
{
	return st->ring[(at & (AI64_LA_RING_SIZE - 1u)) / 4u];
}

/*
 * Every record the RTU has written since the last call. The RTU updates wr
 * after a tick's second record, so a tick never shows up half written.
 */
static void stream_poll(struct stream *st, edge_fn fn, void *ctx)
{
	uint32_t wr = st->h->wr;

	if (wr - st->rd > AI64_LA_RING_SIZE - 65536u) {
		st->missed += (wr - st->rd) / AI64_LA_REC_SIZE;
		st->rd = wr;
		st->ticked = 0;
	}
	while (wr - st->rd >= AI64_LA_REC_SIZE) {
		uint32_t t = ring_word(st, st->rd), v = ring_word(st, st->rd + 4u);
		uint64_t at = st->base + (uint64_t)(int64_t)(int32_t)(t - (uint32_t)st->base);

		switch (AI64_LA_TYPE(v)) {
		case AI64_LA_TICK: {
			uint32_t hi = ring_word(st, st->rd + 12u);

			st->base = (uint64_t)(hi & 0xffffu) << 32 | t;
			if (!st->ticked) {
				st->ticked = 1;
				st->state = v & 0xffffffu;
				if (!st->first) {
					st->first = st->base;
					st->first_state = st->state;
				}
			}
			st->rd += 2u * AI64_LA_REC_SIZE;
			continue;
		}
		case AI64_LA_GAP:
			st->gaps++;
			break;
		default:
			if (st->ticked) {
				st->state = v;
				st->edges++;
				fn(ctx, st, at, v);
			}
			break;
		}
		st->rd += AI64_LA_REC_SIZE;
	}
}

/* Until both streams have ticked; -1 if one does not within 2 s. */
static int wait_ticks(struct stream st[2], edge_fn fn, void *ctx)
{
	double give_up = now_us() + 2e6;
	int s;

	while (!st[0].ticked || !st[1].ticked) {
		for (s = 0; s < 2; s++) {
			stream_poll(&st[s], fn, ctx);
		}
		if (now_us() > give_up) {
			fprintf(stderr, "ICSSG1 RTU%d's stream has no tick: is its RTU running?\n",
				st[0].ticked ? 1 : 0);
			return -1;
		}
		usleep(2000);
	}
	return 0;
}

static inline int level(const uint32_t state[2], int c)
{
	return (int)(state[chans[c].pru] >> chans[c].bit & 1u);
}

/* ---- the la window ---- */

struct meter {
	uint32_t state[2];
	uint64_t last[AI64_LA_NCHAN];  /* the newest change, or the interval's start */
	uint64_t high[AI64_LA_NCHAN];  /* ns high this interval */
	uint32_t edges[AI64_LA_NCHAN];
	uint32_t rises[AI64_LA_NCHAN];
	int started[2];
};

static void meter_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
{
	struct meter *m = ctx;
	uint32_t was = m->state[st->s];
	int c;

	if (!m->started[st->s]) {
		m->started[st->s] = 1;
		was = st->first_state;
	}
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		uint32_t b = 1u << chans[c].bit;

		if (chans[c].pru != st->s || !((was ^ state) & b)) {
			continue;
		}
		if (was & b) {
			m->high[c] += t > m->last[c] ? t - m->last[c] : 0;
		} else {
			m->rises[c]++;
		}
		m->last[c] = t;
		m->edges[c]++;
	}
	m->state[st->s] = state;
}

static void fmt_hz(char *buf, size_t len, double hz)
{
	if (hz >= 1e6) {
		snprintf(buf, len, "%7.3f MHz", hz / 1e6);
	} else if (hz >= 1e3) {
		snprintf(buf, len, "%7.3f kHz", hz / 1e3);
	} else {
		snprintf(buf, len, "%7.1f Hz ", hz);
	}
}

static int view_main(int argc, char **argv)
{
	int once = argc > 2 && !strcmp(argv[2], "once");
	int lines = argc > 2 && !strcmp(argv[2], "lines");
	int every = argc > 2 && !once && !lines ? atoi(argv[2]) : (lines ? 10 : 1);
	struct stream st[2];
	struct meter m;
	uint64_t t0 = 0, prev_edges[2] = { 0, 0 };
	int s, c;

	if (argc > 3 || every < 1) {
		fprintf(stderr, "usage: %s la [seconds|once|lines] | capture <seconds> <file.vcd|->\n",
			argv[0]);
		return 2;
	}
	if (la_map()) {
		return 1;
	}
	memset(&m, 0, sizeof(m));
	for (s = 0; s < 2; s++) {
		stream_init(&st[s], s);
	}
	if (!once) {
		printf("la window ready: ICSSG1's 19 channels every %d s\n", every);
	}
	/* Start where both streams have ticked, so every pin's state is known. */
	if (wait_ticks(st, meter_edge, &m)) {
		return 1;
	}
	/* The edges one stream passed on while waiting for the other count for nothing. */
	t0 = st[0].base > st[1].base ? st[0].base : st[1].base;
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		m.last[c] = t0;
		m.high[c] = 0;
		m.edges[c] = 0;
		m.rises[c] = 0;
	}
	for (s = 0; s < 2; s++) {
		m.state[s] = st[s].state;
		m.started[s] = 1;
		prev_edges[s] = st[s].edges;
	}

	for (;;) {
		double end = now_us() + every * 1e6;
		uint64_t t1, span;

		while (now_us() < end) {
			for (s = 0; s < 2; s++) {
				stream_poll(&st[s], meter_edge, &m);
			}
			usleep(5000);
		}
		t1 = st[0].base < st[1].base ? st[0].base : st[1].base;
		span = t1 > t0 ? t1 - t0 : 1;
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if (level(m.state, c)) {
				m.high[c] += t1 > m.last[c] ? t1 - m.last[c] : 0;
			}
		}
		if (lines) {
			printf("la: PRU0 %.0f edges/s, PRU1 %.0f edges/s; gaps %llu, missed %llu\n",
			       (st[0].edges - prev_edges[0]) * 1e9 / span,
			       (st[1].edges - prev_edges[1]) * 1e9 / span,
			       (unsigned long long)(st[0].gaps + st[1].gaps),
			       (unsigned long long)(st[0].missed + st[1].missed));
		} else {
			printf("\033[2J\033[H");
			printf("ICSSG1 logic analyzer, the last %.3f s; IEP0 at %.3f s\n", span / 1e9,
			       t1 / 1e9);
			for (s = 0; s < 2; s++) {
				printf("  PRU%d %9.0f edges/s, %llu in all; RTU%d gaps %u (%u records), "
				       "reader missed %llu\n",
				       s, (st[s].edges - prev_edges[s]) * 1e9 / span,
				       (unsigned long long)st[s].edges, s, st[s].h->gaps, st[s].h->lost,
				       (unsigned long long)st[s].missed);
			}
			printf("\n  ch  pin    PRU/GPI  wired to          level  edges/s     frequency   duty\n");
			for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
				char hz[24], from[24];

				fmt_hz(hz, sizeof(hz), m.rises[c] * 1e9 / span);
				snprintf(from, sizeof(from), "%s%s%s", wired[c].name ? wired[c].name : "-",
					 wired[c].from ? " " : "", wired[c].from ? wired[c].from : "");
				printf("  %2d  %-5s  %d/%-2d     %-16s  %-5s  %9.0f  %s  %5.1f%%\n", c,
				       chans[c].pin, chans[c].pru, chans[c].bit, from,
				       level(m.state, c) ? "high" : "low", m.edges[c] * 1e9 / span, hz,
				       100.0 * m.high[c] / span);
			}
		}
		fflush(stdout);
		if (once) {
			return 0;
		}
		for (s = 0; s < 2; s++) {
			prev_edges[s] = st[s].edges;
		}
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			m.last[c] = t1;
			m.high[c] = 0;
			m.edges[c] = 0;
			m.rises[c] = 0;
		}
		t0 = t1;
	}
}

/* ---- capture ---- */

struct event {
	uint64_t t;
	uint32_t state;
};

struct events {
	struct event *e;
	size_t n, cap;
};

static void keep_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
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

static void vcd_name(char *buf, size_t len, int c)
{
	char pin[8];
	char *p;

	snprintf(pin, sizeof(pin), "%s", chans[c].pin);
	for (p = pin; *p; p++) {
		if (*p == '.') {
			*p = '_';
		}
	}
	snprintf(buf, len, "ch%d_%s%s%s", c, pin, wired[c].name ? "_" : "",
		 wired[c].name ? wired[c].name : "");
}

static int capture_main(int argc, char **argv)
{
	struct stream st[2];
	struct events ev[2] = { { 0 } };
	uint32_t state[2];
	uint64_t begin, finish;
	size_t i[2] = { 0, 0 };
	double secs, start;
	FILE *f;
	time_t now;
	int s, c;

	if (argc != 5 || (secs = atof(argv[3])) <= 0) {
		fprintf(stderr, "usage: %s la capture <seconds> <file.vcd|->\n", argv[0]);
		return 2;
	}
	if (la_map()) {
		return 1;
	}
	for (s = 0; s < 2; s++) {
		stream_init(&st[s], s);
	}
	if (wait_ticks(st, keep_edge, ev)) {
		return 1;
	}
	start = now_us();
	while (now_us() - start < secs * 1e6) {
		for (s = 0; s < 2; s++) {
			stream_poll(&st[s], keep_edge, ev);
		}
		usleep(2000);
	}
	for (s = 0; s < 2; s++) {
		stream_poll(&st[s], keep_edge, ev);
		if (st[s].missed || st[s].gaps) {
			fprintf(stderr, "PRU%d: %llu records missed here, %llu gaps in the stream: "
				"the capture has holes\n", s, (unsigned long long)st[s].missed,
				(unsigned long long)st[s].gaps);
		}
	}
	/* The span both streams cover, from the later first tick to the earlier last one. */
	begin = st[0].first > st[1].first ? st[0].first : st[1].first;
	finish = st[0].base < st[1].base ? st[0].base : st[1].base;

	f = strcmp(argv[4], "-") ? fopen(argv[4], "w") : stdout;
	if (!f) {
		perror(argv[4]);
		return 1;
	}
	now = time(NULL);
	fprintf(f, "$date %s$end\n$version ai64 la, ICSSG1 IEP0 at %llu ns $end\n"
		"$timescale 1 ns $end\n$scope module la $end\n", ctime(&now),
		(unsigned long long)begin);
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		char name[48];

		vcd_name(name, sizeof(name), c);
		fprintf(f, "$var wire 1 %c %s $end\n", '!' + c, name);
	}
	fprintf(f, "$upscope $end\n$enddefinitions $end\n");

	/* Each stream's state at begin: its first tick's, then its edges before begin. */
	for (s = 0; s < 2; s++) {
		state[s] = st[s].first_state;
		while (i[s] < ev[s].n && ev[s].e[i[s]].t <= begin) {
			state[s] = ev[s].e[i[s]++].state;
		}
	}
	fprintf(f, "#0\n$dumpvars\n");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		fprintf(f, "%d%c\n", level(state, c), '!' + c);
	}
	fprintf(f, "$end\n");

	for (;;) {
		uint32_t was[2] = { state[0], state[1] };
		int a = i[0] < ev[0].n && ev[0].e[i[0]].t <= finish;
		int b = i[1] < ev[1].n && ev[1].e[i[1]].t <= finish;
		uint64_t t;

		if (!a && !b) {
			break;
		}
		s = a && (!b || ev[0].e[i[0]].t <= ev[1].e[i[1]].t) ? 0 : 1;
		t = ev[s].e[i[s]].t;
		state[s] = ev[s].e[i[s]++].state;
		fprintf(f, "#%llu\n", (unsigned long long)(t - begin));
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if (level(state, c) != level(was, c)) {
				fprintf(f, "%d%c\n", level(state, c), '!' + c);
			}
		}
	}
	fprintf(f, "#%llu\n", (unsigned long long)(finish - begin));
	if (f != stdout) {
		fclose(f);
	}
	fprintf(stderr, "%.6f s of 19 channels: PRU0 %zu edges, PRU1 %zu edges%s%s\n",
		(finish - begin) / 1e9, i[0], i[1], f != stdout ? ", in " : "",
		f != stdout ? argv[4] : "");
	free(ev[0].e);
	free(ev[1].e);
	return 0;
}

/* ICSSG1's task managers and IEP0, at their SoC addresses (pru/common/icssg.h). */
#define ICSSG1_TM   0x0b12a000
#define ICSSG1_IEP0 0x0b12e000

static int quiet_main(void)
{
	volatile uint32_t *tm = map_phys(ICSSG1_TM, 4096);
	volatile uint32_t *iep = map_phys(ICSSG1_IEP0, 4096);
	int i;

	if (!tm || !iep) {
		return 1;
	}
	iep[0] &= ~1u;               /* GLOBAL_CFG: IEP0 stops */
	iep[0x70 / 4] = 0;           /* CMP_CFG: no compare */
	iep[0x74 / 4] = 0xffffffffu; /* CMP_STATUS */
	iep[0x10 / 4] = 0;           /* COUNT: from 0, as after a reset, which the */
	iep[0x14 / 4] = 0;           /* scheduler's CMP0 reset needs to ever match */
	for (i = 0; i < 6; i++) {
		tm[0x100 / 4 * i + 0x40 / 4] = 0; /* TS2_GEN_CFG1: no event starts a task */
	}
	usleep(1000);
	return 0;
}

int la_main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[2], "quiet")) {
		return quiet_main();
	}
	if (argc > 2 && !strcmp(argv[2], "capture")) {
		return capture_main(argc, argv);
	}
	return view_main(argc, argv);
}
