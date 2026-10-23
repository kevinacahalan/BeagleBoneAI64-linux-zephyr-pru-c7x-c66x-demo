/* SPDX-License-Identifier: MIT */
/*
 * P9.25: EHRPWM4 output B, through Zephyr's EHRPWM driver. Only R5F0_0's
 * overlay has it (the pwms of /zephyr,user); the other cores build none of
 * this.
 *
 * Linux sets it over eRPC (setPwm for a fixed duty, setPwmSteps), and any
 * core with a fabric route's pwm or steps action for r5f0_0. Like the
 * ICSSG0 slices it starts stepping through AI64_PWM_STEP_HZ and
 * AI64_PWM_STEP_PCT, a step every AI64_PWM_DEFAULT_STEPS_MS, from a
 * delayable work item: each step asks the
 * device manager for the clock rate over TI-SCI, which an interrupt
 * handler must not. About every 10 ms the fabric thread publishes its
 * struct ai64_pwm_status (ai64_shm.h) in this node's fabric page, as the
 * ICSSG0 RTUs do for their PWM slices. The EHRPWM counts no periods, so
 * periods stays 0, and late does not apply.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "ai64_erpc.h"
#include "ai64_shm.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#if AI64_HAS_PWM

static const struct pwm_dt_spec out = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));
static const uint32_t step_hz[AI64_PWM_STEP_FREQS] = AI64_PWM_STEP_HZ;
static const uint32_t step_pct[AI64_PWM_STEPS] = AI64_PWM_STEP_PCT;
static K_MUTEX_DEFINE(lock);
static uint32_t ask_period, ask_duty, run_period, run_duty, ctl_seq, ctl_ack;
static uint32_t steps_ms, step_at;
/* When the next step is due, in ticks: steps keep time, however long a step takes. */
static int64_t step_due;

static void step_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(step, step_work);

/* Set the outputs; the caller holds the lock. */
static int apply(uint32_t period_ns, uint32_t duty_ns)
{
	int ret = pwm_set_dt(&out, period_ns, duty_ns);

	if (ret == 0) {
		run_period = period_ns;
		run_duty = duty_ns;
		ctl_ack = ctl_seq;
	}
	return ret;
}

/* Step n of the 15: the frequency is n / 5, the duty n % 5. */
static int apply_step(uint32_t n)
{
	uint32_t period = 1000000000u / step_hz[n / AI64_PWM_STEPS];

	ask_period = period;
	return apply(period, (uint32_t)((uint64_t)period * step_pct[n % AI64_PWM_STEPS] / 100u));
}

static void step_work(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&lock, K_FOREVER);
	if (steps_ms != 0u) {
		step_at = step_at + 1u == AI64_PWM_STEP_FREQS * AI64_PWM_STEPS ? 0u : step_at + 1u;
		(void)apply_step(step_at);
		step_due += k_ms_to_ticks_ceil64(steps_ms);
		k_work_reschedule(&step, K_TIMEOUT_ABS_TICKS(step_due));
	}
	k_mutex_unlock(&lock);
}

static uint32_t clamp_period(uint32_t period_ns)
{
	if (period_ns < AI64_PWM_PERIOD_MIN_NS) {
		return AI64_PWM_PERIOD_MIN_NS;
	}
	return period_ns > AI64_PWM_PERIOD_MAX_NS ? AI64_PWM_PERIOD_MAX_NS : period_ns;
}

int ai64_pwm_set(const char *who, uint32_t period_ns, uint32_t duty_ns)
{
	int ret;

	period_ns = clamp_period(period_ns);
	if (duty_ns > period_ns) {
		duty_ns = period_ns;
	}
	if (!pwm_is_ready_dt(&out)) {
		LOG_ERR("pwm: EHRPWM4 is not ready");
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	ask_period = period_ns;
	ask_duty = duty_ns;
	steps_ms = 0;
	(void)k_work_cancel_delayable(&step);
	ctl_seq++;
	ret = apply(period_ns, duty_ns);
	k_mutex_unlock(&lock);
	if (ret) {
		LOG_ERR("pwm: %s asked for P9.25 period %u ns duty %u ns, failed (%d)", who,
			period_ns, duty_ns, ret);
	} else {
		LOG_INF("pwm: %s set P9.25 (EHRPWM4_B) to period %u ns, duty %u ns", who,
			period_ns, duty_ns);
	}
	return ret;
}

int ai64_pwm_steps(const char *who, uint32_t ms)
{
	if (!pwm_is_ready_dt(&out)) {
		LOG_ERR("pwm: EHRPWM4 is not ready");
		return -ENODEV;
	}
	if (ms > AI64_PWM_STEPS_MAX_MS) {
		ms = AI64_PWM_STEPS_MAX_MS;
	}
	k_mutex_lock(&lock, K_FOREVER);
	steps_ms = ms;
	ctl_seq++;
	if (ms != 0u) {
		/* The first step now; the work moves on from it. */
		step_at = 0;
		ask_duty = 0;
		(void)apply_step(0);
		step_due = k_uptime_ticks() + k_ms_to_ticks_ceil64(ms);
		k_work_reschedule(&step, K_TIMEOUT_ABS_TICKS(step_due));
	} else {
		(void)k_work_cancel_delayable(&step);
		ctl_ack = ctl_seq;
	}
	k_mutex_unlock(&lock);
	LOG_INF("pwm: %s set P9.25 (EHRPWM4_B) stepping 10, 15, 20 kHz x 10-90%% duty, every %u ms",
		who, ms);
	return 0;
}

void ai64_pwm_publish(volatile struct ai64_pwm_status *s, uint32_t now)
{
	k_mutex_lock(&lock, K_FOREVER);
	s->magic = 0;
	s->engine = AI64_PWM_EHRPWM;
	s->unit = 4;
	s->channel = out.channel;
	s->rpmsg = 1;
	s->ask_period_ns = ask_period;
	s->ask_duty_ns = ask_duty;
	s->ask_dead_ns = 0;
	s->sweep_ms = 0;
	s->steps_ms = steps_ms;
	s->run_period_ns = run_period;
	s->run_duty_ns = run_duty;
	s->run_dead_ns = 0;
	s->periods = 0;
	s->late = 0;
	s->late_max_ns = 0;
	s->ctl_seq = ctl_seq;
	s->ctl_ack = ctl_ack;
	s->at = now;
	s->magic = AI64_PWM_MAGIC;
	k_mutex_unlock(&lock);
}

/* Stepping, from the start. */
static int ai64_pwm_init(void)
{
	return ai64_pwm_steps("Zephyr", AI64_PWM_DEFAULT_STEPS_MS);
}

SYS_INIT(ai64_pwm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
