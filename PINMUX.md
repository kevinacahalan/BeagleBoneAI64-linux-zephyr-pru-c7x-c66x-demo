# Pinmux

This demo muxes header pins from two places: the Linux overlay (the ICSSG0 pins) and the Zephyr devicetree inside the R5F image (the Zephyr pins). RPMsg, the DDR mailbox, and ICSSG shared RAM use no pin.

Most BeagleBone AI-64 header pins are wired to two SoC balls, an "a" ball and a "b" ball. The ball the demo does not use is held off (mux mode 7, transmit disabled) so it cannot fight the one it does.

The Linux overlay is `beagle-linux/overlays/k3-j721e-beagleboneai64-ai64-demo.dtso`. It writes each pad with `J721E_IOPAD()` from BeagleBoard-DeviceTrees `ti/k3-pinctrl.h` (commit `550f7711a4696d5f5e92505289379b754e871313`). This repository does not contain a copy of that header; `./beagle-linux/scripts/build.sh linux` clones the tree and preprocesses the overlay the way that tree's Makefile does (`cpp -nostdinc -I src/arm64`, then `dtc`).

A source line looks like:

```
J721E_IOPAD(0x0f4, PIN_OUTPUT, 0) /* (AB24) P8.11 PRG0_PRU0_GPO17 */
```

`J721E_IOPAD()` keeps the low 12 bits of the pad address (`0x0011c0f4`). `PIN_OUTPUT` compiles to `0x10000`, `PIN_INPUT` to `0x54000`, and `PIN_DISABLE` to `0x200000` (the transmit-disable bit). The mux mode is added to that cell. The Zephyr pins use `K3_PINMUX()` with the same pad offsets, and `0x200000 | mode 7` for a held-off ball.

## The pins

| Header | Ball | Pad | Mux | Core | Signal | Saleae channel on the lab bench |
| --- | --- | --- | --- | --- | --- | --- |
| P8.11 | AB24 | `0x0f4` | 0, `PRG0_PRU0_GPO17` | PRU0 | SLIP frame, UART 8N1 115200, idles high | 1 |
| P9.25 | W26 | `0x1a4` | 7, `GPIO0_104` | Tx_PRU0 | pulse trains | 0 |
| P9.25 | AC4 | `0x200` | 7, held off | — | shorted to W26 | |
| P9.28 | U2 | `0x230` | 6, `SPI7_CS0` | Zephyr | SPI7 chip select | 4 |
| P9.28 | AF28 | `0x0b0` | 7, held off | — | shorted to U2 | |
| P9.30 | V6 | `0x238` | 6, `SPI7_D0` | Zephyr | SPI7 MOSI | 6 |
| P9.30 | AE28 | `0x0b4` | 7, held off | — | shorted to V6 | |
| P9.31 | U3 | `0x234` | 6, `SPI7_CLK` | Zephyr | SPI7 clock, 1 MHz | 5 |
| P9.31 | AB26 | `0x0d4` | 7, held off | — | shorted to U3 | |
| P8.34 | AD22 | `0x01c` | 14, `UART2_TXD` | Zephyr | Zephyr console, 115200 8N1 | |
| P8.22 | AH23 | `0x014` | 14, `UART2_RXD` | Zephyr | Zephyr console | |
| P8.44 | AC25 | `0x10c` | 0, `PRG0_PRU1_GPO3` | ICSSG0 PRU1 | ring square wave | |
| P8.18 | AJ23 | `0x010` | 0, `PRG1_PRU0_GPO3` | ICSSG1 PRU0 | ring square wave | |
| P8.21 | AF21 | `0x07c` | 0, `PRG1_PRU1_GPO9` | ICSSG1 PRU1 | ring square wave | |

All signals are 3.3 V.

## PRU0: P8.11

PRU0 drives R30 bit 17. Mux mode 0 on ball AB24 is `PRG0_PRU0_GPO17`, which connects that bit to the pad. The overlay hangs the pinctrl on the `&pru0_0` node, so `pru_rproc` applies it when the core probes at boot.

## Tx_PRU0: P9.25

