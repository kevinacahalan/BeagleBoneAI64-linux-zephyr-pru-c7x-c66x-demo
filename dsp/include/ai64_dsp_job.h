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
/* The taps in pairs, h[2i] in the low half. */
static int32_t job_hp[AI64_JOB_MAX_TAPS / 2];

/* 32 outputs from the even and the odd outputs' sums. */
static inline short32 job_c7x_out(int16 even, int16 odd)
{
	even = __max(__min(even >> 15, (int16)32767), (int16)-32768);
	odd = __max(__min(odd >> 15, (int16)32767), (int16)-32768);
	/* The low half of each lane, odd above even: y[n + 2i], y[n + 2i + 1]. */
	return __pack_low_short(__as_short32(odd), __as_short32(even));
}
#endif

#ifdef _TMS320C6600
/* The C66x's taps, padded with zeros to a multiple of 8, and one chunk's sums. */
#define JOB_C66X_CHUNK 2048u
#define JOB_MIN_U32(a, b) ((a) < (b) ? (a) : (b))
#pragma DATA_ALIGN(job_h8, 8)
static int16_t job_h8[AI64_JOB_MAX_TAPS];
#pragma DATA_ALIGN(job_acc, 8)
static int32_t job_acc[JOB_C66X_CHUNK];

/* Samples 1-4, 2-5, and 3-6 of the 8 samples in a and b. */
static inline long long job_shift1(long long a, long long b)
{
	return _itoll(_packlh2(_loll(b), _hill(a)), _packlh2(_hill(a), _loll(a)));
}

static inline long long job_shift2(long long a, long long b)
{
	return _itoll(_loll(b), _hill(a));
}

static inline long long job_shift3(long long a, long long b)
{
	return _itoll(_packlh2(_hill(b), _loll(b)), _packlh2(_loll(b), _hill(a)));
}

/*
 * Taps k to k + 7 for outputs 0 to blocks * 8 - 1: x is x + k, h0 and h1
 * the taps. The first pass stores the sums; later passes add to them.
 */
static inline void job_c66x_pass(const int16_t *restrict x, int32_t *restrict acc,
				 long long h0, long long h1, uint32_t blocks, int first)
{
	__x128_t hh0 = _llto128(h0, h0);
	__x128_t hh1 = _llto128(h1, h1);
	uint32_t b;

	_nassert((uintptr_t)x % 8 == 0);
	_nassert((uintptr_t)acc % 8 == 0);
#pragma MUST_ITERATE(1, JOB_C66X_CHUNK / 8)
	for (b = 0; b < blocks; b++) {
		const int16_t *xb = x + 8u * b;
		long long *ab = (long long *)(acc + 8u * b);
		long long w0 = _amem8_const(&xb[0]);
		long long w1 = _amem8_const(&xb[4]);
		long long w2 = _amem8_const(&xb[8]);
		long long w3 = _amem8_const(&xb[12]);
		long long s01, s23, s45, s67;

		s01 = _dadd(_ddotp4h(_llto128(job_shift1(w0, w1), w0), hh0),
			    _ddotp4h(_llto128(job_shift1(w1, w2), w1), hh1));
		s23 = _dadd(_ddotp4h(_llto128(job_shift3(w0, w1), job_shift2(w0, w1)), hh0),
			    _ddotp4h(_llto128(job_shift3(w1, w2), job_shift2(w1, w2)), hh1));
		s45 = _dadd(_ddotp4h(_llto128(job_shift1(w1, w2), w1), hh0),
			    _ddotp4h(_llto128(job_shift1(w2, w3), w2), hh1));
		s67 = _dadd(_ddotp4h(_llto128(job_shift3(w1, w2), job_shift2(w1, w2)), hh0),
			    _ddotp4h(_llto128(job_shift3(w2, w3), job_shift2(w2, w3)), hh1));
		if (first) {
			ab[0] = s01;
			ab[1] = s23;
			ab[2] = s45;
			ab[3] = s67;
		} else {
			ab[0] = _dadd(ab[0], s01);
			ab[1] = _dadd(ab[1], s23);
			ab[2] = _dadd(ab[2], s45);
			ab[3] = _dadd(ab[3], s67);
		}
	}
}
#endif

static inline int16_t job_sat(int32_t acc)
{
	acc >>= 15;
	return acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)acc;
}

