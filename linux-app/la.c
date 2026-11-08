/* SPDX-License-Identifier: MIT */
/*
 * ai64 la [seconds|once|lines]
 *   The logic analyzer on ICSSG1 (pru/logic_analyzer/), the la window:
 *   every channel's level, edges, frequency, and duty over the last
 *   interval (1 s), and each PRU's totals. lines prints one line every
 *   10 s, for the merged console.
 *
 * ai64 la capture <seconds> <file.vcd|-> [--trigger]
 *   Records all 19 channels for that long and writes a VCD file, for
 *   PulseView or GTKWave, in nanoseconds from the start. With --trigger,
 *   waits for the next trigger hit and records around it, a tenth of the
 *   time before it, read back from the ring, and the rest after.
 *
 * ai64 la trigger off | <ch> rise|fall|any | <ch>=0|1 ...
 *   Sets the trigger the Tx_PRUs match (ai64_la.h): an edge on a channel,
 *   or the channels coming to a pattern. A trigger takes the channels of
 *   one PRU.
 *
 * ai64 la measure [seconds] [a:b ...]
 *   Records for that long (1 s) and prints each channel's edges, frequency,
 *   and the spread of its periods and high times; then, for each pair a:b
 *   (the demo's A/B pairs and the two PRUs' copy of one signal if none is
 *   given), the time from each change of a to b's nearest one.
 *
 * ai64 la rate [seconds]
 *   How fast the analyzer goes, end to end: each PRU's records a second as
 *   its RTU copies them to DDR, and as a reader here takes them out and
 *   does nothing else with them, with any it missed, and the gaps it met
 *   (the RTU's, or raw mode's converter's); and the closest two records came, which is the PRU's time to write one when its pins keep
 *   changing. Run against a fast test signal, it says which stage keeps up
 *   (pru/logic_analyzer/README.md, tests/board/stress.sh).
 *
 * ai64 la mode [edge [ch ...] | raw <ch> ...]
 *   Each PRU's mode and the channels it records; or sets them. Edge mode
 *   (all 19 channels, or those given) writes a record at each change, and
 *   can be up to 48 ns late after one. Raw mode takes channels of one PRU:
 *   it samples them every 8 ns with no gap, and the other PRU finds the
 *   changes, so its own channels are not recorded
 *   (pru/logic_analyzer/firmware/la_map.h). Channels not recorded read
 *   low. Restarts both PRUs, a gap of some 50 ms.
 *
 * ai64 la mark <value>
 *   Puts a mark with that 16-bit value in the analyzer's stream, through a
 *   fabric route to ICSSG1 RTU0 (ai64 fabric route g1rtu0 mark <value>);
 *   any fabric node can send one. A capture shows the marks as a 16-bit
 *   signal, mark, each value held until the next.
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
#include "la.h"

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
		uint32_t b = 1u << la_chans[c].bit;

		if (la_chans[c].pru != st->s || !((was ^ state) & b)) {
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
	if (la_map(1)) {
		return 1;
	}
	memset(&m, 0, sizeof(m));
	for (s = 0; s < 2; s++) {
		if (stream_init(&st[s], s)) {
			return 1;
		}
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
			printf("la: PRU0 %.0f edges/s, PRU1 %.0f edges/s; gaps %llu, missed %llu, late %llu\n",
			       (st[0].edges - prev_edges[0]) * 1e9 / span,
			       (st[1].edges - prev_edges[1]) * 1e9 / span,
			       (unsigned long long)(st[0].gaps + st[1].gaps),
			       (unsigned long long)(st[0].missed + st[1].missed),
			       (unsigned long long)(st[0].late + st[1].late));
		} else {
			printf("\033[2J\033[H");
			printf("ICSSG1 logic analyzer, the last %.3f s; IEP0 at %.3f s\n", span / 1e9,
			       t1 / 1e9);
			for (s = 0; s < 2; s++) {
				printf("  PRU%d %9.0f edges/s, %llu in all; RTU%d gaps %u (%u records), "
				       "reader missed %llu, late %llu, marks %llu, trigger hits %llu\n",
				       s, (st[s].edges - prev_edges[s]) * 1e9 / span,
				       (unsigned long long)st[s].edges, s, st[s].h->gaps, st[s].h->lost,
				       (unsigned long long)st[s].missed, (unsigned long long)st[s].late,
				       (unsigned long long)st[s].marks, (unsigned long long)st[s].trigs);
			}
			printf("\n  ch  pin    PRU/GPI  wired to          level  edges/s     frequency   duty\n");
			for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
				char hz[24], from[24];

				fmt_hz(hz, sizeof(hz), m.rises[c] * 1e9 / span);
				snprintf(from, sizeof(from), "%s%s%s", la_wired[c].name ? la_wired[c].name : "-",
					 la_wired[c].from ? " " : "", la_wired[c].from ? la_wired[c].from : "");
				printf("  %2d  %-5s  %d/%-2d     %-16s  %-5s  %9.0f  %s  %5.1f%%\n", c,
				       la_chans[c].pin, la_chans[c].pru, la_chans[c].bit, from,
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
 * The recording r as a VCD file on f: every channel, the marks as a 16-bit
 * signal, and trig changing at each trigger hit. A hole, where the stream
 * lost records, takes its PRU's channels to x, unknown, until its end,
 * which gives them all again. Reads r to its end; nedge gets each PRU's
 * edges, and the return is the marks and hits.
 */
