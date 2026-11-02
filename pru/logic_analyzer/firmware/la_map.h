/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer's memory map: how each ICSSG1 slice splits its 8 KiB
 * of data RAM between its three cores, and where in ICSSG1's shared RAM
 * each PRU leaves its edges for its RTU. Every core sees its own slice's
 * data RAM at local 0x0000, so one map serves both slices. The C code and
 * every core's linker.cmd include this file, so it holds #defines only.
 *
 *   0x0000 - 0x07ff  PRU     2 KiB  stack, data, trace
 *   0x0800 - 0x0bff  Tx_PRU  1 KiB
 *   0x0c00 - 0x1fff  RTU     5 KiB
 *
 * Shared RAM, local 0x10000, 64 KiB:
 *
 *   0x10000  PRU0's ring  16 KiB, 2048 records
 *   0x14000  PRU1's ring  16 KiB
 *   0x18000  PRU0's write count, then PRU1's at 0x18040
 *   0x18080  slice 0's trigger, as the RTU passes it on; slice 1's at 0x180c0
 *   0x18100  slice 0's trigger hits: a write count, then a ring of 64; slice
 *            1's at 0x18400
 *
 * A ring's write count is the bytes its PRU has written since it started,
 * free-running. The PRU writes the record first and the count after it, and
 * never looks at its RTU: an RTU more than a ring behind finds the count
 * past it, and skips to it (a gap record, ai64_la.h). The Tx_PRU reads the
 * same ring, as fast as the records come, to match the trigger; it writes
 * each hit, time and state, into its hit ring for the RTU.
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
#define LA_TXPRU_DRAM      0x00000800
#define LA_TXPRU_DRAM_SIZE 0x00000400
#define LA_RTU_DRAM        0x00000c00
#define LA_RTU_DRAM_SIZE   0x00001400

/* Instruction RAM: 12 KiB on a PRU, 8 KiB on an RTU, 6 KiB on a Tx_PRU. */
#define LA_PRU_IRAM_SIZE   0x00003000
#define LA_RTU_IRAM_SIZE   0x00002000
#define LA_TXPRU_IRAM_SIZE 0x00001800

#define LA_SRING(s)        (0x00010000 + (s) * 0x4000)
#define LA_SRING_SIZE      0x00004000
#define LA_SCTL(s)         (0x00018000 + (s) * 0x40)
#define LA_TRIG(s)         (0x00018080 + (s) * 0x40)
#define LA_HITS(s)         (0x00018100 + (s) * 0x300)
#define LA_HIT_RING        64

#endif
