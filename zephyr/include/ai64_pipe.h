/* SPDX-License-Identifier: MIT */
/*
 * The pipeline: a signal in on one header pin, through five kinds of core,
 * and out on another, with no Linux in the path.
 *
 *   ICSSG0 PRU1   samples P8.31 at 2 MHz with its GPI shift register, and
 *                 packs the samples into windows of 2048 (1.024 ms) in
 *                 ICSSG0's shared RAM. The windows overlap by half: a new
 *                 one ends every 1024 samples (512 us).
 *   ICSSG0 RTU1   stamps each window with the GTC and rings R5F0_0's
 *                 mailbox.
 *   R5F0_0        copies the window with its UDMA channel into the L2 SRAM
 *                 of C66x 0 (even windows) or C66x 1 (odd ones); the same
 *                 descriptor's second TR then writes the count of windows
 *                 copied, that C66x's doorbell.
 *   C66x 0 and 1  window the samples, take a 2048-point FFT, and write 64
 *                 int8 features (the spectrum's first 63 bins, and the DC)
 *                 into the C7x's L2 SRAM. Each has a window every 1.024 ms.
 *   C7x           runs the features through an int8 layer on its Matrix
 *                 Multiply Accelerator, one output per class, rings
 *                 R5F0_1's mailbox with the best, and logs the features
 *                 for Linux.
 *   R5F0_1        sets P9.25 to the class's frequency and duty, once two
 *                 windows in a row agree on it.
 *
 * The bench signal is ICSSG0 slice 1's output B, P9.15, jumpered to P8.31:
 * it steps through the 15 frequency and duty pairs of AI64_PWM_STEP_HZ x
 * AI64_PWM_STEP_PCT (ai64_shm.h), a class each, on the GTC's time. So
 * R5F0_1 knows what each window should have been, and keeps score; and
 * P9.25 shows the pattern again, about 1.5 ms late.
 *
 * Linux (`ai64 pipe`) only starts it, trains the layer, and reads the
 * numbers: the ctl record, the weights, and the feature log are in DDR at
 * AI64_PIPE_ADDR, inside the 1 MiB the overlay reserves for the fabric.
 * Every core maps it uncached. Each record has one writer.
 *
 * Linux, Zephyr, both DSP compilers, and the PRU compiler include this
 * file, so it only uses 32-bit fields (and int8 arrays) at fixed offsets.
 */
#ifndef AI64_PIPE_H_
#define AI64_PIPE_H_

#include <stdint.h>
#include "ai64_shm.h"

#define AI64_PIPE_MAGIC   0x45504950u /* "PIPE" */

/* Sampling: the ICSSG core clock, 250 MHz, divided by 12.5 and by 10. */
#define AI64_PIPE_SAMPLE_HZ  2000000u
#define AI64_PIPE_WIN        2048u                     /* samples a window */
#define AI64_PIPE_CHUNKS     (AI64_PIPE_WIN / 16u)     /* 16-bit chunks a window */
#define AI64_PIPE_WIN_GTC    (AI64_PIPE_WIN * (AI64_GTC_HZ / AI64_PIPE_SAMPLE_HZ))
#define AI64_PIPE_CHUNK_GTC  (16u * (AI64_GTC_HZ / AI64_PIPE_SAMPLE_HZ))
/* From one window's end to the next's: half a window. */
#define AI64_PIPE_HOP        (AI64_PIPE_WIN / 2u)
#define AI64_PIPE_HOP_CHUNKS (AI64_PIPE_HOP / 16u)

#define AI64_PIPE_CLASSES    (AI64_PWM_STEP_FREQS * AI64_PWM_STEPS)
/* A count that jumps this far is a stage that started again, not lost
 * windows: start from the newest. */
#define AI64_PIPE_RESYNC     1024u
#define AI64_PIPE_FEATURES   64u

/*
 * ICSSG0's shared RAM (local 0x10000, SoC 0x0b010000): a ring of window
 * slots, then PRU1's and RTU1's records. PRU1 writes a slot's chunks, in
 * time order, each with its newest sample in bit 0; RTU1 writes its tag.
 * Window w is halves w and w + 1 of PRU1's samples: PRU1 writes each half
 * twice, into the second half of one slot and the first half of the next,
 * so every slot holds a whole window, and a copy of it is one burst.
 */
#define AI64_PIPE_SMEM_LOCAL 0x00010000u
#define AI64_PIPE_SMEM_SOC   0x0B010000u
#define AI64_PIPE_SLOTS      16u
#define AI64_PIPE_SLOT_SIZE  0x200u

struct ai64_pipe_tag {
	uint32_t win;          /* the window's number, from 0 at PRU1's start */
	uint32_t t_end_lo;     /* GTC when its last sample was taken */
	uint32_t t_end_hi;
	uint32_t t_rtu;        /* GTC low word when RTU1 saw it */
};

