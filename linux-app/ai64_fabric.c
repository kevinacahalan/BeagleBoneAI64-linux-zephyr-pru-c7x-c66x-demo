/* SPDX-License-Identifier: MIT */
/*
 * Linux on the fabric (zephyr/include/ai64_fabric.h).
 *
 * ai64 fabric [lines|once]
 *   Joins the fabric as node 0 and shows it: the one-way latency from every
 *   node to every other, each node's pings, R5F0_0's tour, and the spinlock
 *   tally. It redraws every second; lines prints one line every 10 s
 *   instead, for the merged console, and once draws one screen and exits.
 *
 * ai64 fabric route <node>... [arm <period> <count> | xy <x> <y>]
 *   Sends one message along the nodes given, and back to Linux. Each node
 *   stamps it as it passes. With arm or xy, RTU0 runs that command when the
 *   message reaches it, so any core can make Tx_PRU0 pulse or PRU0 send a
 *   frame. Prints the time each hop took. It works with the viewer running.
 *
 * ai64 fabric reset
 *   Frees the tally's spinlock and zeroes the tally, for a core that was
 *   stopped while it held the lock.
 *
 * All of it reads and writes /dev/mem, so it needs root or CAP_SYS_RAWIO.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>


static volatile uint32_t *g_gtc;
static uintptr_t g_fab;
static uintptr_t g_mbox;
static uintptr_t g_lock;

#define AI64_FAB_NOW()    (g_gtc[2])
#define AI64_FAB_BASE     g_fab
#define AI64_FAB_MBOX     g_mbox
#define AI64_FAB_LOCKREG  g_lock
#define AI64_FAB_FENCE()  __sync_synchronize()
/* A viewer killed with the lock taken would leave it taken. */
static sigset_t g_lock_sigs;
#define AI64_FAB_LOCK_ENTER() sigprocmask(SIG_BLOCK, &g_lock_sigs, NULL)
#define AI64_FAB_LOCK_EXIT()  sigprocmask(SIG_UNBLOCK, &g_lock_sigs, NULL)
#include "ai64_fabric.h"

int fabric_main(int argc, char **argv);

#define RESERVED "/proc/device-tree/reserved-memory/ai64-fabric@a9000000"
#define ROUTE_LOCK "/tmp/ai64-fabric-route.lock"
#define NODE0_LOCK "/tmp/ai64-fabric-node0.lock"

static const char *const node_name[AI64_FAB_NODES] = {
	"linux", "r5f0_0", "r5f0_1", "r5f1_0", "r5f1_1", "c66x0",
	"c66x1", "c7x", "rtu0", "g0pru1", "g1pru0", "g1pru1",
};

static void *map(off_t addr, size_t len)
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

/*
 * Without the overlay's reserved-memory node, the fabric's DDR is Linux's
 * own RAM, and writing it would corrupt the kernel.
 */
static int fabric_map(void)
{
	void *fab, *gtc, *mbox, *lock;

	if (access(RESERVED, F_OK)) {
		fprintf(stderr, "%s is missing: the ai64-demo overlay is not applied, so the\n"
			"fabric's DDR belongs to Linux. Run install-beagle.sh and reboot.\n", RESERVED);
		return -1;
	}
	fab = map(AI64_FAB_ADDR, AI64_FAB_SIZE);
	gtc = map(AI64_GTC_ADDR, 4096);
	mbox = map(AI64_FAB_MBOX_ADDR, 4096);
	lock = map(AI64_SPINLOCK_ADDR, 4096);
	if (!fab || !gtc || !mbox || !lock) {
		return -1;
	}
	g_fab = (uintptr_t)fab;
	g_gtc = gtc;
	g_mbox = (uintptr_t)mbox;
	g_lock = (uintptr_t)lock + AI64_FAB_LOCK_OFF;
	sigemptyset(&g_lock_sigs);
	sigaddset(&g_lock_sigs, SIGINT);
	sigaddset(&g_lock_sigs, SIGTERM);
	sigaddset(&g_lock_sigs, SIGHUP);
	return 0;
}

static int node_by_name(const char *s)
{
	uint32_t i;

	for (i = 0; i < AI64_FAB_NODES; i++) {
		if (!strcmp(s, node_name[i])) {
			return (int)i;
		}
	}
	return -1;
}

static double mono_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double ticks_us(uint32_t t)
{
	return t / (AI64_GTC_HZ / 1e6);
}

