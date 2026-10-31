/* SPDX-License-Identifier: MIT */
#ifndef R5F1_0_H_
#define R5F1_0_H_

#include <stddef.h>

/* The square wave on P8.18, blink.c. */
void blink_status(char *buf, size_t len);
/* Starts the timer. */
void blink_start(void);

#endif
