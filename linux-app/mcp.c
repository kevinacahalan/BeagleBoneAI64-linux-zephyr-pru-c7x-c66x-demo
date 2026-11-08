/* SPDX-License-Identifier: MIT */
/*
 * ai64 mcp [port]
 *   An MCP server for the logic analyzer on ICSSG1, on port 8065 (or port),
 *   at /mcp: the Model Context Protocol's Streamable HTTP transport, which
 *   is JSON-RPC 2.0 in HTTP POSTs. An MCP client, Claude Code for one
 *   (claude mcp add --transport http ai64-la http://<board>:8065/mcp),
 *   can then capture and measure the board's pins:
 *
 *     la_channels  the 19 channels, their pins, and what the demo wires to them
 *     la_status    each PRU's edges a second, every channel's level, the trigger
 *     la_measure   each channel's frequency, period, high time, and duty, and
 *                  the timing between channel pairs, over a recording
 *     la_capture   a recording, plain or around the next trigger hit, kept
 *                  here for la_edges and la_decode, and as a VCD file
 *     la_edges     the last capture's changes on some channels, in a window
 *     la_decode    SPI or UART in the last capture
 *     la_trigger   sets the Tx_PRUs' trigger
 *     la_mark      a mark in the stream, through the fabric
 *
 *   Unlike ai64 web it can change something: the trigger, and the marks.
 *   ai64-demo starts it; nothing starts it at boot. With AI64_MCP_TOKEN set,
 *   it asks for "Authorization: Bearer <token>"; it refuses a request whose
 *   Origin is not the address it was sent to, so a web page elsewhere
 *   cannot reach it through a browser. Tools answer with JSON, sized for a
 *   model to read: summaries and windows, not whole captures.
 *
 * It needs root or CAP_SYS_RAWIO, for /dev/mem.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ai64.h"
#include "la.h"

#define MCP_PORT 8065
#define MCP_PROTOCOL "2025-06-18"
#define VCD_PATH "/tmp/ai64-la-capture.vcd"

/* ---- JSON in: a small parser, into an arena reset for each request ---- */

enum jt { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jv {
	enum jt t;
	int b;
	double num;
	char *str;          /* J_STR, decoded */
	const char *src;    /* the value's own text */
	size_t srclen;
	struct jv *kid;     /* J_ARR items, J_OBJ values */
	char **key;         /* J_OBJ names */
	size_t n;
};

static char arena[1 << 20];
static size_t arena_used;

static void *alloc(size_t n)
{
	void *p;

	n = (n + 15) & ~(size_t)15;
	if (arena_used + n > sizeof(arena)) {
		return NULL;
	}
	p = arena + arena_used;
	arena_used += n;
	return p;
}

static void skip_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
		(*p)++;
	}
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int parse_u4(const char *p, unsigned *u)
{
	int i;

	*u = 0;
	for (i = 0; i < 4; i++) {
		int h = hexval(p[i]);

		if (h < 0) {
			return -1;
		}
		*u = *u << 4 | (unsigned)h;
	}
	return 0;
}

static char *parse_string(const char **p)
{
	const char *s = *p + 1, *e = s;
	char *out, *o;

	while (*e && *e != '"') {
		e += *e == '\\' && e[1] ? 2 : 1;
	}
	if (*e != '"' || !(out = alloc((size_t)(e - s) + 1))) {
		return NULL;
	}
	for (o = out; s < e; s++) {
		unsigned u, lo;

		if (*s != '\\') {
			*o++ = *s;
			continue;
		}
		switch (*++s) {
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u':
			if (parse_u4(s + 1, &u)) {
				return NULL;
			}
			s += 4;
			if (u >= 0xd800 && u < 0xdc00 && s[1] == '\\' && s[2] == 'u' &&
			    !parse_u4(s + 3, &lo) && lo >= 0xdc00 && lo < 0xe000) {
				u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
				s += 6;
			}
			if (u < 0x80) {
				*o++ = (char)u;
			} else if (u < 0x800) {
				*o++ = (char)(0xc0 | u >> 6);
				*o++ = (char)(0x80 | (u & 0x3f));
			} else if (u < 0x10000) {
				*o++ = (char)(0xe0 | u >> 12);
				*o++ = (char)(0x80 | (u >> 6 & 0x3f));
				*o++ = (char)(0x80 | (u & 0x3f));
			} else {
				*o++ = (char)(0xf0 | u >> 18);
				*o++ = (char)(0x80 | (u >> 12 & 0x3f));
				*o++ = (char)(0x80 | (u >> 6 & 0x3f));
				*o++ = (char)(0x80 | (u & 0x3f));
			}
			break;
		default: *o++ = *s; break;
		}
	}
	*o = '\0';
	*p = e + 1;
	return out;
}

static int parse_value(const char **p, struct jv *v, int depth)
{
	memset(v, 0, sizeof(*v));
	skip_ws(p);
	v->src = *p;
	if (depth > 32) {
		return -1;
	}
	if (**p == '{' || **p == '[') {
		int obj = **p == '{';
		struct jv tmp[256];
		char *keys[256];
		size_t n = 0;

		(*p)++;
		skip_ws(p);
		while (**p != (obj ? '}' : ']')) {
			if (n == 256) {
				return -1;
			}
			if (obj) {
				skip_ws(p);
				if (**p != '"' || !(keys[n] = parse_string(p))) {
					return -1;
				}
				skip_ws(p);
				if (**p != ':') {
					return -1;
				}
				(*p)++;
			}
			if (parse_value(p, &tmp[n], depth + 1)) {
				return -1;
			}
			n++;
			skip_ws(p);
			if (**p == ',') {
				(*p)++;
				skip_ws(p);
			} else if (**p != (obj ? '}' : ']')) {
				return -1;
			}
		}
		(*p)++;
		v->t = obj ? J_OBJ : J_ARR;
		v->n = n;
		v->kid = alloc(n * sizeof(*v->kid) + 1);
		if (!v->kid) {
			return -1;
		}
		memcpy(v->kid, tmp, n * sizeof(*tmp));
		if (obj) {
			v->key = alloc(n * sizeof(*v->key) + 1);
			if (!v->key) {
				return -1;
			}
			memcpy(v->key, keys, n * sizeof(*keys));
		}
	} else if (**p == '"') {
		v->t = J_STR;
		if (!(v->str = parse_string(p))) {
			return -1;
		}
	} else if (!strncmp(*p, "true", 4) || !strncmp(*p, "false", 5)) {
		v->t = J_BOOL;
		v->b = **p == 't';
		*p += v->b ? 4 : 5;
	} else if (!strncmp(*p, "null", 4)) {
		v->t = J_NULL;
		*p += 4;
	} else {
		char *end;

		v->t = J_NUM;
		v->num = strtod(*p, &end);
		if (end == *p) {
			return -1;
		}
		*p = end;
	}
	v->srclen = (size_t)(*p - v->src);
	return 0;
}

