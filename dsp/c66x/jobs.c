/* SPDX-License-Identifier: MIT */
/*
 * The C66x's job block (ai64_dsp_job.h), in its own L2 SRAM: Linux hands it
 * FIR filter jobs one at a time. L1D snoops Linux's writes to L2 SRAM, so
 * neither side does cache work.
 */

#include <stdint.h>
#include <c6x.h>

#include "image.h"

#define AI64_JOB_LOCAL    AI64_JOB_C66X_LOCAL
#define AI64_JOB_CYCLES() TSCL
#define AI64_JOB_FENCE()  _mfence()
#include "ai64_dsp_job.h"

void jobs_start(void)
{
	job_init();
}

int jobs_poll(void)
{
	return job_poll();
}
