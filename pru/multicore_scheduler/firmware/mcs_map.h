/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2018-2025 Texas Instruments Incorporated - http://www.ti.com/
 *
 * How each ICSSG1 slice's data RAM is split, as in open-pru's linker
 * files for this example: the slice's first 4 KiB for its PRU, then 2 KiB
 * each for its RTU and Tx_PRU. Every core sees its own slice's data RAM at
 * local 0x0000. Each core's linker.cmd includes this file.
 */
#ifndef MCS_MAP_H_
#define MCS_MAP_H_

#define MCS_PRU_DRAM        0x00000000
#define MCS_PRU_DRAM_SIZE   0x00001000
#define MCS_RTU_DRAM        0x00001000
#define MCS_RTU_DRAM_SIZE   0x00000800
#define MCS_TXPRU_DRAM      0x00001800
#define MCS_TXPRU_DRAM_SIZE 0x00000800

#define MCS_PRU_IRAM_SIZE   0x00003000
#define MCS_RTU_IRAM_SIZE   0x00002000
#define MCS_TXPRU_IRAM_SIZE 0x00001800

#endif
