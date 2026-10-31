/* SPDX-License-Identifier: MIT */
/*
 * Turn on the C7x MMU and caches.
 *
 * Out of reset the MMU is off and L2 is all SRAM, with no cache. This maps
 * the few regions the image uses, identity mapped, the way TI's PDK sets
 * up the core, and sizes the caches the way TI's vision_apps does: L1D all
 * cache (32 KiB), and 64 KiB of the 512 KiB L2 as cache, which leaves
 * 448 KiB of SRAM from 0x64800000.
 *
 *   0x00000000  1 GiB  device: the mailbox and every other peripheral
 *   0x40000000  1 GiB  device, apart from the L2 SRAM below
 *   0x64800000  4 KiB  normal non-cacheable: the job block header, which
 *                      Linux and the core both poll
 *   0x64801000 60 KiB  normal write-back: the rest of the job block. The
 *                      job code writes L1D back around each job.
 *   0x64810000  4 KiB  normal non-cacheable: the pipeline's features, which
 *                      C66x 0 writes (ai64_pipe.h)
 *   0x64811000 12 KiB  normal write-back: the pipeline's weights and outputs
 *   0xa8000000  2 MiB  normal non-cacheable: the DMA pool, then the trace
 *                      buffer and resource table (c7x.cmd), which Linux reads
 *   0xa8200000 14 MiB  normal write-back: code, data, and stack
 *   0xa9000000 128 KiB normal non-cacheable: the fabric (ai64_fabric.h),
 *                      which every demo core reads and writes
 *   0xa9090000 64 KiB  normal non-cacheable: the pipeline's DDR block
 *
 * Anything else faults. The tables use a 4 KiB granule and a 48-bit
 * address space, so a walk starts at level 0 (512 GiB an entry) and goes
 * through level 1 (1 GiB), level 2 (2 MiB), and level 3 (4 KiB) where a
 * region needs it.
 *
 * The register values (the MAIR bytes, TCR, and the SCR enable bits) and
 * the order they are written in follow TI's secure startup, from the PDK's
 * ti/csl/arch/c7x Mmu.c, which flushes no cache and no TLB before the MMU
 * goes on. One difference, found on the board: the table base goes in TBR0
 * as well as TBR0_S. With only TBR0_S, the core stops as soon as the MMU
 * is on.
 */

#include <stdint.h>
#include <c7x.h>

#include "ai64_dsp_job.h"
#include "ai64_fabric.h"
#include "ai64_pipe.h"
#include "mmu.h"

/* regs.asm */
extern void ai64_set_mair(uint64_t mair);
extern void ai64_set_tcr(uint64_t tcr);
extern void ai64_set_tbr(uint64_t table);
extern void ai64_scr_or(uint64_t bits);
extern void ai64_set_l1dcfg(uint64_t cfg);
extern void ai64_set_l2cfg(uint64_t cfg);

/* MAIR bytes 0..7, TI's: 0 is device nGnRnE, 4 normal non-cacheable,
 * 7 normal write-back. */
#define MAIR_VALUE   0x7dbb4f440c080400ull
#define ATTR_DEVICE  0u
#define ATTR_NC      4u
#define ATTR_WB      7u

/*
 * TCR: 4 KiB granule, tables in write-back memory (outer shareable, inner
 * and outer cacheable), a 48-bit address space (T0SZ = 16), walks on.
 */
#define TCR_VALUE    (0x8000ull | 0x0400ull | 0x2000ull | 0x0800ull | \
		      ((64ull - 48ull) << 1) | 1ull)

/* SCR: bit 0 turns translation on. Bits 6, 7, and 63 are set as TI does. */
#define SCR_MMU_ON   0x80000000000000c1ull

/* L1DCFG and L2CFG sizes. */
#define L1D_32K      4u
#define L2_64K       2u

#define DESC_TABLE   3ull
#define DESC_BLOCK   1ull           /* a level 1 or 2 block */
#define DESC_PAGE    3ull           /* a level 3 page */
#define DESC_AF      (1ull << 10)   /* access flag */
#define DESC_SH_OUT  (2ull << 8)    /* outer shareable */
#define DESC_PXN     (1ull << 53)   /* no execution in supervisor mode */
#define DESC_UXN     (1ull << 54)   /* no execution in user mode */
#define DESC_ATTR(i) ((uint64_t)(i) << 2)

#define SZ_4K   0x1000ull
#define SZ_2M   0x200000ull
#define SZ_1G   0x40000000ull

