/* SPDX-License-Identifier: MIT */
#ifndef AI64_ERPC_LINUX_H_
#define AI64_ERPC_LINUX_H_

#include <stddef.h>
#include <stdint.h>

int ai64_erpc_open(int fd);
int ai64_erpc_say(const char *msg, char *out, size_t outlen);
int ai64_erpc_spi(const char *text, char *out, size_t outlen);
int ai64_erpc_status(char *out, size_t outlen);
/* R5F0_0 copies with its UDMA: ns it took, or a negative errno. */
int32_t ai64_erpc_dma(uint32_t dst, uint32_t src, uint32_t length, uint32_t *cpu_ns);
int ai64_erpc_pwm(uint32_t period_ns, uint32_t duty_ns);
int ai64_erpc_pwm_steps(uint32_t step_ms);
int ai64_erpc_take(uint32_t seq, uint32_t length, uint32_t crc, uint32_t *got);

#endif
