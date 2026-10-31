# Zephyr on the four main-domain R5F cores

Each R5F core has its own Zephyr app, a directory named after the core, and they share the code in `common/`. One sysbuild build makes all four images: `beagle-linux/scripts/build.sh zephyr` runs

```
west build --sysbuild -b beaglebone_ai64/j721e/main_r5f0_0 zephyr/r5/r5f0_0 -d build/zephyr
```

and `r5f0_0/sysbuild.cmake` adds the other three apps, each for its own board. Each image lands in `build/zephyr/<core>/zephyr/ai64_<core>.elf`.

## What runs where

| Core | Its job | Its own files (`<core>/src/`) | Pins |
| --- | --- | --- | --- |
| R5F0_0 | SPI7; the bulk buffer Linux writes; a UDMA channel; the pipeline's move into the C66x cores; the fabric's tours; the UART2 console | `spi.c`, `bulk.c`, `dma.c`, `pipe_dma.c` | SPI7: P9.31 CLK, P9.28 CS0, P9.30 MOSI. Console: P8.34 TX, P8.22 RX |
| R5F0_1 | EHRPWM4 output B, which Linux, the other cores, and fabric routes set; the pipeline's last stage, which sets it to each window's class | `pwm.c`, `pipe_pwm.c` | P9.25 |
| R5F1_0 | A square wave from GPIO2, toggled by a kernel timer, which times itself on the GTC | `blink.c` | P8.18 |
| R5F1_1 | The monitor: pings the other three, collects their status, and answers Linux's status call with the table | `monitor.c` | none |

Every core also runs `common/`:

| File | What it does |
| --- | --- |
| `rpmsg.c` | RPMsg to Linux on OpenAMP, the mailbox kick, and Linux's request to stop, which parks the core in WFI |
| `erpc.cpp`, `erpc_port_zephyr.cpp` | the eRPC server on that RPMsg endpoint |
| `linux_calls.c` | the functions Linux calls (`zephyr/erpc/ai64.erpc`); a call for another core's job goes to that core |
| `ipc.c` | the `ipc_service` links to the other three cores (`ai64_ipc.dtsi`), and the answers to their requests |
| `fabric.c` | this core's node on the fabric (`zephyr/include/ai64_fabric.h`) |
| `tisci.c`, `sproxy.c` | the mailbox interrupt routes, asked of the device manager over TI-SCI, which a polled secure-proxy driver carries |
| `mpu.c` | MPU regions, so the core's own DDR is cached and shared memory is not |
| `restart.c` | clears what a previous run left in the VIM and the system timer |

## A core's directory

- **`CMakeLists.txt`** reads `common/common.conf` and then the core's `prj.conf`, adds `common/` as a devicetree root for its bindings, includes `common/common.cmake`, and lists the core's own sources.
- **`prj.conf`** names the ELF and turns on what the job needs: `CONFIG_SPI`, `CONFIG_PWM`, a RAM console for the cores without a UART, and so on.
- **`app.overlay`** is the core's devicetree: its TI-SCI host and secure-proxy threads, its mailbox interrupt routes, its `ipc_service` links (it includes `common/ai64_ipc.dtsi`), and its job's devices and pins.
- **`src/main.c`** says what the core is. It fills in `ai64_r5_core` (`common/src/ai64_r5.h`), the hooks the common code calls: its status line, its answer to a request for its job, a fabric route's action, and so on. Its `start()` starts the job, at the `APPLICATION` init level, before `main()` and the common threads run.
- **`Kconfig`**, on R5F1_0 and R5F1_1, holds the job's settings: the half period of P8.18, and how often the monitor pings.

## Adding a core's job

To give a core a different job, change its `src/` and `main.c`: write the job, fill in the hooks it needs, and start it from `start()`. A request another core or Linux can make of it is an `AI64_IPC_*` op in `common/src/ai64_ipc.h`, answered by its `request` hook; `linux_calls.c` routes a Linux call for it to the core whose job it is. Its devices and pins go in its `app.overlay`, and the Kconfig options its drivers need in its `prj.conf`.
