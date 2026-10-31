; SPDX-License-Identifier: MIT
;
; C7x control registers the C compiler has no names for: the MMU, the
; memory attributes, and the cache configuration and maintenance registers.
; ai64_c7x_mmu.c and main.c call these. The core comes out of reset in
; secure supervisor mode. Each MMU register is written in both its secure
; (_S) and its plain copy; see ai64_c7x_mmu.c.
;
; C calling convention: the first argument is in a4, the result in a4.

        .global ai64_set_mair
        .sect ".text:ai64_regs"
ai64_set_mair:
        mvc.s1   a4, MAR_S
        mvc.s1   a4, MAR
||      ret.b1

        .global ai64_set_tcr
ai64_set_tcr:
        mvc.s1   a4, TCR0_S
        mvc.s1   a4, TCR0
||      ret.b1

        .global ai64_set_tbr
ai64_set_tbr:
        mvc.s1   a4, TBR0_S
        mvc.s1   a4, TBR0
||      ret.b1

; SCR |= a4: bit 0 turns the MMU on.
        .global ai64_scr_or
ai64_scr_or:
        mvc.s1   SCR, a3
        nop
        ord.l1   a4, a3, a3
        mvc.s1   a3, SCR
        mvc.s1   a3, SCR_S
||      ret.b1

        .global ai64_get_scr
ai64_get_scr:
        mvc.s1   SCR, a4
||      ret.b1

; Cache sizes. Reading the register back stalls until the change is done.
        .global ai64_set_l1dcfg
ai64_set_l1dcfg:
        mvc.s1   a4, L1DCFG
        mvc.s1   L1DCFG, a4
||      ret.b1

        .global ai64_set_l2cfg
ai64_set_l2cfg:
        mvc.s1   a4, L2CFG
        mvc.s1   L2CFG, a4
||      ret.b1

        .global ai64_get_l1dcfg
ai64_get_l1dcfg:
        mvc.s1   L1DCFG, a4
||      ret.b1

        .global ai64_get_l2cfg
ai64_get_l2cfg:
        mvc.s1   L2CFG, a4
||      ret.b1

; Whole-cache maintenance.
        .global ai64_l1d_wbinv_all
ai64_l1d_wbinv_all:
        mvk64.l1 1, a5
        mvc.s1   a5, L1DWBINV
||      ret.b1

        .global ai64_l1d_wb_all
ai64_l1d_wb_all:
        mvk64.l1 1, a5
        mvc.s1   a5, L1DWB
||      ret.b1

        .global ai64_l2_wbinv_all
ai64_l2_wbinv_all:
        mvk64.l1 1, a5
        mvc.s1   a5, L2WBINV
||      ret.b1
