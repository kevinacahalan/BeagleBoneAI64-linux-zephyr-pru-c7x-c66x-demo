/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/
 *
 * What every core's main.c of the multicore scheduler shares: the task
 * manager registers, and the step every non-PRU0 core runs before it
 * idles. Ported from open-pru examples/multicore_scheduler, where each
 * core's main.c spells this out.
 */
#ifndef MCS_TASKS_H_
#define MCS_TASKS_H_

#include <stdint.h>
#include "icssg.h"

#define HW_WR_REG32(addr, v) (ICSSG_REG(addr) = (uint32_t)(v))
#define TASKS_MGR_GLOBAL_CFG 0x00
#define TASKS_MGR_TS2_PC_S0  0x1C

#endif
