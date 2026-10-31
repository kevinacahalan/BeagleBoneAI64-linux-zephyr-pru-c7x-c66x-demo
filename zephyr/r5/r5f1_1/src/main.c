/* SPDX-License-Identifier: MIT */
/*
 * R5F1_1: the monitor (monitor.c). It pings the other three cores over
 * their ipc_service links, collects their status lines, and answers
 * Linux's status call with the table.
 *
 * Like every R5F core it also runs ../common/: RPMsg and eRPC to Linux,
 * ipc_service links to the other three, and its fabric node. ai64_r5_core
 * below is what this core adds to that, and start() starts its own job.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "ai64_r5.h"
#include "r5f1_1.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

const struct ai64_r5_core ai64_r5_core = {
	.status = monitor_status,
	.status_all = monitor_table,
};

/* At the APPLICATION init level: after the drivers, before main() and the
 * common threads. */
static int start(void)
{
	monitor_start();
	return 0;
}

SYS_INIT(start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int main(void)
{
	LOG_INF("ai64 " AI64_CORE " hello");
	return 0;
}
