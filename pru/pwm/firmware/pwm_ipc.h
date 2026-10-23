/* SPDX-License-Identifier: MIT */
/*
 * The four records a PWM slice's cores pass each other, in the IPC block
 * of the slice's data RAM (pwm_map.h). Each has one writer:
 *
 *   RTU --pwm_ctl--> Tx_PRU --pwm_next--> PRU --pwm_out--> RTU, Tx_PRU
 *                    Tx_PRU --pwm_mod---> RTU
 *
 * The RTU takes requests from the rest of the SoC and writes them to
 * pwm_ctl. The Tx_PRU, the modulator, turns them into the parameters of
 * each coming period: the requested ones, a duty that steps through
 * AI64_PWM_STEP_PCT, or one that sweeps. The PRU makes the edges and reads
 * pwm_next once a period. A writer fills in a record before the field that
 * says it changed (seq), so a reader that sees the new seq sees the new
 * fields too. pwm_next is read in the middle of a
 * PWM period, so it is a seqlock: odd while the Tx_PRU writes it.
 *
 * Both slices make the same waveform, in nanoseconds from the start of a
 * period of length P with duty D and dead band d:
 *
 *   A  high from T0 to T0 + D
 *   B  high from T0 + D + d to P - d
 *
 * T0 is PWM_T0_NS: an IEP compare at count 0 never fires, so the ICSSG
 * PWM module's period starts there, and the software slice does the same.
 */
#ifndef PWM_IPC_H_
#define PWM_IPC_H_

#include <stdint.h>
#include "ai64_shm.h"
#include "pwm_map.h"

#define PWM_T0_NS 40u

#define PWM_CTL_MAGIC 0x41493651u /* 'AI6Q' */
#define PWM_OUT_MAGIC 0x41493652u /* 'AI6R' */
#define PWM_MOD_MAGIC 0x41493653u /* 'AI6S' */

/* What the RTU was asked for, already clamped (pwm_clamp). */
struct pwm_ctl {
	uint32_t magic;
	uint32_t seq;
	uint32_t period_ns;
	uint32_t duty_ns;
	uint32_t dead_ns;
	uint32_t sweep_ms;
	uint32_t steps_ms; /* steps and sweep are never both set */
};

/* The coming periods' parameters. seq is odd while the Tx_PRU writes. */
struct pwm_next {
	uint32_t seq;
	uint32_t period_ns;
	uint32_t duty_ns;
	uint32_t dead_ns;
};

/* What the PRU runs, rewritten every period. */
struct pwm_out {
	uint32_t magic;
	uint32_t periods;
	uint32_t next_seq;
	uint32_t period_ns;
	uint32_t duty_ns;
	uint32_t dead_ns;
	uint32_t late;
	uint32_t late_max_ns;
};

/* The modulator's state. */
struct pwm_mod {
	uint32_t magic;
	uint32_t ctl_seq;  /* the pwm_ctl seq it follows */
	uint32_t updates;  /* pwm_next records it wrote */
	uint32_t duty_ns;  /* the last duty it wrote */
};

#define PWM_CTL  ((volatile struct pwm_ctl *)PWM_IPC_CTL)
#define PWM_NEXT ((volatile struct pwm_next *)PWM_IPC_NEXT)
#define PWM_OUT  ((volatile struct pwm_out *)PWM_IPC_OUT)
#define PWM_MOD  ((volatile struct pwm_mod *)PWM_IPC_MOD)

/* The longest duty a period and dead band leave room for. */
static inline uint32_t pwm_duty_max(uint32_t period, uint32_t dead)
{
	uint32_t used = PWM_T0_NS + 2u * dead + AI64_PWM_EDGE_MIN_NS;

	return period > used + AI64_PWM_EDGE_MIN_NS ? period - used : AI64_PWM_EDGE_MIN_NS;
}

/* Bring a request into range: the period first, then the dead band, then the duty. */
static inline void pwm_clamp(uint32_t *period, uint32_t *duty, uint32_t *dead)
{
	uint32_t dead_max;

	if (*period < AI64_PWM_PERIOD_MIN_NS) {
		*period = AI64_PWM_PERIOD_MIN_NS;
	}
	if (*period > AI64_PWM_PERIOD_MAX_NS) {
		*period = AI64_PWM_PERIOD_MAX_NS;
	}
	/* Room for a minimum duty and a minimum B pulse. */
	dead_max = (*period - PWM_T0_NS - 3u * AI64_PWM_EDGE_MIN_NS) / 2u;
	if (*dead < AI64_PWM_DEAD_MIN_NS) {
		*dead = AI64_PWM_DEAD_MIN_NS;
	}
	if (*dead > dead_max) {
		*dead = dead_max;
	}
	if (*duty < AI64_PWM_EDGE_MIN_NS) {
		*duty = AI64_PWM_EDGE_MIN_NS;
	}
	if (*duty > pwm_duty_max(*period, *dead)) {
		*duty = pwm_duty_max(*period, *dead);
	}
}

#endif
