/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Resource table for an RTU with RPMsg: the RPMsg vdev, its two vrings, and
 * a trace buffer. Used by ICSSG0 RTU0 and ICSSG1 RTU0. Define
 * AI64_TRACE_SIZE and AI64_TRACE_NAME first.
 * Adapted from TI PSSP examples/j721e/RTU_RPMsg_Echo_Interrupt0.
 *
 * Copyright (C) 2015-2021 Texas Instruments Incorporated - https://www.ti.com/
 *
 * The 6.18 pru_rproc copies the table with 16-byte stores, so its size is
 * padded to a multiple of 16: 16 (header) + 8 (offsets) + 68 (vdev and two
 * vrings) + 48 (trace) = 140, plus 4 bytes of padding is 144.
 */
#ifndef AI64_RSC_RPMSG_H_
#define AI64_RSC_RPMSG_H_

#include <stddef.h>
#include <stdint.h>
#include <rsc_types.h>
#include <pru_virtio_ids.h>

#define PRU_RPMSG_VQ0_SIZE 16
#define PRU_RPMSG_VQ1_SIZE 16

#define VIRTIO_RPMSG_F_NS 0
#define RPMSG_PRU_C0_FEATURES (1 << VIRTIO_RPMSG_F_NS)

#pragma DATA_SECTION(ai64_trace_buf, ".ai64_trace")
#pragma RETAIN(ai64_trace_buf)
#pragma DATA_ALIGN(ai64_trace_buf, 8)
char ai64_trace_buf[AI64_TRACE_SIZE] = { 0 };

struct my_resource_table {
	struct resource_table base;
	uint32_t offset[2];
	struct fw_rsc_vdev rpmsg_vdev;
	struct fw_rsc_vdev_vring rpmsg_vring0;
	struct fw_rsc_vdev_vring rpmsg_vring1;
	struct fw_rsc_trace trace;
	uint32_t pad[1];
};

#pragma DATA_SECTION(resourceTable, ".resource_table")
#pragma RETAIN(resourceTable)
#pragma DATA_ALIGN(resourceTable, 8)
struct my_resource_table resourceTable = {
	{ 1, 2, { 0, 0 } },
	{
		offsetof(struct my_resource_table, rpmsg_vdev),
		offsetof(struct my_resource_table, trace),
	},
	{
		(uint32_t)TYPE_VDEV,
		(uint32_t)VIRTIO_ID_RPMSG,
		(uint32_t)0,
		(uint32_t)RPMSG_PRU_C0_FEATURES,
		(uint32_t)0,
		(uint32_t)0,
		(uint8_t)0,
		(uint8_t)2,
		{ (uint8_t)0, (uint8_t)0 },
	},
	{ FW_RSC_ADDR_ANY, 16, PRU_RPMSG_VQ0_SIZE, 0, 0 },
	{ FW_RSC_ADDR_ANY, 16, PRU_RPMSG_VQ1_SIZE, 0, 0 },
	{
		TYPE_TRACE,
		(uint32_t)ai64_trace_buf,
		AI64_TRACE_SIZE,
		0,
		AI64_TRACE_NAME,
	},
	{ 0 },
};

/*
 * The INTC map, in the RTU's own ICSSG. Linux's kick raises system event 21,
 * which this maps to channel 10 and host interrupt 10, R31 bit 30 on a
 * slice-0 RTU. The return path, system event 20, is mapped by the device
 * tree overlay (<20 4 4>), not here.
 */
#pragma DATA_SECTION(my_irq_rsc, ".pru_irq_map")
#pragma RETAIN(my_irq_rsc)
struct pru_irq_rsc my_irq_rsc = {
	0,
	1,
	{
		{ 21, 10, 10 },
	},
};

#include "ai64_trace.h"

#endif
