/* SPDX-License-Identifier: MIT */
/*
 * ai64: the Linux side of the demo, one program with a subcommand for each
 * window ai64-demo opens and for each check you can run by hand. Each
 * subcommand is a file of its own (README.md in this directory).
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>

#include "ai64.h"

static const struct {
	const char *name;
	int (*run)(int argc, char **argv);
	const char *usage;
} subcommands[] = {
	{ "linux", linux_main, "linux <fifo-dir>          the linux window (ai64-demo starts it)" },
	{ "erpc", erpc_main, "erpc [core] <rpmsg-dev> <fifo>  an R5F core's window (core: r5f0_0 ...)" },
	{ "peer", peer_main, "peer <label> <rpmsg-dev> <fifo>  the rtu0 window" },
	{ "pwm", pwm_main, "pwm [seconds|once]        the four PWM generators (the pwm window)" },
	{ "sched", sched_main, "sched [seconds|once]      ICSSG1's multicore scheduler (the sched window)" },
	{ "pads", pads_main, "pads                      check the pin mux of every demo pin" },
	{ "cores", cores_main, "cores                     every demo core: its remoteproc, firmware, node" },
	{ "trace", trace_main, "trace <core>              follow a core's trace log (the c66x and c7x windows)" },
	{ "bench", bench_main, "bench <rpmsg-dev> [count] time eRPC round trips to one Zephyr core" },
	{ "shutdown", shutdown_main, "shutdown <core>           ask a core to idle before Linux stops it" },
	{ "dsp", dsp_main, "dsp <core> [count] [taps] [runs]  offload FIR jobs to a DSP" },
	{ "mma", mma_main, "mma [rows] [runs]         an int8 layer on the C7x's matrix accelerator, and in C" },
	{ "fabric", fabric_main,
	  "fabric [lines|once]       every core talking to every other (the fabric window)\n"
	  "  fabric route <node>... [pwm <period_ns> <duty_ns> | steps <ms> | sweep <ms> | dead <ns>]\n"
	  "                            send one message along those cores and back\n"
	  "  fabric reset              free the tally's spinlock and zero the tally" },
	{ "pipe", pipe_main,
	  "pipe start [seconds]      start the pipeline P8.31 -> ICSSG0 -> UDMA -> C66x FFT ->\n"
	  "                            C7x MMA -> P9.25, training its layer on that many seconds\n"
	  "  pipe [seconds|once] | stop  the pipeline's numbers (the pipe window), or stop it" },
	{ "web", web_main, "web [port]                a live page of every core, on port 8064 (the web window)" },
};

#define NSUB (sizeof(subcommands) / sizeof(subcommands[0]))

static void print_usage(const char *prog)
{
	size_t i;

	printf("usage: %s <subcommand>\n", prog);
	for (i = 0; i < NSUB; i++) {
		printf("  %s\n", subcommands[i].usage);
	}
	printf("A <core> is a label from ai64 cores (c7x, g0rtu1, ...) or a remoteproc name.\n\n");
	printf("In the linux window, ");
	print_help();
}

int main(int argc, char **argv)
{
	size_t i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <subcommand> ... (-h for the list)\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		print_usage(argv[0]);
		return 0;
	}
	for (i = 0; i < NSUB; i++) {
		if (!strcmp(argv[1], subcommands[i].name)) {
			return subcommands[i].run(argc, argv);
		}
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
