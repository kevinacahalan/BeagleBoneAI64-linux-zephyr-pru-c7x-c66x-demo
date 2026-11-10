/* SPDX-License-Identifier: MIT */
/*
 * ICSSG1's memory map for the logic analyzer: how each slice splits its
 * 8 KiB of data RAM. Every core sees its own slice's data RAM at local
 * 0x0000, so one map serves both slices. The C code and every core's
 * linker.cmd include this file, so it holds #defines only.
 *
 *   0x0000 - 0x07ff  PRU     2 KiB  stack, data, trace
 *   0x0c00 - 0x1bff  RTU     4 KiB
 *   0x1f00           slice 0: the analyzer's setup (struct ai64_la_cfg,
 *                    zephyr/include/ai64_la.h), Linux's and PRU0's
 *
 * The Tx_PRUs and PRU1 run nothing in the analyzer: PRU0 samples, and
 * writes the samples to DDR through XFR2VBUS (la_cap.asm). The RTUs are
 * fabric nodes (la_rtu.c).
 *
 * ICSSG1's task managers and IEP0 keep their setup when its cores stop: the
 * multicore scheduler (extra-demos/) leaves IEP0 firing a compare every
 * 100 ns and each core's task manager armed. la_tm.h has how the analyzer
 * starts clean after it.
 */
#ifndef LA_MAP_H_
#define LA_MAP_H_

#define LA_PRU_DRAM        0x00000000
#define LA_PRU_DRAM_SIZE   0x00000800
#define LA_RTU_DRAM        0x00000c00
#define LA_RTU_DRAM_SIZE   0x00001000

/* Instruction RAM: 12 KiB on a PRU, 8 KiB on an RTU. */
#define LA_PRU_IRAM_SIZE   0x00003000
#define LA_RTU_IRAM_SIZE   0x00002000

#endif
