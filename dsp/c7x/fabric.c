/* SPDX-License-Identifier: MIT */
/*
 * The C7x's fabric node (ai64_fabric.h): it answers and sends pings,
 * passes tours and routes on, and adds to the spinlock tally. The MMU
 * (mmu.c) maps the GTC, the mailboxes, and the spinlocks as device memory,
 * and the fabric normal non-cacheable.
 */

#include <stdint.h>
#include <c7x.h>
#include <string.h>

#include "image.h"

#define AI64_FAB_NOW()    (*(volatile uint32_t *)(uintptr_t)(AI64_GTC_ADDR + 8u))
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_FENCE()  __memory_fence(0x1)
/* The fabric is normal memory here, so memcpy can move it in bursts. */
#define AI64_FAB_COPY_IN(d, s, n)  memcpy((d), (const void *)(s), 4u * (n))
#define AI64_FAB_COPY_OUT(d, s, n) memcpy((void *)(d), (s), 4u * (n))
#include "ai64_fabric.h"

uint32_t gtc_lo(void)
{
	return AI64_FAB_NOW();
}

void fabric_start(uint32_t node)
{
	fab_init(node);
}

void fabric_poll(void)
{
	fab_poll();
}