static const struct jv *jget(const struct jv *o, const char *key)
{
	size_t i;

	if (!o || o->t != J_OBJ) {
		return NULL;
	}
	for (i = 0; i < o->n; i++) {
		if (!strcmp(o->key[i], key)) {
			return &o->kid[i];
		}
	}
	return NULL;
}

static double jnum(const struct jv *o, const char *key, double dflt)
{
	const struct jv *v = jget(o, key);

	return v && v->t == J_NUM ? v->num : dflt;
}

static const char *jstr(const struct jv *o, const char *key)
{
	const struct jv *v = jget(o, key);

	return v && v->t == J_STR ? v->str : NULL;
}

/* ---- JSON out ---- */

struct sb {
	char *b;
	size_t n, cap;
};

static void sb_add(struct sb *s, const char *fmt, ...)
{
	va_list ap;
	int k;

	for (;;) {
		size_t room = s->cap - s->n;

		va_start(ap, fmt);
		k = vsnprintf(s->b ? s->b + s->n : NULL, room, fmt, ap);
		va_end(ap);
		if (k >= 0 && (size_t)k < room) {
			s->n += (size_t)k;
			return;
		}
		s->cap = s->cap ? s->cap * 2 : 4096;
		while (s->cap - s->n <= (size_t)k) {
			s->cap *= 2;
		}
		s->b = realloc(s->b, s->cap);
		if (!s->b) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
}

/* str as a JSON string, quotes and all; NULL as null. */
static void sb_str(struct sb *s, const char *str)
{
	if (!str) {
		sb_add(s, "null");
		return;
	}
	sb_add(s, "\"");
	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;

		if (c == '"' || c == '\\') {
			sb_add(s, "\\%c", c);
		} else if (c == '\n') {
			sb_add(s, "\\n");
		} else if (c < 0x20) {
			sb_add(s, "\\u%04x", c);
		} else {
			sb_add(s, "%c", c);
		}
	}
	sb_add(s, "\"");
}

static void sb_spread(struct sb *s, const struct spread *sp)
{
	if (!sp->n) {
		sb_add(s, "null");
	} else {
		sb_add(s, "{\"min\":%lld,\"median\":%lld,\"max\":%lld,\"n\":%zu}", (long long)sp->min,
		       (long long)sp->med, (long long)sp->max, sp->n);
	}
}

/* ---- the last capture, for la_edges and la_decode ---- */

static struct recording cap;
static struct changes cap_ch[AI64_LA_NCHAN];
static int cap_level0[AI64_LA_NCHAN];
static int have_cap;

/* Channel c's level at t (absolute IEP ns) in the last capture. */
static int level_at(int c, uint64_t t)
{
	const struct changes *ch = &cap_ch[c];
	size_t lo = 0, hi = ch->n;

	while (lo < hi) {
		size_t m = (lo + hi) / 2;

		if (ch->t[m] <= t) {
			lo = m + 1;
		} else {
			hi = m;
		}
	}
	return lo ? ch->up[lo - 1] : cap_level0[c];
}

/* A channel argument: dflt if it is not given, -2 if it is not a channel. */
static int chan_arg(const struct jv *args, const char *key, int dflt)
{
	const struct jv *v = jget(args, key);

	if (!v) {
		return dflt;
	}
	return v->t == J_NUM && v->num >= 0 && v->num < AI64_LA_NCHAN && v->num == (int)v->num ?
	       (int)v->num : -2;
}

/* ---- the tools ---- */

typedef const char *(*tool_fn)(const struct jv *args, struct sb *out);

static void chan_json(struct sb *o, int c)
{
	sb_add(o, "\"ch\":%d,\"pin\":\"%s\"", c, la_chans[c].pin);
	if (la_wired[c].name) {
		sb_add(o, ",\"wired_to\":\"%s\",\"from\":\"%s\"", la_wired[c].name, la_wired[c].from);
	}
}

static const char *t_channels(const struct jv *args, struct sb *o)
{
	int c;

	(void)args;
	sb_add(o, "{\"channels\":[");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		sb_add(o, "%s{", c ? "," : "");
		chan_json(o, c);
		sb_add(o, ",\"ball\":\"%s\",\"pru\":%d,\"gpi\":%d}", la_chans[c].ball, la_chans[c].pru,
		       la_chans[c].bit);
	}
	sb_add(o, "],\"note\":\"Each channel is an ICSSG1 PRU GPI pin, 3.3 V only. In edge mode "
		  "each PRU stamps every change on its pins with IEP0, to within 8 ns, and a change "
		  "less than 48 ns after another on the same PRU waits for it; in raw mode (la_mode) "
		  "one PRU's channels are sampled every 8 ns with no gap. wired_to is what the demo's "
		  "jumpers bring; the others are spare.\"}");
	return NULL;
}

