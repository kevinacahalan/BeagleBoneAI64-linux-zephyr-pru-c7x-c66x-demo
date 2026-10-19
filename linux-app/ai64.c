/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ai64_erpc_linux.h"
#include "ai64_shm.h"

/*
 * ai64 peer <label> <rpmsg-dev> <fifo>
 *   Owns one RPMsg device. Used for RTU0.
 *
 * ai64 erpc [core] <rpmsg-dev> <fifo>
 *   Calls Zephyr functions over eRPC on the existing RPMsg fd. core names
 *   one of the other R5F cores (r5f0_1, ...); leave it out for R5F0_0.
 *
 * ai64 watch pru0|txpru0
 *   Prints what PRU0 or Tx_PRU0 does, from ICSSG0 shared RAM. Neither core
 *   has an RPMsg endpoint.
 *
 * ai64 shm
 *   Prints both shared-RAM records once and exits.
 *
 * ai64 pads
 *   Prints the pad registers of every pin the demo muxes, next to the value
 *   the overlay and the Zephyr devicetree ask for.
 *
 * ai64 bench <rpmsg-dev> [count]
 *   Times eRPC say round trips to one Zephyr core. Run it with the demo's
 *   eRPC windows stopped; two clients on one channel mix up replies.
 *
 * ai64 shutdown <remoteproc-name>
 *   Sends a core the RP_MBOX_SHUTDOWN message TI's kernel sends before it
 *   stops a core, and waits until the core has taken it. The 6.18 kernel
 *   does not send it, and cannot stop a C7x that is not idle.
 *
 * ai64 trace <remoteproc-name>
 *   Follows a core's remoteproc trace log, such as 4d80800000.dsp. It reads
 *   the buffer through /dev/mem, so it needs no root and no debugfs.
 *
 * ai64 linux <dir>
 *   Sends the startup script, then reads commands such as xy and arm.
 */

/* The zephyr window writes them to this path. */
static char g_reply_path[512];
/* How the erpc window names its core in what it prints. */
static const char *g_who = "Zephyr";

static void unquote(char *s)
{
	char *w = s;

	for (char *r = s; *r; r++) {
		if (*r != '"') {
			*w++ = *r;
		}
	}
	*w = '\0';
}

static void print_help(void)
{
	printf("commands:\n");
	printf("  zephyr say <text>\n");
	printf("  zephyr arm <period> <count>\n");
	printf("  zephyr chunk <text>\n");
	printf("  zephyr xy <x> <y>\n");
	printf("  zephyr spi [text]\n");
	printf("  r5f0_1|r5f1_0|r5f1_1 say <text>\n");
	printf("  rtu0 xy <x> <y>\n");
	printf("  rtu0 arm <period> <count>\n");
	printf("  rtu0 <text>\n");
	printf("  help\n");
	printf("  quit\n");
	fflush(stdout);
}

static void print_usage(const char *prog)
{
	printf("usage: %s <subcommand>\n", prog);
	printf("  linux <fifo-dir>          the linux window (ai64-demo starts it)\n");
	printf("  erpc [core] <rpmsg-dev> <fifo>  a zephyr window (core: r5f0_1 ...)\n");
	printf("  peer <label> <rpmsg-dev> <fifo>  the rtu0 window\n");
	printf("  watch pru0|txpru0         the pru0 and txpru0 windows\n");
	printf("  shm                       print the shared-RAM records once\n");
	printf("  pads                      check the pin mux of every demo pin\n");
	printf("  trace <remoteproc-name>   follow a core's trace log (the c66x and c7x windows)\n");
	printf("  bench <rpmsg-dev> [count] time eRPC round trips to one Zephyr core\n");
	printf("  shutdown <remoteproc-name> ask a core to idle before Linux stops it\n");
	printf("\n");
	printf("In the linux window, ");
	print_help();
}

static int write_all(int fd, const void *buf, size_t len)
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

/* Print on this process, and hand the same line to the linux window. */
static void note_line(const char *line)
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

static int peer_main(int argc, char **argv)
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

	printf("%s window ready on %s\n", label, devpath);
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
				 * the core name again.
				 */
				printf("%.*s", (int)n, rx);
				if (rx[n - 1] != '\n') {
					printf("\n");
				}
				fflush(stdout);
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
						printf("sent to %s: %s\n", label, line);
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

