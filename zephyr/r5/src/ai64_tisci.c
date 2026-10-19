/* SPDX-License-Identifier: MIT */
/*
 * Route this core's mailbox interrupt, with TI-SCI requests.
 *
 * Linux kicks the R5F through a mailbox FIFO. On J721E the mailbox user
 * interrupt reaches the R5F's VIM only through the NAVSS interrupt router,
 * and nothing programs that router for the R5F. This asks the device
 * manager to do it (TISCI_MSG_RM_IRQ_SET), the way Linux's ti,sci-intr does
 * for its own interrupts and Zephyr PR #121379 does for R5F peripherals:
 * the request names the router as both source and destination, with a
 * router input and a router output from this host's range.
 *
 * Zephyr's TI-SCI driver waits for a secure proxy receive interrupt, which
 * nothing routes either. So this talks to the secure proxy directly and
 * polls, like TI's sciclient in polling mode.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <string.h>

#include "ai64_tisci.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define ROUTE DT_INST(0, ai64_tisci_irq_route)

#if !DT_NODE_EXISTS(ROUTE)
#error "The board overlay needs an ai64,tisci-irq-route node"
#endif

#define SPROXY_DATA      DT_REG_ADDR_BY_IDX(ROUTE, 0)
#define SPROXY_RT        DT_REG_ADDR_BY_IDX(ROUTE, 1)
#define THREAD(base, t)  ((base) + 0x1000u * (t))
/* Data words sit at 0x04-0x3c. Writing 0x3c sends; reading it frees the slot. */
#define DATA_FIRST       0x04u
#define DATA_LAST        0x3cu
#define STATUS_COUNT     0xffu
#define STATUS_ERROR     (1u << 31)

#define TISCI_MSG_RM_IRQ_SET            0x1000u
#define TISCI_MSG_RM_IRQ_RELEASE        0x1001u
#define TISCI_FLAG_REQ_ACK_ON_PROCESSED (1u << 1)
#define TISCI_FLAG_RESP_GENERIC_ACK     (1u << 1)
#define IRQ_DST_ID_VALID                (1u << 0)
#define IRQ_DST_HOST_IRQ_VALID          (1u << 1)
#define IRQ_SECONDARY_HOST_INVALID      0xffu

struct tisci_hdr {
	uint16_t type;
	uint8_t host;
	uint8_t seq;
	uint32_t flags;
} __packed;

/* Same layout for TISCI_MSG_RM_IRQ_SET and TISCI_MSG_RM_IRQ_RELEASE. */
struct tisci_irq_req {
	struct tisci_hdr hdr;
	uint32_t valid_params;
	uint16_t src_id;
	uint16_t src_index;
	uint16_t dst_id;
	uint16_t dst_host_irq;
	uint16_t ia_id;
	uint16_t vint;
	uint16_t global_event;
	uint8_t vint_status_bit_index;
	uint8_t secondary_host;
} __packed;

static int sproxy_send(uint32_t thread, const void *msg, size_t len)
{
	uintptr_t data = THREAD(SPROXY_DATA, thread);
	const uint8_t *p = msg;
	uint32_t off;

	if (sys_read32(THREAD(SPROXY_RT, thread)) & STATUS_ERROR) {
		return -EIO;
	}
	for (off = DATA_FIRST; off <= DATA_LAST; off += 4u) {
		uint32_t word = 0;
		size_t at = off - DATA_FIRST;

		if (at < len) {
			memcpy(&word, p + at, MIN(4u, len - at));
		}
		/* The last register sends, so every word before it must land first. */
		barrier_dmem_fence_full();
		sys_write32(word, data + off);
	}
	return 0;
}

static int sproxy_recv(uint32_t thread, void *msg, size_t len, int timeout_ms)
{
	uintptr_t data = THREAD(SPROXY_DATA, thread);
	uintptr_t rt = THREAD(SPROXY_RT, thread);
	uint8_t *p = msg;
	uint32_t off;
	int64_t end = k_uptime_get() + timeout_ms;

	while (!(sys_read32(rt) & STATUS_COUNT)) {
		if (sys_read32(rt) & STATUS_ERROR) {
			return -EIO;
		}
		if (k_uptime_get() > end) {
			return -ETIMEDOUT;
		}
		k_busy_wait(100);
	}
	for (off = DATA_FIRST; off <= DATA_LAST; off += 4u) {
		uint32_t word = sys_read32(data + off);
		size_t at = off - DATA_FIRST;

		if (at < len) {
			memcpy(p + at, &word, MIN(4u, len - at));
		}
	}
	return 0;
}

/* One request and its answer. Returns 0 on ACK, -EACCES on NACK. */
static int tisci_irq(uint16_t type)
{
	static uint8_t seq;
	struct tisci_irq_req req = {
		.hdr = {
			.type = type,
			.host = DT_PROP(ROUTE, host_id),
			.seq = ++seq,
			.flags = TISCI_FLAG_REQ_ACK_ON_PROCESSED,
		},
		.valid_params = IRQ_DST_ID_VALID | IRQ_DST_HOST_IRQ_VALID,
		.src_id = DT_PROP(ROUTE, ir_id),
		.src_index = DT_PROP(ROUTE, ir_input),
		.dst_id = DT_PROP(ROUTE, ir_id),
		.dst_host_irq = DT_PROP(ROUTE, ir_output),
		.secondary_host = IRQ_SECONDARY_HOST_INVALID,
	};
	struct tisci_hdr resp;
	int ret;

	ret = sproxy_send(DT_PROP(ROUTE, tx_thread), &req, sizeof(req));
	if (ret) {
		return ret;
	}
	ret = sproxy_recv(DT_PROP(ROUTE, rx_thread), &resp, sizeof(resp), 1000);
	if (ret) {
		return ret;
	}
	if (resp.seq != req.hdr.seq || !(resp.flags & TISCI_FLAG_RESP_GENERIC_ACK)) {
		return -EACCES;
	}
	return 0;
}

int ai64_route_mbox_irq(void)
{
	int ret;

	/*
	 * A route this core set up on an earlier boot is still there; drop
	 * it first. A NACK here just means there was none.
	 */
	(void)tisci_irq(TISCI_MSG_RM_IRQ_RELEASE);
	ret = tisci_irq(TISCI_MSG_RM_IRQ_SET);
	if (ret) {
		LOG_WRN("tisci: router %u input %u -> output %u failed (%d)",
			DT_PROP(ROUTE, ir_id), DT_PROP(ROUTE, ir_input),
			DT_PROP(ROUTE, ir_output), ret);
		return ret;
	}
	LOG_INF("tisci: mailbox interrupt routed, router %u input %u -> output %u",
		DT_PROP(ROUTE, ir_id), DT_PROP(ROUTE, ir_input), DT_PROP(ROUTE, ir_output));
	return 0;
}
