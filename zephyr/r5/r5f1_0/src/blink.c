/* SPDX-License-Identifier: MIT */
/*
 * R5F1_0's job: a square wave on P8.18, Zephyr's blinky with a logic
 * analyzer in mind.
 *
 * A periodic k_timer toggles GPIO2_4 (the overlay muxes it to P8.18) every
 * CONFIG_AI64_BLINK_HALF_PERIOD_US, from the timer's interrupt, through
 * Zephyr's GPIO driver: 1 kHz with the default 500 us. Each toggle reads the
 * GTC, so the core measures its own timing: how far each half period is
 * from the one asked for. Every 1000 toggles it keeps the smallest, the
 * largest, and the mean of those, for the status line.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>
#include <stdio.h>

#include "ai64_shm.h"
#include "r5f1_0.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define HALF_US      CONFIG_AI64_BLINK_HALF_PERIOD_US
#define HALF_GTC     ((int32_t)(HALF_US * (AI64_GTC_HZ / 1000000u)))
#define WINDOW       1000u
#define GTC_NS(t)    ((int32_t)(t) * (int32_t)(1000000000u / AI64_GTC_HZ))

static const struct gpio_dt_spec pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), blink_gpios);

struct window {
	int32_t min, max;
	int64_t sum;
	uint32_t n;
};

static struct window cur, last;
static uint32_t toggles, prev;
static int32_t worst;
static struct k_spinlock lock;

static void blink(struct k_timer *t)
{
	uint32_t now = sys_read32(AI64_GTC_ADDR + 8u);
	int32_t err;
	k_spinlock_key_t key;

	ARG_UNUSED(t);
	(void)gpio_pin_toggle_dt(&pin);
	key = k_spin_lock(&lock);
	if (toggles++ != 0u) {
		err = (int32_t)(now - prev) - HALF_GTC;
		if (cur.n == 0u || err < cur.min) {
			cur.min = err;
		}
		if (cur.n == 0u || err > cur.max) {
			cur.max = err;
		}
		if ((err < 0 ? -err : err) > worst) {
			worst = err < 0 ? -err : err;
		}
		cur.sum += err;
		if (++cur.n == WINDOW) {
			last = cur;
			cur = (struct window){0};
		}
	}
	prev = now;
	k_spin_unlock(&lock, key);
}

static K_TIMER_DEFINE(timer, blink, NULL);

void blink_status(char *buf, size_t len)
{
	struct window w;
	uint32_t n;
	int32_t wst;
	k_spinlock_key_t key = k_spin_lock(&lock);

	w = last;
	n = toggles;
	wst = worst;
	k_spin_unlock(&lock, key);
	if (w.n == 0u) {
		snprintf(buf, len, "P8.18: %u toggles, every %u us", n, HALF_US);
		return;
	}
	snprintf(buf, len,
		 "P8.18: %u toggles, every %u us; last %u off by %d to %d ns, mean %d; worst %d ns",
		 n, HALF_US, w.n, GTC_NS(w.min), GTC_NS(w.max), GTC_NS((int32_t)(w.sum / w.n)),
		 GTC_NS(wst));
}

void blink_start(void)
{
	int ret;

	if (!gpio_is_ready_dt(&pin)) {
		LOG_ERR("blink: GPIO2 is not ready");
		return;
	}
	ret = gpio_pin_configure_dt(&pin, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("blink: P8.18 configure failed (%d)", ret);
		return;
	}
	k_timer_start(&timer, K_USEC(HALF_US), K_USEC(HALF_US));
	LOG_INF("blink: P8.18 toggles every %u us", HALF_US);
}
