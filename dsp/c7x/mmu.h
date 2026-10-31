/* SPDX-License-Identifier: MIT */
#ifndef DSP_C7X_MMU_H_
#define DSP_C7X_MMU_H_

#include <stdint.h>

/* Map the image's regions, size the caches, and turn the MMU on. */
void ai64_c7x_mmu_init(void);

/* Whole-cache maintenance (regs.asm). */
void ai64_l1d_wbinv_all(void);
void ai64_l1d_wb_all(void);
void ai64_l2_wbinv_all(void);
uint64_t ai64_get_scr(void);
uint64_t ai64_get_l1dcfg(void);
uint64_t ai64_get_l2cfg(void);

#endif
