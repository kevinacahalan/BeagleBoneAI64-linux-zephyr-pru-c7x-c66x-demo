/* SPDX-License-Identifier: MIT */
/*
 * R5F0_0's stage of the pipeline (ai64_pipe.h): every window of P8.31
 * samples, from ICSSG0's shared RAM into C66x 0's L2 SRAM, with the UDMA.
 *
 * ICSSG0 RTU1 rings FIFO 15 of this core's home mailbox (cluster 11, user
 * 1) with each window's number, while Linux has the pipeline running. The
 * interrupt only wakes the thread, which copies every window from the last
 * it copied to the newest, as long as the ring still holds them. Each copy
 * is one UDMA descriptor: the window's chunks and tag into slot (window %
 * AI64_PIPE_IN_SLOTS), then the count of windows copied into the C66x's
 * doorbell word, which the UDMA writes only once the chunks have landed
 * (ai64_dma_copy_bell()). The C66x polls that word in its own L2.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>
#include <stddef.h>

#include "ai64_dma.h"
#include "ai64_pipe.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define PIPE ((volatile struct ai64_pipe *)AI64_PIPE_ADDR)
#define SMEM_TAG(w) \
	((volatile struct ai64_pipe_tag *)(AI64_PIPE_SMEM_SOC + \
					   ((w) % AI64_PIPE_SLOTS) * AI64_PIPE_SLOT_SIZE + \
					   offsetof(struct ai64_pipe_slot, tag)))

static const struct device *const mbox = DEVICE_DT_GET(DT_NODELABEL(ai64_ipc_home0));
static K_SEM_DEFINE(bell, 0, 1);
/* One more than the newest window RTU1 rang about. */
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

static void stage_reset(volatile struct ai64_pipe_stage *st)
{
	st->magic = 0;
	st->windows = 0;
	st->dropped = 0;
	st->last = 0;
	st->lat_sum = 0;
	st->lat_min = UINT32_MAX;
	st->lat_max = 0;
	st->lat_last = 0;
	st->work_sum = 0;
	st->work_max = 0;
	st->errors = 0;
	st->magic = AI64_PIPE_MAGIC;
}

/* Copy window w on; 0, or a negative errno. */
static int pass_on(volatile struct ai64_pipe_stage *st, uint32_t w)
{
	static uint32_t weights;
	uint32_t slot = w % AI64_PIPE_IN_SLOTS;
	uint64_t dst = AI64_PIPE_C66X_PHYS + offsetof(struct ai64_pipe_c66x_in, slot) +
		       slot * sizeof(struct ai64_pipe_slot);
	uint64_t src = AI64_PIPE_SMEM_SOC + (w % AI64_PIPE_SLOTS) * AI64_PIPE_SLOT_SIZE;
	uint32_t t_end, t0, t1, lat, work;
	int ret;

	/* PRU1 has gone round the ring past it, or RTU1 has not stamped it. */
	if (SMEM_TAG(w)->win != w) {
		st->dropped++;
		return -ESTALE;
	}
	t_end = SMEM_TAG(w)->t_end_lo;
	t0 = gtc_lo();
	ret = ai64_dma_copy_bell(dst, src, AI64_PIPE_SLOT_COPY,
				 AI64_PIPE_C66X_PHYS + offsetof(struct ai64_pipe_c66x_in, ready),
				 w + 1u, NULL);
	t1 = gtc_lo();
	if (ret) {
		st->errors++;
		return ret;
	}
	if (PIPE->ctl.weights_seq != weights) {
		weights = PIPE->ctl.weights_seq;
		stage_reset(st);
	}
	lat = t1 - t_end;
	work = t1 - t0;
	st->windows++;
	st->last = w;
	st->lat_last = lat;
	st->lat_sum += lat;
	if (lat < st->lat_min) {
		st->lat_min = lat;
	}
	if (lat > st->lat_max) {
		st->lat_max = lat;
	}
	st->work_sum += work;
	if (work > st->work_max) {
		st->work_max = work;
	}
	return 0;
}

static void pipe_thread(void *a, void *b, void *c)
{
	volatile struct ai64_pipe_stage *st = &PIPE->stage[AI64_PIPE_DMA];
	uint32_t next = 0, end;
	bool started = false;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	stage_reset(st);
	ret = mbox_register_callback(mbox, AI64_PIPE_FIFO, rung, NULL);
	ret = ret ? ret : mbox_set_enabled(mbox, AI64_PIPE_FIFO, true);
	if (ret) {
		LOG_ERR("pipe: mailbox FIFO %u: %d", AI64_PIPE_FIFO, ret);
		return;
	}
	LOG_INF("pipe: windows from ICSSG0 to C66x 0's L2 by UDMA, on RTU1's doorbell");

	for (;;) {
		k_sem_take(&bell, K_FOREVER);
		end = (uint32_t)atomic_get(&newest);
		/*
		 * The first window, after a stop, or after PRU1 started again:
		 * start from the newest.
		 */
		if (!started || (int32_t)(end - next) <= 0 || end - next > AI64_PIPE_SLOTS) {
			next = end - 1u;
			started = true;
		}
		for (; next != end; next++) {
			ret = pass_on(st, next);
			if (ret == -ESTALE) {
				continue;
			}
			if (ret) {
				LOG_ERR("pipe: window %u: DMA failed (%d)", next, ret);
				next = end;
				break;
			}
		}
	}
}

K_THREAD_DEFINE(ai64_pipe_tid, 1024, pipe_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(2), 0, 0);
