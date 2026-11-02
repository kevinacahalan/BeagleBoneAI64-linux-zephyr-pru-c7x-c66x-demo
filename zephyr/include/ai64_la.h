/* SPDX-License-Identifier: MIT */
/*
 * The logic analyzer on ICSSG1 (pru/logic_analyzer/): its 19 channels, the
 * records its cores write, and where. The ICSSG1 firmware and Linux (ai64
 * la) include this file.
 *
 * Each PRU watches its own GPI pins and, at every change, writes an edge
 * record into a ring in ICSSG1's shared RAM. Its slice's RTU copies the
 * records into that PRU's ring in DDR, adds a tick every millisecond, and
 * keeps the stream's header here up to date. Linux reads both rings and
 * merges them by time. Nothing waits for Linux: a ring Linux falls a whole
 * ring behind on has been written over, which Linux sees from wr.
 *
 * A record is two words:
 *
 *   edge  time   IEP0's count, ns, low 32 bits, when the PRU saw the change
 *         state  the PRU's GPI pins (R31 & its mask), bits 31:24 zero
 *   tick  time   IEP0's count, low 32 bits
 *         state  AI64_LA_TICK << 24 | the state at that time
 *         and the next record holds the GTC's low word, then the GTC's and
 *         IEP0's bits 47:32: gtc_hi16 << 16 | iep_hi16
 *   gap   time   IEP0's count when the RTU found it had fallen behind
 *         state  AI64_LA_GAP << 24 | the records it skipped (24 bits)
 *
 * IEP0 counts nanoseconds on the 250 MHz core clock, from when RTU0 starts.
 * An edge's 32 bits are a time within about 2 s of its stream's last tick.
 */
#ifndef AI64_LA_H_
#define AI64_LA_H_

#include <stdint.h>

/* The streams' headers, in the fabric's reservation (MEMORY.md). */
#define AI64_LA_ADDR       0xA90A0000u
#define AI64_LA_MAGIC      0x4c413634u /* 'LA64' */

/* Each PRU's ring, in the 64 MiB the overlay reserves for it. */
#define AI64_LA_RES_ADDR   0xAC000000u
#define AI64_LA_RES_SIZE   0x04000000u
#define AI64_LA_RING_SIZE  0x02000000u
#define AI64_LA_RING_ADDR(s) (AI64_LA_RES_ADDR + (uint32_t)(s) * AI64_LA_RING_SIZE)

#define AI64_LA_REC_SIZE   8u
#define AI64_LA_TICK       0x80u
#define AI64_LA_GAP        0x81u
#define AI64_LA_TYPE(state) ((state) >> 24)
#define AI64_LA_TICK_GTC   (200000000u / 1000u) /* a tick each ms, in GTC counts */

/* One stream, one PRU: its RTU writes it, Linux reads it. */
struct ai64_la_stream {
	uint32_t magic;     /* AI64_LA_MAGIC once the RTU runs */
	uint32_t wr;        /* bytes written to the ring, free-running */
	uint32_t edges;     /* edge records copied */
	uint32_t gaps;      /* times the RTU fell behind the PRU */
	uint32_t lost;      /* records it skipped then */
	uint32_t state;     /* the newest state it copied */
	uint32_t ticks;
	uint32_t mask;      /* the PRU's GPI pins */
	uint32_t ring;      /* the ring's SoC address */
	uint32_t ring_size;
	uint32_t pad[6];
};

struct ai64_la {
	struct ai64_la_stream s[2];
};

/*
 * The channels, in header pin order: P8 before P9, then by pin number.
 * X(channel, pin, ball, pad, PRU, GPI bit). The overlay muxes each pad to
 * PRG1_PRU<n>_GPI<bit>, mode 1, with its pull-down on. P8.32, P8.33, P8.35,
 * P8.38, and P9.21 have a second ball, which the overlay holds off.
 */
#define AI64_LA_CHANNELS(X) \
	X(0, "P8.03", "AH21", 0x054, 0, 19) \
	X(1, "P8.05", "AH25", 0x088, 1, 12) \
	X(2, "P8.06", "AG25", 0x08c, 1, 13) \
	X(3, "P8.07", "AD24", 0x03c, 0, 14) \
	X(4, "P8.08", "AG24", 0x038, 0, 13) \
	X(5, "P8.09", "AE24", 0x044, 0, 16) \
	X(6, "P8.10", "AC24", 0x040, 0, 15) \
	X(7, "P8.21", "AF21", 0x07c, 1, 9) \
	X(8, "P8.23", "AB23", 0x080, 1, 10) \
	X(9, "P8.25", "AH26", 0x090, 1, 14) \
	X(10, "P8.32", "AG21", 0x06c, 1, 5) \
	X(11, "P8.33", "AH24", 0x068, 1, 4) \
	X(12, "P8.35", "AD23", 0x064, 1, 3) \
	X(13, "P8.36", "AE20", 0x020, 0, 7) \
	X(14, "P8.38", "AJ20", 0x024, 0, 8) \
	X(15, "P9.11", "AC23", 0x004, 0, 0) \
	X(16, "P9.13", "AG22", 0x008, 0, 1) \
	X(17, "P9.21", "AJ22", 0x0a0, 1, 18) \
	X(18, "P9.23", "AG20", 0x028, 0, 9)

#define AI64_LA_NCHAN 19u

/* Each PRU's GPI pins, the bits of the table above. */
#define AI64_LA_MASK_PRU0 0x0009e383u /* 0 1 7 8 9 13 14 15 16 19 */
#define AI64_LA_MASK_PRU1 0x00047638u /* 3 4 5 9 10 12 13 14 18 */
#define AI64_LA_MASK(s)   ((s) ? AI64_LA_MASK_PRU1 : AI64_LA_MASK_PRU0)

#endif
