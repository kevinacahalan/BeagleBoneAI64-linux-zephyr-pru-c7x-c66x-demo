/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's live stream, for the web page's viewer (web/la.html):
 * a WebSocket at /la/stream on ai64 web. Each viewer gets a thread of its
 * own, which follows both PRUs' streams in DDR with a reader of its own
 * (la_stream.c) and sends what they hold as it comes. Like the rest of
 * ai64 web it only reads: the trigger and the marks are ai64 la's.
 *
 * First a text message, the channels:
 *   {"type":"meta","channels":[{"ch":0,"pin":"P8.03","pru":0,"bit":19,
 *    "name":"","from":""},...]}
 * then binary messages, little-endian, each a 16-byte header
 *   u8 kind, u8 stream (PRU), u16 0, u32 count, f64 base
 * where base is IEP0's count in ns, and a body by kind:
 *   1 edges   count x { u32 t - base, u32 the PRU's pins after the edge }
 *   2 events  count x { u32 t - base, u8 1 for a trigger hit or 0 for a
 *             mark, u8 the mark's fabric node, u16 0, u32 its value, or
 *             the hit's pins }
 *   3 gap     records lost before base: count, the RTU's gap records and
 *             this reader's falling a ring behind, together
 *   4 state   u32 the PRU's pins at base, its first tick: before any edge
 *   5 now     no body: the stream's newest tick, so a viewer knows how far
 *             a channel with no edges has got; every 20 ms
 * and once a second a text message,
 *   {"type":"stats","edges":[n0,n1],"gaps":[g0,g1],"missed":[m0,m1],"sent":bytes}
 * with totals since the viewer connected.
 *
 * A viewer too slow for the stream falls behind in the 32 MiB ring, and
 * then a ring behind: it gets a gap message, and carries on from the
 * newest records.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

/* ---- SHA-1 and base64, for the handshake (RFC 6455, section 4.2.2) ---- */

static uint32_t rol(uint32_t v, int n)
{
	return v << n | v >> (32 - n);
}

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
	uint32_t h[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u };
	uint8_t block[64];
	size_t done = 0, total = ((len + 8) / 64 + 1) * 64;
	int i;

	while (done < total) {
		uint32_t w[80], a, b, c, d, e;

		for (i = 0; i < 64; i++) {
			size_t at = done + (size_t)i;

			if (at < len) {
				block[i] = msg[at];
			} else if (at == len) {
				block[i] = 0x80;
			} else if (at >= total - 8) {
				block[i] = (uint8_t)((uint64_t)len * 8u >> (8 * (total - 1 - at)));
			} else {
				block[i] = 0;
			}
		}
		for (i = 0; i < 16; i++) {
			w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
			       (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
		}
		for (i = 16; i < 80; i++) {
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (i = 0; i < 80; i++) {
			uint32_t f, k, t;

			if (i < 20) {
				f = (b & c) | (~b & d), k = 0x5a827999u;
			} else if (i < 40) {
				f = b ^ c ^ d, k = 0x6ed9eba1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdcu;
			} else {
				f = b ^ c ^ d, k = 0xca62c1d6u;
			}
			t = rol(a, 5) + f + e + k + w[i];
			e = d, d = c, c = rol(b, 30), b = a, a = t;
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
		done += 64;
	}
	for (i = 0; i < 20; i++) {
		out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
	}
}

static void base64(const uint8_t *in, size_t len, char *out)
{
	static const char abc[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;

	for (i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
			     (i + 2 < len ? in[i + 2] : 0);

		*out++ = abc[v >> 18 & 63];
		*out++ = abc[v >> 12 & 63];
		*out++ = i + 1 < len ? abc[v >> 6 & 63] : '=';
		*out++ = i + 2 < len ? abc[v & 63] : '=';
	}
	*out = '\0';
}

/* ---- sending ---- */

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* One unmasked WebSocket message: opcode 1 text, 2 binary. */
static int ws_send(int fd, int opcode, const void *buf, size_t len)
{
	uint8_t head[10];
	size_t n = 2;
	int i;

	head[0] = (uint8_t)(0x80 | opcode);
	if (len < 126) {
		head[1] = (uint8_t)len;
	} else if (len < 65536) {
		head[1] = 126;
		head[2] = (uint8_t)(len >> 8);
		head[3] = (uint8_t)len;
		n = 4;
	} else {
		head[1] = 127;
		for (i = 0; i < 8; i++) {
			head[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
		}
		n = 10;
	}
	if (send_all(fd, head, n)) {
		return -1;
	}
	return send_all(fd, buf, len);
}

/* ---- the viewer's thread ---- */

#define BATCH 8192u /* edges a message, at most */

struct head {
	uint8_t kind;
	uint8_t stream;
	uint16_t zero;
	uint32_t count;
	double base;
};

struct viewer {
	int fd;
	int failed;
	uint64_t sent;
	struct {
		struct head h;
		uint32_t rec[BATCH][2];
	} out[2];
};

static void send_msg(struct viewer *v, const void *buf, size_t len)
{
	if (!v->failed && ws_send(v->fd, 2, buf, len)) {
		v->failed = 1;
	}
	v->sent += len;
}

static void flush(struct viewer *v, int s)
{
	if (v->out[s].h.count) {
		send_msg(v, &v->out[s], sizeof(struct head) + 8u * v->out[s].h.count);
		v->out[s].h.count = 0;
	}
}

static void ws_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
{
	struct viewer *v = ctx;
	struct head *h = &v->out[st->s].h;

	/* A batch's times are 32-bit offsets from its base: about 4 s. */
	if (h->count == BATCH || (h->count && t - (uint64_t)h->base > 0xf0000000u)) {
		flush(v, st->s);
	}
	if (!h->count) {
		h->kind = 1;
		h->stream = (uint8_t)st->s;
		h->base = (double)t;
	}
	v->out[st->s].rec[h->count][0] = (uint32_t)(t - (uint64_t)h->base);
	v->out[st->s].rec[h->count][1] = state;
	h->count++;
}

static void send_events(struct viewer *v, struct stream *st)
{
	struct {
		struct head h;
		uint32_t e[64][3];
	} m;
	size_t i = 0;

	while (i < st->nmark) {
		m.h = (struct head){ .kind = 2, .stream = (uint8_t)st->s, .base = (double)st->mark[i].t };
		for (; i < st->nmark && m.h.count < 64; i++) {
			struct mark *k = &st->mark[i];

			m.e[m.h.count][0] = (uint32_t)(k->t - (uint64_t)m.h.base);
			m.e[m.h.count][1] = k->trig | k->from << 8;
			m.e[m.h.count][2] = k->value;
			m.h.count++;
		}
		send_msg(v, &m, sizeof(struct head) + 12u * m.h.count);
	}
	st->nmark = 0;
}

static void send_text(struct viewer *v, const char *text)
{
	if (!v->failed && ws_send(v->fd, 1, text, strlen(text))) {
		v->failed = 1;
	}
}

static void send_meta(struct viewer *v)
{
	char buf[4096];
	size_t n = 0;
	int c;

	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "{\"type\":\"meta\",\"channels\":[");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		n += (size_t)snprintf(buf + n, sizeof(buf) - n,
				      "%s{\"ch\":%d,\"pin\":\"%s\",\"pru\":%d,\"bit\":%d,"
				      "\"name\":\"%s\",\"from\":\"%s\"}", c ? "," : "", c,
				      la_chans[c].pin, la_chans[c].pru, la_chans[c].bit,
				      la_wired[c].name ? la_wired[c].name : "",
				      la_wired[c].from ? la_wired[c].from : "");
	}
	snprintf(buf + n, sizeof(buf) - n, "]}");
	send_text(v, buf);
}

static void *viewer_main(void *arg)
{
	struct viewer *v = arg;
	struct stream st[2];
	uint64_t gaps_told[2] = { 0, 0 };
	double flush_at, stats_at;
	int s;

	memset(st, 0, sizeof(st));
	if (stream_init(&st[0], 0) || stream_init(&st[1], 1)) {
		goto out;
	}
	st[0].keep_marks = st[1].keep_marks = 1;
	send_meta(v);
	if (wait_ticks(st, ws_edge, v)) {
		goto out;
	}
	/* Each PRU's pins at its first tick, before its first edge. */
	for (s = 0; s < 2; s++) {
		struct {
			struct head h;
			uint32_t state;
		} m = { { .kind = 4, .stream = (uint8_t)s, .base = (double)st[s].first },
			st[s].first_state };

		send_msg(v, &m, sizeof(m));
	}
	flush_at = now_us() + 20000;
	stats_at = now_us() + 1e6;
	while (!v->failed) {
		uint32_t before[2] = { st[0].rd, st[1].rd };
		double now;

		for (s = 0; s < 2; s++) {
			uint64_t lost;

			stream_poll(&st[s], ws_edge, v);
			lost = st[s].gaps + st[s].missed;
			if (lost != gaps_told[s]) {
				struct head h = { .kind = 3, .stream = (uint8_t)s,
						  .count = (uint32_t)(lost - gaps_told[s]),
						  .base = (double)st[s].base };

				flush(v, s);
				send_msg(v, &h, sizeof(h));
				gaps_told[s] = lost;
			}
			if (st[s].nmark) {
				flush(v, s);
				send_events(v, &st[s]);
			}
		}
		now = now_us();
		if (now >= flush_at) {
			for (s = 0; s < 2; s++) {
				struct head h = { .kind = 5, .stream = (uint8_t)s, .base = (double)st[s].base };

				flush(v, s);
				send_msg(v, &h, sizeof(h));
			}
			flush_at = now + 20000;
		}
		if (now >= stats_at) {
			char buf[256];

			snprintf(buf, sizeof(buf),
				 "{\"type\":\"stats\",\"edges\":[%llu,%llu],\"gaps\":[%llu,%llu],"
				 "\"missed\":[%llu,%llu],\"sent\":%llu}",
				 (unsigned long long)st[0].edges, (unsigned long long)st[1].edges,
				 (unsigned long long)st[0].gaps, (unsigned long long)st[1].gaps,
				 (unsigned long long)st[0].missed, (unsigned long long)st[1].missed,
				 (unsigned long long)v->sent);
			send_text(v, buf);
			stats_at = now + 1e6;
		}
		if (st[0].rd == before[0] && st[1].rd == before[1]) {
			usleep(2000);
		}
	}
out:
	stream_free(&st[0]);
	stream_free(&st[1]);
	close(v->fd);
	free(v);
	return NULL;
}

/* ---- the upgrade ---- */

/*
 * The request req, on fd, asked for /la/stream. If it is a WebSocket
 * upgrade and the analyzer runs, answers it and hands fd to a thread of
 * its own: 1, and the caller must not close fd. Otherwise 0, and the
 * caller answers it.
 */
int la_ws_upgrade(int fd, const char *req)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	struct timeval tv = { .tv_sec = 30 };
	char key[128], accept[64], resp[256];
	uint8_t digest[20];
	const char *k = strcasestr(req, "\nSec-WebSocket-Key:");
	struct viewer *v;
	pthread_t t;
	size_t n = 0;

	if (!k || !strcasestr(req, "\nUpgrade: websocket") || la_map(0)) {
		return 0;
	}
	for (k += 19; *k == ' '; k++) {
	}
	while (k[n] && k[n] != '\r' && k[n] != '\n' && n < 60) {
		n++;
	}
	memcpy(key, k, n);
	memcpy(key + n, guid, sizeof(guid));
	sha1((const uint8_t *)key, n + sizeof(guid) - 1, digest);
	base64(digest, sizeof(digest), accept);
	snprintf(resp, sizeof(resp),
		 "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
	if (send_all(fd, resp, strlen(resp))) {
		return 0;
	}
	v = calloc(1, sizeof(*v));
	if (!v) {
		return 0;
	}
	v->fd = fd;
	/* A viewer that stops reading for 30 s is gone. */
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (pthread_create(&t, NULL, viewer_main, v)) {
		free(v);
		return 0;
	}
	pthread_detach(t);
	return 1;
}
