/* SPDX-License-Identifier: MIT */
/*
 * The C66x image; both C66x cores run it, and DNUM tells them apart. It
 * runs from the core's own L2 SRAM, so it needs no cache setup, and:
 *
 *   - logs into its remoteproc trace buffer (ai64_dsp_trace.h): a hello,
 *     then a line every 10 s, timed with the core's TSCL cycle counter;
 *   - runs the FIR jobs Linux hands it (jobs.c);
 *   - is the pipeline's FFT stage, C66x 0 for the even windows and C66x 1
 *     for the odd ones (pipe.c);
 *   - is a fabric node (fabric.c);
 *   - goes idle when Linux asks it to, before a stop (ai64_dsp_mbox.h).
 */

#include <stdint.h>
#include <c6x.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c66x"
#include "ai64_dsp_trace.h"
#include "ai64_dsp_mbox.h"
#include "ai64_fabric.h"
#include "image.h"
#include "pipe.h"

/*
 * Mailbox cluster 3 (0x31f83000) carries both C66x cores. Linux receives
 * from C66x 0 on FIFO 0 and sends to it on FIFO 1; C66x 1 uses 2 and 3.
 */
#define MBOX_BASE 0x31f83000u

/* The C66x cores run at 1.35 GHz on the BeagleBone AI-64 (k3conf). */
#define C66X_HZ 1350000000u

int main(void)
{
	uint32_t seconds = 0, jobs = 0;
	uint32_t last;

	trace_init();
	trace_puts("hello from the c66x, DNUM ");
	trace_u32(DNUM);
	trace_putc('\n');
	trace_mark();
	jobs_start();
	fabric_start(AI64_FAB_C66X0 + DNUM);
	ai64_c66x_pipe_init(gtc_lo);
	trace_puts(DNUM ? "pipeline: FFT stage up, odd windows\n" :
		   "pipeline: FFT stage up, even windows\n");

	/* Any write starts TSCL counting. */
	TSCL = 0;
	last = TSCL;
	for (;;) {
		if (jobs_poll() && ++jobs == 1u) {
			trace_puts("first job done\n");
		}
		(void)ai64_c66x_pipe_poll();
		fabric_poll();
		if (mbox_shutdown_requested(MBOX_BASE, 2u * DNUM + 1u, 2u * DNUM)) {
			trace_puts("c66x shutting down\n");
			/* Interrupts are off (GIE clear since reset), so IDLE stays. */
			for (;;) {
				asm(" IDLE");
			}
		}
		if (TSCL - last >= C66X_HZ) {
			last += C66X_HZ;
			seconds++;
			if (seconds % 10u == 0) {
				trace_puts("c66x up ");
				trace_u32(seconds);
				trace_puts(" s\n");
			}
		}
	}
}
