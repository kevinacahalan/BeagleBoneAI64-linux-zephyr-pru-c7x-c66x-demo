/* SPDX-License-Identifier: MIT */
/*
 * The C7x image. It runs from the core's DDR carveout, and:
 *
 *   - turns its MMU and caches on, before anything else (mmu.c);
 *   - logs into its remoteproc trace buffer (ai64_dsp_trace.h): a hello,
 *     then a line every 10 s, timed with the core's TSC cycle counter;
 *   - runs the jobs Linux hands it (jobs.c): FIR filters, and an int8
 *     layer on its Matrix Multiply Accelerator (mma.c);
 *   - is the pipeline's classifier, the same layer on the C66x cores'
 *     features (pipe.c);
 *   - is a fabric node (fabric.c);
 *   - goes idle when Linux asks it to, before a stop (ai64_dsp_mbox.h).
 */

#include <stdint.h>
#include <c7x.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c7x"
#include "ai64_dsp_trace.h"
#include "ai64_dsp_mbox.h"
#include "ai64_fabric.h"
#include "image.h"
#include "mmu.h"
#include "pipe.h"

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
	jobs_start();
	fabric_start(AI64_FAB_C7X);
	ai64_c7x_pipe_init(gtc_lo);

	last = __TSC;
	for (;;) {
		if (jobs_poll() && ++jobs == 1u) {
			trace_puts("first job done\n");
		}
		(void)ai64_c7x_pipe_poll();
		fabric_poll();
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