#define L2SRAM_BASE   0x64800000ull
#define CARVEOUT_BASE 0xa8000000ull
#define CARVEOUT_SIZE 0x01000000ull

/* The tables: level 0 and 1, one level 2 for each GiB that needs one,
 * and level 3 for the L2 SRAM block and for the fabric. */
#pragma DATA_SECTION(mmu_tables, ".ai64_mmu")
#pragma DATA_ALIGN(mmu_tables, 4096)
static uint64_t mmu_tables[6][512];

#define L0      mmu_tables[0]
#define L1      mmu_tables[1]
#define L2_1G   mmu_tables[2] /* 0x40000000 */
#define L2_2G   mmu_tables[3] /* 0x80000000 */
#define L3_SRAM mmu_tables[4] /* 0x64800000 */
#define L3_FAB  mmu_tables[5] /* 0xa9000000 */

static uint64_t leaf(uint64_t type, uint64_t addr, uint32_t attr)
{
	uint64_t desc = type | addr | DESC_ATTR(attr) | DESC_AF | DESC_SH_OUT | DESC_UXN;

	if (attr == ATTR_DEVICE) {
		desc |= DESC_PXN;
	}
	return desc;
}

static uint64_t table(const uint64_t *next)
{
	return DESC_TABLE | (uint64_t)(uintptr_t)next;
}

void ai64_c7x_mmu_init(void)
{
	uint64_t addr;
	uint32_t i;

	for (i = 0; i < 6u * 512u; i++) {
		mmu_tables[i / 512u][i % 512u] = 0;
	}

	L0[0] = table(L1);
	L1[0] = leaf(DESC_BLOCK, 0, ATTR_DEVICE);
	L1[1] = table(L2_1G);
	L1[2] = table(L2_2G);

	for (i = 0; i < 512u; i++) {
		L2_1G[i] = leaf(DESC_BLOCK, SZ_1G + i * SZ_2M, ATTR_DEVICE);
	}
	L2_1G[(L2SRAM_BASE - SZ_1G) / SZ_2M] = table(L3_SRAM);
	L3_SRAM[0] = leaf(DESC_PAGE, L2SRAM_BASE, ATTR_NC);
	for (i = 1; i < AI64_JOB_SIZE / SZ_4K; i++) {
		L3_SRAM[i] = leaf(DESC_PAGE, L2SRAM_BASE + i * SZ_4K, ATTR_WB);
	}
	for (i = 0; i < AI64_PIPE_C7X_SIZE / SZ_4K; i++) {
		L3_SRAM[(AI64_PIPE_C7X_ADDR - L2SRAM_BASE) / SZ_4K + i] =
			leaf(DESC_PAGE, AI64_PIPE_C7X_ADDR + i * SZ_4K, i == 0 ? ATTR_NC : ATTR_WB);
	}

	L2_2G[(CARVEOUT_BASE - 2u * SZ_1G) / SZ_2M] =
		leaf(DESC_BLOCK, CARVEOUT_BASE, ATTR_NC);
	for (addr = CARVEOUT_BASE + SZ_2M; addr < CARVEOUT_BASE + CARVEOUT_SIZE; addr += SZ_2M) {
		L2_2G[(addr - 2u * SZ_1G) / SZ_2M] = leaf(DESC_BLOCK, addr, ATTR_WB);
	}

	L2_2G[(AI64_FAB_ADDR - 2u * SZ_1G) / SZ_2M] = table(L3_FAB);
	for (i = 0; i < AI64_FAB_SIZE / SZ_4K; i++) {
		L3_FAB[i] = leaf(DESC_PAGE, AI64_FAB_ADDR + i * SZ_4K, ATTR_NC);
	}
	for (i = 0; i < AI64_PIPE_SIZE / SZ_4K; i++) {
		L3_FAB[(AI64_PIPE_ADDR - AI64_FAB_ADDR) / SZ_4K + i] =
			leaf(DESC_PAGE, AI64_PIPE_ADDR + i * SZ_4K, ATTR_NC);
	}

	ai64_set_mair(MAIR_VALUE);
	ai64_set_tcr(TCR_VALUE);
	ai64_set_tbr((uint64_t)(uintptr_t)L0);
	ai64_set_l2cfg(L2_64K);
	ai64_set_l1dcfg(L1D_32K);
	__memory_fence(0x1);
	ai64_scr_or(SCR_MMU_ON);
}
