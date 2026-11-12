# Pinmux

This demo muxes header pins from two places, and RPMsg, the fabric, and ICSSG RAM use no pin:

- **The Linux overlay** sets the ICSSG pins (below). Each pad hangs on the ICSSG core that drives it, reads it, or runs its PWM set, so `pru_rproc` applies it when the core probes: P8.31, the pipeline's input, goes with PRU1, in mode 1 (`PRG0_PRU1_GPI0`), and its partner ball is held off. The overlay also carries RTU0's vring interrupt.
- **Each R5F core** muxes its own pins from its `zephyr/r5/<core>/app.overlay`: the SPI7 pins on R5F0_0, P9.25's EHRPWM4_B on R5F0_1, and P8.18 (as GPIO2_4) on R5F1_0; the UART2 console pins come from the upstream board file, on R5F0_0. R5F1_1 uses no pins. A core can write those pad registers only because Linux has already unlocked them.

Most BeagleBone AI-64 header pins are wired to two SoC balls, an "a" ball and a "b" ball. The ball the demo does not use is held off (mux mode 7, transmit disabled) so it cannot fight the one it does. Every ICSSG pin the demo uses has one ball, but P8.31.

The Linux overlay is `beagle-linux/overlays/k3-j721e-beagleboneai64-ai64-demo.dtso`. It writes each pad with `J721E_IOPAD()` from BeagleBoard-DeviceTrees `ti/k3-pinctrl.h` (commit `550f7711a4696d5f5e92505289379b754e871313`). This repository does not contain a copy of that header; `./beagle-linux/scripts/build.sh linux` clones the tree and preprocesses the overlay the way that tree's Makefile does (`cpp -nostdinc -I src/arm64`, then `dtc`).

A source line looks like:

```
J721E_IOPAD(0x0b8, PIN_OUTPUT, 3) /* (AE27) P9.12 PRG0_PWM2_A0 */
```

`J721E_IOPAD()` keeps the low 12 bits of the pad address (`0x0011c0b8`). `PIN_OUTPUT` compiles to `0x10000`, `PIN_INPUT` to `0x54000`, and `PIN_DISABLE` to `0x200000` (the transmit-disable bit). The mux mode is added to that cell. The Zephyr pins use `K3_PINMUX()` with the same pad offsets, and `0x200000 | mode 7` for a held-off ball. The modes are TI's, from the TDA4VM datasheet's pin attributes.

## The pins

