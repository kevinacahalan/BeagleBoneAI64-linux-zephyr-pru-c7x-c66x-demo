/* SPDX-License-Identifier: MIT */
/*
 * C7x hello image. It writes a hello into its remoteproc trace buffer,
 * then one line every 10 s, timed with the core's TSC cycle counter.
 * It runs from the core's DDR carveout with the MMU off, as the core
 * comes out of reset, and touches no peripheral.
 */

#include <stdint.h>
#include <c7x.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c7x"
#include "ai64_dsp_trace.h"

/* The C71x core runs at 1 GHz on the BeagleBone AI-64 (k3conf). */
#define C7X_HZ 1000000000ull

int main(void)
{
	uint32_t seconds = 0;
	uint64_t last;

	trace_init();
	trace_puts("hello from the c7x, DNUM ");
	trace_u32((uint32_t)__DNUM);
	trace_putc('\n');
	trace_mark();

	last = __TSC;
	for (;;) {
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
