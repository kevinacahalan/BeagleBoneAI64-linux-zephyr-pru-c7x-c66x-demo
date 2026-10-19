/* SPDX-License-Identifier: MIT */
/*
 * RTU0 firmware (ICSSG0 slice 0). This core is the transfer core.
 *
 * Linux sends it text over RPMsg (channel "rpmsg-raw", port 30). Zephyr
 * publishes text in the DDR mailbox at AI64_SHM_ADDR, which this core polls.
 * Either way the command lands in ICSSG0 shared RAM:
 *   "xy <x> <y>"            the X/Y record, which PRU0 sends on P8.11
 *   "arm <period> <count>"  the arm block, which Tx_PRU0 turns into pulses
 * Anything else is echoed. This core drives no pin itself.
 */

#include <stdint.h>
#include "ai64_shm.h"

#include <pru_intc.h>
#include <rsc_types.h>
#include <pru_rpmsg.h>
#include "resource_table.h"

/* Host interrupt 10 is R31 bit 30 on the RTU. */
#define HOST_INT      ((uint32_t)1 << 30)
#define TO_ARM_HOST   20
#define FROM_ARM_HOST 21
#define CHAN_PORT     30
#define CHAN_NAME     "rpmsg-raw"
#define VIRTIO_CONFIG_S_DRIVER_OK 4

volatile register uint32_t __R31;

static uint8_t payload[RPMSG_MESSAGE_SIZE];
/*
 * tx and cmd stay in .bss. A 496-byte local on the stack would not fit next
 * to what pru_rpmsg_channel() needs for the name-service announce.
 */
static char tx[RPMSG_MESSAGE_SIZE];
static char cmd[56];

static uint16_t append_str(char *dst, uint16_t n, uint16_t max, const char *s)
{
	while (*s && n + 1 < max) {
		dst[n++] = *s++;
	}
	dst[n] = 0;
	return n;
}

static uint16_t append_u32(char *dst, uint16_t n, uint16_t max, uint32_t v)
{
	char tmp[10];
	uint16_t i = 0;

	if (v == 0) {
		return append_str(dst, n, max, "0");
	}
	while (v && i < sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i && n + 1 < max) {
		dst[n++] = tmp[--i];
	}
	dst[n] = 0;
	return n;
}

static int parse_u32(const char **pp, uint32_t *out)
{
	const char *s = *pp;
	uint32_t v = 0;
	int any = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10u + (uint32_t)(*s - '0');
		s++;
		any = 1;
	}
	if (!any) {
		return 0;
	}
	*out = v;
	*pp = s;
	return 1;
}

/* "<word> <a> <b>", both decimal uint32. */
static int parse_two(const char *s, const char *word, uint32_t *a, uint32_t *b)
{
	while (*word) {
		if (*s++ != *word++) {
			return 0;
		}
	}
	if (*s++ != ' ') {
		return 0;
	}
	if (!parse_u32(&s, a) || *s != ' ') {
		return 0;
	}
	s++;
	return parse_u32(&s, b);
}

static void publish_xy(volatile struct ai64_xy *xy, uint32_t *xy_seq,
		       uint32_t x, uint32_t y)
{
	xy->x = x;
	xy->y = y;
	xy->magic = AI64_XY_MAGIC;
	*xy_seq += 1u;
	xy->seq = *xy_seq;
}

static void arm_pulses(volatile struct ai64_arm *arm, uint32_t *arm_seq,
		       uint32_t *period, uint32_t *count)
{
	if (*period < AI64_ARM_PERIOD_MIN) {
		*period = AI64_ARM_PERIOD_MIN;
	}
	if (*period > AI64_ARM_PERIOD_MAX) {
		*period = AI64_ARM_PERIOD_MAX;
	}
	if (*count > AI64_ARM_COUNT_MAX) {
		*count = AI64_ARM_COUNT_MAX;
	}
	arm->period_cycles = *period;
	arm->periods = *count;
	arm->magic = AI64_ARM_MAGIC;
	*arm_seq += 1u;
	arm->seq = *arm_seq;
}

/* Who asked, and what RTU0 stored. No printf on this core. */
static uint16_t reply_xy(char *dst, uint16_t max, const char *who,
			 uint32_t seq, uint32_t x, uint32_t y)
{
	uint16_t n = 0;

	n = append_str(dst, n, max, who);
	n = append_str(dst, n, max, " asked RTU0 to publish X/Y, and RTU0 wrote x ");
	n = append_u32(dst, n, max, x);
	n = append_str(dst, n, max, " y ");
	n = append_u32(dst, n, max, y);
	n = append_str(dst, n, max, " seq ");
	n = append_u32(dst, n, max, seq);
	n = append_str(dst, n, max, " for PRU0");
	return n;
}

static uint16_t reply_arm(char *dst, uint16_t max, const char *who,
			  uint32_t period, uint32_t count)
{
	uint16_t n = 0;

	n = append_str(dst, n, max, who);
	n = append_str(dst, n, max, " asked RTU0 to arm Tx_PRU0, and RTU0 wrote period ");
	n = append_u32(dst, n, max, period);
	n = append_str(dst, n, max, " count ");
	n = append_u32(dst, n, max, count);
	n = append_str(dst, n, max, " for Tx_PRU0");
	return n;
}

