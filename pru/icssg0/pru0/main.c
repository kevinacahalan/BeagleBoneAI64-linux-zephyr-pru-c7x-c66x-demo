/* SPDX-License-Identifier: MIT */
/*
 * PRU0 firmware (ICSSG0 slice 0). This core sends the SLIP frame.
 *
 * It polls the X/Y record RTU0 publishes in ICSSG0 shared RAM. For each new
 * record it sends one SLIP frame as UART 8N1 on P8.11 (R30 bit 17), then
 * writes the record's seq into consumed. The frame and the bit timing live
 * in slip_tx.asm. This core has no RPMsg; its log is the remoteproc trace.
 */

#include <stdint.h>
#include "ai64_shm.h"

#include <am65x/pru_cfg.h>

#define AI64_TRACE_SIZE 512
#define AI64_TRACE_NAME "trace:pru0"
#include "ai64_rsc_trace.h"

extern void transmit_xy(uint32_t x, uint32_t y);

volatile register uint32_t __R30;

void main(void)
{
	volatile struct ai64_xy *xy = (volatile struct ai64_xy *)AI64_XY_LOCAL;
	uint32_t seen;

	trace_init();

	/* GPO mode 0: R30 drives the PRG0_PRU0_GPO pins directly. */
	CT_CFG.gpcfg0_reg = 0;
	/* UART idle is high. */
	__R30 |= (1u << AI64_XY_TX_BIT);

	/* Adopt the seq already in RAM so a stale record is not a new one. */
	seen = xy->seq;
	trace_puts("pru0 up, SLIP TX on P8.11\n");

	while (1) {
		if (xy->magic == AI64_XY_MAGIC) {
			uint32_t seq = xy->seq;

			if (seq != seen) {
				uint32_t x = xy->x;
				uint32_t y = xy->y;

				if (xy->seq == seq && xy->magic == AI64_XY_MAGIC) {
					seen = seq;
					transmit_xy(x, y);
					xy->consumed = seq;
					trace_puts("pru0 sent xy seq=");
					trace_u32(seq);
					trace_puts(" x=");
					trace_u32(x);
					trace_puts(" y=");
					trace_u32(y);
					trace_putc('\n');
				}
			}
		}
	}
}
