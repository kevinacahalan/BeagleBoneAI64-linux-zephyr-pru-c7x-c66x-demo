/* SPDX-License-Identifier: MIT */
/*
 * An int8 neural-network layer on the C7x's Matrix Multiply Accelerator.
 *
 *   s = round((sum of a[r][k] * b[k][j], k < 64) >> shift)
 *   y[r][j] = relu ? s clamped to 0..255 (uint8) : s clamped to -128..127 (int8)
 *
 * for rows r < rows (a multiple of 16) and columns j < 64: a fully
 * connected layer of 64 inputs and 64 outputs, run on a batch of rows. The
 * same layer runs in plain C too (ai64_c7x_layer_c), so Linux can compare.
 *
 * With ReLU the outputs are unsigned, as a quantized network keeps them.
 * Asked for signed int8 with ReLU, the MMA on this board saturates a sum
 * below -128 to -128 and leaves it there, where TI's emulator (and the
 * header's description) applies ReLU after saturation and gives 0.
 *
 * The MMA multiplies a 64-element vector A by a 64 x 64 matrix B held in
 * one of its two B banks, and writes the 64 sums, 32-bit, to a row of one
 * of its two C banks; a C bank holds 16 such rows. Its X stage then reads a
 * C row out through a transfer buffer, here as 64 int8s after the shift,
 * rounding, saturation, and ReLU, which the C7x takes back with HWARCV. The
 * intrinsics are the compiler's (c7x_mma.h); TI's C7000 Host Emulation runs
 * this same file on a PC, where __HWAADV() steps its model of the MMA, and
 * on the C7x that macro is empty.
 *
 * The C7x's two Streaming Engines feed the MMA: SE1 streams B's 64 rows
 * and SE0 A's rows, a 64-byte vector each, fetched ahead of use, where
 * plain loads from L2 SRAM held each HWALDA up (40 cycles a row).
 *
 * The rows go 16 at a time, alternating C banks. B is loaded once, 64
 * HWALDB, before the first block. Then each row of a block is one HWALDA
 * and one HWAOPXFER: the op computes the row into one C bank while the
 * transfer reads the same row of the block before out of the other, and
 * HWARCV takes it back. The first block has nothing to transfer yet (plain
 * HWAOP), and the last is read out on its own (HWAXFER), after a fence.
 */

#ifdef __C7X_HOSTEM__
#include "c7x_host_emulation.h"
#else
#include <c7x.h>
#endif
#include <stdint.h>
#include <string.h>

#include "ai64_c7x_mma.h"

#define K 64u
#define N 64u
#define BLOCK 16u

/*
 * The plain C version: what the compiler makes of the loops for the C7x's
 * own vector units, no intrinsics. The clamp is branch-free, so the output
 * loop vectorizes too.
 */
void ai64_c7x_layer_c(const int8_t *restrict a, const int8_t *restrict b, int8_t *restrict y,
		      uint32_t rows, uint32_t shift, uint32_t relu)
{
	int32_t lo = relu ? 0 : -128, hi = relu ? 255 : 127;
	int32_t half = shift ? 1 << (shift - 1u) : 0;
	uint32_t r, j, k;

	for (r = 0; r < rows; r++) {
		int32_t acc[N];

#pragma MUST_ITERATE(N, N, N)
		for (j = 0; j < N; j++) {
			acc[j] = 0;
		}
#pragma MUST_ITERATE(K, K, K)
		for (k = 0; k < K; k++) {
			int32_t ak = a[r * K + k];

#pragma MUST_ITERATE(N, N, N)
			for (j = 0; j < N; j++) {
				acc[j] += ak * b[k * N + j];
			}
		}
#pragma MUST_ITERATE(N, N, N)
		for (j = 0; j < N; j++) {
			int32_t v = (acc[j] + half) >> shift;

			v = v < lo ? lo : v;
			v = v > hi ? hi : v;
			y[r * N + j] = (int8_t)v;
		}
	}
}

/* Open Streaming Engine se on bytes bytes from p, 64 at a time. */
static void se_open(int se, const void *p, uint32_t bytes)
{
	__SE_FLAGS flags = __SE_FLAGS_default();
	/* A template is a __uint16; the two compilers spell a vector of zeros differently. */
#ifdef __C7X_HOSTEM__
	__STRM_TEMPLATE params = __uint16(0);
#else
	__STRM_TEMPLATE params = (__uint16)(0);
#endif

	flags.DIMFMT = __SE_DIMFMT_1D;
	flags.ELETYPE = __SE_ELETYPE_8BIT;
	flags.VECLEN = __SE_VECLEN_64ELEMS;
	params = __set_ICNT0(params, bytes);
	params = __set_SE_FLAGS(params, &flags, NULL);
	if (se) {
		__SE1_OPEN(p, params);
	} else {
		__SE0_OPEN(p, params);
	}
}

