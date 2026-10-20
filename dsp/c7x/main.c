/* SPDX-License-Identifier: MIT */
/*
 * C7x image. It writes a hello into its remoteproc trace buffer, then one
 * line every 10 s, timed with the core's TSC cycle counter, and runs the
 * jobs Linux hands it (ai64_dsp_job.h) from the start of its L2 SRAM.
 * It runs from the core's DDR carveout. It turns the MMU and caches on
 * first (ai64_c7x_mmu.c). The only peripheral it touches is its mailbox.
 */

#include <stdint.h>
#include <c7x.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c7x"
#include "ai64_dsp_trace.h"
#include "ai64_dsp_mbox.h"
#include "ai64_c7x_mmu.h"

#define AI64_JOB_LOCAL    AI64_JOB_C7X_LOCAL
#define AI64_JOB_CYCLES() __TSC
#define AI64_JOB_FENCE()  __memory_fence(0x1)
/* The job block past its header is cached. */
#define AI64_JOB_CACHE_IN()  do { ai64_l1d_wbinv_all(); AI64_JOB_FENCE(); } while (0)
#define AI64_JOB_CACHE_OUT() do { ai64_l1d_wb_all(); AI64_JOB_FENCE(); } while (0)
#include "ai64_dsp_job.h"

/* Mailbox cluster 4 (0x31f84000): Linux receives on FIFO 0, sends on FIFO 1. */
#define MBOX_BASE 0x31f84000u

/* The C71x core runs at 1 GHz on the BeagleBone AI-64 (k3conf). */
#define C7X_HZ 1000000000ull

int main(void)
{
	uint32_t seconds = 0, jobs = 0;
	uint64_t last;

	ai64_c7x_mmu_init();
	trace_init();
	trace_puts("hello from the c7x, DNUM ");
	trace_u32((uint32_t)__DNUM);
	trace_puts((ai64_get_scr() & 1u) ? ", MMU on" : ", MMU off");
	trace_puts(", L1DCFG ");
	trace_u32((uint32_t)ai64_get_l1dcfg());
	trace_puts(", L2CFG ");
	trace_u32((uint32_t)ai64_get_l2cfg());
	trace_putc('\n');
	trace_mark();
	job_init();

	last = __TSC;
	for (;;) {
		if (job_poll() && ++jobs == 1u) {
			trace_puts("first job done\n");
		}
		if (mbox_shutdown_requested(MBOX_BASE, 1u, 0u)) {
			trace_puts("c7x shutting down\n");
			ai64_l2_wbinv_all();
			/* Interrupts are off (GIE clear since reset), so IDLE stays. */
			for (;;) {
				__asm(" IDLE");
			}
		}
		if (__TSC - last >= C7X_HZ) {
			last += C7X_HZ;
			seconds++;
			if (seconds % 10u == 0) {
				trace_puts("c7x up ");
				trace_u32(seconds);
				trace_puts(" s\n");
			}
		}
	}
}
