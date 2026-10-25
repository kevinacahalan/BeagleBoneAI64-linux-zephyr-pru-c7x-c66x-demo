/* SPDX-License-Identifier: MIT */
/*
 * ICSSG0 PRU1: PWM from the ICSSG's PWM module (SPRUJ61 chapter 11). Set
 * PWM2 runs on IEP1. Its first pair, PWM2_0, is A on PRG0_PWM2_A0 (P9.12),
 * toggled by IEP1 CMP1, and B on PRG0_PWM2_B0 (P9.15), toggled by CMP2.
 * CMP0 ends the period and resets the counter. The hardware makes every
 * edge on the IEP clock, so none of them jitter.
 *
 * On this ICSSG (unlike AM64x's, which has PWM_EFC_EN) a compare only fires
 * again once software clears its status bit, and an output only toggles,
 * so this core rearms each compare for that pin's next edge as soon as
 * the last one fired: A's compare alternates between its rise and its
 * fall, and so does B's. TI's AM65x PWM firmware works the same way; that
 * is where its 400 ns minimum pulse comes from (AI64_PWM_EDGE_MIN_NS).
 *
 * A period's parameters are taken at B's rise in the period before, from
 * the next record (pwm_ipc.h), and the counter's period at its reset.
 *
 * The first compare event of a PWM set only takes the set from its initial
 * state to its active one; it toggles nothing. Only the events after it
 * toggle. So an unused compare of the same set, CMP5, fires once at 8 ns,
 * before A's first edge; without it A would run inverted.
 *
 * It also samples P8.31 for the pipeline (ai64_pipe.h). Its R31 runs in
 * 28-bit shift mode (SPRUJ61 6.2.2.3.3): the GPI shift register takes
 * PRG0_PRU1_GPI0 at 2 MHz, from the core clock through two dividers, with
 * the newest sample in bit 0, and sets R31 bit 28 (Cnt_16) every 16
 * samples, for one sample's time. The loop comes round many times in that
 * 500 ns, so it takes a chunk where the bit rises: R31's low 16 bits, read
 * in the same load, are the 16 samples since the last. (ICSSG0's INTC
 * latches the bit as system event 2, but cleared it latches again while
 * the bit is still high.) A window of 128 such chunks goes into a slot of
 * the ring in shared RAM, and RTU1 takes it from there. The hardware times
 * every sample, so this costs the PWM no edge: a chunk is two local loads
 * and two stores, once every 8 us.
 */

#include <stdint.h>
#include "icssg.h"
#include "pwm_ipc.h"
#include "ai64_pipe.h"

#define AI64_TRACE_SIZE 256
#define AI64_TRACE_NAME "trace:pwm-pru1"
#include "ai64_rsc_trace.h"

#define IEP       ICSSG_IEP1
#define PWM_SET   2u
#define CMP_A     1u
#define CMP_B     2u
#define BIT_A     (1u << CMP_A)
#define BIT_B     (1u << CMP_B)
/* PWM2_2's positive compare. Its pin is not muxed; it fires once, to start the set. */
#define CMP_START 5u
#define BIT_START (1u << CMP_START)
#define START_NS  8u

#define COUNT()        ICSSG_REG(IEP_COUNT_LO(IEP))
#define CMP_STATUS()   ICSSG_REG(IEP_CMP_STATUS(IEP))
#define CMP(n)         ICSSG_REG(IEP_CMP_LO(IEP, n))

static uint32_t late;

volatile register uint32_t __R31;

/* The GPI shift register: ICSSG_GPCFG1_REG and ICSSG_GPECFG1_REG (PRU1's). */
#define CFG_GPCFG1        (ICSSG_CFG + 0x0cu)
#define CFG_GPECFG1       (ICSSG_CFG + 0x18u)
#define GPI_MODE_SHIFT    2u
#define GPI_DIV0(code)    ((uint32_t)(code) << 3)  /* divide by 1 + code / 2 */
#define GPI_DIV1(code)    ((uint32_t)(code) << 8)
#define GPI_SB_CLR        (1u << 13)
#define GPO_DIV0(code)    ((uint32_t)(code) << 15)
#define GPO_DIV1(code)    ((uint32_t)(code) << 20)
#define GPECFG_SB_P       (1u << 0)  /* the start bit is the first 1 */
#define GPECFG_SHIFT_EN   (1u << 1)
/* 250 MHz / 12.5 / 10 = AI64_PIPE_SAMPLE_HZ. The guide's steps name the
 * GPO dividers for this, its register table the GPI ones: both are set. */