static const char *t_status(const struct jv *args, struct sb *o)
{
	uint32_t e0[2], e1[2];
	double t0, t1;
	int s, c;

	(void)args;
	if (la_map(0)) {
		return "the logic analyzer is not running on ICSSG1 (ai64-demo starts it)";
	}
	for (s = 0; s < 2; s++) {
		e0[s] = la_hdr->s[s].edges;
	}
	t0 = now_us();
	usleep(250000);
	t1 = now_us();
	for (s = 0; s < 2; s++) {
		e1[s] = la_hdr->s[s].edges;
	}
	sb_add(o, "{\"prus\":[");
	for (s = 0; s < 2; s++) {
		volatile struct ai64_la_stream *h = &la_hdr->s[s];
		static const char *const mode[] = { "off", "pattern", "any change" };

		sb_add(o, "%s{\"pru\":%d,\"edges_per_s\":%.0f,\"edges\":%u,\"gaps\":%u,\"lost_records\":%u,"
		       "\"marks\":%u,\"trigger_hits\":%u,\"trigger\":{\"mode\":\"%s\",\"mask\":%u,"
		       "\"value\":%u}}", s ? "," : "", s, (e1[s] - e0[s]) * 1e6 / (t1 - t0), h->edges,
		       h->gaps, h->lost, h->marks, h->trigs,
		       h->trig_mode < 3 ? mode[h->trig_mode] : "?", h->trig_mask, h->trig_value);
	}
	sb_add(o, "],\"levels\":[");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		uint32_t st = la_hdr->s[la_chans[c].pru].state;

		sb_add(o, "%s{", c ? "," : "");
		chan_json(o, c);
		sb_add(o, ",\"level\":%u}", st >> la_chans[c].bit & 1u);
	}
	sb_add(o, "]}");
	return NULL;
}

/* "a:b" pairs from args.pairs, or the demo's. */
static int pairs_arg(const struct jv *args, int pairs[16][2])
{
	static const int demo[][2] = { { 7, 8 }, { 8, 7 }, { 9, 10 }, { 10, 9 }, { 7, 13 } };
	const struct jv *p = jget(args, "pairs");
	int n = 0;
	size_t i;

	if (!p || p->t != J_ARR) {
		for (n = 0; n < 5; n++) {
			pairs[n][0] = demo[n][0];
			pairs[n][1] = demo[n][1];
		}
		return n;
	}
	for (i = 0; i < p->n && n < 16; i++) {
		if (p->kid[i].t != J_STR || sscanf(p->kid[i].str, "%d:%d", &pairs[n][0], &pairs[n][1]) != 2 ||
		    pairs[n][0] < 0 || pairs[n][0] >= (int)AI64_LA_NCHAN || pairs[n][1] < 0 ||
		    pairs[n][1] >= (int)AI64_LA_NCHAN) {
			return -1;
		}
		n++;
	}
	return n;
}

/* Every channel of r (changes in ch) as JSON: the active ones in full, the idle by level. */
static void stats_json(struct sb *o, struct recording *r, struct changes *ch)
{
	size_t most = 1;
	int64_t *v;
	int c, first = 1;

	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		most = ch[c].n > most ? ch[c].n : most;
	}
	v = malloc(most * sizeof(*v));
	sb_add(o, "\"duration_ns\":%llu,\"active\":[", (unsigned long long)(r->finish - r->begin));
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		struct chan_stats cs;

		if (!ch[c].n) {
			continue;
		}
		chan_stats(&ch[c], level(r->state, c), r->begin, r->finish, v, &cs,
			   &r->holes[la_chans[c].pru]);
		sb_add(o, "%s{", first ? "" : ",");
		chan_json(o, c);
		sb_add(o, ",\"edges\":%zu,\"freq_hz\":", cs.edges);
		if (cs.period.n) {
			sb_add(o, "%.3f", 1e9 / (double)cs.period.med);
		} else {
			sb_add(o, "null");
		}
		sb_add(o, ",\"period_ns\":");
		sb_spread(o, &cs.period);
		sb_add(o, ",\"high_ns\":");
		sb_spread(o, &cs.high);
		sb_add(o, ",\"duty_pct\":%.2f}", cs.known_ns ? 100.0 * (double)cs.high_ns / (double)cs.known_ns : 0.0);
		first = 0;
	}
	sb_add(o, "],\"idle\":[");
	first = 1;
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (ch[c].n) {
			continue;
		}
		sb_add(o, "%s{", first ? "" : ",");
		chan_json(o, c);
		sb_add(o, ",\"level\":%d}", level(r->state, c));
		first = 0;
	}
	sb_add(o, "]");
	/* Holes, where the stream lost records: no period or time spans one. */
	sb_add(o, ",\"holes\":[");
	for (c = 0; c < 2; c++) {
		uint64_t ns;
		size_t n = rec_holes(r, c, &ns);

		sb_add(o, "%s{\"pru\":%d,\"holes\":%zu,\"ns\":%llu,\"late_edges\":%llu}", c ? "," : "",
		       c, n, (unsigned long long)ns, (unsigned long long)r->st[c].late);
	}
	sb_add(o, "]");
	free(v);
}

static const char *t_measure(const struct jv *args, struct sb *o)
{
	static struct recording r;
	struct changes ch[AI64_LA_NCHAN];
	double secs = jnum(args, "seconds", 1);
	int pairs[16][2], np = pairs_arg(args, pairs), k;
	size_t most = 1;
	int64_t *v;

	if (np < 0) {
		return "pairs: each an \"a:b\" of two channels, 0 to 18";
	}
	if (secs <= 0 || secs > 10) {
		return "seconds: more than 0, at most 10";
	}
	if (record(&r, secs, 0)) {
		return "the logic analyzer is not running on ICSSG1 (ai64-demo starts it)";
	}
	rec_changes(&r, ch);
	sb_add(o, "{");
	stats_json(o, &r, ch);
	for (k = 0; k < (int)AI64_LA_NCHAN; k++) {
		most = ch[k].n > most ? ch[k].n : most;
	}
	v = malloc(most * sizeof(*v));
	sb_add(o, ",\"pairs\":[");
	for (k = 0; k < np; k++) {
		struct spread up, down;

		const struct holes *ha = &r.holes[la_chans[pairs[k][0]].pru];
		const struct holes *hb = &r.holes[la_chans[pairs[k][1]].pru];

		spread_of(v, nearest(&ch[pairs[k][0]], &ch[pairs[k][1]], 1, v, ha, hb), &up);
		spread_of(v, nearest(&ch[pairs[k][0]], &ch[pairs[k][1]], 0, v, ha, hb), &down);
		sb_add(o, "%s{\"a\":%d,\"b\":%d,\"after_a_rises_ns\":", k ? "," : "", pairs[k][0],
		       pairs[k][1]);
		sb_spread(o, &up);
		sb_add(o, ",\"after_a_falls_ns\":");
		sb_spread(o, &down);
		sb_add(o, "}");
	}
	sb_add(o, "],\"note\":\"Pairs: the signed time from each change of a to b's nearest change. "
		  "freq_hz is from the median period, rise to rise.\"}");
	free(v);
	changes_free(ch);
	rec_free(&r);
	return NULL;
}