/* What the viewer reads of each node, once a draw. */
struct snap {
	int live;
	struct ai64_fab_own own;
	struct ai64_fab_peer peer[AI64_FAB_NODES];
};

static void take(struct snap *s, uint32_t now)
{
	uint32_t n, i;

	for (n = 0; n < AI64_FAB_NODES; n++) {
		volatile struct ai64_fab_page *p = FAB_PAGE(n);
		const volatile uint32_t *src = (const volatile uint32_t *)&p->own;
		uint32_t *dst = (uint32_t *)&s[n].own;

		for (i = 0; i < sizeof(s[n].own) / 4u; i++) {
			dst[i] = src[i];
		}
		src = (const volatile uint32_t *)p->peer;
		dst = (uint32_t *)s[n].peer;
		for (i = 0; i < sizeof(s[n].peer) / 4u; i++) {
			dst[i] = src[i];
		}
		/* A node that beat in the last 100 ms is running. It may have
		 * beaten since now was read, so the difference can be negative. */
		s[n].live = s[n].own.magic == AI64_FAB_MAGIC && s[n].own.node == n &&
			    (int32_t)(now - s[n].own.beat) < (int32_t)AI64_FAB_US(100000);
	}
}

/* The tally, read under its lock. -1 when the lock stayed busy. */
static int tally_read(uint32_t *total, uint32_t *sum, uint32_t *holder)
{
	volatile struct ai64_fab_tally *t = FAB_TALLY;
	uint32_t i, tries = 0;

	AI64_FAB_LOCK_ENTER();
	while (FAB_LOCK != 0u) {
		if (++tries == 100000u) {
			AI64_FAB_LOCK_EXIT();
			*holder = t->holder;
			return -1;
		}
	}
	*total = t->total;
	*holder = t->holder;
	*sum = 0;
	for (i = 0; i < AI64_FAB_NODES; i++) {
		*sum += t->count[i];
	}
	__sync_synchronize();
	FAB_LOCK = 0;
	AI64_FAB_LOCK_EXIT();
	return 0;
}

/* The tour's last lap, as "r5f0_0 >1.2> c66x0 >0.8> ... r5f0_0". */
static void print_tour_route(const struct ai64_fab_own *o)
{
	uint32_t h, prev = o->tour_t0;

	if (o->tour_nhops == 0 || o->tour_nhops > AI64_FAB_MAX_HOPS) {
		return;
	}
	printf("  r5f0_0");
	for (h = 0; h < o->tour_nhops; h++) {
		uint32_t n = ai64_fab_route_node(o->tour_route, h);

		printf(" >%.1f> %s", ticks_us(o->tour_stamp[h] - prev),
		       n < AI64_FAB_NODES ? node_name[n] : "?");
		prev = o->tour_stamp[h];
	}
	printf("\n");
}

