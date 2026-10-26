/* SPDX-License-Identifier: MIT */
/*
 * Linux and the pipeline (zephyr/include/ai64_pipe.h): P8.31 in, P9.25 out,
 * through ICSSG0, R5F0_0's UDMA, both C66x cores, and the C7x's MMA. Linux is not
 * in the path; it starts the pipeline, trains its layer, and reads its
 * numbers.
 *
 * ai64 pipe start [seconds]
 *   Has R5F0_0 pass windows on, logs the C7x's features for that many
 *   seconds (3), and trains the C7x's layer on them: the input steps on
 *   the GTC's time, so each window's tag gives its class. The weights are
 *   each class's mean features, scaled to one length (so the layer scores
 *   a window by how well it lines up with each class), as int8. Then it
 *   checks them on the same windows, in the layer's own int8 arithmetic,
 *   hands them to the C7x, and has R5F0_1 follow the classes on P9.25.
 *
 * ai64 pipe stop
 *   P9.25 goes back to its own steps, and R5F0_0 stops passing windows on.
 *
 * ai64 pipe [seconds|once|lines]
 *   Every few seconds (10): each stage's windows, latency from the
 *   window's last sample, and time spent, and R5F0_1's score. once also
 *   prints the confusion matrix; lines prints one line every 10 s, for
 *   the merged console.
 *
 * All of it reads and writes /dev/mem, so it needs root or CAP_SYS_RAWIO.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "ai64_pipe.h"

int pipe_main(int argc, char **argv);

#define RESERVED "/proc/device-tree/reserved-memory/ai64-fabric@a9000000"
/* As R5F0_1 keeps score (src/ai64_pipe_pwm.c). */
#define MARGIN_GTC (60u * (AI64_GTC_HZ / 1000000u))
#define TRAIN_MAX  16384u

static volatile struct ai64_pipe *g_pipe;

static const uint32_t step_hz[AI64_PWM_STEP_FREQS] = AI64_PWM_STEP_HZ;
static const uint32_t step_pct[AI64_PWM_STEPS] = AI64_PWM_STEP_PCT;

static const char *const stage_name[AI64_PIPE_STAGES] = {
	"ICSSG0 RTU1  stamps the window",
	"R5F0_0       UDMA into C66x L2",
	"C66x 0       FFT, even windows",
	"C66x 1       FFT, odd windows",
	"C7x          MMA layer, class",
	"R5F0_1       P9.25",
};

static int pipe_map(void)
{
	int fd;
	void *p;

	if (access(RESERVED, F_OK)) {
		fprintf(stderr, "%s is missing: the ai64-demo overlay is not applied.\n", RESERVED);
		return -1;
	}
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return -1;
	}
	p = mmap(NULL, AI64_PIPE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AI64_PIPE_ADDR);
	close(fd);
	if (p == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	g_pipe = p;
	return 0;
}

static const char *class_name(uint32_t c)
{
	static char buf[4][24];
	static int n;
	char *b = buf[n++ % 4];

	if (c >= AI64_PIPE_CLASSES) {
		return "none";
	}
	snprintf(b, sizeof(buf[0]), "%u kHz %u%%", step_hz[c / AI64_PWM_STEPS] / 1000u,
		 step_pct[c % AI64_PWM_STEPS]);
	return b;
}

static double mono_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double ticks_us(double t)
{
	return t * 1e6 / AI64_GTC_HZ;
}

/* Every stage has started (each writes its magic when it does). */
static int stages_up(void)
{
	int bad = 0;
	uint32_t s;

	for (s = 0; s < AI64_PIPE_STAGES; s++) {
		if (g_pipe->stage[s].magic != AI64_PIPE_MAGIC) {
			fprintf(stderr, "stage %s has not started: is ai64-demo running?\n",
				stage_name[s]);
			bad = 1;
		}
	}
	return bad ? -1 : 0;
}

/* The C7x's layer (ai64_c7x_mma.c): signed int8 out, no ReLU. */
static void layer_row(const int8_t *a, const int8_t (*b)[AI64_PIPE_FEATURES], uint32_t shift,
		      int32_t *acc, int8_t *y)
{
	uint32_t j, k;

	for (j = 0; j < AI64_PIPE_FEATURES; j++) {
		int32_t v = 0;

		for (k = 0; k < AI64_PIPE_FEATURES; k++) {
			v += a[k] * b[k][j];
		}
		if (acc) {
			acc[j] = v;
		}
		if (y) {
			if (shift) {
				v = (v + (1 << (shift - 1))) >> shift;
			}
			y[j] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
		}
	}
}