static const char *t_capture(const struct jv *args, struct sb *o)
{
	double secs = jnum(args, "seconds", 0.1);
	const struct jv *trig = jget(args, "trigger");
	size_t nedge[2], nmk, i, shown = 0;
	struct mark *mk;
	FILE *f;
	int c;

	if (secs <= 0 || secs > 10) {
		return "seconds: more than 0, at most 10";
	}
	if (have_cap) {
		changes_free(cap_ch);
		rec_free(&cap);
		have_cap = 0;
	}
	if (record(&cap, secs, trig && trig->t == J_BOOL && trig->b)) {
		return "no recording: the analyzer is not running, or no trigger hit came in 30 s "
		       "(la_trigger sets one)";
	}
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		cap_level0[c] = level(cap.state, c);
	}
	rec_changes(&cap, cap_ch);
	have_cap = 1;
	sb_add(o, "{");
	stats_json(o, &cap, cap_ch);
	if (cap.hit) {
		sb_add(o, ",\"trigger_at_ns\":%llu", (unsigned long long)(cap.hit - cap.begin));
	}
	nmk = rec_marks(&cap, &mk);
	sb_add(o, ",\"marks_and_hits\":%zu,\"first_marks\":[", nmk);
	for (i = 0; i < nmk && shown < 50; i++, shown++) {
		sb_add(o, "%s{\"t_ns\":%llu,\"kind\":\"%s\",\"value\":%u", i ? "," : "",
		       (unsigned long long)(mk[i].t - cap.begin), mk[i].trig ? "trigger" : "mark",
		       mk[i].value);
		if (!mk[i].trig) {
			sb_add(o, ",\"from_node\":%u", mk[i].from);
		}
		sb_add(o, "}");
	}
	free(mk);
	sb_add(o, "]");
	f = fopen(VCD_PATH, "w");
	if (f) {
		rec_write_vcd(&cap, f, nedge);
		fclose(f);
		sb_add(o, ",\"vcd_on_board\":\"%s\"", VCD_PATH);
	}
	sb_add(o, ",\"note\":\"Times are ns from the capture's start. la_edges and la_decode read "
		  "this capture.\"}");
	return NULL;
}

static const char *t_edges(const struct jv *args, struct sb *o)
{
	const struct jv *chs = jget(args, "channels");
	uint64_t dur, from, to;
	int limit = (int)jnum(args, "limit", 500), want[AI64_LA_NCHAN], nw = 0, k;

	if (!have_cap) {
		return "no capture yet: run la_capture first";
	}
	dur = cap.finish - cap.begin;
	from = cap.begin + (uint64_t)jnum(args, "from_ns", 0);
	to = cap.begin + (uint64_t)jnum(args, "to_ns", (double)dur);
	if (limit < 1 || limit > 5000) {
		return "limit: 1 to 5000 changes a channel";
	}
	if (chs && chs->t == J_ARR) {
		size_t i;

		for (i = 0; i < chs->n && nw < (int)AI64_LA_NCHAN; i++) {
			if (chs->kid[i].t != J_NUM || chs->kid[i].num < 0 || chs->kid[i].num >= AI64_LA_NCHAN) {
				return "channels: numbers, 0 to 18";
			}
			want[nw++] = (int)chs->kid[i].num;
		}
	} else {
		for (k = 0; k < (int)AI64_LA_NCHAN; k++) {
			if (cap_ch[k].n) {
				want[nw++] = k;
			}
		}
	}
	sb_add(o, "{\"from_ns\":%llu,\"to_ns\":%llu,\"channels\":[", (unsigned long long)(from - cap.begin),
	       (unsigned long long)(to - cap.begin));
	for (k = 0; k < nw; k++) {
		const struct changes *ch = &cap_ch[want[k]];
		size_t i, total = 0, shown = 0;

		sb_add(o, "%s{", k ? "," : "");
		chan_json(o, want[k]);
		sb_add(o, ",\"level_at_from\":%d,\"changes\":[", level_at(want[k], from));
		for (i = 0; i < ch->n; i++) {
			if (ch->t[i] <= from || ch->t[i] > to) {
				continue;
			}
			total++;
			if ((int)shown < limit) {
				sb_add(o, "%s[%llu,%d]", shown ? "," : "",
				       (unsigned long long)(ch->t[i] - cap.begin), ch->up[i]);
				shown++;
			}
		}
		sb_add(o, "],\"in_window\":%zu,\"truncated\":%s}", total, total > shown ? "true" : "false");
	}
	sb_add(o, "],\"note\":\"Each change is [t_ns, level after it], ns from the capture's start.\"}");
	return NULL;
}

static void byte_out(struct sb *hex, struct sb *txt, unsigned b, int first)
{
	sb_add(hex, "%s%02x", first ? "" : " ", b);
	sb_add(txt, "%c", b >= 0x20 && b < 0x7f && b != '"' && b != '\\' ? (char)b : '.');
}

