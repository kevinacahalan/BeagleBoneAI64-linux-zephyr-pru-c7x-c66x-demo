/* SPDX-License-Identifier: MIT */
/*
 * ai64 sched [seconds|once]
 *   Shows ICSSG1's multicore scheduler: IEP0's period, each core's task and
 *   compare, and what P8.17 spends its time at. ICSSG1 has no talking core,
 *   so this reads its registers, and only reads them.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"

/*
 * ICSSG1's multicore scheduler (pru/multicore_scheduler): which IEP0
 * compare starts each core's task, and what the task writes to PWM2_0's
 * initial state, which P8.17 shows. In the order the tasks run.
 */
static const struct {
	const char *core;
	uint32_t tm;     /* the core's task manager, from TM_CFG */
	int high;        /* 1 sets P8.17 high, 0 low */
} mcs_tasks[] = {
	{ "PRU0", 0x000, 1 },
	{ "PRU1", 0x200, 0 },
	{ "RTU1", 0x300, 1 },
	{ "RTU0", 0x100, 0 },
	{ "Tx_PRU1", 0x500, 0 },
	{ "Tx_PRU0", 0x400, 1 },
};

#define ICSSG1_IEP0  0x0b12e000
#define ICSSG1_CFG   0x0b126000
#define ICSSG1_TM    0x0b12a000

static uint32_t iep_cmp(volatile uint32_t *iep, uint32_t n)
{
	return iep[(n < 8 ? 0x78 + 8 * n : 0xc0 + 8 * (n - 8)) / 4];
}

int sched_main(int argc, char **argv)
{
	volatile uint32_t *iep = map_phys(ICSSG1_IEP0, 4096);
	volatile uint32_t *cfg = map_phys(ICSSG1_CFG, 4096);
	volatile uint32_t *tm = map_phys(ICSSG1_TM, 4096);
	int once, every = every_arg(argc, argv, &once);
	char state[32];
	size_t i;

	if (every <= 0) {
		fprintf(stderr, "usage: %s sched [seconds|once]\n", argv[0]);
		return 2;
	}
	if (!iep || !cfg || !tm) {
		return 1;
	}
	if (!once) {
		printf("sched window ready, ICSSG1's schedule every %d s\n", every);
	}
	for (;;) {
		uint32_t high = 0, samples = 0, changes = 0, last = 0, gcfg, period;

		if (!rproc_running(core_get("g1pru0")->rproc, state, sizeof(state))) {
			printf("ICSSG1 PRU0, which sets the scheduler up, is not running\n");
		} else if (!((gcfg = iep[0]) & 1)) {
			printf("ICSSG1 IEP0 is stopped: the scheduler is not set up\n");
		} else {
			period = iep_cmp(iep, 0) + ((gcfg >> 4) & 0xf);
			printf("ICSSG1 multicore scheduler: IEP0 period %u ns, +%u ns a tick (%s)\n",
			       period, (gcfg >> 4) & 0xf,
			       cfg[0x30 / 4] & 1 ? "the 250 MHz core clock" : "the IEP clock");
			for (i = 0; i < sizeof(mcs_tasks) / sizeof(mcs_tasks[0]); i++) {
				uint32_t t = mcs_tasks[i].tm / 4;
				uint32_t ev = tm[t + 0x40 / 4], on = tm[t] == 0x82;

				printf("  %-8s IEP0 CMP%-2u at %5u ns  sets P8.17 %-4s  task manager %s\n",
				       mcs_tasks[i].core, ev >= 16 && ev < 32 ? ev - 16 : 99,
				       ev >= 16 && ev < 32 ? iep_cmp(iep, ev - 16) : 0,
				       mcs_tasks[i].high ? "high" : "low",
				       on ? "on" : "OFF");
			}
			/* A bus read takes far longer than a task; sample many. */
			for (samples = 0; samples < 100000; samples++) {
				uint32_t v = cfg[0x158 / 4] & 0xf;

				high += v == 0xa;
				changes += samples && v != last;
				last = v;
			}
			printf("  P8.17 (PWM2_0's initial state) read high %.0f%% of %u samples, "
			       "%u changes seen\n", 100.0 * high / samples, samples, changes);
		}
		fflush(stdout);
		if (once) {
			return 0;
		}
		sleep((unsigned int)every);
	}
}
