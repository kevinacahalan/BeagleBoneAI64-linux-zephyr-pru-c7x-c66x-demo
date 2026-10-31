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

## Linux to Zephyr: eRPC over RPMsg

- The R5F image opens the RPMsg channel `rpmsg-raw` at address 1024, which Linux sees as `/dev/rpmsgN`.
- `zephyr/erpc/ai64.erpc` defines the functions Linux can call on Zephyr: `say`, `status`, `spiWrite`, `setPwm`, `setPwmSteps`, `takeChunk`, and `dmaCopy`. The generated client and server code is in `zephyr/erpc/shim/`; `zephyr/erpc/regen.sh` regenerates it after you change the IDL.
- Both sides run eRPC's framed transport straight over the RPMsg file descriptor (`linux-app/ai64_erpc_linux.cpp` and `zephyr/r5/common/src/erpc.cpp`).

In split mode a cluster's core 1 cannot power up before its core 0, and core 0 cannot power down before core 1. `ai64-demo` starts R5F0_0, R5F1_0, R5F0_1, R5F1_1 in that order and stops them in reverse.

## Why an app per core

A Cortex-R5F has no cache coherence with another R5F, so Zephyr cannot run one SMP kernel across them (its 32-bit Arm SMP support is for ARMv8-R and ARMv8-A cores). The way Zephyr does several cores is AMP: a kernel on each, built together with sysbuild, talking through `ipc_service`. That is what this directory is: four apps, one build, and the links between them.

Linux still loads and starts each core with remoteproc, and `ai64-demo` still restarts them one at a time. The board files are upstream Zephyr's `beaglebone_ai64/j721e/main_r5f0_0` (and `main_r5f0_1`, `main_r5f1_0`, `main_r5f1_1`), at the Zephyr `main` commit pinned in `zephyr/west.yml`; the cores without a console UART send their log to a RAM console that Linux reads as a trace. `zephyr/patches/` holds five fixes to Zephyr itself, which `zephyr/scripts/apply-patches.sh` applies after each `west update`: two to the EHRPWM driver ([EHRPWM4](#zephyr-ehrpwm4-on-p925)), two to the R5F timer driver ([P8.18](../../README.md#probe-the-pins)), and the TI-SCI ring configuration call the UDMA needs, which Zephyr declares but never implemented ([DMA](#r5f0_0s-dma-the-udma)).

**Asking another core.** `spiWrite`, `setPwm`, and `setPwmSteps` work on any core. A core whose job it is not sends the request to the core whose job it is (`common/src/linux_calls.c`), waits for its answer, and returns it; so `r5f1_0 spi` comes back as `passed spi to R5F0_0, which wrote 10 bytes (323 us)`, the time on R5F1_0's side. A fabric route's `pwm` or `steps` action goes to `r5f0_1`.

## R5F to R5F: ipc_service

`zephyr/r5/common/ai64_ipc.dtsi` gives each core a link to each other one, an instance of `ipc_service`'s icmsg backend: a 4 KiB buffer each way in shared DDR, and a mailbox FIFO each way as the doorbell. `zephyr/r5/common/src/ipc.c` opens the three links, sends a request as a small struct, and has a thread answer requests in order, so a slow job holds up nothing else. Each link uses icmsg's session handshake (`unbound = "enable"`): when a core restarts, it opens its links with a new session number, the others get an unbound callback, and they open the link again, so the links come back by themselves.

The buffers are in the 1 MiB of DDR the Linux overlay reserves at `0xa9000000`, past the fabric, and every core's MPU maps them non-cacheable. The doorbells needed some care:

