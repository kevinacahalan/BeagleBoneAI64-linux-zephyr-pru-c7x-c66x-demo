/* SPDX-License-Identifier: MIT */
/*
 * ai64 la [seconds|once|lines]
 *   The logic analyzer on ICSSG1 (pru/logic_analyzer/), the la window:
 *   every channel's level, edges, frequency, and duty over the last
 *   interval (1 s), and the reader's totals. lines prints one line every
 *   10 s, for the merged console.
 *
 * ai64 la mode [<rate> [<ch> ... | all]]
 *   The rate and the channels PRU0 samples, what that writes to the ring
 *   a second, and how long the ring holds; or sets them: a rate from the
 *   menu (125, 62.5, 25, 12.5, 6.25, 2.5, 1.25 MS/s) and the channels, all
 *   10 if none are given. Restarts PRU0, a gap of some 30 ms.
 *
 * ai64 la capture <seconds> <file.vcd|-> [<ch> rise|fall|any | <ch>=0|1 ...]
 *         [pre <seconds>] [wait <seconds>]
 *   Records every channel for that long and writes a VCD file, for
 *   PulseView or GTKWave, in nanoseconds from the start. With a trigger,
 *   an edge on a channel or the channels coming to a pattern, waits for
 *   its next hit (30 s at most, or wait) and records from pre (a tenth of
 *   the time, unless given) before it, out of the ring, to seconds after.
 *
 * ai64 la measure [seconds] [a:b ...]
 *   Records for that long (1 s) and prints each channel's edges, frequency,
 *   and the spread of its periods and high times; then, for each pair a:b
 *   (the demo's SPI7 if none is given), the time from each change of a to
 *   b's nearest one.
 *
 * ai64 la rate [seconds]
 *   How fast the reader here keeps up: PRU0's blocks a second, the blocks
 *   it read, those lost (dropped by PRU0, or skipped by the reader), and
 *   the edges it found.
 *
 * ai64 la quiet
 *   Before ICSSG1's cores stop: stops IEP0, zeroes its count, and maps no
 *   event to a task in any of the six task managers, so no new task
 *   starts and each core stops outside one (pru/logic_analyzer/firmware/
 *   la_tm.h has why). ai64-demo runs it between firmware sets.
 *
 * PRU0 samples every channel at once, at the rate set, with no gap, and
 * writes the samples to a ring in DDR (ai64_la.h). The reader here
 * (la_src.c) finds the changes, and a recording (la_rec.c) keeps them.
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
#include "la.h"

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

static void print_mode(const struct la_mode *m)
{
	char rate[24];
	int c;

	printf("%s, channels", la_rate_name(m->hz, rate, sizeof(rate)));
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (m->chans >> c & 1u) {
			printf(" %d", c);
		}
	}
	printf("; %u samples a word, %.1f MB/s into the ring, which holds %.3f s; %s\n",
	       AI64_LA_PER_WORD(m->lane), m->bps / 1e6, m->span,
	       m->live ? "the reader keeps up" : "more than the reader keeps up with live: holes");
}

/* ---- the la window ---- */

struct meter {
	uint32_t state;
	int have;
	uint64_t last[AI64_LA_NCHAN];  /* the newest change, or the interval's start */
	uint64_t high[AI64_LA_NCHAN];  /* ns high this interval */
	uint32_t edges[AI64_LA_NCHAN];
	uint32_t rises[AI64_LA_NCHAN];
};

static void meter_edge(void *ctx, uint64_t t, uint32_t state)
{
	struct meter *m = ctx;
	uint32_t x = m->have ? m->state ^ state : 0;
	int c;

	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (!(x >> c & 1u)) {
			continue;
		}
		if (level(m->state, c)) {
			m->high[c] += t > m->last[c] ? t - m->last[c] : 0;
		} else {
			m->rises[c]++;
		}
		m->last[c] = t;
		m->edges[c]++;
	}
	m->state = state;
	m->have = 1;
}

