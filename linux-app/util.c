/* SPDX-License-Identifier: MIT */
/* What the subcommands share: /dev/mem, sysfs, the clock, and the windows' replies. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "ai64.h"

double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

void *map_phys(off_t addr, size_t len)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *p;

	if (fd < 0) {
		perror("/dev/mem");
		return NULL;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, addr);
	close(fd);
	if (p == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}
	return p;
}

int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int read_sysfs(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");

	if (!f) {
		return -1;
	}
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

int rproc_attr(const char *rproc, const char *attr, char *val, size_t len)
{
	char path[128], got[64];
	int i;

	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/name", i);
		if (read_sysfs(path, got, sizeof(got)) || strcmp(got, rproc)) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/%s", i, attr);
		return read_sysfs(path, val, len);
	}
	return -1;
}

int rproc_running(const char *rproc, char *state, size_t len)
{
	char buf[32];

	if (!state) {
		state = buf;
		len = sizeof(buf);
	}
	if (rproc_attr(rproc, "state", state, len)) {
		snprintf(state, len, "missing");
		return 0;
	}
	return !strcmp(state, "running");
}

int every_arg(int argc, char **argv, int *once)
{
	*once = argc > 2 && !strcmp(argv[2], "once");
	if (argc > 3) {
		return -1;
	}
	return argc > 2 && !*once ? atoi(argv[2]) : 10;
}

void unquote(char *s)
{
	char *w = s;

	for (char *r = s; *r; r++) {
		if (*r != '"') {
			*w++ = *r;
		}
	}
	*w = '\0';
}

static char g_reply_path[512];

void reply_to(const char *fifopath)
{
	size_t n = strlen(fifopath);

	if (n >= 3 && !strcmp(fifopath + n - 3, ".in") &&
	    snprintf(g_reply_path, sizeof(g_reply_path), "%.*s.reply",
		     (int)(n - 3), fifopath) >= (int)sizeof(g_reply_path)) {
		g_reply_path[0] = '\0';
	}
}

/* Print on this process, and hand the same line to the linux window. */
void note_line(const char *line)
{
	char buf[720];
	int n;
	int fd;

	printf("%s\n", line);
	fflush(stdout);
	if (!g_reply_path[0]) {
		return;
	}
	/*
	 * Open per line. The linux window recreates the FIFO when it starts,
	 * so a descriptor kept from earlier can point at a stale one. With no
	 * reader, O_NONBLOCK makes the open fail and the line is skipped.
	 */
	fd = open(g_reply_path, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		return;
	}
	n = snprintf(buf, sizeof(buf), "%s\n", line);
	if (n > 0 && (size_t)n < sizeof(buf)) {
		write_all(fd, buf, (size_t)n);
	}
	close(fd);
}
