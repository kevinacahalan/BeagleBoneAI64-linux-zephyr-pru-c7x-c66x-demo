/* SPDX-License-Identifier: MIT */
/*
 * A text log in the remoteproc trace buffer, for cores with no console.
 * Linux reads it at /sys/kernel/debug/remoteproc/remoteprocN/trace0.
 *
 * The including file defines the buffer (ai64_trace_buf, AI64_TRACE_SIZE)
 * next to its resource table. Linux prints the buffer up to the first NUL.
 * When the log fills up it starts again from the top, so the newest lines
 * are always there.
 */
#ifndef AI64_TRACE_H_
#define AI64_TRACE_H_

#include <stdint.h>

static uint16_t ai64_trace_pos;

static void trace_putc(char c)
{
	if (ai64_trace_pos >= AI64_TRACE_SIZE - 1u) {
		uint16_t i;

		for (i = 0; i < AI64_TRACE_SIZE; i++) {
			ai64_trace_buf[i] = 0;
		}
		ai64_trace_pos = 0;
	}
	ai64_trace_buf[ai64_trace_pos++] = c;
	ai64_trace_buf[ai64_trace_pos] = 0;
}

static void trace_puts(const char *s)
{
	while (*s) {
		trace_putc(*s++);
	}
}

static void trace_u32(uint32_t v)
{
	char tmp[10];
	uint16_t i = 0;

	if (v == 0) {
		trace_putc('0');
		return;
	}
	while (v && i < sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (i) {
		trace_putc(tmp[--i]);
	}
}

static void trace_init(void)
{
	uint16_t i;

	for (i = 0; i < AI64_TRACE_SIZE; i++) {
		ai64_trace_buf[i] = 0;
	}
	ai64_trace_pos = 0;
}

#endif