static void job_fir(const int16_t *restrict x, const int16_t *restrict h,
		    int16_t *restrict y, uint32_t count, uint32_t taps)
{
	uint32_t n = 0, k;

	_nassert((uintptr_t)x % 8 == 0);
	_nassert((uintptr_t)h % 8 == 0);
#ifdef __C7000__
	/*
	 * The C7x does 128 outputs at a time, 2 taps per step, taps outside,
	 * then 32 at a time. VDOTP2HW multiplies each pair of 16-bit samples
	 * in a vector by a pair of taps and adds the two products into one
	 * 32-bit lane. A 32-sample load from x[n + k] pairs x[n + 2i + k] with
	 * x[n + 2i + k + 1], which is what even output n + 2i needs for taps k
	 * and k + 1; the load from one sample on does the same for the odd
	 * outputs. So 32 outputs take two loads and two dot products a step,
	 * and the sums stay in registers. An odd tap count is padded with a
	 * zero tap; the one sample past the input that the last step then
	 * reads counts for nothing. 128 outputs a block, not 32, spreads the
	 * cost of starting and ending the tap loop: 17.8 us for 4096 x 32 at
	 * 32, 11.3 at 64, 9.6 at 128.
	 *
	 * Reading x straight from L2 SRAM takes 4096 x 32 from 9.6 us to 38, so
	 * it first copies the input and taps into its own cached DDR.
	 */
	uint32_t i, taps2 = (taps + 1u) & ~1u;

	/* Whole vectors: the block and job_xw both have room for the last one. */
#pragma MUST_ITERATE(1, (AI64_JOB_MAX_COUNT + AI64_JOB_MAX_TAPS) / 32)
	for (i = 0; i < (count + taps + 30u) / 32u; i++) {
		((short32 *)job_xw)[i] = ((const short32 *)x)[i];
	}
	for (i = 0; i < taps2; i++) {
		job_hw[i] = i < taps ? h[i] : 0;
	}
	for (i = 0; i < taps2 / 2u; i++) {
		job_hp[i] = (int32_t)(((uint32_t)(uint16_t)job_hw[2u * i + 1u] << 16) |
				      (uint16_t)job_hw[2u * i]);
	}
	x = job_xw;
	h = job_hw;
	for (; n + 128u <= count; n += 128u) {
		int16 e0 = (int16)0, o0 = (int16)0, e1 = (int16)0, o1 = (int16)0;
		int16 e2 = (int16)0, o2 = (int16)0, e3 = (int16)0, o3 = (int16)0;

#pragma MUST_ITERATE(1, AI64_JOB_MAX_TAPS / 2)
		for (k = 0; k < taps2; k += 2u) {
			short32 hk = __as_short32((int16)job_hp[k / 2u]);
			const int16_t *xk = &x[n + k];

			e0 += __dotp2(*(const short32 *)&xk[0], hk);
			o0 += __dotp2(*(const short32 *)&xk[1], hk);
			e1 += __dotp2(*(const short32 *)&xk[32], hk);
			o1 += __dotp2(*(const short32 *)&xk[33], hk);
			e2 += __dotp2(*(const short32 *)&xk[64], hk);
			o2 += __dotp2(*(const short32 *)&xk[65], hk);
			e3 += __dotp2(*(const short32 *)&xk[96], hk);
			o3 += __dotp2(*(const short32 *)&xk[97], hk);
		}
		*(short32 *)&y[n] = job_c7x_out(e0, o0);
		*(short32 *)&y[n + 32u] = job_c7x_out(e1, o1);
		*(short32 *)&y[n + 64u] = job_c7x_out(e2, o2);
		*(short32 *)&y[n + 96u] = job_c7x_out(e3, o3);
	}
	for (; n + 32u <= count; n += 32u) {
		int16 even = (int16)0, odd = (int16)0;

#pragma MUST_ITERATE(1, AI64_JOB_MAX_TAPS / 2)
		for (k = 0; k < taps2; k += 2u) {
			short32 hk = __as_short32((int16)job_hp[k / 2u]);

			even += __dotp2(*(const short32 *)&x[n + k], hk);
			odd += __dotp2(*(const short32 *)&x[n + k + 1u], hk);
		}
		*(short32 *)&y[n] = job_c7x_out(even, odd);
	}
#elif defined(_TMS320C6600)
	/*
	 * The C66x does 8 taps at a time, taps outside: each pass runs over
	 * all the outputs, 8 at a time, and adds to their sums in job_acc.
	 * Four aligned loads hold x[n + k] to x[n + k + 15]; shifting them by
	 * one to three samples gives each output its own 4 samples, and each
	 * DDOTP4H is two 4-tap dot products, 8 multiplies. The taps are padded
	 * with zeros to a multiple of 8, so the samples past the input that the
	 * last pass reads count for nothing; they are still inside the block.
	 * The outputs go in chunks of JOB_C66X_CHUNK, so that a chunk's sums
	 * and samples stay in the 32 KiB L1D across the passes. 4096 x 32 took
	 * 92 us as plain C, and 35 us with the taps inside.
	 */
	uint32_t taps8 = (taps + 7u) & ~7u;
	uint32_t c, blocks;

	for (k = 0; k < taps8; k++) {
		job_h8[k] = k < taps ? h[k] : 0;
	}
	for (; n + 8u <= count; n += 8u * blocks) {
		blocks = JOB_MIN_U32((count - n) / 8u, JOB_C66X_CHUNK / 8u);
		job_c66x_pass(x + n, job_acc, _amem8_const(&job_h8[0]),
			      _amem8_const(&job_h8[4]), blocks, 1);
		for (k = 8; k < taps8; k += 8u) {
			job_c66x_pass(x + n + k, job_acc, _amem8_const(&job_h8[k]),
				      _amem8_const(&job_h8[k + 4u]), blocks, 0);
		}
#pragma MUST_ITERATE(8, JOB_C66X_CHUNK, 8)
		for (c = 0; c < 8u * blocks; c++) {
			y[n + c] = job_sat(job_acc[c]);
		}
	}
#endif
#pragma MUST_ITERATE(0, AI64_JOB_MAX_COUNT)
	for (; n < count; n++) {
		int32_t acc = 0;

#pragma MUST_ITERATE(1, AI64_JOB_MAX_TAPS)
		for (k = 0; k < taps; k++) {
			acc += (int32_t)h[k] * x[n + k];
		}
		y[n] = job_sat(acc);
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
