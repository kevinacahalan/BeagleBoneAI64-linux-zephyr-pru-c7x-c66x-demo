/* SPDX-License-Identifier: MIT */
/*
 * The DSP job block: Linux hands a DSP one job at a time through it.
 *
 * Each DSP keeps the block in its own L2 SRAM, which Linux reaches through
 * /dev/mem at the SoC address. On the C66x, L1D snoops writes from outside,
 * so neither side has cache work to do. The C7x maps the block's header
 * non-cacheable and writes L1D back around each job (c7x/main.c).
 *
 *   Linux: write op, count, taps, and the input; then req_seq + 1
 *   DSP:   sees req_seq != done_seq, runs the job, writes the output,
 *          status, and cycles; then done_seq = req_seq
 *
 * AI64_JOB_FIR is a 16-bit fixed-point FIR filter, the "valid" part of the
 * convolution: y[n] = sat16((sum of h[k] * x[n + k], k < taps) >> 15), for
 * n < count. x holds count + taps - 1 samples. AI64_JOB_PING does nothing,
 * to time the handoff alone.
 *
 * Linux and both DSP compilers include this file, so it only uses 32-bit
 * fields at fixed offsets.
 */
#ifndef AI64_DSP_JOB_H_
#define AI64_DSP_JOB_H_

#include <stdint.h>

/* The block's address in the core's own view, and in the SoC's. */
#define AI64_JOB_C66X_LOCAL  0x00838000u
#define AI64_JOB_C66X0_PHYS  0x4d80838000ull
#define AI64_JOB_C66X1_PHYS  0x4d81838000ull
#define AI64_JOB_C7X_LOCAL   0x64800000u
#define AI64_JOB_C7X_PHYS    0x64800000ull
#define AI64_JOB_SIZE        0x10000u

#define AI64_JOB_MAGIC     0x424f4a41u /* "AJOB" */

#define AI64_JOB_PING      1u
#define AI64_JOB_FIR       2u

#define AI64_JOB_OK        0u
#define AI64_JOB_BAD_OP    1u
#define AI64_JOB_BAD_SIZE  2u

#define AI64_JOB_MAX_COUNT 8192u
#define AI64_JOB_MAX_TAPS  64u

/* Word offsets of the header fields. */
#define AI64_JOB_W_MAGIC    0  /* DSP: set once at boot */
#define AI64_JOB_W_REQ_SEQ  1  /* Linux */
#define AI64_JOB_W_DONE_SEQ 2  /* DSP */
#define AI64_JOB_W_OP       3  /* Linux */
#define AI64_JOB_W_COUNT    4  /* Linux: output samples */
#define AI64_JOB_W_TAPS     5  /* Linux */
#define AI64_JOB_W_STATUS   6  /* DSP */
#define AI64_JOB_W_CYCLES   7  /* DSP: core cycles the job took */

/* Byte offsets of the int16_t arrays. */
#define AI64_JOB_H_OFF 0x0040u /* taps */
#define AI64_JOB_X_OFF 0x0100u /* input, count + taps - 1 samples */
#define AI64_JOB_Y_OFF 0x4200u /* output, count samples */

#ifdef AI64_JOB_CYCLES
/*
 * The DSP side. The image defines AI64_JOB_CYCLES() as its cycle counter
 * and AI64_JOB_FENCE() as its memory fence, calls job_init() once, and
 * job_poll() from its main loop. An image that caches the job block defines
 * AI64_JOB_CACHE_IN() to drop stale lines before a job reads its input, and
 * AI64_JOB_CACHE_OUT() to write the output back before Linux reads it.
 */

#ifndef AI64_JOB_CACHE_IN
#define AI64_JOB_CACHE_IN()
#endif
#ifndef AI64_JOB_CACHE_OUT
#define AI64_JOB_CACHE_OUT()
#endif

#define JOB_WORD(w) (((volatile uint32_t *)(uintptr_t)AI64_JOB_LOCAL)[w])

static void job_init(void)
{
	JOB_WORD(AI64_JOB_W_DONE_SEQ) = JOB_WORD(AI64_JOB_W_REQ_SEQ);
	JOB_WORD(AI64_JOB_W_MAGIC) = AI64_JOB_MAGIC;
}

