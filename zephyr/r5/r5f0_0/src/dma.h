/* SPDX-License-Identifier: MIT */
#ifndef DMA_H_
#define DMA_H_

#include <stdint.h>

/*
 * Copy len bytes (1 to 65535) from SoC address src to dst with R5F0_0's
 * UDMA channel (dma.c), and wait for it. *ns, if not NULL, gets
 * the time from the doorbell to the completion. Returns 0 or a negative
 * errno.
 */
int dma_copy(uint64_t dst, uint64_t src, uint32_t len, uint32_t *ns);

/* The same, then the 32-bit value written at SoC address bell, by the UDMA
 * once the copy has landed. */
int dma_copy_bell(uint64_t dst, uint64_t src, uint32_t len, uint64_t bell, uint32_t value,
		  uint32_t *ns);

/* Turn the channel off, for a core about to stop (its shutdown hook, main.c). */
void dma_stop(void);

#endif
