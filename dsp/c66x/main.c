/* SPDX-License-Identifier: MIT */
/*
 * C66x hello image. It writes a hello into its remoteproc trace buffer,
 * then one line every 10 s, timed with the core's TSCL cycle counter.
 * Everything lives in the core's own L2 SRAM; it touches no DDR, and the
 * only peripheral is its mailbox, so it needs no cache or MMU setup.
 */

#include <stdint.h>
#include <c6x.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c66x"
#include "ai64_dsp_trace.h"
#include "ai64_dsp_mbox.h"

/*
 * Mailbox cluster 3 (0x31f83000) carries both C66x cores. Linux receives
 * from C66x 0 on FIFO 0 and sends to it on FIFO 1; C66x 1 uses 2 and 3.
 */
#define MBOX_BASE 0x31f83000u

/* The C66x cores run at 1.35 GHz on the BeagleBone AI-64 (k3conf). */
#define C66X_HZ 1350000000u

int main(void)
{
	uint32_t seconds = 0;
	uint32_t last;

	trace_init();
	trace_puts("hello from the c66x, DNUM ");
	trace_u32(DNUM);
	trace_putc('\n');
	trace_mark();

	/* Any write starts TSCL counting. */
	TSCL = 0;
	last = TSCL;
	for (;;) {
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
