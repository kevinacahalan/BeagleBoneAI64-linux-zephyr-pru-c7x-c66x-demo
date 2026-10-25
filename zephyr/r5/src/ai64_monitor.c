/* SPDX-License-Identifier: MIT */
/*
 * R5F1_1's job: keep an eye on the other three R5F cores.
 *
 * Every CONFIG_AI64_MONITOR_PERIOD_MS it pings each of them over its
 * ipc_service link and times the round trip on the GTC: request out,
 * through the peer's icmsg work queue and its request thread, reply back.
 * Once a second it also asks each for its status line. Linux's status call
 * to this core returns the whole table (ai64_monitor_table()).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "ai64_ipc.h"
#include "ai64_shm.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define PERIOD_MS     CONFIG_AI64_MONITOR_PERIOD_MS
#define STATUS_EVERY  (1000 / PERIOD_MS)
#define GTC_NS        (1000000000u / AI64_GTC_HZ)

struct seen {
	bool up;
	uint32_t pings, lost;
	/* Round trips over the last second, in GTC ticks. */
	uint32_t rtt_min, rtt_max, rtt_sum, rtt_n;
	uint32_t show_min, show_max, show_avg;
	char status[AI64_IPC_TEXT];
};

static struct seen seen[AI64_IPC_CORES];
static uint32_t rounds;
static K_MUTEX_DEFINE(lock);

void ai64_job_status(char *buf, size_t len)
{
	snprintf(buf, len, "monitor: pinging the other R5F cores every %u ms, %u rounds", PERIOD_MS,
		 rounds);
}

size_t ai64_monitor_table(char *buf, size_t len)
{
	size_t n;
	unsigned int c;

	ai64_job_status(buf, len);
	n = strlen(buf);
	k_mutex_lock(&lock, K_FOREVER);
	for (c = 0; c < AI64_IPC_CORES && n < len; c++) {
		const struct seen *s = &seen[c];

		if (c == AI64_SELF) {
			continue;
		}
		if (!s->up) {
			n += snprintf(buf + n, len - n, "\n  %s: link down, %u of %u pings lost",
				      ai64_core_name[c], s->lost, s->pings);
			continue;
		}
		n += snprintf(buf + n, len - n,
			      "\n  %s: round trip %u.%02u us (%u.%02u to %u.%02u), %u of %u lost; %s",
			      ai64_core_name[c], s->show_avg * GTC_NS / 1000u,
			      s->show_avg * GTC_NS % 1000u / 10u, s->show_min * GTC_NS / 1000u,
			      s->show_min * GTC_NS % 1000u / 10u, s->show_max * GTC_NS / 1000u,
			      s->show_max * GTC_NS % 1000u / 10u, s->lost, s->pings, s->status);
	}
	k_mutex_unlock(&lock);
	return n < len ? n : len - 1u;
}

static void ping(unsigned int c)
{
	struct ai64_ipc_msg m = {.op = AI64_IPC_PING};
	struct seen *s = &seen[c];
	int ret;

	ret = ai64_ipc_call(c, &m, K_MSEC(20));
	k_mutex_lock(&lock, K_FOREVER);
	s->pings++;
	if (ret) {
		s->lost++;
		s->up = ai64_ipc_bound(c);
	} else {
		/* The reply carries our send time back; arrival is now. */
		uint32_t rtt = sys_read32(AI64_GTC_ADDR + 8u) - m.sent;

		s->up = true;
		if (s->rtt_n == 0u || rtt < s->rtt_min) {
			s->rtt_min = rtt;
		}
		if (rtt > s->rtt_max) {
			s->rtt_max = rtt;
		}
		s->rtt_sum += rtt;
		s->rtt_n++;
	}
	k_mutex_unlock(&lock);
}

static void ask_status(unsigned int c)
{
	struct ai64_ipc_msg m = {.op = AI64_IPC_STATUS};
	struct seen *s = &seen[c];
	int ret = ai64_ipc_call(c, &m, K_MSEC(50));

	k_mutex_lock(&lock, K_FOREVER);
	if (ret == 0) {
		strncpy(s->status, m.text, sizeof(s->status) - 1u);
	} else {
		snprintf(s->status, sizeof(s->status), "no status (%d)", ret);
	}
	if (s->rtt_n) {
		s->show_min = s->rtt_min;
		s->show_max = s->rtt_max;
		s->show_avg = s->rtt_sum / s->rtt_n;
	}
	s->rtt_n = 0;
	s->rtt_sum = 0;
	s->rtt_max = 0;
	k_mutex_unlock(&lock);
}

static void monitor_thread(void *a, void *b, void *c)
{
	static char line[512];
	unsigned int n;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_msleep(PERIOD_MS);
		for (n = 0; n < AI64_IPC_CORES; n++) {
			if (n != AI64_SELF) {
				ping(n);
			}
		}
		if (++rounds % STATUS_EVERY == 0u) {
			for (n = 0; n < AI64_IPC_CORES; n++) {
				if (n != AI64_SELF) {
					ask_status(n);
				}
			}
		}
		/* Every 10 s, the table in this core's trace. */
		if (rounds % (10u * STATUS_EVERY) == 0u) {
			(void)ai64_monitor_table(line, sizeof(line));
			LOG_INF("%s", line);
		}
	}
}

K_THREAD_DEFINE(ai64_monitor_tid, 2048, monitor_thread, NULL, NULL, NULL, 10, 0, 0);