static void *map_phys(off_t addr, size_t len)
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

/* Both records sit in the first page of ICSSG0 shared RAM. */
static int map_records(volatile struct ai64_arm **arm, volatile struct ai64_xy **xy)
{
	volatile uint8_t *base = map_phys(AI64_ARM_CPU, 4096);

	if (!base) {
		return -1;
	}
	*arm = (volatile struct ai64_arm *)base;
	*xy = (volatile struct ai64_xy *)(base + (AI64_XY_CPU - AI64_ARM_CPU));
	return 0;
}

/* The pru0 window: one line per new X/Y record, one when PRU0 sent it. */
static void watch_pru0(volatile struct ai64_xy *xy)
{
	uint32_t last_seq = xy->seq;
	uint32_t last_consumed = xy->consumed;

	printf("pru0 window ready, X/Y record at 0x%x, SLIP TX on P8.11\n", AI64_XY_CPU);
	fflush(stdout);
	for (;;) {
		uint32_t seq = xy->seq;
		uint32_t consumed = xy->consumed;

		if (seq != last_seq) {
			printf("RTU0 published X/Y seq %u x %u y %u for PRU0\n",
			       seq, xy->x, xy->y);
			last_seq = seq;
		}
		if (consumed != last_consumed) {
			printf("PRU0 sent the SLIP frame for seq %u on P8.11\n", consumed);
			last_consumed = consumed;
		}
		fflush(stdout);
		usleep(20000);
	}
}

/* The txpru0 window: one line per new arm block, one when a train ends. */
static void watch_txpru0(volatile struct ai64_arm *arm)
{
	uint32_t last_seq = arm->seq;
	uint32_t last_applied = arm->applied_seq;
	uint32_t last_done = arm->done_seq;

	printf("txpru0 window ready, arm block at 0x%x, pulses on P9.25\n", AI64_ARM_CPU);
	fflush(stdout);
	for (;;) {
		uint32_t seq = arm->seq;
		uint32_t applied = arm->applied_seq;
		uint32_t done = arm->done_seq;

		if (seq != last_seq) {
			printf("RTU0 armed Tx_PRU0, seq %u period %u count %u\n",
			       seq, arm->period_cycles, arm->periods);
			last_seq = seq;
		}
		if (applied != last_applied) {
			printf("Tx_PRU0 started seq %u, period %u cycles\n",
			       applied, arm->applied_period);
			last_applied = applied;
		}
		if (done != last_done) {
			printf("Tx_PRU0 finished seq %u after %u pulses on P9.25\n",
			       done, arm->sent);
			last_done = done;
		}
		fflush(stdout);
		usleep(20000);
	}
}

static int watch_main(int argc, char **argv)
{
	volatile struct ai64_arm *arm;
	volatile struct ai64_xy *xy;

	if (argc != 3 || (strcmp(argv[2], "pru0") && strcmp(argv[2], "txpru0"))) {
		fprintf(stderr, "usage: %s watch pru0|txpru0\n", argv[0]);
		return 2;
	}
	if (map_records(&arm, &xy)) {
		return 1;
	}
	if (!strcmp(argv[2], "pru0")) {
		watch_pru0(xy);
	} else {
		watch_txpru0(arm);
	}
	return 0;
}

static int shm_main(void)
{
	volatile struct ai64_arm *arm;
	volatile struct ai64_xy *xy;

	if (map_records(&arm, &xy)) {
		return 1;
	}
	printf("arm 0x%x magic=%08x seq=%u period=%u count=%u sent=%u applied=%u done=%u\n",
	       AI64_ARM_CPU, arm->magic, arm->seq, arm->period_cycles, arm->periods,
	       arm->sent, arm->applied_seq, arm->done_seq);
	printf("xy  0x%x magic=%08x seq=%u x=%u y=%u consumed=%u\n",
	       AI64_XY_CPU, xy->magic, xy->seq, xy->x, xy->y, xy->consumed);
	return 0;
}

