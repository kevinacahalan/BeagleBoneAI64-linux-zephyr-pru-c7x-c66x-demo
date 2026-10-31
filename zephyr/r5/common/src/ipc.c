/* SPDX-License-Identifier: MIT */
/*
 * Calls between the R5F cores, on Zephyr's ipc_service.
 *
 * common/ai64_ipc.dtsi gives each core an icmsg instance for each of the
 * other three: a shared-memory buffer each way, and a mailbox FIFO each way
 * as the doorbell. Each instance carries one endpoint, and every message on
 * it is a struct ai64_ipc_msg, a request or the reply to one.
 *
 * Requests arrive on icmsg's work queue; that only queues them, and this
 * file's thread answers them in order (answer()), so a slow job, an
 * SPI write say, holds up nothing but later requests to this core. A caller
 * keeps one request in flight per peer and sleeps until the reply with its
 * sequence number arrives.
 *
 * When a peer restarts, it opens the link with a new session number; icmsg
 * calls unbound here, and this thread registers the endpoint again, which
 * binds once the peer is up.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include "ai64_ipc.h"
#include "ai64_r5.h"
#include "ai64_shm.h"
#include "ai64_tisci.h"

LOG_MODULE_REGISTER(ai64_ipc, LOG_LEVEL_INF);

const char *const ai64_core_name[AI64_IPC_CORES] = {"R5F0_0", "R5F0_1", "R5F1_0", "R5F1_1"};

#define PEER_DEV(n)                                                                                \
	COND_CODE_1(DT_NODE_EXISTS(DT_NODELABEL(ai64_ipc_peer##n)),                               \
		    (DEVICE_DT_GET(DT_NODELABEL(ai64_ipc_peer##n))), (NULL))

static const struct device *const peer_dev[AI64_IPC_CORES] = {
	PEER_DEV(0), PEER_DEV(1), PEER_DEV(2), PEER_DEV(3),
};

BUILD_ASSERT(AI64_SELF < AI64_IPC_CORES, "AI64_SELF is 0 to 3");

struct peer {
	struct ipc_ept ept;
	struct ipc_ept_cfg cfg;
	atomic_t bound;
	atomic_t rebind;
	/* One call in flight: the caller waits on done for reply seq. */
	struct k_mutex call_lock;
	struct k_sem done;
	uint16_t seq;
	struct ai64_ipc_msg *reply;
};

static struct peer peers[AI64_IPC_CORES];

struct inbound {
	uint8_t from;
	struct ai64_ipc_msg msg;
};

K_MSGQ_DEFINE(requests, sizeof(struct inbound), 4, 4);
static K_SEM_DEFINE(wake, 0, 1);

static uint32_t gtc_now(void)
{
	return sys_read32(AI64_GTC_ADDR + 8u);
}

static void ep_bound(void *priv)
{
	struct peer *p = priv;

	atomic_set(&p->bound, 1);
	LOG_INF("link to %s up", ai64_core_name[p - peers]);
}

static void ep_unbound(void *priv)
{
	struct peer *p = priv;

	atomic_set(&p->bound, 0);
	atomic_set(&p->rebind, 1);
	LOG_INF("link to %s down: it restarted", ai64_core_name[p - peers]);
	k_sem_give(&wake);
}

static void ep_received(const void *data, size_t len, void *priv)
{
	struct peer *p = priv;
	struct inbound in;

	if (len < offsetof(struct ai64_ipc_msg, text) || len > sizeof(in.msg)) {
		return;
	}
	memset(&in, 0, sizeof(in));
	memcpy(&in.msg, data, len);
	in.msg.text[AI64_IPC_TEXT - 1u] = '\0';

	if (in.msg.reply) {
		/* Only the reply the caller waits for; a late one is dropped. */
		if (p->reply && in.msg.seq == p->seq) {
			*p->reply = in.msg;
			p->reply = NULL;
			k_sem_give(&p->done);
		}
		return;
	}
	in.from = p - peers;
	if (k_msgq_put(&requests, &in, K_NO_WAIT)) {
		LOG_WRN("request from %s dropped: queue full", ai64_core_name[in.from]);
		return;
	}
	k_sem_give(&wake);
}

static void ep_error(const char *message, void *priv)
{
	struct peer *p = priv;

	LOG_ERR("link to %s: %s", ai64_core_name[p - peers], message);
}

