/* SPDX-License-Identifier: MIT */
#ifndef AI64_ERPC_LINUX_H_
#define AI64_ERPC_LINUX_H_

#include <stddef.h>
#include <stdint.h>

int ai64_erpc_open(int fd);
int ai64_erpc_say(const char *msg, char *out, size_t outlen);
int ai64_erpc_spi(const char *text, char *out, size_t outlen);
int ai64_erpc_arm(uint32_t period, uint32_t count);
int ai64_erpc_xy(uint32_t x, uint32_t y);
int ai64_erpc_take(uint32_t seq, uint32_t length, uint32_t crc, uint32_t *got);

#endif
