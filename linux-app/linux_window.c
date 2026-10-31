/* SPDX-License-Identifier: MIT */
/*
 * ai64 linux <fifo-dir>
 *   The linux window: it runs the startup script, then reads commands such
 *   as r5f0_0 pwm and rtu0 steps, and passes each to the window that owns
 *   that core's channel, through the FIFOs in fifo-dir; their answers come
 *   back on each window's reply FIFO.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai64.h"

void print_help(void)
{
	printf("commands (each R5F core runs Zephyr with a job of its own: R5F0_0 SPI7, R5F0_1 P9.25,\n"
	       "R5F1_0 the P8.18 square wave, R5F1_1 the monitor; any core passes spi, pwm, and steps\n"
	       "on to the core whose job they are):\n");
	printf("  r5fX_Y say <text>                   the core echoes it back (eRPC)\n");
	printf("  r5fX_Y status                       its job's status; r5f1_1 shows all four\n");
	printf("  r5fX_Y spi [text]                   R5F0_0 writes SPI7\n");
	printf("  r5fX_Y pwm <period_ns> <duty_ns>    R5F0_1 sets P9.25 (EHRPWM4), fixed\n");
	printf("  r5fX_Y steps <ms>                   R5F0_1 steps P9.25 10/15/20 kHz x 10-90%%, 0 stops\n");
	printf("  r5f0_0 chunk <text>                 Linux writes the DDR bulk buffer, R5F0_0 checks it\n");
	printf("  r5f0_0 dma [rows]                   R5F0_0's UDMA copies a layer's input from DDR to the\n"
	       "                                      C7x's L2; the C7x's MMA runs the layer\n");
	printf("  rtu0 pwm <period_ns> <duty_ns>      P8.11/P8.12 software PWM (ICSSG0 slice 0), and its\n"
	       "                                      eCAP copy of A on P8.15, fixed\n");
	printf("  rtu0 steps <ms> | sweep <ms> | dead <ns> | status\n");
	printf("  rtu0 <text>                         RTU0 echoes it back (RPMsg)\n");
	printf("  pipe [start [seconds] | stop]       the pipeline P8.31 -> ICSSG0 -> UDMA -> C66x FFT ->\n"
	       "                                      C7x MMA -> P9.25: its numbers, or train and start\n"
	       "                                      it, or stop it (needs P9.15 jumpered to P8.31)\n");
	printf("  (P9.12/P9.15, ICSSG0 slice 1's hardware PWM, has no RPMsg: ai64 fabric route g0rtu1 pwm ...)\n");
	printf("  help\n");
	printf("  quit\n");
	fflush(stdout);
}

static int open_out(const char *dir, const char *name)
{
	char path[512];
	int fd;

	snprintf(path, sizeof(path), "%s/%s.in", dir, name);
	printf("opening %s\n", path);
	fflush(stdout);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
	}
	return fd;
}

/*
 * The windows the linux window talks to: what you type, the FIFO names,
 * and how lines name them. Each window writes its result lines back on a
 * reply FIFO, which the linux window prints as they come.
 */
static struct target {
	const char *cmd;
	const char *who;
	int in;
	int reply;
	char buf[720];
	size_t fill;
} tg[] = {
	{ "r5f0_0", "R5F0_0 (Zephyr)", -1, -1, "", 0 },
	{ "r5f0_1", "R5F0_1 (Zephyr)", -1, -1, "", 0 },
	{ "r5f1_0", "R5F1_0 (Zephyr)", -1, -1, "", 0 },
	{ "r5f1_1", "R5F1_1 (Zephyr)", -1, -1, "", 0 },
	{ "rtu0", "ICSSG0 RTU0", -1, -1, "", 0 },
};
#define NUM_TG (sizeof(tg) / sizeof(tg[0]))
#define RTU0_TG (&tg[NUM_TG - 1])

static struct target *find_tg(const char *cmd)
{
	size_t i;

	for (i = 0; i < NUM_TG; i++) {
		if (!strcmp(tg[i].cmd, cmd)) {
			return &tg[i];
		}
	}
	return NULL;
}

/* Print the reply lines that have come in, a whole line at a time. */
static void drain_reply(struct target *t)
{
	char c;

	while (t->reply >= 0 && read(t->reply, &c, 1) == 1) {
		if (c == '\n') {
			t->buf[t->fill] = '\0';
			printf("  %s\n", t->buf);
			t->fill = 0;
		} else if (t->fill + 1 < sizeof(t->buf)) {
			t->buf[t->fill++] = c;
		}
	}
	fflush(stdout);
}

/* Wait up to ms for stdin, printing reply lines meanwhile. 1 when stdin has input. */
static int pump(int ms, int want_stdin)
{
	struct pollfd pf[NUM_TG + 1];
	double end = now_us() + ms * 1000.0;
	size_t i;

	for (;;) {
		int left = (int)((end - now_us()) / 1000.0);

		for (i = 0; i < NUM_TG; i++) {
			pf[i].fd = tg[i].reply;
			pf[i].events = POLLIN;
		}
		pf[NUM_TG].fd = want_stdin ? 0 : -1;
		pf[NUM_TG].events = POLLIN;
		if (ms >= 0 && left <= 0) {
			return 0;
		}
		if (poll(pf, NUM_TG + 1, ms < 0 ? -1 : left) < 0 && errno != EINTR) {
			return 0;
		}
		for (i = 0; i < NUM_TG; i++) {
			if (pf[i].revents & POLLIN) {
				drain_reply(&tg[i]);
			}
		}
		if (pf[NUM_TG].revents & (POLLIN | POLLHUP)) {
			return 1;
		}
	}
}

