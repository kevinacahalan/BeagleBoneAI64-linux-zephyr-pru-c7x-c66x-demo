/* SPDX-License-Identifier: MIT */
/*
 * The records a slice's RTU and Tx_PRU share in ICSSG1's shared RAM, at the
 * addresses in la_map.h. Each has one writer.
 */
#ifndef LA_IPC_H_
#define LA_IPC_H_

#include <stdint.h>
#include "la_map.h"

/* The trigger, as Linux set it in the stream's header (ai64_la.h). RTU writes, Tx_PRU reads. */
struct la_trig {
	uint32_t seq;   /* bumped after the other three change */
	uint32_t mode;  /* AI64_LA_TRIG_OFF, _ENTER, or _ANY */
	uint32_t mask;
	uint32_t value;
};

/* The Tx_PRU's hits: the edge record that met the trigger. Tx_PRU writes, RTU reads. */
struct la_hits {
	uint32_t wr;    /* hits written, free-running; after the hit itself */
	uint32_t pad[15];
	uint32_t hit[LA_HIT_RING][2];
};

#define LA_TRIG_REC(s) ((volatile struct la_trig *)LA_TRIG(s))
#define LA_HITS_REC(s) ((volatile struct la_hits *)LA_HITS(s))

#endif