| Header | Ball | Pad | Mux | Core | Signal |
| --- | --- | --- | --- | --- | --- |
| P8.11 | AB24 | `0x0f4` | 0, `PRG0_PRU0_GPO17` | ICSSG0 PRU0 | software PWM, output A |
| P8.12 | AH28 | `0x0f0` | 0, `PRG0_PRU0_GPO16` | ICSSG0 PRU0 | software PWM, output B |
| P8.15 | AB29 | `0x0f8` | 4, `PRG0_ECAP0_IN_APWM_OUT` | ICSSG0 eCAP0, run by Tx_PRU0 | eCAP PWM, a copy of output A |
| P9.12 | AE27 | `0x0b8` | 3, `PRG0_PWM2_A0` | ICSSG0 PWM module, run by PRU1 | ICSSG PWM, output A |
| P9.15 | AD25 | `0x0c0` | 3, `PRG0_PWM2_B0` | ICSSG0 PWM module, run by PRU1 | ICSSG PWM, output B |
| P8.31 | AE29 | `0x100` | 1, `PRG0_PRU1_GPI0` | ICSSG0 PRU1, its GPI shift register | the pipeline's input, jumpered from P9.15 |
| P8.31 | AJ25 | `0x084` | 7, held off | — | shorted to AE29 |
| P8.03 | AH21 | `0x054` | 1, `PRG1_PRU0_GPI19`, pull-down | ICSSG1 PRU0 | logic analyzer ch0 |
| P8.05 | AH25 | `0x088` | 1, `PRG1_PRU1_GPI12`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.06 | AG25 | `0x08c` | 1, `PRG1_PRU1_GPI13`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.07 | AD24 | `0x03c` | 1, `PRG1_PRU0_GPI14`, pull-down | ICSSG1 PRU0 | logic analyzer ch1 |
| P8.08 | AG24 | `0x038` | 1, `PRG1_PRU0_GPI13`, pull-down | ICSSG1 PRU0 | logic analyzer ch2 |
| P8.09 | AE24 | `0x044` | 1, `PRG1_PRU0_GPI16`, pull-down | ICSSG1 PRU0 | logic analyzer ch3 |
| P8.10 | AC24 | `0x040` | 1, `PRG1_PRU0_GPI15`, pull-down | ICSSG1 PRU0 | logic analyzer ch4 |
| P8.21 | AF21 | `0x07c` | 1, `PRG1_PRU1_GPI9`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.23 | AB23 | `0x080` | 1, `PRG1_PRU1_GPI10`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.25 | AH26 | `0x090` | 1, `PRG1_PRU1_GPI14`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.32 | AG21 | `0x06c` | 1, `PRG1_PRU1_GPI5`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.32 | AD28 | `0x104` | 7, held off | — | shorted to AG21 |
| P8.33 | AH24 | `0x068` | 1, `PRG1_PRU1_GPI4`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.33 | AA2 | `0x1c0` | 7, held off | — | shorted to AH24 |
| P8.35 | AD23 | `0x064` | 1, `PRG1_PRU1_GPI3`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P8.35 | Y3 | `0x1d4` | 7, held off | — | shorted to AD23 |
| P8.36 | AE20 | `0x020` | 1, `PRG1_PRU0_GPI7`, pull-down | ICSSG1 PRU0 | logic analyzer ch5 |
| P8.38 | AJ20 | `0x024` | 1, `PRG1_PRU0_GPI8`, pull-down | ICSSG1 PRU0 | logic analyzer ch6 |
| P8.38 | Y29 | `0x1a8` | 7, held off | — | shorted to AJ20 |
| P9.11 | AC23 | `0x004` | 1, `PRG1_PRU0_GPI0`, pull-down | ICSSG1 PRU0 | logic analyzer ch7 |
| P9.13 | AG22 | `0x008` | 1, `PRG1_PRU0_GPI1`, pull-down | ICSSG1 PRU0 | logic analyzer ch8 |
| P9.21 | AJ22 | `0x0a0` | 1, `PRG1_PRU1_GPI18`, pull-down | ICSSG1 PRU1 | an input nothing samples: PRU1 is free |
| P9.21 | U28 | `0x16c` | 7, held off | — | shorted to AJ22 |
| P9.23 | AG20 | `0x028` | 1, `PRG1_PRU0_GPI9`, pull-down | ICSSG1 PRU0 | logic analyzer ch9 |
| P8.17 | AF22 | `0x00c` | 3, `PRG1_PWM2_A0` | the six ICSSG1 cores, with `--icssg1 scheduler` | the multicore scheduler (extra-demos/) |
| P9.25 | W26 | `0x1a4` | 6, `EHRPWM4_B` | Zephyr on R5F0_1 | EHRPWM4 output B |
| P9.25 | AC4 | `0x200` | 7, held off | — | shorted to W26 |
| P9.28 | U2 | `0x230` | 6, `SPI7_CS0` | Zephyr on R5F0_0 | SPI7 chip select |
| P9.28 | AF28 | `0x0b0` | 7, held off | — | shorted to U2 |
| P9.30 | V6 | `0x238` | 6, `SPI7_D0` | Zephyr on R5F0_0 | SPI7 MOSI |
| P9.30 | AE28 | `0x0b4` | 7, held off | — | shorted to V6 |
| P9.31 | U3 | `0x234` | 6, `SPI7_CLK` | Zephyr on R5F0_0 | SPI7 clock, 1 MHz |
| P9.31 | AB26 | `0x0d4` | 7, held off | — | shorted to U3 |
| P8.34 | AD22 | `0x01c` | 14, `UART2_TXD` | Zephyr on R5F0_0 | Zephyr console, 115200 8N1 |
| P8.22 | AH23 | `0x014` | 14, `UART2_RXD` | Zephyr | Zephyr console |
| P8.18 | AJ23 | `0x010` | 7, `GPIO0_4`, here GPIO2_4 | Zephyr on R5F1_0 | 1 kHz square wave |

