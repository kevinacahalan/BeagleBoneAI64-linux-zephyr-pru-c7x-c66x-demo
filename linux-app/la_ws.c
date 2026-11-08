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
 *   1 edges   u32 the PRU's pins before the first edge, then count edges,
 *             the first at base, each a LEB128 varint (7 bits a byte, low
 *             first, the top bit set on all but the last) of
 *               (the time since the edge before, in IEP0's 4 ns steps) << 5 | pin
 *             where pin is the one GPI bit that changed; or 31 when more
 *             than one did, followed by the pins after the edge, 3 bytes;
 *             or 30 for a hole, where records were lost, starting then:
 *             a varint of its length in 4 ns steps, and the pins at its
 *             end, 3 bytes. Between, the PRU's pins are unknown.
 *             An edge a few hundred ns after the last takes 2 bytes, one a
 *             few hundred us after 3: a quarter of a time and state each.
 *   2 events  count x { u32 t - base, u8 1 for a trigger hit or 0 for a
 *             mark, u8 the mark's fabric node, u16 0, u32 its value, or
 *             the hit's pins }
 *   3 gap     records lost before base: count, the RTU's gap records and
 *             this reader's falling a ring behind, together
 *   4 state   u32 the PRU's pins at base, its first tick: before any edge
 *   5 now     no body: the stream's newest tick, so a viewer knows how far
 *             a channel with no edges has got; every 20 ms
 * and once a second a text message,
 *   {"type":"stats","edges":[n0,n1],"gaps":[g0,g1],"missed":[m0,m1],
 *    "late":[l0,l1],"sent":bytes}
 * with totals since the viewer connected: late counts edge mode's
 * changes that came while the PRU was busy (AI64_LA_LATE); and at the start, and whenever
 * it changes, from here or anywhere (ai64 la mode), the analyzer's mode:
 *   {"type":"mode","prus":[{"mode":"edge","lane":0,"mask":m0},
 *    {"mode":"raw","lane":1,"mask":m1}],"channels":[c,...]}
 * each PRU's mode (edge, raw, or conv), raw mode's lane, and the pins it
 * records, and the channels recorded.
 *
 * The viewer sends two kinds of text message. One sets the mode
 * (la_set_mode()), with the channels to record:
 *   {"type":"mode","mode":"edge"|"raw"|"dump","channels":[c,...]}
 * and gets back {"type":"mode_error","error":"..."} if it cannot be set.
 * In dump mode every viewer's thread reads the raw PRU's channels from the
 * burst buffer, every sample, as the copier fills it (la_burst.c), and
 * sends them as edges and holes like any; how fast it keeps up is the
 * limit then, and falling behind is a hole.
 * That restarts both PRUs, a gap of some 50 ms in every viewer's stream.
 * The other takes a burst (la_burst.c) of channels of one PRU, seconds
 * long, 0 for as long as the buffer holds:
 *   {"type":"burst","channels":[c,...],"seconds":s}
 * which comes in this viewer's stream as edges and holes like any, the
 * other PRU a hole, followed by
 *   {"type":"burst","from":t,"to":t,"pru":p,"blocks":n,"lost":n,"edges":n,
 *    "channels":[c,...]}
 * or {"type":"burst_error","error":"..."}. Every viewer's stream has a gap
 * where it was: the PRUs restart into dump mode and back.
 *
 * A viewer too slow for the stream falls behind in the 16 MiB ring, and
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

#define BATCH 65536u /* bytes of edges a message, at most */

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
		uint32_t state;
		uint8_t b[BATCH];
	} out[2];
	size_t len[2];      /* bytes in out[s].b */
	uint8_t in[4096];   /* what the viewer sent, not yet a whole frame */
	size_t nin;
	uint32_t mode_told[2], mask_told[2];
	uint64_t last_t[2]; /* the last edge's time and pins, each stream */
	uint32_t last_state[2];
	int have[2];
	uint64_t hole_to;   /* while ws_hole puts one: where it ends */
	char burst[512];    /* a burst message, for the main loop to run */
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
		send_msg(v, &v->out[s], sizeof(struct head) + 4u + v->len[s]);
		v->out[s].h.count = 0;
		v->len[s] = 0;
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

static void ws_edge(void *ctx, struct stream *st, uint64_t t, uint32_t state)
{
	struct viewer *v = ctx;
	int s = st->s;
	struct head *h = &v->out[s].h;
	uint8_t *b = v->out[s].b;
	uint32_t x, pin;
	size_t n;

	/* The longest edge, or hole: two 64-bit varints, and the pins. */
	if (v->len[s] > BATCH - 24u) {
		flush(v, s);
	}
	if (!v->have[s]) {
		v->last_t[s] = t;
		v->last_state[s] = st->first_state;
		v->have[s] = 1;
	}
	if (!h->count) {
		h->kind = 1;
		h->stream = (uint8_t)s;
		h->base = (double)t;
		v->out[s].state = v->last_state[s];
		v->last_t[s] = t;
	}
	x = state ^ v->last_state[s];
	pin = v->hole_to ? 30u : x && !(x & (x - 1u)) ? (uint32_t)__builtin_ctz(x) : 31u;
	n = put_varint(b, v->len[s], (t > v->last_t[s] ? (t - v->last_t[s]) >> 2 : 0) << 5 | pin);
	if (pin == 30u) {
		n = put_varint(b, n, v->hole_to > t ? (v->hole_to - t) >> 2 : 0);
		t = v->hole_to;
		v->hole_to = 0;
	}
	if (pin >= 30u) {
		b[n++] = (uint8_t)state;
		b[n++] = (uint8_t)(state >> 8);
		b[n++] = (uint8_t)(state >> 16);
	}
	v->len[s] = n;
	v->last_t[s] = t;
	v->last_state[s] = state;
	h->count++;
}

/* A hole from from to to, the pins state at its end: code 30, in line with the edges. */
static void ws_hole(void *ctx, struct stream *st, uint64_t from, uint64_t to, uint32_t state)
{
	struct viewer *v = ctx;

	v->hole_to = to > from ? to : from + 1;
	ws_edge(ctx, st, from, state);
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

/* Each PRU's mode, as its RTU has it, if it differs from what v was last told. */
static void send_modes(struct viewer *v, int force)
{
	static const char *const names[] = { "edge", "raw", "conv", "dump" };
	char buf[512];
	size_t n = 0;
	int s, c, changed = force;

	for (s = 0; s < 2; s++) {
		changed |= la_hdr->s[s].cfg_mode != v->mode_told[s] ||
			   la_hdr->s[s].cfg_mask != v->mask_told[s];
	}
	if (!changed) {
		return;
	}
	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "{\"type\":\"mode\",\"prus\":[");
	for (s = 0; s < 2; s++) {
		uint32_t m = la_hdr->s[s].cfg_mode;

		v->mode_told[s] = m;
		v->mask_told[s] = la_hdr->s[s].cfg_mask;
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s{\"mode\":\"%s\",\"lane\":%u,\"mask\":%u}",
				      s ? "," : "", names[AI64_LA_MODE_OF(m) < 4 ? AI64_LA_MODE_OF(m) : 0],
				      AI64_LA_LANE_OF(m), v->mask_told[s]);
	}
	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "],\"channels\":[");
	for (c = 0, s = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (v->mask_told[la_chans[c].pru] >> la_chans[c].bit & 1u) {
			n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%d", s++ ? "," : "", c);
		}
	}
	snprintf(buf + n, sizeof(buf) - n, "]}");
	send_text(v, buf);
}

