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
#include "ai64_dsp_job.h"
#include "ai64_shm.h"
#include "ai64_fabric.h"

/*
 * ai64 peer <label> <rpmsg-dev> <fifo>
 *   Owns one RPMsg device. Used for ICSSG0 RTU0.
 *
 * ai64 erpc [core] <rpmsg-dev> <fifo>
 *   Calls Zephyr functions over eRPC on the existing RPMsg fd. core is the
 *   R5F core it talks to, r5f0_0 (the default), r5f0_1, r5f1_0, or r5f1_1.
 *
 * ai64 pwm [seconds|once]
 *   Prints what the four PWM generators run: ICSSG0 slice 0 (software),
 *   its copy of A on ICSSG0's eCAP (P8.15), ICSSG0 slice 1 (the ICSSG PWM
 *   module), and EHRPWM4 on P9.25 (Zephyr),
 *   from the status each one's talking core publishes in its fabric page.
 *
 * ai64 sched [seconds|once]
 *   Shows ICSSG1's multicore scheduler: IEP0's period, each core's task and
 *   compare, and what P8.17 spends its time at. ICSSG1 has no talking core,
 *   so this reads its registers, and only reads them.
 *
 * ai64 pads
 *   Prints the pad registers of every pin the demo muxes, next to the value
 *   the overlay and the Zephyr devicetree ask for.
 *
 * ai64 bench <rpmsg-dev> [count]
 *   Times eRPC say round trips to one Zephyr core. Run it with the demo's
 *   eRPC windows stopped; two clients on one channel mix up replies.
 *
 * ai64 dsp <remoteproc-name> [count] [taps] [runs]
 *   Hands a DSP FIR filter jobs through its job block (ai64_dsp_job.h), and
 *   checks each result against the same filter run here. Prints the DSP's
 *   cycles, the time the A72 waited, and the time the A72 takes alone.
 *
 * ai64 shutdown <remoteproc-name>
 *   Sends a core the RP_MBOX_SHUTDOWN message TI's kernel sends before it
 *   stops a core, and waits until the core has taken it. The BeagleBoard
 *   6.18 kernel does not send it. That kernel cannot stop a C7x that is not
 *   idle, and stopping an R5F that is busy on the bus can hang the SoC.
 *
 * ai64 trace <remoteproc-name>
 *   Follows a core's remoteproc trace log, such as 4d80800000.dsp. It reads
 *   the buffer through /dev/mem, so it needs no root and no debugfs.
 *
 * ai64 fabric [lines|once] | route <node>... [pwm|steps|sweep|dead ...] | reset
 *   Joins the fabric, where every core talks to every other one, and shows
 *   it (ai64_fabric.c).
 *
 * ai64 linux <dir>
 *   Sends the startup script, then reads commands such as r5f0_0 pwm and
 *   rtu0 steps.
 */

int fabric_main(int argc, char **argv);

/* An erpc or peer window writes its result lines to this path too. */
static char g_reply_path[512];
/* How the erpc window names its core in what it prints. */
static const char *g_who = "R5F0_0";

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
	printf("commands (each R5F core runs its own Zephyr; R5F0_0 also has P9.25, SPI7, and the bulk buffer):\n");
	printf("  r5f0_0 say <text>                   R5F0_0 echoes it back (eRPC)\n");
	printf("  r5f0_0 pwm <period_ns> <duty_ns>    P9.25 hardware PWM (EHRPWM4), fixed\n");
	printf("  r5f0_0 steps <ms>                   P9.25 steps 10/15/20 kHz x 10-90%%, 0 stops\n");
	printf("  r5f0_0 chunk <text>                 Linux writes the DDR bulk buffer, R5F0_0 checks it\n");
	printf("  r5f0_0 spi [text]                   R5F0_0 writes SPI7\n");
	printf("  r5f0_1|r5f1_0|r5f1_1 say <text>\n");
	printf("  rtu0 pwm <period_ns> <duty_ns>      P8.11/P8.12 software PWM (ICSSG0 slice 0), and its\n"
	       "                                      eCAP copy of A on P8.15, fixed\n");
	printf("  rtu0 steps <ms> | sweep <ms> | dead <ns> | status\n");
	printf("  rtu0 <text>                         RTU0 echoes it back (RPMsg)\n");
	printf("  (P9.12/P9.15, ICSSG0 slice 1's hardware PWM, has no RPMsg: ai64 fabric route g0rtu1 pwm ...)\n");
	printf("  help\n");
	printf("  quit\n");
	fflush(stdout);
}