size_t rec_write_vcd(struct recording *r, FILE *f, size_t nedge[2])
{
	struct mark *mk;
	size_t nmk, im = 0;
	int ntrig = 0, unknown[2] = { 0, 0 };
	size_t ih[2] = { 0, 0 };
	uint32_t was[2];
	uint64_t t;
	time_t now = time(NULL);
	int c, s;

	nedge[0] = nedge[1] = 0;
	fprintf(f, "$date %s$end\n$version ai64 la, ICSSG1 IEP0 at %llu ns $end\n"
		"$timescale 1 ns $end\n$scope module la $end\n", ctime(&now),
		(unsigned long long)r->begin);
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		char name[48];

		vcd_name(name, sizeof(name), c);
		fprintf(f, "$var wire 1 %c %s $end\n", '!' + c, name);
	}
	/* The marks (ai64 la mark): each holds its value until the next; trig
	 * changes at each trigger hit. */
	fprintf(f, "$var wire 16 ~ mark $end\n$var wire 1 } trig $end\n");
	fprintf(f, "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		fprintf(f, "%d%c\n", level(r->state, c), '!' + c);
	}
	fprintf(f, "b0 ~\n0}\n$end\n");
	nmk = rec_marks(r, &mk);
	for (;;) {
		int edge = rec_next(r, &t, was);

		/* The marks and holes up to this edge, or all that are left after the
		 * last, in time order. */
		for (;;) {
			uint64_t th = UINT64_MAX;
			int hs = -1;

			for (s = 0; s < 2; s++) {
				const struct holes *h = &r->holes[s];

				while (ih[s] < h->n && h->b[ih[s]] <= r->begin) {
					ih[s]++;
				}
				if (ih[s] < h->n && h->a[ih[s]] <= r->finish) {
					uint64_t a = h->a[ih[s]] > r->begin ? h->a[ih[s]] : r->begin;

					if (a < th) {
						th = a;
						hs = s;
					}
				}
			}
			if (hs >= 0 && (!edge || th <= t) && (im >= nmk || th <= mk[im].t)) {
				fprintf(f, "#%llu\n$comment a hole in PRU%d's stream: records lost $end\n",
					(unsigned long long)(th - r->begin), hs);
				for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
					if (la_chans[c].pru == hs) {
						fprintf(f, "x%c\n", '!' + c);
					}
				}
				unknown[hs] = 1;
				ih[hs]++;
				continue;
			}
			if (im >= nmk || (edge && mk[im].t > t)) {
				break;
			}
			if (mk[im].trig) {
				ntrig++;
				fprintf(f, "#%llu\n$comment trigger hit%s $end\n%d}\n",
					(unsigned long long)(mk[im].t - r->begin),
					mk[im].t == r->hit ? ", the one the capture is around" : "",
					ntrig & 1);
				im++;
				continue;
			}
			fprintf(f, "#%llu\n$comment mark %u from fabric node %u $end\nb",
				(unsigned long long)(mk[im].t - r->begin), mk[im].value, mk[im].from);
			for (c = 15; c >= 0; c--) {
				fputc('0' + (mk[im].value >> c & 1u), f);
			}
			fprintf(f, " ~\n");
			im++;
		}
		if (!edge) {
			break;
		}
		s = edge - 1;
		if (r->state[s] == was[s] && !unknown[s]) {
			continue;
		}
		fprintf(f, "#%llu\n", (unsigned long long)(t - r->begin));
		if (r->state[s] != was[s]) {
			nedge[s]++;
		}
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if (level(r->state, c) != level(was, c) || (unknown[s] && la_chans[c].pru == s)) {
				fprintf(f, "%d%c\n", level(r->state, c), '!' + c);
			}
		}
		unknown[s] = 0;
	}
	fprintf(f, "#%llu\n", (unsigned long long)(r->finish - r->begin));
	free(mk);
	return nmk;
}

