/* SPDX-License-Identifier: MIT */
#ifndef R5F0_1_H_
#define R5F0_1_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ai64_ipc.h"

/* P9.25, pwm.c. who names the asker in the log. */
int p925_set(const char *who, uint32_t period_ns, uint32_t duty_ns);
/* ms 0 stops the steps and leaves the duty where it is. */
int p925_steps(const char *who, uint32_t ms);
/* Hold P9.25 on step n of the program, quietly: the pipeline's class n
 * (pipe_pwm.c). A set or steps request ends it. */
int p925_class(uint32_t n);
/* P9.25 is on the pipeline's class, with no request since. */
bool p925_following(void);
void p925_status(char *buf, size_t len);
void p925_request(const char *who, const struct ai64_ipc_msg *req, struct ai64_ipc_msg *rep);
uint32_t p925_fab_action(uint32_t action, uint32_t arg0, uint32_t arg1);
/* Its struct ai64_pwm_status, in this node's fabric page. */
void p925_publish(volatile void *page, uint32_t now);
/* Starts the steps. */
void p925_start(void);

/* The pipeline's last stage, pipe_pwm.c: starts its thread. */
void pipe_pwm_start(void);

#endif
