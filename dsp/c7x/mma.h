/* SPDX-License-Identifier: MIT */
#ifndef AI64_C7X_MMA_H_
#define AI64_C7X_MMA_H_

#include <stdint.h>

/*
 * A fully connected int8 layer, 64 inputs to 64 outputs, on rows rows of a
 * (a multiple of 16): y = round((a x b) >> shift), saturated to int8, or
 * with relu to uint8 (0 to 255). a is rows x 64, b is 64 x 64, y is
 * rows x 64, row-major.
 * The _mma version runs on the C7x's Matrix Multiply Accelerator, the _c
 * version in C on the C7x itself (ai64_c7x_mma.c).
 */
void ai64_c7x_layer_mma(const int8_t *a, const int8_t *b, int8_t *y, uint32_t rows,
			uint32_t shift, uint32_t relu);
void ai64_c7x_layer_c(const int8_t *a, const int8_t *b, int8_t *y, uint32_t rows,
		      uint32_t shift, uint32_t relu);

#endif