#define DIV_12_5          0x17u
#define DIV_10            0x12u

#define R31_CNT16         (1u << 28)

#define PIPE_PRU ((volatile struct ai64_pipe_pru *)(AI64_PIPE_SMEM_LOCAL + AI64_PIPE_PRU_OFF))

static void sampler_init(void)
{
	uint32_t cfg = GPI_MODE_SHIFT | GPI_DIV0(DIV_12_5) | GPI_DIV1(DIV_10) |
		       GPO_DIV0(DIV_12_5) | GPO_DIV1(DIV_10);

	/* Off, the start bit's polarity, the start bit cleared, the clock, on. */
	ICSSG_REG(CFG_GPECFG1) = GPECFG_SB_P;
	ICSSG_REG(CFG_GPCFG1) = cfg | GPI_SB_CLR;
	ICSSG_REG(CFG_GPCFG1) = cfg;
	PIPE_PRU->magic = 0;
	PIPE_PRU->chunks = 0;
	PIPE_PRU->wins = 0;
	PIPE_PRU->gpcfg = ICSSG_REG(CFG_GPCFG1);
	PIPE_PRU->magic = AI64_PIPE_MAGIC;
	ICSSG_REG(CFG_GPECFG1) = GPECFG_SB_P | GPECFG_SHIFT_EN;
}

/* Arm a compare for later in this period; count it if the time has passed. */
static inline void arm_now(uint32_t n, uint32_t at)
{
	CMP(n) = at;
	if ((int32_t)(COUNT() - at) >= 0) {
		late++;
	}
}

static int read_next(uint32_t *seen, uint32_t *period, uint32_t *duty, uint32_t *dead)
{
	volatile struct pwm_next *n = PWM_NEXT;
	uint32_t s = n->seq, p, d, dt;

	if (s == *seen || (s & 1u)) {
		return 0;
	}
	p = n->period_ns;
	d = n->duty_ns;
	dt = n->dead_ns;
	if (n->seq != s) {
		return 0;
	}
	*seen = s;
	*period = p;
	*duty = d;
	*dead = dt;
	return 1;
}

