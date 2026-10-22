/* SPDX-License-Identifier: MIT */
/*
 * Building and parsing short text messages on an RTU, which has no printf.
 * Every append stops one byte short of max and keeps the text terminated.
 */
#ifndef AI64_TEXT_H_
#define AI64_TEXT_H_

#include <stdint.h>

static uint16_t append_str(char *dst, uint16_t n, uint16_t max, const char *s)
{
	while (*s && n + 1 < max) {
		dst[n++] = *s++;
	}
	dst[n] = 0;
	return n;
}

static uint16_t append_u32(char *dst, uint16_t n, uint16_t max, uint32_t v)
{
	char tmp[10];
	uint16_t i = 0;

	if (v == 0) {
		return append_str(dst, n, max, "0");
	}
	while (v && i < sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i && n + 1 < max) {
		dst[n++] = tmp[--i];
	}
	dst[n] = 0;
	return n;
}

static int parse_u32(const char **pp, uint32_t *out)
{
	const char *s = *pp;
	uint32_t v = 0;
	int any = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10u + (uint32_t)(*s - '0');
		s++;
		any = 1;
	}
	if (!any) {
		return 0;
	}
	*out = v;
	*pp = s;
	return 1;
}

/* s starts with word and a space; *pp is left after the space. */
static int parse_word(const char **pp, const char *word)
{
	const char *s = *pp;

	while (*word) {
		if (*s++ != *word++) {
			return 0;
		}
	}
	if (*s++ != ' ') {
		return 0;
	}
	*pp = s;
	return 1;
}

/* "<word> <a>", decimal uint32. */
static int parse_one(const char *s, const char *word, uint32_t *a)
{
	return parse_word(&s, word) && parse_u32(&s, a) && *s == 0;
}

/* "<word> <a> <b>", both decimal uint32. */
static int parse_two(const char *s, const char *word, uint32_t *a, uint32_t *b)
{
	if (!parse_word(&s, word) || !parse_u32(&s, a) || *s != ' ') {
		return 0;
	}
	s++;
	return parse_u32(&s, b);
}

#endif
