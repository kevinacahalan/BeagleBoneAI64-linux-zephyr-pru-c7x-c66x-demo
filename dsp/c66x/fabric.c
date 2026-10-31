/* SPDX-License-Identifier: MIT */
/*
 * The C66x's fabric node (ai64_fabric.h): it answers and sends pings,
 * passes tours and routes on, and adds to the spinlock tally.
 *
 * The C66x cannot reach the GTC at its own address: everything below
 * 0x0c000000 is the core's internal space. MPAX segment 2 maps one logical
 * page, AI64_C66X_GTC, onto it. The XMC's segment registers are pairs,
 * XMPAXL (the replacement address) then XMPAXH (the logical base and
 * size). The MAR registers of that page and of the fabric's 16 MiB reset
 * with XMC prefetch (PFX) on. Clearing them makes every read go to the GTC
 * or to DDR. The pipeline writes the C7x's L2 SRAM, which gets the same.
 */

#include <stdint.h>
#include <c6x.h>
#include <string.h>

#include "ai64_pipe.h"
#include "image.h"

#define AI64_C66X_GTC 0x7ffff000u
#define XMPAXL(n)     (*(volatile uint32_t *)(0x08000000u + 8u * (n)))
#define XMPAXH(n)     (*(volatile uint32_t *)(0x08000004u + 8u * (n)))
#define MPAX_4K       0x0bu         /* segment size 2^(11 + 1) */
#define MPAX_RWX      0xbfu         /* supervisor and user read, write, execute */
#define MAR(addr)     (*(volatile uint32_t *)(0x01848000u + 4u * ((addr) >> 24)))

#define AI64_FAB_NOW()    (*(volatile uint32_t *)(AI64_C66X_GTC + 8u))
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_FENCE()  _mfence()
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
	MAR(AI64_C66X_GTC) = 0;
	MAR(AI64_FAB_ADDR) = 0;
	MAR(AI64_PIPE_C7X_ADDR) = 0;
	XMPAXL(2) = ((AI64_GTC_ADDR >> 12) << 8) | MPAX_RWX;
	XMPAXH(2) = AI64_C66X_GTC | MPAX_4K;
	_mfence();
	fab_init(node);
}

void fabric_poll(void)
{
	fab_poll();
}
