/* SPDX-License-Identifier: MIT */
/*
 * Functions Linux calls through Zephyr eRPC.
 * armPulses and sendXy hand a command to RTU0 through the small mailbox.
 * takeChunk checks a bulk buffer Linux already wrote.
 * Only R5F0_0 has the mailbox, the bulk buffer, and SPI7. On the other cores
 * those calls fail, and say still answers.
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai64_erpc.h"
#include "ai64_shm.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#if AI64_HAS_SHM
static volatile struct ai64_bulk *const bulk =
	(volatile struct ai64_bulk *)AI64_BULK_ADDR;
#endif

#define REPLY_PREFIX "zephyr " AI64_CORE ": "

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

char *spiWrite(const char *text)
{
	static const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
	uint8_t buf[256];
	const uint8_t *src;
	size_t n;
	int ret;
	char *out;

	if (!text || !text[0]) {
		src = hello;
		n = sizeof(hello);
	} else {
		n = strlen(text);
		if (n > sizeof(buf)) {
			n = sizeof(buf);
		}
		memcpy(buf, text, n);
		src = buf;
	}
	ret = ai64_spi7_write(src, n);
	out = k_malloc(64);
	if (!out) {
		return NULL;
	}
	if (ret) {
		snprintf(out, 64, REPLY_PREFIX "spi write failed (%d)", ret);
		LOG_ERR("erpc spi fail %d", ret);
		return out;
	}
	snprintf(out, 64, REPLY_PREFIX "spi wrote %u bytes", (unsigned int)n);
	LOG_INF("%s", out);
	return out;
}

int32_t armPulses(uint32_t period_cycles, uint32_t periods)
{
	char cmd[56];

	if (!AI64_HAS_SHM) {
		LOG_INF("erpc arm pulses: no mailbox on " AI64_CORE);
		return -1;
	}
	snprintf(cmd, sizeof(cmd), "arm %u %u", period_cycles, periods);
	ai64_shm_publish(cmd);
	LOG_INF("erpc arm pulses period %u count %u", period_cycles, periods);
	return 0;
}

int32_t sendXy(uint32_t x, uint32_t y)
{
	char cmd[56];

	if (!AI64_HAS_SHM) {
		LOG_INF("erpc xy: no mailbox on " AI64_CORE);
		return -1;
	}
	snprintf(cmd, sizeof(cmd), "xy %u %u", x, y);
	ai64_shm_publish(cmd);
	LOG_INF("erpc xy x %u y %u", x, y);
	return 0;
}

uint32_t takeChunk(uint32_t seq, uint32_t length, uint32_t crc)
{
#if !AI64_HAS_SHM
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