/* PINMUX.md has the same table. */
static const struct {
	uint32_t pad;
	uint32_t want;
	const char *pin;
} pads[] = {
	{ 0x0f4, 0x10000, "P8.11 AB24 PRU0 SLIP TX" },
	{ 0x1a4, 0x10007, "P9.25 W26 Tx_PRU0 GPIO0_104" },
	{ 0x200, 0x200007, "P9.25 AC4 held off" },
	{ 0x230, 0x10006, "P9.28 U2 Zephyr SPI7_CS0" },
	{ 0x0b0, 0x200007, "P9.28 AF28 held off" },
	{ 0x238, 0x10006, "P9.30 V6 Zephyr SPI7_D0" },
	{ 0x0b4, 0x200007, "P9.30 AE28 held off" },
	{ 0x234, 0x10006, "P9.31 U3 Zephyr SPI7_CLK" },
	{ 0x0d4, 0x200007, "P9.31 AB26 held off" },
	{ 0x01c, 0x1000e, "P8.34 AD22 Zephyr UART2_TXD" },
	{ 0x014, 0x5000e, "P8.22 AH23 Zephyr UART2_RXD" },
};

static int pads_main(void)
{
	volatile uint32_t *padcfg = map_phys(0x0011c000, 4096);
	size_t i;
	int bad = 0;

	if (!padcfg) {
		return 1;
	}
	for (i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
		uint32_t v = padcfg[pads[i].pad / 4];
		int ok = v == pads[i].want;

		printf("0x%03x %-30s 0x%08x %s\n", pads[i].pad, pads[i].pin, v,
		       ok ? "ok" : "DIFFERENT");
		bad |= !ok;
	}
	if (bad) {
		printf("Zephyr pads only change once R5F0_0 has booted; the others need the overlay.\n");
	}
	return bad;
}

/*
 * Where each core's device addresses live in the SoC address map. The
 * trace buffer address in a resource table is a device address: the
 * core's own view of memory. Local RAMs map to a SoC window, and anything
 * else (DDR) is the same address on both sides.
 */
static const struct {
	const char *name;
	uint32_t da;
	uint32_t len;
	uint64_t pa;
} core_windows[] = {
	{ "4d80800000.dsp", 0x00800000, 0x00048000, 0x4d80800000ull }, /* C66x 0 L2 */
	{ "4d81800000.dsp", 0x00800000, 0x00048000, 0x4d81800000ull }, /* C66x 1 L2 */
	{ "64800000.dsp", 0x00800000, 0x00080000, 0x64800000ull },     /* C71x L2 */
	{ "b034000.pru", 0x00000000, 0x00004000, 0x0b000000ull },      /* ICSSG0 data RAM */
	{ "b004000.rtu", 0x00000000, 0x00004000, 0x0b000000ull },
	{ "b00a000.txpru", 0x00000000, 0x00004000, 0x0b000000ull },
};

static int read_sysfs(const char *path, char *buf, size_t len)
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

/* The remoteproc firmware name of the core called name, or -1. */
static int rproc_firmware(const char *name, char *fw, size_t len)
{
	char path[128], got[64];
	int i;

	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/name", i);
		if (read_sysfs(path, got, sizeof(got)) || strcmp(got, name)) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/firmware", i);
		return read_sysfs(path, fw, len);
	}
	return -1;
}

/*
 * Find the first trace entry in the .resource_table section of an ELF
 * file. Handles 32-bit (C66x, R5F, PRU) and 64-bit (C7x) little-endian ELF.
 */
