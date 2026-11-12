/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's live stream, for the web page's viewer (web/la.html):
 * a WebSocket at /la/stream on ai64 web. Each viewer gets a thread of its
 * own, with a reader of its own (la_src.c) that finds the changes in PRU0's
 * samples, and sends them as they come. The readers share one pump out of
 * DDR.
 *
 * First a text message, the channels and what can be set:
 *   {"type":"meta","channels":[{"ch":0,"pin":"P8.03","pru":0,"bit":0,
 *    "gpi":19,"name":"","from":""},...],"rates":[125000000,...],
 *    "live_bps":150000000,"ring":33554432,"slot":64,"words":15}
 * where bit is the channel's bit in the levels below (its number), and gpi
 * its PRU0 GPI bit.
 * then binary messages, little-endian, each a 16-byte header
 *   u8 kind, u8 0, u16 0, u32 count, f64 base
 * where base is IEP0's count in ns, and a body by kind:
 *   1 edges   u32 the levels before the first edge, then count edges, the
 *             first at base, each a LEB128 varint (7 bits a byte, low
 *             first, the top bit set on all but the last) of
 *               (the time since the edge before, in 4 ns steps) << 5 | ch
 *             where ch is the one channel that changed; or 31 when more
 *             than one did, followed by the levels after the edge, 3
 *             bytes; or 30 for a hole, where samples were lost, starting
 *             then: a varint of its length in 4 ns steps, and the levels
 *             at its end, 3 bytes. Between, the levels are unknown.
 *   3 lost    blocks of samples lost before base: count
 *   4 state   u32 the levels at base, the first sample: before any edge
 *   5 now     no body: how far the reader has got, so a viewer knows how
 *             far a channel with no edges has got; every 20 ms
 * and once a second a text message,
 *   {"type":"stats","edges":n,"blocks":n,"lost":n,"behind":n,"sent":bytes}
 * with totals since the viewer connected; and at the start, and whenever
 * it changes, from here or anywhere (ai64 la mode), the setup:
 *   {"type":"mode","hz":25000000,"channels":[c,...],"lane":0,"per_word":1,
 *    "bps":106666666,"span":0.3145,"live":true}
 * or {"type":"mode","error":"..."} while the analyzer does not run.
 *
 * The viewer can set the setup (la_set_mode()), the rate and channels:
 *   {"type":"mode","hz":62500000,"channels":[c,...]}
 * and gets back {"type":"mode_error","error":"..."} if it cannot be set.
 * That restarts PRU0, a gap of some 30 ms in every viewer's stream, which
 * comes as a hole.
 *
 * A viewer too slow for the stream, or a setup that writes more than the
 * pump reads (LA_LIVE_BPS), falls behind: what it skips is a hole.
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

#define BATCH 65536u /* bytes of edges a message, at most */

struct head {
	uint8_t kind;
	uint8_t zero8;
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
		uint32_t state;
		uint8_t b[BATCH];
	} out;
	size_t len;         /* bytes in out.b */
	uint8_t in[4096];   /* what the viewer sent, not yet a whole frame */
	size_t nin;
	uint64_t last_t;    /* the last edge's time and levels */
	uint32_t last_state;
	int have;           /* the levels are known: a first sample sent */
	uint64_t gap_from;  /* with a run before, where its samples end */
	uint64_t hole_to;   /* while ws_hole puts one: where it ends */
	uint64_t edges, blocks, lost, behind; /* the runs before this one */
};

static void send_msg(struct viewer *v, const void *buf, size_t len)
{
	if (!v->failed && ws_send(v->fd, 2, buf, len)) {
		v->failed = 1;
	}
	v->sent += len;
}

static void flush(struct viewer *v)
{
	if (v->out.h.count) {
		send_msg(v, &v->out, sizeof(struct head) + 4u + v->len);
		v->out.h.count = 0;
		v->len = 0;
	}
}

static size_t put_varint(uint8_t *b, size_t n, uint64_t code)
{
	while (code >= 0x80u) {
		b[n++] = (uint8_t)(code | 0x80u);
		code >>= 7;
	}
	b[n++] = (uint8_t)code;
	return n;
}

static void ws_hole(void *ctx, uint64_t from, uint64_t to, uint32_t state);