void ai64_c7x_layer_mma(const int8_t *a, const int8_t *b, int8_t *y, uint32_t rows,
			uint32_t shift, uint32_t relu)
{
	__HWA_CONFIG_REG cfg;
	__HWA_OFFSET_REG off;
	uint32_t r, blk;

	memset(&cfg, 0, sizeof(cfg));
	memset(&off, 0, sizeof(off));
	/* A: signed 8-bit; B: 64 rows of 8-bit, loaded once into bank 0. */
	cfg.A_ATYPE = __MMA_A_CONFIG_ATYPE_INT8;
	cfg.B_BTYPE = __MMA_B_CONFIG_SIZE8;
	cfg.B_ORDER = __MMA_B_CONFIG_ROW;
	cfg.B_BSWPER = K;
	cfg.B_BRSTPER = K;
	/* C = A x B, never accumulating, always from B bank 0. */
	cfg.C_ATYPE = __MMA_C_CONFIG_ATYPE_SA;
	cfg.C_BTYPE = __MMA_C_CONFIG_BTYPE_INT8;
	cfg.C_OPERATION0 = __MMA_C_CONFIG_MUL;
	cfg.C_OPERATION1 = __MMA_C_CONFIG_MUL;
	cfg.C_OP0PER = 0xffffffffu;
	cfg.C_BSWPER = 0xffffffffu;
	cfg.C_HWLDDST = __MMA_C_CONFIG_HWLDDST_X1;
	cfg.C_HWLDTYPE = __MMA_C_CONFIG_HWLDTYPE_INT32;
	cfg.C_CRSWPER = 0xffffffffu;
	cfg.C_CRRSTPER = BLOCK;
	/* Results: a C bank of 16 rows, then the other. */
	cfg.C_CWSWPER = BLOCK;
	cfg.C_CWRSTPER = BLOCK;
	/* X: int32 sums shifted, rounded, and saturated to int8, or with ReLU to uint8. */
	cfg.X_CTYPE = __MMA_X_CONFIG_CTYPE_INT32;
	cfg.X_XTYPE = relu ? __MMA_X_CONFIG_XTYPE_UINT8 : __MMA_X_CONFIG_XTYPE_INT8;
	cfg.X_SHIFT = shift;
	cfg.X_RE = shift ? 1u : 0u;
	cfg.X_SAT = 1;
	cfg.X_ReLU = relu ? 1u : 0u;
	cfg.X_CSWPER = BLOCK;
	cfg.X_CRRSTPER = BLOCK;
	cfg.PARITYCTRL = __MMA_NORMAL;

	__HWAOPEN(cfg, off, __MMA_OPEN_FSM_RESET);
	__HWAADV();
	se_open(1, b, K * N);
	se_open(0, a, rows * K);
	for (r = 0; r < K; r++) {
		__HWALDB(__SE1ADV(uchar64));
		__HWAADV();
	}
	__SE1_CLOSE();
	for (r = 0; r < BLOCK; r++) {
		__HWALDA(__SE0ADV(uchar64));
		__HWAOP(0);
		__HWAADV();
	}
	for (blk = BLOCK; blk < rows; blk += BLOCK) {
		/* The emulator's model needs the steps a block's ops take. */
		for (r = 0; r < BLOCK; r++) {
			__HWAADV();
		}
		for (r = blk; r < blk + BLOCK; r++) {
			__HWALDA(__SE0ADV(uchar64));
			__HWAOPXFER(0);
			__HWAADV();
			*(__uchar64_ptr)(uint8_t *)&y[(r - BLOCK) * N] = __HWARCV(0);
			__HWAADV();
		}
	}
	__SE0_CLOSE();
	/* Every computation of the last block is in its C bank before X reads it. */
	__HWAFENCE_OPXFER();
	for (r = 0; r < BLOCK; r++) {
		__HWAADV();
	}
	for (r = rows - BLOCK; r < rows; r++) {
		__HWAXFER(0);
		__HWAADV();
		*(__uchar64_ptr)(uint8_t *)&y[r * N] = __HWARCV(0);
		__HWAADV();
	}
	__HWACLOSE(0);
}