/* The class the C7x would pick: the first of the best. */
static uint32_t best_of(const int8_t *y)
{
	uint32_t c, best = 0;

	for (c = 1; c < AI64_PIPE_CLASSES; c++) {
		if (y[c] > y[best]) {
			best = c;
		}
	}
	return best;
}

static struct {
	int8_t f[AI64_PIPE_FEATURES];
	uint32_t cls;
} g_set[TRAIN_MAX];

/* Collect labelled windows from the C7x's log for secs seconds. */
static uint32_t collect(double secs, uint32_t *mixed, uint32_t *lost)
{
	uint32_t n = 0, at = g_pipe->log_n, now, i;
	double end = mono_s() + secs;

	*mixed = 0;
	*lost = 0;
	while (mono_s() < end) {
		usleep(20000);
		now = g_pipe->log_n;
		if (now - at > AI64_PIPE_LOG_N / 2u) {
			*lost += now - at - AI64_PIPE_LOG_N / 2u;
			at = now - AI64_PIPE_LOG_N / 2u;
		}
		for (; at != now; at++) {
			volatile struct ai64_pipe_log *e = &g_pipe->log[at % AI64_PIPE_LOG_N];
			uint64_t t_end = ((uint64_t)e->tag.t_end_hi << 32) | e->tag.t_end_lo;
			int32_t truth = ai64_pipe_truth(t_end, g_pipe->ctl.steps_ms, MARGIN_GTC);

			if (truth < 0) {
				(*mixed)++;
				continue;
			}
			if (n == TRAIN_MAX) {
				continue;
			}
			for (i = 0; i < AI64_PIPE_FEATURES; i++) {
				g_set[n].f[i] = e->f[i];
			}
			g_set[n].cls = (uint32_t)truth;
			n++;
		}
	}
	return n;
}

static int8_t g_w[AI64_PIPE_FEATURES][AI64_PIPE_FEATURES];

/* Weights from the windows: each class's mean, all scaled to one length. */
static int train(uint32_t n, uint32_t *shift_out, uint32_t *count)
{
	static double mean[AI64_PIPE_CLASSES][AI64_PIPE_FEATURES];
	double norm[AI64_PIPE_CLASSES], top = 0;
	int32_t acc[AI64_PIPE_FEATURES], amax = 1;
	uint32_t i, j, c, shift = 0;

	memset(mean, 0, sizeof(mean));
	memset(count, 0, AI64_PIPE_CLASSES * sizeof(*count));
	for (i = 0; i < n; i++) {
		count[g_set[i].cls]++;
		for (j = 0; j < AI64_PIPE_FEATURES; j++) {
			mean[g_set[i].cls][j] += g_set[i].f[j];
		}
	}
	for (c = 0; c < AI64_PIPE_CLASSES; c++) {
		if (count[c] == 0) {
			fprintf(stderr, "no clean window of %s: is ICSSG0 slice 1 stepping, with P9.15 "
				"jumpered to P8.31?\n", class_name(c));
			return -1;
		}
		norm[c] = 0;
		for (j = 0; j < AI64_PIPE_FEATURES; j++) {
			mean[c][j] /= count[c];
			norm[c] += mean[c][j] * mean[c][j];
		}
		norm[c] = sqrt(norm[c]);
		if (norm[c] == 0) {
			fprintf(stderr, "the windows of %s are all zero\n", class_name(c));
			return -1;
		}
		for (j = 0; j < AI64_PIPE_FEATURES; j++) {
			double w = fabs(mean[c][j] / norm[c]);

			top = w > top ? w : top;
		}
	}
	memset(g_w, 0, sizeof(g_w));
	for (c = 0; c < AI64_PIPE_CLASSES; c++) {
		for (j = 0; j < AI64_PIPE_FEATURES; j++) {
			g_w[j][c] = (int8_t)lround(127.0 * mean[c][j] / norm[c] / top);
		}
	}
	/* The shift that keeps the best score on these windows under 100. */
	for (i = 0; i < n; i++) {
		layer_row(g_set[i].f, g_w, 0, acc, NULL);
		for (c = 0; c < AI64_PIPE_CLASSES; c++) {
			int32_t v = acc[c] < 0 ? -acc[c] : acc[c];

			amax = v > amax ? v : amax;
		}
	}
	while ((amax >> shift) > 100) {
		shift++;
	}
	*shift_out = shift;
	return 0;
}

