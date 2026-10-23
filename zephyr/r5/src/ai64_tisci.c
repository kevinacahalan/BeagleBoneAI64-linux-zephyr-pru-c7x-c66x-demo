/* SPDX-License-Identifier: MIT */
/*
 * Route this core's mailbox interrupt, with a TI-SCI request.
 *
 * Linux kicks the R5F through a mailbox FIFO. On J721E the mailbox user
 * interrupt reaches the R5F's VIM only through the NAVSS interrupt router,
 * and nothing programs that router for the R5F. This asks the device
 * manager to do it (TISCI_MSG_RM_IRQ_SET), the way Linux's ti,sci-intr does
 * for its own interrupts and Zephyr PR #121379 does for R5F peripherals:
 * the request names the router as both source and destination, with a
 * router input and a router output from this host's range.
 *
 * The request goes through Zephyr's TI-SCI driver, over the polled secure
 * proxy mailbox in ai64_sproxy.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/firmware/tisci/tisci.h>
#include <zephyr/logging/log.h>

#include "ai64_tisci.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

#define ROUTE DT_INST(0, ai64_tisci_irq_route)

#if !DT_NODE_EXISTS(ROUTE)
#error "The board overlay needs an ai64,tisci-irq-route node"
#endif

#define IRQ_SECONDARY_HOST_INVALID 0xffu

static const struct device *const dmsc = DEVICE_DT_GET(DT_NODELABEL(dmsc));

int ai64_route_mbox_irq(void)
{
	struct tisci_irq_release_req rel = {
		.valid_params = TISCI_MSG_VALUE_RM_DST_ID_VALID |
				TISCI_MSG_VALUE_RM_DST_HOST_IRQ_VALID,
		.src_id = DT_PROP(ROUTE, ir_id),
		.src_index = DT_PROP(ROUTE, ir_input),
		.dst_id = DT_PROP(ROUTE, ir_id),
		.dst_host_irq = DT_PROP(ROUTE, ir_output),
		.secondary_host = IRQ_SECONDARY_HOST_INVALID,
	};
	struct tisci_irq_set_req set = {
		.valid_params = rel.valid_params,
		.src_id = rel.src_id,
		.src_index = rel.src_index,
		.dst_id = rel.dst_id,
		.dst_host_irq = rel.dst_host_irq,
		.secondary_host = IRQ_SECONDARY_HOST_INVALID,
	};
	int ret;

	if (!device_is_ready(dmsc)) {
		LOG_WRN("tisci: the TI-SCI device is not ready");
		return -ENODEV;
	}
	/*
	 * A route this core set up on an earlier boot is still there; drop
	 * it first. A NACK here just means there was none.
	 */
	(void)tisci_cmd_rm_irq_release(dmsc, &rel);
	ret = tisci_cmd_rm_irq_set(dmsc, &set);
	if (ret) {
		LOG_WRN("tisci: router %u input %u -> output %u failed (%d)", set.src_id,
			set.src_index, set.dst_host_irq, ret);
		return ret;
	}
	LOG_INF("tisci: mailbox interrupt routed, router %u input %u -> output %u", set.src_id,
		set.src_index, set.dst_host_irq);
	return 0;
}
