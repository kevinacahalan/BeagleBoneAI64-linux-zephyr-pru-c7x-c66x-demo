/* SPDX-License-Identifier: MIT */
/*
 * ai64 mma [rows] [runs]
 *   Runs an int8 neural-network layer (64 inputs, 64 outputs, on a batch of
 *   rows) on the C7x's Matrix Multiply Accelerator, and in plain C on the
 *   C7x, through the job block; checks both against the A72, and prints the
 *   cycles each took.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai64.h"
#include "ai64_dsp_job.h"

/* int8, or with ReLU uint8 (stored in the same byte). */
static int8_t layer_out(int32_t v, uint32_t shift, uint32_t relu)
{
	if (shift) {
		v = (v + (1 << (shift - 1))) >> shift;
	}
	if (relu) {
		return (int8_t)(uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v);
	}
	return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
}

/* The layer the C7x runs (ai64_dsp_job.h), here on the A72. */
void layer_ref(const int8_t *a, const int8_t *b, int8_t *y, uint32_t rows,
		      uint32_t shift, uint32_t relu)
{
	uint32_t r, j, k;

	for (r = 0; r < rows; r++) {
		int32_t acc[64] = {0};

		for (k = 0; k < 64; k++) {
			for (j = 0; j < 64; j++) {
				acc[j] += a[r * 64 + k] * b[k * 64 + j];
			}
		}
		for (j = 0; j < 64; j++) {
			y[r * 64 + j] = layer_out(acc[j], shift, relu);
		}
	}
}

/*
 * ai64 mma [rows] [runs]: the int8 layer on the C7x's MMA and in C on the
 * C7x, each checked against the A72, with the cycles each took.
 */
int mma_main(int argc, char **argv)
{
	uint32_t rows = argc > 2 ? (uint32_t)atoi(argv[2]) : AI64_JOB_LAYER_MAX_ROWS;
	int runs = argc > 3 ? atoi(argv[3]) : 20;
	static int8_t a[AI64_JOB_LAYER_MAX_ROWS * 64], b[64 * 64], want[AI64_JOB_LAYER_MAX_ROWS * 64];
	static const uint32_t op[2] = { AI64_JOB_LAYER_MMA, AI64_JOB_LAYER_C };
	double cycles[2] = { 0, 0 }, wait[2] = { 0, 0 }, arm = 0, t0;
	volatile uint32_t *job;
	volatile int8_t *ja, *jb, *jy;
	char state[32];
	uint32_t i, shift, relu;
	int r, o;

	if (rows == 0 || rows > AI64_JOB_LAYER_MAX_ROWS || rows % 16 || runs <= 0) {
		fprintf(stderr, "usage: %s mma [rows, a multiple of 16 up to %u] [runs]\n", argv[0],
			AI64_JOB_LAYER_MAX_ROWS);
		return 2;
	}
	/* A powered-off DSP's L2 SRAM does not answer, and reading it hangs the SoC. */
	if (!rproc_running(core_get("c7x")->rproc, state, sizeof(state))) {
		fprintf(stderr, "the C7x is %s; start it first (ai64-demo does)\n", state);
		return 1;
	}
	job = map_phys(AI64_JOB_C7X_PHYS, AI64_JOB_SIZE);
	if (!job) {
		return 1;
	}
	if (job[AI64_JOB_W_MAGIC] != AI64_JOB_MAGIC) {
		fprintf(stderr, "the C7x has no job block. Is it running ai64-c7x.elf?\n");
		return 1;
	}
	ja = (volatile int8_t *)((volatile uint8_t *)job + AI64_JOB_LAYER_A_OFF);
	jb = (volatile int8_t *)((volatile uint8_t *)job + AI64_JOB_LAYER_B_OFF);
	jy = (volatile int8_t *)((volatile uint8_t *)job + AI64_JOB_LAYER_Y_OFF);

	srand(2);
	for (r = 0; r < runs; r++) {
		/* New data, shift, and ReLU each run, so a stale result cannot pass. */
		for (i = 0; i < rows * 64; i++) {
			a[i] = (int8_t)(rand() % 256 - 128);
			ja[i] = a[i];
		}
		for (i = 0; i < 64 * 64; i++) {
			b[i] = (int8_t)(rand() % 256 - 128);
			jb[i] = b[i];
		}
		shift = 8 + rand() % 6;
		relu = rand() & 1;
		job[AI64_JOB_W_ARG] = shift | relu << 8;
		t0 = now_us();
		layer_ref(a, b, want, rows, shift, relu);
		arm += now_us() - t0;
		for (o = 0; o < 2; o++) {
			for (i = 0; i < rows * 64; i++) {
				jy[i] = 0x55;
			}
			t0 = now_us();
			if (dsp_run(job, op[o], rows, 0)) {
				return 1;
			}
			wait[o] += now_us() - t0;
			cycles[o] += job[AI64_JOB_W_CYCLES];
			{
				uint32_t bad = 0;

				for (i = 0; i < rows * 64; i++) {
					if (jy[i] != want[i]) {
						if (getenv("AI64_MMA_DEBUG") && bad < 12) {
							int32_t acc = 0;
							uint32_t k;

							for (k = 0; k < 64; k++) {
								acc += a[i / 64 * 64 + k] * b[k * 64 + i % 64];
							}
							printf("  y[%u][%u] is %d, expected %d, acc %d\n", i / 64, i % 64,
							       jy[i], want[i], acc);
						}
						bad++;
					}
				}
				if (bad) {
					printf("run %d, %s: %u of %u wrong (shift %u, relu %u)\n", r, o ? "C" : "MMA",
					       bad, rows * 64, shift, relu);
					return 1;
				}
			}
		}
	}
	printf("C7x: %d int8 layers, %u rows x 64 inputs x 64 outputs (%u multiply-adds), all match the A72\n",
	       runs, rows, rows * 64 * 64);
	printf("  on the MMA:   %.0f cycles = %.2f us, %.0f multiply-adds a cycle; A72 waited %.1f us\n",
	       cycles[0] / runs, cycles[0] / runs / 1000, rows * 4096.0 / (cycles[0] / runs), wait[0] / runs);
	printf("  in C on the C7x: %.0f cycles = %.2f us, %.0f multiply-adds a cycle; A72 waited %.1f us\n",
	       cycles[1] / runs, cycles[1] / runs / 1000, rows * 4096.0 / (cycles[1] / runs), wait[1] / runs);
	printf("  the same layer on one A72 core: %.1f us\n", arm / runs);
	return 0;
}
