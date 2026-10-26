/* SPDX-License-Identifier: MIT */
/*
 * ai64 web [port]
 *   Serves a page of what every demo core is doing, live, on port 8064 (or
 *   port), to anyone on the board's network. The page (web/index.html,
 *   built into the program by ai64_web_page.S) asks for /status.json twice
 *   a second and draws it: each core's remoteproc state and firmware, every
 *   fabric node's page (its heartbeat, its pings to the others), the four
 *   PWM generators' status records, and the pipeline's stages, score, and
 *   confusion matrix.
 *
 *   It only reads: sysfs, and shared memory through /dev/mem, mapped read
 *   only. It joins no link and takes no lock, so it changes nothing the
 *   demo does, and the counters it sends are the cores' own running totals;
 *   the page turns them into rates. ai64-demo starts it; nothing starts it
 *   at boot.
 *
 * It needs root or CAP_SYS_RAWIO, for /dev/mem.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ai64_shm.h"
#include "ai64_fabric.h"
#include "ai64_pipe.h"

int web_main(int argc, char **argv);

#define RESERVED  "/proc/device-tree/reserved-memory/ai64-fabric@a9000000"
#define WEB_PORT  8064

/* web/index.html, from ai64_web_page.S. */
extern const char ai64_web_page[], ai64_web_page_end[];

static volatile uint32_t *g_gtc;
static volatile uint8_t *g_fab;
static volatile struct ai64_fab_tally *g_tally;
static volatile struct ai64_pipe *g_pipe;

static void *map_ro(off_t addr, size_t len)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	void *p;

	if (fd < 0) {
		perror("/dev/mem");
		return NULL;
	}
	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, addr);
	close(fd);
	if (p == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}
	return p;
}

/* The JSON reply, built up in one buffer. */
static char g_out[96 * 1024];
static size_t g_len;

static void out(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (g_len >= sizeof(g_out)) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(g_out + g_len, sizeof(g_out) - g_len, fmt, ap);
	va_end(ap);
	if (n > 0) {
		g_len += (size_t)n;
	}
}

/* A sysfs value, as a JSON string; "" when it cannot be read. */
static void out_sysfs(const char *path)
{
	char buf[128];
	FILE *f = fopen(path, "r");
	size_t i;

	buf[0] = '\0';
	if (f) {
		if (!fgets(buf, sizeof(buf), f)) {
			buf[0] = '\0';
		}
		fclose(f);
	}
	buf[strcspn(buf, "\n")] = '\0';
	out("\"");
	for (i = 0; buf[i]; i++) {
		if (buf[i] == '"' || buf[i] == '\\') {
			out("\\%c", buf[i]);
		} else if ((unsigned char)buf[i] >= 0x20) {
			out("%c", buf[i]);
		}
	}
	out("\"");
}

/* Node n's page: AI64_FAB_PAGE apart, more than the struct. */
static volatile struct ai64_fab_page *page(uint32_t n)
{
	return (volatile struct ai64_fab_page *)(g_fab + n * AI64_FAB_PAGE);
}

static void out_u32s(const volatile uint32_t *v, size_t n)
{
	size_t i;

	out("[");
	for (i = 0; i < n; i++) {
		out("%s%u", i ? "," : "", v[i]);
	}
	out("]");
}

static uint64_t gtc_now(void)
{
	uint32_t hi, lo;

	do {
		hi = g_gtc[3];
		lo = g_gtc[2];
	} while (g_gtc[3] != hi);
	return ((uint64_t)hi << 32) | lo;
}