static int capture_main(int argc, char **argv)
{
	static struct recording r;
	size_t nedge[2], nmk;
	double secs;
	FILE *f;

	if ((argc != 5 && (argc != 6 || strcmp(argv[5], "--trigger"))) ||
	    (secs = atof(argv[3])) <= 0) {
		fprintf(stderr, "usage: %s la capture <seconds> <file.vcd|-> [--trigger]\n", argv[0]);
		return 2;
	}
	if (record(&r, secs, argc == 6)) {
		return 1;
	}
	f = strcmp(argv[4], "-") ? fopen(argv[4], "w") : stdout;
	if (!f) {
		perror(argv[4]);
		return 1;
	}
	nmk = rec_write_vcd(&r, f, nedge);
	if (f != stdout) {
		fclose(f);
	}
	fprintf(stderr, "%.6f s of 19 channels%s: PRU0 %zu edges, PRU1 %zu edges, %zu marks "
		"and trigger hits%s%s\n", (r.finish - r.begin) / 1e9,
		r.hit ? ", from a tenth before the trigger hit" : "", nedge[0], nedge[1], nmk,
		f != stdout ? ", in " : "", f != stdout ? argv[4] : "");
	rec_free(&r);
	return 0;
}

/* ---- measure ---- */

/* A spread as "min med max", or a dash for none. */
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
	static const int demo_pairs[][2] = { { 7, 8 }, { 8, 7 }, { 9, 10 }, { 10, 9 }, { 7, 13 } };
	struct changes ch[AI64_LA_NCHAN];
	int pairs[16][2], npairs = 0;
	uint64_t span;
	double secs = 1;
	int64_t *v;
	size_t vcap = 0;
	int c, k, s, argi = 3;

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
	if (record(&r, secs, 0)) {
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

	printf("ICSSG1 logic analyzer, %.6f s measured; times in ns\n", span / 1e9);
	for (s = 0; s < 2; s++) {
		uint64_t ns;
		size_t n = rec_holes(&r, s, &ns);

		if (n) {
			printf("PRU%d: %zu holes, %.6f s in all, where records were lost: no period or "
			       "time below spans one\n", s, n, ns / 1e9);
		}
	}
	printf("\n  ch  pin    wired to          edges   frequency    period  min      med      max"
	       "    high  min      med      max    duty\n");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		struct chan_stats cs;
		char hz[24], per[40], high[40], from[24];

		chan_stats(&ch[c], level(r.state, c), r.begin, r.finish, v, &cs,
			   &r.holes[la_chans[c].pru]);
		spread(per, sizeof(per), &cs.period);
		spread(high, sizeof(high), &cs.high);
		/* With holes, the median period's: rises over the span would count them. */
		fmt_hz(hz, sizeof(hz), cs.period.n && rec_holes(&r, la_chans[c].pru, &(uint64_t){ 0 }) ?
		       1e9 / (double)cs.period.med : cs.rises * 1e9 / span);
		snprintf(from, sizeof(from), "%s%s%s", la_wired[c].name ? la_wired[c].name : "-",
			 la_wired[c].from ? " " : "", la_wired[c].from ? la_wired[c].from : "");
		printf("  %2d  %-5s  %-16s %7zu  %s  %s  %s  %5.1f%%\n", c, la_chans[c].pin, from,
		       ch[c].n, hz, per, high, cs.known_ns ? 100.0 * cs.high_ns / cs.known_ns : 0.0);
	}

	printf("\n  from each change of a, to the nearest change of b       a rises:  min      med      max"
	       "      a falls:  min      med      max\n");
	for (k = 0; k < npairs; k++) {
		int a = pairs[k][0], b = pairs[k][1];
		const struct holes *ha = &r.holes[la_chans[a].pru], *hb = &r.holes[la_chans[b].pru];
		char up[40], down[40], label[48];
		struct spread sp;

		spread_of(v, nearest(&ch[a], &ch[b], 1, v, ha, hb), &sp);
		spread(up, sizeof(up), &sp);
		spread_of(v, nearest(&ch[a], &ch[b], 0, v, ha, hb), &sp);
		spread(down, sizeof(down), &sp);
		snprintf(label, sizeof(label), "ch%d %s -> ch%d %s", a, chan_name(a), b, chan_name(b));
		printf("  %-54s %s        %s\n", label, up, down);
	}
	free(v);
	changes_free(ch);
	rec_free(&r);
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

