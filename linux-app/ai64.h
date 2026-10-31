/* SPDX-License-Identifier: MIT */
#ifndef AI64_H_
#define AI64_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * What the ai64 subcommands share. Each subcommand is a file of its own,
 * with a <name>_main(argc, argv) that ai64.c calls; README.md in this
 * directory lists them.
 */

/* Every demo core, cores.c. */
struct ai64_core {
	const char *label;  /* r5f0_0, g0rtu0, g1pru1, c66x0, c7x */
	const char *rproc;  /* its remoteproc name, 5c00000.r5f */
	const char *fw;     /* its firmware in /lib/firmware */
	const char *group;  /* r5, g0, g1, or dsp: struct ai64_group */
	const char *name;   /* within its group: R5F0_0, PRU0, C66x 0 */
	const char *role;   /* what it does, in a few words */
	int start;          /* ai64-demo starts its group in this order */
	int node;           /* its fabric node (ai64_fabric.h), or -1 */
	/* Linux's mailbox FIFO to it (ti,mbox-tx), for ai64 shutdown; base 0: none. */
	uint32_t mbox;
	uint32_t fifo;
	/* A DSP: its job block (ai64_dsp_job.h), its clock, and its L2 SRAM, as
	 * the core sees it (da) and in the SoC (pa). */
	uint64_t job;
	double mhz;
	uint32_t l2_da, l2_len;
	uint64_t l2_pa;
	/* An ICSSG core: its ICSSG's base and its slice. */
	uint64_t icssg;
	uint32_t slice;
};

struct ai64_group {
	const char *group;
	const char *title;
};

extern const struct ai64_core ai64_cores[];
extern const size_t ai64_ncores;
extern const struct ai64_group ai64_groups[];
extern const size_t ai64_ngroups;

/* The core with this label or remoteproc name, or NULL. */
const struct ai64_core *core_find(const char *label_or_rproc);
/* The same, but exits with a message naming what it wanted. */
const struct ai64_core *core_get(const char *label);

/* util.c */
double now_us(void);
void *map_phys(off_t addr, size_t len);
int write_all(int fd, const void *buf, size_t len);
int read_sysfs(const char *path, char *buf, size_t len);
/* A remoteproc attribute (firmware, state) of the core called rproc, or -1. */
int rproc_attr(const char *rproc, const char *attr, char *val, size_t len);
/* Whether it runs; state, if not NULL, gets its state. */
int rproc_running(const char *rproc, char *state, size_t len);
/* "once" or a number of seconds, for the pwm and sched windows. */
int every_arg(int argc, char **argv, int *once);
/* Drop every double quote. */
void unquote(char *s);
/* A window's result lines also go to the linux window, on <fifo>.reply
 * for a window that reads <fifo>.in. */
void reply_to(const char *fifopath);
void note_line(const char *line);

/* The C7x's layer, on the A72 (mma.c). */
void layer_ref(const int8_t *a, const int8_t *b, int8_t *y, uint32_t rows, uint32_t shift,
	       uint32_t relu);
/* Hand a DSP one job through its job block and wait for it (dsp.c). */
int dsp_run(volatile uint32_t *job, uint32_t op, uint32_t count, uint32_t taps);

#define FABRIC_DT "/proc/device-tree/reserved-memory/ai64-fabric@a9000000"

/* The subcommands. */
int bench_main(int argc, char **argv);
int cores_main(int argc, char **argv);
int dsp_main(int argc, char **argv);
int erpc_main(int argc, char **argv);
int fabric_main(int argc, char **argv);
int linux_main(int argc, char **argv);
int mma_main(int argc, char **argv);
int pads_main(int argc, char **argv);
int peer_main(int argc, char **argv);
int pipe_main(int argc, char **argv);
int pwm_main(int argc, char **argv);
int sched_main(int argc, char **argv);
int shutdown_main(int argc, char **argv);
int trace_main(int argc, char **argv);
int web_main(int argc, char **argv);

/* The linux window's command list (linux_window.c). */
void print_help(void);

#endif
