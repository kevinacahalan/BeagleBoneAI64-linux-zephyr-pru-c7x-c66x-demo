/*
 * C7x hello image: everything in the C71x's DDR carveout (0xa8100000,
 * 15 MiB; the 1 MiB at 0xa8000000 below it is the core's DMA pool). The
 * boot address (_c_int00) has to be 2 MiB aligned, so the code starts at
 * 0xa8200000. The trace buffer and the resource table, which Linux reads,
 * go in the 1 MiB below it: the MMU maps that 2 MiB block non-cacheable
 * and the rest write-back (mmu.c). The job block
 * (ai64_dsp_job.h) is in L2 SRAM, outside the image.
 */
-c
-e _c_int00
--stack_size=0x4000
--heap_size=0x1000

MEMORY
{
	DDR_NC : origin = 0xa8100000, length = 0x00100000
	DDR    : origin = 0xa8200000, length = 0x00e00000
}

SECTIONS
{
	.text:_c_int00 > 0xa8200000
	.text          > DDR
	.const         > DDR
	.cinit         > DDR
	.switch        > DDR
	.rodata        > DDR
	.bss           > DDR
	.data          > DDR
	.far           > DDR
	.fardata       > DDR
	.stack         > DDR
	.sysmem        > DDR
	.cio           > DDR
	.init_array    > DDR
	.ai64_mmu       : ALIGN(4096) > DDR
	.resource_table : ALIGN(8) > DDR_NC
	.ai64_trace     : ALIGN(8) > DDR_NC
}
