/* SPDX-License-Identifier: MIT */
/*
 * ai64 peer <label> <rpmsg-dev> <fifo>
 *   The rtu0 window: it owns ICSSG0 RTU0's RPMsg channel, writes each line
 *   the linux window sends on fifo to it, and prints what RTU0 answers.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"

int peer_main(int argc, char **argv)
{
	const char *label, *devpath, *fifopath;
	int dev, fifo;
	char rx[512];
	char line[512];
	size_t fill = 0;

	if (argc != 5) {
		fprintf(stderr, "usage: %s peer <label> <dev> <fifo>\n", argv[0]);
		return 2;
	}
	label = argv[2];
	devpath = argv[3];
	fifopath = argv[4];

	dev = open(devpath, O_RDWR);
	if (dev < 0) {
		perror(devpath);
		return 1;
	}
	/*
	 * O_RDWR keeps a writer open on our own FIFO. With O_RDONLY, the FIFO
	 * reports end-of-file once the linux window exits, and poll() then
	 * returns at once forever.
	 */
	fifo = open(fifopath, O_RDWR);
	if (fifo < 0) {
		perror(fifopath);
		return 1;
	}

	reply_to(fifopath);
	printf("%s window: Linux and ICSSG0 RTU0 over RPMsg, on %s\n", label, devpath);
	fflush(stdout);

	for (;;) {
		struct pollfd pf[2];
		char chunk[256];
		ssize_t n;
		size_t i;

		pf[0].fd = dev;
		pf[0].events = POLLIN;
		pf[1].fd = fifo;
		pf[1].events = POLLIN;
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("poll");
			return 1;
		}

		if (pf[0].revents & POLLIN) {
			n = read(dev, rx, sizeof(rx));
			if (n > 0) {
				/* The firmware sentence already names who asked
				 * and what this core did. Do not prefix it with
				 * the core name again. The linux window gets it too.
				 */
				char text[520];

				snprintf(text, sizeof(text), "%.*s", (int)(rx[n - 1] == '\n' ? n - 1 : n), rx);
				note_line(text);
			}
		}

		if (pf[1].revents & (POLLIN | POLLHUP)) {
			n = read(fifo, chunk, sizeof(chunk));
			if (n <= 0) {
				continue;
			}
			for (i = 0; i < (size_t)n; i++) {
				if (chunk[i] == '\n') {
					line[fill] = '\0';
					if (fill && write_all(dev, line, fill)) {
						perror("rpmsg write");
						return 1;
					}
					if (fill) {
						printf("Linux -> %s: %s\n", label, line);
						fflush(stdout);
					}
					fill = 0;
				} else if (fill + 1 < sizeof(line)) {
					line[fill++] = chunk[i];
				}
			}
		}
	}
}
