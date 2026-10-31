/* SPDX-License-Identifier: MIT */
/*
 * This core's fabric node (zephyr/include/ai64_fabric.h). One thread at the
 * lowest priority polls the inbox and yields after each pass, so it only
 * gets the time the eRPC and RPMsg threads leave, and answers within a few
 * microseconds when they are idle. R5F0_0 also sends the tours: its
 * CMakeLists.txt defines AI64_FAB_TOURS.
 *
 * The MPU maps the fabric non-cacheable (mpu.c), and the GTC, the
 * mailboxes, and the spinlocks are in the low 2 GiB it maps as device
 * memory.
 *
 * A route's action goes to the core's fab_action hook (ai64_r5.h), and its
 * fab_tick hook runs every 10 ms: on R5F0_1, a pwm or steps action sets
 * P9.25, and the tick publishes the PWM's status in this node's page.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "ai64_r5.h"
#include "ai64_shm.h"

struct ai64_fab_msg;
static uint32_t fab_action(struct ai64_fab_msg *m);

#define AI64_FAB_NOW()    sys_read32(AI64_GTC_ADDR + 8u)
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_FENCE()  barrier_dmem_fence_full()
/* The RPMsg thread parks the core for a shutdown; not while the lock is held. */
#define AI64_FAB_LOCK_ENTER() k_sched_lock()
#define AI64_FAB_LOCK_EXIT()  k_sched_unlock()
/* The fabric is normal memory here, so memcpy can move it in bursts. */
#define AI64_FAB_COPY_IN(d, s, n)  memcpy((d), (const void *)(s), 4u * (n))
#define AI64_FAB_COPY_OUT(d, s, n) memcpy((void *)(d), (s), 4u * (n))
#define AI64_FAB_ACTION(m) fab_action(m)
#include "ai64_fabric.h"

LOG_MODULE_REGISTER(ai64_fabric, LOG_LEVEL_INF);

BUILD_ASSERT(AI64_FAB_NODE >= AI64_FAB_R5F0_0 && AI64_FAB_NODE <= AI64_FAB_R5F1_1,
	     "an R5F is fabric node 1 to 4");

/* A route's action for this node: 1, not here, unless the core takes it. */
static uint32_t fab_action(struct ai64_fab_msg *m)
{
	if (!ai64_r5_core.fab_action) {
		return 1u;
	}
	return ai64_r5_core.fab_action(m->action, m->arg0, m->arg1);
}

#define STATUS_TICKS AI64_FAB_US(10000)

static void fabric_thread(void *a, void *b, void *c)
{
	uint32_t status_at;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	fab_init(AI64_FAB_NODE);
	LOG_INF("fabric node %u up, start %u", AI64_FAB_NODE,
		FAB_PAGE(AI64_FAB_NODE)->own.boot);
	status_at = AI64_FAB_NOW();
	for (;;) {
		fab_poll();
		if (ai64_r5_core.fab_tick && FAB_AFTER(AI64_FAB_NOW(), status_at)) {
			status_at = AI64_FAB_NOW() + STATUS_TICKS;
			ai64_r5_core.fab_tick(FAB_PAGE(AI64_FAB_NODE)->app, status_at);
		}
		k_yield();
	}
}

K_THREAD_DEFINE(fabric_tid, 2048, fabric_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