/* The bytes of m that matter: the header, and text up to its NUL. */
static size_t msg_len(const struct ai64_ipc_msg *m)
{
	return offsetof(struct ai64_ipc_msg, text) + strnlen(m->text, AI64_IPC_TEXT - 1u) + 1u;
}

static int send(unsigned int peer, const struct ai64_ipc_msg *m)
{
	int ret = ipc_service_send(&peers[peer].ept, m, msg_len(m));

	return ret < 0 ? ret : 0;
}

bool ai64_ipc_bound(unsigned int peer)
{
	return peer < AI64_IPC_CORES && peer_dev[peer] && atomic_get(&peers[peer].bound);
}

int ai64_ipc_call(unsigned int peer, struct ai64_ipc_msg *m, k_timeout_t timeout)
{
	struct peer *p;
	int ret;

	if (!ai64_ipc_bound(peer)) {
		return -ENOTCONN;
	}
	p = &peers[peer];
	k_mutex_lock(&p->call_lock, K_FOREVER);
	k_sem_reset(&p->done);
	m->reply = 0;
	m->seq = ++p->seq;
	m->gtc = gtc_now();
	p->reply = m;
	ret = send(peer, m);
	if (ret == 0 && k_sem_take(&p->done, timeout)) {
		ret = -EAGAIN;
	}
	p->reply = NULL;
	k_mutex_unlock(&p->call_lock);
	return ret;
}

static void open_link(unsigned int n)
{
	int ret = ipc_service_register_endpoint(peer_dev[n], &peers[n].ept, &peers[n].cfg);

	if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("link to %s: open failed (%d), trying again", ai64_core_name[n], ret);
		atomic_set(&peers[n].rebind, 1);
	}
}

void ai64_ipc_start(void)
{
	unsigned int n;
	int ret;

	ret = ai64_route_ipc_irq();
	if (ret) {
		LOG_ERR("no doorbell interrupt (%d): links to the other cores stay down", ret);
		return;
	}
	for (n = 0; n < AI64_IPC_CORES; n++) {
		if (!peer_dev[n]) {
			continue;
		}
		k_mutex_init(&peers[n].call_lock);
		k_sem_init(&peers[n].done, 0, 1);
		peers[n].cfg = (struct ipc_ept_cfg){
			.name = "ai64",
			.cb = {
				.bound = ep_bound,
				.unbound = ep_unbound,
				.received = ep_received,
				.error = ep_error,
			},
			.priv = &peers[n],
		};
		ret = ipc_service_open_instance(peer_dev[n]);
		if (ret < 0 && ret != -EALREADY) {
			LOG_ERR("link to %s: no instance (%d)", ai64_core_name[n], ret);
			continue;
		}
		open_link(n);
	}
}

/* This core's answer to a request from core from: PING and STATUS here, the
 * rest by its job's request hook, which only the core whose job it is has;
 * the caller sends each request to that core in the first place. */
static void answer(unsigned int from, const struct ai64_ipc_msg *req, struct ai64_ipc_msg *rep)
{
	switch (req->op) {
	case AI64_IPC_PING:
		break;
	case AI64_IPC_STATUS:
		ai64_r5_core.status(rep->text, sizeof(rep->text));
		rep->ret = 0;
		break;
	default:
		rep->ret = -ENOTSUP;
		if (ai64_r5_core.request) {
			ai64_r5_core.request(ai64_core_name[from], req, rep);
		}
		break;
	}
}

static void ipc_thread(void *a, void *b, void *c)
{
	struct inbound in;
	struct ai64_ipc_msg rep;
	unsigned int n;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	ai64_ipc_start();
	for (;;) {
		/* A link whose open failed (its doorbell FIFO full) gets another go each second. */
		(void)k_sem_take(&wake, K_SECONDS(1));
		for (n = 0; n < AI64_IPC_CORES; n++) {
			if (peer_dev[n] && atomic_cas(&peers[n].rebind, 1, 0)) {
				open_link(n);
			}
		}
		while (k_msgq_get(&requests, &in, K_NO_WAIT) == 0) {
			memset(&rep, 0, sizeof(rep));
			rep.op = in.msg.op;
			rep.reply = 1;
			rep.seq = in.msg.seq;
			rep.sent = in.msg.gtc;
			answer(in.from, &in.msg, &rep);
			rep.gtc = gtc_now();
			(void)send(in.from, &rep);
		}
	}
}

K_THREAD_DEFINE(ai64_ipc_tid, 2048, ipc_thread, NULL, NULL, NULL, K_PRIO_COOP(9), 0, 0);
