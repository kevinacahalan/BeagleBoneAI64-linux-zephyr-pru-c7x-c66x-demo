/* SPDX-License-Identifier: MIT */
/*
 * Functions Linux calls through Zephyr eRPC, on every core.
 *
 * spiWrite writes SPI7, R5F0_0's job (ai64_spi.c); setPwm and setPwmSteps
 * set P9.25, R5F0_1's job (ai64_pwm.c). A core that gets one of these and
 * does not have the job passes it to the core that does, over their
 * ipc_service link (ai64_ipc.c), and returns that core's answer. status
 * returns this core's job status; on R5F1_1, the monitor, every core's.
 * takeChunk checks a bulk buffer Linux already wrote, on R5F0_0 only.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai64_erpc.h"
#include "ai64_ipc.h"
#include "ai64_shm.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#if AI64_HAS_BULK
static volatile struct ai64_bulk *const bulk =
	(volatile struct ai64_bulk *)AI64_BULK_ADDR;
#endif

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
	if (AI64_HAS_SPI7) {
		ret = ai64_spi7_write(text, n);
		if (ret) {
			snprintf(out, 96, WHO_PREFIX "spi write failed (%d)", ret);
		} else {
			snprintf(out, 96, WHO_PREFIX "spi wrote %u bytes", (unsigned int)n);
		}
		LOG_INF("%s", out);
		return out;
	}
	memcpy(m.text, text, n);
	ret = ai64_ipc_call(AI64_CORE_SPI, &m, K_MSEC(500));
	if (ret == 0) {
		ret = m.ret;
	}
	if (ret) {
		snprintf(out, 96, WHO_PREFIX "passed spi to R5F0_0, which failed (%d)", ret);
	} else {
		snprintf(out, 96, WHO_PREFIX "passed spi to R5F0_0, which wrote %u bytes (%u us)",
			 m.arg0, fwd_us(m.sent));
	}
	LOG_INF("%s", out);
	return out;
}

/* Pass a PWM request to R5F0_1, and log the answer. */
static int32_t pwm_forward(struct ai64_ipc_msg *m, const char *what)
{
	int ret = ai64_ipc_call(AI64_CORE_PWM, m, K_MSEC(500));

	if (ret == 0) {
		ret = m->ret;
	}
	LOG_INF("erpc %s: passed to R5F0_1, %s (%u us)", what, ret ? "failed" : "done",
		fwd_us(m->sent));
	return ret ? -1 : 0;
}

int32_t setPwm(uint32_t period_ns, uint32_t duty_ns)
{
	struct ai64_ipc_msg m = {.op = AI64_IPC_PWM, .arg0 = period_ns, .arg1 = duty_ns};

	if (AI64_HAS_PWM) {
		return ai64_pwm_set("Linux", period_ns, duty_ns) ? -1 : 0;
	}
	return pwm_forward(&m, "pwm");
}

int32_t setPwmSteps(uint32_t step_ms)
{
	struct ai64_ipc_msg m = {.op = AI64_IPC_STEPS, .arg0 = step_ms};

	if (AI64_HAS_PWM) {
		return ai64_pwm_steps("Linux", step_ms) ? -1 : 0;
	}
	return pwm_forward(&m, "pwm steps");
}

char *status(void)
{
	size_t len = IS_ENABLED(CONFIG_AI64_JOB_MONITOR) ? 640u : 200u;
	char *out = k_malloc(len);
	size_t n;

	if (!out) {
		return NULL;
	}
	n = snprintf(out, len, "Zephyr on " AI64_CORE_NAME ": ");
#if CONFIG_AI64_JOB_MONITOR
	(void)ai64_monitor_table(out + n, len - n);
#else
	ai64_job_status(out + n, len - n);
#endif
	return out;
}

uint32_t takeChunk(uint32_t seq, uint32_t length, uint32_t crc)
{
#if !AI64_HAS_BULK
	ARG_UNUSED(seq);
	ARG_UNUSED(length);
	ARG_UNUSED(crc);
	LOG_INF("erpc chunk: no bulk buffer on " AI64_CORE);
	return 0;
#else
	uint32_t got;
	uint32_t magic;
	uint32_t got_seq;
	uint32_t got_len;
	uint32_t got_crc;

	if (length > sizeof(bulk->data)) {
		return 0;
	}
	sys_cache_data_invd_range((void *)bulk, 16u + length);
	magic = bulk->magic;
	got_seq = bulk->seq;
	got_len = bulk->length;
	got_crc = bulk->crc;
	if (magic != AI64_BULK_MAGIC || got_seq != seq || got_len != length ||
	    got_crc != crc) {
		LOG_INF("chunk mismatch magic %08x seq %u len %u crc %08x",
			magic, got_seq, got_len, got_crc);
		return 0;
	}
	got = ai64_crc32((const uint8_t *)bulk->data, length);
	LOG_INF("chunk seq %u len %u crc %08x", seq, length, got);
	return got;
#endif
}
