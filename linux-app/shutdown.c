/* SPDX-License-Identifier: MIT */
/*
 * ai64 shutdown <core>
 *   Sends a core the RP_MBOX_SHUTDOWN message TI's kernel sends before it
 *   stops a core, on the mailbox FIFO Linux sends it messages on (ti,mbox-tx
 *   in the Linux device tree), and waits until the core has taken it. The
 *   BeagleBoard 6.18 kernel does not send it. That kernel cannot stop a C7x
 *   that is not idle, and stopping an R5F that is busy on the bus can hang
 *   the SoC.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"

#define RP_MBOX_SHUTDOWN 0xFFFFFF14u

int shutdown_main(int argc, char **argv)
{
	const struct ai64_core *c;
	volatile uint32_t *mbox;
	uint32_t fifo;
	int ms;

	if (argc != 3) {
		fprintf(stderr, "usage: %s shutdown <core>, such as c7x or 64800000.dsp\n", argv[0]);
		return 2;
	}
	c = core_find(argv[2]);
	if (!c || !c->mbox) {
		fprintf(stderr, "no mailbox known for %s\n", argv[2]);
		return 2;
	}
	mbox = map_phys(c->mbox, 4096);
	fifo = c->fifo;
	if (!mbox) {
		return 1;
	}
	mbox[(0x40 + 4 * fifo) / 4] = RP_MBOX_SHUTDOWN;
	/*
	 * The core polls its FIFO; an empty FIFO means it took the message.
	 * Give it a moment more to answer and park before Linux stops it.
	 */
	for (ms = 0; ms < 500; ms++) {
		if (!(mbox[(0xc0 + 4 * fifo) / 4] & 0x3f)) {
			usleep(10000);
			return 0;
		}
		usleep(1000);
	}
	fprintf(stderr, "%s did not take the shutdown message\n", argv[2]);
	return 1;
}
