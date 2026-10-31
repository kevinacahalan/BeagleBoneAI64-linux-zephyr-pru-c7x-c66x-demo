# The ICSSG cores

How the twelve ICSSG cores' firmware is laid out, and what each slice does. [`pwm/README.md`](pwm/README.md) and [`multicore_scheduler/README.md`](multicore_scheduler/README.md) go further into each project.

## The ICSSG firmware

The J721E has two ICSSGs with two slices each, and every slice has a PRU, an RTU, and a Tx_PRU: twelve cores. The firmware is two projects, laid out the way TI's [open-pru](https://github.com/TexasInstruments/open-pru) projects are:

```
pru/
  Makefile             builds both projects
  pru_rules.mak        the rules every core's makefile includes
  common/              headers both projects use: trace, RPMsg, text, ICSSG registers
  pwm/                 ICSSG0
    README.md
    firmware/          pwm_map.h, pwm_ipc.h, and the RTU and Tx_PRU sources
      beaglebone-ai64/icss_g0_<core>_fw/
        main.c         a PRU's own source
        ti-pru-cgt/    makefile, linker.cmd
  multicore_scheduler/ ICSSG1, the same way
```

Each core's makefile names its image, its sources, and its defines, and includes `pru_rules.mak`, as open-pru's do. Each core's `linker.cmd` includes its project's memory map, so the C code and the linker share one set of addresses. After each link the build prints how full each image's RAM is, and fails a region past 95%. Every core builds as PRU silicon version 4, the ICSSG.

| Core | remoteproc | Image | Project, and what it runs |
| --- | --- | --- | --- |
| ICSSG0 RTU0 | `b004000.rtu` | `ai64-icssg0-rtu0.elf` | pwm, slice 0: talks, over RPMsg and the fabric (node 8, `g0rtu0`) |
| ICSSG0 Tx_PRU0 | `b00a000.txpru` | `ai64-icssg0-txpru0.elf` | pwm, slice 0: the modulator, and eCAP0 on P8.15 |
| ICSSG0 PRU0 | `b034000.pru` | `ai64-icssg0-pru0.elf` | pwm, slice 0: PWM in software |
| ICSSG0 RTU1 | `b006000.rtu` | `ai64-icssg0-rtu1.elf` | pwm, slice 1: talks, over the fabric (node 9, `g0rtu1`) |
| ICSSG0 Tx_PRU1 | `b00c000.txpru` | `ai64-icssg0-txpru1.elf` | pwm, slice 1: the modulator |
| ICSSG0 PRU1 | `b038000.pru` | `ai64-icssg0-pru1.elf` | pwm, slice 1: the ICSSG PWM module |
| ICSSG1, all six | `b134000.pru`, `b138000.pru`, `b104000.rtu`, `b106000.rtu`, `b10a000.txpru`, `b10c000.txpru` | `ai64-icssg1-<core><slice>.elf` | multicore_scheduler |

In ICSSG0, **the RTU talks, and the PRU and Tx_PRU keep time**:

- **The RTU does everything slow or unpredictable.** It reads and writes DDR, rings mailboxes, takes the hardware spinlock, runs RPMsg, and reads the GTC. How long any of that takes depends on what the rest of the chip is doing.
- **The PRU and Tx_PRU only touch their slice's own data RAM** and their ICSSG's own peripherals, at a fixed few cycles an access. They talk to their RTU through records there, each written by one core and read by the others, and never wait on it. So nothing outside the ICSSG can stretch an edge. The one exception is the Tx_PRU's read of the GTC once a period, to keep the steps on SoC time; it has the rest of the period to write the next period's record, so a slow read never reaches an edge.
- **The trace log is the exception.** Every core keeps a remoteproc trace buffer in its own data RAM, which Linux reads (`ai64 trace <core>`); writing a line is a few local stores. `TRACE=0 ./beagle-linux/scripts/build.sh pru` builds the trace calls out of the PRUs and Tx_PRUs. The RTUs always keep theirs.
- **One RTU talks over RPMsg and one over shared memory only**, so the template has a working example of each. RPMsg needs a vring interrupt in the overlay and a few KiB of data RAM. Both RTUs are fabric nodes, and both publish what they know in their fabric page.

ICSSG1 keeps TI's design instead: every core runs a task, and nothing on it talks. `ai64 sched` reads its registers, and only reads them.

## ICSSG0: PWM, three ways

Both slices make the same waveform, from the same request, with the same modulator. Only the PRU differs. A third way copies slice 0's A, with the ICSSG's eCAP. [`pru/pwm/README.md`](pwm/README.md) has the details: the memory map, the four IPC records, and how each core uses them.

