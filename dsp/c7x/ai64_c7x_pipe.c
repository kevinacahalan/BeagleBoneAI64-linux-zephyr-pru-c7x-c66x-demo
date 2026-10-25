/* SPDX-License-Identifier: MIT */
/*
 * The C7x's stage of the pipeline (ai64_pipe.h): 64 features in, a class
 * out, on the Matrix Multiply Accelerator.
 *
 * C66x 0 writes each window's features into a ring of 16 rows in this
 * core's L2 SRAM, in the page the MMU maps non-cacheable, then the count of
 * windows it has written. On a new count the C7x runs the newest 16 rows,
 * the ring as it stands, through the int8 layer (ai64_c7x_layer_mma()):
 * the same 64 x 64 layer as `ai64 mma`, with Linux's trained weights, one
 * output column per class. The MMA takes a block of 16 rows in the time a
 * row would take, so the whole ring costs what the one new row does. The
 * best of the first AI64_PIPE_CLASSES outputs on the newest window's row
 * is its class. The verdict goes into the result ring in DDR, and the
 * window's number into R5F0_1's mailbox.
 *
 * The weights come from DDR when Linux bumps weights_seq, into a cached
 * page here, written back from L1D before the Streaming Engine reads them.
 */

#include <c7x.h>
#include <stdint.h>

#include "ai64_pipe.h"
#include "ai64_c7x_mma.h"
#include "ai64_c7x_mmu.h"
#include "ai64_c7x_pipe.h"

#define IN   ((volatile struct ai64_pipe_c7x_in *)AI64_PIPE_C7X_ADDR)
#define PIPE ((volatile struct ai64_pipe *)AI64_PIPE_ADDR)
#define B    ((int8_t *)AI64_PIPE_C7X_B)
#define Y    ((int8_t *)AI64_PIPE_C7X_Y)

static uint32_t (*now)(void);
static uint32_t seen, weights_seen, shift;

static void stage_reset(volatile struct ai64_pipe_stage *st)
{
	st->magic = 0;
	st->windows = 0;
	st->dropped = 0;
	st->last = 0;
	st->lat_sum = 0;
	st->lat_min = 0xffffffffu;
	st->lat_max = 0;
	st->lat_last = 0;
	st->work_sum = 0;
	st->work_max = 0;
	st->errors = 0;
	st->magic = AI64_PIPE_MAGIC;
}

void ai64_c7x_pipe_init(uint32_t (*gtc_lo)(void))
{
	uint32_t i;

	now = gtc_lo;
	/* No weights yet: every output 0, until Linux trains the layer. */
	for (i = 0; i < AI64_PIPE_FEATURES * AI64_PIPE_FEATURES; i++) {
		B[i] = 0;
	}
	ai64_l1d_wb_all();
	weights_seen = 0;
	shift = 0;
	seen = IN->feat_seq;
	stage_reset(&PIPE->stage[AI64_PIPE_MMA]);
}

static void take_weights(void)
{
	uint32_t i;

	for (i = 0; i < AI64_PIPE_FEATURES * AI64_PIPE_FEATURES; i += 4u) {
		*(uint32_t *)&B[i] = *(volatile uint32_t *)&PIPE->weights[i / 64u][i % 64u];
	}
	shift = PIPE->ctl.shift & 31u;
	ai64_l1d_wb_all();
	__memory_fence(0x1);
}

int ai64_c7x_pipe_poll(void)
{
	volatile struct ai64_pipe_stage *st;
	volatile struct ai64_pipe_result *res;
	uint32_t fs = IN->feat_seq, w, row, t0, t1, t_end, lat, c, best = 0;
	int32_t top = -129, second = -129;

	if (fs == seen) {
		return 0;
	}
	st = &PIPE->stage[AI64_PIPE_MMA];
	if (PIPE->ctl.magic == AI64_PIPE_MAGIC && PIPE->ctl.weights_seq != weights_seen) {
		weights_seen = PIPE->ctl.weights_seq;
		take_weights();
		stage_reset(st);
	}
	/* Windows it missed; a bigger jump is C66x 0 or this core starting again. */
	if (fs - seen > 1u && fs - seen < AI64_PIPE_RESYNC) {
		st->dropped += fs - seen - 1u;
	}
	seen = fs;
	w = fs - 1u;
	row = w % AI64_PIPE_ROWS;
	if (IN->tag[row].win != w) {
		st->errors++;
		return 1;
	}

	t_end = IN->tag[row].t_end_lo;
	t0 = now();
	ai64_c7x_layer_mma((const int8_t *)IN->a, B, Y, AI64_PIPE_ROWS, shift, 0);
	for (c = 0; c < AI64_PIPE_CLASSES; c++) {
		int32_t v = Y[row * AI64_PIPE_FEATURES + c];

		if (v > top) {
			second = top;
			top = v;
			best = c;
		} else if (v > second) {
			second = v;
		}
	}
	t1 = now();

	res = &PIPE->result[w % AI64_PIPE_RESULTS];
	res->win = w;
	res->cls = best;
	res->score = top;
	res->second = second;
	res->t_end_lo = t_end;
	res->t_end_hi = IN->tag[row].t_end_hi;
	res->t_feat = IN->tag[row].t_rtu;
	res->t_class = t1;
	__memory_fence(0x1);
	*(volatile uint32_t *)(uintptr_t)AI64_PIPE_MBOX_MSG(AI64_PIPE_MBOX_R5F0_1) = w;

	lat = t1 - t_end;
	st->windows++;
	st->last = w;
	st->lat_last = lat;
	st->lat_sum += lat;
	if (lat < st->lat_min) {
		st->lat_min = lat;
	}
	if (lat > st->lat_max) {
		st->lat_max = lat;
	}
	st->work_sum += t1 - t0;
	if (t1 - t0 > st->work_max) {
		st->work_max = t1 - t0;
	}
	return 1;
}
