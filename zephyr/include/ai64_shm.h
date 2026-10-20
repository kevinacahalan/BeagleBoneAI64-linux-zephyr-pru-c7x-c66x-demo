/* SPDX-License-Identifier: MIT */
/*
 * Shared layout for the BeagleBone AI-64 demo. Linux, Zephyr, and every
 * ICSSG core include this file.
 *
 * Mailbox: Zephyr writes, RTU0 polls. In DDR, inside R5F0_0's carveout.
 * Bulk region: Linux writes chunks, Zephyr reads them. Next to the mailbox.
 * Arm block: RTU0 writes it in ICSSG0 shared RAM. Tx_PRU0 reads it and
 *   sends that many pulses on P9.25.
 * X/Y record: RTU0 writes it in ICSSG0 shared RAM. PRU0 reads it and sends
 *   one SLIP frame on P8.11.
 * Ring records: the PRU, RTU, and Tx_PRU of each other ICSSG slice pass a
 *   token around through one, in their ICSSG's shared RAM.
 *
 * Do not put a loadable ELF section on the DDR addresses. The Zephyr overlay
 * and these macros must stay on the same addresses.
 */
#ifndef AI64_SHM_H_
#define AI64_SHM_H_

#include <stdint.h>

/*
 * R5F0_0 owns 0xa2000000-0xa2ffffff (Linux reserved-memory, no-map).
 * Zephyr's board file splits it into vrings (1 MiB), resource table (1 MiB),
 * and DRAM (14 MiB at 0xa2200000). zephyr/r5/boards/ shrinks DRAM so the last
 * 68 KiB hold the bulk buffer and the mailbox.
 */
#define AI64_SHM_ADDR   0xA2FFF000u
#define AI64_SHM_MAGIC  0x41493653u /* 'AI6S' */

#define AI64_BULK_ADDR  0xA2FEF000u
#define AI64_BULK_SIZE  0x00010000u
#define AI64_BULK_MAGIC 0x41493642u /* 'AI6B' */

/*
 * ICSSG0 shared RAM. 64 KiB at local 0x00010000 for every core in ICSSG0.
 * The SoC address of local 0x00010000 is 0x0B010000 (ICSSG0 at 0x0B000000).
 */
#define AI64_ARM_LOCAL  0x00010000u
#define AI64_ARM_CPU    0x0B010000u
#define AI64_ARM_MAGIC  0x41493641u /* 'AI6A' */

#define AI64_XY_LOCAL   0x00010040u
#define AI64_XY_CPU     0x0B010040u
#define AI64_XY_MAGIC   0x41493658u /* 'AI6X' */

/*
 * The ring records, one per slice, in its ICSSG's shared RAM: ICSSG0 slice
 * 1, ICSSG1 slice 0, and ICSSG1 slice 1 (ICSSG1 is at 0x0B100000). ICSSG0
 * slice 0 runs the demo above instead.
 */
#define AI64_RING_LOCAL(slice)      (0x00010100u + 0x40u * (slice))
#define AI64_RING_CPU(icssg, slice) (0x0B010100u + 0x100000u * (icssg) + 0x40u * (slice))
#define AI64_RING_MAGIC             0x41493652u /* 'AI6R' */

/* SLIP (RFC 1055) framing of the X/Y payload. */
#define AI64_SLIP_END      0xC0u
#define AI64_SLIP_ESC      0xDBu
#define AI64_SLIP_ESC_END  0xDCu
#define AI64_SLIP_ESC_ESC  0xDDu

/*
 * PRU0 shifts each SLIP byte out as UART 8N1 on R30 bit 17.
 * Header P8.11, ball AB24, pad offset 0x0f4, mux mode 0 (PRG0_PRU0_GPO17).
 * The ICSSG core clock on the BeagleBoard Debian image is 250 MHz.
 * 250000000 / 115200 is 2170 cycles per bit. slip_tx.asm delays that long
 * per bit, plus a few cycles of loop overhead (about 0.3% slow).
 */
