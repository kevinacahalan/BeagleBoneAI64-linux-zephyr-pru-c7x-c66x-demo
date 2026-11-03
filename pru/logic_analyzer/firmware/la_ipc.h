/* SPDX-License-Identifier: MIT */
/*
 * The records a slice's cores share, at the addresses in la_map.h: its
 * PRU's edge records in ICSSG1's shared RAM, and the RTU's and the Tx_PRU's
 * in the slice's data RAM. Each has one writer.
 */
#ifndef LA_IPC_H_
#define LA_IPC_H_

#include <stdint.h>
#include "la_map.h"

/* An edge record in a PRU's ring (la_sample.asm). PRU writes, RTU and Tx_PRU read. */
struct la_srec {
	uint32_t time;  /* IEP0's count */
	uint32_t state; /* the PRU's pins */
	uint32_t seq;   /* the bytes written before this record */
	uint32_t pad;
};

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

#define LA_SREC(s, rd) ((volatile struct la_srec *)(LA_SRING(s) + ((rd) & (LA_SRING_SIZE - 1u))))
#define LA_TRIG_REC   ((volatile struct la_trig *)LA_TRIG)
#define LA_HITS_REC   ((volatile struct la_hits *)LA_HITS)
#define LA_READY_WORD (*(volatile uint32_t *)LA_READY)

#endif
