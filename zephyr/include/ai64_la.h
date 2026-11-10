/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer on ICSSG1 (pru/logic_analyzer/): its 10 channels, how
 * it samples them, and where the samples go. The ICSSG1 firmware and Linux
 * (ai64 la) include this file.
 *
 * PRU0 samples its GPI pins, every channel at once, at a fixed rate, every
 * period PRU cycles (4 ns each), with no gap, and packs them into blocks:
 * 15 words of samples and a tag. Each block goes to XFR2VBUS (SPRUJ61
 * 7.3.1), which writes its 64 bytes to the ring in DDR without the PRU
 * waiting for DDR, so the PRU keeps sampling whatever the pins do, at any
 * rate in the menu. Linux finds the changes after. Nothing else in ICSSG1
 * takes part, and nothing waits for Linux.
 *
 * Block n of a run goes to slot n of the ring, round again when full; its
 * tag is the run's key + n, so a slot that does not hold it holds another
 * run's block, or one written over, or one never written: XFR2VBUS was
 * still busy with the block before, when the SoC held DDR, and PRU0
 * dropped this one. Its samples are lost then, which Linux marks as a hole.
 * The PRU keeps no count: sample k of block n is at t0 + (n * spb + k) *
 * period * 4 ns, spb the samples a block, so Linux knows from IEP0 which
 * blocks are due.
 *
 * A sample word holds one, two, or four samples, by the lane, which the
 * pins recorded fit: every GPI bit, a sample a word; bits 0-15 or 8-23, two
 * a word, the first in the low half; or bits 0-7 or 8-15, four a word, the
 * first in the low byte. Fewer bytes a sample, fewer to write and read.
 * At 25 MS/s and slower the lane is always every bit: the bytes are few.
 */
#ifndef AI64_LA_H_
#define AI64_LA_H_

#include <stdint.h>

/* The ring, in the 64 MiB the overlay reserves (the rest unused). */
#define AI64_LA_RES_ADDR   0xAC000000u
#define AI64_LA_RES_SIZE   0x04000000u
#define AI64_LA_RING_ADDR  AI64_LA_RES_ADDR
#define AI64_LA_RING_SIZE  0x02000000u
#define AI64_LA_SLOT       64u
#define AI64_LA_SLOTS      (AI64_LA_RING_SIZE / AI64_LA_SLOT)
#define AI64_LA_WORDS      15u /* sample words a block; word 15 is the tag */

#define AI64_LA_LANE_ALL   0u /* every GPI bit, a sample a word */
#define AI64_LA_LANE_LO16  1u /* bits 0 - 15, two a word */
#define AI64_LA_LANE_MID16 2u /* bits 8 - 23, two a word */
#define AI64_LA_LANE_B0    3u /* bits 0 - 7, four a word */
#define AI64_LA_LANE_B1    4u /* bits 8 - 15, four a word */
#define AI64_LA_LANES      5u
#define AI64_LA_PER_WORD(lane) ((lane) == AI64_LA_LANE_ALL ? 1u : (lane) <= AI64_LA_LANE_MID16 ? 2u : 4u)
#define AI64_LA_LANE_SHIFT(lane) ((lane) == AI64_LA_LANE_MID16 || (lane) == AI64_LA_LANE_B1 ? 8u : 0u)

/*
 * The rates, X(samples a second, PRU cycles a sample). Two cycles is the
 * fastest the loop goes: a sample, and one instruction more.
 */
#define AI64_LA_RATES(X) \
	X(125000000u, 2u) \
	X(62500000u, 4u) \
	X(25000000u, 10u) \
	X(12500000u, 20u) \
	X(6250000u, 40u) \
	X(2500000u, 100u) \
	X(1250000u, 200u)
#define AI64_LA_NS_PER_CYCLE 4u
/* The slowest period with lanes; slower ones take every bit. */
#define AI64_LA_LANE_PERIOD_MAX 4u

/*
 * The run's setup, in PRU0's data RAM at AI64_LA_CFG (ICSSG1_DRAM0 +
 * 0x1f00 to Linux). Linux stops PRU0, writes the first five words, run 0,
 * and starts it (ai64 la mode); PRU0 sets t0 and run. With no magic, as
 * at boot, PRU0 takes the default, every pin at 25 MS/s, and writes it
 * back.
 */
struct ai64_la_cfg {
	uint32_t magic;     /* AI64_LA_CFG_MAGIC */
	uint32_t mask;      /* the GPI bits to record, of AI64_LA_MASK */
	uint32_t lane;      /* AI64_LA_LANE_*, which the mask fits */
	uint32_t period;    /* PRU cycles a sample: AI64_LA_RATES */
	uint32_t key;       /* block 0's tag */
	uint32_t t0;        /* IEP0's count, ns, just before the first sample */
	uint32_t t0_hi;     /* and its upper 32 bits */
	uint32_t run;       /* AI64_LA_RUN_MAGIC once t0 is set */
};

#define AI64_LA_CFG        0x00001f00u
#define AI64_LA_CFG_MAGIC  0x4c414347u /* 'LACG' */
#define AI64_LA_RUN_MAGIC  0x4c415255u /* 'LARU' */
#define AI64_LA_DEFAULT_PERIOD 10u

/*
 * The channels, in header pin order: P8 before P9, then by pin number.
 * X(channel, pin, ball, pad, GPI bit). The overlay muxes each pad to
 * PRG1_PRU0_GPI<bit>, mode 1, with its pull-down on. P8.38 has a second
 * ball, which the overlay holds off.
 */
#define AI64_LA_CHANNELS(X) \
	X(0, "P8.03", "AH21", 0x054, 19) \
	X(1, "P8.07", "AD24", 0x03c, 14) \
	X(2, "P8.08", "AG24", 0x038, 13) \
	X(3, "P8.09", "AE24", 0x044, 16) \
	X(4, "P8.10", "AC24", 0x040, 15) \
	X(5, "P8.36", "AE20", 0x020, 7) \
	X(6, "P8.38", "AJ20", 0x024, 8) \
	X(7, "P9.11", "AC23", 0x004, 0) \
	X(8, "P9.13", "AG22", 0x008, 1) \
	X(9, "P9.23", "AG20", 0x028, 9)

#define AI64_LA_NCHAN 10u

/* PRU0's GPI pins, the bits of the table above. */
#define AI64_LA_MASK 0x0009e383u /* 0 1 7 8 9 13 14 15 16 19 */

#endif