static void draw(const struct snap *s, const struct snap *was, double dt, int lines)
{
	const struct ai64_fab_own *tour = &s[AI64_FAB_R5F0_0].own;
	const struct ai64_fab_own *tour_was = &was[AI64_FAB_R5F0_0].own;
	uint32_t r, c, up = 0, pongs = 0, timeouts = 0, errors = 0;
	uint32_t total = 0, sum = 0, holder = 0, laps;
	double ow_min = 1e9, ow_max = 0;
	int tally;

	for (r = 0; r < AI64_FAB_NODES; r++) {
		up += s[r].live;
		for (c = 0; c < AI64_FAB_NODES; c++) {
			const struct ai64_fab_peer *p = &s[r].peer[c], *q = &was[r].peer[c];
			uint32_t n = p->pongs - q->pongs;
			double ow;

			if (!s[r].live || !was[r].live || s[r].own.boot != was[r].own.boot ||
			    r == c) {
				continue;
			}
			pongs += n;
			timeouts += p->timeouts - q->timeouts;
			errors += p->errors - q->errors;
			if (n) {
				ow = ticks_us(p->oneway_sum - q->oneway_sum) / n;
				ow_min = ow < ow_min ? ow : ow_min;
				ow_max = ow > ow_max ? ow : ow_max;
			}
		}
	}
	tally = tally_read(&total, &sum, &holder);
	laps = tour->tour_laps - tour_was->tour_laps;

	if (lines) {
		printf("%u of %u nodes up, %.0f pongs/s, one-way %.1f to %.1f us, "
		       "%u timeouts, %u errors; tour %.0f laps/s, %.1f us a lap; ",
		       up, AI64_FAB_NODES, pongs / dt, ow_min > ow_max ? 0 : ow_min, ow_max,
		       timeouts, errors, laps / dt,
		       laps ? ticks_us(tour->tour_sum - tour_was->tour_sum) / laps : 0.0);
		if (tally) {
			printf("tally lock busy (holder %u)\n", holder);
		} else {
			printf("tally %u, %s\n", total, total == sum ? "counts agree" : "COUNTS DIFFER");
		}
		fflush(stdout);
		return;
	}

	printf("\033[H\033[J");
	printf("fabric: one-way latency in us, from the row's node to the column's, "
	       "over the last %.1f s\n", dt);
	printf("%-7s", "");
	for (c = 0; c < AI64_FAB_NODES; c++) {
		printf("%7s", node_name[c]);
	}
	printf("\n");
	for (r = 0; r < AI64_FAB_NODES; r++) {
		printf("%-7s", node_name[r]);
		for (c = 0; c < AI64_FAB_NODES; c++) {
			const struct ai64_fab_peer *p = &s[r].peer[c], *q = &was[r].peer[c];
			uint32_t n = p->pongs - q->pongs;

			if (r == c) {
				printf("%7s", ".");
			} else if (!s[r].live || s[r].own.boot != was[r].own.boot) {
				printf("%7s", "");
			} else if (!p->up || !n) {
				printf("%7s", "--");
			} else {
				printf("%7.1f", ticks_us(p->oneway_sum - q->oneway_sum) / n);
			}
		}
		printf("\n");
	}

	printf("\n%-7s %6s %9s %8s %9s %18s %8s %6s %9s\n", "node", "starts", "polls/s",
	       "pongs/s", "rtt us", "rtt min to max us", "timeouts", "errors", "passed/s");
	for (r = 0; r < AI64_FAB_NODES; r++) {
		const struct ai64_fab_own *o = &s[r].own, *ow = &was[r].own;
		uint32_t n = 0, rtt = 0, mn = 0xffffffffu, mx = 0, to = 0, er = 0;

		if (!s[r].live) {
			printf("%-7s %s\n", node_name[r], r == AI64_FAB_LINUX ? "" : "not running");
			continue;
		}
		if (o->boot != ow->boot) {
			printf("%-7s %6u started again\n", node_name[r], o->boot);
			continue;
		}
		for (c = 0; c < AI64_FAB_NODES; c++) {
			const struct ai64_fab_peer *p = &s[r].peer[c], *q = &was[r].peer[c];

			if (c == r) {
				continue;
			}
			n += p->pongs - q->pongs;
			rtt += p->rtt_sum - q->rtt_sum;
			to += p->timeouts - q->timeouts;
			er += p->errors - q->errors;
			if (p->up && p->rtt_max) {
				mn = p->rtt_min < mn ? p->rtt_min : mn;
				mx = p->rtt_max > mx ? p->rtt_max : mx;
			}
		}
		printf("%-7s %6u %9.0f %8.0f %9.1f %8.1f to %6.1f %8u %6u %9.0f\n", node_name[r],
		       o->boot, (o->polls - ow->polls) / dt, n / dt, n ? ticks_us(rtt) / n : 0.0,
		       mx ? ticks_us(mn) : 0.0, ticks_us(mx), to, er,
		       (o->forwarded - ow->forwarded) / dt);
	}

	printf("\ntour from r5f0_0 round every node that answers its pings: ");
	if (!s[AI64_FAB_R5F0_0].live || tour->boot != tour_was->boot) {
		printf("r5f0_0 is not running, or started again\n");
	} else {
		printf("%.0f laps/s, %.1f us a lap (%.1f to %.1f), errors %u, timeouts %u\n",
		       laps / dt, laps ? ticks_us(tour->tour_sum - tour_was->tour_sum) / laps : 0.0,
		       ticks_us(tour->tour_min), ticks_us(tour->tour_max),
		       tour->tour_errors, tour->tour_timeouts);
		print_tour_route(tour);
	}

	printf("\ntally under hardware spinlock %u: ", AI64_FAB_LOCK);
	if (tally) {
		printf("the lock stayed busy, holder %u (ai64 fabric reset frees it)\n", holder);
	} else {
		printf("%u adds, and the %u counts sum to %u%s\n", total, AI64_FAB_NODES, sum,
		       total == sum ? "" : "  DIFFERENT: a core stopped inside the lock?");
	}
	fflush(stdout);
}

