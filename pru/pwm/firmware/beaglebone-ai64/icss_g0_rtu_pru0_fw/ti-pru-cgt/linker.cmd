/*
 * SPDX-License-Identifier: MIT
 * linker.cmd: ICSSG0 RTU0, slice 0's talking core, with RPMsg (pwm_rtu.c).
 * Its part of the slice's data RAM comes from the project's memory map.
 */

-cr		/* Link using C conventions */

#include "pwm_map.h"

MEMORY
{
      PAGE 0:
	IMEM	: org = 0x00000000 len = PWM_RTU_IRAM_SIZE

      PAGE 1:
	DMEM	: org = PWM_RTU_DRAM len = PWM_RTU_DRAM_SIZE

      PAGE 2:
	/* The INTC, for CT_INTC (pru_intc.h). The full length matches the
	 * pruIntc structure. */
	PRU_INTC	: org = 0x00020000 len = 0x00001504	CREGISTER=0
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
	/* The fabric node's state (ai64_fabric.h) */
	.ai64_fab	>  DMEM, PAGE 1

	.pru_irq_map (COPY) :
	{
		*(.pru_irq_map)
	}
}