#define AI64_XY_TX_BIT        17u
#define AI64_PRU_HZ           250000000u
#define AI64_UART_BAUD        115200u
#define AI64_UART_BIT_CYCLES  2170u

/*
 * Tx_PRU0 has no pins of its own: its R30 does not reach the pads. It drives
 * P9.25 through the main GPIO0 module instead, with stores to the bank-3
 * set/clear registers. P9.25 is ball W26, GPIO0_104 (pad 0x1a4, mode 7).
 * The overlay muxes the pad and hogs the line as an output, so Linux sets
 * the direction once and Tx_PRU0 never writes DIR.
 */
#define AI64_PULSE_GPIO_SET   0x00600090u /* GPIO0 SET_DATA for lines 96-127 */
#define AI64_PULSE_GPIO_CLR   0x00600094u /* GPIO0 CLR_DATA for lines 96-127 */
#define AI64_PULSE_GPIO_MASK  (1u << (104u - 96u))

/* Pulse period in ICSSG cycles. 2500 cycles is 10 us; 250000000 is 1 s. */
#define AI64_ARM_PERIOD_MIN 2500u
#define AI64_ARM_PERIOD_MAX 250000000u
#define AI64_ARM_COUNT_MAX  100000u

struct ai64_shm {
	uint32_t magic;
	uint32_t seq;
	char cmd[56];
};

struct ai64_bulk {
	uint32_t magic;
	uint32_t seq;
	uint32_t length;
	uint32_t crc;
	uint8_t data[AI64_BULK_SIZE - 16u];
};

/*
 * RTU0 fills period_cycles, periods, magic, then seq. Tx_PRU0 writes the
 * last four fields: pulses sent so far, applied_seq when it starts a train,
 * and done_seq when the train ends.
 */
struct ai64_arm {
	uint32_t magic;
	uint32_t seq;
	uint32_t period_cycles;
	uint32_t periods;
	uint32_t sent;
	uint32_t applied_seq;
	uint32_t applied_period;
	uint32_t done_seq;
};

/* RTU0 fills x, y, magic, then seq. PRU0 writes consumed after the frame. */
struct ai64_xy {
	uint32_t magic;
	uint32_t seq;
	uint32_t x;
	uint32_t y;
	uint32_t consumed;
};

/*
 * A token goes round PRU, RTU, Tx_PRU, and back to the PRU. stage says
 * whose turn it is, and only that core writes the record. The PRU sets
 * value to the lap number; the RTU rotates it left 5 bits and XORs in
 * AI64_RING_RTU_XOR, and the Tx_PRU adds AI64_RING_TXPRU_ADD. The PRU
 * checks what comes back and counts a mismatch in errors.
 *
 * The PRU starts each run: it bumps run, zeroes the counts, and only then
 * sets magic. Lap times are in ICSSG cycles, the minimum and maximum over
 * the last window of AI64_RING_WINDOW laps.
 */
#define AI64_RING_PRU       0u
#define AI64_RING_RTU       1u
#define AI64_RING_TXPRU     2u
#define AI64_RING_RTU_XOR   0xA5A5A5A5u
#define AI64_RING_TXPRU_ADD 0x9E3779B9u
#define AI64_RING_WINDOW    65536u

struct ai64_ring {
	uint32_t magic;
	uint32_t run;
	uint32_t stage;
	uint32_t value;
	uint32_t laps;
	uint32_t rtu_hops;
	uint32_t txpru_hops;
	uint32_t errors;
	uint32_t lap_last;
	uint32_t lap_min;
	uint32_t lap_max;
};

static inline uint32_t ai64_ring_rtu(uint32_t v)
{
	return ((v << 5) | (v >> 27)) ^ AI64_RING_RTU_XOR;
}

static inline uint32_t ai64_ring_txpru(uint32_t v)
{
	return v + AI64_RING_TXPRU_ADD;
}

#ifndef PRU_CORE
static inline uint32_t ai64_crc32(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xffffffffu;
	uint32_t i, b;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 0; b < 8u; b++) {
			uint32_t mask = -(crc & 1u);

			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}
	return ~crc;
}
#endif

#endif
