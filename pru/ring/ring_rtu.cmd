/*
 * ring_rtu.cmd
 *
 * Linker script for the ring RTU of any ICSSG slice (ring/main.c). Each
 * slice sees its own data RAM at local 0x0000 and the other slice's at
 * 0x2000, so one script serves both slices.
 *
 * The three ring cores of a slice share the first 4 KiB of its data RAM:
 * the PRU 0x000-0x7ff, the RTU 0x800-0xbff, the Tx_PRU 0xc00-0xfff. In
 * ICSSG0 the upper 4 KiB of slice 1's data RAM is not free: RTU0 and
 * Tx_PRU0 keep their stack and trace there (their linker scripts).
 */

-cr		/* Link using C conventions */

MEMORY
{
      PAGE 0:
	IMEM	: org = 0x00000000 len = 0x00002000

      PAGE 1:
	/* This core's part of its slice's data RAM */
	DMEM	: org = 0x00000800 len = 0x00000400
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
	.rodata		>  DMEM, PAGE 1
	.rofardata	>  DMEM, PAGE 1
	.farbss		>  DMEM, PAGE 1
	.fardata	>  DMEM, PAGE 1

	/* 8-byte aligned for the ARMv8 kernel */
	.resource_table : ALIGN (8) >  DMEM, PAGE 1
	.ai64_trace : ALIGN (8) >  DMEM, PAGE 1
}