static void meter_hole(void *ctx, uint64_t from, uint64_t to, uint32_t state)
{
	struct meter *m = ctx;
	int c;

	(void)from;
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		m->last[c] = to;
	}
	m->state = state;
}

static int view_main(int argc, char **argv)
{
	int once = argc > 2 && !strcmp(argv[2], "once");
	int lines = argc > 2 && !strcmp(argv[2], "lines");
	int every = argc > 2 && !once && !lines ? atoi(argv[2]) : (lines ? 10 : 1);
	struct la_src src;
	struct meter m;
	uint64_t t0, blocks0 = 0, lost0 = 0, edges0 = 0;
	char err[160], rate[24];
	int c;

	if (argc > 3 || every < 1) {
		fprintf(stderr, "usage: %s la [seconds|once|lines]\n", argv[0]);
		return 2;
	}
	if (!once) {
		printf("la window ready: ICSSG1 PRU0's 10 channels every %d s\n", every);
	}
	for (;;) {
		while (la_src_open(&src, 0, err, sizeof(err))) {
			if (once) {
				fprintf(stderr, "%s\n", err);
				return 1;
			}
			printf("la: %s\n", err);
			fflush(stdout);
			sleep(5);
		}
		memset(&m, 0, sizeof(m));
		t0 = src.now;
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			m.last[c] = t0;
		}
		blocks0 = lost0 = edges0 = 0;
		while (!la_src_stale(&src)) {
			double end = now_us() + every * 1e6;
			uint64_t t1, span;

			while (now_us() < end) {
				la_src_poll(&src, meter_edge, meter_hole, &m, 100000000u);
				usleep(5000);
			}
			t1 = src.now;
			span = t1 > t0 ? t1 - t0 : 1;
			for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
				if (level(m.state, c)) {
					m.high[c] += t1 > m.last[c] ? t1 - m.last[c] : 0;
				}
			}
			if (lines) {
				printf("la: %s, %.0f edges/s; %llu blocks lost\n",
				       la_rate_name(src.mode.hz, rate, sizeof(rate)),
				       (src.edges - edges0) * 1e9 / span, (unsigned long long)src.lost);
			} else {
				printf("\033[2J\033[H");
				printf("ICSSG1 logic analyzer, the last %.3f s; IEP0 at %.3f s\n  ", span / 1e9,
				       t1 / 1e9);
				print_mode(&src.mode);
				printf("  read %.0f blocks/s, %.0f edges/s; lost %llu blocks this interval, "
				       "%llu in all\n", (src.blocks - blocks0) * 1e9 / span,
				       (src.edges - edges0) * 1e9 / span,
				       (unsigned long long)(src.lost - lost0), (unsigned long long)src.lost);
				printf("\n  ch  pin    GPI  wired to          level  edges/s     frequency   duty\n");
				for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
					char hz[24], from[24];
					int on = src.mode.chans >> c & 1u;

					fmt_hz(hz, sizeof(hz), m.rises[c] * 1e9 / span);
					snprintf(from, sizeof(from), "%s%s%s",
						 la_wired[c].name ? la_wired[c].name : "-",
						 la_wired[c].from ? " " : "", la_wired[c].from ? la_wired[c].from : "");
					if (!on) {
						printf("  %2d  %-5s  %-3d  %-16s  not recorded\n", c, la_chans[c].pin,
						       la_chans[c].bit, from);
						continue;
					}
					printf("  %2d  %-5s  %-3d  %-16s  %-5s  %9.0f  %s  %5.1f%%\n", c,
					       la_chans[c].pin, la_chans[c].bit, from,
					       level(m.state, c) ? "high" : "low", m.edges[c] * 1e9 / span, hz,
					       100.0 * m.high[c] / span);
				}
			}
			fflush(stdout);
			if (once) {
				la_src_close(&src);
				return 0;
			}
			blocks0 = src.blocks;
			lost0 = src.lost;
			edges0 = src.edges;
			for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
				m.last[c] = t1;
				m.high[c] = 0;
				m.edges[c] = 0;
				m.rises[c] = 0;
			}
			t0 = t1;
		}
		la_src_close(&src);
		if (!lines) {
			printf("la: the setup changed; following the new one\n");
		}
	}
}

