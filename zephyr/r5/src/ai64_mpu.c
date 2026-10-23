/* SPDX-License-Identifier: MIT */
/*
 * Make this core's own DDR cacheable.
 *
 * The J721E R5F image runs from DDR, in the 16 MiB carveout Linux reserves
 * for the core. The SoC boot code maps all of DDR with one MPU region that is
 * inner non-cacheable, and CONFIG_ARM_MPU is off, so every instruction fetch
 * and load goes to DDR. That costs about a millisecond per eRPC call.
 *
 * This adds regions on top (a higher region number wins):
 *   1  the low 2 GiB, device memory: every peripheral is there. With the
 *      caches on, the background map would make these accesses cacheable,
 *      and the secure proxy, for one, answers a line fill with an abort.
 *   2  ATCM (the vector table) and 3 BTCM, normal non-cacheable, so the
 *      core can still execute from them
 *   4  the whole carveout, normal write-through: code and data are cached,
 *      and every write still reaches DDR, so Linux reads the RAM console
 *      trace as it is written
 *   5  its first 2 MiB, non-cacheable: the vrings and the resource table,
 *      which Linux writes
 *   6  on R5F0_0, the 128 KiB holding the bulk buffer, non-cacheable:
 *      Linux writes it
 *   7  the fabric, normal non-cacheable: every demo core's inbox
 *      (ai64_fabric.h)
 *
 * It runs at the EARLY init level, before soc_early_init_hook() invalidates
 * and enables both caches (CONFIG_CACHE_MANAGEMENT), so the regions are in
 * place before anything is cached and no line from an earlier run survives.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/barrier.h>

#include "ai64_erpc.h"
#include "ai64_shm.h"
#include "ai64_fabric.h"

/* The core's carveout starts at the vring region (zephyr,ipc_shm). */
#define CARVEOUT_BASE  DT_REG_ADDR(DT_CHOSEN(zephyr_ipc_shm))
#define CARVEOUT_SIZE  0x01000000u

/* DRACR: AP = full access, plus TEX/C/B. */
#define ACC_FULL       (3u << 8)
#define ATTR_DEVICE    (ACC_FULL | (1u << 2) | 1u)            /* TEX 000, C 0, B 1, S 1 */
#define ATTR_NORMAL_WT (ACC_FULL | (0u << 3) | (1u << 1) | 0u) /* TEX 000, C 1, B 0 */
#define ATTR_NORMAL_NC (ACC_FULL | (1u << 3))                  /* TEX 001, C 0, B 0 */

/* DRSR: size field is log2(size) - 1, bit 0 enables the region. */
#define REGION_SIZE(log2) ((((log2) - 1u) << 1) | 1u)

/* RGNR selects the region; DRBAR, DRACR, and DRSR then describe it. */
static void mpu_set(uint32_t num, uint32_t base, uint32_t size_log2, uint32_t attr)
{
	uint32_t size = REGION_SIZE(size_log2);

	__asm__ volatile("mcr p15, 0, %0, c6, c2, 0" : : "r"(num));
	__asm__ volatile("mcr p15, 0, %0, c6, c1, 0" : : "r"(base));
	__asm__ volatile("mcr p15, 0, %0, c6, c1, 4" : : "r"(attr));
	__asm__ volatile("mcr p15, 0, %0, c6, c1, 2" : : "r"(size));
}

static int ai64_mpu_init(void)
{
	BUILD_ASSERT((CARVEOUT_BASE & (CARVEOUT_SIZE - 1u)) == 0u,
		     "the carveout must be aligned to its size");

	barrier_dsync_fence_full();
	mpu_set(1, 0x00000000u, 31, ATTR_DEVICE);
	mpu_set(2, DT_REG_ADDR(DT_NODELABEL(atcm)), 15, ATTR_NORMAL_NC);
	mpu_set(3, DT_REG_ADDR(DT_NODELABEL(btcm)), 15, ATTR_NORMAL_NC);
	mpu_set(4, CARVEOUT_BASE, 24, ATTR_NORMAL_WT);
	mpu_set(5, CARVEOUT_BASE, 21, ATTR_NORMAL_NC);
#if AI64_HAS_BULK
	BUILD_ASSERT((AI64_BULK_ADDR & ~0x1ffffu) == ((AI64_BULK_ADDR + AI64_BULK_SIZE - 1u) & ~0x1ffffu),
		     "the bulk buffer is inside one 128 KiB region");
	mpu_set(6, AI64_BULK_ADDR & ~0x1ffffu, 17, ATTR_NORMAL_NC);
#endif
	BUILD_ASSERT((AI64_FAB_ADDR & (AI64_FAB_SIZE - 1u)) == 0u &&
		     AI64_FAB_SIZE == 0x20000u, "the fabric is one 128 KiB region");
	mpu_set(7, AI64_FAB_ADDR, 17, ATTR_NORMAL_NC);
	barrier_dsync_fence_full();
	barrier_isync_fence_full();
	return 0;
}

SYS_INIT(ai64_mpu_init, EARLY, 0);
