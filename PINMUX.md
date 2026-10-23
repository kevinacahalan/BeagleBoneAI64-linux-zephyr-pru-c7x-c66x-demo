# Pinmux

This demo muxes header pins from two places: the Linux overlay (the ICSSG pins) and the Zephyr devicetree inside the R5F0_0 image (the Zephyr pins). RPMsg, the fabric, and ICSSG RAM use no pin.

Most BeagleBone AI-64 header pins are wired to two SoC balls, an "a" ball and a "b" ball. The ball the demo does not use is held off (mux mode 7, transmit disabled) so it cannot fight the one it does. Every ICSSG pin the demo uses has one ball.

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
| P9.12 | AE27 | `0x0b8` | 3, `PRG0_PWM2_A0` | ICSSG0 PWM module, run by PRU1 | ICSSG PWM, output A |
| P9.15 | AD25 | `0x0c0` | 3, `PRG0_PWM2_B0` | ICSSG0 PWM module, run by PRU1 | ICSSG PWM, output B |
| P8.17 | AF22 | `0x00c` | 3, `PRG1_PWM2_A0` | the six ICSSG1 cores | the multicore scheduler |
| P9.25 | W26 | `0x1a4` | 6, `EHRPWM4_B` | Zephyr on R5F0_0 | EHRPWM4 output B |
| P9.25 | AC4 | `0x200` | 7, held off | — | shorted to W26 |
| P9.28 | U2 | `0x230` | 6, `SPI7_CS0` | Zephyr | SPI7 chip select |
| P9.28 | AF28 | `0x0b0` | 7, held off | — | shorted to U2 |
| P9.30 | V6 | `0x238` | 6, `SPI7_D0` | Zephyr | SPI7 MOSI |
| P9.30 | AE28 | `0x0b4` | 7, held off | — | shorted to V6 |
| P9.31 | U3 | `0x234` | 6, `SPI7_CLK` | Zephyr | SPI7 clock, 1 MHz |
| P9.31 | AB26 | `0x0d4` | 7, held off | — | shorted to U3 |
| P8.34 | AD22 | `0x01c` | 14, `UART2_TXD` | Zephyr | Zephyr console, 115200 8N1 |
| P8.22 | AH23 | `0x014` | 14, `UART2_RXD` | Zephyr | Zephyr console |

All signals are 3.3 V.

## ICSSG0 slice 0: P8.11 and P8.12

PRU0 drives R30 bits 17 and 16. Mux mode 0 on balls AB24 and AH28 is `PRG0_PRU0_GPO17` and `PRG0_PRU0_GPO16`, which connect those bits to the pads. The overlay hangs the pinctrl on the `&pru0_0` node, so `pru_rproc` applies it when the core probes. ICSSG0's `GPCFG0` register stays 0, direct GPO mode.

## ICSSG0 slice 1: P9.12 and P9.15

Mux mode 3 on these balls is the ICSSG's PWM module: `PRG0_PWM2_A0` and `PRG0_PWM2_B0` are the positive and negative outputs of set 2's first pair, which IEP1's CMP1 and CMP2 toggle. The pinctrl hangs on `&pru0_1`, the PRU that runs that set. A PWM pad is in tri-state with its pull-down on until the module drives it: the set's state registers start at Z, and so do they in the trip state, which an unmasked, unconnected trip input holds it in. PRU1 sets both pins low before the first period, and masks the trips.

## ICSSG1: P8.17

`PRG1_PWM2_A0`, mode 3, is the positive output of ICSSG1's PWM set 2. IEP1 never runs on ICSSG1, so the set stays in its initial state, and the pin follows the initial state the six cores' tasks write: that is how the RTUs and Tx_PRUs, whose R30 reaches no pad, drive a pin. The pinctrl hangs on `&pru1_0`, the core that sets the scheduler up.

## Zephyr: P9.25, SPI7, and UART2

The Zephyr image muxes its own pins from its devicetree, the way the PocketBeagle 2 demo's M4 does. The R5F can write the pad registers because Linux has already unlocked them. Linux does not touch these pads: `main_spi7`, `main_uart2`, and `main_ehrpwm4` stay disabled in the Linux device tree.

- **P9.25** is EHRPWM4 output B, mode 6 on W26, with AC4 held off: `zephyr/r5/boards/beaglebone_ai64_j721e_main_r5f0_0.overlay`. The stock image's boot entry selects `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`, which gives the same ball and EHRPWM4 to Linux. `install-beagle.sh` removes that overlay from the default label and adds this demo's; the original `extlinux.conf` is saved as `extlinux.conf.ai64-bak`.
- **SPI7**: the same overlay. D0 is MOSI (`ti,d1-miso-d0-mosi`). MISO (P9.29) is not muxed. The board firmware leaves MCSPI7 powered (`k3conf dump device` shows `DEVICE_STATE_ON`).
- **UART2**: the upstream Zephyr board file. Any 3.3 V USB-serial adapter reads the console there.

A Saleae Logic Pro at 100 MS/s measured P9.25 at 1,000,051.8 ns a period and 250,008.3 ns high, P8.11 and P8.12 at 50,001.8 ns a period, 12,500.2 and 35,460.4 ns high, edges within ±30 ns, and P9.12 and P9.15 at 12,500.3 and 35,461.0 ns high, edges within one 10 ns sample; the dead band between each pair measured 1,000 ns after A and 1,040 ns before it. P8.17 repeats every 100 ns, its steps 4 to 8 ns apart, at 500 MS/s.

## Check on the board

With the demo running, the pad registers at `0x0011c000 + pad` read:

| Pad | Value | Meaning |
| --- | --- | --- |
| `0x0f4`, `0x0f0` | `0x10000` | output, mode 0 |
| `0x0b8`, `0x0c0`, `0x00c` | `0x10003` | output, mode 3 |
| `0x1a4` | `0x10006` | output, mode 6 |
| `0x200`, `0x0b0`, `0x0b4`, `0x0d4` | `0x200007` | held off |
| `0x230`, `0x234`, `0x238` | `0x10006` | output, mode 6 |
| `0x01c` | `0x1000e` | output, mode 14 |
| `0x014` | `0x5000e` | input, mode 14 |

`sudo ai64 pads` reads them and marks any that differ. The Zephyr pads only take these values once R5F0_0 has booted.

## Left alone

| Pin | Why |
| --- | --- |
| P9.29 | SPI7 MISO, not used. Left as the base tree has it. |
| Every other header pin | Not used by the demo. Left as the base tree has it. |

The base tree muxes most header pads as GPIO inputs with transmit disabled. Toggling one of them as a Linux GPIO output does nothing until an overlay clears the transmit-disable bit. Many other header pins carry ICSSG PWM outputs in mode 3 (the BeagleBone AI-64 [expansion header tables](https://docs.beagleboard.io/boards/beaglebone/ai-64/04-expansion.html) list them), if a project needs more.
