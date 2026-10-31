/* SPDX-License-Identifier: MIT */
#ifndef DSP_C66X_PIPE_H_
#define DSP_C66X_PIPE_H_

#include <stdint.h>

/*
 * The C66x cores' stage of the pipeline (pipe.c). init makes the
 * FFT's tables, takes gtc_lo to read the GTC's low word with, and zeroes
 * the stage's numbers. poll runs every window R5F0_0 has brought this core
 * since the last call; 1 when there were any.
 */
void ai64_c66x_pipe_init(uint32_t (*gtc_lo)(void));
int ai64_c66x_pipe_poll(void);

#endif