- **Which mailboxes.** After boot, the MCU R5F's firmware, which runs the device manager, listens on mailbox cluster 7 and leaves messages in clusters 5, 6, 8, and 9. Writing doorbells into cluster 7 stopped the device manager answering anyone, until a power cycle. Clusters 10 and 11 have nobody listening; the fabric polls cluster 11's FIFOs 0 to 9.
- **Which users.** Only users 1 to 3 of a cluster reach the interrupt router (TI-SCI's `pend_intr` 0 to 2). So R5F0_1, R5F1_0, and R5F1_1 are users 1 to 3 of cluster 10, and R5F0_0 is user 1 of cluster 11, on its FIFOs 12 to 14. Each core routes its user to VIM 229, the way it routes Linux's doorbell ([below](#linux-to-zephyr-the-mailbox-interrupt)):

| Core | Doorbells | Router 213 input | Router output | VIM input |
| --- | --- | --- | --- | --- |
| R5F0_0 | `NAVSS0_MAILBOX_11` user 1 | 393 | 197 | 229 |
| R5F0_1 | `NAVSS0_MAILBOX_10` user 1 | 397 | 229 | 229 |
| R5F1_0 | `NAVSS0_MAILBOX_10` user 2 | 398 | 261 | 229 |
| R5F1_1 | `NAVSS0_MAILBOX_10` user 3 | 399 | 293 | 229 |

- **Restarts.** A mailbox is outside the core, so its interrupt enables survive a restart, and the others keep ringing meanwhile. Zephyr's mailbox driver skips a FIFO it has not enabled yet without reading it, so a stale one held the interrupt asserted and the restarted core did nothing but take it. `restart.c` clears the core's enables at boot.

R5F1_1's monitor pings the other three every 100 ms: about 80 µs a round trip, through the peer's icmsg work queue and its request thread and back.

## Zephyr and the device manager: TI-SCI

The board's device manager, TI's firmware on the MCU R5F, owns the SoC's clocks, power domains, and interrupt routers; every other core asks it over TI-SCI. Zephyr has a TI-SCI driver (`drivers/firmware/tisci`), and on top of it a clock-control driver and a power-domain driver, but it talks to the device manager through a secure-proxy mailbox driver that needs a receive interrupt for each thread and handles threads 0 to 31. On J721E no main secure-proxy interrupt is routed to a main-domain R5F, and these cores' threads are numbered past 31.

`zephyr/r5/common/src/sproxy.c` is a mailbox driver for the same secure proxy (`ai64,secure-proxy-poll`, in `zephyr/r5/common/dts/bindings/`) with no interrupt. A send writes the message to the core's transmit thread, then waits for the answer on its receive thread and hands it to the TI-SCI driver's callback before it returns, which is when the TI-SCI driver starts to wait for it. It waits a fixed number of polls rather than a time, so it also works before the system timer runs. Each core's overlay names its threads and TI-SCI host:

| Core | TI-SCI host | Transmit thread | Receive thread |
| --- | --- | --- | --- |
| R5F0_0 | 35 | 63 | 61 |
| R5F0_1 | 37 | 73 | 71 |
| R5F1_0 | 40 | 83 | 81 |
| R5F1_1 | 42 | 93 | 91 |

With that, Zephyr's own TI-SCI drivers work on these cores unchanged. Every core's mailbox interrupt routes use them (below), and so do R5F0_1's EHRPWM4, R5F1_0's GPIO2, and R5F0_0's UDMA channel.

## Linux to Zephyr: the mailbox interrupt

RPMsg rings sit in shared DDR. Each side tells the other "look at the rings" through a TI mailbox FIFO; each R5F core has its own pair of FIFOs. Zephyr's kick to Linux works as is. Linux's kick to Zephyr arrives in the FIFO, but on J721E the mailbox interrupt only reaches the R5F once the device manager routes it through the NAVSS interrupt router, and the upstream board does not ask for that.

So each R5F core asks at boot (`zephyr/r5/common/src/tisci.c`), with `tisci_cmd_rm_irq_set()`. The request names the router as both source and destination, with a router input and an output from the core's own range, the way Linux's `ti,sci-intr` and Zephyr PR [#121379](https://github.com/zephyrproject-rtos/zephyr/pull/121379) program it. First it releases the route a previous run left; the device manager refuses that when there was none. Each core's numbers are in its board overlay:

| Core | Mailbox user | Router 213 input | Router output | VIM input |
| --- | --- | --- | --- | --- |
| R5F0_0 | `NAVSS0_MAILBOX_1` user 2 | 434 | 196 | 228 |
| R5F0_1 | `NAVSS0_MAILBOX_1` user 1 | 433 | 228 | 228 |
| R5F1_0 | `NAVSS0_MAILBOX_2` user 1 | 429 | 260 | 228 |
| R5F1_1 | `NAVSS0_MAILBOX_2` user 2 | 430 | 292 | 228 |

The overlays also move the board's mailbox interrupt from VIM 256 to 228. If the request fails, the core logs it and its RPMsg thread polls the FIFO once a millisecond instead (`mbox_rx_drain()` in `zephyr/r5/common/src/rpmsg.c`).

## Zephyr: EHRPWM4 on P9.25

P9.25's ball W26 can be `EHRPWM4_B`, output B of the SoC's EHRPWM4. Zephyr's `ti,am3352-ehrpwm` driver drives it on R5F0_1, and `zephyr/r5/r5f0_1/src/pwm.c` sets it through the PWM API, on the `pwms` of `/zephyr,user`. At boot it starts the 15 steps, 5 ms a step, from a delayable work item on the GTC's time like the ICSSG0 slices' steps; after that it does whatever `setPwm` or `setPwmSteps` over eRPC, or a fabric route's `pwm` or `steps` action, asks for. `pwm_set()` would ask the device manager for EHRPWM4's clock rate over TI-SCI on every call, about 60 µs, so `pwm.c` asks once (`pwm_get_cycles_per_sec()`) and then calls `pwm_set_cycles()`. Steps keep time only if Zephyr's clock does: `zephyr/patches/0002` fixes the R5F timer driver, which lost the part of a tick left over at each timer interrupt, so Zephyr's clock ran about 1.6% slow on these cores; `0003` sets each tickless timeout from the tick rather than from the moment the driver runs. R5F0_1's overlay describes the rest:

- **Its clock**, through TI-SCI clock control: device 87, clock 0, the same as the Linux device tree's `main_ehrpwm4`.
- **Its power**, through a TI-SCI power domain for device 87, which turns the module on at boot (`CONFIG_POWER_DOMAIN`).
- **Its time-base clock gate**, bit 0 of `CTRLMMR_EPWM4_CTRL` at `0x00104150` (Linux's `ehrpwm_tbclk` 4), through a `ti,control-module` syscon. Zephyr's SoC code unlocks its CTRL_MMR partition at boot.
- **The pins**: W26 in mode 6, and AC4, shorted to it on the header, held off.

The stock Debian image gives EHRPWM4 and W26 to Linux with `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`; `install-beagle.sh` takes that overlay off, so that only Zephyr uses them.

The driver had one bug on this board: it handed `clock_control_get_rate()` its `uint64_t` rate as a `uint32_t *`, which leaves the upper half unset, and `pwm_set()` then saw a clock rate in the billions and failed with `-ENOTSUP`. `zephyr/patches/0001-drivers-pwm-ti_am3352_ehrpwm-set-all-64-bits-of-the-rate.patch` fixes it. It also made every period one count long, writing the period's count to TBPRD where counting up the period is TBPRD + 1 counts; `0004` fixes that ([Probe the pins](../../README.md#probe-the-pins)).

Every 10 ms R5F0_1's fabric thread publishes the PWM's `struct ai64_pwm_status` (`zephyr/include/ai64_shm.h`) in its fabric page, as the ICSSG0 RTUs do for their slices, and the `pwm` window shows it with the other three.

## R5F1_0: P8.18 from GPIO2

P8.18's ball AJ23 is `GPIO0_4` in mux mode 7. On J721E, bits 5:4 of a pad register pick which of the main domain's GPIO0, GPIO2, GPIO4, and GPIO6 drives the pad. Linux owns GPIO0 and has outputs in every one of its banks, and Zephyr's GPIO driver sets every pin of a bank to input when it starts, so R5F1_0 takes GPIO2, which Linux leaves disabled: its overlay sets bit 4 on the pad, asks the device manager for GPIO2's power (device 107) with a TI-SCI power domain, and enables upstream Zephyr's `main_gpio2_0`. `zephyr/r5/r5f1_0/src/blink.c` toggles the pin from a periodic `k_timer` and times each half period on the GTC.

## R5F0_0's DMA: the UDMA

J721E moves data with the main NAVSS's UDMA, and Zephyr has no driver for it. `zephyr/r5/r5f0_0/src/dma.c` does for one memory-to-memory channel what Linux's and U-Boot's `k3-udma` drivers do, polled:

1. **Ask the device manager**, over TI-SCI, for this host's ranges of UDMAP transmit and receive channels and of general-purpose rings (`tisci_cmd_get_resource_range()`); R5F0_0 gets channel 44 and ring 744.
2. **Configure**: the channel's transmit ring (ring 44) and a completion ring (744), 16 eight-byte elements each, in normal non-cacheable DDR; the transmit and receive channels as block-copy channels (TI-SCI channel type 12) that return finished descriptors to the completion ring; and the PSI-L pairing of transmit thread `0x1000 + 44` to receive thread `0x9000 + 44`. Then enable both channels.
3. **Copy**: one CPPI5 TR descriptor with one type-15 TR, source and destination address and a length up to 64 KiB. Write its address into the transmit ring and ring the ring's doorbell; poll the completion ring's occupancy until the descriptor comes back, then pop it.

Linux calls it with `dmaCopy` over eRPC, which only copies from the bulk buffer into a DSP's job block. `r5f0_0 dma` uses it to put an int8 layer's input into the C7x's L2 SRAM, 16 KiB in about 34 µs, then has the C7x run the layer on its MMA ([the DSPs](../../dsp/README.md#the-c7xs-matrix-multiply-accelerator)).

The pipeline uses the same channel for every window of P8.31 samples, with a second TR in the descriptor ([the pipeline](../../docs/pipeline.md)): after the copy, a 4-byte one writes the count of windows copied into the C66x's L2, the C66x's doorbell. Each TR has the WAIT flag, so the UDMA only starts the second once the first one's writes have landed, and the C66x never sees the count before the samples.

Two things on the way: Zephyr declares `tisci_cmd_ring_config()` but never implemented it (`zephyr/patches/0005` does), and the channel's realtime registers must not be touched before TI-SCI has configured the channel for this host. One early write there, to switch off a channel a previous run might have left on, was refused; the abort it left pending kept the core from ever reaching WFI, so Linux could no longer stop it (`can't stop rproc: -110`). Instead the core switches the channel off itself when Linux asks it to shut down.

## R5F caches

The J721E SoC code maps all of DDR as non-cacheable for the R5F, and leaves both caches off unless `CONFIG_CACHE_MANAGEMENT` is set. The image runs from DDR, so every instruction fetch went to DDR, and an eRPC call took about 1.3 ms.

`zephyr/r5/common/src/mpu.c` adds MPU regions at the first init level, before the caches come on: device memory for the peripherals in the low 2 GiB, the TCMs, and the core's 16 MiB carveout as write-through, so that every write still reaches DDR. The vrings, the resource table, the bulk buffer, the fabric, and the `ipc_service` buffers and DMA rings stay non-cacheable. `prj.conf` turns on `CONFIG_CACHE_MANAGEMENT`.

`ai64 bench <rpmsg-dev> [count]` times eRPC calls. On the bench, an eRPC `say` round trip went from 1263 µs to 85 µs on R5F0_1, R5F1_0, and R5F1_1, and to 160 µs on R5F0_0, which also logs each call on its UART. For comparison, an RPMsg echo from RTU0, which busy-polls, takes 35 µs.

## Linux to Zephyr: the bulk buffer

`r5f0_0 chunk` passes data too large for an RPC argument:
1. Linux writes the bytes into a 64 KiB DDR buffer at `0xA2FEF000` through `/dev/mem`.
2. Linux calls `takeChunk(seq, length, crc)`.
3. Zephyr checks the CRC.

The buffer sits inside the memory Linux already reserves for R5F0_0 (`0xA2000000`-`0xA2FFFFFF`). `zephyr/r5/r5f0_0/app.overlay` shrinks the board's DRAM region so that no part of the Zephyr image lands there. The layout is in `zephyr/include/ai64_shm.h`, which Linux, Zephyr, and the ICSSG cores include.

## Restarting an R5F core

The patched `ti_k3_r5_remoteproc` ([Two 6.18 kernels](../../docs/kernels.md#why-debian-needs-two-patched-modules)) stops a core by holding it in reset, without powering it off. Two things outlive that, and Zephyr's drivers expect a fresh power-up:

- **The VIM**, the core's interrupt controller. Taking an interrupt makes its priority level active until the handler ends it. A core stopped inside a handler leaves that level active, and the VIM then holds back every interrupt at that priority, the system timer's included. The core boots, logs a few lines, and stops dead.
- **The system timer**, a DMTIMER outside the core. It keeps counting, with a match interrupt pending from the last run.

`zephyr/r5/common/src/restart.c` runs before the system timer starts. It ends any interrupt left in service, clears the VIM's status, and stops the timer and clears its pending match. Without it, about one restart in six left a core hung, and after many restarts most of them were. With it, 30 restarts of all four cores in a row all came up.