static int view_main(int lines, int once)
{
	static struct snap a[AI64_FAB_NODES], b[AI64_FAB_NODES];
	struct snap *s = a, *was = b, *tmp;
	const struct timespec gap = { 0, 20000 };
	double period = lines ? 10.0 : 1.0, t0, t;
	int fd;

	/* Two viewers would both answer as node 0, and garble each other. */
	fd = open(NODE0_LOCK, O_RDONLY | O_CREAT, 0644);
	if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
		fprintf(stderr, "another ai64 fabric viewer is running (the fabric window?); "
			"Linux can only be node 0 once\n");
		return 1;
	}
	fab_init(AI64_FAB_LINUX);
	if (lines) {
		printf("fabric window ready, a line every 10 s\n");
		fflush(stdout);
	}
	/*
	 * Pings sent to Linux before it joined time out within 50 ms. Count
	 * from after that, so the first draw does not show them.
	 */
	t0 = mono_s();
	while (mono_s() - t0 < 0.1) {
		fab_poll();
		nanosleep(&gap, NULL);
	}
	t0 = mono_s();
	take(was, AI64_FAB_NOW());
	for (;;) {
		fab_poll();
		nanosleep(&gap, NULL);
		t = mono_s();
		if (t - t0 < period) {
			continue;
		}
		take(s, AI64_FAB_NOW());
		draw(s, was, t - t0, lines);
		if (once) {
			return 0;
		}
		tmp = was;
		was = s;
		s = tmp;
		t0 = t;
	}
}

