/* SPDX-License-Identifier: MIT */
/*
 * ai64 pads
 *   Prints the pad registers of every pin the demo muxes, next to the value
 *   the overlay and the Zephyr devicetree ask for.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>

#include "ai64.h"

/* PINMUX.md has the same table. */
static const struct {
	uint32_t pad;
	uint32_t want;
	const char *pin;
} pads[] = {
	{ 0x0f4, 0x10000, "P8.11 AB24 ICSSG0 PRU0 GPO17" },
	{ 0x0f0, 0x10000, "P8.12 AH28 ICSSG0 PRU0 GPO16" },
	{ 0x0f8, 0x10004, "P8.15 AB29 PRG0_ECAP0_IN_APWM_OUT" },
	{ 0x0b8, 0x10003, "P9.12 AE27 PRG0_PWM2_A0" },
	{ 0x0c0, 0x10003, "P9.15 AD25 PRG0_PWM2_B0" },
	{ 0x100, 0x54001, "P8.31 AE29 PRG0_PRU1_GPI0, the pipeline's input" },
	{ 0x084, 0x200007, "P8.31 AJ25 held off" },
	{ 0x00c, 0x10003, "P8.17 AF22 PRG1_PWM2_A0" },
	{ 0x1a4, 0x10006, "P9.25 W26 Zephyr R5F0_1 EHRPWM4_B" },
	{ 0x200, 0x200007, "P9.25 AC4 held off" },
	{ 0x230, 0x10006, "P9.28 U2 Zephyr SPI7_CS0" },
	{ 0x0b0, 0x200007, "P9.28 AF28 held off" },
	{ 0x238, 0x10006, "P9.30 V6 Zephyr SPI7_D0" },
	{ 0x0b4, 0x200007, "P9.30 AE28 held off" },
	{ 0x234, 0x10006, "P9.31 U3 Zephyr SPI7_CLK" },
	{ 0x0d4, 0x200007, "P9.31 AB26 held off" },
	{ 0x01c, 0x1000e, "P8.34 AD22 Zephyr UART2_TXD" },
	{ 0x014, 0x5000e, "P8.22 AH23 Zephyr UART2_RXD" },
	{ 0x010, 0x10017, "P8.18 AJ23 Zephyr R5F1_0 GPIO2_4" },
};

int pads_main(int argc, char **argv)
{
	volatile uint32_t *padcfg = map_phys(0x0011c000, 4096);
	size_t i;
	int bad = 0;

	(void)argc;
	(void)argv;
	if (!padcfg) {
		return 1;
	}
	for (i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
		uint32_t v = padcfg[pads[i].pad / 4];
		int ok = v == pads[i].want;

		printf("0x%03x %-36s 0x%08x %s\n", pads[i].pad, pads[i].pin, v,
		       ok ? "ok" : "DIFFERENT");
		bad |= !ok;
	}
	if (bad) {
		printf("A Zephyr pad changes once its core has booted: SPI7 and UART2 on R5F0_0, P9.25 on\n"
		       "R5F0_1, P8.18 on R5F1_0. The others need the overlay.\n");
	}
	return bad;
}
