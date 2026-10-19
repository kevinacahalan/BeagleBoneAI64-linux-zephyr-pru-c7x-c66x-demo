/* SPDX-License-Identifier: MIT */
/*
 * Clean up what the previous run of this core left behind.
 *
 * The patched ti_k3_r5_remoteproc (beagle-linux/kernel/k3_r5_rproc) stops an
 * R5F core by asserting its reset, without powering the core module off.
 * Two things survive that, and Zephyr's drivers assume a fresh power-up:
 *
 * - The VIM. Taking an interrupt (reading IRQVEC) makes its priority level
 *   active until the ISR writes IRQVEC again. A core stopped inside an ISR
 *   leaves that level active, and the VIM then holds back every interrupt at
 *   that priority, the system timer included. The core boots, prints a few
 *   lines, and stops dead.
 * - The system timer, a DMTIMER outside the core. It keeps running, with a
 *   match interrupt pending from the last run.
 *
 * This runs before the system timer driver (PRE_KERNEL_2) starts.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/interrupt_controller/intc_vim.h>

#define TIMER_NODE   DT_CHOSEN(zephyr_system_timer)
#define TIMER_BASE   DT_REG_ADDR(TIMER_NODE)
#define DMT_IRQSTATUS     0x28u
#define DMT_IRQENABLE_CLR 0x30u
#define DMT_TCLR          0x38u
#define DMT_TWPS          0x48u

static int ai64_restart_cleanup(void)
{
	unsigned int i;

	/* End any interrupt left in service: one write per priority level. */
	for (i = 0; i <= VIM_PRI_INT_MAX; i++) {
		sys_write32(0, VIM_IRQVEC);
	}
	for (i = 0; i <= VIM_MAX_GROUP_NUM; i++) {
		sys_write32(VIM_GRP_STS_MSK_MASK, VIM_STS(i));
	}

	/* Stop the system timer and drop its pending match. */
	sys_write32(0, TIMER_BASE + DMT_TCLR);
	while (sys_read32(TIMER_BASE + DMT_TWPS)) {
	}
	sys_write32(0x7, TIMER_BASE + DMT_IRQENABLE_CLR);
	sys_write32(0x7, TIMER_BASE + DMT_IRQSTATUS);
	return 0;
}

SYS_INIT(ai64_restart_cleanup, PRE_KERNEL_1, 0);
