/* SPDX-License-Identifier: MIT */
/*
 * tm_clear(): the first thing each analyzer core does. A core's task
 * manager keeps its setup when the core stops, so another firmware set's
 * (the multicore scheduler in extra-demos/) would still be armed: task
 * swaps off, no event mapped to a task, the task manager off.
 *
 * A core stopped in the middle of a task stays in it, and its next yield
 * would return to where the old firmware was interrupted. So ai64-demo
 * runs ai64 la quiet before it stops ICSSG1's cores: with IEP0 stopped and
 * no event mapped to a task, each core finishes its task and stops outside
 * one.
 */
#ifndef LA_TM_H_
#define LA_TM_H_

#include <stdint.h>
#include "icssg.h"

static inline void tm_clear(uint32_t tm)
{
	__asm("	TSEN 0");
	ICSSG_REG(TM_TS2_GEN_CFG1(tm)) = 0;
	ICSSG_REG(TM_GLOBAL_CFG(tm)) = 0;
}

#endif