static int write_line(int fd, const char *what, const char *msg)
{
	char buf[480];
	int n = snprintf(buf, sizeof(buf), "%s\n", msg);

	if (n < 0 || (size_t)n >= sizeof(buf)) {
		fprintf(stderr, "message too long\n");
		return -1;
	}
	if (write_all(fd, buf, (size_t)n)) {
		perror(what);
		return -1;
	}
	return 0;
}

static int send_to(struct target *t, const char *msg)
{
	printf("Linux -> %s: %s\n", t->who, msg);
	fflush(stdout);
	return write_line(t->in, t->cmd, msg);
}

/* The startup script. Each step waits long enough for the one before it. */
static const struct {
	const char *who;
	const char *msg;
} startup[] = {
	/* RTU0 learns Linux's endpoint from the first RPMsg it receives. */
	{ "rtu0", "ping" },
	{ "r5f0_0", "say hello from linux" },
	{ "r5f0_0", "chunk bulk-hello" },
	{ "r5f0_0", "spi" },
	/* The PWM default again: P9.25 stepping, 5 ms a step. */
	{ "r5f0_1", "steps 5" },
	{ "rtu0", "status" },
	{ "r5f0_1", "say hello from linux" },
	{ "r5f1_0", "say hello from linux" },
	{ "r5f1_1", "say hello from linux" },
	/* R5F1_0 has no SPI: it passes this to R5F0_0 over ipc_service. */
	{ "r5f1_0", "spi via r5f1_0" },
	/* By now the monitor has a second of round trips. */
	{ "r5f1_1", "status" },
	/* DDR -> R5F0_0's UDMA -> the C7x's L2 -> its MMA. */
	{ "r5f0_0", "dma" },
};

static void usage_hint(void)
{
	printf("use r5fX_Y say|status|spi|pwm|steps ..., r5f0_0 chunk <text>|dma [rows], "
	       "rtu0 pwm|steps|sweep|dead|status|<text>, pipe [start|stop], or help\n");
	fflush(stdout);
}

int linux_main(int argc, char **argv)
{
	const char *dir;
	char input[512];
	size_t fill = 0;
	size_t i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s linux <fifo-dir>\n", argv[0]);
		return 2;
	}
	dir = argv[2];

	for (i = 0; i < NUM_TG; i++) {
		char path[512];

		/* Each window writes its result lines back here. */
		snprintf(path, sizeof(path), "%s/%s.reply", dir, tg[i].cmd);
		unlink(path);
		if (mkfifo(path, 0666) && errno != EEXIST) {
			perror(path);
		}
		tg[i].reply = open(path, O_RDWR | O_NONBLOCK);
		tg[i].in = open_out(dir, tg[i].cmd);
		if (tg[i].in < 0) {
			return 1;
		}
	}

	printf("Each line \"Linux -> X\" is a command Linux sent; the indented lines under it are\n"
	       "the answers, which also show in that core's own window.\n");
	for (i = 0; i < sizeof(startup) / sizeof(startup[0]); i++) {
		if (send_to(find_tg(startup[i].who), startup[i].msg)) {
			return 1;
		}
		pump(400, 0);
	}

	/*
	 * Last, the pipeline: P8.31, jumpered to P9.15, to P9.25, through
	 * ICSSG0, R5F0_0's UDMA, both C66x cores, and the C7x (ai64_pipe.c). Without
	 * the jumper it finds no windows to train on, and P9.25 keeps stepping.
	 */
	{
		char *av[] = { argv[0], "pipe", "start", "3", NULL };

		printf("Linux: the pipeline, P8.31 to P9.25 (ai64 pipe start)\n");
		fflush(stdout);
		pump(0, 0);
		if (pipe_main(4, av)) {
			printf("  the pipeline did not start; P9.25 keeps its own steps\n");
		}
	}

	printf("linux window ready. commands: r5fX_Y say|status|spi|pwm|steps ... | r5f0_0 chunk|dma ... | "
	       "rtu0 pwm|steps|sweep|dead|status|<text> | pipe [start|stop] | help\n");
	fflush(stdout);

	for (;;) {
		struct target *t;
		char *msg, c;
		ssize_t got;

		if (!pump(-1, 1)) {
			continue;
		}
		got = read(0, &c, 1);
		if (got <= 0) {
			break;
		}
		if (c != '\n') {
			if (c != '\r' && fill + 1 < sizeof(input)) {
				input[fill++] = c;
			}
			continue;
		}
		input[fill] = '\0';
		fill = 0;
		if (!input[0] || input[0] == '#') {
			continue;
		}
		if (!strcmp(input, "quit")) {
			break;
		}
		if (!strcmp(input, "help")) {
			print_help();
			continue;
		}
		if (!strncmp(input, "pipe", 4) && (input[4] == ' ' || !input[4])) {
			char *av[5] = { argv[0], "pipe", "once", NULL, NULL };
			int ac = 3;

			if (input[4]) {
				av[2] = strtok(input + 5, " ");
				av[3] = av[2] ? strtok(NULL, " ") : NULL;
				ac = av[3] ? 4 : 3;
				av[2] = av[2] ? av[2] : "once";
			}
			pipe_main(ac, av);
			fflush(stdout);
			continue;
		}
		msg = strchr(input, ' ');
		if (!msg) {
			usage_hint();
			continue;
		}
		*msg++ = '\0';
		t = find_tg(input);
		if (!t) {
			usage_hint();
			continue;
		}
		if (t != RTU0_TG && (!strncmp(msg, "say ", 4) || !strncmp(msg, "spi ", 4))) {
			unquote(msg + 4);
		}
		send_to(t, msg);
	}
	return 0;
}
