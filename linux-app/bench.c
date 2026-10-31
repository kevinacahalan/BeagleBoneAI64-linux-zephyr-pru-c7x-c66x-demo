/* SPDX-License-Identifier: MIT */
/*
 * ai64 bench <rpmsg-dev> [count]
 *   Times eRPC say round trips to one Zephyr core. Run it with the demo's
 *   eRPC windows stopped; two clients on one channel mix up replies.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ai64.h"
#include "ai64_erpc_linux.h"

int bench_main(int argc, char **argv)
{
	int count = argc > 3 ? atoi(argv[3]) : 1000;
	double min = 1e12, max = 0, total = 0;
	char reply[200];
	int dev, i;

	if (argc < 3 || count <= 0) {
		fprintf(stderr, "usage: %s bench <rpmsg-dev> [count]\n", argv[0]);
		return 2;
	}
	dev = open(argv[2], O_RDWR);
	if (dev < 0 || ai64_erpc_open(dev)) {
		perror(argv[2]);
		return 1;
	}
	for (i = 0; i < count; i++) {
		double t0 = now_us(), dt;

		if (ai64_erpc_say("x", reply, sizeof(reply))) {
			fprintf(stderr, "call %d failed\n", i);
			return 1;
		}
		dt = now_us() - t0;
		total += dt;
		min = dt < min ? dt : min;
		max = dt > max ? dt : max;
	}
	printf("%d eRPC say round trips on %s: mean %.0f us, min %.0f us, max %.0f us\n",
	       count, argv[2], total / count, min, max);
	return 0;
}
