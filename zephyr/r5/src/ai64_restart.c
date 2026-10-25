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
 * - This core's user of its home mailbox (boards/ai64_ipc.dtsi), outside
 *   the core too. Its interrupt enables stay set from the last run, and the
 *   other cores keep ringing while this one restarts. Zephyr's mailbox
 *   driver skips a FIFO it has not enabled yet without reading it, so once
 *   the first link enables the interrupt, a stale FIFO holds it asserted
 *   and the core does nothing but take it.
 *
 * This runs before the system timer driver (PRE_KERNEL_2) starts.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/interrupt_controller/intc_vim.h>
#include <zephyr/sys/util.h>

#define TIMER_NODE   DT_CHOSEN(zephyr_system_timer)
#define TIMER_BASE   DT_REG_ADDR(TIMER_NODE)
#define DMT_IRQSTATUS     0x28u
#define DMT_IRQENABLE_CLR 0x30u
#define DMT_TCLR          0x38u
#define DMT_TWPS          0x48u

#define HOME_NODE         DT_NODELABEL(CONCAT(ai64_ipc_home, AI64_SELF))
#define HOME_BASE         DT_REG_ADDR(HOME_NODE)
#define HOME_USER         DT_PROP(HOME_NODE, usr_id)
#define MBOX_IRQSTATUS(u) (HOME_BASE + 0x104u + 0x10u * (u))
#define MBOX_IRQENABLE_CLR(u) (HOME_BASE + 0x10cu + 0x10u * (u))

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

	/* Disable every FIFO's interrupts for this core's mailbox user. */
	sys_write32(0xffffffffu, MBOX_IRQENABLE_CLR(HOME_USER));
	sys_write32(0xffffffffu, MBOX_IRQSTATUS(HOME_USER));
	return 0;
}

SYS_INIT(ai64_restart_cleanup, PRE_KERNEL_1, 0);
