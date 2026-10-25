/* SPDX-License-Identifier: MIT */
#ifndef AI64_C7X_PIPE_H_
#define AI64_C7X_PIPE_H_

#include <stdint.h>

/*
 * The C7x's stage of the pipeline (ai64_c7x_pipe.c). init clears the
 * weights, takes gtc_lo to read the GTC's low word with, and zeroes the
 * stage's numbers; poll classifies the newest window C66x 0 has written
 * since the last call, 1 when there was one.
 */
void ai64_c7x_pipe_init(uint32_t (*gtc_lo)(void));
int ai64_c7x_pipe_poll(void);

#endif