static void print_usage(const char *prog)
{
	printf("usage: %s <subcommand>\n", prog);
	printf("  linux <fifo-dir>          the linux window (ai64-demo starts it)\n");
	printf("  erpc [core] <rpmsg-dev> <fifo>  an R5F core's window (core: r5f0_0 ...)\n");
	printf("  peer <label> <rpmsg-dev> <fifo>  the rtu0 window\n");
	printf("  pwm [seconds|once]        the four PWM generators (the pwm window)\n");
	printf("  sched [seconds|once]      ICSSG1's multicore scheduler (the sched window)\n");
	printf("  pads                      check the pin mux of every demo pin\n");
	printf("  trace <remoteproc-name>   follow a core's trace log (the c66x and c7x windows)\n");
	printf("  bench <rpmsg-dev> [count] time eRPC round trips to one Zephyr core\n");
	printf("  shutdown <remoteproc-name> ask a core to idle before Linux stops it\n");
	printf("  dsp <remoteproc-name> [count] [taps] [runs] offload FIR jobs to a DSP\n");
	printf("  fabric [lines|once]       every core talking to every other (the fabric window)\n");
	printf("  fabric route <node>... [pwm <period_ns> <duty_ns> | steps <ms> | sweep <ms> | dead <ns>]\n");
	printf("                            send one message along those cores and back\n");
	printf("  fabric reset              free the tally's spinlock and zero the tally\n");
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

	{
		size_t n = strlen(fifopath);

		if (n >= 3 && !strcmp(fifopath + n - 3, ".in") &&
		    snprintf(g_reply_path, sizeof(g_reply_path), "%.*s.reply",
			     (int)(n - 3), fifopath) >= (int)sizeof(g_reply_path)) {
			g_reply_path[0] = '\0';
		}
	}
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

/* PINMUX.md has the same table. */
static const struct {
	uint32_t pad;
	uint32_t want;
	const char *pin;
} pads[] = {
	{ 0x0f4, 0x10000, "P8.11 AB24 ICSSG0 PRU0 GPO17" },
	{ 0x0f0, 0x10000, "P8.12 AH28 ICSSG0 PRU0 GPO16" },
	{ 0x0f8, 0x10004, "P8.15 AB29 PRG0_ECAP0_IN_APWM_OUT" },
	{ 0x0b8, 0x10003, "P9.12 AE27 PRG0_PWM2_A0" },
	{ 0x0c0, 0x10003, "P9.15 AD25 PRG0_PWM2_B0" },
	{ 0x00c, 0x10003, "P8.17 AF22 PRG1_PWM2_A0" },
	{ 0x1a4, 0x10006, "P9.25 W26 Zephyr EHRPWM4_B" },
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

		printf("0x%03x %-36s 0x%08x %s\n", pads[i].pad, pads[i].pin, v,
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
};

/*
 * The ICSSG cores: which ICSSG, which slice. A core sees its own slice's
 * data RAM at 0x0000, the other slice's at 0x2000, and its ICSSG's shared
 * RAM at 0x10000. In the SoC, data RAM 0 (slice 0's) is at the ICSSG's base,
 * data RAM 1 at base + 0x2000, and shared RAM at base + 0x10000.
 */
static const struct {
	const char *name;
	uint64_t base;
	uint32_t slice;
} icssg_cores[] = {
	{ "b034000.pru", 0x0b000000ull, 0 },
	{ "b004000.rtu", 0x0b000000ull, 0 },
	{ "b00a000.txpru", 0x0b000000ull, 0 },
	{ "b038000.pru", 0x0b000000ull, 1 },
	{ "b006000.rtu", 0x0b000000ull, 1 },
	{ "b00c000.txpru", 0x0b000000ull, 1 },
	{ "b134000.pru", 0x0b100000ull, 0 },
	{ "b104000.rtu", 0x0b100000ull, 0 },
	{ "b10a000.txpru", 0x0b100000ull, 0 },
	{ "b138000.pru", 0x0b100000ull, 1 },
	{ "b106000.rtu", 0x0b100000ull, 1 },
	{ "b10c000.txpru", 0x0b100000ull, 1 },
};

/* The SoC address of device address da on core name. */
static uint64_t core_da_to_pa(const char *name, uint32_t da)
{
	size_t i;

	for (i = 0; i < sizeof(core_windows) / sizeof(core_windows[0]); i++) {
		if (!strcmp(core_windows[i].name, name) && da >= core_windows[i].da &&
		    da - core_windows[i].da < core_windows[i].len) {
			return core_windows[i].pa + (da - core_windows[i].da);
		}
	}
	for (i = 0; i < sizeof(icssg_cores) / sizeof(icssg_cores[0]); i++) {
		uint64_t base = icssg_cores[i].base;
		uint32_t slice = icssg_cores[i].slice;

		if (strcmp(icssg_cores[i].name, name)) {
			continue;
		}
		if (da < 0x2000) {
			return base + 0x2000u * slice + da;
		}
		if (da < 0x4000) {
			return base + 0x2000u * (1u - slice) + (da - 0x2000u);
		}
		if (da >= 0x10000 && da < 0x20000) {
			return base + da;
		}
	}
	return da;
}

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

/* A remoteproc attribute (firmware, state) of the core called name, or -1. */
static int rproc_attr(const char *name, const char *attr, char *val, size_t len)
{
	char path[128], got[64];
	int i;

	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/name", i);
		if (read_sysfs(path, got, sizeof(got)) || strcmp(got, name)) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/%s", i, attr);
		return read_sysfs(path, val, len);
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
	char state[32];
	uint64_t pa;
	volatile char *buf;
	long page = sysconf(_SC_PAGESIZE);
	char *mapped;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s trace <remoteproc-name>, such as 4d80800000.dsp\n", argv[0]);
		return 2;
	}
	name = argv[2];
	if (rproc_attr(name, "firmware", fw, sizeof(fw))) {
		fprintf(stderr, "no remoteproc called %s\n", name);
		return 1;
	}
	/* A powered-off DSP's RAM hangs the SoC when read. */
	if (rproc_attr(name, "state", state, sizeof(state)) || strcmp(state, "running")) {
		fprintf(stderr, "%s is %s; start it first (ai64-demo does)\n", name, state);
		return 1;
	}
	snprintf(path, sizeof(path), "/lib/firmware/%s", fw);
	if (elf_trace(path, &da, &len)) {
		return 1;
	}
	pa = core_da_to_pa(name, da);
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

#define FABRIC_DT "/proc/device-tree/reserved-memory/ai64-fabric@a9000000"

static double now_us(void);

/* "once" or a number of seconds, for the pwm and sched windows. */
static int every_arg(int argc, char **argv, int *once)
{
	*once = argc > 2 && !strcmp(argv[2], "once");
	if (argc > 3) {
		return -1;
	}
	return argc > 2 && !*once ? atoi(argv[2]) : 10;
}

/*
 * The four PWM generators: the fabric node whose page holds its status
 * (struct ai64_pwm_status, at word rec of the app block), the core that
 * writes it, its pins, and what makes the edges.
 */
static const struct {
	const char *pins;
	const char *a, *b;
	uint32_t node, rec;
	const char *core;
	const char *how;
} pwm_gens[] = {
	{ "P8.11 + P8.12", "P8.11", "P8.12", AI64_FAB_G0RTU0, 0, "b004000.rtu",
	  "software PWM: ICSSG0 slice 0, PRU0 sets the pins at the times it reads from IEP0" },
	{ "P8.15", "P8.15", NULL, AI64_FAB_G0RTU0, AI64_PWM_STATUS2_WORDS, "b004000.rtu",
	  "hardware PWM: ICSSG0's eCAP0 in APWM mode, a copy of P8.11 (A) that Tx_PRU0 sets" },
	{ "P9.12 + P9.15", "P9.12", "P9.15", AI64_FAB_G0RTU1, 0, "b006000.rtu",
	  "hardware PWM: ICSSG0 slice 1, the ICSSG's PWM module toggles the pins on IEP1 compares" },
	{ "P9.25", "P9.25", NULL, AI64_FAB_R5F0_0, 0, "5c00000.r5f",
	  "hardware PWM: the SoC's EHRPWM4, output B, set by Zephyr on R5F0_0 (Zephyr's PWM driver)" },
};

#define NPWM (sizeof(pwm_gens) / sizeof(pwm_gens[0]))

static void pwm_program(const volatile struct ai64_pwm_status *s)
{
	static const uint32_t hz[AI64_PWM_STEP_FREQS] = AI64_PWM_STEP_HZ;
	static const uint32_t pct[AI64_PWM_STEPS] = AI64_PWM_STEP_PCT;
	size_t i;

	if (s->steps_ms) {
		printf("  stepping: ");
		for (i = 0; i < AI64_PWM_STEP_FREQS; i++) {
			printf("%s%u", i ? ", " : "", hz[i] / 1000u);
		}
		printf(" kHz, at each ");
		for (i = 0; i < AI64_PWM_STEPS; i++) {
			printf("%s%u", i ? ", " : "", pct[i]);
		}
		printf("%% duty, %u ms a step\n", s->steps_ms);
	} else if (s->sweep_ms) {
		printf("  sweeping: the duty from its least to its most and back every %u ms\n",
		       s->sweep_ms);
	} else {
		printf("  fixed: period %u ns, high %u ns, as asked\n", s->ask_period_ns, s->ask_duty_ns);
	}
}

/*
 * ai64 pwm [seconds|once]: a few lines per generator every few seconds,
 * from what its talking core publishes in shared memory. Linux reads no
 * ICSSG0 or EHRPWM register, and nothing here is measured on the pins.
 */
static int pwm_main(int argc, char **argv)
{
	volatile struct ai64_pwm_status *st[NPWM] = { 0 };
	uint32_t at[NPWM] = { 0 }, periods[NPWM] = { 0 };
	int once, every = every_arg(argc, argv, &once);
	double t0 = now_us();
	char state[32];
	size_t g;

	if (every <= 0) {
		fprintf(stderr, "usage: %s pwm [seconds|once]\n", argv[0]);
		return 2;
	}
	if (access(FABRIC_DT, F_OK)) {
		fprintf(stderr, "%s is missing: the ai64-demo overlay is not applied.\n", FABRIC_DT);
		return 1;
	}
	for (g = 0; g < NPWM; g++) {
		volatile uint8_t *page = map_phys(AI64_FAB_ADDR + pwm_gens[g].node * AI64_FAB_PAGE,
						  AI64_FAB_PAGE);

		if (!page) {
			return 1;
		}
		st[g] = (volatile struct ai64_pwm_status *)
			&((volatile struct ai64_fab_page *)page)->app[pwm_gens[g].rec];
		at[g] = st[g]->at;
		periods[g] = st[g]->periods;
	}
	if (!once) {
		printf("pwm window ready: every %d s, what each PWM generator's core reports it runs\n"
		       "(read from shared memory; \"now\" is whichever step it was on)\n", every);
	}
	fflush(stdout);
	for (;;) {
		double t1, dt;

		sleep(once ? 1 : (unsigned int)every);
		t1 = now_us();
		dt = (t1 - t0) / 1e6;
		t0 = t1;
		for (g = 0; g < NPWM; g++) {
			volatile struct ai64_pwm_status *s = st[g];
			uint32_t period, duty, dead, n;

			printf("%-14s %s\n", pwm_gens[g].pins, pwm_gens[g].how);
			if (rproc_attr(pwm_gens[g].core, "state", state, sizeof(state)) ||
			    strcmp(state, "running")) {
				printf("  its talking core, %s, is not running\n", pwm_gens[g].core);
				continue;
			}
			if (s->magic != AI64_PWM_MAGIC) {
				printf("  no status yet\n");
				continue;
			}
			if (s->at == at[g]) {
				printf("  its status has stopped\n");
				continue;
			}
			at[g] = s->at;
			period = s->run_period_ns;
			duty = s->run_duty_ns;
			dead = s->run_dead_ns;
			n = s->periods - periods[g];
			periods[g] = s->periods;
			pwm_program(s);
			printf("  now: %.3f kHz, %.1f%% duty\n", period ? 1e6 / period : 0.0,
			       period ? 100.0 * duty / period : 0.0);
			if (pwm_gens[g].b) {
				printf("  %s (A) high %u ns of each %u ns period, from %u ns in\n",
				       pwm_gens[g].a, duty, period, AI64_PWM_T0_NS);
				printf("  %s (B) high %u ns: A inverted, but low for %u ns of dead band "
				       "either side of A's pulse\n", pwm_gens[g].b,
				       period - AI64_PWM_T0_NS - duty - 2u * dead, dead);
				printf("  %.0f periods/s, %u late edges", n / dt, s->late);
				if (s->engine == AI64_PWM_SOFT) {
					printf(" (the latest edge %u ns late)", s->late_max_ns);
				}
				printf("\n");
			} else {
				printf("  %s high %u ns of each %u ns period\n", pwm_gens[g].a, duty, period);
				if (s->engine == AI64_PWM_ECAP) {
					printf("  %.0f periods/s, counted from the eCAP's period flag\n", n / dt);
				}
			}
			if (s->ctl_ack != s->ctl_seq) {
				printf("  its last request is not running yet\n");
			}
		}
		fflush(stdout);
		if (once) {
			return 0;
		}
	}
}

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

static int sched_main(int argc, char **argv)
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

		if (rproc_attr("b134000.pru", "state", state, sizeof(state)) ||
		    strcmp(state, "running")) {
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

static const struct {
	const char *name;
	off_t job;
	double mhz;
} dsp_job[] = {
	{ "4d80800000.dsp", AI64_JOB_C66X0_PHYS, 1350 },
	{ "4d81800000.dsp", AI64_JOB_C66X1_PHYS, 1350 },
	{ "64800000.dsp", AI64_JOB_C7X_PHYS, 1000 },
};

static void fir_ref(const int16_t *x, const int16_t *h, int16_t *y, uint32_t count, uint32_t taps)
{
	uint32_t n, k;

	for (n = 0; n < count; n++) {
		int32_t acc = 0;

		for (k = 0; k < taps; k++) {
			acc += (int32_t)h[k] * x[n + k];
		}
		acc >>= 15;
		y[n] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)acc;
	}
}

/* Hand the DSP one job and wait for it. 0 on success. */
static int dsp_run(volatile uint32_t *job, uint32_t op, uint32_t count, uint32_t taps)
{
	uint32_t seq = job[AI64_JOB_W_REQ_SEQ] + 1;
	double t0;

	job[AI64_JOB_W_OP] = op;
	job[AI64_JOB_W_COUNT] = count;
	job[AI64_JOB_W_TAPS] = taps;
	__sync_synchronize();
	job[AI64_JOB_W_REQ_SEQ] = seq;
	t0 = now_us();
	while (job[AI64_JOB_W_DONE_SEQ] != seq) {
		if (now_us() - t0 > 2e6) {
			fprintf(stderr, "the DSP did not finish job %u in 2 s\n", seq);
			return -1;
		}
	}
	__sync_synchronize();
	if (job[AI64_JOB_W_STATUS] != AI64_JOB_OK) {
		fprintf(stderr, "the DSP refused job %u: status %u\n", seq, job[AI64_JOB_W_STATUS]);
		return -1;
	}
	return 0;
}

static int dsp_main(int argc, char **argv)
{
	uint32_t count = argc > 3 ? (uint32_t)atoi(argv[3]) : 4096;
	uint32_t taps = argc > 4 ? (uint32_t)atoi(argv[4]) : 32;
	int runs = argc > 5 ? atoi(argv[5]) : 20;
	static int16_t x[AI64_JOB_MAX_COUNT + AI64_JOB_MAX_TAPS], h[AI64_JOB_MAX_TAPS];
	static int16_t want[AI64_JOB_MAX_COUNT];
	volatile uint32_t *job = NULL;
	volatile int16_t *jx, *jh, *jy;
	char state[32];
	double mhz = 0, wait = 0, ping = 0, arm = 0, cycles = 0, t0;
	uint32_t i;
	size_t c;
	int r;

	if (argc < 3 || count == 0 || count > AI64_JOB_MAX_COUNT || taps == 0 ||
	    taps > AI64_JOB_MAX_TAPS || runs <= 0) {
		fprintf(stderr, "usage: %s dsp <remoteproc-name> [count <= %u] [taps <= %u] [runs]\n",
			argv[0], AI64_JOB_MAX_COUNT, AI64_JOB_MAX_TAPS);
		return 2;
	}
	/* A powered-off DSP's L2 SRAM does not answer, and reading it hangs the SoC. */
	if (!rproc_attr(argv[2], "state", state, sizeof(state)) && strcmp(state, "running")) {
		fprintf(stderr, "%s is %s; start it first (ai64-demo does)\n", argv[2], state);
		return 1;
	}
	for (c = 0; c < sizeof(dsp_job) / sizeof(dsp_job[0]); c++) {
		if (!strcmp(dsp_job[c].name, argv[2])) {
			job = map_phys(dsp_job[c].job, AI64_JOB_SIZE);
			mhz = dsp_job[c].mhz;
			if (!job) {
				return 1;
			}
		}
	}
	if (!job) {
		fprintf(stderr, "%s is not a DSP; try 4d80800000.dsp, 4d81800000.dsp, or 64800000.dsp\n", argv[2]);
		return 2;
	}
	if (job[AI64_JOB_W_MAGIC] != AI64_JOB_MAGIC) {
		fprintf(stderr, "%s has no job block. Is it running ai64-c66x.elf or ai64-c7x.elf?\n", argv[2]);
		return 1;
	}
	jx = (volatile int16_t *)((volatile uint8_t *)job + AI64_JOB_X_OFF);
	jh = (volatile int16_t *)((volatile uint8_t *)job + AI64_JOB_H_OFF);
	jy = (volatile int16_t *)((volatile uint8_t *)job + AI64_JOB_Y_OFF);

	srand(1);
	for (r = 0; r < runs; r++) {
		/* New data each run, so a stale result cannot pass. */
		for (i = 0; i < taps; i++) {
			h[i] = (int16_t)(rand() % 8192 - 4096);
			jh[i] = h[i];
		}
		for (i = 0; i < count + taps - 1; i++) {
			x[i] = (int16_t)(rand() % 65536 - 32768);
			jx[i] = x[i];
		}
		t0 = now_us();
		fir_ref(x, h, want, count, taps);
		arm += now_us() - t0;

		t0 = now_us();
		if (dsp_run(job, AI64_JOB_FIR, count, taps)) {
			return 1;
		}
		wait += now_us() - t0;
		cycles += job[AI64_JOB_W_CYCLES];
		for (i = 0; i < count; i++) {
			if (jy[i] != want[i]) {
				printf("run %d: y[%u] is %d, expected %d\n", r, i, jy[i], want[i]);
				return 1;
			}
		}

		t0 = now_us();
		if (dsp_run(job, AI64_JOB_PING, 0, 0)) {
			return 1;
		}
		ping += now_us() - t0;
	}
	printf("%s: %d FIR jobs, %u samples x %u taps, all match the A72\n", argv[2], runs, count, taps);
	printf("  DSP filter %.0f cycles = %.1f us; A72 waited %.1f us; empty job %.1f us\n",
	       cycles / runs, cycles / runs / mhz, wait / runs, ping / runs);
	printf("  the same filter on one A72 core: %.1f us\n", arm / runs);
	return 0;
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
			snprintf(shown, sizeof(shown), "%s: pwm: the call failed (only R5F0_0 has P9.25)",
				 g_who);
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
			snprintf(shown, sizeof(shown), "%s: steps: the call failed (only R5F0_0 has P9.25)",
				 g_who);
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
			 "%s: commands are say <text> | pwm <period_ns> <duty_ns> | steps <ms> | chunk <text> | spi [text]",
			 g_who);
		note_line(shown);
	}
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
 * reply FIFO, which the linux window prints as they come. Only R5F0_0 has
 * EHRPWM4, the bulk buffer, and SPI7.
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
	{ "r5f0_0", "steps 5" },
	{ "rtu0", "status" },
	{ "r5f0_1", "say hello from linux" },
	{ "r5f1_0", "say hello from linux" },
	{ "r5f1_1", "say hello from linux" },
};

