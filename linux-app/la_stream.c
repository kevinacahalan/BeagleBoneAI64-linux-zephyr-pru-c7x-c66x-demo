/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's reader (la.h): follows one PRU's stream in DDR
 * (ai64_la.h), unwraps each edge's time against the stream's last tick,
 * and hands the edges, marks, and trigger hits on. ai64 la (la.c) and the
 * web page's stream (la_ws.c) both use it; each reader keeps its own copy
 * buffer, so readers can run in threads side by side, and none writes.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

#define LA_DT "/proc/device-tree/reserved-memory/ai64-la@ac000000"

const struct la_chan la_chans[AI64_LA_NCHAN] = {
#define X(c, pin, ball, pad, pru, bit) [c] = { pin, ball, pad, pru, bit },
	AI64_LA_CHANNELS(X)
#undef X
};

/* What the demo's jumpers bring to each channel (pru/logic_analyzer/README.md). */
const struct la_wired la_wired[AI64_LA_NCHAN] = {
	[3] = { "SPI7_CS", "P9.28" },
	[4] = { "SPI7_CLK", "P9.31" },
	[5] = { "SPI7_MOSI", "P9.30" },
	[6] = { "SQUARE", "P8.18" },
	[7] = { "S0_A", "P8.11" },
	[8] = { "S0_B", "P8.12" },
	[9] = { "S1_A", "P9.12" },
	[10] = { "S1_B", "P9.15" },
	[11] = { "ECAP", "P8.15" },
	[12] = { "EHRPWM4", "P9.25" },
	[13] = { "S0_A", "P8.21" },
};

volatile struct ai64_la *la_hdr;
static const volatile uint32_t *rings;

/* Read only: only ai64 la trigger writes, to the headers (la_map_trigger). */
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
	return p == MAP_FAILED ? NULL : p;
}

int la_map(int verbose)
{
	int s;

	if (la_hdr && rings) {
		goto check;
	}
	if (access(LA_DT, F_OK)) {
		if (verbose)
			fprintf(stderr, "%s is missing: the overlay is older than the logic analyzer.\n"
			"Run install-beagle.sh and reboot.\n", LA_DT);
		return -1;
	}
	la_hdr = map_ro(AI64_LA_ADDR, 4096);
	rings = map_ro(AI64_LA_RES_ADDR, AI64_LA_RES_SIZE);
	if (!la_hdr || !rings) {
		return -1;
	}
check:
	for (s = 0; s < 2; s++) {
		if (la_hdr->s[s].magic != AI64_LA_MAGIC) {
			if (verbose)
				fprintf(stderr, "ICSSG1 RTU%d is not running the logic analyzer "
				"(ai64-demo --icssg1 la starts it)\n", s);
			return -1;
		}
	}
	return 0;
}

int stream_init(struct stream *st, int s)
{
	memset(st, 0, sizeof(*st));
	st->chunk = aligned_alloc(64, CHUNK);
	if (!st->chunk) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	st->s = s;
	st->h = &la_hdr->s[s];
	st->ring = rings + (AI64_LA_RING_ADDR(s) - AI64_LA_RES_ADDR) / 4u;
	st->rd = st->h->wr;
	st->wr0 = st->rd;
	return 0;
}

void stream_free(struct stream *st)
{
	free(st->chunk);
	free(st->mark);
	st->chunk = NULL;
	st->mark = NULL;
}

