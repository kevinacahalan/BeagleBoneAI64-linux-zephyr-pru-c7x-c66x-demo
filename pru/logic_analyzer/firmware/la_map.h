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
 *   0x0c00 - 0x1bff  RTU     4 KiB
 *   0x1c00           the slice's RTU, Tx_PRU, and PRU share: the trigger
 *                    as the RTU passes it on, then at 0x1c40 the Tx_PRU's
 *                    trigger hits, a write count and a ring of 64; at
 *                    0x1ff0 the RTU's word that the ring is ready
 *
 * Shared RAM, local 0x10000, 64 KiB: each PRU's ring of edge records,
 * 32 KiB, 2048 records, PRU0's at 0x10000 and PRU1's at 0x18000.
 *
 * A record in a ring (struct la_srec) is the edge's time and state, as in
 * DDR (ai64_la.h), and its seq: the bytes the PRU had written before it,
 * free-running, a multiple of 16. The PRU writes the three words in one
 * store and keeps no count, and so does nothing else for an edge: a reader
 * at rd (bytes, free-running) has a new record when the one at rd's slot
 * has seq == rd; one less is the last lap's, not yet written over; one
 * more, the PRU has gone a lap past it. Before its PRU starts, the RTU
 * gives each slot the seq of the lap before the first.
 *
 * Both the RTU and the Tx_PRU read the ring as fast as the records come:
 * the RTU copies them into the PRU's ring in DDR, and an RTU more than a
 * ring behind skips to where the PRU is and writes a gap record
 * (ai64_la.h); the Tx_PRU matches the trigger, and writes each hit, time
 * and state, into its hit ring for the RTU.
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
#define LA_RTU_DRAM_SIZE   0x00001000

/* Instruction RAM: 12 KiB on a PRU, 8 KiB on an RTU, 6 KiB on a Tx_PRU. */
#define LA_PRU_IRAM_SIZE   0x00003000
#define LA_RTU_IRAM_SIZE   0x00002000
#define LA_TXPRU_IRAM_SIZE 0x00001800

#define LA_SRING(s)        (0x00010000 + (s) * 0x8000)
#define LA_SRING_SIZE      0x00008000
#define LA_SREC_SIZE       16
#define LA_TRIG            0x00001c00
#define LA_HITS            0x00001c40
#define LA_HIT_RING        64
#define LA_READY           0x00001ff0
#define LA_READY_MAGIC     0x52454459

#endif
