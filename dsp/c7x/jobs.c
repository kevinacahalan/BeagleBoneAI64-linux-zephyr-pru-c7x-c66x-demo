/* SPDX-License-Identifier: MIT */
/*
 * The C7x's job block (ai64_dsp_job.h), at the start of its L2 SRAM: Linux
 * hands it FIR filter jobs and int8 layers (mma.c) one at a time. The MMU
 * maps the block's header non-cacheable and the rest write-back, so a job
 * writes L1D back around itself.
 */

#include <stdint.h>
#include <c7x.h>

#include "image.h"
#include "mma.h"
#include "mmu.h"

#define AI64_JOB_LOCAL    AI64_JOB_C7X_LOCAL
#define AI64_JOB_CYCLES() __TSC
#define AI64_JOB_FENCE()  __memory_fence(0x1)
/* The job block past its header is cached. */
#define AI64_JOB_CACHE_IN()  do { ai64_l1d_wbinv_all(); AI64_JOB_FENCE(); } while (0)
#define AI64_JOB_CACHE_OUT() do { ai64_l1d_wb_all(); AI64_JOB_FENCE(); } while (0)
#include "ai64_dsp_job.h"

void jobs_start(void)
{
	job_init();
}

int jobs_poll(void)
{
	return job_poll();
}