static const char *decode_spi(const struct jv *args, struct sb *o)
{
	int cs = chan_arg(args, "cs", -1), clk = chan_arg(args, "clk", -1);
	int mosi = chan_arg(args, "mosi", -1), miso = chan_arg(args, "miso", -1);
	int cpol = (int)jnum(args, "cpol", 0), cpha = (int)jnum(args, "cpha", 0);
	int bits = (int)jnum(args, "bits", 8);
	const struct jv *lsb = jget(args, "lsb_first");
	int lsb_first = lsb && lsb->t == J_BOOL && lsb->b;
	int sample_up = cpol == cpha, frames = 0, nb = 0, first = 1;
	unsigned acc_o = 0, acc_i = 0;
	const struct changes *k;
	struct sb hex = { 0 }, txt = { 0 }, ihex = { 0 }, itxt = { 0 };
	uint64_t frame_t = 0;
	int in_frame = 0;
	size_t i;

	if (clk < 0 || mosi < 0 || cs == -2 || miso == -2 || bits < 1 || bits > 32) {
		return "spi: clk and mosi channels, and optionally cs (active low) and miso; cpol, cpha, "
		       "bits, lsb_first";
	}
	k = &cap_ch[clk];
	sb_add(o, "{\"protocol\":\"spi\",\"frames\":[");
	for (i = 0; i < k->n && frames < 200; i++) {
		uint64_t t = k->t[i];
		int sel = cs < 0 || level_at(cs, t) == 0;

		if (k->up[i] != sample_up) {
			continue;
		}
		if (!sel) {
			continue;
		}
		/* A new frame at each fall of CS. */
		if (cs >= 0) {
			size_t lo = 0, hi = cap_ch[cs].n;
			uint64_t fell = cap.begin;

			while (lo < hi) {
				size_t m = (lo + hi) / 2;

				if (cap_ch[cs].t[m] <= t) {
					lo = m + 1;
				} else {
					hi = m;
				}
			}
			if (lo) {
				fell = cap_ch[cs].t[lo - 1];
			}
			if (!in_frame || fell != frame_t) {
				if (in_frame) {
					sb_add(o, "%s{\"t_ns\":%llu,\"mosi\":\"%s\",\"mosi_text\":\"%s\"", frames ? "," : "",
					       (unsigned long long)(frame_t - cap.begin), hex.b ? hex.b : "",
					       txt.b ? txt.b : "");
					if (miso >= 0) {
						sb_add(o, ",\"miso\":\"%s\"", ihex.b ? ihex.b : "");
					}
					sb_add(o, "}");
					frames++;
					hex.n = txt.n = ihex.n = itxt.n = 0;
					if (hex.b) {
						hex.b[0] = txt.b[0] = '\0';
					}
					if (ihex.b) {
						ihex.b[0] = itxt.b[0] = '\0';
					}
					first = 1;
				}
				in_frame = 1;
				frame_t = fell;
				nb = 0;
				acc_o = acc_i = 0;
			}
		} else if (!in_frame) {
			in_frame = 1;
			frame_t = t;
		}
		if (lsb_first) {
			acc_o |= (unsigned)level_at(mosi, t) << nb;
			acc_i |= miso >= 0 ? (unsigned)level_at(miso, t) << nb : 0;
		} else {
			acc_o = acc_o << 1 | (unsigned)level_at(mosi, t);
			acc_i = acc_i << 1 | (miso >= 0 ? (unsigned)level_at(miso, t) : 0);
		}
		if (++nb == bits) {
			byte_out(&hex, &txt, acc_o, first);
			if (miso >= 0) {
				byte_out(&ihex, &itxt, acc_i, first);
			}
			first = 0;
			nb = 0;
			acc_o = acc_i = 0;
		}
	}
	if (in_frame && hex.n) {
		sb_add(o, "%s{\"t_ns\":%llu,\"mosi\":\"%s\",\"mosi_text\":\"%s\"", frames ? "," : "",
		       (unsigned long long)(frame_t - cap.begin), hex.b, txt.b);
		if (miso >= 0) {
			sb_add(o, ",\"miso\":\"%s\"", ihex.b ? ihex.b : "");
		}
		sb_add(o, "}");
	}
	sb_add(o, "],\"sampled_on\":\"%s clock edges\"}", sample_up ? "rising" : "falling");
	free(hex.b);
	free(txt.b);
	free(ihex.b);
	free(itxt.b);
	return NULL;
}

static const char *decode_uart(const struct jv *args, struct sb *o)
{
	int ch = chan_arg(args, "channel", -1), bits = (int)jnum(args, "bits", 8);
	double baud = jnum(args, "baud", 0);
	const struct jv *inv = jget(args, "invert");
	int invert = inv && inv->t == J_BOOL && inv->b;
	const struct changes *k;
	struct sb hex = { 0 }, txt = { 0 };
	uint64_t busy_until = 0;
	size_t i, n = 0;

	if (ch < 0 || baud < 50 || baud > 20e6 || bits < 5 || bits > 9) {
		return "uart: channel and baud, and optionally bits (8) and invert; no parity, one stop bit";
	}
	k = &cap_ch[ch];
	for (i = 0; i < k->n && n < 4096; i++) {
		double bit = 1e9 / baud;
		unsigned v = 0;
		int b;

		/* A start bit: the line leaving idle (high, or low if inverted). */
		if (k->t[i] < busy_until || k->up[i] != invert) {
			continue;
		}
		for (b = 0; b < bits; b++) {
			int lv = level_at(ch, k->t[i] + (uint64_t)((1.5 + b) * bit));

			v |= (unsigned)(lv ^ invert) << b;
		}
		byte_out(&hex, &txt, v & 0xff, n == 0);
		busy_until = k->t[i] + (uint64_t)((bits + 1.5) * bit);
		n++;
	}
	sb_add(o, "{\"protocol\":\"uart\",\"bytes\":%zu,\"hex\":\"%s\",\"text\":\"%s\"}", n,
	       hex.b ? hex.b : "", txt.b ? txt.b : "");
	free(hex.b);
	free(txt.b);
	return NULL;
}

static const char *t_decode(const struct jv *args, struct sb *o)
{
	const char *p = jstr(args, "protocol");

	if (!have_cap) {
		return "no capture yet: run la_capture first";
	}
	if (p && !strcmp(p, "spi")) {
		return decode_spi(args, o);
	}
	if (p && !strcmp(p, "uart")) {
		return decode_uart(args, o);
	}
	return "protocol: spi or uart";
}

/* The analyzer's mode, as the RTUs have it. */
static void mode_json(struct sb *o)
{
	static const char *const names[] = { "edge", "raw", "conv" };
	int s, c, n = 0;

	sb_add(o, "{\"prus\":[");
	for (s = 0; s < 2; s++) {
		uint32_t m = la_hdr->s[s].cfg_mode;

		sb_add(o, "%s{\"pru\":%d,\"mode\":\"%s\",\"two_samples_a_word\":%s}", s ? "," : "", s,
		       names[AI64_LA_MODE_OF(m) < 3 ? AI64_LA_MODE_OF(m) : 0],
		       AI64_LA_LANE_OF(m) ? "true" : "false");
	}
	sb_add(o, "],\"channels_recorded\":[");
	for (c = 0; c < (int)AI64_LA_NCHAN; c++) {
		if (la_hdr->s[la_chans[c].pru].cfg_mask >> la_chans[c].bit & 1u) {
			sb_add(o, "%s%d", n++ ? "," : "", c);
		}
	}
	sb_add(o, "],\"note\":\"edge: both PRUs record each change of their channels, up to about "
		  "15 million edges a second streamed; a change less than 48 ns after another on the "
		  "same PRU waits. raw: one PRU's channels sampled every 8 ns with no gap, and the "
		  "other PRU (conv) finds the changes and records none of its own; up to 10 million "
		  "edges a second with none lost when the channels fit 16 pins (two samples a word), "
		  "6.7 million otherwise. Channels not recorded read low.\"}");
}