/*
 * ai64 la trigger off | <ch> rise|fall|any | <ch>=0|1 ...: the trigger, on
 * the PRU those channels belong to, set in that stream's header for its
 * RTU to pass to the Tx_PRU; the other PRU's goes off.
 */
static int trigger_main(int argc, char **argv)
{
	uint32_t mode = AI64_LA_TRIG_OFF, mask = 0, value = 0;
	int pru = -1, i;

	if (argc < 4) {
		goto usage;
	}
	if (strcmp(argv[3], "off")) {
		int c;
		char edge[8];

		if (argc == 5 && sscanf(argv[3], "%d", &c) == 1 && !strchr(argv[3], '=')) {
			snprintf(edge, sizeof(edge), "%s", argv[4]);
			if (c < 0 || c >= (int)AI64_LA_NCHAN) {
				goto usage;
			}
			pru = la_chans[c].pru;
			mask = 1u << la_chans[c].bit;
			if (!strcmp(edge, "rise")) {
				mode = AI64_LA_TRIG_ENTER;
				value = mask;
			} else if (!strcmp(edge, "fall")) {
				mode = AI64_LA_TRIG_ENTER;
			} else if (!strcmp(edge, "any")) {
				mode = AI64_LA_TRIG_ANY;
			} else {
				goto usage;
			}
		} else {
			mode = AI64_LA_TRIG_ENTER;
			for (i = 3; i < argc; i++) {
				int lv;

				if (sscanf(argv[i], "%d=%d", &c, &lv) != 2 || c < 0 ||
				    c >= (int)AI64_LA_NCHAN || (lv != 0 && lv != 1)) {
					goto usage;
				}
				if (pru >= 0 && la_chans[c].pru != pru) {
					fprintf(stderr, "ch%d is on PRU%d, the others on PRU%d: a trigger "
						"takes one PRU's channels (pru/logic_analyzer/README.md)\n",
						c, la_chans[c].pru, pru);
					return 2;
				}
				pru = la_chans[c].pru;
				mask |= 1u << la_chans[c].bit;
				value |= (uint32_t)lv << la_chans[c].bit;
			}
		}
	} else if (argc != 4) {
		goto usage;
	}
	if (la_set_trigger(pru, mode, mask, value)) {
		return 1;
	}
	if (mode == AI64_LA_TRIG_OFF) {
		printf("trigger off\n");
	} else {
		printf("trigger on PRU%d: %s, pins mask 0x%05x value 0x%05x; Tx_PRU%d matches it\n",
		       pru, mode == AI64_LA_TRIG_ANY ? "any change" : "the pins come to value",
		       mask, value, pru);
	}
	return 0;
usage:
	fprintf(stderr, "usage: %s la trigger off | <ch> rise|fall|any | <ch>=0|1 ...\n", argv[0]);
	return 2;
}

/* A mark in the analyzer's stream: a fabric route from Linux to ICSSG1 RTU0. */
/* Each stream's last edge, and the closest two edges came. */
struct closest {
	uint64_t last[2];
	uint64_t min[2];
};

static void count_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
{
	struct closest *c = ctx;

	(void)state;
	if (c) {
		if (c->last[st->s] && t - c->last[st->s] < c->min[st->s]) {
			c->min[st->s] = t - c->last[st->s];
		}
		c->last[st->s] = t;
	}
}