/* One viewer at a time sets the mode, or takes a burst. */
static pthread_mutex_t mode_lock = PTHREAD_MUTEX_INITIALIZER;

/* A message's "channels":[...], as each PRU's pins; NULL, or what is wrong. */
static const char *parse_channels(const char *msg, uint32_t mask[2])
{
	const char *p = strstr(msg, "\"channels\"");

	mask[0] = mask[1] = 0;
	if (!p || !(p = strchr(p, '['))) {
		return "no channels";
	}
	for (p++; *p && *p != ']'; p++) {
		if (*p >= '0' && *p <= '9') {
			long c = strtol(p, (char **)&p, 10);

			if (c >= (long)AI64_LA_NCHAN) {
				return "no such channel";
			}
			mask[la_chans[c].pru] |= 1u << la_chans[c].bit;
			p--;
		}
	}
	return NULL;
}

static void send_error(struct viewer *v, const char *type, const char *err)
{
	char reply[256];

	snprintf(reply, sizeof(reply), "{\"type\":\"%s\",\"error\":\"%s\"}", type, err);
	send_text(v, reply);
}

/* {"type":"mode","mode":"edge"|"raw","channels":[...]}: set it. */
static void set_mode(struct viewer *v, const char *msg)
{
	uint32_t mask[2];
	const char *m = strstr(msg, "\"mode\":\""), *bad = parse_channels(msg, mask);
	char err[160];
	int raw = -1, dump = 0;

	if (bad || !m) {
		send_error(v, "mode_error", bad ? bad : "a mode message needs mode and channels");
		return;
	}
	if (!strncmp(m + 8, "raw\"", 4) || !strncmp(m + 8, "dump\"", 5)) {
		if (!mask[0] == !mask[1]) {
			send_error(v, "mode_error", mask[0] ? "raw mode takes the channels of one PRU" :
				   "raw mode needs at least one channel");
			return;
		}
		raw = mask[0] ? 0 : 1;
		dump = m[8] == 'd';
	} else if (!mask[0] && !mask[1]) {
		send_error(v, "mode_error", "edge mode needs at least one channel");
		return;
	}
	pthread_mutex_lock(&mode_lock);
	if (la_set_mode_ex(raw, mask, dump, err, sizeof(err))) {
		send_error(v, "mode_error", err);
	}
	pthread_mutex_unlock(&mode_lock);
}

