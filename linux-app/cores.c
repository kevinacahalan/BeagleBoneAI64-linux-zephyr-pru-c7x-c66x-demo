/* SPDX-License-Identifier: MIT */
/*
 * Every demo core, once: ai64 reads this table for its subcommands, the web
 * page gets it in /status.json, and ai64-demo reads it from ai64 cores.
 *
 * ai64 cores
 *   Prints the table, a core a line, each group in the order ai64-demo
 *   starts it: group, label, remoteproc name, firmware, fabric node (- for
 *   none), and name.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai64.h"
#include "ai64_dsp_job.h"
#include "ai64_fabric.h"

const struct ai64_group ai64_groups[] = {
	{ "r5", "Zephyr on the main-domain R5F cores" },
	{ "g0", "ICSSG0: the PWM slices, and the pipeline's sampler" },
	{ "g1", "ICSSG1: TI's multicore scheduler, a task each on IEP0's time" },
	{ "dsp", "The DSPs" },
};
const size_t ai64_ngroups = sizeof(ai64_groups) / sizeof(ai64_groups[0]);

#define NONE .node = -1

/*
 * In each group the cores are listed as the web page shows them; start is
 * the order ai64-demo starts them in. In split mode an R5F cluster's core 1
 * cannot power up before its core 0, so each cluster's core 0 comes first.
 * An ICSSG0 slice's Tx_PRU waits for its RTU's record and its PRU for the
 * Tx_PRU's, so the RTU starts first. ICSSG1's PRU0 sets up IEP0 for the
 * other five, so it starts last.
 */
