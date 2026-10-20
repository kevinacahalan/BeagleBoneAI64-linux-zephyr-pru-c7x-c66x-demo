/* SPDX-License-Identifier: MIT */
/*
 * Answer Linux's remoteproc shutdown request on a DSP.
 *
 * TI's kernel (the Yocto image's TI 6.18) stops a DSP by sending
 * RP_MBOX_SHUTDOWN through the core's mailbox, waiting for
 * RP_MBOX_SHUTDOWN_ACK, and then for the core to report that it is idle,
 * before it powers the core off. The BeagleBoard 6.18 Debian kernel sends
 * nothing, so ai64-demo puts the same message in the FIFO itself before it
 * stops the C7x, whose power-off the device manager refuses while the core
 * runs.
 *
 * The image polls its receive FIFO. Other messages, such as the echo
 * request Linux sends when it probes the core, are dropped.
 */
#ifndef AI64_DSP_MBOX_H_
#define AI64_DSP_MBOX_H_

#include <stdint.h>

#define RP_MBOX_SHUTDOWN     0xFFFFFF14u
#define RP_MBOX_SHUTDOWN_ACK 0xFFFFFF15u

#define MBOX_REG(base, off)  (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#define MBOX_MESSAGE(b, f)   MBOX_REG(b, 0x40u + 4u * (f))
#define MBOX_MSGSTATUS(b, f) MBOX_REG(b, 0xc0u + 4u * (f))

/* 1 when Linux asked this core to shut down; the ACK is already sent. */
static int mbox_shutdown_requested(uint32_t base, uint32_t rx_fifo, uint32_t tx_fifo)
{
	while (MBOX_MSGSTATUS(base, rx_fifo) & 0x3fu) {
		if (MBOX_MESSAGE(base, rx_fifo) == RP_MBOX_SHUTDOWN) {
			MBOX_MESSAGE(base, tx_fifo) = RP_MBOX_SHUTDOWN_ACK;
			return 1;
		}
	}
	return 0;
}

#endif