static const char *t_mode(const struct jv *args, struct sb *o)
{
	const char *mode = jstr(args, "mode");
	const struct jv *chs = jget(args, "channels");
	uint32_t mask[2] = { 0, 0 };
	char err[160];
	int raw = -1;
	size_t i;

	if (la_map(0)) {
		return "the logic analyzer is not running on ICSSG1";
	}
	if (!mode) {
		mode_json(o);
		return NULL;
	}
	if (chs && chs->t != J_ARR) {
		return "channels: an array of channel numbers, 0 to 18";
	}
	for (i = 0; chs && i < chs->n; i++) {
		const struct jv *c = &chs->kid[i];

		if (c->t != J_NUM || c->num < 0 || c->num >= AI64_LA_NCHAN || c->num != (int)c->num) {
			return "channels: an array of channel numbers, 0 to 18";
		}
		mask[la_chans[(int)c->num].pru] |= 1u << la_chans[(int)c->num].bit;
	}
	if (!strcmp(mode, "edge")) {
		if (!mask[0] && !mask[1]) {
			mask[0] = AI64_LA_MASK(0);
			mask[1] = AI64_LA_MASK(1);
		}
	} else if (!strcmp(mode, "raw")) {
		if (!mask[0] == !mask[1]) {
			return mask[0] ? "raw mode takes the channels of one PRU (la_channels has which)" :
			       "raw mode needs its channels";
		}
		raw = mask[0] ? 0 : 1;
	} else {
		return "mode: edge or raw";
	}
	if (la_set_mode(raw, mask, err, sizeof(err))) {
		sb_add(o, "{\"error\":\"%s\"}", err);
		return NULL;
	}
	mode_json(o);
	return NULL;
}

static const char *t_trigger(const struct jv *args, struct sb *o)
{
	const char *mode = jstr(args, "mode");
	const struct jv *pat = jget(args, "pattern");
	uint32_t m = AI64_LA_TRIG_OFF, mask = 0, value = 0;
	int pru = -1, c = chan_arg(args, "channel", -1);

	if (!mode) {
		return "mode: off, rise, fall, any, or pattern";
	}
	if (!strcmp(mode, "rise") || !strcmp(mode, "fall") || !strcmp(mode, "any")) {
		if (c < 0) {
			return "channel: 0 to 18";
		}
		pru = la_chans[c].pru;
		mask = 1u << la_chans[c].bit;
		m = !strcmp(mode, "any") ? AI64_LA_TRIG_ANY : AI64_LA_TRIG_ENTER;
		value = !strcmp(mode, "rise") ? mask : 0;
	} else if (!strcmp(mode, "pattern")) {
		size_t i;

		if (!pat || pat->t != J_OBJ || !pat->n) {
			return "pattern: an object of channel to level, such as {\"3\": 0, \"4\": 1}";
		}
		m = AI64_LA_TRIG_ENTER;
		for (i = 0; i < pat->n; i++) {
			int ch = atoi(pat->key[i]);

			if (ch < 0 || ch >= (int)AI64_LA_NCHAN || pat->kid[i].t != J_NUM) {
				return "pattern: channels 0 to 18, levels 0 or 1";
			}
			if (pru >= 0 && la_chans[ch].pru != pru) {
				return "pattern: a trigger takes one PRU's channels (la_channels has which)";
			}
			pru = la_chans[ch].pru;
			mask |= 1u << la_chans[ch].bit;
			value |= (pat->kid[i].num != 0) << la_chans[ch].bit;
		}
	} else if (strcmp(mode, "off")) {
		return "mode: off, rise, fall, any, or pattern";
	}
	if (la_set_trigger(pru, m, mask, value)) {
		return "the logic analyzer is not running on ICSSG1";
	}
	if (pru < 0) {
		sb_add(o, "{\"trigger\":\"off\"}");
	} else {
		sb_add(o, "{\"trigger\":\"%s\",\"pru\":%d,\"mask\":%u,\"value\":%u,\"note\":\"Tx_PRU%d "
			  "matches it; la_capture with trigger true records around the next hit.\"}",
		       mode, pru, mask, value, pru);
	}
	return NULL;
}

static const char *t_mark(const struct jv *args, struct sb *o)
{
	double v = jnum(args, "value", -1);
	char val[16];
	char *av[] = { "ai64", "fabric", "route", "g1rtu0", "mark", val, NULL };

	if (v < 0 || v > 65535 || v != (int)v) {
		return "value: 0 to 65535";
	}
	snprintf(val, sizeof(val), "%d", (int)v);
	if (fabric_main(6, av)) {
		return "the route to ICSSG1 RTU0 failed: is the demo running?";
	}
	fflush(stdout);
	sb_add(o, "{\"marked\":%d,\"note\":\"The mark went through the fabric to ICSSG1 RTU0, which "
		  "put it in PRU0's stream.\"}", (int)v);
	return NULL;
}