static void ws_edge(void *ctx, uint64_t t, uint32_t state)
{
	struct viewer *v = ctx;
	struct head *h = &v->out.h;
	uint8_t *b = v->out.b;
	uint32_t x, ch;
	size_t n;

	if (!v->have) {
		/* The first sample: the levels, before any edge. */
		struct {
			struct head h;
			uint32_t state;
		} m = { { .kind = 4, .base = (double)t }, state };

		send_msg(v, &m, sizeof(m));
		v->last_t = t;
		v->last_state = state;
		v->have = 1;
		return;
	}
	if (v->gap_from) {
		/* A new run's first sample: a hole from where the run before ended. */
		uint64_t from = v->gap_from;

		v->gap_from = 0;
		ws_hole(ctx, from, t, state);
		return;
	}
	/* The longest edge, or hole: two 64-bit varints, and the levels. */
	if (v->len > BATCH - 24u) {
		flush(v);
	}
	if (!h->count) {
		h->kind = 1;
		h->base = (double)t;
		v->out.state = v->last_state;
		v->last_t = t;
	}
	x = state ^ v->last_state;
	ch = v->hole_to ? 30u : x && !(x & (x - 1u)) ? (uint32_t)__builtin_ctz(x) : 31u;
	n = put_varint(b, v->len, (t > v->last_t ? (t - v->last_t) >> 2 : 0) << 5 | ch);
	if (ch == 30u) {
		n = put_varint(b, n, v->hole_to > t ? (v->hole_to - t) >> 2 : 0);
		t = v->hole_to;
		v->hole_to = 0;
	}
	if (ch >= 30u) {
		b[n++] = (uint8_t)state;
		b[n++] = (uint8_t)(state >> 8);
		b[n++] = (uint8_t)(state >> 16);
	}
	v->len = n;
	v->last_t = t;
	v->last_state = state;
	h->count++;
}

/* A hole from from to to, the levels state at its end: code 30, in line with the edges. */
static void ws_hole(void *ctx, uint64_t from, uint64_t to, uint32_t state)
{
	struct viewer *v = ctx;

	v->hole_to = to > from ? to : from + 1;
	ws_edge(ctx, from, state);
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
	size_t n = 0, i;
	int c;

	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "{\"type\":\"meta\",\"channels\":[");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		n += (size_t)snprintf(buf + n, sizeof(buf) - n,
				      "%s{\"ch\":%d,\"pin\":\"%s\",\"pru\":0,\"bit\":%d,\"gpi\":%d,\"name\":\"%s\","
				      "\"from\":\"%s\"}", c ? "," : "", c, la_chans[c].pin, c, la_chans[c].bit,
				      la_wired[c].name ? la_wired[c].name : "",
				      la_wired[c].from ? la_wired[c].from : "");
	}
	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "],\"rates\":[");
	for (i = 0; i < la_nrates; i++) {
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%u", i ? "," : "", la_rates[i].hz);
	}
	snprintf(buf + n, sizeof(buf) - n, "],\"live_bps\":%u,\"ring\":%u,\"slot\":%u,\"words\":%u}",
		 la_live_bps(), AI64_LA_RING_SIZE, AI64_LA_SLOT, AI64_LA_WORDS);
	send_text(v, buf);
}

static void send_mode(struct viewer *v, const struct la_mode *m)
{
	char buf[512];
	size_t n = 0;
	int c, k = 0;

	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "{\"type\":\"mode\",\"hz\":%u,\"channels\":[",
			      m->hz);
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (m->chans >> c & 1u) {
			n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%d", k++ ? "," : "", c);
		}
	}
	snprintf(buf + n, sizeof(buf) - n, "],\"lane\":%u,\"per_word\":%u,\"bps\":%.0f,\"span\":%.6f,"
		 "\"live\":%s}", m->lane, AI64_LA_PER_WORD(m->lane), m->bps, m->span,
		 m->live ? "true" : "false");
	send_text(v, buf);
}

static void send_error(struct viewer *v, const char *type, const char *err)
{
	char reply[320];

	snprintf(reply, sizeof(reply), "{\"type\":\"%s\",\"error\":\"%s\"}", type, err);
	send_text(v, reply);
}

/* One viewer at a time sets the setup. */
static pthread_mutex_t mode_lock = PTHREAD_MUTEX_INITIALIZER;

/* {"type":"mode","hz":h,"channels":[...]}: set it. */
static void set_mode(struct viewer *v, const char *msg)
{
	const char *p = strstr(msg, "\"channels\""), *hz = strstr(msg, "\"hz\":");
	uint32_t chans = 0;
	char err[160];

	if (!p || !(p = strchr(p, '[')) || !hz) {
		send_error(v, "mode_error", "a mode message needs hz and channels");
		return;
	}
	for (p++; *p && *p != ']'; p++) {
		if (*p >= '0' && *p <= '9') {
			long c = strtol(p, (char **)&p, 10);

			if (c >= (long)AI64_LA_NCHAN) {
				send_error(v, "mode_error", "no such channel");
				return;
			}
			chans |= 1u << c;
			p--;
		}
	}
	if (!chans) {
		send_error(v, "mode_error", "at least one channel");
		return;
	}
	pthread_mutex_lock(&mode_lock);
	if (la_set_mode(chans, (uint32_t)strtoul(hz + 5, NULL, 10), err, sizeof(err))) {
		send_error(v, "mode_error", err);
	}
	pthread_mutex_unlock(&mode_lock);
}

