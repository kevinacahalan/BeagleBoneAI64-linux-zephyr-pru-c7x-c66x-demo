/* SPDX-License-Identifier: MIT */
/*
 * A text log in the remoteproc trace buffer, for cores with no console.
 * Linux reads it at /sys/kernel/debug/remoteproc/remoteprocN/trace0.
 *
 * The including file defines the buffer (ai64_trace_buf, AI64_TRACE_SIZE)
 * next to its resource table. Linux prints the buffer up to the first NUL.
 * When the log fills up it starts again from the top, so the newest lines
 * are always there.
 *
 * A real-time core built with AI64_TRACE=0 (make TRACE=0) keeps the buffer,
 * so its resource table does not change, but every call below does nothing.
 */
#ifndef AI64_TRACE_H_
#define AI64_TRACE_H_

#include <stdint.h>

#if defined(AI64_TRACE) && AI64_TRACE == 0
#define AI64_TRACE_ON 0
#define trace_putc(c) ((void)(c))
#define trace_puts(s) ((void)(s))
#define trace_u32(v)  ((void)(v))
#define trace_init()  ((void)0)
#else
#define AI64_TRACE_ON 1
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

/* Decimal without a divide, as append_u32 in ai64_text.h. */
static void trace_u32(uint32_t v)
{
	static const uint32_t pow10[10] = {
		1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
		10000u, 1000u, 100u, 10u, 1u,
	};
	uint16_t i, any = 0;

	for (i = 0; i < 10u; i++) {
		char d = '0';

		while (v >= pow10[i]) {
			v -= pow10[i];
			d++;
		}
		if (d != '0' || any || i == 9u) {
			any = 1;
			trace_putc(d);
		}
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

#endif
