/* SPDX-License-Identifier: MIT */
#ifndef R5F1_1_H_
#define R5F1_1_H_

#include <stddef.h>

/* The monitor, monitor.c: its own status line, and that line with one for
 * each other core, for Linux's status call. */
void monitor_status(char *buf, size_t len);
void monitor_table(char *buf, size_t len);
/* Starts its thread. */
void monitor_start(void);

#endif