static int route_main(int argc, char **argv)
{
	static struct ai64_fab_msg m;
	volatile struct ai64_fab_page *mine, *first;
	uint32_t route[AI64_FAB_MAX_HOPS], stamp[AI64_FAB_MAX_HOPS], n = 0, h, last, base, v, prev;
	uint32_t action = AI64_FAB_ACT_NONE, a0 = 0, a1 = 0, act_node = 0;
	double start;
	int i, fd, node;

	for (i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "arm") || !strcmp(argv[i], "xy")) {
			if (i + 3 != argc) {
				break;
			}
			action = !strcmp(argv[i], "arm") ? AI64_FAB_ACT_ARM : AI64_FAB_ACT_XY;
			a0 = strtoul(argv[i + 1], NULL, 0);
			a1 = strtoul(argv[i + 2], NULL, 0);
			i = argc;
			break;
		}
		node = node_by_name(argv[i]);
		if (node < 0 || n + 1 >= AI64_FAB_MAX_HOPS) {
			fprintf(stderr, "%s: not a node, or too many hops\n", argv[i]);
			return 2;
		}
		route[n++] = (uint32_t)node;
	}
	if (n == 0 || i != argc) {
		fprintf(stderr, "usage: ai64 fabric route <node>... [arm <period> <count> | xy <x> <y>]\n"
			"nodes:");
		for (h = 1; h < AI64_FAB_NODES; h++) {
			fprintf(stderr, " %s", node_name[h]);
		}
		fprintf(stderr, "\n");
		return 2;
	}
	if (route[0] == AI64_FAB_LINUX) {
		fprintf(stderr, "the route starts at Linux; name the nodes after it\n");
		return 2;
	}
	if (action != AI64_FAB_ACT_NONE) {
		act_node = AI64_FAB_RTU0;
		for (h = 0; h < n && route[h] != AI64_FAB_RTU0; h++) {
		}
		if (h == n) {
			fprintf(stderr, "arm and xy run on rtu0, so the route has to pass rtu0\n");
			return 2;
		}
	}
	route[n++] = AI64_FAB_LINUX;

	if (fabric_map()) {
		return 1;
	}
	/* One route at a time: Linux has one ROUTE slot in each inbox. */
	fd = open(ROUTE_LOCK, O_RDONLY | O_CREAT, 0644);
	if (fd < 0 || flock(fd, LOCK_EX)) {
		perror(ROUTE_LOCK);
		return 1;
	}

	memset(&m, 0, sizeof(m));
	for (h = 0; h < n; h++) {
		m.route[h / 4] |= route[h] << (8 * (h % 4));
	}
	m.origin = AI64_FAB_LINUX;
	m.id = (uint32_t)getpid();
	m.nhops = n;
	m.action = action;
	m.action_node = act_node;
	m.arg0 = a0;
	m.arg1 = a1;
	m.status = 0xffffffffu;

	mine = FAB_PAGE(AI64_FAB_LINUX);
	first = FAB_PAGE(route[0]);
	last = route[n - 2];
	base = mine->rseq[last];
	m.t0 = AI64_FAB_NOW();
	m.value = m.t0;
	{
		volatile uint32_t *dw = (volatile uint32_t *)&first->slot[AI64_FAB_ROUTE][AI64_FAB_LINUX];
		const uint32_t *sw = (const uint32_t *)&m;

		for (h = 0; h < AI64_FAB_MSG_WORDS; h++) {
			dw[h] = sw[h];
		}
	}
	__sync_synchronize();
	first->rseq[AI64_FAB_LINUX] = first->rseq[AI64_FAB_LINUX] + 1u;
	__sync_synchronize();
	if ((FAB_MBOX_STATUS(route[0]) & 0x3fu) < AI64_FAB_MBOX_DEPTH) {
		FAB_MBOX_MSG(route[0]) = AI64_FAB_LINUX;
	}

	start = mono_s();
	while (mine->rseq[last] == base) {
		if (mono_s() - start > 1.0) {
			fprintf(stderr, "the route did not come back within 1 s; is every node on it running?\n");
			return 1;
		}
	}
	stamp[n - 1] = AI64_FAB_NOW();
	__sync_synchronize();
	fab_copy_in(&m, &mine->slot[AI64_FAB_ROUTE][last], AI64_FAB_MSG_WORDS);
	/* Each hop wrote its stamp into Linux's page. Linux's own is the one
	 * above: the viewer may not be running to make it. */
	for (h = 0; h + 1 < n; h++) {
		stamp[h] = mine->trip[AI64_FAB_ROUTE - AI64_FAB_TOUR][h];
	}

	v = m.t0;
	prev = m.t0;
	for (h = 0; h < n; h++) {
		uint32_t from = h ? route[h - 1] : AI64_FAB_LINUX;

		v = ai64_fab_mix(v, route[h]);
		printf("%-6s -> %-6s %8.1f us", node_name[from], node_name[route[h]],
		       ticks_us(stamp[h] - prev));
		if (action != AI64_FAB_ACT_NONE && route[h] == act_node) {
			act_node = AI64_FAB_NODES; /* only the first pass acts */
			printf("   %s %s", node_name[route[h]],
			       m.status ? "did not run the action" :
			       action == AI64_FAB_ACT_ARM ? "armed Tx_PRU0" : "handed PRU0 the X/Y");
		}
		printf("\n");
		prev = stamp[h];
	}
	/* Linux mixes in its number too, as the viewer would. */
	printf("%u hops, %.1f us, value %s\n", n, ticks_us(stamp[n - 1] - m.t0),
	       ai64_fab_mix(m.value, AI64_FAB_LINUX) == v ? "ok" : "WRONG");
	return 0;
}

static int reset_main(void)
{
	volatile struct ai64_fab_tally *t;
	uint32_t i, tries = 0;

	if (fabric_map()) {
		return 1;
	}
	t = FAB_TALLY;
	while (FAB_LOCK != 0u) {
		if (++tries == 100000u) {
			printf("spinlock %u was held (holder %u); freeing it\n", AI64_FAB_LOCK, t->holder);
			FAB_LOCK = 0;
			tries = 0;
		}
	}
	t->total = 0;
	t->holder = 0;
	for (i = 0; i < AI64_FAB_NODES; i++) {
		t->count[i] = 0;
	}
	t->magic = AI64_FAB_MAGIC;
	__sync_synchronize();
	FAB_LOCK = 0;
	printf("fabric tally zeroed\n");
	return 0;
}

int fabric_main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";

	if (!strcmp(mode, "route")) {
		return route_main(argc, argv);
	}
	if (!strcmp(mode, "reset")) {
		return reset_main();
	}
	if (argc > 3 || (argc == 3 && strcmp(mode, "lines") && strcmp(mode, "once"))) {
		fprintf(stderr, "usage: %s fabric [lines|once]\n"
			"       %s fabric route <node>... [arm <period> <count> | xy <x> <y>]\n"
			"       %s fabric reset\n", argv[0], argv[0], argv[0]);
		return 2;
	}
	if (fabric_map()) {
		return 1;
	}
	return view_main(!strcmp(mode, "lines"), !strcmp(mode, "once"));
}