/* ---- capture ---- */

static void vcd_name(char *buf, size_t len, int c)
{
	char pin[8];
	char *p;

	snprintf(pin, sizeof(pin), "%s", la_chans[c].pin);
	for (p = pin; *p; p++) {
		if (*p == '.') {
			*p = '_';
		}
	}
	snprintf(buf, len, "ch%d_%s%s%s", c, pin, la_wired[c].name ? "_" : "",
		 la_wired[c].name ? la_wired[c].name : "");
}

/*
 * The recording r as a VCD file on f: the channels recorded, and trig,
 * which goes high at the trigger hit. A hole, where samples were lost,
 * takes every channel to x, unknown, until its end, which gives them all
 * again. Returns the edges.
 */
size_t rec_write_vcd(struct recording *r, FILE *f)
{
	time_t now = time(NULL);
	size_t ih = 0, nedge = 0;
	int unknown = 0, hit = !r->hit;
	uint32_t was;
	uint64_t t;
	char rate[24];
	int c;

	fprintf(f, "$date %s$end\n$version ai64 la, %s, ICSSG1 IEP0 at %llu ns $end\n"
		"$timescale 1 ns $end\n$scope module la $end\n", ctime(&now),
		la_rate_name(r->mode.hz, rate, sizeof(rate)), (unsigned long long)r->begin);
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		char name[48];

		if (r->mode.chans >> c & 1u) {
			vcd_name(name, sizeof(name), c);
			fprintf(f, "$var wire 1 %c %s $end\n", '!' + c, name);
		}
	}
	if (r->hit) {
		fprintf(f, "$var wire 1 } trig $end\n");
	}
	fprintf(f, "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n");
	rec_rewind(r);
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (r->mode.chans >> c & 1u) {
			fprintf(f, "%d%c\n", level(r->state, c), '!' + c);
		}
	}
	if (r->hit) {
		fprintf(f, "0}\n");
	}
	fprintf(f, "$end\n");
	for (;;) {
		int edge = rec_next(r, &t, &was);

		/* The holes, and the hit, up to this edge, or all that are left after the last. */
		for (;;) {
			while (ih < r->holes.n && r->holes.b[ih] <= r->begin) {
				ih++;
			}
			if (!hit && (!edge || r->hit <= t) &&
			    (ih >= r->holes.n || r->hit <= r->holes.a[ih])) {
				fprintf(f, "#%llu\n$comment the trigger hit $end\n1}\n",
					(unsigned long long)(r->hit - r->begin));
				hit = 1;
				continue;
			}
			if (ih < r->holes.n && r->holes.a[ih] <= r->finish && (!edge || r->holes.a[ih] <= t)) {
				uint64_t a = r->holes.a[ih] > r->begin ? r->holes.a[ih] : r->begin;

				fprintf(f, "#%llu\n$comment a hole: samples lost $end\n",
					(unsigned long long)(a - r->begin));
				for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
					if (r->mode.chans >> c & 1u) {
						fprintf(f, "x%c\n", '!' + c);
					}
				}
				unknown = 1;
				ih++;
				continue;
			}
			break;
		}
		if (!edge) {
			break;
		}
		if (r->state == was && !unknown) {
			continue;
		}
		fprintf(f, "#%llu\n", (unsigned long long)(t - r->begin));
		if (r->state != was) {
			nedge++;
		}
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if ((r->mode.chans >> c & 1u) && (level(r->state, c) != level(was, c) || unknown)) {
				fprintf(f, "%d%c\n", level(r->state, c), '!' + c);
			}
		}
		unknown = 0;
	}
	fprintf(f, "#%llu\n", (unsigned long long)(r->finish - r->begin));
	return nedge;
}

