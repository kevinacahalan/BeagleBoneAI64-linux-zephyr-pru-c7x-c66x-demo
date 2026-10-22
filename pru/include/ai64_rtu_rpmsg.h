/* SPDX-License-Identifier: MIT */
/*
 * RPMsg on a slice-0 RTU: one text channel, "rpmsg-raw" on port 30, to
 * Linux. Include it after ai64_rsc_rpmsg.h, which holds the vdev.
 *
 * rtu_rpmsg_up() waits for Linux's virtio driver, then announces the
 * channel. rtu_rpmsg_receive() returns the next message as a C string, with
 * trailing newlines dropped, or 0 when there is none. Linux's address comes
 * from its first message, so rtu_rpmsg_send() sends nothing before that.
 */
#ifndef AI64_RTU_RPMSG_H_
#define AI64_RTU_RPMSG_H_

#include <stdint.h>
#include <pru_intc.h>
#include <pru_rpmsg.h>

/* Host interrupt 10 is R31 bit 30 on a slice-0 RTU. */
#define HOST_INT      ((uint32_t)1 << 30)
#define TO_ARM_HOST   20
#define FROM_ARM_HOST 21
#define CHAN_PORT     30
#define CHAN_NAME     "rpmsg-raw"
#define VIRTIO_CONFIG_S_DRIVER_OK 4

volatile register uint32_t __R31;

static struct pru_rpmsg_transport transport;
/* Linux's address and this core's, from Linux's first message. */
static uint16_t rpmsg_src;
static uint16_t rpmsg_dst;
static uint16_t rpmsg_peer_known;
static uint8_t rpmsg_payload[RPMSG_MESSAGE_SIZE];

static void rtu_rpmsg_up(void)
{
	volatile uint8_t *status = &resourceTable.rpmsg_vdev.status;

	CT_INTC.STATUS_CLR_INDEX_REG_bit.STATUS_CLR_INDEX = FROM_ARM_HOST;
	while (!(*status & VIRTIO_CONFIG_S_DRIVER_OK)) {
	}
	pru_rpmsg_init(&transport, &resourceTable.rpmsg_vring0,
		       &resourceTable.rpmsg_vring1, TO_ARM_HOST, FROM_ARM_HOST);
	while (pru_rpmsg_channel(RPMSG_NS_CREATE, &transport, CHAN_NAME,
				 CHAN_PORT) != PRU_RPMSG_SUCCESS) {
	}
}

/* The kick only says the vring has something; drain it before clearing. */
static int rtu_rpmsg_pending;

static uint16_t rtu_rpmsg_receive(char *cmd, uint16_t max)
{
	uint16_t len, i;

	if (!rtu_rpmsg_pending) {
		if (!(__R31 & HOST_INT)) {
			return 0;
		}
		CT_INTC.STATUS_CLR_INDEX_REG_bit.STATUS_CLR_INDEX = FROM_ARM_HOST;
		rtu_rpmsg_pending = 1;
	}
	if (pru_rpmsg_receive(&transport, &rpmsg_src, &rpmsg_dst, rpmsg_payload, &len) !=
	    PRU_RPMSG_SUCCESS) {
		rtu_rpmsg_pending = 0;
		return 0;
	}
	rpmsg_peer_known = 1;
	while (len && (rpmsg_payload[len - 1] == '\n' || rpmsg_payload[len - 1] == '\r')) {
		len--;
	}
	for (i = 0; i < len && i + 1 < max; i++) {
		cmd[i] = (char)rpmsg_payload[i];
	}
	cmd[i] = 0;
	/* An empty message still counts: return its length plus one. */
	return i + 1u;
}

static void rtu_rpmsg_send(const char *msg, uint16_t len)
{
	if (!rpmsg_peer_known || len == 0) {
		return;
	}
	if (len > RPMSG_MESSAGE_SIZE) {
		len = RPMSG_MESSAGE_SIZE;
	}
	pru_rpmsg_send(&transport, rpmsg_dst, rpmsg_src, (void *)msg, len);
}

#endif
