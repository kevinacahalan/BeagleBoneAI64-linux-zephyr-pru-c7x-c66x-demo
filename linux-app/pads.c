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
	{ 0x054, 0x44001, "P8.03 AH21 PRG1_PRU0_GPI19, LA ch0" },
	{ 0x088, 0x44001, "P8.05 AH25 PRG1_PRU1_GPI12, free" },
	{ 0x08c, 0x44001, "P8.06 AG25 PRG1_PRU1_GPI13, free" },
	{ 0x03c, 0x44001, "P8.07 AD24 PRG1_PRU0_GPI14, LA ch1" },
	{ 0x038, 0x44001, "P8.08 AG24 PRG1_PRU0_GPI13, LA ch2" },
	{ 0x044, 0x44001, "P8.09 AE24 PRG1_PRU0_GPI16, LA ch3" },
	{ 0x040, 0x44001, "P8.10 AC24 PRG1_PRU0_GPI15, LA ch4" },
	{ 0x07c, 0x44001, "P8.21 AF21 PRG1_PRU1_GPI9, free" },
	{ 0x080, 0x44001, "P8.23 AB23 PRG1_PRU1_GPI10, free" },
	{ 0x090, 0x44001, "P8.25 AH26 PRG1_PRU1_GPI14, free" },
	{ 0x06c, 0x44001, "P8.32 AG21 PRG1_PRU1_GPI5, free" },
	{ 0x104, 0x200007, "P8.32 AD28 held off" },
	{ 0x068, 0x44001, "P8.33 AH24 PRG1_PRU1_GPI4, free" },
	{ 0x1c0, 0x200007, "P8.33 AA2 held off" },
	{ 0x064, 0x44001, "P8.35 AD23 PRG1_PRU1_GPI3, free" },
	{ 0x1d4, 0x200007, "P8.35 Y3 held off" },
	{ 0x020, 0x44001, "P8.36 AE20 PRG1_PRU0_GPI7, LA ch5" },
	{ 0x024, 0x44001, "P8.38 AJ20 PRG1_PRU0_GPI8, LA ch6" },
	{ 0x1a8, 0x200007, "P8.38 Y29 held off" },
	{ 0x004, 0x44001, "P9.11 AC23 PRG1_PRU0_GPI0, LA ch7" },
	{ 0x008, 0x44001, "P9.13 AG22 PRG1_PRU0_GPI1, LA ch8" },
	{ 0x0a0, 0x44001, "P9.21 AJ22 PRG1_PRU1_GPI18, free" },
	{ 0x16c, 0x200007, "P9.21 U28 held off" },
	{ 0x028, 0x44001, "P9.23 AG20 PRG1_PRU0_GPI9, LA ch9" },
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
