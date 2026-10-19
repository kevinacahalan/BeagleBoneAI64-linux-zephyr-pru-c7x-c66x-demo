/*
 * C66x hello image: everything in the core's own L2 SRAM. Linux loads it
 * there through the core's local view (0x00800000), which ti_k3_dsp maps
 * for both C66x cores. The boot address (_c_int00) has to be 1 KiB aligned.
 * The last 64 KiB, from 0x00838000, is the job block (ai64_dsp_job.h).
 */
-c
-e _c_int00
--stack_size=0x2000
--heap_size=0x1000

MEMORY
{
	L2SRAM : origin = 0x00800000, length = 0x00038000
}

SECTIONS
{
	.text:_c_int00 > 0x00800000
	.text          > L2SRAM
	.const         > L2SRAM
	.cinit         > L2SRAM
	.switch        > L2SRAM
	.neardata      > L2SRAM
	.rodata        > L2SRAM
	.fardata       > L2SRAM
	.far           > L2SRAM
	.bss           > L2SRAM
	.data          > L2SRAM
	.stack         > L2SRAM
	.sysmem        > L2SRAM
	.cio           > L2SRAM
	.init_array    > L2SRAM
	.resource_table : ALIGN(8) > L2SRAM
	.ai64_trace     : ALIGN(8) > L2SRAM
}