static int start_main(int argc, char **argv)
{
	double secs = argc > 3 ? atof(argv[3]) : 3.0;
	uint32_t n, mixed, lost, shift, right = 0, i, c;
	uint32_t count[AI64_PIPE_CLASSES];
	int8_t y[AI64_PIPE_FEATURES];

	if (secs <= 0.2 || secs > 30) {
		fprintf(stderr, "usage: %s pipe start [seconds, 0.2 to 30]\n", argv[0]);
		return 2;
	}
	if (stages_up()) {
		return 1;
	}
	g_pipe->ctl.follow = 0;
	g_pipe->ctl.steps_ms = AI64_PWM_DEFAULT_STEPS_MS;
	g_pipe->ctl.magic = AI64_PIPE_MAGIC;
	__sync_synchronize();
	g_pipe->ctl.run = 1;
	printf("pipeline: P8.31 -> ICSSG0 PRU1 and RTU1 -> R5F0_0's UDMA -> C66x 0 and 1's FFT -> "
	       "C7x's MMA -> R5F0_1 -> P9.25\n");
	printf("training: %.1f s of windows of P8.31, each labelled with the step the input was on\n",
	       secs);
	n = collect(secs, &mixed, &lost);
	if (n == 0) {
		fprintf(stderr, "no windows came: the C7x logged nothing (%u across a step)\n", mixed);
		return 1;
	}
	if (train(n, &shift, count)) {
		return 1;
	}
	for (i = 0; i < n; i++) {
		layer_row(g_set[i].f, g_w, shift, NULL, y);
		right += best_of(y) == g_set[i].cls;
	}
	{
		uint32_t lo = count[0], hi = count[0];

		for (c = 1; c < AI64_PIPE_CLASSES; c++) {
			lo = count[c] < lo ? count[c] : lo;
			hi = count[c] > hi ? count[c] : hi;
		}
		printf("  %u windows, %u to %u a class; %u across a step left out", n, lo, hi, mixed);
	}
	if (lost) {
		printf(", %u not read in time", lost);
	}
	printf("\n  the int8 layer, checked here on the same windows: %u of %u right (%.1f%%), "
	       "shift %u\n", right, n, 100.0 * right / n, shift);

	memcpy((void *)g_pipe->weights, g_w, sizeof(g_w));
	g_pipe->ctl.shift = shift;
	__sync_synchronize();
	g_pipe->ctl.weights_seq = g_pipe->ctl.weights_seq + 1u;
	__sync_synchronize();
	g_pipe->ctl.follow = 1;
	printf("the C7x has the weights; R5F0_1 sets P9.25 to each window's class (ai64 pipe shows "
	       "the score)\n");
	return 0;
}

static int stop_main(void)
{
	g_pipe->ctl.follow = 0;
	__sync_synchronize();
	usleep(50000);
	g_pipe->ctl.run = 0;
	printf("pipeline stopped: P9.25 is back on its own steps\n");
	return 0;
}

struct snap {
	struct ai64_pipe_stage st[AI64_PIPE_STAGES];
	uint32_t right, wrong, mixed, changes, cls, waits;
	double t;
};

static void take(struct snap *s)
{
	uint32_t i;

	for (i = 0; i < AI64_PIPE_STAGES; i++) {
		memcpy(&s->st[i], (const void *)&g_pipe->stage[i], sizeof(s->st[i]));
	}
	s->right = g_pipe->score.right;
	s->wrong = g_pipe->score.wrong;
	s->mixed = g_pipe->score.mixed;
	s->changes = g_pipe->score.changes;
	s->cls = g_pipe->score.cls;
	s->waits = g_pipe->score.waits;
	s->t = mono_s();
}

/*
 * A stage, or the score, that started its numbers again since the last
 * snapshot (new weights do that) counts from zero, not from the snapshot.
 */
static void rebase(struct snap *was, const struct snap *s)
{
	uint32_t i;

	for (i = 0; i < AI64_PIPE_STAGES; i++) {
		if (s->st[i].windows < was->st[i].windows) {
			memset(&was->st[i], 0, sizeof(was->st[i]));
		}
	}
	if (s->right < was->right || s->wrong < was->wrong || s->mixed < was->mixed) {
		was->right = was->wrong = was->mixed = 0;
	}
}