static void status_json(void)
{
	struct timespec ts;
	char path[96];
	uint32_t n, p, c;
	int i;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	g_len = 0;
	out("{\"t\":%.4f,\"gtc\":%llu,\"gtc_hz\":%u,", ts.tv_sec + ts.tv_nsec / 1e9,
	    (unsigned long long)gtc_now(), AI64_GTC_HZ);

	/* Every remoteproc, demo core or not: the page picks out its own. */
	out("\"rproc\":[");
	for (i = 0, n = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/name", i);
		if (access(path, F_OK)) {
			continue;
		}
		out("%s{\"name\":", n++ ? "," : "");
		out_sysfs(path);
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/state", i);
		out(",\"state\":");
		out_sysfs(path);
		snprintf(path, sizeof(path), "/sys/class/remoteproc/remoteproc%d/firmware", i);
		out(",\"fw\":");
		out_sysfs(path);
		out("}");
	}
	out("],");

	/*
	 * Each fabric node's own record and its pings to each peer:
	 * [pongs, timeouts, errors, rtt_sum, oneway_sum, rtt_min, rtt_max, up].
	 */
	out("\"fabric\":[");
	for (n = 0; n < AI64_FAB_NODES; n++) {
		volatile struct ai64_fab_own *o = &page(n)->own;

		out("%s{\"up\":%u,\"boot\":%u,\"beat\":%u,\"polls\":%u,\"sends\":%u,\"forwarded\":%u,"
		    "\"peers\":[", n ? "," : "", o->magic == AI64_FAB_MAGIC && o->node == n, o->boot,
		    o->beat, o->polls, o->sends, o->forwarded);
		for (p = 0; p < AI64_FAB_NODES; p++) {
			out("%s", p ? "," : "");
			out_u32s((const volatile uint32_t *)&page(n)->peer[p],
				 sizeof(struct ai64_fab_peer) / 4u);
		}
		out("]}");
	}
	out("],\"tally\":{\"ok\":%u,\"total\":%u,\"count\":", g_tally->magic == AI64_FAB_MAGIC,
	    g_tally->total);
	out_u32s(g_tally->count, AI64_FAB_NODES);
	out("},");

	/* The four PWM generators, in the order of ai64 pwm. */
	{
		static const struct { uint32_t node, rec; } gen[] = {
			{ AI64_FAB_G0RTU0, 0 },
			{ AI64_FAB_G0RTU0, AI64_PWM_STATUS2_WORDS },
			{ AI64_FAB_G0RTU1, 0 },
			{ AI64_FAB_R5F0_1, 0 },
		};

		out("\"pwm\":[");
		for (n = 0; n < sizeof(gen) / sizeof(gen[0]); n++) {
			volatile struct ai64_pwm_status *s =
				(volatile struct ai64_pwm_status *)&page(gen[n].node)->app[gen[n].rec];

			out("%s{\"ok\":%u,\"engine\":%u,\"ask_period\":%u,\"sweep_ms\":%u,"
			    "\"steps_ms\":%u,\"period\":%u,\"duty\":%u,\"dead\":%u,\"periods\":%u,"
			    "\"late\":%u,\"at\":%u}", n ? "," : "", s->magic == AI64_PWM_MAGIC,
			    s->engine, s->ask_period_ns, s->sweep_ms, s->steps_ms, s->run_period_ns,
			    s->run_duty_ns, s->run_dead_ns, s->periods, s->late, s->at);
		}
		out("],");
	}

	/*
	 * The pipeline: each stage's
	 * [magic ok, windows, dropped, last, lat_sum, lat_min, lat_max, work_sum, work_max, errors],
	 * the score, and the newest verdict.
	 */
	{
		volatile struct ai64_pipe_ctl *ctl = &g_pipe->ctl;
		volatile struct ai64_pipe_score *sc = &g_pipe->score;
		volatile struct ai64_pipe_result *r;

		out("\"pipe\":{\"ok\":%u,\"run\":%u,\"follow\":%u,\"steps_ms\":%u,\"weights\":%u,"
		    "\"stages\":[", ctl->magic == AI64_PIPE_MAGIC, ctl->run, ctl->follow,
		    ctl->steps_ms, ctl->weights_seq);
		for (n = 0; n < AI64_PIPE_STAGES; n++) {
			volatile struct ai64_pipe_stage *st = &g_pipe->stage[n];

			out("%s[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", n ? "," : "",
			    st->magic == AI64_PIPE_MAGIC, st->windows, st->dropped, st->last,
			    st->lat_sum, st->lat_min, st->lat_max, st->work_sum, st->work_max,
			    st->errors);
		}
		out("],\"score\":{\"right\":%u,\"wrong\":%u,\"mixed\":%u,\"changes\":%u,"
		    "\"cls\":%d,\"waits\":%u,\"confusion\":[", sc->right, sc->wrong, sc->mixed,
		    sc->changes, sc->cls < AI64_PIPE_CLASSES ? (int)sc->cls : -1, sc->waits);
		for (c = 0; c < AI64_PIPE_CLASSES; c++) {
			out("%s", c ? "," : "");
			out_u32s(sc->confusion[c], AI64_PIPE_CLASSES);
		}
		r = &g_pipe->result[g_pipe->stage[AI64_PIPE_MMA].last % AI64_PIPE_RESULTS];
		out("]},\"newest\":{\"win\":%u,\"cls\":%u,\"score\":%d,\"second\":%d}}}", r->win,
		    r->cls, r->score, r->second);
	}
}

