/* SPDX-License-Identifier: MIT */
#ifndef AI64_ERPC_H_
#define AI64_ERPC_H_

#include <stddef.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <stdint.h>

#include "ai64_shm.h"

/*
 * The same app runs on every main-domain R5F core, each with its own job
 * (Kconfig): SPI7 on R5F0_0, which also holds the bulk buffer, and EHRPWM4
 * on P9.25 on R5F0_1. AI64_CORE ("r5f0_0" and so on) and AI64_SELF (0 to 3)
 * come from CMakeLists.txt.
 */
#define AI64_HAS_SPI7 IS_ENABLED(CONFIG_AI64_JOB_SPI)
#define AI64_HAS_BULK DT_NODE_EXISTS(DT_NODELABEL(ai64_bulk))
#define AI64_HAS_PWM  IS_ENABLED(CONFIG_AI64_JOB_PWM)

/* SPI7, src/ai64_spi.c. */
int ai64_spi7_write(const void *buf, size_t len);
/* P9.25, src/ai64_pwm.c. who names the asker in the log. */
int ai64_pwm_set(const char *who, uint32_t period_ns, uint32_t duty_ns);
/* ms 0 stops the steps and leaves the duty where it is. */
int ai64_pwm_steps(const char *who, uint32_t ms);
void ai64_pwm_publish(volatile struct ai64_pwm_status *s, uint32_t now);
int ai64_rpmsg_tx(const void *data, size_t len);
void ai64_erpc_push(const void *data, size_t len);
void ai64_erpc_server(void);

#endif
