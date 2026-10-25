/* SPDX-License-Identifier: MIT */
#ifndef AI64_TISCI_H_
#define AI64_TISCI_H_

/*
 * Ask the device manager to route this core's mailbox interrupt.
 * Returns 0 when the route is in place.
 */
int ai64_route_mbox_irq(void);
/* The same for its mailbox cluster 7 user, the other R5F cores' doorbells. */
int ai64_route_ipc_irq(void);

#endif
