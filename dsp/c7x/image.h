/* SPDX-License-Identifier: MIT */
#ifndef DSP_C7X_IMAGE_H_
#define DSP_C7X_IMAGE_H_

#include <stdint.h>

/* The C7x image's parts, which main.c starts and polls. */

/* fabric.c: join the fabric as node; poll answers what has come in. The
 * MMU (mmu.c) maps the fabric and the GTC. */
void fabric_start(uint32_t node);
void fabric_poll(void);
/* The GTC's low word. */
uint32_t gtc_lo(void);

/* jobs.c: the job block Linux hands FIR and layer jobs through
 * (ai64_dsp_job.h); poll runs one if there is one, 1 when it did. */
void jobs_start(void);
int jobs_poll(void);

#endif
