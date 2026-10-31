/* SPDX-License-Identifier: MIT */
/*
 * Functions Linux calls through Zephyr eRPC, on every core
 * (zephyr/erpc/ai64.erpc).
 *
 * spiWrite writes SPI7, R5F0_0's job; setPwm and setPwmSteps set P9.25,
 * R5F0_1's job. Each becomes an ai64_ipc_msg request: on the core whose job
 * it is, its own request hook (ai64_r5.h) answers it; any other core passes
 * it to that core over their ipc_service link (ipc.c) and returns its
 * answer. status returns this core's status line; on R5F1_1, the monitor,
 * every core's. takeChunk and dmaCopy are R5F0_0's, the only core with the
 * bulk buffer and a UDMA channel; elsewhere they answer that.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai64_ipc.h"
#include "ai64_r5.h"
#include "ai64_shm.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define WHO_PREFIX   "Zephyr on " AI64_CORE_NAME ": "
#define REPLY_PREFIX "Zephyr on " AI64_CORE_NAME " got: "

char *say(const char *msg)
{
	size_t n;
	char *out;

	if (!msg) {
		msg = "";
	}
	n = strlen(msg);
	if (n > 160) {
		n = 160;
	}
	out = k_malloc(n + 32);
	if (!out) {
		return NULL;
	}
	snprintf(out, n + 32, REPLY_PREFIX "%.*s", (int)n, msg);
	LOG_INF("erpc say: %s", out);
	return out;
}

/* A forwarded call's round trip, from this core's GTC, in microseconds. */
static uint32_t fwd_us(uint32_t sent)
{
	return (sys_read32(AI64_GTC_ADDR + 8u) - sent) / (AI64_GTC_HZ / 1000000u);
}

/* Linux's request m for core: this core's own hook answers it if it is
 * core, else core does. Returns 0 or a negative errno; m holds the reply. */
static int ask(unsigned int core, struct ai64_ipc_msg *m)
{
	struct ai64_ipc_msg rep = {.op = m->op, .ret = -ENOTSUP};
	int ret;

	if (core != AI64_SELF) {
		ret = ai64_ipc_call(core, m, K_MSEC(500));
		return ret ? ret : m->ret;
	}
	if (ai64_r5_core.request) {
		ai64_r5_core.request("Linux", m, &rep);
	}
	*m = rep;
	return rep.ret;
}

char *spiWrite(const char *text)
{
	static const char hello[] = "hello";
	struct ai64_ipc_msg m = {.op = AI64_IPC_SPI};
	size_t n;
	int ret;
	char *out;

	if (!text || !text[0]) {
		text = hello;
	}
	n = strlen(text);
	if (n > AI64_IPC_TEXT - 1u) {
		n = AI64_IPC_TEXT - 1u;
	}
	out = k_malloc(96);
	if (!out) {
		return NULL;
	}
	memcpy(m.text, text, n);
	ret = ask(AI64_CORE_SPI, &m);
	if (AI64_SELF == AI64_CORE_SPI) {
		if (ret) {
			snprintf(out, 96, WHO_PREFIX "spi write failed (%d)", ret);
		} else {
			snprintf(out, 96, WHO_PREFIX "spi wrote %u bytes", (unsigned int)n);
		}
	} else if (ret) {
		snprintf(out, 96, WHO_PREFIX "passed spi to R5F0_0, which failed (%d)", ret);
	} else {
		snprintf(out, 96, WHO_PREFIX "passed spi to R5F0_0, which wrote %u bytes (%u us)",
			 m.arg0, fwd_us(m.sent));
	}
	LOG_INF("%s", out);
	return out;
}

/* A PWM request for R5F0_1: logged here when it was passed on. */
static int32_t pwm_ask(struct ai64_ipc_msg *m, const char *what)
{
	int ret = ask(AI64_CORE_PWM, m);

	if (AI64_SELF != AI64_CORE_PWM) {
		LOG_INF("erpc %s: passed to R5F0_1, %s (%u us)", what, ret ? "failed" : "done",
			fwd_us(m->sent));
	}
	return ret ? -1 : 0;
}

int32_t setPwm(uint32_t period_ns, uint32_t duty_ns)
{
	struct ai64_ipc_msg m = {.op = AI64_IPC_PWM, .arg0 = period_ns, .arg1 = duty_ns};

	return pwm_ask(&m, "pwm");
}

int32_t setPwmSteps(uint32_t step_ms)
{
	struct ai64_ipc_msg m = {.op = AI64_IPC_STEPS, .arg0 = step_ms};

	return pwm_ask(&m, "pwm steps");
}

char *status(void)
{
	size_t len = ai64_r5_core.status_all ? AI64_R5_STATUS_ALL : 200u;
	char *out = k_malloc(len);
	size_t n;

	if (!out) {
		return NULL;
	}
	n = snprintf(out, len, "Zephyr on " AI64_CORE_NAME ": ");
	if (ai64_r5_core.status_all) {
		ai64_r5_core.status_all(out + n, len - n);
	} else {
		ai64_r5_core.status(out + n, len - n);
	}
	return out;
}

uint32_t takeChunk(uint32_t seq, uint32_t length, uint32_t crc)
{
	if (!ai64_r5_core.chunk) {
		LOG_INF("erpc chunk: no bulk buffer on " AI64_CORE);
		return 0;
	}
	return ai64_r5_core.chunk(seq, length, crc);
}

int32_t dmaCopy(uint32_t dst, uint32_t src, uint32_t length, uint32_t *cpuNs)
{
	*cpuNs = 0;
	if (!ai64_r5_core.dma) {
		return -ENOTSUP;
	}
	return ai64_r5_core.dma(dst, src, length, cpuNs);
}