static void reply(int fd, const char *status, const char *type, const char *body, size_t len)
{
	char head[256];
	int n = snprintf(head, sizeof(head),
			 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len);

	if (write(fd, head, (size_t)n) == n) {
		while (len) {
			ssize_t w = write(fd, body, len);

			if (w <= 0) {
				break;
			}
			body += w;
			len -= (size_t)w;
		}
	}
}

static void serve(int fd)
{
	char req[1024];
	ssize_t n = read(fd, req, sizeof(req) - 1);
	char *path, *end;

	if (n <= 0) {
		return;
	}
	req[n] = '\0';
	if (strncmp(req, "GET ", 4)) {
		reply(fd, "405 Method Not Allowed", "text/plain", "GET only\n", 9);
		return;
	}
	path = req + 4;
	end = strpbrk(path, " ?\r\n");
	if (end) {
		*end = '\0';
	}
	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		reply(fd, "200 OK", "text/html; charset=utf-8", ai64_web_page,
		      (size_t)(ai64_web_page_end - ai64_web_page));
	} else if (!strcmp(path, "/status.json")) {
		status_json();
		reply(fd, "200 OK", "application/json", g_out, g_len);
	} else {
		reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
	}
}

/* The addresses the page can be reached on. */
static void print_urls(int port)
{
	struct ifaddrs *ifa, *i;
	char ip[INET_ADDRSTRLEN];
	int any = 0;

	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
			    (i->ifa_flags & IFF_LOOPBACK)) {
				continue;
			}
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip,
				  sizeof(ip));
			printf("  http://%s:%d/  (%s)\n", ip, port, i->ifa_name);
			any = 1;
		}
		freeifaddrs(ifa);
	}
	if (!any) {
		printf("  http://<board>:%d/\n", port);
	}
}

int web_main(int argc, char **argv)
{
	int port = argc > 2 ? atoi(argv[2]) : WEB_PORT;
	struct sockaddr_in a = { 0 };
	struct timeval tv = { .tv_sec = 2 };
	int s, one = 1;

	if (port <= 0 || port > 65535 || argc > 3) {
		fprintf(stderr, "usage: %s web [port]\n", argv[0]);
		return 2;
	}
	if (access(RESERVED, F_OK)) {
		fprintf(stderr, "%s is missing: the ai64-demo overlay is not applied.\n", RESERVED);
		return 1;
	}
	g_gtc = map_ro(AI64_GTC_ADDR, 4096);
	g_fab = map_ro(AI64_FAB_ADDR, AI64_FAB_SIZE);
	g_pipe = map_ro(AI64_PIPE_ADDR, AI64_PIPE_SIZE);
	if (!g_gtc || !g_fab || !g_pipe) {
		return 1;
	}
	g_tally = (volatile struct ai64_fab_tally *)(g_fab + AI64_FAB_TALLY_OFF);

	signal(SIGPIPE, SIG_IGN);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 8)) {
		fprintf(stderr, "port %d: %s\n", port, strerror(errno));
		close(s);
		return 1;
	}
	printf("web page: every core's state, the fabric, the PWM generators, and the pipeline, "
	       "live (read only)\n");
	print_urls(port);
	fflush(stdout);

	/* One request at a time: each is a page or a few KiB of JSON. */
	for (;;) {
		int c = accept(s, NULL, NULL);

		if (c < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("accept");
			return 1;
		}
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		serve(c);
		close(c);
	}
}