/*
 * A trigger from argv[i...]: <ch> rise|fall|any, or <ch>=0|1 ...; the
 * arguments it took, 0 if none is there, -1 if it is wrong.
 */
static int parse_trigger(int argc, char **argv, int i, struct la_trig *g)
{
	int c, lv, n = 0;

	memset(g, 0, sizeof(*g));
	if (i + 1 < argc && strspn(argv[i], "0123456789") == strlen(argv[i]) &&
	    (!strcmp(argv[i + 1], "rise") || !strcmp(argv[i + 1], "fall") ||
	     !strcmp(argv[i + 1], "any"))) {
		c = atoi(argv[i]);
		if (c >= (int)AI64_LA_NCHAN) {
			return -1;
		}
		g->mask = 1u << c;
		g->kind = argv[i + 1][0] == 'a' ? LA_TRIG_ANY : LA_TRIG_ENTER;
		g->value = argv[i + 1][0] == 'r' ? g->mask : 0;
		return 2;
	}
	while (i + n < argc && sscanf(argv[i + n], "%d=%d", &c, &lv) == 2) {
		if (c < 0 || c >= (int)AI64_LA_NCHAN || (lv != 0 && lv != 1)) {
			return -1;
		}
		g->kind = LA_TRIG_ENTER;
		g->mask |= 1u << c;
		g->value |= (uint32_t)lv << c;
		n++;
	}
	return n;
}

