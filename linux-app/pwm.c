/* SPDX-License-Identifier: MIT */
/*
 * ai64 pwm [seconds|once]
 *   Prints what the four PWM generators run: ICSSG0 slice 0 (software),
 *   its copy of A on ICSSG0's eCAP (P8.15), ICSSG0 slice 1 (the ICSSG PWM
 *   module), and EHRPWM4 on P9.25 (Zephyr), from the status each one's
 *   talking core publishes in its fabric page.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ai64.h"
#include "ai64_shm.h"
#include "ai64_fabric.h"

/*
 * The four PWM generators: the fabric node whose page holds its status
 * (struct ai64_pwm_status, at word rec of the app block), the core that
 * writes it, its pins, and what makes the edges.
 */
static const struct {
	const char *pins;
	const char *a, *b;
	uint32_t node, rec;
	const char *core; /* its label in cores.c */
	const char *how;
} pwm_gens[] = {
	{ "P8.11 + P8.12", "P8.11", "P8.12", AI64_FAB_G0RTU0, 0, "g0rtu0",
	  "software PWM: ICSSG0 slice 0, PRU0 sets the pins at the times it reads from IEP0" },
	{ "P8.15", "P8.15", NULL, AI64_FAB_G0RTU0, AI64_PWM_STATUS2_WORDS, "g0rtu0",
	  "hardware PWM: ICSSG0's eCAP0 in APWM mode, a copy of P8.11 (A) that Tx_PRU0 sets" },
	{ "P9.12 + P9.15", "P9.12", "P9.15", AI64_FAB_G0RTU1, 0, "g0rtu1",
	  "hardware PWM: ICSSG0 slice 1, the ICSSG's PWM module toggles the pins on IEP1 compares" },
	{ "P9.25", "P9.25", NULL, AI64_FAB_R5F0_1, 0, "r5f0_1",
	  "hardware PWM: the SoC's EHRPWM4, output B, set by Zephyr on R5F0_1 (Zephyr's PWM driver)" },
};

#define NPWM (sizeof(pwm_gens) / sizeof(pwm_gens[0]))

static void pwm_program(const volatile struct ai64_pwm_status *s)
{
	static const uint32_t hz[AI64_PWM_STEP_FREQS] = AI64_PWM_STEP_HZ;
	static const uint32_t pct[AI64_PWM_STEPS] = AI64_PWM_STEP_PCT;
	size_t i;

	if (s->steps_ms) {
		printf("  stepping: ");
		for (i = 0; i < AI64_PWM_STEP_FREQS; i++) {
			printf("%s%u", i ? ", " : "", hz[i] / 1000u);
		}
		printf(" kHz, at each ");
		for (i = 0; i < AI64_PWM_STEPS; i++) {
			printf("%s%u", i ? ", " : "", pct[i]);
		}
		printf("%% duty, %u ms a step\n", s->steps_ms);
	} else if (s->ask_period_ns == 0) {
		printf("  following the pipeline: the class the C7x finds on P8.31 (ai64 pipe)\n");
	} else if (s->sweep_ms) {
		printf("  sweeping: the duty from its least to its most and back every %u ms\n",
		       s->sweep_ms);
	} else {
		printf("  fixed: period %u ns, high %u ns, as asked\n", s->ask_period_ns, s->ask_duty_ns);
	}
}

/*
 * ai64 pwm [seconds|once]: a few lines per generator every few seconds,
 * from what its talking core publishes in shared memory. Linux reads no
 * ICSSG0 or EHRPWM register, and nothing here is measured on the pins.
 */
int pwm_main(int argc, char **argv)
{
	volatile struct ai64_pwm_status *st[NPWM] = { 0 };
	uint32_t at[NPWM] = { 0 }, periods[NPWM] = { 0 };
	int once, every = every_arg(argc, argv, &once);
	double t0 = now_us();
	char state[32];
	size_t g;

	if (every <= 0) {
		fprintf(stderr, "usage: %s pwm [seconds|once]\n", argv[0]);
		return 2;
	}
	if (access(FABRIC_DT, F_OK)) {
		fprintf(stderr, "%s is missing: the ai64-demo overlay is not applied.\n", FABRIC_DT);
		return 1;
	}
	for (g = 0; g < NPWM; g++) {
		volatile uint8_t *page = map_phys(AI64_FAB_ADDR + pwm_gens[g].node * AI64_FAB_PAGE,
						  AI64_FAB_PAGE);

		if (!page) {
			return 1;
		}
		st[g] = (volatile struct ai64_pwm_status *)
			&((volatile struct ai64_fab_page *)page)->app[pwm_gens[g].rec];
		at[g] = st[g]->at;
		periods[g] = st[g]->periods;
	}
	if (!once) {
		printf("pwm window ready: every %d s, what each PWM generator's core reports it runs\n"
		       "(read from shared memory; \"now\" is whichever step it was on)\n", every);
	}
	fflush(stdout);
	for (;;) {
		double t1, dt;

		sleep(once ? 1 : (unsigned int)every);
		t1 = now_us();
		dt = (t1 - t0) / 1e6;
		t0 = t1;
		for (g = 0; g < NPWM; g++) {
			volatile struct ai64_pwm_status *s = st[g];
			uint32_t period, duty, dead, n;

			printf("%-14s %s\n", pwm_gens[g].pins, pwm_gens[g].how);
			if (!rproc_running(core_get(pwm_gens[g].core)->rproc, state, sizeof(state))) {
				printf("  its talking core, %s, is not running\n",
				       core_get(pwm_gens[g].core)->rproc);
				continue;
			}
			if (s->magic != AI64_PWM_MAGIC) {
				printf("  no status yet\n");
				continue;
			}
			if (s->at == at[g]) {
				printf("  its status has stopped\n");
				continue;
			}
			at[g] = s->at;
			period = s->run_period_ns;
			duty = s->run_duty_ns;
			dead = s->run_dead_ns;
			n = s->periods - periods[g];
			periods[g] = s->periods;
			pwm_program(s);
			printf("  now: %.3f kHz, %.1f%% duty\n", period ? 1e6 / period : 0.0,
			       period ? 100.0 * duty / period : 0.0);
			if (pwm_gens[g].b) {
				printf("  %s (A) high %u ns of each %u ns period, from %u ns in\n",
				       pwm_gens[g].a, duty, period, AI64_PWM_T0_NS);
				printf("  %s (B) high %u ns: A inverted, but low for %u ns of dead band "
				       "either side of A's pulse\n", pwm_gens[g].b,
				       period - AI64_PWM_T0_NS - duty - 2u * dead, dead);
				printf("  %.0f periods/s, %u late edges", n / dt, s->late);
				if (s->engine == AI64_PWM_SOFT) {
					printf(" (the latest edge %u ns late)", s->late_max_ns);
				}
				printf("\n");
			} else {
				printf("  %s high %u ns of each %u ns period\n", pwm_gens[g].a, duty, period);
				if (s->engine == AI64_PWM_ECAP) {
					printf("  %.0f periods/s, counted from the eCAP's period flag\n", n / dt);
				}
			}
			if (s->ctl_ack != s->ctl_seq) {
				printf("  its last request is not running yet\n");
			}
		}
		fflush(stdout);
		if (once) {
			return 0;
		}
	}
}
