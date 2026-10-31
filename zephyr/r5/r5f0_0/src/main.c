/* SPDX-License-Identifier: MIT */
/*
 * R5F0_0: SPI7 on header P9 (spi.c), the bulk buffer Linux writes (bulk.c),
 * a UDMA channel (dma.c), and the pipeline's move (pipe_dma.c): each window
 * of P8.31 samples from ICSSG0 into a C66x's L2 SRAM. It keeps the board's
 * UART2 console, and sends the fabric's tours.
 *
 * Like every R5F core it also runs ../common/: RPMsg and eRPC to Linux,
 * ipc_service links to the other three, and its fabric node. ai64_r5_core
 * below is what this core adds to that, and start() starts its own job.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "ai64_r5.h"
#include "dma.h"
#include "r5f0_0.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

const struct ai64_r5_core ai64_r5_core = {
	.status = spi7_status,
	.request = spi7_request,
	.chunk = bulk_chunk,
	.dma = bulk_dma,
	/* A channel left on would still be on when the core runs again. */
	.shutdown = dma_stop,
};

/* At the APPLICATION init level: after the drivers, before main() and the
 * common threads. */
static int start(void)
{
	spi7_start();
	pipe_dma_start();
	return 0;
}

SYS_INIT(start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int main(void)
{
	LOG_INF("ai64 " AI64_CORE " hello");
	return 0;
}