static void draw(const struct snap *s, const struct snap *was)
{
	double dt = s->t - was->t;
	uint32_t i, right = s->right - was->right, wrong = s->wrong - was->wrong;
	volatile struct ai64_pipe_result *r;

	printf("pipeline (%s, %s): %-28s %9s %21s %17s %5s %5s\n",
	       g_pipe->ctl.run ? "running" : "stopped",
	       g_pipe->ctl.follow ? "P9.25 follows" : "P9.25 steps on its own", "stage", "windows/s",
	       "latency mean/max us", "work mean/max us", "lost", "err");
	for (i = 0; i < AI64_PIPE_STAGES; i++) {
		const struct ai64_pipe_stage *a = &s->st[i], *b = &was->st[i];
		uint32_t n = a->windows - b->windows;

		printf("  %-30s %20.1f", stage_name[i], n / dt);
		if (n) {
			printf("   %8.1f / %8.1f   %7.1f / %7.1f", ticks_us((double)(a->lat_sum - b->lat_sum) / n),
			       ticks_us(a->lat_max), ticks_us((double)(a->work_sum - b->work_sum) / n),
			       ticks_us(a->work_max));
		} else {
			printf("   %21s %17s", "-", "-");
		}
		printf(" %5u %5u\n", a->dropped, a->errors);
	}
	r = &g_pipe->result[s->st[AI64_PIPE_MMA].last % AI64_PIPE_RESULTS];
	printf("  newest window %u: %s, score %d against %d; P9.25 holds %s, set %u times "
	       "(%u classes waited for a second window)\n", r->win, class_name(r->cls), r->score,
	       r->second, class_name(s->cls), s->changes, s->waits);
	printf("  score: %u right, %u wrong", right, wrong);
	if (right + wrong) {
		printf(" (%.2f%%)", 100.0 * right / (right + wrong));
	}
	printf(", %u across a step, in %.1f s; since training %u right, %u wrong\n",
	       s->mixed - was->mixed, dt, s->right, s->wrong);
	fflush(stdout);
}

static void confusion(void)
{
	uint32_t t, c;

	printf("  windows by the step the input was on (rows) and the class found (columns):\n");
	printf("  %-12s", "");
	for (c = 0; c < AI64_PIPE_CLASSES; c++) {
		printf(" %4u%s", step_pct[c % AI64_PWM_STEPS], c % AI64_PWM_STEPS ? " " : "k");
	}
	printf("\n");
	for (t = 0; t < AI64_PIPE_CLASSES; t++) {
		printf("  %-12s", class_name(t));
		for (c = 0; c < AI64_PIPE_CLASSES; c++) {
			printf(" %5u", g_pipe->score.confusion[t][c]);
		}
		printf("\n");
	}
}

/* One line: the windows, P8.31 to P9.25, and the score. */
static void line(const struct snap *s, const struct snap *was)
{
	const struct ai64_pipe_stage *a = &s->st[AI64_PIPE_PWM], *b = &was->st[AI64_PIPE_PWM];
	uint32_t n = a->windows - b->windows, right = s->right - was->right;
	uint32_t wrong = s->wrong - was->wrong;

	if (!g_pipe->ctl.run || n == 0) {
		printf("pipeline %s\n", g_pipe->ctl.run ? "running, but no window reached R5F0_1" :
		       "stopped");
	} else {
		printf("pipeline: %.0f windows/s, window end to P9.25 set %.0f us (max %.0f), "
		       "%u right %u wrong (%.2f%%), P9.25 %s\n", n / (s->t - was->t),
		       ticks_us((double)(a->lat_sum - b->lat_sum) / n), ticks_us(a->lat_max), right,
		       wrong, right + wrong ? 100.0 * right / (right + wrong) : 0.0,
		       g_pipe->ctl.follow ? class_name(s->cls) : "on its own steps");
	}
	fflush(stdout);
}

static int view_main(int argc, char **argv)
{
	int once = argc > 2 && !strcmp(argv[2], "once");
	int lines = argc > 2 && !strcmp(argv[2], "lines");
	int every = argc > 2 && !once && !lines ? atoi(argv[2]) : 10;
	struct snap was, now;

	if (every <= 0 || argc > 3) {
		fprintf(stderr, "usage: %s pipe [seconds|once|lines] | start [seconds] | stop\n",
			argv[0]);
		return 2;
	}
	take(&was);
	for (;;) {
		sleep(once ? 1 : every);
		take(&now);
		rebase(&was, &now);
		if (lines) {
			line(&now, &was);
			was = now;
			continue;
		}
		draw(&now, &was);
		if (once) {
			confusion();
			return 0;
		}
		was = now;
	}
}

int pipe_main(int argc, char **argv)
{
	if (pipe_map()) {
		return 1;
	}
	if (argc > 2 && !strcmp(argv[2], "start")) {
		return start_main(argc, argv);
	}
	if (argc > 2 && !strcmp(argv[2], "stop")) {
		return stop_main();
	}
	return view_main(argc, argv);
}
