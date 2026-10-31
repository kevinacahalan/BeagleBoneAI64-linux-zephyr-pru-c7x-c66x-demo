/* SPDX-License-Identifier: MIT */
/*
 * The C7x's stage of the pipeline (ai64_pipe.h): 64 features in, a class
 * out, on the Matrix Multiply Accelerator.
 *
 * The C66x cores write each window's features into a ring of 16 rows in
 * this core's L2 SRAM, in the page the MMU maps non-cacheable, then one
 * past the window's number, each in its own word: C66x 0 the even windows,
 * C66x 1 the odd ones. When either word moves on, the C7x runs the 16 rows,
 * the ring as it stands, through the int8 layer (ai64_c7x_layer_mma()):
 * the same 64 x 64 layer as `ai64 mma`, with Linux's trained weights, one
 * output column per class. The MMA takes a block of 16 rows in the time a
 * row would take, so the whole ring costs what one new row does. Then for
 * each window since the last, in order, the best of the first
 * AI64_PIPE_CLASSES outputs on its row is its class. The verdict goes into
 * the result ring in DDR, the window's number into R5F0_1's mailbox, and
 * the features into the log in DDR, for Linux to train on. A window that
 * is not in its row by the time a later one is, the C66x lost.
 *
 * The weights come from DDR when Linux bumps weights_seq, into a cached
 * page here, written back from L1D before the Streaming Engine reads them.
 */

#include <c7x.h>
#include <stdint.h>

#include "ai64_pipe.h"
#include "mma.h"
#include "mmu.h"
#include "pipe.h"

#define IN   ((volatile struct ai64_pipe_c7x_in *)AI64_PIPE_C7X_ADDR)
#define PIPE ((volatile struct ai64_pipe *)AI64_PIPE_ADDR)
#define B    ((int8_t *)AI64_PIPE_C7X_B)
#define Y    ((int8_t *)AI64_PIPE_C7X_Y)

static uint32_t (*now)(void);
/* One past the newest window taken. */
static uint32_t seen, weights_seen, shift;

/* One past the newest window the C66x cores have written. */
static uint32_t newest(void)
{
	uint32_t f0 = IN->feat_seq[0], f1 = IN->feat_seq[1];

	return (int32_t)(f1 - f0) > 0 ? f1 : f0;
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
	seen = newest();
	ai64_pipe_stage_reset(&PIPE->stage[AI64_PIPE_MMA]);
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

/* Window w's verdict, from its row of Y, the MMA's output at t_class. */
static void verdict(volatile struct ai64_pipe_stage *st, uint32_t w, uint32_t t0,
		    uint32_t t_class)
{
	uint32_t row = w % AI64_PIPE_ROWS, c, i, best = 0;
	int32_t top = -129, second = -129;
	volatile struct ai64_pipe_result *res;
	volatile struct ai64_pipe_log *log;

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
	res = &PIPE->result[w % AI64_PIPE_RESULTS];
	res->win = w;
	res->cls = best;
	res->score = top;
	res->second = second;
	res->t_end_lo = IN->tag[row].t_end_lo;
	res->t_end_hi = IN->tag[row].t_end_hi;
	res->t_feat = IN->tag[row].t_rtu;
	res->t_class = t_class;
	__memory_fence(0x1);
	*(volatile uint32_t *)(uintptr_t)AI64_PIPE_MBOX_MSG(AI64_PIPE_MBOX_R5F0_1) = w;

	log = &PIPE->log[PIPE->log_n % AI64_PIPE_LOG_N];
	log->tag.win = w;
	log->tag.t_end_lo = IN->tag[row].t_end_lo;
	log->tag.t_end_hi = IN->tag[row].t_end_hi;
	log->tag.t_rtu = IN->tag[row].t_rtu;
	for (i = 0; i < AI64_PIPE_FEATURES; i += 4u) {
		*(volatile uint32_t *)&log->f[i] = *(volatile const uint32_t *)&IN->a[row][i];
	}
	__memory_fence(0x1);
	PIPE->log_n = PIPE->log_n + 1u;

	ai64_pipe_stage_count(st, w, t_class - IN->tag[row].t_end_lo, t_class - t0);
}

int ai64_c7x_pipe_poll(void)
{
	volatile struct ai64_pipe_stage *st;
	uint32_t end = newest(), n, i, here = 0, t0, t1;

	if (end == seen) {
		return 0;
	}
	st = &PIPE->stage[AI64_PIPE_MMA];
	if (PIPE->ctl.magic == AI64_PIPE_MAGIC && PIPE->ctl.weights_seq != weights_seen) {
		weights_seen = PIPE->ctl.weights_seq;
		take_weights();
		ai64_pipe_stage_reset(st);
	}
	/* Further on than the ring holds: windows lost; a big jump, or back, is
	 * a C66x or this core starting again. Either way, from the newest on. */
	if (end - seen > AI64_PIPE_ROWS) {
		if (end - seen < AI64_PIPE_RESYNC) {
			st->dropped += end - seen - 1u;
		}
		seen = end - 1u;
	}
	/* Which of them are in their rows, before the MMA reads the rows. */
	n = end - seen;
	for (i = 0; i < n; i++) {
		if (IN->tag[(seen + i) % AI64_PIPE_ROWS].win == seen + i) {
			here |= 1u << i;
		}
	}

	t0 = now();
	ai64_c7x_layer_mma((const int8_t *)IN->a, B, Y, AI64_PIPE_ROWS, shift, 0);
	t1 = now();
	for (i = 0; i < n; i++) {
		if (here & (1u << i)) {
			verdict(st, seen + i, t0, t1);
		} else {
			st->dropped++;
		}
	}
	seen = end;
	return 1;
}
