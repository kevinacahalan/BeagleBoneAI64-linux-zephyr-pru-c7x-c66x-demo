/*
 * ring_rtu_rpmsg.cmd
 *
 * Linker script for ICSSG1 RTU0 (ring/rtu.c built with AI64_RPMSG). It sees
 * its slice's data RAM at local 0x0000.
 *
 * A ring slice's three cores share the first 4 KiB of its data RAM: the
 * PRU 0x000-0x3ff, the Tx_PRU 0x400-0x7ff, the RTU 0x800-0xfff. In ICSSG1
 * the upper 4 KiB is free too, and this RTU needs it for the RPMsg buffers.
 * ICSSG0 slice 1's upper 4 KiB is not free (ring_rtu.cmd), so this script
 * is for ICSSG1 slice 0 only.
 */

-cr		/* Link using C conventions */

MEMORY
{
      PAGE 0:
	IMEM	: org = 0x00000000 len = 0x00002000

      PAGE 1:
	/* This core's part of its slice's data RAM */
	DMEM	: org = 0x00000800 len = 0x00000800
	DMEM_HI	: org = 0x00001000 len = 0x00001000

      PAGE 2:
	/* The INTC, for CT_INTC (pru_intc.h). The full length matches the
	 * pruIntc structure. */
	PRU_INTC	: org = 0x00020000 len = 0x00001504	CREGISTER=0
}

SECTIONS {
	.text:_c_int00*	>  0x0, PAGE 0
	.text		>  IMEM, PAGE 0

	.stack		>  DMEM, PAGE 1
	.bss		>  DMEM_HI, PAGE 1
	.cio		>  DMEM_HI, PAGE 1
	.data		>  DMEM_HI, PAGE 1
	.switch		>  DMEM_HI, PAGE 1
	.sysmem		>  DMEM_HI, PAGE 1
	.cinit		>  DMEM_HI, PAGE 1
	/* pru_rproc copies a segment in 32-bit words and refuses one whose size
	 * is not a multiple of 4. palign pads the strings out to one. */
	.rodata		: palign(4) >  DMEM_HI, PAGE 1
	.rofardata	>  DMEM_HI, PAGE 1
	.farbss		>  DMEM_HI, PAGE 1
	.fardata	>  DMEM_HI, PAGE 1

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