static int elf_trace(const char *file, uint32_t *da, uint32_t *len)
{
	FILE *f = fopen(file, "rb");
	unsigned char *img = NULL;
	long size;
	int ret = -1;
	int is64;
	uint64_t shoff;
	unsigned shentsize, shnum, shstrndx, i;

	if (!f) {
		perror(file);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	img = malloc((size_t)size);
	if (!img || fread(img, 1, (size_t)size, f) != (size_t)size ||
	    size < 64 || memcmp(img, "\177ELF", 4)) {
		goto out;
	}
	is64 = img[4] == 2;
#define U16(o) ((uint32_t)img[o] | (uint32_t)img[(o) + 1] << 8)
#define U32(o) (U16(o) | U16((o) + 2) << 16)
#define U64(o) ((uint64_t)U32(o) | (uint64_t)U32((o) + 4) << 32)
	shoff = is64 ? U64(0x28) : U32(0x20);
	shentsize = is64 ? U16(0x3a) : U16(0x2e);
	shnum = is64 ? U16(0x3c) : U16(0x30);
	shstrndx = is64 ? U16(0x3e) : U16(0x32);
	if (shoff + (uint64_t)shnum * shentsize > (uint64_t)size || shstrndx >= shnum) {
		goto out;
	}
	{
		uint64_t s = shoff + (uint64_t)shstrndx * shentsize;
		uint64_t stroff = is64 ? U64(s + 0x18) : U32(s + 0x10);

		for (i = 0; i < shnum; i++) {
			uint64_t h = shoff + (uint64_t)i * shentsize;
			uint64_t off = is64 ? U64(h + 0x18) : U32(h + 0x10);
			uint64_t sz = is64 ? U64(h + 0x20) : U32(h + 0x14);
			uint32_t num, k;

			if (stroff + U32(h) >= (uint64_t)size ||
			    strcmp((const char *)img + stroff + U32(h), ".resource_table") ||
			    off + sz > (uint64_t)size || sz < 16) {
				continue;
			}
			num = U32(off + 4);
			for (k = 0; k < num && 16 + 4 * (k + 1) <= sz; k++) {
				uint64_t e = off + U32(off + 16 + 4 * k);

				if (e + 16 <= off + sz && U32(e) == 2) { /* RSC_TRACE */
					*da = U32(e + 4);
					*len = U32(e + 8);
					ret = 0;
					goto out;
				}
			}
		}
	}
#undef U64
#undef U32
#undef U16
out:
	if (ret) {
		fprintf(stderr, "%s has no trace in its resource table\n", file);
	}
	free(img);
	fclose(f);
	return ret;
}

static int trace_main(int argc, char **argv)
{
	const char *name;
	char fw[128], path[256];
	uint32_t da, len, shown = 0;
	uint64_t pa;
	volatile char *buf;
	size_t i;
	long page = sysconf(_SC_PAGESIZE);
	char *mapped;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s trace <remoteproc-name>, such as 4d80800000.dsp\n", argv[0]);
		return 2;
	}
	name = argv[2];
	if (rproc_firmware(name, fw, sizeof(fw))) {
		fprintf(stderr, "no remoteproc called %s\n", name);
		return 1;
	}
	snprintf(path, sizeof(path), "/lib/firmware/%s", fw);
	if (elf_trace(path, &da, &len)) {
		return 1;
	}
	pa = da;
	for (i = 0; i < sizeof(core_windows) / sizeof(core_windows[0]); i++) {
		if (!strcmp(core_windows[i].name, name) && da >= core_windows[i].da &&
		    da - core_windows[i].da < core_windows[i].len) {
			pa = core_windows[i].pa + (da - core_windows[i].da);
		}
	}
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	mapped = mmap(NULL, len + (pa % page), PROT_READ, MAP_SHARED, fd, (off_t)(pa - pa % page));
	close(fd);
	if (mapped == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	buf = (volatile char *)(mapped + pa % page);
	printf("%s trace (%s), %u bytes at 0x%llx\n", name, fw, len, (unsigned long long)pa);
	fflush(stdout);
	for (;;) {
		uint32_t n = 0;

		while (n < len && buf[n]) {
			n++;
		}
		/* The firmware starts the log again from the top when it is full. */
		if (n < shown) {
			shown = 0;
		}
		for (; shown < n; shown++) {
			putchar(buf[shown]);
		}
		fflush(stdout);
		usleep(200000);
	}
}

/*
 * The mailbox FIFO Linux sends each core messages on (ti,mbox-tx in the
 * Linux device tree).
 */
static const struct {
	const char *name;
	uint32_t base;
	uint32_t fifo;
} core_mbox[] = {
	{ "5c00000.r5f", 0x31f81000, 1 },
	{ "5d00000.r5f", 0x31f81000, 3 },
	{ "5e00000.r5f", 0x31f82000, 1 },
	{ "5f00000.r5f", 0x31f82000, 3 },
	{ "4d80800000.dsp", 0x31f83000, 1 },
	{ "4d81800000.dsp", 0x31f83000, 3 },
	{ "64800000.dsp", 0x31f84000, 1 },
};

#define RP_MBOX_SHUTDOWN 0xFFFFFF14u

static int shutdown_main(int argc, char **argv)
{
	volatile uint32_t *mbox = NULL;
	uint32_t fifo = 0;
	size_t i;
	int ms;

	if (argc != 3) {
		fprintf(stderr, "usage: %s shutdown <remoteproc-name>, such as 64800000.dsp\n", argv[0]);
		return 2;
	}
	for (i = 0; i < sizeof(core_mbox) / sizeof(core_mbox[0]); i++) {
		if (!strcmp(core_mbox[i].name, argv[2])) {
			mbox = map_phys(core_mbox[i].base, 4096);
			fifo = core_mbox[i].fifo;
			if (!mbox) {
				return 1;
			}
		}
	}
	if (!mbox) {
		fprintf(stderr, "no mailbox known for %s\n", argv[2]);
		return 2;
	}
	mbox[(0x40 + 4 * fifo) / 4] = RP_MBOX_SHUTDOWN;
	/* The core polls its FIFO; an empty FIFO means it took the message. */
	for (ms = 0; ms < 500; ms++) {
		if (!(mbox[(0xc0 + 4 * fifo) / 4] & 0x3f)) {
			return 0;
		}
		usleep(1000);
	}
	fprintf(stderr, "%s did not take the shutdown message\n", argv[2]);
	return 1;
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int bench_main(int argc, char **argv)
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
		printf("Linux asked %s to check the chunk, and it saw a mismatch seq %u len %u crc %08x got %08x\n",
		       g_who, seq, n, crc, got);
		fflush(stdout);
		return -1;
	}
	printf("Linux asked %s to check the chunk, and it matched seq %u len %u crc %08x\n",
	       g_who, seq, n, crc);
	fflush(stdout);
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
			snprintf(shown, sizeof(shown),
				 "Linux asked %s to say \"%s\", and it failed",
				 g_who, text);
			note_line(shown);
			return -1;
		}
		snprintf(shown, sizeof(shown),
			 "Linux asked %s to say \"%s\", and it replied \"%s\"",
			 g_who, text, reply);
		note_line(shown);
		return 0;
	}
	if (!strncmp(line, "arm ", 4)) {
		unsigned period = 0, count = 0;

		if (sscanf(line + 4, "%u %u", &period, &count) != 2) {
			printf("zephyr arm wants <period> <count>\n");
			fflush(stdout);
			return -1;
		}
		if (ai64_erpc_arm(period, count)) {
			printf("Linux asked %s to arm Tx_PRU0, and it failed (only R5F0_0 has the mailbox)\n", g_who);
			fflush(stdout);
			return -1;
		}
		printf("Linux asked %s to arm Tx_PRU0, and it published period %u count %u for RTU0\n",
		       g_who, period, count);
		fflush(stdout);
		return 0;
	}
	if (!strncmp(line, "chunk ", 6)) {
		return do_chunk(line + 6);
	}
	if (!strncmp(line, "xy ", 3)) {
		unsigned x = 0, y = 0;

		if (sscanf(line + 3, "%u %u", &x, &y) != 2) {
			printf("zephyr xy wants <x> <y>\n");
			fflush(stdout);
			return -1;
		}
		if (ai64_erpc_xy(x, y)) {
			printf("Linux asked %s to publish X/Y, and it failed (only R5F0_0 has the mailbox)\n", g_who);
			fflush(stdout);
			return -1;
		}
		printf("Linux asked %s to publish X/Y %u %u, and it sent that pair to RTU0\n",
		       g_who, x, y);
		fflush(stdout);
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
			snprintf(shown, sizeof(shown),
				 "Linux asked %s to write SPI7 \"%s\", and it failed",
				 g_who, label);
			note_line(shown);
			return -1;
		}
		snprintf(shown, sizeof(shown),
			 "Linux asked %s to write SPI7 \"%s\", and it replied \"%s\"",
			 g_who, label, reply);
		note_line(shown);
		return 0;
	}
	printf("zephyr commands: say <text> | arm <period> <count> | chunk <text> | xy <x> <y> | spi [text]\n");
	fflush(stdout);
	return -1;
}

