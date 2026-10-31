/* SPDX-License-Identifier: MIT */
/*
 * R5F0_1's stage of the pipeline (ai64_pipe.h), the last: the class the
 * C7x found for each window, on P9.25.
 *
 * The C7x writes its verdict into the result ring in DDR, then rings FIFO
 * 15 of this core's home mailbox (cluster 10, user 1) with the window's
 * number; the thread takes every window from the last it took to that
 * one, in order. While Linux asks it to follow, it holds P9.25 on the
 * class's frequency and duty (p925_class()), changing it only when the
 * class changes, and only to a class two windows in a row agree on. A
 * window that falls across a step change can match a third class: half a
 * window of 20 kHz at 90% and half of 10 kHz at 10% have the spectrum of
 * 10 kHz at 90% or 10%, and a DC in between. The windows overlap by half,
 * so two fall across each change, but one is mostly the old step and the
 * other mostly the new, and they do not agree. So P9.25 makes no step the
 * input did not. A pwm or steps request from anyone else ends that, until
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

#include "ai64_pipe.h"
#include "r5f0_1.h"

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

/* P9.25 is on a class this set; let go of it (someone else set it) until a new start. */
static bool following, let_go;
static uint32_t last_cls = UINT32_MAX, weights;

/* Window w, whose verdict the C7x has written; agreed: the window before had its class. */
static void one(volatile struct ai64_pipe_stage *st, volatile struct ai64_pipe_score *sc,
		uint32_t w, bool in_order)
{
	volatile struct ai64_pipe_result *r = &PIPE->result[w % AI64_PIPE_RESULTS];
	uint32_t t0 = gtc_lo(), t1, cls;
	uint64_t t_end;
	int32_t truth;
	bool agreed;

	if (r->win != w) {
		st->errors++;
		return;
	}
	cls = r->cls;
	agreed = in_order && cls == last_cls;
	last_cls = cls;
	t_end = ((uint64_t)r->t_end_hi << 32) | r->t_end_lo;

	if (PIPE->ctl.weights_seq != weights) {
		weights = PIPE->ctl.weights_seq;
		score_reset(sc);
		ai64_pipe_stage_reset(st);
		let_go = false;
	}
	if (following && !p925_following()) {
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
			if (p925_class(cls) == 0) {
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
		(void)p925_steps("the pipeline", AI64_PWM_DEFAULT_STEPS_MS);
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
	ai64_pipe_stage_count(st, w, t1 - (uint32_t)t_end, t1 - t0);
}

static void pipe_thread(void *a, void *b, void *c)
{
	volatile struct ai64_pipe_stage *st = &PIPE->stage[AI64_PIPE_PWM];
	volatile struct ai64_pipe_score *sc = &PIPE->score;
	uint32_t next = 0, end;
	bool started = false;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	weights = PIPE->ctl.weights_seq;
	ai64_pipe_stage_reset(st);
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
		bool in_order = true;

		k_sem_take(&bell, K_FOREVER);
		end = (uint32_t)atomic_get(&newest);
		/*
		 * The first window, or a jump: a stage started again, or the
		 * ring no longer holds them. Start from the newest.
		 */
		if (!started || (int32_t)(end - next) <= 0 || end - next > AI64_PIPE_RESULTS) {
			if (started && end - next < AI64_PIPE_RESYNC) {
				st->dropped += end - next - 1u;
			}
			next = end - 1u;
			started = true;
			in_order = false;
		}
		for (; next != end; next++) {
			one(st, sc, next, in_order);
			in_order = true;
		}
	}
}

static K_THREAD_STACK_DEFINE(pipe_stack, 1024);
static struct k_thread pipe_tid;

void pipe_pwm_start(void)
{
	k_thread_create(&pipe_tid, pipe_stack, K_THREAD_STACK_SIZEOF(pipe_stack), pipe_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
	k_thread_name_set(&pipe_tid, "pipe_pwm");
}