/*
 * {"type":"burst","channels":[...],"seconds":s}: a burst of those channels
 * (la_burst.c), s long or as long as the buffer holds, into this viewer's
 * stream in time order: what came before it, then the burst, the other
 * PRU a hole, then a text {"type":"burst","from":t,"to":t,...}; then the
 * mode as it was, and the stream from there.
 */
static void run_burst(struct viewer *v, struct stream st[2], const char *msg)
{
	static struct recording r;
	struct burst_info bi;
	uint32_t mask[2], was_mask[2];
	const char *bad = parse_channels(msg, mask), *sec = strstr(msg, "\"seconds\":");
	char err[160], back[160], reply[400];
	size_t j = 0, k, n;
	int was_raw, was_dump, pru, s, rc;

	if (!bad && !mask[0] == !mask[1]) {
		bad = "a burst takes channels of one PRU";
	}
	if (bad) {
		send_error(v, "burst_error", bad);
		return;
	}
	pru = mask[0] ? 0 : 1;
	pthread_mutex_lock(&mode_lock);
	if (la_get_mode(&was_raw, was_mask, &was_dump)) {
		pthread_mutex_unlock(&mode_lock);
		send_error(v, "burst_error", "the logic analyzer does not run");
		return;
	}
	for (s = 0; s < 2; s++) {
		stream_poll(&st[s], ws_edge, v);
	}
	rc = la_burst(pru, mask[pru], sec ? atof(sec + 10) : 0, &r, &bi, err, sizeof(err));
	if (!rc) {
		/* Whatever came before it, then the burst, in time order. */
		for (s = 0; s < 2; s++) {
			stream_poll(&st[s], ws_edge, v);
		}
		for (k = 0; k < r.ev[pru].n; k++) {
			const struct event *e = &r.ev[pru].e[k];

			if (j < r.holes[pru].n && r.holes[pru].b[j] == e->t) {
				ws_hole(v, &st[pru], r.holes[pru].a[j], e->t, e->state);
				j++;
			} else {
				ws_edge(v, &st[pru], e->t, e->state);
			}
		}
		ws_hole(v, &st[1 - pru], r.begin, r.finish, v->last_state[1 - pru]);
		flush(v, 0);
		flush(v, 1);
		/* A hole the readers find later starts after the burst, not before it. */
		st[0].last = st[1].last = r.finish;
		n = (size_t)snprintf(reply, sizeof(reply),
				     "{\"type\":\"burst\",\"from\":%llu,\"to\":%llu,\"pru\":%d,\"blocks\":%llu,"
				     "\"lost\":%llu,\"edges\":%llu,\"channels\":[", (unsigned long long)r.begin,
				     (unsigned long long)r.finish, pru, (unsigned long long)bi.blocks,
				     (unsigned long long)bi.lost, (unsigned long long)bi.edges);
		for (k = 0, s = 0; k < AI64_LA_NCHAN; k++) {
			if (la_chans[k].pru == pru && mask[pru] >> la_chans[k].bit & 1u) {
				n += (size_t)snprintf(reply + n, sizeof(reply) - n, "%s%zu", s++ ? "," : "", k);
			}
		}
		snprintf(reply + n, sizeof(reply) - n, "]}");
		send_text(v, reply);
	}
	if (la_set_mode_ex(was_raw, was_mask, was_dump, back, sizeof(back))) {
		send_error(v, "mode_error", back);
	}
	pthread_mutex_unlock(&mode_lock);
	if (rc) {
		send_error(v, "burst_error", err);
	}
	rec_free(&r);
}

