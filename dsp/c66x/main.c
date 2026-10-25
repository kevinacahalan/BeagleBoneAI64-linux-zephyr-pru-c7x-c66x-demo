/* SPDX-License-Identifier: MIT */
/*
 * C66x image. It writes a hello into its remoteproc trace buffer, then one
 * line every 10 s, timed with the core's TSCL cycle counter, and runs the
 * jobs Linux hands it (ai64_dsp_job.h). Everything lives in the core's own
 * L2 SRAM, the job block included, so it needs no cache setup. It is also
 * a fabric node (ai64_fabric.h): it answers and sends pings, passes tours
 * and routes on, and adds to the spinlock tally. C66x 0 is also the
 * pipeline's FFT stage (ai64_c66x_pipe.c).
 */

#include <stdint.h>
#include <c6x.h>
#include <string.h>

#define AI64_TRACE_SIZE 2048
#define AI64_TRACE_NAME "trace:c66x"
#include "ai64_dsp_trace.h"
#include "ai64_dsp_mbox.h"
#include "ai64_c66x_pipe.h"
#include "ai64_pipe.h"

#define AI64_JOB_LOCAL    AI64_JOB_C66X_LOCAL
#define AI64_JOB_CYCLES() TSCL
#define AI64_JOB_FENCE()  _mfence()
#include "ai64_dsp_job.h"

/*
 * The C66x cannot reach the GTC at its own address: everything below
 * 0x0c000000 is the core's internal space. MPAX segment 2 maps one logical
 * page, AI64_C66X_GTC, onto it. The XMC's segment registers are pairs,
 * XMPAXL (the replacement address) then XMPAXH (the logical base and
 * size). The MAR registers of that page and of the fabric's 16 MiB reset
 * with XMC prefetch (PFX) on. Clearing them makes every read go to the GTC
 * or to DDR. The pipeline writes the C7x's L2 SRAM, which gets the same.
 */
#define AI64_C66X_GTC 0x7ffff000u
#define XMPAXL(n)     (*(volatile uint32_t *)(0x08000000u + 8u * (n)))
#define XMPAXH(n)     (*(volatile uint32_t *)(0x08000004u + 8u * (n)))
#define MPAX_4K       0x0bu         /* segment size 2^(11 + 1) */
#define MPAX_RWX      0xbfu         /* supervisor and user read, write, execute */
#define MAR(addr)     (*(volatile uint32_t *)(0x01848000u + 4u * ((addr) >> 24)))

#define AI64_FAB_NOW()    (*(volatile uint32_t *)(AI64_C66X_GTC + 8u))
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_FENCE()  _mfence()
/* The fabric is normal memory here, so memcpy can move it in bursts. */
#define AI64_FAB_COPY_IN(d, s, n)  memcpy((d), (const void *)(s), 4u * (n))
#define AI64_FAB_COPY_OUT(d, s, n) memcpy((void *)(d), (s), 4u * (n))
#include "ai64_fabric.h"

static void fabric_map(void)
{
	MAR(AI64_C66X_GTC) = 0;
	MAR(AI64_FAB_ADDR) = 0;
	MAR(AI64_PIPE_C7X_ADDR) = 0;
	XMPAXL(2) = ((AI64_GTC_ADDR >> 12) << 8) | MPAX_RWX;
	XMPAXH(2) = AI64_C66X_GTC | MPAX_4K;
	_mfence();
}

static uint32_t gtc_lo(void)
{
	return AI64_FAB_NOW();
}

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
	job_init();
	fabric_map();
	fab_init(AI64_FAB_C66X0 + DNUM);
	if (DNUM == 0) {
		ai64_c66x_pipe_init(gtc_lo);
		trace_puts("pipeline: FFT stage up\n");
	}

	/* Any write starts TSCL counting. */
	TSCL = 0;
	last = TSCL;
	for (;;) {
		if (job_poll() && ++jobs == 1u) {
			trace_puts("first job done\n");
		}
		if (DNUM == 0) {
			(void)ai64_c66x_pipe_poll();
		}
		fab_poll();
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