#ifdef __C7000__
/* The C7x's copy of a job's input and taps, in its cached DDR. */
#pragma DATA_ALIGN(job_xw, 64)
static int16_t job_xw[AI64_JOB_MAX_COUNT + AI64_JOB_MAX_TAPS];
#pragma DATA_ALIGN(job_hw, 64)
static int16_t job_hw[AI64_JOB_MAX_TAPS];
#endif

static void job_fir(const int16_t *restrict x, const int16_t *restrict h,
		    int16_t *restrict y, uint32_t count, uint32_t taps)
{
	uint32_t n = 0, k;

	_nassert((uintptr_t)x % 8 == 0);
	_nassert((uintptr_t)h % 8 == 0);
#ifdef __C7000__
	/*
	 * The C7x does 32 outputs at a time, taps outside: two vectors of 16
	 * 32-bit sums stay in registers while one 32-sample load per tap feeds
	 * both. With the taps inside, a 32-tap filter is two vectors per
	 * output and the loop overhead costs more than the multiplies.
	 *
	 * It reads its L2 SRAM at about half the speed it reads cached DDR, so
	 * it first copies the input and taps into its own DDR. 4096 x 32 takes
	 * 19 us with the copy and 51 us without.
	 */
	uint32_t i;

	for (i = 0; i < count + taps - 1u; i++) {
		job_xw[i] = x[i];
	}
	for (i = 0; i < taps; i++) {
		job_hw[i] = h[i];
	}
	x = job_xw;
	h = job_hw;
	for (; n + 32u <= count; n += 32u) {
		int16 lo = (int16)0, hi = (int16)0;
		short32 out;

#pragma MUST_ITERATE(1, AI64_JOB_MAX_TAPS)
		for (k = 0; k < taps; k++) {
			short32 xv = *(const short32 *)&x[n + k];
			int16 hk = (int16)h[k];

			lo += __convert_int16(xv.lo) * hk;
			hi += __convert_int16(xv.hi) * hk;
		}
		lo = __max(__min(lo >> 15, (int16)32767), (int16)-32768);
		hi = __max(__min(hi >> 15, (int16)32767), (int16)-32768);
		out.lo = __convert_short16(lo);
		out.hi = __convert_short16(hi);
		*(short32 *)&y[n] = out;
	}
#endif
#pragma MUST_ITERATE(0, AI64_JOB_MAX_COUNT)
	for (; n < count; n++) {
		int32_t acc = 0;

#pragma MUST_ITERATE(1, AI64_JOB_MAX_TAPS)
		for (k = 0; k < taps; k++) {
			acc += (int32_t)h[k] * x[n + k];
		}
		acc >>= 15;
		y[n] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)acc;
	}
}

/* 1 when it ran a job. */
static int job_poll(void)
{
	uint8_t *base = (uint8_t *)(uintptr_t)AI64_JOB_LOCAL;
	uint32_t seq = JOB_WORD(AI64_JOB_W_REQ_SEQ);
	uint32_t count, taps, status = AI64_JOB_OK;
	uint32_t start;

	if (seq == JOB_WORD(AI64_JOB_W_DONE_SEQ)) {
		return 0;
	}
	AI64_JOB_FENCE();
	start = (uint32_t)AI64_JOB_CYCLES();
	AI64_JOB_CACHE_IN();
	count = JOB_WORD(AI64_JOB_W_COUNT);
	taps = JOB_WORD(AI64_JOB_W_TAPS);
	switch (JOB_WORD(AI64_JOB_W_OP)) {
	case AI64_JOB_PING:
		break;
	case AI64_JOB_FIR:
		if (count == 0 || count > AI64_JOB_MAX_COUNT ||
		    taps == 0 || taps > AI64_JOB_MAX_TAPS) {
			status = AI64_JOB_BAD_SIZE;
			break;
		}
		job_fir((const int16_t *)(base + AI64_JOB_X_OFF),
			(const int16_t *)(base + AI64_JOB_H_OFF),
			(int16_t *)(base + AI64_JOB_Y_OFF), count, taps);
		break;
	default:
		status = AI64_JOB_BAD_OP;
		break;
	}
	AI64_JOB_CACHE_OUT();
	JOB_WORD(AI64_JOB_W_CYCLES) = (uint32_t)AI64_JOB_CYCLES() - start;
	JOB_WORD(AI64_JOB_W_STATUS) = status;
	AI64_JOB_FENCE();
	JOB_WORD(AI64_JOB_W_DONE_SEQ) = seq;
	return 1;
}
#endif

#endif
