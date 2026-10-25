/* SPDX-License-Identifier: MIT */
/*
 * R5F0_0's job: SPI7 on header P9 (P9.31 CLK, P9.28 CS0, P9.30 MOSI), 1 MHz,
 * mode 0, MSB first, through Zephyr's McSPI driver. It writes "hello" at
 * boot, and then what Linux (spiWrite) or another core (an AI64_IPC_SPI
 * request) asks for.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>

#include "ai64_erpc.h"
#include "ai64_ipc.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(main_spi7));

static K_MUTEX_DEFINE(lock);
static uint32_t transfers, bytes;

int ai64_spi7_write(const void *buf, size_t len)
{
	static const struct spi_config cfg = {
		.frequency = 1000000,
		.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
		.peripheral = 0,
	};
	const struct spi_buf txb = {
		.buf = (void *)buf,
		.len = len,
	};
	const struct spi_buf_set tx = {
		.buffers = &txb,
		.count = 1,
	};
	int ret;

	if (!buf || !len) {
		return -EINVAL;
	}
	if (!device_is_ready(spi)) {
		LOG_ERR("spi7 not ready");
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	ret = spi_write(spi, &cfg, &tx);
	if (ret == 0) {
		transfers++;
		bytes += len;
	}
	k_mutex_unlock(&lock);
	return ret;
}

void ai64_job_status(char *buf, size_t len)
{
	k_mutex_lock(&lock, K_FOREVER);
	snprintf(buf, len, "SPI7: %u writes, %u bytes", transfers, bytes);
	k_mutex_unlock(&lock);
}

static int spi_hello(void)
{
	static const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
	int ret = ai64_spi7_write(hello, sizeof(hello));

	if (ret) {
		LOG_ERR("spi7 hello failed: %d", ret);
		return 0;
	}
	LOG_INF("spi7 hello wrote %u bytes", (unsigned int)sizeof(hello));
	return 0;
}

SYS_INIT(spi_hello, APPLICATION, 0);