static uint16_t reply_echo(char *dst, uint16_t max, const char *who, const char *text)
{
	uint16_t n = 0;

	n = append_str(dst, n, max, who);
	n = append_str(dst, n, max, " sent \"");
	n = append_str(dst, n, max, text);
	n = append_str(dst, n, max, "\" to RTU0, and RTU0 echoed it");
	return n;
}

static void trace_cmd(const char *who, const char *text)
{
	trace_puts(who);
	trace_puts(": ");
	trace_puts(text);
	trace_putc('\n');
}

/*
 * Run one command. Returns the reply length, or 0 when nothing should be
 * sent (a Zephyr command before Linux has said hello).
 */
static uint16_t run_cmd(const char *who, volatile struct ai64_arm *arm,
			volatile struct ai64_xy *xy, uint32_t *arm_seq,
			uint32_t *xy_seq)
{
	uint32_t a, b;

	trace_cmd(who, cmd);
	if (parse_two(cmd, "xy", &a, &b)) {
		publish_xy(xy, xy_seq, a, b);
		return reply_xy(tx, sizeof(tx), who, *xy_seq, a, b);
	}
	if (parse_two(cmd, "arm", &a, &b)) {
		arm_pulses(arm, arm_seq, &a, &b);
		return reply_arm(tx, sizeof(tx), who, a, b);
	}
	return reply_echo(tx, sizeof(tx), who, cmd);
}

static int16_t send_bytes(struct pru_rpmsg_transport *transport,
			  uint16_t src, uint16_t dst, const char *msg, uint16_t len)
{
	if (len > RPMSG_MESSAGE_SIZE) {
		len = RPMSG_MESSAGE_SIZE;
	}
	return pru_rpmsg_send(transport, src, dst, (void *)msg, len);
}

void main(void)
{
	struct pru_rpmsg_transport transport;
	volatile struct ai64_shm *shm = (volatile struct ai64_shm *)AI64_SHM_ADDR;
	volatile struct ai64_arm *arm = (volatile struct ai64_arm *)AI64_ARM_LOCAL;
	volatile struct ai64_xy *xy = (volatile struct ai64_xy *)AI64_XY_LOCAL;
	volatile uint8_t *status;
	uint16_t src = 0;
	uint16_t dst = 0;
	uint16_t len;
	uint32_t last_seq;
	uint32_t arm_seq = 0;
	uint32_t xy_seq = 0;
	uint16_t peer_known = 0;

	trace_init();
	trace_puts("rtu0 up, waiting for virtio\n");

	CT_INTC.STATUS_CLR_INDEX_REG_bit.STATUS_CLR_INDEX = FROM_ARM_HOST;
	status = &resourceTable.rpmsg_vdev.status;
	while (!(*status & VIRTIO_CONFIG_S_DRIVER_OK)) {
	}

	pru_rpmsg_init(&transport, &resourceTable.rpmsg_vring0,
		       &resourceTable.rpmsg_vring1, TO_ARM_HOST, FROM_ARM_HOST);
	while (pru_rpmsg_channel(RPMSG_NS_CREATE, &transport, CHAN_NAME,
				 CHAN_PORT) != PRU_RPMSG_SUCCESS) {
	}
	trace_puts("rtu0 channel rpmsg-raw port 30 is up\n");

	/*
	 * Adopt the mailbox seq already in DDR. Otherwise a restarted RTU0
	 * replays the last command Zephyr published, such as an old xy.
	 */
	last_seq = shm->seq;

	/*
	 * Start both records empty, status fields too, so a watcher does not
	 * mistake the last run's applied_seq or done_seq for this run's.
	 * PRU0 and Tx_PRU0 wait for a new seq.
	 */
	arm->magic = 0;
	arm->seq = 0;
	arm->sent = 0;
	arm->applied_seq = 0;
	arm->applied_period = 0;
	arm->done_seq = 0;
	xy->x = 0;
	xy->y = 0;
	xy->consumed = 0;
	xy->magic = 0;
	xy->seq = 0;

	while (1) {
		if (__R31 & HOST_INT) {
			CT_INTC.STATUS_CLR_INDEX_REG_bit.STATUS_CLR_INDEX = FROM_ARM_HOST;
			while (pru_rpmsg_receive(&transport, &src, &dst, payload, &len) ==
			       PRU_RPMSG_SUCCESS) {
				uint16_t i;
				uint16_t copy = len;
				uint16_t n;

				peer_known = 1;
				while (copy && (payload[copy - 1] == '\n' ||
						payload[copy - 1] == '\r')) {
					copy--;
				}
				for (i = 0; i < copy && i + 1 < sizeof(cmd); i++) {
					cmd[i] = (char)payload[i];
				}
				cmd[i] = 0;
				n = run_cmd("Linux", arm, xy, &arm_seq, &xy_seq);
				send_bytes(&transport, dst, src, tx, n);
			}
		}

		if (shm->magic == AI64_SHM_MAGIC && shm->seq != last_seq) {
			uint32_t seq = shm->seq;
			uint16_t i;
			uint16_t n;

			if (seq != shm->seq || shm->magic != AI64_SHM_MAGIC) {
				continue;
			}
			last_seq = seq;
			for (i = 0; i < sizeof(cmd) - 1 && shm->cmd[i]; i++) {
				cmd[i] = shm->cmd[i];
			}
			cmd[i] = 0;
			n = run_cmd("Zephyr", arm, xy, &arm_seq, &xy_seq);
			/* RTU0 learns Linux's address from Linux's first message. */
			if (peer_known) {
				send_bytes(&transport, dst, src, tx, n);
			}
		}
	}
}