struct ai64_pipe_slot {
	uint16_t chunk[AI64_PIPE_CHUNKS];  /* 0x000 */
	struct ai64_pipe_tag tag;          /* 0x100 */
	uint32_t pad[(AI64_PIPE_SLOT_SIZE - 0x110u) / 4u];
};

/* What a DMA copies out of a slot: the chunks and the tag. */
#define AI64_PIPE_SLOT_COPY  (AI64_PIPE_CHUNKS * 2u + sizeof(struct ai64_pipe_tag))

/* PRU1's record, after the ring. */
#define AI64_PIPE_PRU_OFF    (AI64_PIPE_SLOTS * AI64_PIPE_SLOT_SIZE)
struct ai64_pipe_pru {
	uint32_t magic;
	uint32_t chunks;       /* 16-sample chunks taken */
	uint32_t wins;         /* windows filled: halves taken, less one */
	uint32_t gpcfg;        /* ICSSG_GPCFG1_REG, as PRU1 left it */
};

/*
 * Doorbells: a FIFO of a core's mailbox, each written with a window
 * number. Neither FIFO is used otherwise (boards/ai64_ipc.dtsi). A full
 * FIFO drops the write; the receiver goes by the counts, not the messages.
 */
#define AI64_PIPE_MBOX_R5F0_0 0x31F8B000u /* cluster 11: R5F0_0 is user 1 */
#define AI64_PIPE_MBOX_R5F0_1 0x31F8A000u /* cluster 10: R5F0_1 is user 1 */
#define AI64_PIPE_FIFO        15u
#define AI64_PIPE_MBOX_MSG(base) ((base) + 0x40u + 4u * AI64_PIPE_FIFO)

/*
 * Each C66x's L2 SRAM, the 32 KiB below its job block (c66x/c66x.cmd): the
 * windows R5F0_0's DMA brings, AI64_PIPE_IN_SLOTS deep, and the doorbell.
 * C66x n gets the windows w with w % 2 == n, in slot (w / 2) % IN_SLOTS.
 */
#define AI64_PIPE_C66X_LOCAL 0x00830000u
#define AI64_PIPE_C66X_PHYS(n) (0x4D80830000ull + 0x01000000ull * (n))
#define AI64_PIPE_IN_SLOTS   4u
#define AI64_PIPE_IN_SLOT(w) (((w) / 2u) % AI64_PIPE_IN_SLOTS)

struct ai64_pipe_c66x_in {
	uint32_t ready;                               /* 0x000: one past the newest window brought */
	uint32_t pad[15];
	struct ai64_pipe_slot slot[AI64_PIPE_IN_SLOTS]; /* 0x040 */
};

/*
 * The C7x's L2 SRAM, past its job block. The first 4 KiB, which the C7x
 * maps non-cacheable (c7x/ai64_c7x_mmu.c), take the C66x cores' output: a
 * ring of the last 16 windows' features, the MMA's input rows, their tags,
 * and from each C66x, one past the newest window it wrote. The two write
 * alternate rows. The C7x keeps the weights and the
 * outputs in the cached pages after it.
 */
#define AI64_PIPE_C7X_ADDR   0x64810000u
#define AI64_PIPE_C7X_SIZE   0x00004000u
#define AI64_PIPE_ROWS       16u

struct ai64_pipe_c7x_in {
	uint32_t feat_seq[2];                         /* 0x000: C66x 0's, C66x 1's */
	uint32_t pad[14];
	struct ai64_pipe_tag tag[AI64_PIPE_ROWS];     /* 0x040: t_rtu is t_feat here */
	uint32_t pad2[(0x400u - 0x140u) / 4u];
	int8_t a[AI64_PIPE_ROWS][AI64_PIPE_FEATURES]; /* 0x400 */
};

#define AI64_PIPE_C7X_B   (AI64_PIPE_C7X_ADDR + 0x1000u) /* 64 x 64 weights */
#define AI64_PIPE_C7X_Y   (AI64_PIPE_C7X_ADDR + 0x2000u) /* 16 x 64 outputs */

/* The DDR block: Linux's control record, each stage's numbers, the result
 * ring, the weights, and the feature log. */
#define AI64_PIPE_ADDR       0xA9090000u
#define AI64_PIPE_SIZE       0x00010000u

/* Written by Linux. */
struct ai64_pipe_ctl {
	uint32_t magic;
	uint32_t run;          /* 1: R5F0_0 passes windows on */
	uint32_t follow;       /* 1: R5F0_1 sets P9.25 from the class */
	uint32_t steps_ms;     /* the input's step length, for keeping score */
	uint32_t weights_seq;  /* bumped after new weights; every stage then
				* starts its numbers again */
	uint32_t shift;        /* the layer's output shift */
	uint32_t pad[10];
};

/*
 * One stage's numbers, from its own core. lat is the time from the
 * window's last sample to the stage being done with it, work the time the
 * stage spent on it, both in GTC ticks (5 ns).
 */
