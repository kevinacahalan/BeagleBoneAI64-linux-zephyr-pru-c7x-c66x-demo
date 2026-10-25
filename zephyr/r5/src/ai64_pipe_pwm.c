/* SPDX-License-Identifier: MIT */
/*
 * R5F0_1's stage of the pipeline (ai64_pipe.h), the last: the class the
 * C7x found for each window, on P9.25.
 *
 * The C7x writes its verdict into the result ring in DDR, then rings FIFO
 * 15 of this core's home mailbox (cluster 10, user 1) with the window's
 * number. While Linux asks it to follow, the thread holds P9.25 on the
 * class's frequency and duty (ai64_pwm_class()), changing it only when the
 * class changes, and only to a class two windows in a row agree on. Only
 * one window can fall across a step change, and it can match a third class:
 * half a window of 20 kHz at 90% and half of 10 kHz at 10% have the spectrum
 * of 10 kHz at 90% or 10%, and a DC in between. The window after it is the
 * new step's alone. So P9.25 follows a window later, about 1 ms, and makes
 * no step the input did not. A pwm or steps request from anyone else ends that, until
 * Linux starts the pipeline again. Either way it keeps score: the input
 * steps on the GTC's time, so each window's tag says which class it should
 * have been, unless a step change falls inside it. New weights start the
 * score again.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>

#include "ai64_erpc.h"
#include "ai64_pipe.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define PIPE ((volatile struct ai64_pipe *)AI64_PIPE_ADDR)

/*
 * How far from a step change a window must end to be scored: the input
 * changes at the end of the period nearest the step's time, up to half
 * its longest period (50 us) off, and RTU1's stamp is up to a chunk (8 us)
 * late.
 */
#define MARGIN_GTC (60u * (AI64_GTC_HZ / 1000000u))

static const struct device *const mbox = DEVICE_DT_GET(DT_NODELABEL(ai64_ipc_home1));
static K_SEM_DEFINE(bell, 0, 1);
static atomic_t newest;

static uint32_t gtc_lo(void)
{
	return sys_read32(AI64_GTC_ADDR + 8u);
}

static void rung(const struct device *dev, mbox_channel_id_t channel, void *user_data,
		 struct mbox_msg *msg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(user_data);

	atomic_set(&newest, (atomic_val_t)(*(const uint32_t *)msg->data + 1u));
	k_sem_give(&bell);
}

static void score_reset(volatile struct ai64_pipe_score *sc)
{
	uint32_t i, j;

	sc->magic = 0;
	sc->right = 0;
	sc->wrong = 0;
	sc->mixed = 0;
	sc->waits = 0;
	sc->changes = 0;
	for (i = 0; i < AI64_PIPE_CLASSES; i++) {
		for (j = 0; j < AI64_PIPE_CLASSES; j++) {
			sc->confusion[i][j] = 0;
		}
	}
	sc->magic = AI64_PIPE_MAGIC;
}

static void stage_reset(volatile struct ai64_pipe_stage *st)
{
	st->magic = 0;
	st->windows = 0;
	st->dropped = 0;
	st->last = 0;
	st->lat_sum = 0;
	st->lat_min = UINT32_MAX;
	st->lat_max = 0;
	st->lat_last = 0;
	st->work_sum = 0;
	st->work_max = 0;
	st->errors = 0;
	st->magic = AI64_PIPE_MAGIC;
}

static void pipe_thread(void *a, void *b, void *c)
{
	volatile struct ai64_pipe_stage *st = &PIPE->stage[AI64_PIPE_PWM];
	volatile struct ai64_pipe_score *sc = &PIPE->score;
	uint32_t w, last = UINT32_MAX, weights = PIPE->ctl.weights_seq, t0, t1, lat;
	/* P9.25 is on a class this set; let go of it (someone else set it) until a new start. */
	bool following = false, let_go = false, agreed;
	uint32_t last_cls = UINT32_MAX;
	int32_t truth;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	stage_reset(st);
	score_reset(sc);
	sc->cls = UINT32_MAX;
	ret = mbox_register_callback(mbox, AI64_PIPE_FIFO, rung, NULL);
	ret = ret ? ret : mbox_set_enabled(mbox, AI64_PIPE_FIFO, true);
	if (ret) {
		LOG_ERR("pipe: mailbox FIFO %u: %d", AI64_PIPE_FIFO, ret);
		return;
	}
	LOG_INF("pipe: P9.25 from the C7x's classes, on its doorbell");

	for (;;) {
		volatile struct ai64_pipe_result *r;
		uint32_t cls;
		uint64_t t_end;

		k_sem_take(&bell, K_FOREVER);
		t0 = gtc_lo();
		w = (uint32_t)atomic_get(&newest) - 1u;
		r = &PIPE->result[w % AI64_PIPE_RESULTS];
		if (r->win != w) {
			st->errors++;
			continue;
		}
		cls = r->cls;
		agreed = w == last + 1u && cls == last_cls;
		last_cls = cls;
		t_end = ((uint64_t)r->t_end_hi << 32) | r->t_end_lo;
		if (last != UINT32_MAX && w - last > 1u && w - last < 0x80000000u) {
			st->dropped += w - last - 1u;
		}
		last = w;

		if (PIPE->ctl.weights_seq != weights) {
			weights = PIPE->ctl.weights_seq;
			score_reset(sc);
			stage_reset(st);
			let_go = false;
		}
		if (following && !ai64_pwm_following()) {
			LOG_INF("pipe: P9.25 was set by hand; the pipeline lets go of it");
			following = false;
			let_go = true;
			sc->cls = UINT32_MAX;
		}
		if (PIPE->ctl.magic == AI64_PIPE_MAGIC && PIPE->ctl.follow && !let_go) {
			if (cls == sc->cls) {
				/* Nothing to do. */
			} else if (!agreed) {
				sc->waits++;
			} else if (cls < AI64_PIPE_CLASSES) {
				if (ai64_pwm_class(cls) == 0) {
					following = true;
					sc->cls = cls;
					sc->changes++;
				} else {
					st->errors++;
				}
			}
		} else if (following) {
			/* Linux stopped it: back to the program, in step with the others. */
			following = false;
			sc->cls = UINT32_MAX;
			(void)ai64_pwm_steps("the pipeline", AI64_PWM_DEFAULT_STEPS_MS);
		} else if (!PIPE->ctl.follow) {
			let_go = false;
		}
		t1 = gtc_lo();

		truth = ai64_pipe_truth(t_end, PIPE->ctl.steps_ms, MARGIN_GTC);
		if (truth < 0 || cls >= AI64_PIPE_CLASSES) {
			sc->mixed++;
		} else {
			if ((uint32_t)truth == cls) {
				sc->right++;
			} else {
				sc->wrong++;
			}
			sc->confusion[truth][cls]++;
		}

		lat = t1 - (uint32_t)t_end;
		st->windows++;
		st->last = w;
		st->lat_last = lat;
		st->lat_sum += lat;
		if (lat < st->lat_min) {
			st->lat_min = lat;
		}
		if (lat > st->lat_max) {
			st->lat_max = lat;
		}
		st->work_sum += t1 - t0;
		if (t1 - t0 > st->work_max) {
			st->work_max = t1 - t0;
		}
	}
}

K_THREAD_DEFINE(ai64_pipe_tid, 1024, pipe_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, 0);
