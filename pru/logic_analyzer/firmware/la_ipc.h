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

/*
 * The mode a slice's PRU starts in (la_map.h). The RTU writes mode and
 * mask from the stream's header (ai64_la.h) and keeps resume at its rd;
 * the PRU writes t0 in raw mode.
 */
struct la_cfg {
	uint32_t mode;   /* AI64_LA_MODE_EDGE, _RAW, _CONV, or _DUMP */
	uint32_t mask;   /* the PRU's pins to record */
	uint32_t resume; /* the seq a writer starting now begins at */
	uint32_t t0;     /* raw: IEP0's count at the first sample */
	uint32_t wr;     /* dump: blocks copied into the burst buffer, for Linux to follow */
};

#define LA_SREC(s, rd) ((volatile struct la_srec *)(LA_SRING(s) + ((rd) & (LA_SRING_SIZE - 1u))))
#define LA_TRIG_REC   ((volatile struct la_trig *)LA_TRIG)
#define LA_HITS_REC   ((volatile struct la_hits *)LA_HITS)
#define LA_READY_WORD (*(volatile uint32_t *)LA_READY)
#define LA_CFG_REC    ((volatile struct la_cfg *)LA_CFG)
/* The other slice's, as a PRU sees it. */
#define LA_CFG_OTHER  ((volatile struct la_cfg *)(LA_OTHER_DRAM + LA_CFG))

#endif
