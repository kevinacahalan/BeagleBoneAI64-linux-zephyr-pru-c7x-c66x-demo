/* SPDX-License-Identifier: MIT */
/*
 * The PWM project's memory map: how each ICSSG0 slice splits its 8 KiB of
 * data RAM between its three cores, and where the slice's IPC records sit.
 * Every core sees its own slice's data RAM at local 0x0000, so one map
 * serves both slices. The C code (pwm_ipc.h) and every core's linker.cmd
 * include this file, so it holds #defines only.
 *
 *   0x0000 - 0x07ff  PRU     2 KiB  stack, data, trace
 *   0x0800 - 0x0bff  Tx_PRU  1 KiB
 *   0x0c00 - 0x1eff  RTU     4.75 KiB, RPMsg and the fabric node included
 *   0x1f00 - 0x1fff  IPC     the slice's four records (pwm_ipc.h)
 *
 * A slice never touches the other slice's data RAM. Slice 1 keeps the
 * pipeline's windows of P8.31 samples in ICSSG0's shared RAM at 0x10000
 * (zephyr/include/ai64_pipe.h); the rest of its 64 KiB is free.
 */
#ifndef PWM_MAP_H_
#define PWM_MAP_H_

#define PWM_PRU_DRAM        0x00000000
#define PWM_PRU_DRAM_SIZE   0x00000800
#define PWM_TXPRU_DRAM      0x00000800
#define PWM_TXPRU_DRAM_SIZE 0x00000400
#define PWM_RTU_DRAM        0x00000c00
#define PWM_RTU_DRAM_SIZE   0x00001300
#define PWM_IPC_DRAM        0x00001f00
#define PWM_IPC_DRAM_SIZE   0x00000100

/* Instruction RAM: 12 KiB on a PRU, 8 KiB on an RTU, 6 KiB on a Tx_PRU. */
#define PWM_PRU_IRAM_SIZE   0x00003000
#define PWM_RTU_IRAM_SIZE   0x00002000
#define PWM_TXPRU_IRAM_SIZE 0x00001800

/* The records, one writer each */
#define PWM_IPC_CTL   (PWM_IPC_DRAM + 0x00) /* RTU writes, Tx_PRU reads */
#define PWM_IPC_NEXT  (PWM_IPC_DRAM + 0x40) /* Tx_PRU writes, PRU reads */
#define PWM_IPC_OUT   (PWM_IPC_DRAM + 0x80) /* PRU writes, RTU and Tx_PRU read */
#define PWM_IPC_MOD   (PWM_IPC_DRAM + 0xc0) /* Tx_PRU writes, RTU reads */

#endif
