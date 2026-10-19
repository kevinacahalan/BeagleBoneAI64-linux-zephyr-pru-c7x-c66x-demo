/* SPDX-License-Identifier: MIT */
#ifndef AI64_ERPC_H_
#define AI64_ERPC_H_

#include <stddef.h>
#include <zephyr/devicetree.h>

/*
 * The same app runs on every main-domain R5F core. Only R5F0_0's overlay
 * adds SPI7 and the DDR mailbox, so the other cores leave both out.
 * AI64_CORE ("r5f0_0" and so on) comes from CMakeLists.txt.
 */
#define AI64_HAS_SPI7 DT_NODE_EXISTS(DT_NODELABEL(main_spi7))
#define AI64_HAS_SHM  DT_NODE_EXISTS(DT_NODELABEL(ai64_shm))

void ai64_shm_publish(const char *cmd);
int ai64_spi7_write(const void *buf, size_t len);
int ai64_rpmsg_tx(const void *data, size_t len);
void ai64_erpc_push(const void *data, size_t len);
void ai64_erpc_server(void);

#endif
