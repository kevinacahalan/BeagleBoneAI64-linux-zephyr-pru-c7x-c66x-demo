/*
 * linker.cmd: ICSSG1 Tx_PRU0, the multicore scheduler.
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/
 *
 * From open-pru's icss_g0_tx_pru0_fw linker.cmd, with only the regions this
 * core uses: its instruction RAM and its part of the slice's data RAM
 * (mcs_map.h). The task's register writes go through constants C5 (CFG +
 * 0x100) and C26 (IEP0), which the hardware fixes.
 */

-cr		/* Link using C conventions */

#include "mcs_map.h"

MEMORY
{
      PAGE 0:
	IMEM	: org = 0x00000000 len = MCS_TXPRU_IRAM_SIZE

      PAGE 1:
	DMEM	: org = MCS_TXPRU_DRAM len = MCS_TXPRU_DRAM_SIZE
}

SECTIONS {
	.text:_c_int00*	>  0x0, PAGE 0
	.text		>  IMEM, PAGE 0

	.stack		>  DMEM, PAGE 1
	.bss		>  DMEM, PAGE 1
	.cio		>  DMEM, PAGE 1
	.data		>  DMEM, PAGE 1
	.switch		>  DMEM, PAGE 1
	.sysmem		>  DMEM, PAGE 1
	.cinit		>  DMEM, PAGE 1
	/* pru_rproc copies a segment in 32-bit words and refuses one whose size
	 * is not a multiple of 4. palign pads the strings out to one. */
	.rodata		: palign(4) >  DMEM, PAGE 1
	.rofardata	>  DMEM, PAGE 1
	.farbss		>  DMEM, PAGE 1
	.fardata	>  DMEM, PAGE 1

	/* 8-byte aligned for the ARMv8 kernel */
	.resource_table : ALIGN (8) >  DMEM, PAGE 1
	.ai64_trace : ALIGN (8) >  DMEM, PAGE 1
}
