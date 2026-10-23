/* SPDX-License-Identifier: MIT */
/*
 * The J721E PRU_ICSSG blocks the demo programs, as every core of an ICSSG
 * sees them at its local addresses. The register names and offsets are
 * TI's, from the J721E PRU user's guide (SPRUJ61): CFG in section 15.5,
 * IEP in 15.9, and the task managers in 15.19.
 *
 * Only what the demo uses is here. PSSP ships C headers with bit fields for
 * some of these blocks for other SoCs, but not for J721E.
 */
#ifndef AI64_ICSSG_H_
#define AI64_ICSSG_H_

#include <stdint.h>

#define ICSSG_REG(addr) (*(volatile uint32_t *)(addr))

/* Every core's view: its slice's data RAM, the other slice's, shared RAM. */
#define ICSSG_DRAM_OWN    0x00000000u
#define ICSSG_DRAM_OTHER  0x00002000u
#define ICSSG_DRAM_SIZE   0x00002000u
#define ICSSG_SHARED      0x00010000u
#define ICSSG_SHARED_SIZE 0x00010000u

/* SoC base of each ICSSG; local address a is at base + a. */
#define ICSSG0_SOC 0x0B000000u
#define ICSSG1_SOC 0x0B100000u

/*
 * CFG. IEPCLK bit 0 runs the IEPs on the ICSSG core clock (sync mode); it
 * only clears on a hardware reset. The PWM module (SPRUJ61 chapter 11) has
 * four sets, PWM0 and PWM1 driven by IEP0, PWM2 and PWM3 by IEP1. Each set
 * has a trip register and three state registers, one per pos/neg pair.
 */
#define ICSSG_CFG          0x00026000u
#define ICSSG_CFG_IEPCLK   (ICSSG_CFG + 0x30u)
#define ICSSG_IEPCLK_SYNC  (1u << 0)
#define ICSSG_CFG_PWM(m)   (ICSSG_CFG + 0x130u + 4u * (m))
#define ICSSG_CFG_PWM_STATE(m, k) (ICSSG_CFG + 0x140u + 0xcu * (m) + 4u * (k))

/* ICSSG_PWMm: trip configuration */
#define ICSSG_PWM_TRIP_MASK(v)  ((uint32_t)(v) << 8) /* 9 bits, 1 = ignore that trip */
#define ICSSG_PWM_TRIP_MASK_ALL ICSSG_PWM_TRIP_MASK(0x1ffu)
#define ICSSG_PWM_TRIP_CMP0_EN  (1u << 17)
#define ICSSG_PWM_TRIP_RESET    (1u << 18)

/*
 * ICSSG_PWMm_k: what pos (A) and neg (B) drive in each state.
 * Initial and trip: 0 Z, 1 low, 2 high. Active: 0 toggle, 1 low, 2 high.
 */
#define ICSSG_PWM_Z      0u
#define ICSSG_PWM_LOW    1u
#define ICSSG_PWM_HIGH   2u
#define ICSSG_PWM_TOGGLE 0u
#define ICSSG_PWM_STATE(pos_init, neg_init, pos_trip, neg_trip, pos_act, neg_act) \
	((uint32_t)(pos_init) | (uint32_t)(neg_init) << 2 | (uint32_t)(pos_trip) << 4 | \
	 (uint32_t)(neg_trip) << 6 | (uint32_t)(pos_act) << 8 | (uint32_t)(neg_act) << 10)

/*
 * IEP0 and IEP1. In sync mode the counter ticks with the 250 MHz core
 * clock; with an increment of 4 it counts nanoseconds. A compare hit sets
 * its bit in CMP_STATUS until software writes the bit back. CMP0 can reset
 * the counter (CMP_CFG bit 0); CMP_CFG bit n + 1 enables CMPn.
 */
#define ICSSG_IEP0 0x0002E000u
#define ICSSG_IEP1 0x0002F000u
#define IEP_GLOBAL_CFG(iep) ((iep) + 0x00u)
#define IEP_COUNT_LO(iep)   ((iep) + 0x10u)
#define IEP_COUNT_HI(iep)   ((iep) + 0x14u)
#define IEP_CMP_CFG(iep)    ((iep) + 0x70u)
#define IEP_CMP_STATUS(iep) ((iep) + 0x74u)
#define IEP_CMP_LO(iep, n)  ((iep) + ((n) < 8u ? 0x78u + 8u * (n) : 0xc0u + 8u * ((n) - 8u)))
#define IEP_CMP_HI(iep, n)  (IEP_CMP_LO(iep, n) + 4u)

#define IEP_CNT_ENABLE        (1u << 0)
#define IEP_DEFAULT_INC(i)    ((uint32_t)(i) << 4)
#define IEP_CMP_INC(i)        ((uint32_t)(i) << 8)
#define IEP_CMP0_RST_CNT_EN   (1u << 0)
#define IEP_CMP_EN(n)         (1u << ((n) + 1u))

#define ICSSG_CORE_HZ 250000000u
#define ICSSG_NS_PER_CYCLE 4u

/*
 * Task managers, one per core, at TM_CFG + 0x100 * the core's index. In
 * general purpose mode an event, such as an IEP compare, preempts the core
 * into a task at the PC in TS2_PC_S0; XIN 252 returns from it.
 */
#define ICSSG_TM_PRU0   0x0002A000u
#define ICSSG_TM_RTU0   0x0002A100u
#define ICSSG_TM_PRU1   0x0002A200u
#define ICSSG_TM_RTU1   0x0002A300u
#define ICSSG_TM_TXPRU0 0x0002A400u
#define ICSSG_TM_TXPRU1 0x0002A500u
#define TM_GLOBAL_CFG(tm)  ((tm) + 0x00u)
#define TM_TS2_PC_S0(tm)   ((tm) + 0x1cu)
#define TM_TS2_GEN_CFG1(tm) ((tm) + 0x40u)
/* Task manager event numbers (SPRUJ61 table 7-13) */
#define TM_EVENT_IEP0_CMP(n) (16u + (n))
#define TM_EVENT_IEP1_CMP(n) (40u + (n))

#endif