All signals are 3.3 V.

## ICSSG0 slice 0: P8.11 and P8.12

PRU0 drives R30 bits 17 and 16. Mux mode 0 on balls AB24 and AH28 is `PRG0_PRU0_GPO17` and `PRG0_PRU0_GPO16`, which connect those bits to the pads. The overlay hangs the pinctrl on the `&pru0_0` node, so `pru_rproc` applies it when the core probes. ICSSG0's `GPCFG0` register stays 0, direct GPO mode.

## ICSSG0 eCAP0: P8.15

Mux mode 4 on ball AB29 is `PRG0_ECAP0_IN_APWM_OUT`, the pin of ICSSG0's eCAP: its capture input, or in APWM mode its PWM output. Tx_PRU0 runs the eCAP, so the overlay hangs the pinctrl on `&tx_pru0_0`. The eCAP stays in capture mode, not driving the pin, until the first request. ICSSG1's eCAP has a header pin too, P9.18 (ball AH22, mode 6, with Y2 held off), which the demo does not use.

## ICSSG0 slice 1: P9.12 and P9.15

Mux mode 3 on these balls is the ICSSG's PWM module: `PRG0_PWM2_A0` and `PRG0_PWM2_B0` are the positive and negative outputs of set 2's first pair, which IEP1's CMP1 and CMP2 toggle. The pinctrl hangs on `&pru0_1`, the PRU that runs that set. A PWM pad is in tri-state with its pull-down on until the module drives it: the set's state registers start at Z, and so do they in the trip state, which an unmasked, unconnected trip input holds it in. PRU1 sets both pins low before the first period, and masks the trips.

## ICSSG0 slice 1: P8.31, the pipeline's input

Mux mode 1 on ball AE29 is `PRG0_PRU1_GPI0`, input 0 of PRU1's R31, the one its GPI shift register samples in 28-bit shift mode ([the pipeline](docs/pipeline.md#the-pipeline-p831-to-p925)). PRU1 drives no pin of its own, since the PWM module drives P9.12 and P9.15, so its inputs are free. P8.31's other ball, AJ25 (`PRG1_PRU1_GPO11`, pad `0x084`), is held off. The pinctrl goes with PRU1's other pins, on `&pru0_1`. A jumper wire from P9.15 brings it slice 1's output B; without one the pin floats, and the pipeline finds nothing to train on.

## ICSSG1: the logic analyzer's 10 inputs

