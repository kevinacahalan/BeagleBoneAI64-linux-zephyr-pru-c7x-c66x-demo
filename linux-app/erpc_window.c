/* SPDX-License-Identifier: MIT */
/*
 * ai64 erpc [core] <rpmsg-dev> <fifo>
 *   An R5F core's window: it calls Zephyr functions over eRPC on the
 *   core's RPMsg channel, for each line the linux window writes to fifo.
 *   core is the R5F core it talks to, r5f0_0 (the default), r5f0_1,
 *   r5f1_0, or r5f1_1. R5F0_0's window also writes the bulk buffer for
 *   chunk, and for dma a layer's input, which R5F0_0's UDMA copies into
 *   the C7x.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"
#include "ai64_erpc_linux.h"
#include "ai64_dsp_job.h"
#include "ai64_shm.h"

/* How the window names its core in what it prints. */
static const char *g_who = "R5F0_0";

static uint32_t chunk_seq = 1;

static int do_chunk(const char *text)
{
	/* Mapped once and kept. Each chunk reuses the same 64 KiB window. */
	static volatile struct ai64_bulk *bulk;
	uint32_t n;
	uint32_t crc;
	uint32_t got = 0;
	uint32_t seq;
	uint32_t i;
	char line[200];

	if (!bulk) {
		bulk = map_phys(AI64_BULK_ADDR, AI64_BULK_SIZE);
	}
	if (!bulk) {
		return -1;
	}
	n = (uint32_t)strlen(text);
	if (n > sizeof(bulk->data)) {
		n = sizeof(bulk->data);
	}
	crc = ai64_crc32((const uint8_t *)text, n);
	/*
	 * This DDR mapping is device memory. A memcpy wider than one store
	 * raises SIGBUS. Byte stores through the volatile mapping do not.
	 */
	for (i = 0; i < n; i++) {
		bulk->data[i] = (uint8_t)text[i];
	}
	bulk->length = n;
	bulk->crc = crc;
	bulk->magic = AI64_BULK_MAGIC;
	seq = chunk_seq++;
	bulk->seq = seq;
	if (ai64_erpc_take(seq, n, crc, &got)) {
		snprintf(line, sizeof(line),
			 "%s: chunk: the bulk buffer did not match (seq %u, %u bytes, crc %08x, it saw %08x)",
			 g_who, seq, n, crc, got);
		note_line(line);
		return -1;
	}
	snprintf(line, sizeof(line), "%s: chunk: the bulk buffer matched (seq %u, %u bytes, crc %08x)",
		 g_who, seq, n, crc);
	note_line(line);
	return 0;
}

/*
 * r5f0_0 dma [rows]: Linux writes a layer's input in R5F0_0's bulk buffer
 * (DDR), R5F0_0's UDMA copies it into the C7x's job block (L2 SRAM), the
 * C7x runs the layer on its MMA, and Linux checks the copy and the result.
 */