/*
 * What the viewer sent, if anything, without waiting: whole frames, masked
 * as a browser sends them. A close, or a frame too big for in, ends it.
 */
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
			if (strstr((char *)data, "\"type\":\"burst\"") && len < sizeof(v->burst)) {
				memcpy(v->burst, data, len + 1);
			}
			data[len] = (uint8_t)save;
		}
		memmove(v->in, v->in + head + len, v->nin - head - len);
		v->nin -= head + len;
	}
}

static void *viewer_main(void *arg)
{
	struct viewer *v = arg;
	struct la_follow fol = { 0 };
	struct stream st[2];
	char err0[80];
	uint64_t gaps_told[2] = { 0, 0 }, fol_edges[2] = { 0, 0 }, fol_lost[2] = { 0, 0 };
	double flush_at, stats_at;
	int s;

	memset(st, 0, sizeof(st));
	if (stream_init(&st[0], 0) || stream_init(&st[1], 1)) {
		goto out;
	}
	st[0].keep_marks = st[1].keep_marks = 1;
	st[0].on_hole = st[1].on_hole = ws_hole;
	send_meta(v);
	send_modes(v, 1);
	/* In dump mode the RTUs keep quiet, with no ticks: start from the copier. */
	if (!la_follow_open(&fol, err0, sizeof(err0))) {
		st[0].first = st[1].first = st[0].last = st[1].last = fol.now;
	} else if (wait_ticks(st, ws_edge, v)) {
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
		uint32_t before[2] = { st[0].rd, st[1].rd }, fol_rd = fol.rd;
		double now;
		char ferr[80];

		/*
		 * Dump mode: the raw PRU's channels come from the burst buffer,
		 * every sample, followed as it fills (la_burst.c); the streams
		 * have nothing then, not even ticks. A new mode, or a burst,
		 * starts the copier again, so the reader starts again too.
		 */
		if (fol.open && la_follow_stale(&fol)) {
			st[0].last = st[1].last = fol.now;
			fol_edges[fol.pru] += fol.edges;
			fol_lost[fol.pru] += fol.lost;
			la_follow_close(&fol);
		}
		if (!fol.open && (AI64_LA_MODE_OF(la_hdr->s[0].cfg_mode) == AI64_LA_MODE_DUMP ||
				  AI64_LA_MODE_OF(la_hdr->s[1].cfg_mode) == AI64_LA_MODE_DUMP)) {
			la_follow_open(&fol, ferr, sizeof(ferr));
			fol_rd = fol.rd;
		}
		if (fol.open) {
			la_follow_poll(&fol, &st[fol.pru], ws_edge, v);
		}

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
		read_viewer(v);
		if (v->burst[0]) {
			run_burst(v, st, v->burst);
			v->burst[0] = '\0';
		}
		send_modes(v, 0);
		now = now_us();
		if (now >= flush_at) {
			for (s = 0; s < 2; s++) {
				struct head h = { .kind = 5, .stream = (uint8_t)s,
						  .base = (double)(fol.open ? fol.now : st[s].base) };

				flush(v, s);
				send_msg(v, &h, sizeof(h));
			}
			flush_at = now + 20000;
		}
		if (now >= stats_at) {
			char buf[256];

			snprintf(buf, sizeof(buf),
				 "{\"type\":\"stats\",\"edges\":[%llu,%llu],\"gaps\":[%llu,%llu],"
				 "\"missed\":[%llu,%llu],\"late\":[%llu,%llu],\"sent\":%llu}",
				 (unsigned long long)(st[0].edges + fol_edges[0] + (fol.pru == 0 ? fol.edges : 0)),
				 (unsigned long long)(st[1].edges + fol_edges[1] + (fol.pru == 1 ? fol.edges : 0)),
				 (unsigned long long)(st[0].gaps + fol_lost[0] + (fol.pru == 0 ? fol.lost : 0)),
				 (unsigned long long)(st[1].gaps + fol_lost[1] + (fol.pru == 1 ? fol.lost : 0)),
				 (unsigned long long)(st[0].missed + (fol.pru == 0 ? fol.behind : 0)),
				 (unsigned long long)(st[1].missed + (fol.pru == 1 ? fol.behind : 0)),
				 (unsigned long long)st[0].late, (unsigned long long)st[1].late,
				 (unsigned long long)v->sent);
			send_text(v, buf);
			stats_at = now + 1e6;
		}
		if (st[0].rd == before[0] && st[1].rd == before[1] && fol.rd == fol_rd) {
			usleep(fol.open ? 500 : 2000);
		}
	}
out:
	la_follow_close(&fol);
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
