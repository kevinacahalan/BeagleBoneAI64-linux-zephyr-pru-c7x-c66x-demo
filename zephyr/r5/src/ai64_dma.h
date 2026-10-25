/* SPDX-License-Identifier: MIT */
#ifndef AI64_DMA_H_
#define AI64_DMA_H_

#include <stdint.h>

/*
 * Copy len bytes (1 to 65535) from SoC address src to dst with R5F0_0's
 * UDMA channel (src/ai64_dma.c), and wait for it. *ns, if not NULL, gets
 * the time from the doorbell to the completion. Returns 0 or a negative
 * errno.
 */
int ai64_dma_copy(uint64_t dst, uint64_t src, uint32_t len, uint32_t *ns);

/* Turn the channel off, for a core about to stop (main.c's shutdown). */
void ai64_dma_stop(void);

#endif