static void keep_mark(struct stream *st, uint64_t t, uint32_t trig, uint32_t v)
{
	if (st->nmark == st->capmark) {
		st->capmark = st->capmark ? st->capmark * 2 : 64;
		st->mark = realloc(st->mark, st->capmark * sizeof(*st->mark));
		if (!st->mark) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	st->mark[st->nmark].t = t;
	st->mark[st->nmark].trig = trig;
	st->mark[st->nmark].from = trig ? 0 : (v >> 16) & 0xffu;
	st->mark[st->nmark].value = trig ? v & 0xffffffu & ~AI64_LA_LATE : v & 0xffffu;
	st->mark[st->nmark++].fresh = (int32_t)(st->rd - st->wr0) >= 0;
}

/*
 * The part of a ring being decoded, copied out of DDR 64 bytes at a time.
 * Linux maps the rings uncached, and an uncached load costs about the same,
 * some 190 ns, whatever its size: 64-byte loads read about 266 MB/s, a word
 * at a time 21 MB/s, too slow for both PRUs at full rate.
 */
static void copy_blocks(const struct stream *st, uint32_t from, uint32_t to)
{
	uint32_t at;

	for (at = from; at != to; at += 64u) {
		const volatile uint32_t *src = st->ring + (at & (AI64_LA_RING_SIZE - 1u)) / 4u;
		uint32_t *dst = st->chunk + (at - from) / 4u;
#ifdef __aarch64__
		__asm__ volatile("ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n\t"
				 "st1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%1]"
				 : : "r"(src), "r"(dst) : "v0", "v1", "v2", "v3", "memory");
#else
		for (int i = 0; i < 16; i++) {
			dst[i] = src[i];
		}
#endif
	}
}

/*
 * A hole opens at from: the pins are unknown from there to the next record,
 * or, with raw 2, raw mode's converter's, the next record it writes.
 */
static void open_hole(struct stream *st, uint64_t from, int raw)
{
	if (!st->hole && st->last) {
		st->hole = raw ? 2 : 1;
		st->hole_from = from > st->last ? from : st->last;
	}
}

/* The open hole ends at t, the pins there state: to on_hole, or as an edge. */
static void close_hole(struct stream *st, edge_fn fn, void *ctx, uint64_t t, uint32_t state, int edge)
{
	st->hole = 0;
	if (st->on_hole) {
		st->on_hole(ctx, st, st->hole_from, t, state);
	} else if (edge) {
		fn(ctx, st, t, state);
	}
}

void la_read_res(void *dst, uint32_t addr, size_t len)
{
	size_t at;

	for (at = 0; at < len; at += 64u) {
		const volatile uint32_t *src = rings + (addr - AI64_LA_RES_ADDR + at) / 4u;
		uint32_t *d = (uint32_t *)dst + at / 4u;
#ifdef __aarch64__
		__asm__ volatile("ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n\t"
				 "st1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%1]"
				 : : "r"(src), "r"(d) : "v0", "v1", "v2", "v3", "memory");
#else
		for (int i = 0; i < 16; i++) {
			d[i] = src[i];
		}
#endif
	}
}

/*
 * Every record the RTU has written since the last call. The RTU updates wr
 * after a tick's second record, so a tick never shows up half written.
 *
 * A hole, where records were lost, runs from the last edge before it to
 * the next record after, which holds every pin: an RTU's gap record, or
 * this reader falling a ring behind; a tick ends one too, with the pins the
 * RTU copied last. Raw mode's converter says where its hole starts, and
 * writes the pins where it ends; its records come a few microseconds
 * behind the RTU's ticks, so a tick neither ends its hole nor sets last.
 */
void stream_poll(struct stream *st, edge_fn fn, void *ctx)
{
	uint32_t wr = st->h->wr;

	if (wr - st->rd > AI64_LA_RING_SIZE - 65536u) {
		st->missed += (wr - st->rd) / AI64_LA_REC_SIZE;
		st->rd = wr;
		if (st->ticked) {
			open_hole(st, st->last, 0);
		}
		st->ticked = 0;
	}
	while (wr - st->rd >= AI64_LA_REC_SIZE) {
		uint32_t from = st->rd & ~63u, to = (wr + 63u) & ~63u, end, was = st->rd;

		if (to - from > CHUNK) {
			to = from + CHUNK;
		}
		end = wr - from < to - from ? wr : to;
		copy_blocks(st, from, to);
		while (end - st->rd >= AI64_LA_REC_SIZE) {
			uint32_t t = st->chunk[(st->rd - from) / 4u], v = st->chunk[(st->rd - from) / 4u + 1u];
			uint64_t at = st->base + (uint64_t)(int64_t)(int32_t)(t - (uint32_t)st->base);

			switch (AI64_LA_TYPE(v)) {
			case AI64_LA_TICK: {
				uint32_t hi;

				if (end - st->rd < 2u * AI64_LA_REC_SIZE) {
					goto next; /* its second record is past this chunk */
				}
				hi = st->chunk[(st->rd - from) / 4u + 3u];
				st->base = (uint64_t)(hi & 0xffffu) << 32 | t;
				if (!st->ticked) {
					st->ticked = 1;
					st->state = v & 0xffffffu;
					if (!st->first) {
						st->first = st->base;
						st->first_state = st->state;
					}
				}
				if (st->hole == 1) {
					st->state = v & 0xffffffu;
					close_hole(st, fn, ctx, st->base > st->hole_from ? st->base : st->hole_from,
						   st->state, 0);
				}
				st->rd += 2u * AI64_LA_REC_SIZE;
				continue;
			}
			case AI64_LA_GAP:
				st->gaps++;
				open_hole(st, st->last, 0);
				break;
			case AI64_LA_HOLE:
				st->gaps++;
				if (st->ticked) {
					open_hole(st, at - (uint64_t)(v & 0xffffffu) * 8u, 1);
				}
				break;
			case AI64_LA_MARK:
			case AI64_LA_TRIG:
				if (AI64_LA_TYPE(v) == AI64_LA_TRIG) {
					st->trigs++;
				} else {
					st->marks++;
				}
				if (st->keep_marks && st->ticked) {
					keep_mark(st, at, AI64_LA_TYPE(v) == AI64_LA_TRIG, v);
				}
				break;
			default:
				if (st->ticked) {
					if (v & AI64_LA_LATE) {
						st->late++;
						v &= ~AI64_LA_LATE;
					}
					st->state = v;
					st->edges++;
					if (st->hole) {
						close_hole(st, fn, ctx, at, v, 1);
					} else {
						fn(ctx, st, at, v);
					}
					st->last = at;
				}
				break;
			}
			st->rd += AI64_LA_REC_SIZE;
		}
next:
		if (st->rd == was) {
			break;
		}
	}
}

/* Until both streams have ticked; -1 if one does not within 2 s. */
int wait_ticks(struct stream st[2], edge_fn fn, void *ctx)
{
	double give_up = now_us() + 2e6;
	int s;

	while (!st[0].ticked || !st[1].ticked) {
		for (s = 0; s < 2; s++) {
			stream_poll(&st[s], fn, ctx);
		}
		if (now_us() > give_up) {
			fprintf(stderr, "ICSSG1 RTU%d's stream has no tick: is its RTU running?\n",
				st[0].ticked ? 1 : 0);
			return -1;
		}
		usleep(2000);
	}
	return 0;
}

volatile struct ai64_la *la_map_trigger(void)
{
	if (la_map(1)) {
		return NULL;
	}
	return map_phys(AI64_LA_ADDR, 4096);
}