static int erpc_main(int argc, char **argv)
{
	const char *devpath, *fifopath;
	int dev, fifo;
	char chunk[256];
	char line[512];
	size_t fill = 0;

	if (argc == 5) {
		static char who[64];

		/* "r5f0_1" is printed as "Zephyr on R5F0_1". */
		snprintf(who, sizeof(who), "Zephyr on %s", argv[2]);
		for (char *c = who + 10; *c; c++) {
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
	/* O_RDWR for the same reason as in peer_main: no end-of-file spin. */
	fifo = open(fifopath, O_RDWR);
	if (fifo < 0) {
		perror(fifopath);
		return 1;
	}
	{
		size_t n = strlen(fifopath);

		if (n >= 3 && !strcmp(fifopath + n - 3, ".in") &&
		    snprintf(g_reply_path, sizeof(g_reply_path), "%.*s.reply",
			     (int)(n - 3), fifopath) >= (int)sizeof(g_reply_path)) {
			g_reply_path[0] = '\0';
		}
	}
	printf("%s window ready on %s (eRPC)\n", g_who, devpath);
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
 * The Zephyr cores the linux window talks to. cmd is what you type, and the
 * FIFO names. Only R5F0_0 has the mailbox, the bulk buffer, and SPI7.
 */
static struct zephyr_target {
	const char *cmd;
	const char *who;
	int in;
	int reply;
} zt[] = {
	{ "zephyr", "Zephyr", -1, -1 },
	{ "r5f0_1", "Zephyr on R5F0_1", -1, -1 },
	{ "r5f1_0", "Zephyr on R5F1_0", -1, -1 },
	{ "r5f1_1", "Zephyr on R5F1_1", -1, -1 },
};
#define NUM_ZT (sizeof(zt) / sizeof(zt[0]))

static struct zephyr_target *find_zt(const char *cmd)
{
	size_t i;

	for (i = 0; i < NUM_ZT; i++) {
		if (!strcmp(zt[i].cmd, cmd)) {
			return &zt[i];
		}
	}
	return NULL;
}

static void print_reply(int fd)
{
	char reply[700];
	struct pollfd pf;
	size_t fill = 0;

	fflush(stdout);
	if (fd < 0) {
		return;
	}
	pf.fd = fd;
	pf.events = POLLIN;
	while (fill + 1 < sizeof(reply)) {
		ssize_t got;
		char c;

		if (poll(&pf, 1, 3000) <= 0) {
			break;
		}
		got = read(fd, &c, 1);
		if (got <= 0) {
			break;
		}
		if (c == '\n') {
			reply[fill] = '\0';
			printf("%s\n", reply);
			fflush(stdout);
			return;
		}
		reply[fill++] = c;
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

static int send_rtu0(int fd, const char *msg)
{
	if (write_line(fd, "rtu0", msg)) {
		return -1;
	}
	if (!strncmp(msg, "xy ", 3)) {
		printf("Linux asked RTU0 to publish X/Y %s\n", msg + 3);
	} else if (!strncmp(msg, "arm ", 4)) {
		printf("Linux asked RTU0 to arm Tx_PRU0 %s\n", msg + 4);
	} else {
		printf("Linux sent \"%s\" to RTU0\n", msg);
	}
	fflush(stdout);
	return 0;
}

static int send_zephyr(struct zephyr_target *z, const char *msg)
{
	if (write_line(z->in, z->cmd, msg)) {
		return -1;
	}
	if (!strncmp(msg, "xy ", 3)) {
		printf("Linux asked %s to publish X/Y %s\n", z->who, msg + 3);
	} else if (!strncmp(msg, "arm ", 4)) {
		printf("Linux asked %s to arm Tx_PRU0 %s\n", z->who, msg + 4);
	} else if (!strncmp(msg, "chunk ", 6)) {
		printf("Linux asked %s to check chunk %s\n", z->who, msg + 6);
	} else if (!strncmp(msg, "say ", 4)) {
		printf("Linux asked %s to say %s\n", z->who, msg + 4);
		print_reply(z->reply);
		return 0;
	} else if (!strcmp(msg, "spi") || !strncmp(msg, "spi ", 4)) {
		if (!strcmp(msg, "spi") || msg[4] == '\0') {
			printf("Linux asked %s to write SPI7 hello\n", z->who);
		} else {
			printf("Linux asked %s to write SPI7 %s\n", z->who, msg + 4);
		}
		print_reply(z->reply);
		return 0;
	} else {
		printf("Linux sent %s %s\n", z->cmd, msg);
	}
	fflush(stdout);
	return 0;
}

/* The startup script. Each step waits long enough for the one before it. */
static const struct {
	const char *who;
	const char *msg;
} startup[] = {
	/* RTU0 learns Linux's endpoint from the first RPMsg it receives. */
	{ "rtu0", "ping" },
	{ "zephyr", "say hello from linux" },
	{ "zephyr", "arm 250000 20" },
	{ "zephyr", "chunk bulk-hello" },
	{ "zephyr", "xy 41 83" },
	{ "zephyr", "spi" },
	{ "rtu0", "arm 125000 10" },
	{ "r5f0_1", "say hello from linux" },
	{ "r5f1_0", "say hello from linux" },
	{ "r5f1_1", "say hello from linux" },
};

static void usage_hint(void)
{
	printf("use zephyr say|arm|chunk|xy|spi ..., r5f0_1|r5f1_0|r5f1_1 say <text>, rtu0 xy|arm|<text>, or help\n");
	fflush(stdout);
}

static int linux_main(int argc, char **argv)
{
	const char *dir;
	int rtu0;
	char input[512];
	size_t i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s linux <fifo-dir>\n", argv[0]);
		return 2;
	}
	dir = argv[2];

	for (i = 0; i < NUM_ZT; i++) {
		char path[512];

		/* Each erpc window writes Zephyr's reply lines back here. */
		snprintf(path, sizeof(path), "%s/%s.reply", dir, zt[i].cmd);
		unlink(path);
		if (mkfifo(path, 0666) && errno != EEXIST) {
			perror(path);
		}
		zt[i].reply = open(path, O_RDWR | O_NONBLOCK);
		zt[i].in = open_out(dir, zt[i].cmd);
		if (zt[i].in < 0) {
			return 1;
		}
	}
	rtu0 = open_out(dir, "rtu0");
	if (rtu0 < 0) {
		return 1;
	}

	for (i = 0; i < sizeof(startup) / sizeof(startup[0]); i++) {
		int ret;

		if (i) {
			usleep(300000);
		}
		if (!strcmp(startup[i].who, "rtu0")) {
			ret = send_rtu0(rtu0, startup[i].msg);
		} else {
			ret = send_zephyr(find_zt(startup[i].who), startup[i].msg);
		}
		if (ret) {
			return 1;
		}
	}

	printf("linux window ready. commands: zephyr say|arm|chunk|xy|spi ... | r5f0_1|r5f1_0|r5f1_1 say ... | rtu0 xy|arm|<text> | help\n");
	fflush(stdout);

	while (fgets(input, sizeof(input), stdin)) {
		struct zephyr_target *z;
		char *msg;
		size_t len = strlen(input);

		while (len && (input[len - 1] == '\n' || input[len - 1] == '\r')) {
			input[--len] = '\0';
		}
		if (!len || input[0] == '#') {
			continue;
		}
		if (!strcmp(input, "quit")) {
			break;
		}
		if (!strcmp(input, "help")) {
			print_help();
			continue;
		}
		msg = strchr(input, ' ');
		if (!msg) {
			usage_hint();
			continue;
		}
		*msg++ = '\0';
		z = find_zt(input);
		if (z) {
			if (!strncmp(msg, "say ", 4) || !strncmp(msg, "spi ", 4)) {
				unquote(msg + 4);
			}
			send_zephyr(z, msg);
		} else if (!strcmp(input, "rtu0")) {
			send_rtu0(rtu0, msg);
		} else {
			usage_hint();
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s peer|erpc|watch|shm|pads|trace|bench|shutdown|linux ... (-h for help)\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		print_usage(argv[0]);
		return 0;
	}
	if (!strcmp(argv[1], "peer")) {
		return peer_main(argc, argv);
	}
	if (!strcmp(argv[1], "erpc")) {
		return erpc_main(argc, argv);
	}
	if (!strcmp(argv[1], "watch")) {
		return watch_main(argc, argv);
	}
	if (!strcmp(argv[1], "shm")) {
		return shm_main();
	}
	if (!strcmp(argv[1], "pads")) {
		return pads_main();
	}
	if (!strcmp(argv[1], "trace")) {
		return trace_main(argc, argv);
	}
	if (!strcmp(argv[1], "bench")) {
		return bench_main(argc, argv);
	}
	if (!strcmp(argv[1], "shutdown")) {
		return shutdown_main(argc, argv);
	}
	if (!strcmp(argv[1], "linux")) {
		return linux_main(argc, argv);
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
