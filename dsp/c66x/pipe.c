/* SPDX-License-Identifier: MIT */
/*
 * The C66x cores' stage of the pipeline (ai64_pipe.h): a window of P8.31
 * samples in, 64 int8 features out. C66x 0 takes the even windows, C66x 1
 * the odd ones, so each has one every 1.024 ms.
 *
 * R5F0_0's UDMA puts each window into this core's L2 SRAM, then writes
 * one past its number; L1D snoops both writes, so polling that word costs
 * a cached load. For each window:
 *
 *   - the 2048 samples, 1 for high and -1 for low, times a Hann window,
 *     go through a 2048-point complex FFT in single-precision float, radix
 *     2, from a table of twiddles made at boot. Each of its 11 stages is
 *     one loop over its 1024 butterflies, from one pair of arrays into the
 *     other: no butterfly then depends on another, and the compiler
 *     pipelines the loop. Short loops of the stages' own groups (a stage
 *     has 1024 groups of one butterfly at the start) took 325 us a window;
 *     this takes 89 us.
 *   - feature 0 is the DC, the windowed mean in -1 to 1, as -127 to 127:
 *     it is about 2 x duty - 1. Features 1 to 62 are bins 1 to 62
 *     (977 Hz each, up to 60 kHz), as amplitudes, 100 for a full-scale
 *     sine. Feature 63 is 0.
 *
 * The bins line up with the input's frequencies: 10, 15, and 20 kHz are
 * bins 10.2, 15.4, and 20.5, and a duty that is not 50% puts harmonics
 * at their multiples. The features go into the C7x's L2 SRAM, the row of
 * the window's number % 16, with the window's tag, and then one past the
 * window's number, in this core's word. The C7x logs them for Linux.
 */

#include <stdint.h>
#include <math.h>
#include <c6x.h>

#include "ai64_pipe.h"
#include "ai64_c66x_pipe.h"

#define N     AI64_PIPE_WIN
#define LOG2N 11u

#define IN   ((volatile struct ai64_pipe_c66x_in *)AI64_PIPE_C66X_LOCAL)
#define OUT  ((volatile struct ai64_pipe_c7x_in *)AI64_PIPE_C7X_ADDR)
#define PIPE ((volatile struct ai64_pipe *)AI64_PIPE_ADDR)

/* The FFT goes from re0, im0 to re1, im1 and back, a stage at a time. */
#pragma DATA_ALIGN(re0, 8)
static float re0[N];
#pragma DATA_ALIGN(im0, 8)
static float im0[N];
#pragma DATA_ALIGN(re1, 8)
static float re1[N];
#pragma DATA_ALIGN(im1, 8)
static float im1[N];
/* e^(-2 pi i k / N) = tw_c[k] - i tw_s[k], for k < N / 2 */
static float tw_c[N / 2], tw_s[N / 2];
static float hann[N];
static uint16_t rev[N];
static float hann_sum;
/* One past the newest window taken; this core's number (DNUM). */
static uint32_t seen, weights, core;

static uint32_t (*now)(void);

void ai64_c66x_pipe_init(uint32_t (*gtc_lo)(void))
{
	uint32_t n, b, r;

	now = gtc_lo;
	core = DNUM;
	hann_sum = 0.0f;
	for (n = 0; n < N; n++) {
		hann[n] = (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * n / N));
		hann_sum += hann[n];
		for (b = 0, r = 0; b < LOG2N; b++) {
			r |= ((n >> b) & 1u) << (LOG2N - 1u - b);
		}
		rev[n] = (uint16_t)r;
	}
	for (n = 0; n < N / 2u; n++) {
		tw_c[n] = (float)cos(2.0 * 3.14159265358979323846 * n / N);
		tw_s[n] = (float)sin(2.0 * 3.14159265358979323846 * n / N);
	}
	/* Windows from before this core started are not this run's. */
	seen = IN->ready;
	weights = PIPE->ctl.weights_seq;
	ai64_pipe_stage_reset(&PIPE->stage[AI64_PIPE_FFT(core)]);
}

/*
 * Stage s of a decimation-in-time FFT on bit-reversed input: butterfly k
 * joins a and a + 2^s, in groups of 2^(s+1), with twiddle j = k % 2^s of
 * the 2^(s+1)-point transform.
 */
