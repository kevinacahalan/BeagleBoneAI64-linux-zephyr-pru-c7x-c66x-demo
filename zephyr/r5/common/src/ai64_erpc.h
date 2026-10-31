/* SPDX-License-Identifier: MIT */
#ifndef AI64_ERPC_H_
#define AI64_ERPC_H_

#include <stddef.h>

/* The eRPC server over the RPMsg endpoint: rpmsg.c hands it what arrives,
 * and it sends through ai64_rpmsg_tx() (erpc.cpp). */
int ai64_rpmsg_tx(const void *data, size_t len);
void ai64_erpc_push(const void *data, size_t len);
void ai64_erpc_server(void);

#endif