static int do_dma(const char *arg)
{
	static volatile struct ai64_bulk *bulk;
	static volatile uint32_t *job;
	static int8_t a[AI64_JOB_LAYER_MAX_ROWS * 64], b[64 * 64], want[AI64_JOB_LAYER_MAX_ROWS * 64];
	uint32_t rows = arg[0] ? (uint32_t)atoi(arg) : AI64_JOB_LAYER_MAX_ROWS;
	uint32_t i, len, shift, relu, cpu_ns = 0;
	volatile int8_t *ja, *jb, *jy;
	char state[32], line[300];
	double t0, layer_us;
	int32_t ns;

	if (rows == 0 || rows > AI64_JOB_LAYER_MAX_ROWS || rows % 16) {
		snprintf(line, sizeof(line), "%s: dma wants rows, a multiple of 16 up to %u", g_who,
			 AI64_JOB_LAYER_MAX_ROWS);
		note_line(line);
		return -1;
	}
	/* A powered-off DSP's L2 SRAM does not answer, and reading it hangs the SoC. */
	if (!rproc_running(core_get("c7x")->rproc, state, sizeof(state))) {
		snprintf(line, sizeof(line), "%s: dma: the C7x is %s", g_who, state);
		note_line(line);
		return -1;
	}
	if (!bulk) {
		bulk = map_phys(AI64_BULK_ADDR, AI64_BULK_SIZE);
	}
	if (!job) {
		job = map_phys(AI64_JOB_C7X_PHYS, AI64_JOB_SIZE);
	}
	if (!bulk || !job || job[AI64_JOB_W_MAGIC] != AI64_JOB_MAGIC) {
		snprintf(line, sizeof(line), "%s: dma: no bulk buffer or C7x job block", g_who);
		note_line(line);
		return -1;
	}
	ja = (volatile int8_t *)((volatile uint8_t *)job + AI64_JOB_LAYER_A_OFF);
	jb = (volatile int8_t *)((volatile uint8_t *)job + AI64_JOB_LAYER_B_OFF);
	jy = (volatile int8_t *)((volatile uint8_t *)job + AI64_JOB_LAYER_Y_OFF);
	len = rows * 64;

	/* The input goes to DDR only; the C7x's copy starts as filler. */
	for (i = 0; i < len; i++) {
		a[i] = (int8_t)(rand() % 256 - 128);
		bulk->data[i] = (uint8_t)a[i];
		ja[i] = 0x55;
	}
	for (i = 0; i < 64 * 64; i++) {
		b[i] = (int8_t)(rand() % 256 - 128);
		jb[i] = b[i];
	}
	shift = 8 + rand() % 6;
	relu = rand() & 1;

	ns = ai64_erpc_dma((uint32_t)(AI64_JOB_C7X_PHYS + AI64_JOB_LAYER_A_OFF),
			   AI64_BULK_ADDR + 16u, len, &cpu_ns);
	if (ns < 0) {
		snprintf(line, sizeof(line), "%s: dma: the call failed (%d)", g_who, ns);
		note_line(line);
		return -1;
	}
	for (i = 0; i < len; i++) {
		if (ja[i] != a[i]) {
			snprintf(line, sizeof(line), "%s: dma: byte %u in the C7x's L2 is %d, expected %d",
				 g_who, i, ja[i], a[i]);
			note_line(line);
			return -1;
		}
	}
	layer_ref(a, b, want, rows, shift, relu);
	job[AI64_JOB_W_ARG] = shift | relu << 8;
	t0 = now_us();
	if (dsp_run(job, AI64_JOB_LAYER_MMA, rows, 0)) {
		snprintf(line, sizeof(line), "%s: dma: the C7x refused the layer", g_who);
		note_line(line);
		return -1;
	}
	layer_us = now_us() - t0;
	for (i = 0; i < len; i++) {
		if (jy[i] != want[i]) {
			snprintf(line, sizeof(line), "%s: dma: layer output %u is %d, expected %d", g_who, i,
				 jy[i], want[i]);
			note_line(line);
			return -1;
		}
	}
	snprintf(line, sizeof(line),
		 "%s: dma: its UDMA copied %u bytes from DDR to the C7x's L2 in %.1f us (%.0f MB/s; "
		 "the R5F reading them itself: %.1f us), then the C7x's MMA ran the layer: "
		 "%u cycles, %.1f us round trip; all match the A72",
		 g_who, len, ns / 1000.0, len * 1000.0 / ns, cpu_ns / 1000.0,
		 job[AI64_JOB_W_CYCLES], layer_us);
	note_line(line);
	return 0;
}

