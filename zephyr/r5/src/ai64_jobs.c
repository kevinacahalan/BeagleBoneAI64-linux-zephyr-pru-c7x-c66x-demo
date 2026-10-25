/* SPDX-License-Identifier: MIT */
/*
 * What this core does for a request from another R5F core (ai64_ipc.c).
 * The core whose job it is does it; the others answer -ENOTSUP, and the
 * caller forwards to the right one (ai64_service.c) in the first place.
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai64_erpc.h"
#include "ai64_ipc.h"

void ai64_job_handle(unsigned int from, const struct ai64_ipc_msg *req, struct ai64_ipc_msg *rep)
{
	char who[24];

	snprintf(who, sizeof(who), "%s", ai64_core_name[from]);
	rep->ret = -ENOTSUP;
	switch (req->op) {
	case AI64_IPC_STATUS:
		ai64_job_status(rep->text, sizeof(rep->text));
		rep->ret = 0;
		break;
#if AI64_HAS_SPI7
	case AI64_IPC_SPI: {
		size_t n = strnlen(req->text, sizeof(req->text) - 1u);

		rep->ret = ai64_spi7_write(req->text, n);
		rep->arg0 = rep->ret ? 0u : n;
		break;
	}
#endif
#if AI64_HAS_PWM
	case AI64_IPC_PWM:
		rep->ret = ai64_pwm_set(who, req->arg0, req->arg1);
		break;
	case AI64_IPC_STEPS:
		rep->ret = ai64_pwm_steps(who, req->arg0);
		break;
#endif
	default:
		break;
	}
}
