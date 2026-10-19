/* SPDX-License-Identifier: MIT */
/*
 * Resource table with one entry, a trace buffer. Used by PRU0 and Tx_PRU0,
 * which have no RPMsg. Define AI64_TRACE_SIZE and AI64_TRACE_NAME first.
 *
 * The buffer goes in section .ai64_trace, which each core's linker script
 * places in that core's own data RAM.
 *
 * The 6.18 pru_rproc copies the table with 16-byte stores, so its size is
 * padded to a multiple of 16: 16 (header) + 4 (offset) + 48 (trace) = 68,
 * plus 12 bytes of padding is 80.
 */
#ifndef AI64_RSC_TRACE_H_
#define AI64_RSC_TRACE_H_

#include <stddef.h>
#include <stdint.h>
#include <rsc_types.h>

#pragma DATA_SECTION(ai64_trace_buf, ".ai64_trace")
#pragma RETAIN(ai64_trace_buf)
#pragma DATA_ALIGN(ai64_trace_buf, 8)
char ai64_trace_buf[AI64_TRACE_SIZE] = { 0 };

struct ai64_rsc_table {
	struct resource_table base;
	uint32_t offset[1];
	struct fw_rsc_trace trace;
	uint32_t pad[3];
};

#pragma DATA_SECTION(resourceTable, ".resource_table")
#pragma RETAIN(resourceTable)
#pragma DATA_ALIGN(resourceTable, 8)
struct ai64_rsc_table resourceTable = {
	{ 1, 1, { 0, 0 } },
	{ offsetof(struct ai64_rsc_table, trace) },
	{
		TYPE_TRACE,
		(uint32_t)ai64_trace_buf,
		AI64_TRACE_SIZE,
		0,
		AI64_TRACE_NAME,
	},
	{ 0, 0, 0 },
};

#include "ai64_trace.h"

#endif
