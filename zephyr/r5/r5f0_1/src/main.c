/* SPDX-License-Identifier: MIT */
/*
 * R5F0_1: P9.25, EHRPWM4 output B (pwm.c), which Linux, the other cores,
 * and fabric routes set; and the pipeline's last stage (pipe_pwm.c), which
 * sets P9.25 to the class the C7x finds for each window.
 *
 * Like every R5F core it also runs ../common/: RPMsg and eRPC to Linux,
 * ipc_service links to the other three, and its fabric node. ai64_r5_core
 * below is what this core adds to that, and start() starts its own job.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "ai64_r5.h"
#include "r5f0_1.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

const struct ai64_r5_core ai64_r5_core = {
	.status = p925_status,
	.request = p925_request,
	.fab_action = p925_fab_action,
	.fab_tick = p925_publish,
};

/* At the APPLICATION init level: after the drivers, before main() and the
 * common threads. */
static int start(void)
{
	p925_start();
	pipe_pwm_start();
	return 0;
}

SYS_INIT(start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int main(void)
{
	LOG_INF("ai64 " AI64_CORE " hello");
	return 0;
}
