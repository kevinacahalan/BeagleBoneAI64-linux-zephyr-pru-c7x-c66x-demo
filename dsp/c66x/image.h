/* SPDX-License-Identifier: MIT */
#ifndef DSP_C66X_IMAGE_H_
#define DSP_C66X_IMAGE_H_

#include <stdint.h>

/* The C66x image's parts, which main.c starts and polls. */

/* fabric.c: map the GTC and the shared memory, and join the fabric as
 * node; poll answers what has come in. */
void fabric_start(uint32_t node);
void fabric_poll(void);
/* The GTC's low word, through fabric.c's MPAX window. */
uint32_t gtc_lo(void);

/* jobs.c: the job block Linux hands FIR jobs through (ai64_dsp_job.h);
 * poll runs one if there is one, 1 when it did. */
void jobs_start(void);
int jobs_poll(void);

#endif
