/* SPDX-License-Identifier: MIT */
#ifndef R5F0_0_H_
#define R5F0_0_H_

#include <stddef.h>
#include <stdint.h>

#include "ai64_ipc.h"

/* SPI7, spi.c: write buf, 0 or a negative errno. */
int spi7_write(const void *buf, size_t len);
void spi7_status(char *buf, size_t len);
void spi7_request(const char *who, const struct ai64_ipc_msg *req, struct ai64_ipc_msg *rep);
/* Writes "hello". */
void spi7_start(void);

/* Linux's takeChunk and dmaCopy, on the bulk buffer (bulk.c). */
uint32_t bulk_chunk(uint32_t seq, uint32_t length, uint32_t crc);
int32_t bulk_dma(uint32_t dst, uint32_t src, uint32_t length, uint32_t *cpu_ns);

/* The pipeline's stage, pipe_dma.c: starts its thread. */
void pipe_dma_start(void);

#endif