static void stage(const float *restrict xr, const float *restrict xi, float *restrict yr,
		  float *restrict yi, uint32_t s)
{
	uint32_t half = 1u << s, mask = half - 1u, tshift = LOG2N - 1u - s, k;

#pragma MUST_ITERATE(N / 2, N / 2, N / 2)
	for (k = 0; k < N / 2u; k++) {
		uint32_t j = k & mask;
		uint32_t a = ((k >> s) << (s + 1u)) | j, b = a + half;
		float wr = tw_c[j << tshift], wi = tw_s[j << tshift];
		float tr = xr[b] * wr + xi[b] * wi;
		float ti = xi[b] * wr - xr[b] * wi;

		yr[a] = xr[a] + tr;
		yi[a] = xi[a] + ti;
		yr[b] = xr[a] - tr;
		yi[b] = xi[a] - ti;
	}
}

/* From re0, im0; 11 stages leave the result in re1, im1. */
static void fft(void)
{
	uint32_t s;

	for (s = 0; s < LOG2N; s++) {
		if (s & 1u) {
			stage(re1, im1, re0, im0, s);
		} else {
			stage(re0, im0, re1, im1, s);
		}
	}
}

static int8_t q8(float v)
{
	v = v < 0.0f ? v - 0.5f : v + 0.5f;
	return v > 127.0f ? 127 : v < -127.0f ? -127 : (int8_t)v;
}

static void features(const volatile uint16_t *chunk, int8_t *f)
{
	uint32_t c, i, n;

	for (c = 0, n = 0; c < AI64_PIPE_CHUNKS; c++) {
		uint32_t bits = chunk[c];

		/* The oldest sample is bit 15. */
		for (i = 0; i < 16u; i++, n++) {
			float x = (bits >> (15u - i)) & 1u ? hann[n] : -hann[n];

			re0[rev[n]] = x;
			im0[rev[n]] = 0.0f;
		}
	}
	fft();
	f[0] = q8(127.0f * re1[0] / hann_sum);
	for (i = 1; i < AI64_PIPE_FEATURES - 1u; i++) {
		f[i] = q8(100.0f * sqrtf(re1[i] * re1[i] + im1[i] * im1[i]) / (hann_sum * 0.5f));
	}
	f[AI64_PIPE_FEATURES - 1u] = 0;
}

static void one(volatile struct ai64_pipe_stage *st, uint32_t w)
{
	volatile struct ai64_pipe_slot *sl = &IN->slot[AI64_PIPE_IN_SLOT(w)];
	uint32_t row = w % AI64_PIPE_ROWS, t0 = now(), t1, i;
	int8_t f[AI64_PIPE_FEATURES];
	struct ai64_pipe_tag tag;

	tag.win = sl->tag.win;
	tag.t_end_lo = sl->tag.t_end_lo;
	tag.t_end_hi = sl->tag.t_end_hi;
	if (tag.win != w) {
		st->errors++;
		return;
	}
	features(sl->chunk, f);
	t1 = now();
	tag.t_rtu = t1;

	for (i = 0; i < AI64_PIPE_FEATURES; i++) {
		OUT->a[row][i] = f[i];
	}
	OUT->tag[row].win = tag.win;
	OUT->tag[row].t_end_lo = tag.t_end_lo;
	OUT->tag[row].t_end_hi = tag.t_end_hi;
	OUT->tag[row].t_rtu = tag.t_rtu;
	_mfence();
	OUT->feat_seq[core] = w + 1u;

	if (PIPE->ctl.weights_seq != weights) {
		weights = PIPE->ctl.weights_seq;
		ai64_pipe_stage_reset(st);
	}
	ai64_pipe_stage_count(st, w, t1 - tag.t_end_lo, t1 - t0);
}

int ai64_c66x_pipe_poll(void)
{
	volatile struct ai64_pipe_stage *st;
	uint32_t ready = IN->ready;

	if (ready == seen) {
		return 0;
	}
	st = &PIPE->stage[AI64_PIPE_FFT(core)];
	/*
	 * So far on that the slots no longer hold this core's windows: it fell
	 * behind, and lost them. Further on, or back: R5F0_0 started again (or
	 * this core did, and the word is from the last run).
	 */
	if (ready - seen > 2u * AI64_PIPE_IN_SLOTS) {
		if (ready - seen < AI64_PIPE_RESYNC) {
			st->dropped += (ready - seen - 1u) / 2u;
		}
		seen = ready - 1u;
	}
	/* The windows between are the other core's. */
	for (; seen != ready; seen++) {
		if (seen % 2u == core) {
			one(st, seen);
		}
	}
	return 1;
}