Tx_PRU0's R30 does not reach any pad; on the bench, a Tx_PRU0 toggling R30 with P8.11 muxed to `PRG0_PRU0_GPO17` produced no edges. So Tx_PRU0 drives P9.25 through the main GPIO0 module instead. W26 is muxed as `GPIO0_104` and hogged as an output in the overlay:

```
&main_gpio0 {
	ai64-txpru0-pulse-hog {
		gpio-hog;
		gpios = <104 GPIO_ACTIVE_HIGH>;
		output-low;
		line-name = "ai64-txpru0-pulse";
	};
};
```

Linux sets the direction once. Tx_PRU0 then writes only `SET_DATA` (`0x00600090`) and `CLR_DATA` (`0x00600094`) for lines 96-127, bit 8. Those registers set or clear just the bits written, so they do not disturb the other GPIO0 lines Linux owns. `gpioinfo -c gpiochip1 104` on the board shows the line as `output consumer="ai64-txpru0-pulse"`.

The stock image's boot entry selects `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`, which muxes the same W26 ball as `EHRPWM4_B`. `install-beagle.sh` removes that overlay from the default label and adds this demo's; the original `extlinux.conf` is saved as `extlinux.conf.ai64-bak`.

## The rings: P8.44, P8.18, P8.21

Each ring's PRU toggles one R30 bit every 65536 laps, so its pin is a square wave at the lap rate divided by 131072: about 15 Hz for ICSSG0 slice 1 (1.98 M laps/s) and 12.4 Hz for each ICSSG1 slice (1.63 M laps/s). The pads are muxed the same way as P8.11, mode 0 to the PRU's own GPO, with the pinctrl on the `&pru0_1`, `&pru1_0`, and `&pru1_1` nodes. Each of these header pins has only one ball, so there is no second ball to hold off. Neither ICSSG sets its GPCFG registers: both read `0`, direct GPO mode, the same as ICSSG0 slice 0, where P8.11 works.

On the lab bench these three pins have no Saleae channel yet, so their output has only been checked through the pad registers (`ai64 pads`), not on a logic analyzer.

## Zephyr: SPI7 and UART2

The Zephyr image muxes its own pins from its devicetree, the way the PocketBeagle 2 demo's M4 does. The R5F can write the pad registers because Linux has already unlocked them. Linux does not touch these pads: `main_spi7` and `main_uart2` stay disabled in the Linux device tree, and the board firmware leaves both modules powered (`k3conf dump device` shows `MCSPI7` and `UART2` as `DEVICE_STATE_ON`).

- SPI7 pins: `zephyr/r5/boards/beaglebone_ai64_j721e_main_r5f0_0.overlay`. D0 is MOSI (`ti,d1-miso-d0-mosi`). MISO (P9.29) is not muxed.
- UART2 pins: the upstream Zephyr board file. On the lab bench they go to a Raspberry Pi Debug Probe.

## Check on the board

With the demo running, the pad registers at `0x0011c000 + pad` read:

| Pad | Value | Meaning |
| --- | --- | --- |
| `0x0f4` | `0x10000` | output, mode 0 |
| `0x1a4` | `0x10007` | output, mode 7 |
| `0x200`, `0x0b0`, `0x0b4`, `0x0d4` | `0x200007` | held off |
| `0x230`, `0x234`, `0x238` | `0x10006` | output, mode 6 |
| `0x01c` | `0x1000e` | output, mode 14 |
| `0x014` | `0x5000e` | input, mode 14 |
| `0x10c`, `0x010`, `0x07c` | `0x10000` | output, mode 0 |

`sudo ai64 pads` reads them and marks any that differ. The Zephyr pads only take these values once R5F0_0 has booted.

## Left alone

| Pin | Why |
| --- | --- |
| P9.29 | SPI7 MISO, not used. Left as the base tree has it. |
| Every other header pin | Not used by the demo. Left as the base tree has it. |

The base tree muxes most header pads as GPIO inputs with transmit disabled. Toggling one of them as a Linux GPIO output does nothing until an overlay clears the transmit-disable bit.
