/*
 * ring_rtu.cmd
 *
 * Linker script for the RTU of a ring slice without RPMsg (ring/rtu.c). Each
 * slice sees its own data RAM at local 0x0000 and the other slice's at
 * 0x2000, so one script serves both slices.
 *
 * A ring slice's three cores share the first 4 KiB of its data RAM: the
 * PRU 0x000-0x3ff, the Tx_PRU 0x400-0x7ff, the RTU 0x800-0xfff. In ICSSG0
 * the upper 4 KiB of slice 1's data RAM is not free: RTU0 and Tx_PRU0 keep
 * their stack and trace there (their linker scripts). In ICSSG1 it is, and
 * ICSSG1 RTU0, which has RPMsg, also uses its slice's (ring_rtu_rpmsg.cmd).
 */

-cr		/* Link using C conventions */

MEMORY
{
      PAGE 0:
	IMEM	: org = 0x00000000 len = 0x00002000

      PAGE 1:
	/* This core's part of its slice's data RAM */
	DMEM	: org = 0x00000800 len = 0x00000800
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
}