static int capture_main(int argc, char **argv)
{
	static struct recording r;
	struct la_trig g = { 0 };
	double secs, pre = -1, wait = 30;
	char err[160], rate[24];
	size_t nedge;
	FILE *f;
	int i = 5, n;

	if (argc < 5 || (secs = atof(argv[3])) <= 0) {
		goto usage;
	}
	n = parse_trigger(argc, argv, i, &g);
	if (n < 0) {
		goto usage;
	}
	for (i += n; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "pre")) {
			pre = atof(argv[i + 1]);
		} else if (!strcmp(argv[i], "wait")) {
			wait = atof(argv[i + 1]);
		} else {
			goto usage;
		}
	}
	if (i != argc) {
		goto usage;
	}
	if (pre < 0) {
		pre = secs / 10;
	}
	if (record(&r, secs, &g, pre, wait, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	f = strcmp(argv[4], "-") ? fopen(argv[4], "w") : stdout;
	if (!f) {
		perror(argv[4]);
		rec_free(&r);
		return 1;
	}
	nedge = rec_write_vcd(&r, f);
	if (f != stdout) {
		fclose(f);
	}
	fprintf(stderr, "%.6f s at %s%s: %zu edges, %zu holes%s%s\n", (r.finish - r.begin) / 1e9,
		la_rate_name(r.mode.hz, rate, sizeof(rate)), r.hit ? ", around the trigger hit" : "",
		nedge, r.holes.n, f != stdout ? ", in " : "", f != stdout ? argv[4] : "");
	rec_free(&r);
	return 0;
usage:
	fprintf(stderr, "usage: %s la capture <seconds> <file.vcd|-> [<ch> rise|fall|any | <ch>=0|1 ...] "
		"[pre <seconds>] [wait <seconds>]\n", argv[0]);
	return 2;
}

/* ---- measure ---- */

static void spread(char *buf, size_t len, const struct spread *sp)
{
	if (!sp->n) {
		snprintf(buf, len, "%26s", "-");
		return;
	}
	snprintf(buf, len, "%8lld %8lld %8lld", (long long)sp->min, (long long)sp->med,
		 (long long)sp->max);
}

static const char *chan_name(int c)
{
	return la_wired[c].name ? la_wired[c].name : la_chans[c].pin;
}

static int measure_main(int argc, char **argv)
{
	static struct recording r;
	/* The demo's SPI7: chip select to clock, clock to data. */
	static const int demo_pairs[][2] = { { 1, 2 }, { 2, 3 } };
	struct changes ch[AI64_LA_NCHAN];
	int pairs[16][2], npairs = 0;
	uint64_t span, hole_ns;
	double secs = 1;
	char err[160];
	int64_t *v;
	size_t vcap = 0, nholes;
	int c, k, argi = 3;

	if (argc > 3 && strchr(argv[3], ':') == NULL) {
		secs = atof(argv[3]);
		argi = 4;
	}
	for (; argi < argc && npairs < 16; argi++) {
		if (sscanf(argv[argi], "%d:%d", &pairs[npairs][0], &pairs[npairs][1]) != 2 ||
		    pairs[npairs][0] < 0 || pairs[npairs][0] >= (int)AI64_LA_NCHAN ||
		    pairs[npairs][1] < 0 || pairs[npairs][1] >= (int)AI64_LA_NCHAN) {
			secs = 0;
			break;
		}
		npairs++;
	}
	if (secs <= 0) {
		fprintf(stderr, "usage: %s la measure [seconds] [a:b ...]\n", argv[0]);
		return 2;
	}
	if (!npairs) {
		for (k = 0; k < (int)(sizeof(demo_pairs) / sizeof(demo_pairs[0])); k++) {
			pairs[npairs][0] = demo_pairs[k][0];
			pairs[npairs++][1] = demo_pairs[k][1];
		}
	}
	if (record(&r, secs, NULL, 0, 0, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	rec_changes(&r, ch);
	span = r.finish - r.begin;
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (ch[c].n > vcap) {
			vcap = ch[c].n;
		}
	}
	v = malloc((vcap + 1) * sizeof(*v));
	if (!v) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	printf("ICSSG1 logic analyzer, %.6f s measured; times in ns\n  ", span / 1e9);
	print_mode(&r.mode);
	nholes = rec_holes(&r, &hole_ns);
	if (nholes) {
		printf("%zu holes, %.6f s in all, where samples were lost: no period or time below "
		       "spans one\n", nholes, hole_ns / 1e9);
	}
	printf("\n  ch  pin    wired to          edges   frequency    period  min      med      max"
	       "    high  min      med      max    duty\n");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		struct chan_stats cs;
		char hz[24], per[40], high[40], from[24];

		snprintf(from, sizeof(from), "%s%s%s", la_wired[c].name ? la_wired[c].name : "-",
			 la_wired[c].from ? " " : "", la_wired[c].from ? la_wired[c].from : "");
		if (!(r.mode.chans >> c & 1u)) {
			printf("  %2d  %-5s  %-16s  not recorded\n", c, la_chans[c].pin, from);
			continue;
		}
		chan_stats(&ch[c], level(r.state0, c), r.begin, r.finish, v, &cs, &r.holes);
		spread(per, sizeof(per), &cs.period);
		spread(high, sizeof(high), &cs.high);
		/* With holes, the median period's: rises over the span would count them. */
		fmt_hz(hz, sizeof(hz), cs.period.n && nholes ? 1e9 / (double)cs.period.med :
		       cs.rises * 1e9 / span);
		printf("  %2d  %-5s  %-16s %7zu  %s  %s  %s  %5.1f%%\n", c, la_chans[c].pin, from,
		       ch[c].n, hz, per, high, cs.known_ns ? 100.0 * cs.high_ns / cs.known_ns : 0.0);
	}

	printf("\n  from each change of a, to the nearest change of b       a rises:  min      med      max"
	       "      a falls:  min      med      max\n");
	for (k = 0; k < npairs; k++) {
		int a = pairs[k][0], b = pairs[k][1];
		char up[40], down[40], label[48];
		struct spread sp;

		spread_of(v, nearest(&ch[a], &ch[b], 1, v, &r.holes), &sp);
		spread(up, sizeof(up), &sp);
		spread_of(v, nearest(&ch[a], &ch[b], 0, v, &r.holes), &sp);
		spread(down, sizeof(down), &sp);
		snprintf(label, sizeof(label), "ch%d %s -> ch%d %s", a, chan_name(a), b, chan_name(b));
		printf("  %-54s %s        %s\n", label, up, down);
	}
	free(v);
	changes_free(ch);
	rec_free(&r);
	return 0;
}

/* ---- the rest ---- */

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

/* Each edge's time, for the closest two. */
struct closest {
	uint64_t last, min;
	int have;
};

static void count_edge(void *ctx, uint64_t t, uint32_t state)
{
	struct closest *c = ctx;

	(void)state;
	if (c->have && t - c->last < c->min) {
		c->min = t - c->last;
	}
	c->last = t;
	c->have = 1;
}

static void count_hole(void *ctx, uint64_t from, uint64_t to, uint32_t state)
{
	struct closest *c = ctx;

	(void)from;
	(void)to;
	(void)state;
	c->have = 0;
}

static int rate_main(int argc, char **argv)
{
	double secs = argc > 3 ? atof(argv[3]) : 1.0, t0, t1;
	struct closest c = { 0, UINT64_MAX, 0 };
	struct la_src src;
	char err[160];

	if (argc > 4 || secs <= 0) {
		fprintf(stderr, "usage: %s la rate [seconds]\n", argv[0]);
		return 2;
	}
	if (la_src_open(&src, 0, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	t0 = now_us();
	do {
		la_src_poll(&src, count_edge, count_hole, &c, 100000000u);
		usleep(500);
	} while (now_us() < t0 + secs * 1e6);
	t1 = now_us();
	print_mode(&src.mode);
	printf("PRU0 wrote %.0f blocks/s (%.0f MS/s); the reader read %.0f blocks/s and found %.0f "
	       "edges/s; blocks lost %llu (dropped by PRU0, or skipped by the reader), times it fell a "
	       "ring behind %llu; closest edges %lld ns apart\n",
	       1e9 / (double)src.mode.block_ns, src.mode.hz / 1e6,
	       src.blocks * 1e6 / (t1 - t0), src.edges * 1e6 / (t1 - t0),
	       (unsigned long long)src.lost, (unsigned long long)src.behind,
	       c.min == UINT64_MAX ? -1LL : (long long)c.min);
	la_src_close(&src);
	return 0;
}

static int mode_main(int argc, char **argv)
{
	struct la_mode m;
	uint32_t hz, chans = 0;
	char err[160];
	int i, c;

	if (argc == 3) {
		if (la_get_mode(&m, err, sizeof(err))) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		print_mode(&m);
		return 0;
	}
	if (la_parse_rate(argv[3], &hz)) {
		fprintf(stderr, "usage: %s la mode [<rate> [<ch> ... | all]]: rate 125, 62.5, 25, 12.5, "
			"6.25, 2.5, or 1.25 (MS/s)\n", argv[0]);
		return 2;
	}
	for (i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "all")) {
			chans = (1u << AI64_LA_NCHAN) - 1u;
			continue;
		}
		c = atoi(argv[i]);
		if (c < 0 || c >= (int)AI64_LA_NCHAN || strspn(argv[i], "0123456789") != strlen(argv[i])) {
			fprintf(stderr, "no channel %s: 0 to %u\n", argv[i], AI64_LA_NCHAN - 1);
			return 2;
		}
		chans |= 1u << c;
	}
	if (!chans) {
		chans = (1u << AI64_LA_NCHAN) - 1u;
	}
	if (la_set_mode(chans, hz, err, sizeof(err)) || la_get_mode(&m, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	print_mode(&m);
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
	if (argc > 2 && !strcmp(argv[2], "mode")) {
		return mode_main(argc, argv);
	}
	if (argc > 2 && !strcmp(argv[2], "rate")) {
		return rate_main(argc, argv);
	}
	if (argc > 2 && !strcmp(argv[2], "measure")) {
		return measure_main(argc, argv);
	}
	return view_main(argc, argv);
}