static void usage_hint(void)
{
	printf("use r5f0_0 say|pwm|steps|chunk|spi ..., r5f0_1|r5f1_0|r5f1_1 say <text>, "
	       "rtu0 pwm|steps|sweep|dead|status|<text>, or help\n");
	fflush(stdout);
}

static int linux_main(int argc, char **argv)
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

	printf("linux window ready. commands: r5f0_0 say|pwm|steps|chunk|spi ... | r5f0_1|r5f1_0|r5f1_1 say ... | "
	       "rtu0 pwm|steps|sweep|dead|status|<text> | help\n");
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

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s peer|erpc|pwm|sched|pads|trace|bench|dsp|shutdown|fabric|linux ... (-h for help)\n", argv[0]);
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
	if (!strcmp(argv[1], "pwm")) {
		return pwm_main(argc, argv);
	}
	if (!strcmp(argv[1], "sched")) {
		return sched_main(argc, argv);
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
	if (!strcmp(argv[1], "dsp")) {
		return dsp_main(argc, argv);
	}
	if (!strcmp(argv[1], "shutdown")) {
		return shutdown_main(argc, argv);
	}
	if (!strcmp(argv[1], "fabric")) {
		return fabric_main(argc, argv);
	}
	if (!strcmp(argv[1], "linux")) {
		return linux_main(argc, argv);
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
