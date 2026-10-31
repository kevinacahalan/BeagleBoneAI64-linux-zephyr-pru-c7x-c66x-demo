/* SPDX-License-Identifier: MIT */
/*
 * ai64 dsp <core> [count] [taps] [runs]
 *   Hands a DSP FIR filter jobs through its job block (ai64_dsp_job.h), and
 *   checks each result against the same filter run here. Prints the DSP's
 *   cycles, the time the A72 waited, and the time the A72 takes alone.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai64.h"
#include "ai64_dsp_job.h"

static void fir_ref(const int16_t *x, const int16_t *h, int16_t *y, uint32_t count, uint32_t taps)
{
	uint32_t n, k;

	for (n = 0; n < count; n++) {
		int32_t acc = 0;

		for (k = 0; k < taps; k++) {
			acc += (int32_t)h[k] * x[n + k];
		}
		acc >>= 15;
		y[n] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)acc;
	}
}

/* Hand the DSP one job and wait for it. 0 on success. */
int dsp_run(volatile uint32_t *job, uint32_t op, uint32_t count, uint32_t taps)
{
	uint32_t seq = job[AI64_JOB_W_REQ_SEQ] + 1;
	double t0;

	job[AI64_JOB_W_OP] = op;
	job[AI64_JOB_W_COUNT] = count;
	job[AI64_JOB_W_TAPS] = taps;
	__sync_synchronize();
	job[AI64_JOB_W_REQ_SEQ] = seq;
	t0 = now_us();
	while (job[AI64_JOB_W_DONE_SEQ] != seq) {
		if (now_us() - t0 > 2e6) {
			fprintf(stderr, "the DSP did not finish job %u in 2 s\n", seq);
			return -1;
		}
	}
	__sync_synchronize();
	if (job[AI64_JOB_W_STATUS] != AI64_JOB_OK) {
		fprintf(stderr, "the DSP refused job %u: status %u\n", seq, job[AI64_JOB_W_STATUS]);
		return -1;
	}
	return 0;
}

int dsp_main(int argc, char **argv)
{
	uint32_t count = argc > 3 ? (uint32_t)atoi(argv[3]) : 4096;
	uint32_t taps = argc > 4 ? (uint32_t)atoi(argv[4]) : 32;
	int runs = argc > 5 ? atoi(argv[5]) : 20;
	static int16_t x[AI64_JOB_MAX_COUNT + AI64_JOB_MAX_TAPS], h[AI64_JOB_MAX_TAPS];
	static int16_t want[AI64_JOB_MAX_COUNT];
	volatile uint32_t *job = NULL;
	volatile int16_t *jx, *jh, *jy;
	char state[32];
	double mhz = 0, wait = 0, ping = 0, arm = 0, cycles = 0, t0;
	const struct ai64_core *c;
	uint32_t i;
	int r;

	if (argc < 3 || count == 0 || count > AI64_JOB_MAX_COUNT || taps == 0 ||
	    taps > AI64_JOB_MAX_TAPS || runs <= 0) {
		fprintf(stderr, "usage: %s dsp <core> [count <= %u] [taps <= %u] [runs]\n",
			argv[0], AI64_JOB_MAX_COUNT, AI64_JOB_MAX_TAPS);
		return 2;
	}
	c = core_find(argv[2]);
	if (!c || !c->job) {
		fprintf(stderr, "%s is not a DSP; try c66x0, c66x1, or c7x\n", argv[2]);
		return 2;
	}
	/* A powered-off DSP's L2 SRAM does not answer, and reading it hangs the SoC. */
	if (!rproc_running(c->rproc, state, sizeof(state))) {
		fprintf(stderr, "%s is %s; start it first (ai64-demo does)\n", c->rproc, state);
		return 1;
	}
	job = map_phys(c->job, AI64_JOB_SIZE);
	mhz = c->mhz;
	if (!job) {
		return 1;
	}
	if (job[AI64_JOB_W_MAGIC] != AI64_JOB_MAGIC) {
		fprintf(stderr, "%s has no job block. Is it running ai64-c66x.elf or ai64-c7x.elf?\n", argv[2]);
		return 1;
	}
	jx = (volatile int16_t *)((volatile uint8_t *)job + AI64_JOB_X_OFF);
	jh = (volatile int16_t *)((volatile uint8_t *)job + AI64_JOB_H_OFF);
	jy = (volatile int16_t *)((volatile uint8_t *)job + AI64_JOB_Y_OFF);

	srand(1);
	for (r = 0; r < runs; r++) {
		/* New data each run, so a stale result cannot pass. */
		for (i = 0; i < taps; i++) {
			h[i] = (int16_t)(rand() % 8192 - 4096);
			jh[i] = h[i];
		}
		for (i = 0; i < count + taps - 1; i++) {
			x[i] = (int16_t)(rand() % 65536 - 32768);
			jx[i] = x[i];
		}
		t0 = now_us();
		fir_ref(x, h, want, count, taps);
		arm += now_us() - t0;

		t0 = now_us();
		if (dsp_run(job, AI64_JOB_FIR, count, taps)) {
			return 1;
		}
		wait += now_us() - t0;
		cycles += job[AI64_JOB_W_CYCLES];
		for (i = 0; i < count; i++) {
			if (jy[i] != want[i]) {
				printf("run %d: y[%u] is %d, expected %d\n", r, i, jy[i], want[i]);
				return 1;
			}
		}

		t0 = now_us();
		if (dsp_run(job, AI64_JOB_PING, 0, 0)) {
			return 1;
		}
		ping += now_us() - t0;
	}
	printf("%s: %d FIR jobs, %u samples x %u taps, all match the A72\n", argv[2], runs, count, taps);
	printf("  DSP filter %.0f cycles = %.1f us; A72 waited %.1f us; empty job %.1f us\n",
	       cycles / runs, cycles / runs / mhz, wait / runs, ping / runs);
	printf("  the same filter on one A72 core: %.1f us\n", arm / runs);
	return 0;
}