static int rate_main(int argc, char **argv)
{
	double secs = argc > 3 ? atof(argv[3]) : 1.0, t0, t1;
	struct closest c = { { 0, 0 }, { UINT64_MAX, UINT64_MAX } };
	struct stream st[2];
	uint32_t copied0[2];
	uint64_t read0[2], missed0[2], gaps0[2], late0[2];
	int s;

	if (argc > 4 || secs <= 0) {
		fprintf(stderr, "usage: %s la rate [seconds]\n", argv[0]);
		return 2;
	}
	if (la_map(1)) {
		return 1;
	}
	for (s = 0; s < 2; s++) {
		if (stream_init(&st[s], s)) {
			return 1;
		}
	}
	if (wait_ticks(st, count_edge, NULL)) {
		return 1;
	}
	t0 = now_us();
	for (s = 0; s < 2; s++) {
		copied0[s] = st[s].h->edges;
		read0[s] = st[s].edges;
		missed0[s] = st[s].missed;
		gaps0[s] = st[s].gaps;
		late0[s] = st[s].late;
	}
	do {
		for (s = 0; s < 2; s++) {
			stream_poll(&st[s], count_edge, &c);
		}
	} while (now_us() < t0 + secs * 1e6);
	t1 = now_us();
	for (s = 0; s < 2; s++) {
		printf("PRU%d: RTU%d copied %.0f records/s, the reader read %.0f/s and missed %.0f/s; "
		       "RTU gaps %u (%u records) since it started; gaps in the stream %llu; "
		       "closest records %lld ns apart; late %.0f/s\n",
		       s, s, (uint32_t)(st[s].h->edges - copied0[s]) * 1e6 / (t1 - t0),
		       (st[s].edges - read0[s]) * 1e6 / (t1 - t0),
		       (st[s].missed - missed0[s]) * 1e6 / (t1 - t0), st[s].h->gaps, st[s].h->lost,
		       (unsigned long long)(st[s].gaps - gaps0[s]),
		       c.min[s] == UINT64_MAX ? -1LL : (long long)c.min[s],
		       (st[s].late - late0[s]) * 1e6 / (t1 - t0));
		stream_free(&st[s]);
	}
	return 0;
}

static void print_modes(void)
{
	int s, c;

	for (s = 0; s < 2; s++) {
		const volatile struct ai64_la_stream *h = &la_hdr->s[s];

		printf("PRU%d: %s", s,
		       AI64_LA_MODE_OF(h->cfg_mode) == AI64_LA_MODE_CONV ?
		       "converting the other's samples, no channels" :
		       AI64_LA_MODE_OF(h->cfg_mode) != AI64_LA_MODE_RAW ?
		       "edge, a record at each change, channels" :
		       AI64_LA_LANE_OF(h->cfg_mode) == AI64_LA_LANE_ALL ?
		       "raw, every 8 ns with no gap, a sample a word, channels" :
		       "raw, every 8 ns with no gap, two samples a word, channels");
		for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
			if (la_chans[c].pru == s && (h->cfg_mask >> la_chans[c].bit & 1u)) {
				printf(" %d", c);
			}
		}
		printf("\n");
	}
}

static int mode_main(int argc, char **argv)
{
	uint32_t mask[2] = { 0, 0 };
	char err[128];
	int raw, i, c;

	if (la_map(1)) {
		return 1;
	}
	if (argc == 3) {
		print_modes();
		return 0;
	}
	if (strcmp(argv[3], "edge") && strcmp(argv[3], "raw")) {
		fprintf(stderr, "usage: %s la mode [edge [ch ...] | raw <ch> ...]\n", argv[0]);
		return 2;
	}
	for (i = 4; i < argc; i++) {
		c = atoi(argv[i]);
		if (c < 0 || c >= (int)AI64_LA_NCHAN || strspn(argv[i], "0123456789") != strlen(argv[i])) {
			fprintf(stderr, "no channel %s\n", argv[i]);
			return 2;
		}
		mask[la_chans[c].pru] |= 1u << la_chans[c].bit;
	}
	if (!strcmp(argv[3], "edge")) {
		raw = -1;
		if (!mask[0] && !mask[1]) {
			mask[0] = AI64_LA_MASK(0);
			mask[1] = AI64_LA_MASK(1);
		}
	} else if (mask[0] && mask[1]) {
		fprintf(stderr, "raw mode takes channels of one PRU: PRU0 has 0 3 4 5 6 13 14 15 16 18, "
			"PRU1 1 2 7 8 9 10 11 12 17\n");
		return 2;
	} else if (!mask[0] && !mask[1]) {
		fprintf(stderr, "raw mode needs its channels\n");
		return 2;
	} else {
		raw = mask[0] ? 0 : 1;
	}
	if (la_set_mode(raw, mask, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	print_modes();
	return 0;
}

static int mark_main(int argc, char **argv)
{
	char *av[] = { argv[0], "fabric", "route", "g1rtu0", "mark", argv[3], NULL };

	if (argc != 4) {
		fprintf(stderr, "usage: %s la mark <value>\n", argv[0]);
		return 2;
	}
	return fabric_main(6, av);
}

int la_main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[2], "quiet")) {
		return quiet_main();
	}
	if (argc > 2 && !strcmp(argv[2], "capture")) {
		return capture_main(argc, argv);
	}
	if (argc > 2 && !strcmp(argv[2], "trigger")) {
		return trigger_main(argc, argv);
	}
	if (argc > 2 && !strcmp(argv[2], "mark")) {
		return mark_main(argc, argv);
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