| | Slice 0: in software | Slice 1: the ICSSG PWM module |
| --- | --- | --- |
| Pins | A on P8.11, B on P8.12, R30 bits 17 and 16 | A on P9.12 (`PRG0_PWM2_A0`), B on P9.15 (`PRG0_PWM2_B0`) |
| What makes an edge | PRU0 writes R30 when IEP0, polled, reaches the edge's time | IEP1 CMP1 toggles A, CMP2 toggles B; CMP0 ends the period |
| What the PRU does | waits for every edge | after each compare, rearms it for that pin's next edge |
| Timing | within a few tens of nanoseconds; PRU0 counts the edges it was more than 100 ns late for | exact, on the 250 MHz IEP clock |

The waveform is A high for `D` from 40 ns into a period of `P`, and B high from `D + d` after that until `d` before the period ends: B is A's complement with dead band `d` on each side ([drawn here](../README.md#probe-the-pins)). Both start with a 1 µs dead band, stepping through 10, 15, and 20 kHz, and at each through 10, 25, 50, 75, and 90% duty, 5 ms a step on the GTC's time, so they change step with each other and with P9.25. The Tx_PRU of each slice makes the steps, sweeps the duty up and down every `sweep_ms` instead, or passes a fixed duty through.

The third way is ICSSG0's eCAP0 in APWM mode, on P8.15. It is a PWM generator in its own right: a 32-bit counter on the 250 MHz core clock that counts to a period and holds its pin high below a compare value, with shadow registers that load at a period's end. Slice 0's Tx_PRU gives it the period and high time it gives PRU0, during the eCAP period whose end is nearest the time A changes, and does nothing else with it. It has one output, so no B and no dead band, and its counter is not in phase with IEP0. [`pru/pwm/README.md`](pwm/README.md#the-cores) has the details.

Two things about the J721E's PWM module took finding out on the board. An IEP compare only fires again once software clears its status bit (AM64x's ICSSG has `PWM_EFC_EN` to do that, and J721E's does not), and in the active state an output only toggles: its active level does nothing, and CMP0 does not bring it back. So each pin has one compare, which PRU1 moves between that pin's rise and its fall as soon as it fires; TI's AM65x PWM firmware works the same way. And the set's trip inputs must be masked: unmasked and unconnected, they keep the set in its trip state, and its pins at Z.

Slice 1's PRU1 also samples P8.31 for [the pipeline](../docs/pipeline.md#the-pipeline-p831-to-p925), and its RTU1 stamps the windows: the shift register times the samples, so the PWM loses nothing to it. Neither slice counted a late edge with the pipeline running either.

`ai64 pwm` shows both slices running about 15,000 periods a second while they step (10, 15, and 20 kHz, a third of the time each), 20,000 at their fixed default of 20 kHz after `steps 0`, and 50,000 after `fabric route ... g0rtu1 pwm 20000 5000`. With both RTUs and the whole fabric running, the software slice's latest edge was 24 ns late, and neither slice counted a late edge in 25 restarts of the demo.

## ICSSG1: the multicore scheduler

[`pru/multicore_scheduler/`](multicore_scheduler/README.md) is TI's open-pru example `multicore_scheduler`, ported from ICSSG0 of the AM243x LaunchPad to ICSSG1 here. IEP0's CMP0 resets the counter every 100 ns, and on each 4 ns step after it one more compare fires and starts one core's task through that core's task manager: PRU0, PRU1, RTU1, RTU0, Tx_PRU1, Tx_PRU0. Each task sets PWM set 2's initial state high or low; PWM2 runs on IEP1, which nothing starts, so P8.17 (`PRG1_PWM2_A0`) follows it.

The port keeps TI's files, names, and timing. What J721E needed: no clock or pin setup in PRU0, since Linux has done both; and, with no `PWM_EFC_EN`, the compare flags cleared in software. Each task clears its own, each core clears its own once its task manager is on, and PRU0 maps every task's event before it starts IEP0, and clears all six flags once more two periods later. Without those, a compare that fires before its core's task manager sees it leaves a flag nothing clears, and that core's task never runs again. `MCS_STEP_NS` and `MCS_PERIOD_NS`, on the make command line, change the schedule.

`ai64 sched` reads PWM2_0's state over the bus, much slower than the tasks change it, and finds P8.17 high about 79% of the time, against 88% for the schedule. A logic analyzer at 500 MS/s or more shows the schedule itself: a 100 ns period, low from 8 to 12 ns and from 16 to 24 ns, high the rest of the time ([Probe the pins](../README.md#probe-the-pins)). With `MCS_STEP_NS=1000 MCS_PERIOD_NS=10000`, any analyzer does.

## Linux to RTU0: RPMsg text

- ICSSG0 RTU0 announces RPMsg channel `rpmsg-raw` on port 30.
- Linux writes plain text: `pwm <period_ns> <duty_ns>`, `steps <ms>`, `sweep <ms>`, `dead <ns>`, or `status`.
- RTU0 replies with one sentence that names who asked and what it did.
- RTU0 learns Linux's address from the first message it receives, so Linux sends `ping` first. RTU0 can't report anything that a fabric route asks it to do until Linux has sent that first message.
- The code is `pru/common/ai64_rtu_rpmsg.h` and the resource table in `pru/common/ai64_rsc_rpmsg.h`.