static int erpc_line(const char *line)
{
	char reply[200];

	if (!strncmp(line, "say ", 4)) {
		char text[512];
		char shown[800];

		snprintf(text, sizeof(text), "%s", line + 4);
		unquote(text);
		if (ai64_erpc_say(text, reply, sizeof(reply))) {
			snprintf(shown, sizeof(shown), "%s: say \"%s\": the call failed", g_who, text);
			note_line(shown);
			return -1;
		}
		snprintf(shown, sizeof(shown), "%s: say \"%s\": it replied \"%s\"", g_who, text, reply);
		note_line(shown);
		return 0;
	}
	if (!strncmp(line, "pwm ", 4)) {
		unsigned period = 0, duty = 0;
		char shown[200];

		if (sscanf(line + 4, "%u %u", &period, &duty) != 2) {
			snprintf(shown, sizeof(shown), "%s: pwm wants <period_ns> <duty_ns>", g_who);
		} else if (ai64_erpc_pwm(period, duty)) {
			snprintf(shown, sizeof(shown), "%s: pwm: the call failed", g_who);
		} else {
			snprintf(shown, sizeof(shown),
				 "%s: pwm: P9.25 (EHRPWM4 output B) now fixed at %u ns a period, %u ns high",
				 g_who, period, duty);
			note_line(shown);
			return 0;
		}
		note_line(shown);
		return -1;
	}
	if (!strncmp(line, "steps ", 6)) {
		unsigned ms = 0;
		char shown[200];

		if (sscanf(line + 6, "%u", &ms) != 1) {
			snprintf(shown, sizeof(shown), "%s: steps wants <ms>, 0 to stop", g_who);
		} else if (ai64_erpc_pwm_steps(ms)) {
			snprintf(shown, sizeof(shown), "%s: steps: the call failed", g_who);
		} else {
			if (ms) {
				snprintf(shown, sizeof(shown),
					 "%s: steps: P9.25 (EHRPWM4 output B) steps 10, 15, 20 kHz x 10-90%% duty, "
					 "%u ms a step", g_who, ms);
			} else {
				snprintf(shown, sizeof(shown), "%s: steps: P9.25 stays on the step it was on",
					 g_who);
			}
			note_line(shown);
			return 0;
		}
		note_line(shown);
		return -1;
	}
	if (!strncmp(line, "chunk ", 6)) {
		return do_chunk(line + 6);
	}
	if (!strcmp(line, "dma") || !strncmp(line, "dma ", 4)) {
		return do_dma(line[3] ? line + 4 : "");
	}
	if (!strcmp(line, "status")) {
		char text[800];
		char shown[900];

		if (ai64_erpc_status(text, sizeof(text))) {
			snprintf(shown, sizeof(shown), "%s: status: the call failed", g_who);
			note_line(shown);
			return -1;
		}
		snprintf(shown, sizeof(shown), "%s: status: %s", g_who, text);
		note_line(shown);
		return 0;
	}
	if (!strcmp(line, "spi") || !strncmp(line, "spi ", 4)) {
		char text[512];
		char shown[800];
		const char *label;

		text[0] = '\0';
		if (!strncmp(line, "spi ", 4)) {
			snprintf(text, sizeof(text), "%s", line + 4);
			unquote(text);
		}
		label = text[0] ? text : "hello";
		if (ai64_erpc_spi(text, reply, sizeof(reply))) {
			snprintf(shown, sizeof(shown), "%s: spi \"%s\": the call failed", g_who, label);
			note_line(shown);
			return -1;
		}
		snprintf(shown, sizeof(shown), "%s: spi \"%s\": it replied \"%s\"", g_who, label, reply);
		note_line(shown);
		return 0;
	}
	{
		char shown[200];

		snprintf(shown, sizeof(shown),
			 "%s: commands are say <text> | status | pwm <period_ns> <duty_ns> | steps <ms> | chunk <text> | spi [text] | dma [rows]",
			 g_who);
		note_line(shown);
	}
	return -1;
}

int erpc_main(int argc, char **argv)
{
	const char *devpath, *fifopath;
	int dev, fifo;
	char chunk[256];
	char line[512];
	size_t fill = 0;

	if (argc == 5) {
		static char who[16];

		/* "r5f0_1" is printed as "R5F0_1". */
		snprintf(who, sizeof(who), "%s", argv[2]);
		for (char *c = who; *c; c++) {
			if (*c >= 'a' && *c <= 'z') {
				*c = (char)(*c - 'a' + 'A');
			}
		}
		g_who = who;
		argv++;
		argc--;
	}
	if (argc != 4) {
		fprintf(stderr, "usage: %s erpc [core] <dev> <fifo>\n", argv[0]);
		return 2;
	}
	devpath = argv[2];
	fifopath = argv[3];
	dev = open(devpath, O_RDWR);
	if (dev < 0) {
		perror(devpath);
		return 1;
	}
	if (ai64_erpc_open(dev)) {
		fprintf(stderr, "eRPC client init failed\n");
		return 1;
	}
	/* O_RDWR for the same reason as in peer_main (rtu0_window.c): no end-of-file spin. */
	fifo = open(fifopath, O_RDWR);
	if (fifo < 0) {
		perror(fifopath);
		return 1;
	}
	reply_to(fifopath);
	printf("%s window: Linux calls Zephyr on %s over eRPC, on %s\n", g_who, g_who, devpath);
	fflush(stdout);

	for (;;) {
		ssize_t n;
		size_t i;

		n = read(fifo, chunk, sizeof(chunk));
		if (n <= 0) {
			continue;
		}
		for (i = 0; i < (size_t)n; i++) {
			if (chunk[i] == '\n') {
				line[fill] = '\0';
				if (fill) {
					erpc_line(line);
				}
				fill = 0;
			} else if (fill + 1 < sizeof(line)) {
				line[fill++] = chunk[i];
			}
		}
	}
}