struct ai64_pipe_stage {
	uint32_t magic;
	uint32_t windows;      /* passed on */
	uint32_t dropped;      /* windows it never saw, or could not pass on */
	uint32_t last;         /* the last window's number */
	uint32_t lat_sum;
	uint32_t lat_min;
	uint32_t lat_max;
	uint32_t lat_last;
	uint32_t work_sum;
	uint32_t work_max;
	uint32_t errors;
	uint32_t pad[5];
};

#define AI64_PIPE_RTU    0u /* ICSSG0 RTU1 */
#define AI64_PIPE_DMA    1u /* R5F0_0 */
#define AI64_PIPE_FFT(n) (2u + (n)) /* C66x n */
#define AI64_PIPE_MMA    4u /* C7x */
#define AI64_PIPE_PWM    5u /* R5F0_1 */
#define AI64_PIPE_STAGES 6u

/* The C7x's verdict on one window, for R5F0_1. */
struct ai64_pipe_result {
	uint32_t win;
	uint32_t cls;
	int32_t score;
	int32_t second;        /* the runner-up's score */
	uint32_t t_end_lo;
	uint32_t t_end_hi;
	uint32_t t_feat;       /* GTC when the C66x wrote the features */
	uint32_t t_class;      /* GTC when the C7x had the class */
};

#define AI64_PIPE_RESULTS 16u

/* R5F0_1's score, against the step the input was on. */
struct ai64_pipe_score {
	uint32_t magic;
	uint32_t right;
	uint32_t wrong;
	uint32_t mixed;        /* windows across a step change: not scored */
	uint32_t changes;      /* times it set P9.25, since the weights came */
	uint32_t cls;          /* what P9.25 runs now */
	uint32_t waits;        /* windows whose class the window before did not have */
	uint32_t pad[9];
	uint32_t confusion[AI64_PIPE_CLASSES][AI64_PIPE_CLASSES]; /* [truth][class] */
};

/* The C7x's log of every window's features, for training. */
struct ai64_pipe_log {
	struct ai64_pipe_tag tag;
	int8_t f[AI64_PIPE_FEATURES];
};

#define AI64_PIPE_LOG_N 512u

struct ai64_pipe {
	struct ai64_pipe_ctl ctl;                         /* 0x0000 Linux */
	struct ai64_pipe_stage stage[AI64_PIPE_STAGES];   /* 0x0040 each its own */
	uint32_t pad0[(0x200u - 0x40u - AI64_PIPE_STAGES * sizeof(struct ai64_pipe_stage)) / 4u];
	struct ai64_pipe_score score;                     /* 0x0200 R5F0_1 */
	uint32_t pad1[(0x800u - 0x200u - sizeof(struct ai64_pipe_score)) / 4u];
	struct ai64_pipe_result result[AI64_PIPE_RESULTS]; /* 0x0800 C7x */
	uint32_t pad2[(0x1000u - 0x800u - AI64_PIPE_RESULTS * sizeof(struct ai64_pipe_result)) / 4u];
	int8_t weights[AI64_PIPE_FEATURES][AI64_PIPE_FEATURES]; /* 0x1000 Linux */
	uint32_t log_n;                                   /* 0x2000 C7x: entries written */
	uint32_t pad3[15];
	struct ai64_pipe_log log[AI64_PIPE_LOG_N];        /* 0x2040 C7x */
};

/* A stage starts its numbers again: at its start, and on new weights. */
static inline void ai64_pipe_stage_reset(volatile struct ai64_pipe_stage *st)
{
	st->magic = 0;
	st->windows = 0;
	st->dropped = 0;
	st->last = 0;
	st->lat_sum = 0;
	st->lat_min = 0xffffffffu;
	st->lat_max = 0;
	st->lat_last = 0;
	st->work_sum = 0;
	st->work_max = 0;
	st->errors = 0;
	st->magic = AI64_PIPE_MAGIC;
}

/* A window passed on: lat and work as the stage measured them. */
static inline void ai64_pipe_stage_count(volatile struct ai64_pipe_stage *st, uint32_t w,
					 uint32_t lat, uint32_t work)
{
	st->windows++;
	st->last = w;
	st->lat_last = lat;
	st->lat_sum += lat;
	if (lat < st->lat_min) {
		st->lat_min = lat;
	}
	if (lat > st->lat_max) {
		st->lat_max = lat;
	}
	st->work_sum += work;
	if (work > st->work_max) {
		st->work_max = work;
	}
}

#ifndef PRU_CORE
/*
 * The class of the step the input was on over a window ending at GTC t_end,
 * or -1 when a step change falls inside it (with margin_gtc either side).
 */
static inline int32_t ai64_pipe_truth(uint64_t t_end, uint32_t steps_ms, uint32_t margin_gtc)
{
	uint64_t len = AI64_PWM_STEP_GTC(steps_ms);
	uint64_t a = t_end - AI64_PIPE_WIN_GTC - margin_gtc;
	uint64_t b = t_end + margin_gtc;

	if (steps_ms == 0u || a / len != b / len) {
		return -1;
	}
	return (int32_t)((a / len) % AI64_PIPE_CLASSES);
}
#endif

#endif