Mode 1 on each of these balls is `PRG1_PRU<n>_GPI<bit>`, an input of PRU0's or PRU1's R31, read in direct mode. Each pad has its receiver and its pull-down on (`PIN_INPUT_PULLDOWN`), so a pin with nothing on it reads low. PRU0's ten go on `&pru1_0`, and the analyzer samples them; PRU1's nine go on `&pru1_1` and stay inputs, but nothing samples them, since PRU1 is free. `pru_rproc` applies them when the cores probe; ICSSG1 drives none of them. P8.32, P8.33, P8.35, P8.38, and P9.21 each have a second ball, held off. [The analyzer's README](pru/logic_analyzer/README.md#the-channels) has the jumpers that bring the demo's signals to them, and why the other ICSSG1 inputs on the header are not among them.

## ICSSG1: P8.17, with the multicore scheduler

`PRG1_PWM2_A0`, mode 3, is the positive output of ICSSG1's PWM set 2. IEP1 never runs on ICSSG1, so the set stays in its initial state, and the pin follows the initial state the six cores' tasks write: that is how the RTUs and Tx_PRUs, whose R30 reaches no pad, drive a pin. The pinctrl hangs on `&pru1_0`, the core that sets the scheduler up, next to the analyzer's PRU0 pins: the overlay serves either firmware set.

## Zephyr: P9.25, SPI7, UART2, and P8.18

Each Zephyr image muxes its own pins from its devicetree, the way the PocketBeagle 2 demo's M4 does. The R5F can write the pad registers because Linux has already unlocked them. Linux does not touch these pads: `main_spi7`, `main_uart2`, and `main_ehrpwm4` stay disabled in the Linux device tree.

- **P9.25** is EHRPWM4 output B, mode 6 on W26, with AC4 held off: `zephyr/r5/r5f0_1/app.overlay`. The stock image's boot entry selects `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`, which gives the same ball and EHRPWM4 to Linux. `install-beagle.sh` removes that overlay from the default label and adds this demo's; the original `extlinux.conf` is saved as `extlinux.conf.ai64-bak`.
- **SPI7**: `zephyr/r5/r5f0_0/app.overlay`. Its functional clock is 50 MHz. D0 is MOSI (`ti,d1-miso-d0-mosi`). MISO (P9.29) is not muxed. The board firmware leaves MCSPI7 powered (`k3conf dump device` shows `DEVICE_STATE_ON`).
- **UART2**: the upstream Zephyr board file for R5F0_0. Any 3.3 V USB-serial adapter reads the console there.
- **P8.18** is ball AJ23 in mode 7, GPIO, with bit 4 of the pad set: bits 5:4 pick which of the main domain's GPIO0, GPIO2, GPIO4, and GPIO6 drives a pad, and 1 is GPIO2. Linux owns GPIO0 and leaves GPIO2 disabled, so R5F1_0 takes GPIO2 whole: `zephyr/r5/r5f1_0/app.overlay`. The pad reads `0x10017`.

What each pin should show, and how B relates to A on the ICSSG0 pairs, is in the README's [Probe the pins](README.md#probe-the-pins).

## Check on the board

With the demo running, the pad registers at `0x0011c000 + pad` read:

| Pad | Value | Meaning |
| --- | --- | --- |
| `0x0f4`, `0x0f0` | `0x10000` | output, mode 0 |
| `0x0f8` | `0x10004` | output, mode 4 |
| `0x0b8`, `0x0c0`, `0x00c` | `0x10003` | output, mode 3 |
| `0x100` | `0x54001` | input, mode 1 |
| `0x1a4` | `0x10006` | output, mode 6 |
| `0x200`, `0x0b0`, `0x0b4`, `0x0d4`, `0x084` | `0x200007` | held off |
| `0x230`, `0x234`, `0x238` | `0x10006` | output, mode 6 |
| `0x01c` | `0x1000e` | output, mode 14 |
| `0x014` | `0x5000e` | input, mode 14 |
| `0x010` | `0x10017` | output, mode 7, GPIO2 |

`sudo ai64 pads` reads them and marks any that differ. A Zephyr pad only takes its value once its core has booted: SPI7 and UART2 with R5F0_0, P9.25 with R5F0_1, P8.18 with R5F1_0.

## Left alone

| Pin | Why |
| --- | --- |
| P9.29 | SPI7 MISO, not used. Left as the base tree has it. |
| Every other header pin | Not used by the demo. Left as the base tree has it. |

The base tree muxes most header pads as GPIO inputs with transmit disabled. Toggling one of them as a Linux GPIO output does nothing until an overlay clears the transmit-disable bit. Many other header pins carry ICSSG PWM outputs in mode 3 (the BeagleBone AI-64 [expansion header tables](https://docs.beagleboard.io/boards/beaglebone/ai-64/04-expansion.html) list them), if a project needs more.
