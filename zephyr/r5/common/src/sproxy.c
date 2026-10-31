/* SPDX-License-Identifier: MIT */
/*
 * The main secure proxy as a Zephyr mailbox, polled, so that Zephyr's
 * TI-SCI driver (drivers/firmware/tisci) and the drivers built on it work
 * on these R5Fs: TI-SCI clock control, TI-SCI power domains, and through
 * them the EHRPWM driver (r5f0_1/src/pwm.c).
 *
 * Zephyr's ti,secure-proxy driver needs a receive interrupt for each
 * thread and handles threads 0-31. On J721E nothing routes the main secure
 * proxy's interrupts to a main-domain R5F, and these cores' threads are
 * past 31 (61 and 63 for R5F0_0). So this driver has no interrupt: a send
 * waits for the answer on the enabled receive threads, and hands it to the
 * receive callback before it returns. The TI-SCI driver only starts to wait
 * for its response semaphore after the send, so it finds it given. It polls
 * a bounded number of times rather than for a time, so it also works
 * before the system timer runs (power domains turn on in PRE_KERNEL_1).
 */

#define DT_DRV_COMPAT ai64_secure_proxy_poll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ai64_sproxy, LOG_LEVEL_INF);

#define THREAD(base, t)  ((base) + 0x1000u * (t))
/* Data words sit at 0x04-0x3c. Writing 0x3c sends; reading it frees the slot. */
#define DATA_FIRST       0x04u
#define DATA_LAST        0x3cu
#define MSG_SIZE         (DATA_LAST - DATA_FIRST + 4u)
#define STATUS_COUNT     0xffu
#define STATUS_ERROR     BIT(31)
#define MAX_CHANNELS     128u
/* Each status read takes a few hundred nanoseconds: about a second. */
#define POLLS            4000000u

/* The head of the TI-SCI driver's receive buffer descriptor. */
struct sproxy_rx_msg {
	uint8_t seq;
	size_t size;
	void *buf;
};

struct sproxy_config {
	uintptr_t data;
	uintptr_t rt;
};

struct sproxy_data {
	mbox_callback_t cb[MAX_CHANNELS];
	void *user_data[MAX_CHANNELS];
	bool enabled[MAX_CHANNELS];
};

/* Take one waiting message off a receive thread. */
static void sproxy_receive(const struct device *dev, uint32_t ch)
{
	const struct sproxy_config *cfg = dev->config;
	struct sproxy_data *data = dev->data;
	struct sproxy_rx_msg *rx = data->user_data[ch];
	uintptr_t base = THREAD(cfg->data, ch);
	uint32_t off;

	for (off = DATA_FIRST; off <= DATA_LAST; off += 4u) {
		uint32_t word = sys_read32(base + off);

		if (rx != NULL && rx->buf != NULL && off - DATA_FIRST + 4u <= rx->size) {
			memcpy((uint8_t *)rx->buf + (off - DATA_FIRST), &word, 4u);
		}
	}
	if (rx != NULL) {
		rx->size = MSG_SIZE;
	}
	if (data->cb[ch] != NULL) {
		data->cb[ch](dev, ch, data->user_data[ch], NULL);
	}
}

static int sproxy_send(const struct device *dev, uint32_t channel, const struct mbox_msg *msg)
{
	const struct sproxy_config *cfg = dev->config;
	struct sproxy_data *data = dev->data;
	uintptr_t base = THREAD(cfg->data, channel);
	const uint8_t *p;
	uint32_t off, i, ch;

	if (channel >= MAX_CHANNELS || msg == NULL || msg->data == NULL || msg->size == 0u) {
		return -EINVAL;
	}
	if (msg->size > MSG_SIZE) {
		return -EMSGSIZE;
	}
	if (sys_read32(THREAD(cfg->rt, channel)) & STATUS_ERROR) {
		LOG_ERR("thread %u is in error", channel);
		return -EIO;
	}
	p = msg->data;
	for (off = DATA_FIRST; off <= DATA_LAST; off += 4u) {
		uint32_t word = 0;
		size_t at = off - DATA_FIRST;

		if (at < msg->size) {
			memcpy(&word, p + at, MIN(4u, msg->size - at));
		}
		/* The last register sends, so every word before it must land first. */
		barrier_dmem_fence_full();
		sys_write32(word, base + off);
	}

	for (i = 0; i < POLLS; i++) {
		for (ch = 0; ch < MAX_CHANNELS; ch++) {
			if (data->enabled[ch] &&
			    (sys_read32(THREAD(cfg->rt, ch)) & STATUS_COUNT) != 0u) {
				sproxy_receive(dev, ch);
				return 0;
			}
		}
	}
	LOG_ERR("no answer to a message on thread %u", channel);
	return -ETIMEDOUT;
}

static int sproxy_register_callback(const struct device *dev, uint32_t channel,
				    mbox_callback_t cb, void *user_data)
{
	struct sproxy_data *data = dev->data;

	if (channel >= MAX_CHANNELS) {
		return -EINVAL;
	}
	data->cb[channel] = cb;
	data->user_data[channel] = user_data;
	return 0;
}

static int sproxy_set_enabled(const struct device *dev, uint32_t channel, bool enable)
{
	const struct sproxy_config *cfg = dev->config;
	struct sproxy_data *data = dev->data;

	if (channel >= MAX_CHANNELS) {
		return -EINVAL;
	}
	/* An answer left from before this boot would be taken for the next one. */
	while (enable && (sys_read32(THREAD(cfg->rt, channel)) & STATUS_COUNT) != 0u) {
		(void)sys_read32(THREAD(cfg->data, channel) + DATA_LAST);
	}
	data->enabled[channel] = enable;
	return 0;
}

static int sproxy_mtu_get(const struct device *dev)
{
	ARG_UNUSED(dev);

	return MSG_SIZE;
}

static uint32_t sproxy_max_channels_get(const struct device *dev)
{
	ARG_UNUSED(dev);

	return MAX_CHANNELS;
}

static DEVICE_API(mbox, sproxy_api) = {
	.send = sproxy_send,
	.register_callback = sproxy_register_callback,
	.mtu_get = sproxy_mtu_get,
	.max_channels_get = sproxy_max_channels_get,
	.set_enabled = sproxy_set_enabled,
};

/* Before the TI-SCI driver, which registers its callback when it starts
 * (prj.conf puts CONFIG_TISCI_INIT_PRIORITY at 10). */
#define SPROXY_INIT_PRIORITY 5
#define SPROXY_DEFINE(n)                                                                           \
	static const struct sproxy_config sproxy_config_##n = {                                    \
		.data = DT_INST_REG_ADDR_BY_NAME(n, target_data),                                  \
		.rt = DT_INST_REG_ADDR_BY_NAME(n, rt),                                             \
	};                                                                                         \
	static struct sproxy_data sproxy_data_##n;                                                 \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &sproxy_data_##n, &sproxy_config_##n, PRE_KERNEL_1,   \
			      SPROXY_INIT_PRIORITY, &sproxy_api);

DT_INST_FOREACH_STATUS_OKAY(SPROXY_DEFINE)
