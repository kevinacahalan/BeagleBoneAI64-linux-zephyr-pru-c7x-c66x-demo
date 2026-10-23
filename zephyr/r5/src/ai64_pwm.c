/* SPDX-License-Identifier: MIT */
/*
 * P9.25: EHRPWM4 output B, through Zephyr's EHRPWM driver. Only R5F0_0's
 * overlay has it (the pwms of /zephyr,user); the other cores build none of
 * this.
 *
 * Linux sets it over eRPC (setPwm), and any core with a fabric route's pwm
 * action for r5f0_0. About every 10 ms the fabric thread publishes its
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
static K_MUTEX_DEFINE(lock);
static uint32_t ask_period, ask_duty, run_period, run_duty, ctl_seq, ctl_ack;

int ai64_pwm_set(const char *who, uint32_t period_ns, uint32_t duty_ns)
{
	int ret;

	if (period_ns < AI64_PWM_PERIOD_MIN_NS) {
		period_ns = AI64_PWM_PERIOD_MIN_NS;
	}
	if (period_ns > AI64_PWM_PERIOD_MAX_NS) {
		period_ns = AI64_PWM_PERIOD_MAX_NS;
	}
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
	ctl_seq++;
	ret = pwm_set_dt(&out, period_ns, duty_ns);
	if (ret == 0) {
		run_period = period_ns;
		run_duty = duty_ns;
		ctl_ack = ctl_seq;
	}
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

/* 1 kHz, 25%, from the start. */
static int ai64_pwm_init(void)
{
	return ai64_pwm_set("Zephyr", out.period, out.period / 4u);
}

SYS_INIT(ai64_pwm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
