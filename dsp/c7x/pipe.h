/* SPDX-License-Identifier: MIT */
#ifndef DSP_C7X_PIPE_H_
#define DSP_C7X_PIPE_H_

#include <stdint.h>

/*
 * The C7x's stage of the pipeline (pipe.c). init clears the
 * weights, takes gtc_lo to read the GTC's low word with, and zeroes the
 * stage's numbers; poll classifies every window the C66x cores have
 * written since the last call, 1 when there were any.
 */
void ai64_c7x_pipe_init(uint32_t (*gtc_lo)(void));
int ai64_c7x_pipe_poll(void);

#endif
