/* SPDX-License-Identifier: MIT */
/*
 * Shared layout for the BeagleBone AI-64 demo. Linux, Zephyr, and the ICSSG
 * cores include this file.
 *
 * Bulk region: Linux writes chunks, Zephyr on R5F0_0 reads them. In DDR,
 * inside R5F0_0's carveout.
 * The PWM generators: the limits every one of them accepts, and the status
 * record each publishes in the app block of its fabric page
 * (ai64_fabric.h), for any core to read. Three of them: ICSSG0 slice 0
 * makes PWM in software, ICSSG0 slice 1 with the ICSSG's PWM module
 * (pru/pwm/), and Zephyr on R5F0_0 with the SoC's EHRPWM4 on P9.25.
 *
 * Do not put a loadable ELF section on the DDR addresses. The Zephyr overlay
 * and these macros must stay on the same addresses.
 */
#ifndef AI64_SHM_H_
#define AI64_SHM_H_

#include <stdint.h>

/*
 * R5F0_0 owns 0xa2000000-0xa2ffffff (Linux reserved-memory, no-map).
 * Zephyr's board file splits it into vrings (1 MiB), resource table (1 MiB),
 * and DRAM (14 MiB at 0xa2200000). zephyr/r5/boards/ shrinks DRAM so the
 * last 68 KiB hold the bulk buffer and a spare 4 KiB page.
 */
#define AI64_BULK_ADDR  0xA2FEF000u
#define AI64_BULK_SIZE  0x00010000u
#define AI64_BULK_MAGIC 0x41493642u /* 'AI6B' */

struct ai64_bulk {
	uint32_t magic;
	uint32_t seq;
	uint32_t length;
	uint32_t crc;
	uint8_t data[AI64_BULK_SIZE - 16u];
};

/*
 * One PWM period, in nanoseconds: output A is high for duty_ns, and output
 * B, where there is one, is A's complement with dead_ns of dead band on
 * each side. The ICSSG generators keep every edge AI64_PWM_EDGE_MIN_NS from
 * the next on the same pin, so 0% and 100% duty are out of range; they
 * clamp what they are asked for and report what they run.
 */
#define AI64_PWM_PERIOD_MIN_NS 2000u
#define AI64_PWM_PERIOD_MAX_NS 1000000000u
#define AI64_PWM_EDGE_MIN_NS   400u
#define AI64_PWM_DEAD_MIN_NS   8u
#define AI64_PWM_SWEEP_MAX_MS  60000u
/* The ICSSG generators' A rises this far into each period (pwm_ipc.h). */
#define AI64_PWM_T0_NS         40u

/* What the ICSSG generators start with: 20 kHz, 25%, 1 us dead band. */
#define AI64_PWM_DEFAULT_PERIOD_NS 50000u
#define AI64_PWM_DEFAULT_DUTY_NS   12500u
#define AI64_PWM_DEFAULT_DEAD_NS   1000u

/*
 * Steps: every generator can run a short program, a step every steps_ms:
 * at each frequency of AI64_PWM_STEP_HZ in turn, each duty of
 * AI64_PWM_STEP_PCT (percent of the period) in turn, then over again. That
 * is 15 steps, 75 ms at 5 ms a step. All three start doing it, so a logic
 * analyzer shows them change together; a request for a fixed period and
 * duty stops it.
 */
#define AI64_PWM_STEP_FREQS        3u
#define AI64_PWM_STEP_HZ           { 10000u, 15000u, 20000u }
#define AI64_PWM_STEPS             5u
#define AI64_PWM_STEP_PCT          { 10u, 25u, 50u, 75u, 90u }
#define AI64_PWM_DEFAULT_STEPS_MS  5u
#define AI64_PWM_STEPS_MAX_MS      60000u

/* What makes the edges */
#define AI64_PWM_SOFT   0u /* a PRU writing R30, timed by IEP0 */
#define AI64_PWM_ICSSG  1u /* the ICSSG's PWM module, on IEP1 compares */
#define AI64_PWM_EHRPWM 2u /* a SoC EHRPWM, through Zephyr's driver */

#define AI64_PWM_MAGIC 0x41493650u /* 'AI6P' */

/*
 * The generator's status, rewritten about every 10 ms by its talking core:
 * the slice's RTU, or R5F0_0. magic is cleared while it writes. The ask_
 * fields are the last request, the run_ fields what the outputs do now.
 * late counts edges the real-time core was late for: a software edge more
 * than AI64_PWM_LATE_NS late, or a compare it rearmed after the time had
 * passed. late_max_ns is the latest software edge since start.
 */
#define AI64_PWM_LATE_NS 100u

struct ai64_pwm_status {
	uint32_t magic;
	uint32_t engine;     /* AI64_PWM_SOFT, AI64_PWM_ICSSG, AI64_PWM_EHRPWM */
	uint32_t unit;       /* the ICSSG, or the EHRPWM instance */
	uint32_t channel;    /* the slice, or the EHRPWM output (0 A, 1 B) */
	uint32_t rpmsg;      /* 1 when Linux can also reach it over RPMsg */
	uint32_t ask_period_ns;
	uint32_t ask_duty_ns;
	uint32_t ask_dead_ns;
	uint32_t sweep_ms;   /* 0, or the duty sweeps min-max-min this often */
	uint32_t steps_ms;   /* 0, or it steps through AI64_PWM_STEP_HZ x AI64_PWM_STEP_PCT, this long each */
	uint32_t run_period_ns;
	uint32_t run_duty_ns;
	uint32_t run_dead_ns;
	uint32_t periods;    /* since the real-time core started */
	uint32_t late;
	uint32_t late_max_ns;
	uint32_t ctl_seq;    /* requests taken */
	uint32_t ctl_ack;    /* requests the outputs follow */
	uint32_t at;         /* GTC when it was written */
};

#ifndef PRU_CORE
static inline uint32_t ai64_crc32(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xffffffffu;
	uint32_t i, b;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 0; b < 8u; b++) {
			uint32_t mask = -(crc & 1u);

			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}
	return ~crc;
}
#endif

#endif