static const struct {
	const char *name;
	tool_fn fn;
	const char *desc;
	const char *schema;
} tools[] = {
	{ "la_channels", t_channels,
	  "The logic analyzer's 19 channels on the BeagleBone AI-64: each one's header pin, ball, "
	  "PRU and GPI bit, and what the demo's jumper wires bring to it.",
	  "{\"type\":\"object\",\"properties\":{}}" },
	{ "la_status", t_status,
	  "Live state, from a quarter-second look: each PRU's edges per second and totals, gaps, "
	  "marks, trigger hits, the trigger, and every channel's level now.",
	  "{\"type\":\"object\",\"properties\":{}}" },
	{ "la_measure", t_measure,
	  "Records the channels for some seconds and measures each active one: edges, frequency, "
	  "period and high time (min, median, max, in ns), and duty. Pairs a:b give the time from "
	  "each change of a to b's nearest change, such as a PWM's dead band between A and B. "
	  "holes lists each PRU's holes, where the stream lost records (no interval spans one), "
	  "and its late edges: in edge mode, changes that came while the PRU was busy, so up to "
	  "50 ns late, with pulses that short possibly missing; raw mode has them exactly.",
	  "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"number\",\"description\":"
	  "\"how long to record, at most 10; default 1\"},\"pairs\":{\"type\":\"array\",\"items\":"
	  "{\"type\":\"string\"},\"description\":\"\\\"a:b\\\" channel pairs; default the demo's PWM "
	  "A/B pairs and its two copies of one signal\"}}}" },
	{ "la_capture", t_capture,
	  "Records all 19 channels, for some seconds or around the next trigger hit (a tenth before, "
	  "the rest after), keeps it for la_edges and la_decode, saves a VCD file on the board, and "
	  "returns a summary: each active channel's measurements, and the marks and trigger hits.",
	  "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"number\",\"description\":"
	  "\"at most 10; default 0.1\"},\"trigger\":{\"type\":\"boolean\",\"description\":\"wait for "
	  "the next trigger hit (la_trigger sets one) and record around it\"}}}" },
	{ "la_edges", t_edges,
	  "The last capture's changes on some channels in a time window, as [t_ns, level] from the "
	  "capture's start, at most limit a channel.",
	  "{\"type\":\"object\",\"properties\":{\"channels\":{\"type\":\"array\",\"items\":{\"type\":"
	  "\"integer\"},\"description\":\"default every channel with changes\"},\"from_ns\":{\"type\":"
	  "\"number\"},\"to_ns\":{\"type\":\"number\"},\"limit\":{\"type\":\"integer\",\"description\":"
	  "\"changes a channel, at most 5000; default 500\"}}}" },
	{ "la_decode", t_decode,
	  "Decodes SPI (clk, mosi, optional cs active low and miso, cpol, cpha, bits, lsb_first) or "
	  "UART (channel, baud, optional bits and invert; 8N1) in the last capture. The demo's SPI7 "
	  "is cs 3, clk 4, mosi 5, mode 0.",
	  "{\"type\":\"object\",\"properties\":{\"protocol\":{\"type\":\"string\",\"enum\":[\"spi\","
	  "\"uart\"]},\"cs\":{\"type\":\"integer\"},\"clk\":{\"type\":\"integer\"},\"mosi\":{\"type\":"
	  "\"integer\"},\"miso\":{\"type\":\"integer\"},\"cpol\":{\"type\":\"integer\"},\"cpha\":{"
	  "\"type\":\"integer\"},\"bits\":{\"type\":\"integer\"},\"lsb_first\":{\"type\":\"boolean\"},"
	  "\"channel\":{\"type\":\"integer\"},\"baud\":{\"type\":\"number\"},\"invert\":{\"type\":"
	  "\"boolean\"}},\"required\":[\"protocol\"]}" },
	{ "la_mode", t_mode,
	  "The capture mode, or sets it. edge: every channel (or those given), each change "
	  "stamped to within 8 ns, a change within 48 ns of another on the same PRU late. raw: "
	  "the given channels, all of one PRU, sampled every 8 ns with no gap, so pulses and "
	  "gaps down to 8 ns are exact; the other PRU's channels are not recorded. Setting it "
	  "restarts both PRUs, a gap of about 50 ms. Without mode, says which runs.",
	  "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"edge\","
	  "\"raw\"]},\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}}}" },
	{ "la_trigger", t_trigger,
	  "Sets the trigger the ICSSG1 Tx_PRUs match on the edges as they come: off, a channel's "
	  "rise, fall, or any change, or a pattern of channel levels (one PRU's channels only).",
	  "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"off\","
	  "\"rise\",\"fall\",\"any\",\"pattern\"]},\"channel\":{\"type\":\"integer\"},\"pattern\":{"
	  "\"type\":\"object\",\"description\":\"channel number to level, such as {\\\"3\\\": 0, "
	  "\\\"4\\\": 1}\"}},\"required\":[\"mode\"]}" },
	{ "la_mark", t_mark,
	  "Puts a mark with a 16-bit value in the analyzer's stream, through the board's fabric to "
	  "ICSSG1 RTU0, so a capture shows when it happened next to the pins.",
	  "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},\"required\":"
	  "[\"value\"]}" },
};

#define NTOOLS (sizeof(tools) / sizeof(tools[0]))

/* ---- JSON-RPC ---- */

static void rpc_error(struct sb *o, const struct jv *id, int code, const char *msg)
{
	sb_add(o, "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"error\":{\"code\":%d,\"message\":",
	       id ? (int)id->srclen : 4, id ? id->src : "null", code);
	sb_str(o, msg);
	sb_add(o, "}}");
}

