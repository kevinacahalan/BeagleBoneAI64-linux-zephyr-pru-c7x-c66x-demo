# Multicore Scheduler

## Introduction

A port of TI's open-pru example [`examples/multicore_scheduler`](https://github.com/TexasInstruments/open-pru/tree/main/examples/multicore_scheduler) from ICSSG0 of the AM243x LaunchPad to ICSSG1 of the BeagleBone AI-64 (J721E). It uses the IEP timer and the task manager of all six cores to run one task on each, back to back, with no delay in between.

The source files keep TI's BSD-3-Clause headers, and the original's layout and names: one directory per core, `firmware/beaglebone-ai64/icss_g1_<core>_fw/`, each with its `main.c`, its `pwm_toggle.asm`, and a `ti-pru-cgt/` directory for its makefile and linker script.

## Overview

IEP0's CMP0 resets the counter every period. Each core's task manager starts that core's task on one of the other compares, which hit on consecutive steps of the period, in this order:

| Step | Compare | Core | Its task sets P8.17 |
| --- | --- | --- | --- |
| 1 | CMP7 | PRU0 | high |
| 2 | CMP2 | PRU1 | low |
| 3 | CMP3 | RTU1 | high |
| 4 | CMP4 | RTU0 | low |
| 5 | CMP5 | Tx_PRU1 | low |
| 6 | CMP6 | Tx_PRU0 | high |

The RTUs' and Tx_PRUs' R30 reach no pad, so the tasks drive the pin through something every core can write: the initial state of PWM set 2's first pair (`ICSSG_PWM2_0`). PWM2 runs on IEP1, which nothing starts, so the set stays in its initial state and the pin, `PRG1_PWM2_A0` on P8.17, follows what the tasks write. As in the original, this has nothing to do with PWM itself.

PRU0 does the setup, in `FN_MCS_CFG` (`icss_g1_pru0_fw/pwm_toggle.asm`): it maps every core's task manager to its compare, programs IEP0, and starts it. Every other core arms its own task manager with its task's address, and then idles. TI's README has the [overview](https://github.com/TexasInstruments/open-pru/blob/main/examples/multicore_scheduler/pictures/multicore_scheduling_overview.png) and [timing](https://github.com/TexasInstruments/open-pru/blob/main/examples/multicore_scheduler/pictures/multicore_scheduling_timing_diagram.png) diagrams.

## What changed for J721E

- **No clock or pin setup in PRU0.** On AM243x, PRU0 sets the ICSSG core clock to 250 MHz through the PLL and muxes the pin. Here Linux already runs ICSSG1 at 250 MHz, and the overlay muxes P8.17. IEP0 still runs in sync mode, on the core clock.
- **The compare flags are cleared in software.** AM243x has `PWM_EFC_EN` (`ICSSG_SA_MX_REG` bit 16), which clears an IEP compare's status flag in hardware. J721E does not: the flag stays set, and the task manager sees no new event until it is clear. So:
  - every task clears its own compare's flag (r25 holds the bit), one store more than the original's task;
  - every core clears it once more right after `tsen 1`, for a compare that hit before its task manager was on;
  - `FN_MCS_CFG` maps the task managers' events before it starts IEP0, not after. A compare that hits before its core's task manager has its event would set a flag that no task ever clears, and that task would never run.
  With those, the six cores can start in any order.
- **The schedule is a build option.** `MCS_STEP_NS` is the time between steps, and `MCS_PERIOD_NS` the period: `make -C pru MCS_STEP_NS=1000 MCS_PERIOD_NS=10000 ...` for a schedule slow enough for any logic analyzer. The default is the original's: steps of 4 ns, one core clock, and a period of 100 ns.
- **Each core logs one line to its remoteproc trace** before its task manager takes over, so `ai64 trace b134000.pru` (and the other five) shows what it set up.

## Data RAM

Each slice's data RAM is split the way open-pru's linker files for this example split it: the slice's first 4 KiB for its PRU, then 2 KiB each for its RTU and Tx_PRU (`firmware/mcs_map.h`, which each `linker.cmd` includes).

## Build

From the repo root, `./beagle-linux/scripts/build.sh pru` builds this project and the PWM project into `pru/build/ai64-icssg1-<core><slice>.elf`.

## Run

`ai64-demo` starts the six cores, PRU0 last. Then `sudo ai64 sched`, the `sched` window, reads ICSSG1's registers (only reads them: ICSSG1 has no core left to talk for it) and prints the schedule, each task manager's state, and how often it finds P8.17 high. On a logic analyzer, the default schedule makes P8.17 low from 8 to 12 ns and from 16 to 24 ns of every 100 ns period, and high the rest of the time; PRU0's step at 4 ns sets it high when it already is.
