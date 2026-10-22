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
 *
 * It is also fabric node 8 (ai64_fabric.h). A route can carry an arm or xy
 * action for RTU0, so any core can make Tx_PRU0 pulse or PRU0 send a
 * frame; RTU0 tells Linux over RPMsg when one does.
 *
 * RTU0 is the slice's only link to the rest of the SoC. PRU0 and Tx_PRU0
 * only read the records RTU0 writes, and do their timed work.
 */

#include <stdint.h>
#include "ai64_shm.h"

/*
 * RTU0's first data RAM slice is 2 KiB and holds the RPMsg buffers, so the
 * log stays small and sits in the second slice with the stack.
 */
#define AI64_TRACE_SIZE 512
#define AI64_TRACE_NAME "trace:rtu0"
#include "ai64_rsc_rpmsg.h"
#include "ai64_rtu_rpmsg.h"
#include "ai64_text.h"

static volatile struct ai64_arm *const arm = (volatile struct ai64_arm *)AI64_ARM_LOCAL;
static volatile struct ai64_xy *const xy = (volatile struct ai64_xy *)AI64_XY_LOCAL;
static uint32_t arm_seq;
static uint32_t xy_seq;

/*
 * tx and cmd stay in .bss. A 496-byte local on the stack would not fit next
 * to what pru_rpmsg_channel() needs for the name-service announce.
 */
static char tx[RPMSG_MESSAGE_SIZE];
static char cmd[56];

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
static uint16_t run_cmd(const char *who)
{
	uint32_t a, b;

	trace_cmd(who, cmd);
	if (parse_two(cmd, "xy", &a, &b)) {
		publish_xy(xy, &xy_seq, a, b);
		return reply_xy(tx, sizeof(tx), who, xy_seq, a, b);
	}
	if (parse_two(cmd, "arm", &a, &b)) {
		arm_pulses(arm, &arm_seq, &a, &b);
		return reply_arm(tx, sizeof(tx), who, a, b);
	}
	return reply_echo(tx, sizeof(tx), who, cmd);
}

/*
 * The fabric. RTU0 polls it on every pass of its main loop. A route whose
 * action is for RTU0 runs it here, between hops.
 */
struct ai64_fab_msg;
static uint32_t fab_action(struct ai64_fab_msg *m);

#define AI64_FAB_NOW()    (*(volatile uint32_t *)(AI64_GTC_ADDR + 8u))
#define AI64_FAB_BASE     AI64_FAB_ADDR
#define AI64_FAB_MBOX     AI64_FAB_MBOX_ADDR
#define AI64_FAB_LOCKREG  (AI64_SPINLOCK_ADDR + AI64_FAB_LOCK_OFF)
#define AI64_FAB_ACTION(m) fab_action(m)
#include "ai64_fabric.h"

static uint32_t fab_action(struct ai64_fab_msg *m)
{
	static const char who[] = "A fabric route";
	uint32_t a = m->arg0, b = m->arg1;
	uint16_t n;

	if (m->action == AI64_FAB_ACT_ARM) {
		arm_pulses(arm, &arm_seq, &a, &b);
		n = reply_arm(tx, sizeof(tx), who, a, b);
	} else if (m->action == AI64_FAB_ACT_XY) {
		publish_xy(xy, &xy_seq, a, b);
		n = reply_xy(tx, sizeof(tx), who, xy_seq, a, b);
	} else {
		return 1;
	}
	trace_puts(tx);
	trace_putc('\n');
	rtu_rpmsg_send(tx, n);
	return 0;
}

void main(void)
{
	volatile struct ai64_shm *shm = (volatile struct ai64_shm *)AI64_SHM_ADDR;
	uint32_t last_seq;

	trace_init();
	trace_puts("rtu0 up, waiting for virtio\n");
	rtu_rpmsg_up();
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

	fab_init(AI64_FAB_G0RTU0);
	trace_puts("rtu0 is fabric node 8\n");

	while (1) {
		fab_poll();
		if (rtu_rpmsg_receive(cmd, sizeof(cmd))) {
			uint16_t n = run_cmd("Linux");

			rtu_rpmsg_send(tx, n);
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
			n = run_cmd("Zephyr");
			/* RTU0 learns Linux's address from Linux's first message. */
			rtu_rpmsg_send(tx, n);
		}
	}
}
