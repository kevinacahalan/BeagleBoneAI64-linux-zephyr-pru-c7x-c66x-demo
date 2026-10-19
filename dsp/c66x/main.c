/* SPDX-License-Identifier: MIT */
/*
 * C66x hello image. It writes a hello into its remoteproc trace buffer,
 * then one line every 10 s, timed with the core's TSCL cycle counter.
 * Everything lives in the core's own L2 SRAM; it touches no DDR and no
 * peripheral, so it needs no cache or MMU setup.
 */

#include <stdint.h>
#include <c6x.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c66x"
#include "ai64_dsp_trace.h"

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