static void read_viewer(struct viewer *v)
{
	ssize_t got = recv(v->fd, v->in + v->nin, sizeof(v->in) - 1 - v->nin, MSG_DONTWAIT);

	if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
		v->failed = 1;
		return;
	}
	if (got > 0) {
		v->nin += (size_t)got;
	}
	while (v->nin >= 6) {
		uint8_t op = v->in[0] & 0x0fu;
		size_t len = v->in[1] & 0x7fu, head = 6, i;
		uint8_t *key, *data;

		if (len == 127 || !(v->in[1] & 0x80u)) {
			v->failed = 1;
			return;
		}
		if (len == 126) {
			if (v->nin < 8) {
				return;
			}
			len = (size_t)v->in[2] << 8 | v->in[3];
			head = 8;
		}
		if (head + len > sizeof(v->in) - 1) {
			v->failed = 1;
			return;
		}
		if (v->nin < head + len) {
			return;
		}
		key = v->in + head - 4;
		data = v->in + head;
		for (i = 0; i < len; i++) {
			data[i] ^= key[i & 3u];
		}
		if (op == 8) {
			v->failed = 1;
			return;
		}
		if (op == 1) {
			char save = (char)data[len];

			data[len] = '\0';
			if (strstr((char *)data, "\"type\":\"mode\"")) {
				set_mode(v, (char *)data);
			}
			data[len] = (uint8_t)save;
		}
		memmove(v->in, v->in + head + len, v->nin - head - len);
		v->nin -= head + len;
	}
}

static void send_stats(struct viewer *v, const struct la_src *src)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "{\"type\":\"stats\",\"edges\":%llu,\"blocks\":%llu,\"lost\":%llu,"
		 "\"behind\":%llu,\"sent\":%llu}",
		 (unsigned long long)(v->edges + (src->open ? src->edges : 0)),
		 (unsigned long long)(v->blocks + (src->open ? src->blocks : 0)),
		 (unsigned long long)(v->lost + (src->open ? src->lost : 0)),
		 (unsigned long long)(v->behind + (src->open ? src->behind : 0)),
		 (unsigned long long)v->sent);
	send_text(v, buf);
}

static void *viewer_main(void *arg)
{
	struct viewer *v = arg;
	struct la_src src = { 0 };
	uint64_t lost_told = 0;
	double flush_at, stats_at, retry_at = 0;
	char err[160];

	send_meta(v);
	flush_at = now_us() + 20000;
	stats_at = now_us() + 1e6;
	while (!v->failed) {
		uint64_t at = src.next;
		double now = now_us();

		/* A new run (a new setup, from anywhere): follow it, the gap a hole. */
		if (src.open && la_src_stale(&src)) {
			flush(v);
			v->edges += src.edges;
			v->blocks += src.blocks;
			v->lost += src.lost;
			v->behind += src.behind;
			if (src.have) {
				v->gap_from = src.last + src.mode.sample_ns;
			}
			la_src_close(&src);
		}
		if (!src.open && now >= retry_at) {
			if (la_src_open(&src, 0, err, sizeof(err))) {
				send_error(v, "mode", err);
				retry_at = now + 1e6;
			} else {
				send_mode(v, &src.mode);
				lost_told = 0;
				at = src.next;
			}
		}
		if (src.open) {
			la_src_poll(&src, ws_edge, ws_hole, v, 20000000u);
			if (src.lost + src.behind != lost_told) {
				struct head h = { .kind = 3, .count = (uint32_t)(src.lost + src.behind - lost_told),
						  .base = (double)src.now };

				flush(v);
				send_msg(v, &h, sizeof(h));
				lost_told = src.lost + src.behind;
			}
		}
		read_viewer(v);
		if (now >= flush_at) {
			flush(v);
			if (src.open && v->have) {
				struct head h = { .kind = 5, .base = (double)src.now };

				send_msg(v, &h, sizeof(h));
			}
			flush_at = now + 20000;
		}
		if (now >= stats_at) {
			send_stats(v, &src);
			stats_at = now + 1e6;
		}
		if (!src.open || src.next == at) {
			usleep(2000);
		}
	}
	la_src_close(&src);
	close(v->fd);
	free(v);
	return NULL;
}

/* ---- the upgrade ---- */

/*
 * The request req, on fd, asked for /la/stream. If it is a WebSocket
 * upgrade, answers it and hands fd to a thread of its own: 1, and the
 * caller must not close fd. Otherwise 0, and the caller answers it.
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

	if (!k || !strcasestr(req, "\nUpgrade: websocket")) {
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