void main(void)
{
	volatile struct pwm_out *out = PWM_OUT;
	uint32_t seen = 0, period = 0, duty = 0, dead = 0;
	/* What the next period runs, taken at B's rise. */
	uint32_t n_period = 0, n_duty = 0, n_dead = 0, n_seen = 0;
	uint32_t periods = 0, a_high = 0, b_high = 0;
	/* The sampler: where the next chunk goes, and the counts. */
	volatile uint16_t *chunk = (volatile uint16_t *)AI64_PIPE_SMEM_LOCAL;
	uint32_t c = 0, slot = 0, chunks = 0, wins = 0, cnt16 = R31_CNT16;

	trace_init();

	/* Stop IEP1 and the PWM set where a restart left them. */
	ICSSG_REG(IEP_GLOBAL_CFG(IEP)) = IEP_DEFAULT_INC(ICSSG_NS_PER_CYCLE) |
					 IEP_CMP_INC(ICSSG_NS_PER_CYCLE);
	ICSSG_REG(IEP_CMP_CFG(IEP)) = 0;
	ICSSG_REG(ICSSG_CFG_IEPCLK) |= ICSSG_IEPCLK_SYNC;

	out->magic = 0;
	out->periods = 0;
	out->late = 0;
	out->late_max_ns = 0;
	out->magic = PWM_OUT_MAGIC;
	trace_puts("ICSSG0 PRU1 up: ICSSG PWM2_0 on IEP1, A on P9.12, B on P9.15\n");
	sampler_init();
	trace_puts("sampling P8.31 at 2 MHz, 2048 a window\n");

	while (!read_next(&seen, &period, &duty, &dead)) {
	}
	n_period = period;
	n_duty = duty;
	n_dead = dead;
	n_seen = seen;
	trace_puts("first period: ");
	trace_u32(period);
	trace_puts(" ns, duty ");
	trace_u32(duty);
	trace_puts(" ns\n");

	/*
	 * Both outputs start low and toggle on their compare: low in the
	 * initial and trip states, toggle in the active one. The trip inputs
	 * are masked; nothing here wires one.
	 */
	ICSSG_REG(ICSSG_CFG_PWM_STATE(PWM_SET, 0)) =
		ICSSG_PWM_STATE(ICSSG_PWM_LOW, ICSSG_PWM_LOW, ICSSG_PWM_LOW, ICSSG_PWM_LOW,
				ICSSG_PWM_TOGGLE, ICSSG_PWM_TOGGLE);
	ICSSG_REG(ICSSG_CFG_PWM(PWM_SET)) = ICSSG_PWM_TRIP_MASK_ALL | ICSSG_PWM_TRIP_RESET;
	ICSSG_REG(ICSSG_CFG_PWM(PWM_SET)) = ICSSG_PWM_TRIP_MASK_ALL;

	ICSSG_REG(IEP_COUNT_LO(IEP)) = 0;
	ICSSG_REG(IEP_COUNT_HI(IEP)) = 0;
	ICSSG_REG(IEP_CMP_HI(IEP, 0)) = 0;
	ICSSG_REG(IEP_CMP_HI(IEP, CMP_A)) = 0;
	ICSSG_REG(IEP_CMP_HI(IEP, CMP_B)) = 0;
	ICSSG_REG(IEP_CMP_HI(IEP, CMP_START)) = 0;
	CMP(CMP_START) = START_NS;
	CMP(0) = period - ICSSG_NS_PER_CYCLE;
	CMP(CMP_A) = PWM_T0_NS;
	CMP(CMP_B) = PWM_T0_NS + duty + dead;
	CMP_STATUS() = 0xffffu;
	ICSSG_REG(IEP_CMP_CFG(IEP)) = IEP_CMP0_RST_CNT_EN | IEP_CMP_EN(0) |
				      IEP_CMP_EN(CMP_A) | IEP_CMP_EN(CMP_B) | IEP_CMP_EN(CMP_START);
	ICSSG_REG(IEP_GLOBAL_CFG(IEP)) |= IEP_CNT_ENABLE;

	for (;;) {
		uint32_t s = CMP_STATUS();
		uint32_t r = __R31;

		/* cnt16 starts set, so a chunk already up when the loop starts is skipped. */
		if ((r & ~cnt16) & R31_CNT16) {
			chunk[c] = (uint16_t)r;
			chunks++;
			PIPE_PRU->chunks = chunks;
			if (++c == AI64_PIPE_CHUNKS) {
				c = 0;
				wins++;
				PIPE_PRU->wins = wins;
				slot = slot + 1u == AI64_PIPE_SLOTS ? 0u : slot + 1u;
				chunk = (volatile uint16_t *)(AI64_PIPE_SMEM_LOCAL +
							      slot * AI64_PIPE_SLOT_SIZE);
			}
		}
		cnt16 = r & R31_CNT16;

		if (s & 1u) {
			/* The counter wrapped: a new period, maybe a new length. */
			CMP(0) = period - ICSSG_NS_PER_CYCLE;
			CMP_STATUS() = 1u;
			periods++;
			out->periods = periods;
			out->next_seq = seen;
			out->period_ns = period;
			out->duty_ns = duty;
			out->dead_ns = dead;
			out->late = late;
		}
		if (s & BIT_START) {
			ICSSG_REG(IEP_CMP_CFG(IEP)) &= ~IEP_CMP_EN(CMP_START);
			CMP_STATUS() = BIT_START;
		}
		if (s & BIT_A) {
			a_high ^= 1u;
			if (a_high) {
				arm_now(CMP_A, PWM_T0_NS + duty);
			} else {
				CMP(CMP_A) = PWM_T0_NS;
			}
			CMP_STATUS() = BIT_A;
		}
		if (s & BIT_B) {
			b_high ^= 1u;
			if (b_high) {
				arm_now(CMP_B, period - dead);
				/* The rest of this period is set; take the next one's. */
				(void)read_next(&n_seen, &n_period, &n_duty, &n_dead);
			} else {
				/* B's fall is the period's last edge: switch over. */
				period = n_period;
				duty = n_duty;
				dead = n_dead;
				seen = n_seen;
				CMP(CMP_B) = PWM_T0_NS + duty + dead;
			}
			CMP_STATUS() = BIT_B;
		}
	}
}
