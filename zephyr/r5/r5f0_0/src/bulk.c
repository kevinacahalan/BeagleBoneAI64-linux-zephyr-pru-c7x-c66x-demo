/* SPDX-License-Identifier: MIT */
/*
 * R5F0_0's bulk buffer, 64 KiB of DDR Linux writes through /dev/mem
 * (struct ai64_bulk, ai64_shm.h), and Linux's two calls on it: takeChunk
 * checks what Linux wrote there, and dmaCopy copies from it into the C7x's
 * job block with this core's UDMA channel (dma.c), and nowhere else. The
 * overlay keeps the Zephyr image out of the buffer, and the MPU maps it
 * non-cacheable (common/src/mpu.c).
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ai64_shm.h"
#include "ai64_dsp_job.h"
#include "dma.h"
#include "r5f0_0.h"

LOG_MODULE_DECLARE(ai64_r5, LOG_LEVEL_INF);

BUILD_ASSERT(DT_NODE_EXISTS(DT_NODELABEL(ai64_bulk)), "R5F0_0's overlay has the bulk buffer");

static volatile struct ai64_bulk *const bulk =
	(volatile struct ai64_bulk *)AI64_BULK_ADDR;

uint32_t bulk_chunk(uint32_t seq, uint32_t length, uint32_t crc)
{
	uint32_t got;
	uint32_t magic;
	uint32_t got_seq;
	uint32_t got_len;
	uint32_t got_crc;

	if (length > sizeof(bulk->data)) {
		return 0;
	}
	sys_cache_data_invd_range((void *)bulk, 16u + length);
	magic = bulk->magic;
	got_seq = bulk->seq;
	got_len = bulk->length;
	got_crc = bulk->crc;
	if (magic != AI64_BULK_MAGIC || got_seq != seq || got_len != length ||
	    got_crc != crc) {
		LOG_INF("chunk mismatch magic %08x seq %u len %u crc %08x",
			magic, got_seq, got_len, got_crc);
		return 0;
	}
	got = ai64_crc32((const uint8_t *)bulk->data, length);
	LOG_INF("chunk seq %u len %u crc %08x", seq, length, got);
	return got;
}

/* Whether [a, a + n) lies inside [base, base + size). */
static bool inside(uint32_t a, uint32_t n, uint64_t base, uint32_t size)
{
	return a >= base && n <= size && a - base <= size - n;
}

/* Where the R5F's own copy goes, for cpu_ns. */
static uint8_t cpu_copy[0x4000] __aligned(64);

int32_t bulk_dma(uint32_t dst, uint32_t src, uint32_t length, uint32_t *cpu_ns)
{
	uint32_t ns = 0, t0, n;
	int ret;

	*cpu_ns = 0;
	if (!inside(src, length, AI64_BULK_ADDR + 16u, AI64_BULK_SIZE - 16u) ||
	    !inside(dst, length, AI64_JOB_C7X_PHYS, AI64_JOB_SIZE)) {
		LOG_INF("erpc dma: %#x -> %#x, %u bytes: only bulk buffer -> C7x job block", src, dst,
			length);
		return -EINVAL;
	}
	ret = dma_copy(dst, src, length, &ns);
	if (ret) {
		LOG_ERR("erpc dma: failed (%d)", ret);
		return ret;
	}
	/* The same bytes copied by this core, as far as cpu_copy holds them. */
	n = length < sizeof(cpu_copy) ? length : sizeof(cpu_copy);
	t0 = sys_read32(AI64_GTC_ADDR + 8u);
	memcpy(cpu_copy, (const void *)(uintptr_t)src, n);
	*cpu_ns = (uint32_t)((uint64_t)(sys_read32(AI64_GTC_ADDR + 8u) - t0) *
			     (1000000000u / AI64_GTC_HZ) * length / n);
	LOG_INF("erpc dma: %u bytes in %u ns (memcpy %u ns)", length, ns, *cpu_ns);
	return (int32_t)ns;
}
