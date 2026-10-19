/* SPDX-License-Identifier: MIT */
/*
 * A remoteproc resource table with one entry, a trace buffer, and a small
 * text log in that buffer. Used by the C66x and C7x hello images, which
 * have no console and no RPMsg. Linux reads the log at
 * /sys/kernel/debug/remoteproc/remoteprocN/trace0.
 *
 * Define AI64_TRACE_SIZE and AI64_TRACE_NAME before including this. The
 * buffer goes in section .ai64_trace and the table in .resource_table;
 * each core's linker script places both in memory Linux can reach.
 * When the log fills up it starts again from the mark (trace_mark()), so
 * the lines written before the mark, such as the hello, stay.
 */
#ifndef AI64_DSP_TRACE_H_
#define AI64_DSP_TRACE_H_

#include <stdint.h>

/* The remoteproc resource table layout (include/linux/remoteproc.h). */
#define AI64_RSC_TRACE 2u

struct ai64_fw_rsc_trace {
	uint32_t type;
	uint32_t da;
	uint32_t len;
	uint32_t reserved;
	char name[32];
};

struct ai64_rsc_table {
	uint32_t ver;
	uint32_t num;
	uint32_t reserved[2];
	uint32_t offset[1];
	struct ai64_fw_rsc_trace trace;
};

#pragma DATA_SECTION(ai64_trace_buf, ".ai64_trace")
#pragma DATA_ALIGN(ai64_trace_buf, 8)
#pragma RETAIN(ai64_trace_buf)
char ai64_trace_buf[AI64_TRACE_SIZE];

#pragma DATA_SECTION(ai64_rsc_table, ".resource_table")
#pragma DATA_ALIGN(ai64_rsc_table, 8)
#pragma RETAIN(ai64_rsc_table)
const struct ai64_rsc_table ai64_rsc_table = {
	1, 1, { 0, 0 },
	{ 20 }, /* offsetof(struct ai64_rsc_table, trace) */
	{
		AI64_RSC_TRACE,
		(uint32_t)(uintptr_t)ai64_trace_buf,
		AI64_TRACE_SIZE,
		0,
		AI64_TRACE_NAME,
	},
};

static uint32_t ai64_trace_pos;
static uint32_t ai64_trace_keep;

static void trace_putc(char c)
{
	if (ai64_trace_pos >= AI64_TRACE_SIZE - 1u) {
		uint32_t i;

		for (i = ai64_trace_keep; i < AI64_TRACE_SIZE; i++) {
			ai64_trace_buf[i] = 0;
		}
		ai64_trace_pos = ai64_trace_keep;
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
	uint32_t i = 0;

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

/* A full log starts again from here. */
static void trace_mark(void)
{
	ai64_trace_keep = ai64_trace_pos;
}

static void trace_init(void)
{
	uint32_t i;

	for (i = 0; i < AI64_TRACE_SIZE; i++) {
		ai64_trace_buf[i] = 0;
	}
	ai64_trace_pos = 0;
	ai64_trace_keep = 0;
}

#endif
