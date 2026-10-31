/* SPDX-License-Identifier: MIT */
#ifndef AI64_R5_H_
#define AI64_R5_H_

#include <stddef.h>
#include <stdint.h>

#include "ai64_ipc.h"

/*
 * What a core adds to the common code (zephyr/r5/common/). Every core runs
 * the common part: the RPMsg and eRPC link to Linux (rpmsg.c, erpc.cpp,
 * linux_calls.c), the links to the other R5F cores (ipc.c), its fabric
 * node (fabric.c), and its boot-time setup (mpu.c, restart.c, tisci.c,
 * sproxy.c). Its own job comes from its own directory, r5f0_0/ to r5f1_1/,
 * whose main.c defines ai64_r5_core and starts the rest.
 *
 * AI64_CORE ("r5f0_1"), AI64_CORE_NAME ("R5F0_1"), AI64_SELF (0 to 3), and
 * AI64_FAB_NODE (1 to 4) come from common.cmake.
 */
struct ai64_r5_core {
	/* One line on how its job is going: the other cores' STATUS
	 * request, and Linux's status call. */
	void (*status)(char *buf, size_t len);
	/* Linux's status call, if it says more: R5F1_1's table of every
	 * core. len is AI64_R5_STATUS_ALL. NULL: status. */
	void (*status_all)(char *buf, size_t len);
	/* A request for this core's job, AI64_IPC_SPI, _PWM, or _STEPS, from
	 * another core or from Linux on this one. who names the asker in the
	 * log. Sets rep->ret, 0 or a negative errno. NULL: none. */
	void (*request)(const char *who, const struct ai64_ipc_msg *req,
			struct ai64_ipc_msg *rep);
	/* A fabric route's action at this node (ai64_fabric.h): returns 0
	 * done, 1 not here, 2 failed. NULL: none. */
	uint32_t (*fab_action)(uint32_t action, uint32_t arg0, uint32_t arg1);
	/* Every 10 ms, from the fabric thread: page is this node's app area
	 * of its fabric page, now the GTC low word. NULL: none. */
	void (*fab_tick)(volatile void *page, uint32_t now);
	/* Linux's takeChunk and dmaCopy calls (linux_calls.c). NULL: they
	 * answer that this core has neither. */
	uint32_t (*chunk)(uint32_t seq, uint32_t length, uint32_t crc);
	int32_t (*dma)(uint32_t dst, uint32_t src, uint32_t length, uint32_t *cpu_ns);
	/* Before the core parks for Linux to stop it (rpmsg.c). NULL: none. */
	void (*shutdown)(void);
};

/* Each core's main.c defines it. */
extern const struct ai64_r5_core ai64_r5_core;

#define AI64_R5_STATUS_ALL 640u

#endif