const struct ai64_core ai64_cores[] = {
	{ .label = "r5f0_0", .rproc = "5c00000.r5f", .fw = "ai64_r5f0_0.elf", .group = "r5",
	  .name = "R5F0_0", .role = "SPI7 and the UDMA; moves the pipeline's windows",
	  .start = 1, .node = AI64_FAB_R5F0_0, .mbox = 0x31f81000, .fifo = 1 },
	{ .label = "r5f0_1", .rproc = "5d00000.r5f", .fw = "ai64_r5f0_1.elf", .group = "r5",
	  .name = "R5F0_1", .role = "EHRPWM4 on P9.25; the pipeline's last stage",
	  .start = 3, .node = AI64_FAB_R5F0_1, .mbox = 0x31f81000, .fifo = 3 },
	{ .label = "r5f1_0", .rproc = "5e00000.r5f", .fw = "ai64_r5f1_0.elf", .group = "r5",
	  .name = "R5F1_0", .role = "a square wave on P8.18",
	  .start = 2, .node = AI64_FAB_R5F1_0, .mbox = 0x31f82000, .fifo = 1 },
	{ .label = "r5f1_1", .rproc = "5f00000.r5f", .fw = "ai64_r5f1_1.elf", .group = "r5",
	  .name = "R5F1_1", .role = "watches the other three over ipc_service",
	  .start = 4, .node = AI64_FAB_R5F1_1, .mbox = 0x31f82000, .fifo = 3 },

	{ .label = "g0pru0", .rproc = "b034000.pru", .fw = "ai64-icssg0-pru0.elf", .group = "g0",
	  .name = "PRU0", .role = "slice 0: PWM in software on P8.11, P8.12",
	  .start = 3, NONE, .icssg = 0x0b000000ull, .slice = 0 },
	{ .label = "g0txpru0", .rproc = "b00a000.txpru", .fw = "ai64-icssg0-txpru0.elf",
	  .group = "g0", .name = "Tx_PRU0", .role = "slice 0: the modulator, and eCAP0 on P8.15",
	  .start = 2, NONE, .icssg = 0x0b000000ull, .slice = 0 },
	{ .label = "g0rtu0", .rproc = "b004000.rtu", .fw = "ai64-icssg0-rtu0.elf", .group = "g0",
	  .name = "RTU0", .role = "slice 0 talks: RPMsg to Linux, the fabric",
	  .start = 1, .node = AI64_FAB_G0RTU0, .icssg = 0x0b000000ull, .slice = 0 },
	{ .label = "g0pru1", .rproc = "b038000.pru", .fw = "ai64-icssg0-pru1.elf", .group = "g0",
	  .name = "PRU1",
	  .role = "slice 1: the ICSSG PWM module on P9.12, P9.15; samples P8.31 at 2 MHz",
	  .start = 6, NONE, .icssg = 0x0b000000ull, .slice = 1 },
	{ .label = "g0txpru1", .rproc = "b00c000.txpru", .fw = "ai64-icssg0-txpru1.elf",
	  .group = "g0", .name = "Tx_PRU1", .role = "slice 1: the modulator",
	  .start = 5, NONE, .icssg = 0x0b000000ull, .slice = 1 },
	{ .label = "g0rtu1", .rproc = "b006000.rtu", .fw = "ai64-icssg0-rtu1.elf", .group = "g0",
	  .name = "RTU1", .role = "slice 1 talks; stamps the pipeline's windows",
	  .start = 4, .node = AI64_FAB_G0RTU1, .icssg = 0x0b000000ull, .slice = 1 },

	{ .label = "g1pru0", .rproc = "b134000.pru", .fw = "ai64-icssg1-pru0.elf", .group = "g1",
	  .name = "PRU0", .role = "sets the scheduler up; its task sets P8.17 high",
	  .start = 6, NONE, .icssg = 0x0b100000ull, .slice = 0 },
	{ .label = "g1pru1", .rproc = "b138000.pru", .fw = "ai64-icssg1-pru1.elf", .group = "g1",
	  .name = "PRU1", .role = "its task sets P8.17 low",
	  .start = 5, NONE, .icssg = 0x0b100000ull, .slice = 1 },
	{ .label = "g1rtu0", .rproc = "b104000.rtu", .fw = "ai64-icssg1-rtu0.elf", .group = "g1",
	  .name = "RTU0", .role = "its task sets P8.17 low",
	  .start = 1, NONE, .icssg = 0x0b100000ull, .slice = 0 },
	{ .label = "g1rtu1", .rproc = "b106000.rtu", .fw = "ai64-icssg1-rtu1.elf", .group = "g1",
	  .name = "RTU1", .role = "its task sets P8.17 high",
	  .start = 2, NONE, .icssg = 0x0b100000ull, .slice = 1 },
	{ .label = "g1txpru0", .rproc = "b10a000.txpru", .fw = "ai64-icssg1-txpru0.elf",
	  .group = "g1", .name = "Tx_PRU0", .role = "its task sets P8.17 high",
	  .start = 3, NONE, .icssg = 0x0b100000ull, .slice = 0 },
	{ .label = "g1txpru1", .rproc = "b10c000.txpru", .fw = "ai64-icssg1-txpru1.elf",
	  .group = "g1", .name = "Tx_PRU1", .role = "its task sets P8.17 low",
	  .start = 4, NONE, .icssg = 0x0b100000ull, .slice = 1 },

	{ .label = "c66x0", .rproc = "4d80800000.dsp", .fw = "ai64-c66x.elf", .group = "dsp",
	  .name = "C66x 0", .role = "FIR jobs; the pipeline's FFT, even windows",
	  .start = 1, .node = AI64_FAB_C66X0, .mbox = 0x31f83000, .fifo = 1,
	  .job = AI64_JOB_C66X0_PHYS, .mhz = 1350,
	  .l2_da = 0x00800000, .l2_len = 0x00048000, .l2_pa = 0x4d80800000ull },
	{ .label = "c66x1", .rproc = "4d81800000.dsp", .fw = "ai64-c66x.elf", .group = "dsp",
	  .name = "C66x 1", .role = "FIR jobs; the pipeline's FFT, odd windows",
	  .start = 2, .node = AI64_FAB_C66X1, .mbox = 0x31f83000, .fifo = 3,
	  .job = AI64_JOB_C66X1_PHYS, .mhz = 1350,
	  .l2_da = 0x00800000, .l2_len = 0x00048000, .l2_pa = 0x4d81800000ull },
	{ .label = "c7x", .rproc = "64800000.dsp", .fw = "ai64-c7x.elf", .group = "dsp",
	  .name = "C7x", .role = "FIR and MMA layer jobs; classifies each window",
	  .start = 3, .node = AI64_FAB_C7X, .mbox = 0x31f84000, .fifo = 1,
	  .job = AI64_JOB_C7X_PHYS, .mhz = 1000,
	  .l2_da = 0x00800000, .l2_len = 0x00080000, .l2_pa = 0x64800000ull },
};
const size_t ai64_ncores = sizeof(ai64_cores) / sizeof(ai64_cores[0]);

const struct ai64_core *core_find(const char *s)
{
	size_t i;

	for (i = 0; i < ai64_ncores; i++) {
		if (!strcmp(ai64_cores[i].label, s) || !strcmp(ai64_cores[i].rproc, s)) {
			return &ai64_cores[i];
		}
	}
	return NULL;
}

const struct ai64_core *core_get(const char *label)
{
	const struct ai64_core *c = core_find(label);

	if (!c) {
		fprintf(stderr, "ai64: no core %s in cores.c\n", label);
		exit(1);
	}
	return c;
}

int cores_main(int argc, char **argv)
{
	size_t g, i;
	int s;

	if (argc != 2) {
		fprintf(stderr, "usage: %s cores\n", argv[0]);
		return 2;
	}
	printf("# group label rproc firmware node name\n");
	for (g = 0; g < ai64_ngroups; g++) {
		for (s = 1; s <= (int)ai64_ncores; s++) {
			for (i = 0; i < ai64_ncores; i++) {
				const struct ai64_core *c = &ai64_cores[i];

				if (strcmp(c->group, ai64_groups[g].group) || c->start != s) {
					continue;
				}
				printf("%s %s %s %s ", c->group, c->label, c->rproc, c->fw);
				if (c->node >= 0) {
					printf("%d", c->node);
				} else {
					printf("-");
				}
				printf(" %s\n", c->name);
			}
		}
	}
	return 0;
}