/* One JSON-RPC message; 0 if it was a notification, which gets no answer. */
static int rpc(const char *body, struct sb *o)
{
	struct jv msg;
	const struct jv *id, *params;
	const char *method, *p = body;

	arena_used = 0;
	if (parse_value(&p, &msg, 0) || msg.t != J_OBJ) {
		rpc_error(o, NULL, -32700, "parse error");
		return 1;
	}
	id = jget(&msg, "id");
	method = jstr(&msg, "method");
	params = jget(&msg, "params");
	if (!method) {
		if (!id) {
			return 0; /* a response to us, or noise */
		}
		rpc_error(o, id, -32600, "no method");
		return 1;
	}
	if (!id) {
		return 0; /* notifications/initialized and the like */
	}
	sb_add(o, "{\"jsonrpc\":\"2.0\",\"id\":%.*s,", (int)id->srclen, id->src);
	if (!strcmp(method, "initialize")) {
		const char *want = jstr(params, "protocolVersion");
		const char *ver = want && (!strcmp(want, "2025-06-18") || !strcmp(want, "2025-03-26") ||
					   !strcmp(want, "2024-11-05")) ? want : MCP_PROTOCOL;

		sb_add(o, "\"result\":{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{"
			  "\"listChanged\":false}},\"serverInfo\":{\"name\":\"ai64-la\",\"title\":"
			  "\"BeagleBone AI-64 logic analyzer\",\"version\":\"1.0\"},\"instructions\":"
			  "\"A 19-channel logic analyzer on the BeagleBone AI-64's ICSSG1, watching the "
			  "demo's own signals through jumper wires: start with la_channels, then "
			  "la_measure, or la_capture and la_edges or la_decode.\"}}", ver);
	} else if (!strcmp(method, "ping")) {
		sb_add(o, "\"result\":{}}");
	} else if (!strcmp(method, "tools/list")) {
		size_t i;

		sb_add(o, "\"result\":{\"tools\":[");
		for (i = 0; i < NTOOLS; i++) {
			sb_add(o, "%s{\"name\":\"%s\",\"description\":", i ? "," : "", tools[i].name);
			sb_str(o, tools[i].desc);
			sb_add(o, ",\"inputSchema\":%s}", tools[i].schema);
		}
		sb_add(o, "]}}");
	} else if (!strcmp(method, "tools/call")) {
		const char *name = jstr(params, "name");
		const struct jv *args = jget(params, "arguments");
		struct sb res = { 0 };
		const char *err = "no such tool";
		size_t i;

		for (i = 0; i < NTOOLS; i++) {
			if (name && !strcmp(name, tools[i].name)) {
				err = tools[i].fn(args, &res);
				break;
			}
		}
		if (err) {
			sb_add(o, "\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
			sb_str(o, err);
			sb_add(o, "}],\"isError\":true}}");
		} else {
			sb_add(o, "\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
			sb_str(o, res.b);
			sb_add(o, "}],\"structuredContent\":%s,\"isError\":false}}", res.b);
		}
		free(res.b);
	} else {
		o->n = 0;
		rpc_error(o, id, -32601, "method not found");
	}
	return 1;
}

/* ---- HTTP ---- */

static void reply(int fd, const char *status, const char *type, const char *body, size_t len);

static void reply_text(int fd, const char *status, const char *text)
{
	reply(fd, status, "text/plain", text, strlen(text));
}

static void reply(int fd, const char *status, const char *type, const char *body, size_t len)
{
	char head[256];
	int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len);

	if (write_all(fd, head, (size_t)n) == 0 && len) {
		write_all(fd, body, len);
	}
}

/* The value of header name in the request head, into buf; NULL if it has none. */
static const char *header(const char *head, const char *name, char *buf, size_t len)
{
	const char *p = head;
	size_t n = strlen(name);

	while ((p = strchr(p, '\n'))) {
		p++;
		if (!strncasecmp(p, name, n) && p[n] == ':') {
			size_t k = 0;

			for (p += n + 1; *p == ' '; p++) {
			}
			while (p[k] && p[k] != '\r' && p[k] != '\n' && k + 1 < len) {
				k++;
			}
			memcpy(buf, p, k);
			buf[k] = '\0';
			return buf;
		}
	}
	return NULL;
}

static void serve(int fd, const char *token)
{
	static char req[65536 + 1];
	size_t n = 0, head_len = 0, body_len = 0;
	char val[512], host[256];
	const char *eoh;
	ssize_t k;

	while (n < sizeof(req) - 1) {
		k = read(fd, req + n, sizeof(req) - 1 - n);
		if (k <= 0) {
			return;
		}
		n += (size_t)k;
		req[n] = '\0';
		if (!head_len && (eoh = strstr(req, "\r\n\r\n"))) {
			head_len = (size_t)(eoh - req) + 4;
			body_len = header(req, "Content-Length", val, sizeof(val)) ? strtoul(val, NULL, 10) : 0;
			if (head_len + body_len > sizeof(req) - 1) {
				reply_text(fd, "413 Payload Too Large", "too large\n");
				return;
			}
		}
		if (head_len && n >= head_len + body_len) {
			break;
		}
	}
	if (!head_len) {
		return;
	}
	req[head_len - 2] = '\0'; /* the head, on its own */
	if (strncmp(req, "POST /mcp ", 10) && strncmp(req, "POST /mcp?", 10)) {
		if (!strncmp(req, "GET /mcp", 8) || !strncmp(req, "DELETE /mcp", 11)) {
			reply_text(fd, "405 Method Not Allowed", "POST only: no event stream\n");
		} else {
			reply_text(fd, "404 Not Found", "the MCP endpoint is /mcp\n");
		}
		return;
	}
	/* A browser page elsewhere must not reach this through the user's browser. */
	if (header(req, "Origin", val, sizeof(val)) && strcmp(val, "null")) {
		const char *o = strstr(val, "://");

		if (!header(req, "Host", host, sizeof(host)) || !o || strcmp(o + 3, host)) {
			reply_text(fd, "403 Forbidden", "bad Origin\n");
			return;
		}
	}
	if (token) {
		char want[300];

		snprintf(want, sizeof(want), "Bearer %s", token);
		if (!header(req, "Authorization", val, sizeof(val)) || strcmp(val, want)) {
			reply_text(fd, "401 Unauthorized", "AI64_MCP_TOKEN is set: send it as a "
			      "bearer token\n");
			return;
		}
	}
	req[head_len + body_len] = '\0';
	{
		struct sb o = { 0 };

		if (rpc(req + head_len, &o)) {
			reply(fd, "200 OK", "application/json", o.b, o.n);
		} else {
			reply_text(fd, "202 Accepted", "");
		}
		free(o.b);
	}
}

int mcp_main(int argc, char **argv)
{
	int port = argc > 2 ? atoi(argv[2]) : MCP_PORT;
	const char *token = getenv("AI64_MCP_TOKEN");
	struct sockaddr_in a = { 0 };
	struct timeval tv = { .tv_sec = 5 };
	int s, one = 1;

	if (port <= 0 || port > 65535 || argc > 3) {
		fprintf(stderr, "usage: %s mcp [port]\n", argv[0]);
		return 2;
	}
	if (token && !*token) {
		token = NULL;
	}
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
	printf("mcp window ready: the logic analyzer's MCP server on port %d, at /mcp%s\n", port,
	       token ? ", with a bearer token" : "");
	printf("  claude mcp add --transport http ai64-la http://<board>:%d/mcp\n", port);
	fflush(stdout);
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
		serve(c, token);
		close(c);
	}
}
