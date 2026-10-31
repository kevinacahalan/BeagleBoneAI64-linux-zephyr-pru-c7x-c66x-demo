/* SPDX-License-Identifier: MIT */
#ifndef AI64_IPC_H_
#define AI64_IPC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * The R5F cores' calls to each other, over Zephyr ipc_service
 * (common/ai64_ipc.dtsi, common/src/ipc.c). Core n is R5F0_0, R5F0_1, R5F1_0,
 * R5F1_1 for n = 0 to 3; AI64_SELF is this one.
 *
 * A call sends a request to one core and waits for its reply. Each core
 * answers every op: PING and STATUS in ipc.c, the others with its
 * ai64_r5_core.request (ai64_r5.h), or -ENOTSUP when it has none.
 */
#define AI64_IPC_CORES 4u

/* Whose job is what. */
#define AI64_CORE_SPI     0u /* R5F0_0: SPI7 and the UART2 console */
#define AI64_CORE_PWM     1u /* R5F0_1: EHRPWM4 on P9.25 */
#define AI64_CORE_BLINK   2u /* R5F1_0: the P8.18 square wave */
#define AI64_CORE_MONITOR 3u /* R5F1_1: pings and status */

enum ai64_ipc_op {
	AI64_IPC_PING = 1,  /* answered with nothing; for the round trip */
	AI64_IPC_STATUS,    /* text: the core's job and how it is doing */
	AI64_IPC_SPI,       /* text: bytes to write on SPI7 */
	AI64_IPC_PWM,       /* arg0 period ns, arg1 duty ns */
	AI64_IPC_STEPS,     /* arg0 ms a step, 0 stops */
};

#define AI64_IPC_TEXT 160u

struct ai64_ipc_msg {
	uint8_t op;
	uint8_t reply;  /* 0 a request, 1 its reply */
	uint16_t seq;
	int32_t ret;    /* reply: 0, or a negative errno */
	uint32_t arg0;
	uint32_t arg1;
	uint32_t gtc;   /* GTC low word when the sender sent it */
	uint32_t sent;  /* reply: the request's gtc, for the round trip */
	char text[AI64_IPC_TEXT];
};

extern const char *const ai64_core_name[AI64_IPC_CORES];

/* Starts the links. The thread that answers requests calls it. */
void ai64_ipc_start(void);

/* Whether the link to peer is up (both cores have opened it). */
bool ai64_ipc_bound(unsigned int peer);

/*
 * Send m to peer and wait up to timeout for its reply, which replaces m.
 * Returns 0, -ENOTCONN when the link is down, -EAGAIN on timeout, or what
 * mbox_send returned.
 */
int ai64_ipc_call(unsigned int peer, struct ai64_ipc_msg *m, k_timeout_t timeout);

#endif
