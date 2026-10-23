/* SPDX-License-Identifier: MIT */
/*
 * This core's fabric node (zephyr/include/ai64_fabric.h). One thread at the
 * lowest priority polls the inbox and yields after each pass, so it only
 * gets the time the eRPC and RPMsg threads leave, and answers within a few
 * microseconds when they are idle. R5F0_0 also sends the tours.
 *
 * The MPU maps the fabric non-cacheable (ai64_mpu.c), and the GTC, the
 * mailboxes, and the spinlocks are in the low 2 GiB it maps as device
 * memory.
 *
 * On R5F0_0 a route's pwm or steps action sets P9.25 (ai64_pwm.c), and the thread
 * publishes the PWM's status in this node's page every 10 ms.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "ai64_erpc.h"

struct ai64_fab_msg;
static uint32_t fab_action(struct ai64_fab_msg *m);

#if AI64_FAB_NODE == 1
#define AI64_FAB_TOURS 1
#endif
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

/* A route's action for this node: only R5F0_0's pwm and steps, for P9.25. */
static uint32_t fab_action(struct ai64_fab_msg *m)
{
	if (AI64_HAS_PWM && m->action == AI64_FAB_ACT_PWM) {
		return ai64_pwm_set("A fabric route", m->arg0, m->arg1) ? 2u : 0u;
	}
	if (AI64_HAS_PWM && m->action == AI64_FAB_ACT_STEPS) {
		return ai64_pwm_steps("A fabric route", m->arg0) ? 2u : 0u;
	}
	return 1u;
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
		if (AI64_HAS_PWM && FAB_AFTER(AI64_FAB_NOW(), status_at)) {
			status_at = AI64_FAB_NOW() + STATUS_TICKS;
			ai64_pwm_publish((volatile struct ai64_pwm_status *)FAB_PAGE(AI64_FAB_NODE)->app,
					 status_at);
		}
		k_yield();
	}
}

K_THREAD_DEFINE(fabric_tid, 2048, fabric_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
