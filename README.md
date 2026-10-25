# BeagleBone AI-64: Linux, Zephyr, PRU, RTU, Tx_PRU, and DSPs together

A starting point for BeagleBone AI-64 (TI TDA4VM / J721E) projects that use its heterogeneous cores:

- **Linux** on the Cortex-A72s runs `ai64`, one program that drives the demo.
- **Zephyr** runs on all four main-domain Cortex-R5F cores, R5F0_0, R5F0_1, R5F1_0, and R5F1_1 (cluster 0 or 1, core 0 or 1): one app, which one `west build --sysbuild` makes into an image per core, each with a job of its own ([One Zephyr app, four cores](#one-zephyr-app-four-cores)). R5F0_0 writes SPI7, and moves data with a channel of the SoC's UDMA; R5F0_1 drives a PWM on P9.25 with the SoC's EHRPWM4; R5F1_0 makes a square wave on P8.18 from a kernel timer; R5F1_1 watches the other three. They call each other through Zephyr's `ipc_service`, so any core takes a request for SPI or the PWM and passes it on to the one whose job it is. Each answers function calls from Linux too (eRPC over RPMsg). Their drivers get clocks, power, and interrupt routes from the board's device manager over TI-SCI, which a small polled mailbox driver carries on these cores ([Zephyr and the device manager](#zephyr-and-the-device-manager-ti-sci)).
- **ICSSG0 makes PWM three ways** ([PWM, three ways](#icssg0-pwm-three-ways)): slice 0 in software, its PRU writing its pins at times it polls from IEP0; slice 1 with the ICSSG's own PWM module, on IEP1 compare events; and the ICSSG's eCAP in APWM mode, which slice 0's Tx_PRU only gives a period and duty, copying slice 0's A to P8.15. Each slice makes a complementary pair, A and B: B is high while A is low, except for 1 µs of dead band where both are low. Like P9.25, both start stepping through 10, 15, and 20 kHz, and at each through 10, 25, 50, 75, and 90% duty, a step every 5 ms. In each slice the RTU talks to the rest of the SoC, the Tx_PRU modulates the duty, and the PRU makes the edges. RTU0 talks to Linux over RPMsg; RTU1 only over shared memory.
- **ICSSG1 runs a port of TI's open-pru [`multicore_scheduler`](https://github.com/TexasInstruments/open-pru/tree/main/examples/multicore_scheduler)** on all six of its cores: IEP0 compares start a task on each core in turn, 4 ns apart, and each task sets a header pin high or low ([The multicore scheduler](#icssg1-the-multicore-scheduler)).
- **The two C66x DSPs and the C7x DSP** log through their remoteproc trace buffers, which Linux follows, and run FIR filter jobs that Linux hands them through shared memory. The C7x also runs an int8 neural-network layer on its Matrix Multiply Accelerator, fed by its Streaming Engines: 256 rows in under 2 µs, 250 times one A72 core ([The C7x's MMA](#the-c7xs-matrix-multiply-accelerator)), with its input brought into its L2 SRAM by R5F0_0's DMA ([R5F0_0's DMA](#r5f0_0s-dma-the-udma)).
- **The fabric** joins these cores, and Linux, directly to each other: R5F to R5F, DSP to DSP, DSP to R5F, any of them to the ICSSG0 slices, and back. Every core pings every other one, R5F0_0 sends a message round all of them, and Linux can send one along any route, so that, say, the C7x passes it to a C66x that has ICSSG0 RTU1 change its PWM. Each message is stamped with the SoC's shared clock, so Linux shows the one-way latency of all 90 paths ([The fabric](#every-core-to-every-other-the-fabric)).

The ICSSG firmware is laid out the way TI's [open-pru](https://github.com/TexasInstruments/open-pru) projects are: `pru/pwm/` and `pru/multicore_scheduler/`, a directory per core with its own makefile and linker script, shared sources and a memory map per project ([The ICSSG firmware](#the-icssg-firmware)). In ICSSG0 **the RTU talks, and the PRU and Tx_PRU keep time**: the RTU is the slice's only link to the rest of the SoC, and the PRU and Tx_PRU only touch their slice's own RAM, so nothing elsewhere on the chip can stretch their timing. ICSSG1 keeps TI's design, where every core runs a task.

```mermaid
flowchart LR
  fab(("the fabric<br/>any core to any other"))
  subgraph dsps["DSPs"]
    c660["C66x 0"]
    c661["C66x 1"]
    c7x["C7x"]
    mma["C7x MMA<br/>matrix accelerator"]
  end
  ai64["Linux ai64<br/>A72"]
  subgraph zeph["Zephyr: one app, a job on each R5F"]
    r500["R5F0_0<br/>SPI, DMA"]
    r501["R5F0_1<br/>PWM"]
    r510["R5F1_0<br/>blink"]
    r511["R5F1_1<br/>monitor"]
    ipc(("ipc_service<br/>R5F to R5F"))
  end
  uart2["UART2, Zephyr console<br/>P8.34 TX, P8.22 RX"]
  spi7["SPI7<br/>P9.31 CLK, P9.28 CS0, P9.30 MOSI"]
  udma["main NAVSS UDMA<br/>a memcpy channel"]
  ep["EHRPWM4<br/>P9.25"]
  gpio2["GPIO2<br/>P8.18"]
  dm["device manager<br/>MCU R5F"]
  subgraph g0["ICSSG0"]
    subgraph s00["slice 0, software PWM"]
      rtu0["RTU0"]
      tx0["Tx_PRU0<br/>modulator"]
      pru0["PRU0"]
    end
    subgraph s01["slice 1, PWM module"]
      rtu1["RTU1"]
      tx1["Tx_PRU1<br/>modulator"]
      pru1["PRU1"]
    end
    iep0["IEP0<br/>counts ns"]
    r30["PRU0's R30<br/>A, B on P8.11, P8.12"]
    ecap["eCAP0, APWM<br/>A's copy on P8.15"]
    iep1["IEP1<br/>CMP0, CMP1, CMP2"]
    pwm2["PWM module, set 2<br/>A, B on P9.12, P9.15"]
  end
  subgraph g1["ICSSG1, scheduler"]
    mcs["six cores<br/>PRU, RTU, Tx_PRU x2"]
    g1iep["IEP0<br/>a compare every 4 ns"]
    g1tm["six task managers"]
    g1pwm["PWM2_0 initial state<br/>P8.17"]
  end
  c660 <-->|"FIR jobs, trace, shutdown"| ai64
  c661 <-->|"FIR jobs, trace, shutdown"| ai64
  c7x <-->|"FIR and layer jobs,<br/>trace, shutdown"| ai64
  c7x -->|"a layer, 16 rows a block"| mma
  ai64 <-->|"eRPC, bulk buffer, shutdown"| r500
  ai64 <-->|"eRPC, trace, shutdown"| r501
  ai64 <-->|"eRPC, trace, shutdown"| r510
  ai64 <-->|"eRPC, trace, shutdown"| r511
  ipc <-->|"icmsg"| r500
  ipc <-->|"icmsg"| r501
  ipc <-->|"icmsg"| r510
  ipc <-->|"icmsg"| r511
  r500 -->|"Zephyr console"| uart2
  r500 -->|"Zephyr SPI driver"| spi7
  r500 -->|"TR descriptors"| udma
  udma -->|"DDR to its L2"| c7x
  r501 -->|"Zephyr PWM driver"| ep
  r510 -->|"Zephyr GPIO driver,<br/>a k_timer"| gpio2
  r500 <-->|"TI-SCI: interrupt routes,<br/>UDMA rings and channels"| dm
  r501 <-->|"TI-SCI: interrupt routes,<br/>EHRPWM4 clock"| dm
  r510 <-->|"TI-SCI: interrupt routes,<br/>GPIO2 power"| dm
  r511 <-->|"TI-SCI: interrupt routes"| dm
  dm -->|"clock, power"| ep
  rtu0 -->|"pwm_ctl"| tx0
  tx0 -->|"pwm_next"| pru0
  pru0 -->|"sets up, polls"| iep0
  pru0 -->|"writes each edge"| r30
  tx0 -->|"period, duty"| ecap
  rtu1 -->|"pwm_ctl"| tx1
  tx1 -->|"pwm_next"| pru1
  pru1 -->|"sets up, rearms compares"| iep1
  pru1 -->|"sets up"| pwm2
  iep1 -->|"compare events toggle"| pwm2
  mcs -->|"set up"| g1iep
  mcs -->|"arm"| g1tm
  g1iep -->|"compare events"| g1tm
  g1tm -->|"start a task"| mcs
  mcs -->|"each task writes"| g1pwm
  ai64 -.->|"reads the registers"| g1
  ai64 <-->|"RPMsg text"| rtu0
  fab <--> ai64
  fab <--> r500
  fab <--> r501
  fab <--> r510
  fab <--> r511
  fab <--> c660
  fab <--> c661
  fab <--> c7x
  fab <-->|"requests, status"| rtu0
  fab <-->|"requests, status"| rtu1
```

Each arrow goes from a core to what it controls or talks to; the label says how. Linux also loads and starts every core here through remoteproc, which the diagram leaves out. The four R5Fs run one Zephyr app, built once with sysbuild into an image per core, each with a job of its own, and talk to each other through Zephyr's `ipc_service`. The fabric joins Linux, the R5Fs, the DSPs, and both ICSSG0 RTUs directly to each other, so any of them can reach either slice, not only Linux. RTU1 takes requests only over the fabric; RTU0 takes them over RPMsg too. What the fabric is made of, and how far into a slice it reaches:

```mermaid
flowchart LR
  fab(("the fabric<br/>DDR inboxes, mailbox doorbells,<br/>GTC stamps, a hardware spinlock"))
  l["Linux"] <--> fab
  z["R5F0_0, R5F0_1,<br/>R5F1_0, R5F1_1"] <--> fab
  d["C66x 0, C66x 1, C7x"] <--> fab
  p["ICSSG0 RTU0 and RTU1"] <--> fab
  p <-->|"slice RAM only"| rt["each slice's Tx_PRU and PRU"]
```

Each link is small and easy to copy into your own project. [How it works](#how-it-works) walks through them.

The MCU-domain R5F cores are left alone: they run the board's device manager.

It runs on the BeagleBoard Debian image, plus a one-time install script. A Yocto (Poky) configuration in `yocto/` builds the demo into an image of its own, which has been built and tested on the board. Both images run Linux 6.18, but different kernels: BeagleBoard's on Debian, TI's on Yocto ([Two 6.18 kernels](#two-618-kernels)).

## What you need

- A BeagleBone AI-64, a microSD card (16 GB or more), and a 5 V supply for it.
- A network link to it: Ethernet to your LAN, or the USB-C port, which brings up a USB network adapter on the PC.
- A Linux PC with `git`, `ssh`, `rsync`, `xz`, and either `podman` or `docker`. Every compiler runs in a container, so you don't install any toolchains.
- About 6 GB of free disk on the PC: roughly 3 GB of container images plus 2 GB of fetched sources. The first build takes 10–20 minutes, mostly downloading. Later builds take seconds to a minute.
- Network access from the PC to GitHub, git.ti.com, software-dl.ti.com, and Docker Hub. git.ti.com is only for the PRU Software Support Package.
- Optional: a logic analyzer, to watch SPI7, the four PWM generators, and the multicore scheduler on the header ([Probe the pins](#probe-the-pins)); and a 3.3 V USB-serial adapter on P8.34 (TX) and P8.22 (RX), to read the Zephyr console.

Get the code:

```
git clone https://github.com/kevinacahalan/BeagleBoneAI64-linux-zephyr-pru-c7x-c66x-demo.git
cd BeagleBoneAI64-linux-zephyr-pru-c7x-c66x-demo
```

Every command below runs from this directory unless it says "on the board".

## Quick start: BeagleBoard Debian

### 1. Flash the card and log in

Download [BeagleBone AI-64 Debian 13.7 IoT, v6.18 k3](https://www.beagleboard.org/distros/bbai64-debian-13-7-2026-10-09-iot-v6-18-x-k3), the file `bbai64-debian-13.7-iot-v6.18-k3-arm64-2026-10-09-10gb.img.xz`. A fresh image has no login until you give it one. Pick either way:

- **BeagleBoard Imager** (easiest). Choose the image, and in its settings set a user name, a password, and your SSH public key. Imager writes them for you.
- **By hand.**
  1. Write the image: `xz -dc bbai64-debian-*.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress`. Replace `/dev/sdX` with your card. Check it with `lsblk` first: `dd` overwrites whatever you give it.
  2. Mount the card's `BOOT` partition.
  3. Add these lines to the end of `sysconf.txt`, with your own values. The board applies them on first boot and then clears the password from the file.

     ```
     user_name=debian
     user_password=choose-a-password
     user_authorized_key=ssh-ed25519 AAAA... you@pc
     hostname=bbai64
     ```

     The key line is the contents of your `~/.ssh/id_ed25519.pub`. Run `ssh-keygen -t ed25519` if you don't have one. With the key in place, the build steps below don't keep asking for a password.

Put the card in the board and power it. With a card in the slot the board boots from it; the console prints `Trying to boot from MMC1`. If yours boots its eMMC instead, hold the BOOT button while you apply power.

A fresh card reboots twice on its own before it stays up: the first boot applies `sysconf.txt` and grows the root partition, and the second resizes the filesystem. Give it about three minutes. Then log in over the network:

```
ssh debian@bbai64.local
```

Below, `USER@BOARD` means that login, for example `debian@bbai64.local`. You'll also need its password for `sudo` on the board. If `.local` names don't resolve on your PC, use the address your router gave the board. Over the USB-C cable the board is `192.168.7.2`.

The serial console is UART0 on the 3-pin debug header, 115200 8N1 (`ttyS2` on the board), if SSH doesn't come up.

### 2. Build on the PC

```
./beagle-linux/scripts/build.sh
AI64_SSH=USER@BOARD ./beagle-linux/scripts/build.sh kmod
./beagle-linux/scripts/build.sh bundle
```

- **The first command** builds `ai64`, the overlay, the twelve ICSSG images, the four Zephyr R5F images, and the two DSP images.
- **`kmod`** builds the two patched kernel modules for the kernel running on your board. It asks the board for its kernel version over SSH, and the first time it copies that kernel's headers with `rsync`. The board must be up and reachable. [Why two patched modules](#why-debian-needs-two-patched-modules) explains why they're needed.
- **`bundle`** gathers everything into `out/ai64-beagle/`.

### 3. Install on the board and reboot

```
scp -r out/ai64-beagle USER@BOARD:
ssh -t USER@BOARD 'sudo ./ai64-beagle/install-beagle.sh && sudo reboot'
```

It asks for your `sudo` password on the board. `install-beagle.sh` makes the setup survive reboots:
- It installs `ai64`, `ai64-demo`, and the firmware for every core.
- It installs the patched `pru_rproc` and `ti_k3_r5_remoteproc` modules so the kernel loads them at boot.
- It adds the demo overlay to the default boot entry, and takes off the stock P9.25 PWM overlay, which muxes the same pin. The original is saved as `extlinux.conf.ai64-bak`.
- It adds a udev rule so the `dialout` group can use `/dev/rpmsg*`.
- It installs `tmux` with apt if the image doesn't have it. The stock IoT image doesn't. This needs the board to reach the internet; over Ethernet it usually can. If it can't, the script says so and carries on.

Nothing of the demo starts at boot: every core stays off until `ai64-demo` starts it, so the whole demo starts and stops in one place. The R5F images go in as `/lib/firmware/ai64_r5f0_0.elf` and so on, not under the kernel's default names (`j7-main-r5f0_0-fw`), so the kernel does not boot them either. An earlier version started the ICSSG cores from a service; `install-beagle.sh` removes it.

Give the board about 30 seconds to reboot, then log back in.

### 4. Run the demo

On the board:

```
ai64-demo --tmux
```

- It asks for your `sudo` password once.
- It restarts the four R5F cores, the twelve ICSSG cores, and the DSPs with the installed firmware, then opens a tmux session with twelve windows: `linux`, `r5f0_0`, `r5f0_1`, `r5f1_0`, `r5f1_1` (Zephyr on each R5F core), `rtu0` (ICSSG0 RTU0), `pwm`, `sched`, `c66x0`, `c66x1`, `c7x`, and `fabric`. Before it stops each R5F and DSP, it asks the core to go idle ([why](#restarting-the-c7x)). The `fabric` window is about 100 columns wide.
- [What you should see](#what-you-should-see) shows the output of a passing run, and [Commands](#commands) lists what you can type in the `linux` window.

tmux keys: switch windows with Ctrl+b then 0–9, or Ctrl+b then n and p for next and previous, or Ctrl+b then w for a list. Detach, leaving the demo running, with Ctrl+b then d. Come back with `ai64-demo --attach --tmux`.

To stop the demo, run `tmux kill-session -t ai64`. Typing `quit` in the `linux` window only ends that window.

Without tmux, plain `ai64-demo` prints all twelve windows on one console instead. There, `quit` or Ctrl+C stops the whole demo.

### After you change the code

- **`ai64` or any core's firmware:** rebuild with `build.sh`, run `build.sh bundle`, copy the bundle, and run `install-beagle.sh` again. No reboot is needed: the next `ai64-demo` restarts every core with the new firmware.
- **The overlay:** the same steps, then reboot. Pin mux only changes at boot.
- **After the board's kernel is upgraded** (for example by `apt upgrade`): run `build.sh kmod` again, then `bundle`, `install-beagle.sh`, and reboot. The patched modules only fit the kernel they were built for.

## What you should see

On startup the `linux` window runs a short script: it pings RTU0; asks Zephyr on R5F0_0 to `say` something, check a `chunk` of the bulk buffer, and write `spi`; asks R5F0_1 to start P9.25's `steps`; asks RTU0 for its `status`; asks Zephyr on each other R5F core to `say` something; asks R5F1_0, which has no SPI, to write `spi`, which it passes to R5F0_0; asks the monitor, R5F1_1, for every core's `status`; and has R5F0_0 `dma` a layer's input into the C7x for its MMA. Each `Linux -> X` line is a command Linux sent, and the indented lines under it are the answers, which also show in that core's own window. A passing run, one block per window:

```
== linux
Each line "Linux -> X" is a command Linux sent; the indented lines under it are
the answers, which also show in that core's own window.
Linux -> ICSSG0 RTU0: ping
  Linux sent "ping" to ICSSG0 RTU0, and it echoed it
Linux -> R5F0_0 (Zephyr): say hello from linux
  R5F0_0: say "hello from linux": it replied "Zephyr on R5F0_0 got: hello from linux"
Linux -> R5F0_0 (Zephyr): chunk bulk-hello
  R5F0_0: chunk: the bulk buffer matched (seq 1, 10 bytes, crc c959d039)
Linux -> R5F0_0 (Zephyr): spi
  R5F0_0: spi "hello": it replied "Zephyr on R5F0_0: spi wrote 5 bytes"
Linux -> R5F0_1 (Zephyr): steps 5
  R5F0_1: steps: P9.25 (EHRPWM4 output B) steps 10, 15, 20 kHz x 10-90% duty, 5 ms a step
Linux -> ICSSG0 RTU0: status
  software PWM on P8.11 (A) and P8.12 (B) now: period 66664 ns, A high 59996 ns, dead band 1000 ns, 84571 periods, 0 late edges
  hardware PWM on P8.15 (eCAP0, a copy of A) now: period 66664 ns, high 59996 ns, 87125 periods
Linux -> R5F0_1 (Zephyr): say hello from linux
  R5F0_1: say "hello from linux": it replied "Zephyr on R5F0_1 got: hello from linux"
Linux -> R5F1_0 (Zephyr): say hello from linux
  R5F1_0: say "hello from linux": it replied "Zephyr on R5F1_0 got: hello from linux"
Linux -> R5F1_1 (Zephyr): say hello from linux
  R5F1_1: say "hello from linux": it replied "Zephyr on R5F1_1 got: hello from linux"
Linux -> R5F1_0 (Zephyr): spi via r5f1_0
  R5F1_0: spi "via r5f1_0": it replied "Zephyr on R5F1_0: passed spi to R5F0_0, which wrote 10 bytes (323 us)"
Linux -> R5F1_1 (Zephyr): status
  R5F1_1: status: Zephyr on R5F1_1: monitor: pinging the other R5F cores every 100 ms, 84 rounds
    R5F0_0: round trip 79.96 us (76.93 to 90.41), 0 of 84 lost; SPI7: 2 writes, 10 bytes
    R5F0_1: round trip 78.18 us (75.65 to 82.45), 0 of 84 lost; P9.25: stepping every 5 ms, now 20000 Hz at 90% duty
    R5F1_0: round trip 82.81 us (77.45 to 95.36), 0 of 84 lost; P8.18: 17010 toggles, every 500 us; last 1000 off by -2280 to 2240 ns, mean 0; worst 14535 ns
Linux -> R5F0_0 (Zephyr): dma
  R5F0_0: dma: its UDMA copied 16384 bytes from DDR to the C7x's L2 in 33.6 us (487 MB/s; the R5F reading them itself: 5445.7 us), then the C7x's MMA ran the layer: 2488 cycles, 4.0 us round trip; all match the A72
linux window ready. commands: r5fX_Y say|status|spi|pwm|steps ... | r5f0_0 chunk|dma ... | rtu0 pwm|steps|sweep|dead|status|<text> | help

== r5f0_0 (each R5F core's window shows the calls Linux made to it)
R5F0_0 window: Linux calls Zephyr on R5F0_0 over eRPC, on /dev/rpmsg1
R5F0_0: say "hello from linux": it replied "Zephyr on R5F0_0 got: hello from linux"
R5F0_0: chunk: the bulk buffer matched (seq 1, 10 bytes, crc c959d039)
R5F0_0: spi "hello": it replied "Zephyr on R5F0_0: spi wrote 5 bytes"
R5F0_0: dma: its UDMA copied 16384 bytes from DDR to the C7x's L2 in 33.6 us (487 MB/s; the R5F reading them itself: 5445.7 us), then the C7x's MMA ran the layer: 2488 cycles, 4.0 us round trip; all match the A72

== rtu0
rtu0 window: Linux and ICSSG0 RTU0 over RPMsg, on /dev/rpmsg5
software PWM on P8.11 (A) and P8.12 (B) runs request 3, now period 66664 ns, A high 6664 ns
Linux -> rtu0: ping
Linux sent "ping" to ICSSG0 RTU0, and it echoed it
Linux -> rtu0: status
software PWM on P8.11 (A) and P8.12 (B) now: period 50000 ns, A high 5000 ns, dead band 1000 ns, 90899 periods, 0 late edges
hardware PWM on P8.15 (eCAP0, a copy of A) now: period 50000 ns, high 5000 ns, 96665 periods

== pwm (every 10 s, what each PWM generator's core reports)
pwm window ready: every 10 s, what each PWM generator's core reports it runs
(read from shared memory; "now" is whichever step it was on)
P8.11 + P8.12  software PWM: ICSSG0 slice 0, PRU0 sets the pins at the times it reads from IEP0
  stepping: 10, 15, 20 kHz, at each 10, 25, 50, 75, 90% duty, 5 ms a step
  now: 15.001 kHz, 75.0% duty
  P8.11 (A) high 49996 ns of each 66664 ns period, from 40 ns in
  P8.12 (B) high 14628 ns: A inverted, but low for 1000 ns of dead band either side of A's pulse
  14996 periods/s, 0 late edges (the latest edge 24 ns late)
P8.15          hardware PWM: ICSSG0's eCAP0 in APWM mode, a copy of P8.11 (A) that Tx_PRU0 sets
  stepping: 10, 15, 20 kHz, at each 10, 25, 50, 75, 90% duty, 5 ms a step
  now: 15.001 kHz, 75.0% duty
  P8.15 high 49996 ns of each 66664 ns period
  14996 periods/s, counted from the eCAP's period flag
P9.12 + P9.15  hardware PWM: ICSSG0 slice 1, the ICSSG's PWM module toggles the pins on IEP1 compares
  stepping: 10, 15, 20 kHz, at each 10, 25, 50, 75, 90% duty, 5 ms a step
  now: 15.001 kHz, 75.0% duty
  P9.12 (A) high 49996 ns of each 66664 ns period, from 40 ns in
  P9.15 (B) high 14628 ns: A inverted, but low for 1000 ns of dead band either side of A's pulse
  14990 periods/s, 0 late edges
P9.25          hardware PWM: the SoC's EHRPWM4, output B, set by Zephyr on R5F0_1 (Zephyr's PWM driver)
  stepping: 10, 15, 20 kHz, at each 10, 25, 50, 75, 90% duty, 5 ms a step
  now: 10.000 kHz, 50.0% duty
  P9.25 high 50000 ns of each 100000 ns period

== sched (ICSSG1's schedule every 10 s)
sched window ready, ICSSG1's schedule every 10 s
ICSSG1 multicore scheduler: IEP0 period 100 ns, +4 ns a tick (the 250 MHz core clock)
  PRU0     IEP0 CMP7  at     4 ns  sets P8.17 high  task manager on
  PRU1     IEP0 CMP2  at     8 ns  sets P8.17 low   task manager on
  RTU1     IEP0 CMP3  at    12 ns  sets P8.17 high  task manager on
  RTU0     IEP0 CMP4  at    16 ns  sets P8.17 low   task manager on
  Tx_PRU1  IEP0 CMP5  at    20 ns  sets P8.17 low   task manager on
  Tx_PRU0  IEP0 CMP6  at    24 ns  sets P8.17 high  task manager on
  P8.17 (PWM2_0's initial state) read high 79% of 100000 samples, 41664 changes seen

== c66x0 (c66x1 is the same, with DNUM 1)
4d80800000.dsp trace (ai64-c66x.elf), 2048 bytes at 0x4d80806dd8
hello from the c66x, DNUM 0
c66x up 10 s
c66x up 20 s

== c7x
64800000.dsp trace (ai64-c7x.elf), 2048 bytes at 0xa8100000
hello from the c7x, DNUM 1024, MMU on, L1DCFG 4, L2CFG 2
c7x up 10 s
c7x up 20 s
```

- **The `pwm` window** shows what each generator's own core says it is running, read from shared memory. It does not measure the pins. Each generator starts as two lines: its pins, then whether it is software or hardware PWM and what makes the edges. The values are a snapshot of whichever step the generator was on at that moment, so they change every 10 s. `periods/s` averages the three frequencies over the 10 s, so it reads about 15000.
- **A and B** on each ICSSG0 slice form a complementary pair: B is high while A is low, except for 1000 ns either side of A's pulse, where both are low (the dead band, which keeps a half bridge's two switches from ever being on together). So B's high time is the period less A's, less two dead bands, less the 40 ns that A starts into each period. [Probe the pins](#probe-the-pins) draws it.
- **The R5F round trips** are R5F1_1 pinging each other core over `ipc_service` every 100 ms: a request into the peer's buffer and a mailbox doorbell, through the peer's icmsg work queue and its request thread, and the reply back the same way, about 80 µs. The `dma` line's copy is R5F0_0's UDMA moving 16 KiB; the R5F's own time for the same bytes is that slow because it reads them through an uncached mapping, which is how it shares the buffer with Linux.
- **RTU0's `runs request` line** comes when PRU0 starts running the last request. An ICSSG0 RTU keeps counting requests across restarts, so the number is not always the same.

The `fabric` window, the latency of every core to every other, is in [The fabric](#every-core-to-every-other-the-fabric). The `/dev/rpmsg` numbers can differ on your board. `sched` reads P8.17's state over the bus, far slower than the tasks change it, so its share is a rough one; [The multicore scheduler](#icssg1-the-multicore-scheduler) has what the schedule does.

The other three R5F cores have no console UART. Each logs into its remoteproc trace buffer instead:

```
$ ai64 trace 5d00000.r5f
*** Booting Zephyr OS build a915b2f12675 ***
I: pwm: Zephyr set P9.25 (EHRPWM4_B) stepping 10, 15, 20 kHz x 10-90% duty, every 5 ms
I: eRPC server waiting on rpmsg
I: tisci: mailbox interrupt routed, router 213 input 433 -> output 228
I: tisci: mailbox interrupt routed, router 213 input 397 -> output 229
I: link to R5F0_0 up
I: link to R5F1_0 up
I: ai64 r5f0_1 hello
I: fabric node 2 up, start 9
I: rpmsg channel rpmsg-raw addr 1024 is up
I: link to R5F1_1 up
I: pwm: Linux set P9.25 (EHRPWM4_B) stepping 10, 15, 20 kHz x 10-90% duty, every 5 ms
I: erpc say: Zephyr on R5F0_1 got: hello from linux
```

The first route is Linux's doorbell, the second the other R5F cores'; the `link ... up` lines are its `ipc_service` links coming up as the others boot. R5F1_0 starts with `blink: P8.18 toggles every 500 us`, and R5F1_1 writes the monitor's table into its trace every 10 s. R5F0_0 logs the same kind of lines on its console on P8.34, plus `spi7 hello wrote 5 bytes` at boot, `dma: UDMA channel 44, transmit ring 44, completion ring 744` at its first copy, and a line for each request.

The ICSSG cores write their logs the same way, one or two lines each:

```
$ ai64 trace b006000.rtu
ICSSG0 RTU1 up
ICSSG0 RTU1 is fabric node 9
hardware PWM on P9.12 (A) and P9.15 (B) runs request 14, now period 66664 ns, A high 59996 ns
$ ai64 trace b038000.pru
ICSSG0 PRU1 up: ICSSG PWM2_0 on IEP1, A on P9.12, B on P9.15
first period: 100000 ns, duty 10000 ns
$ ai64 trace b104000.rtu
ICSSG1 RTU0 up: its task, on IEP0 CMP4, sets P8.17 low
```

`N` in `remoteprocN` is the core's number in `/sys/class/remoteproc`; `cat /sys/class/remoteproc/remoteprocN/name` shows which core it is ([The ICSSG firmware](#the-icssg-firmware) lists the twelve ICSSG cores' names).

### Commands

Type these in the linux window. The R5F commands start with the core's name, `r5f0_0`, `r5f0_1`, `r5f1_0`, or `r5f1_1`: each R5F core runs its own copy of the app, with its own job. `spi`, `pwm`, and `steps` work on any of them: a core without that job passes the call to the one with it.

| Command | What happens |
| --- | --- |
| `r5fX_Y say <text>` | Zephyr on that core replies `Zephyr on R5FX_Y got: <text>` (eRPC) |
| `r5fX_Y status` | that core's job and how it is doing; on R5F1_1, the monitor, a line for every other core too, with its round trip |
| `r5fX_Y spi [text]` | R5F0_0 writes `text`, or `hello`, on SPI7 (header P9) |
| `r5fX_Y pwm <period_ns> <duty_ns>` | R5F0_1 sets P9.25 (EHRPWM4 output B) to a fixed period and high time, and stops the steps |
| `r5fX_Y steps <ms>` | R5F0_1 steps P9.25 through 10, 15, and 20 kHz, and at each through 10, 25, 50, 75, and 90% duty, `ms` a step; `steps 0` stays on the current step |
| `r5f0_0 chunk <text>` | Linux writes the DDR bulk buffer, and Zephyr checks its CRC |
| `r5f0_0 dma [rows]` | Linux writes an int8 layer's input (`rows` x 64 bytes, 256 rows unless you say) in the bulk buffer; R5F0_0's UDMA copies it into the C7x's L2 SRAM; the C7x runs the layer on its MMA; Linux checks both against the A72 |
| `rtu0 pwm <period_ns> <duty_ns>` | RTU0 sets the software PWM on P8.11 (A) and P8.12 (B), and its eCAP copy of A on P8.15, to a fixed period and A high time, and stops the steps |
| `rtu0 steps <ms>` | the same steps as P9.25, on the same GTC time, made by the slice's Tx_PRU; `steps 0` stops them and goes back to the last fixed period and duty (20 kHz, 25% until there is one) |
| `rtu0 sweep <ms>` | at the period of the last `pwm`, the Tx_PRU sweeps A's high time from its least to its most and back every `ms`; `sweep 0` stops it |
| `rtu0 dead <ns>` | the dead band on each side of A's pulse |
| `rtu0 status` | what PRU0 runs now, how many periods, and how many late edges; and what the eCAP on P8.15 runs |
| `rtu0 <text>` | RTU0 echoes the text |
| `help`, `quit` | List the commands, or leave |

Periods run from 2000 ns to 1 s. On the ICSSG0 slices every time is a multiple of 4 ns, one IEP count, and every edge stays at least 400 ns from the next on the same pin, so A's high time runs from 400 ns to the period less 440 ns and twice the dead band; the RTU clamps what it is asked for. All four generators start stepping, 5 ms a step; a fixed `pwm` request stops the steps, and `steps <ms>` starts them again. The eCAP on P8.15 has no requests of its own: it follows slice 0. Slice 1, the hardware PWM on P9.12 and P9.15, has no RPMsg: `sudo ai64 fabric route g0rtu1 pwm <period_ns> <duty_ns>` (or `steps`, `sweep`, or `dead`) reaches it through the fabric instead, and so can any core on the route.

`ai64-demo` without `--tmux` prints all twelve windows on one console instead, with each line prefixed `[Linux]`, `[R5F0_0]`, `[R5F0_1]`, `[R5F1_0]`, `[R5F1_1]`, `[RTU0]`, `[PWM]`, `[Sched]`, `[C66x0]`, `[C66x1]`, `[C7x]`, or `[Fabric]`. Either way it restarts the cores with their firmware first. `--attach` connects to the running cores without restarting them.

`ai64` has a few more subcommands for checking a board: `sudo ai64 pads` checks the pin mux of every demo pin, `ai64 trace <remoteproc-name>` follows any core's trace log (`ai64 trace 5d00000.r5f`, `ai64 trace b034000.pru`, and so on), `sudo ai64 pwm once` and `sudo ai64 sched once` print one sample of the `pwm` and `sched` windows, `ai64 bench <rpmsg-dev>` times eRPC calls to one R5F core, `sudo ai64 dsp <remoteproc-name>` offloads FIR jobs to a DSP ([how](#linux-to-the-dsps-fir-jobs)), `sudo ai64 mma [rows] [runs]` runs the int8 layer on the C7x's MMA and in plain C ([how](#the-c7xs-matrix-multiply-accelerator)), `sudo ai64 shutdown <remoteproc-name>` asks a core to go idle before Linux stops it, and `sudo ai64 fabric route <node>... [pwm <period_ns> <duty_ns> | steps <ms> | sweep <ms> | dead <ns>]` sends a message along any cores you name and prints each hop's time ([routes](#every-core-to-every-other-the-fabric)).

## Probe the pins

All signals are 3.3 V; set a logic analyzer's threshold for 3.3 V logic.

| Signal | Header pins | What you should see |
| --- | --- | --- |
| SPI7, Zephyr on R5F0_0 | P9.28 CS, P9.31 CLK, P9.30 MOSI | `hello` at 1 MHz, mode 0, MSB first, once at startup and again for each `spi` |
| Hardware PWM, EHRPWM4 output B, set by Zephyr on R5F0_1 | P9.25 | the steps below, one output |
| Square wave, GPIO2, Zephyr on R5F1_0 | P8.18 | 1 kHz: a kernel timer toggles it every 500 µs |
| Software PWM, ICSSG0 slice 0 | P8.11 A, P8.12 B | the steps below on A; B the complement of A with dead band |
| Hardware PWM, ICSSG0's eCAP0, set by Tx_PRU0 | P8.15 | the same period and high time as P8.11, one output, from the eCAP's own counter (its periods are not in phase with P8.11's) |
| Hardware PWM, the ICSSG PWM module, ICSSG0 slice 1 | P9.12 A, P9.15 B | the same as P8.11 and P8.12, from the PWM module instead of software |
| Multicore scheduler, ICSSG1 | P8.17 | low from 8 to 12 ns and from 16 to 24 ns of every 100 ns, high the rest |
| Zephyr console | P8.34 TX, P8.22 RX | 115200 8N1 |

The pictures here are logic analyzer captures of the board running the demo, started by `ai64-demo` and left at its defaults.

![Captured: each period's duty on P8.11, P8.15, P9.12, and P9.25 through one 75 ms pass of the steps, and a 300 µs zoom on one step change](media/pwm-steps.svg)

**The steps.** All four PWM generators run the same 15-step program, 5 ms a step, 75 ms in all, then over again: 10 kHz at 10, 25, 50, 75, and 90% duty, then 15 kHz at the same five, then 20 kHz. The program keeps the GTC's time, the SoC's 200 MHz counter: step `n` runs while `GTC / 5 ms` counts `n` modulo 15. Every generator works out from the GTC which step it is on, so all four change step together, however far apart they were started. Each takes a new step at the end of one of its own periods, and their periods are not in phase with each other, so they do not change at the same instant. In the capture above, the three ICSSG0 generators change within 34 µs of each other, and P9.25 from 63 µs before them to 71 µs after: Zephyr sets its step from a 100 µs kernel tick, and the EHRPWM takes it at the end of its period. The 15 kHz period is 66,664 ns, the nearest multiple of 4 ns, on the ICSSG0 slices.

**A and B.** On P8.11/P8.12 and on P9.12/P9.15, B is not simply A inverted. In a period of `P` ns with A high for `D` ns and a dead band of `d` ns (1000 ns):

![Captured at 20 kHz and 25% duty: A, B, and the dead band between them on both slices, and P8.15 and P9.25 at their own phase](media/pwm-pair.svg)

- A rises 40 ns into each period (an IEP compare at count 0 never fires, so both slices start there) and is high for `D`.
- B rises `d` after A falls, and falls `d` before the period ends, so both pins are low for `d` after A's pulse and for `d` + 40 ns before the next one.
- B's high time is `P − D − 2d − 40`: at 20 kHz and 25% duty, A is high 12,500 ns and B 35,460 ns. The `pwm` window prints both.
- P8.15 and P9.25 run the same `P` and `D` as A, each from a counter of its own, so their periods start wherever their counters do. They have no B.

That is what a half bridge's gate drive needs: the two switches are never on together. The software slice's edges wander by a few tens of ns, the time PRU0 takes to see IEP0 pass an edge time; the hardware slice's edges are exact to the 4 ns IEP count. Over the 96 periods of one 20 kHz, 25% step at 250 MS/s, slice 0's A was high 12,476 to 12,524 ns and its dead band 988 to 1,020 ns; slice 1's were 12,496 to 12,500 ns and 1,000 to 1,004 ns, within the analyzer's 4 ns sample.

P9.25's period was 8 ns long at every step, 50,008 ns at 20 kHz, in this capture: one count of EHRPWM4's 125 MHz clock. Zephyr's EHRPWM driver wrote the period's count to TBPRD, but counting up the EHRPWM's period is TBPRD + 1 counts. `zephyr/patches/` now fixes that, and P9.25's period matches the ICSSG0 generators' to within 0.04 ns at all three frequencies.

**P8.17** changes every 4 ns, the ICSSG core clock, so it needs an analyzer at 500 MS/s or more to resolve; built with `MCS_STEP_NS=1000 MCS_PERIOD_NS=10000` ([The multicore scheduler](#icssg1-the-multicore-scheduler)) it is slow enough for any.

![Captured at 500 MS/s: P8.17 over two 100 ns periods, low from 8 to 12 ns and from 16 to 24 ns of each, with the core whose task each compare starts](media/p817.svg)

At 500 MS/s the capture resolves the schedule to the analyzer's 2 ns sample: 10.000 million periods a second, each low for 4 and then 8 ns.

**SPI7** sends `hello` once at startup, and whatever `spi` asks for after that. Chip select stays low for the whole transfer, 175 µs for `hello`; the clock starts 3.9 µs after it falls.

![Captured at 50 MS/s: SPI7's first byte, h = 0x68: CS0 low, eight 1 µs clocks, MOSI 0 1 1 0 1 0 0 0](media/spi7.svg)

An earlier capture showed a 0.96 µs clock: the devicetree said SPI7's clock was 48 MHz, so the driver divided by 48 for 1 MHz, but on this board it is 50 MHz.

**P8.18** toggles every 500 µs from a periodic `k_timer`, so its timing is the R5F kernel's: half periods of 500 µs, within a few µs, with now and then one up to 20 µs off, when another interrupt holds the timer's up. R5F1_0 measures the same thing itself on the GTC, and its `status` line reports it.

![Captured: 20,036 half periods of P8.18 over 10 s, 99.83% within 2.5 µs of 500, the extremes 480.6 and 518.4 µs](media/p818.svg)

Before `zephyr/patches/` fixed Zephyr's DMTimer system timer, one half period in 17 was a whole tick, 94 µs, short: the driver set each next timeout from the moment it ran, not from the tick, so each wakeup was a few µs late, and the lateness added up until the kernel counted two ticks at once.

The test bench the demo was developed on: the BeagleBone AI-64 under a screw-terminal breakout on headers P8 and P9, 5 V from a bench supply, Ethernet, logic analyzer probes on the demo's pins, and an SD card mux in the microSD slot (teal, right) so the PC can reflash the card. To run the demo you need only the board, a microSD card, a 5 V supply, and a network link.

![The BeagleBone AI-64 on the test bench, with a screw-terminal breakout, a bench power supply, logic analyzer probes, and an SD card mux](media/test-bench.jpg)

`PINMUX.md` lists every pin the demo muxes, plus the shorted partner balls it holds off.

## How it works

### Linux to Zephyr: eRPC over RPMsg

- The R5F image opens the RPMsg channel `rpmsg-raw` at address 1024, which Linux sees as `/dev/rpmsgN`.
- `zephyr/erpc/ai64.erpc` defines the functions Linux can call on Zephyr: `say`, `status`, `spiWrite`, `setPwm`, `setPwmSteps`, `takeChunk`, and `dmaCopy`. The generated client and server code is in `zephyr/erpc/shim/`; `zephyr/erpc/regen.sh` regenerates it after you change the IDL.
- Both sides run eRPC's framed transport straight over the RPMsg file descriptor (`linux-app/ai64_erpc_linux.cpp` and `zephyr/r5/src/ai64_erpc.cpp`).

In split mode a cluster's core 1 cannot power up before its core 0, and core 0 cannot power down before core 1. `ai64-demo` starts R5F0_0, R5F1_0, R5F0_1, R5F1_1 in that order and stops them in reverse.

### One Zephyr app, four cores

A Cortex-R5F has no cache coherence with another R5F, so Zephyr cannot run one SMP kernel across them (its 32-bit Arm SMP support is for ARMv8-R and ARMv8-A cores). The way Zephyr does several cores is AMP: a kernel on each, built together with sysbuild, talking through `ipc_service`. That is what `zephyr/r5` is:

- **One build.** `west build --sysbuild -b beaglebone_ai64/j721e/main_r5f0_0 zephyr/r5` builds the app for R5F0_0, and `zephyr/r5/sysbuild.cmake` adds the same app for the other three boards, so one command makes all four ELFs (`build/zephyr/r5/zephyr/ai64_r5f0_0.elf`, `build/zephyr/r5f0_1/zephyr/ai64_r5f0_1.elf`, and so on). `beagle-linux/scripts/build.sh zephyr` runs it in the container.
- **A job each.** `zephyr/r5/Kconfig` has a choice of job, and each core's `zephyr/r5/boards/<board>.conf` picks one; `CMakeLists.txt` builds that job's file, and the core's overlay adds its devices. R5F0_0: SPI7 (`ai64_spi.c`), the bulk buffer, and the UDMA (`ai64_dma.c`). R5F0_1: EHRPWM4 on P9.25 (`ai64_pwm.c`). R5F1_0: a square wave on P8.18 (`ai64_blink.c`). R5F1_1: the monitor (`ai64_monitor.c`).
- **The rest is common.** Every core answers Linux over eRPC, sits on the fabric, makes its TI-SCI calls, sets up its MPU, and links to the other three.

Linux still loads and starts each core with remoteproc, and `ai64-demo` still restarts them one at a time. The board files are upstream Zephyr's `beaglebone_ai64/j721e/main_r5f0_0` (and `main_r5f0_1`, `main_r5f1_0`, `main_r5f1_1`), at the Zephyr `main` commit pinned in `zephyr/west.yml`; the cores without a console UART send their log to a RAM console that Linux reads as a trace. `zephyr/patches/` holds five fixes to Zephyr itself, which `zephyr/scripts/apply-patches.sh` applies after each `west update`: two to the EHRPWM driver ([EHRPWM4](#zephyr-ehrpwm4-on-p925)), two to the R5F timer driver ([P8.18](#probe-the-pins)), and the TI-SCI ring configuration call the UDMA needs, which Zephyr declares but never implemented ([DMA](#r5f0_0s-dma-the-udma)).

**Asking another core.** `spiWrite`, `setPwm`, and `setPwmSteps` work on any core. A core whose job it is not sends the request to the core whose job it is (`ai64_service.c`), waits for its answer, and returns it; so `r5f1_0 spi` comes back as `passed spi to R5F0_0, which wrote 10 bytes (323 us)`, the time on R5F1_0's side. A fabric route's `pwm` or `steps` action goes to `r5f0_1`.

### R5F to R5F: ipc_service

`zephyr/r5/boards/ai64_ipc.dtsi` gives each core a link to each other one, an instance of `ipc_service`'s icmsg backend: a 4 KiB buffer each way in shared DDR, and a mailbox FIFO each way as the doorbell. `zephyr/r5/src/ai64_ipc.c` opens the three links, sends a request as a small struct, and has a thread answer requests in order, so a slow job holds up nothing else. Each link uses icmsg's session handshake (`unbound = "enable"`): when a core restarts, it opens its links with a new session number, the others get an unbound callback, and they open the link again, so the links come back by themselves.

The buffers are in the 1 MiB of DDR the Linux overlay reserves at `0xa9000000`, past the fabric, and every core's MPU maps them non-cacheable. The doorbells needed some care:

- **Which mailboxes.** After boot, the MCU R5F's firmware, which runs the device manager, listens on mailbox cluster 7 and leaves messages in clusters 5, 6, 8, and 9. Writing doorbells into cluster 7 stopped the device manager answering anyone, until a power cycle. Clusters 10 and 11 have nobody listening; the fabric polls cluster 11's FIFOs 0 to 9.
- **Which users.** Only users 1 to 3 of a cluster reach the interrupt router (TI-SCI's `pend_intr` 0 to 2). So R5F0_1, R5F1_0, and R5F1_1 are users 1 to 3 of cluster 10, and R5F0_0 is user 1 of cluster 11, on its FIFOs 12 to 14. Each core routes its user to VIM 229, as for Linux's doorbell above:

| Core | Doorbells | Router 213 input | Router output | VIM input |
| --- | --- | --- | --- | --- |
| R5F0_0 | `NAVSS0_MAILBOX_11` user 1 | 393 | 197 | 229 |
| R5F0_1 | `NAVSS0_MAILBOX_10` user 1 | 397 | 229 | 229 |
| R5F1_0 | `NAVSS0_MAILBOX_10` user 2 | 398 | 261 | 229 |
| R5F1_1 | `NAVSS0_MAILBOX_10` user 3 | 399 | 293 | 229 |

- **Restarts.** A mailbox is outside the core, so its interrupt enables survive a restart, and the others keep ringing meanwhile. Zephyr's mailbox driver skips a FIFO it has not enabled yet without reading it, so a stale one held the interrupt asserted and the restarted core did nothing but take it. `ai64_restart.c` clears the core's enables at boot.

R5F1_1's monitor pings the other three every 100 ms: about 80 µs a round trip, through the peer's icmsg work queue and its request thread and back.

### Zephyr and the device manager: TI-SCI

The board's device manager, TI's firmware on the MCU R5F, owns the SoC's clocks, power domains, and interrupt routers; every other core asks it over TI-SCI. Zephyr has a TI-SCI driver (`drivers/firmware/tisci`), and on top of it a clock-control driver and a power-domain driver, but it talks to the device manager through a secure-proxy mailbox driver that needs a receive interrupt for each thread and handles threads 0 to 31. On J721E no main secure-proxy interrupt is routed to a main-domain R5F, and these cores' threads are numbered past 31.

`zephyr/r5/src/ai64_sproxy.c` is a mailbox driver for the same secure proxy (`ai64,secure-proxy-poll`, in `zephyr/r5/dts/bindings/`) with no interrupt. A send writes the message to the core's transmit thread, then waits for the answer on its receive thread and hands it to the TI-SCI driver's callback before it returns, which is when the TI-SCI driver starts to wait for it. It waits a fixed number of polls rather than a time, so it also works before the system timer runs. Each core's overlay names its threads and TI-SCI host:

| Core | TI-SCI host | Transmit thread | Receive thread |
| --- | --- | --- | --- |
| R5F0_0 | 35 | 63 | 61 |
| R5F0_1 | 37 | 73 | 71 |
| R5F1_0 | 40 | 83 | 81 |
| R5F1_1 | 42 | 93 | 91 |

With that, Zephyr's own TI-SCI drivers work on these cores unchanged. Every core's mailbox interrupt routes use them (below), and so do R5F0_1's EHRPWM4, R5F1_0's GPIO2, and R5F0_0's UDMA channel.

### Linux to Zephyr: the mailbox interrupt

RPMsg rings sit in shared DDR. Each side tells the other "look at the rings" through a TI mailbox FIFO; each R5F core has its own pair of FIFOs. Zephyr's kick to Linux works as is. Linux's kick to Zephyr arrives in the FIFO, but on J721E the mailbox interrupt only reaches the R5F once the device manager routes it through the NAVSS interrupt router, and the upstream board does not ask for that.

So each R5F core asks at boot (`zephyr/r5/src/ai64_tisci.c`), with `tisci_cmd_rm_irq_set()`. The request names the router as both source and destination, with a router input and an output from the core's own range, the way Linux's `ti,sci-intr` and Zephyr PR [#121379](https://github.com/zephyrproject-rtos/zephyr/pull/121379) program it. First it releases the route a previous run left; the device manager refuses that when there was none. Each core's numbers are in its board overlay:

| Core | Mailbox user | Router 213 input | Router output | VIM input |
| --- | --- | --- | --- | --- |
| R5F0_0 | `NAVSS0_MAILBOX_1` user 2 | 434 | 196 | 228 |
| R5F0_1 | `NAVSS0_MAILBOX_1` user 1 | 433 | 228 | 228 |
| R5F1_0 | `NAVSS0_MAILBOX_2` user 1 | 429 | 260 | 228 |
| R5F1_1 | `NAVSS0_MAILBOX_2` user 2 | 430 | 292 | 228 |

The overlays also move the board's mailbox interrupt from VIM 256 to 228. If the request fails, the core logs it and its RPMsg thread polls the FIFO once a millisecond instead (`mbox_rx_drain()` in `zephyr/r5/src/main.c`).

### Zephyr: EHRPWM4 on P9.25

P9.25's ball W26 can be `EHRPWM4_B`, output B of the SoC's EHRPWM4. Zephyr's `ti,am3352-ehrpwm` driver drives it on R5F0_1, and `zephyr/r5/src/ai64_pwm.c` sets it through the PWM API, on the `pwms` of `/zephyr,user`. At boot it starts the 15 steps, 5 ms a step, from a delayable work item on the GTC's time like the ICSSG0 slices' steps; after that it does whatever `setPwm` or `setPwmSteps` over eRPC, or a fabric route's `pwm` or `steps` action, asks for. `pwm_set()` would ask the device manager for EHRPWM4's clock rate over TI-SCI on every call, about 60 µs, so `ai64_pwm.c` asks once (`pwm_get_cycles_per_sec()`) and then calls `pwm_set_cycles()`. Steps keep time only if Zephyr's clock does: `zephyr/patches/0002` fixes the R5F timer driver, which lost the part of a tick left over at each timer interrupt, so Zephyr's clock ran about 1.6% slow on these cores; `0003` sets each tickless timeout from the tick rather than from the moment the driver runs. R5F0_1's overlay describes the rest:

- **Its clock**, through TI-SCI clock control: device 87, clock 0, the same as the Linux device tree's `main_ehrpwm4`.
- **Its power**, through a TI-SCI power domain for device 87, which turns the module on at boot (`CONFIG_POWER_DOMAIN`).
- **Its time-base clock gate**, bit 0 of `CTRLMMR_EPWM4_CTRL` at `0x00104150` (Linux's `ehrpwm_tbclk` 4), through a `ti,control-module` syscon. Zephyr's SoC code unlocks its CTRL_MMR partition at boot.
- **The pins**: W26 in mode 6, and AC4, shorted to it on the header, held off.

The stock Debian image gives EHRPWM4 and W26 to Linux with `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`; `install-beagle.sh` takes that overlay off, so that only Zephyr uses them.

The driver had one bug on this board: it handed `clock_control_get_rate()` its `uint64_t` rate as a `uint32_t *`, which leaves the upper half unset, and `pwm_set()` then saw a clock rate in the billions and failed with `-ENOTSUP`. `zephyr/patches/0001-drivers-pwm-ti_am3352_ehrpwm-set-all-64-bits-of-the-rate.patch` fixes it. It also made every period one count long, writing the period's count to TBPRD where counting up the period is TBPRD + 1 counts; `0004` fixes that ([Probe the pins](#probe-the-pins)).

Every 10 ms R5F0_1's fabric thread publishes the PWM's `struct ai64_pwm_status` (`zephyr/include/ai64_shm.h`) in its fabric page, as the ICSSG0 RTUs do for their slices, and the `pwm` window shows it with the other three.

### R5F1_0: P8.18 from GPIO2

P8.18's ball AJ23 is `GPIO0_4` in mux mode 7. On J721E, bits 5:4 of a pad register pick which of the main domain's GPIO0, GPIO2, GPIO4, and GPIO6 drives the pad. Linux owns GPIO0 and has outputs in every one of its banks, and Zephyr's GPIO driver sets every pin of a bank to input when it starts, so R5F1_0 takes GPIO2, which Linux leaves disabled: its overlay sets bit 4 on the pad, asks the device manager for GPIO2's power (device 107) with a TI-SCI power domain, and enables upstream Zephyr's `main_gpio2_0`. `ai64_blink.c` toggles the pin from a periodic `k_timer` and times each half period on the GTC.

### R5F0_0's DMA: the UDMA

J721E moves data with the main NAVSS's UDMA, and Zephyr has no driver for it. `zephyr/r5/src/ai64_dma.c` does for one memory-to-memory channel what Linux's and U-Boot's `k3-udma` drivers do, polled:

1. **Ask the device manager**, over TI-SCI, for this host's ranges of UDMAP transmit and receive channels and of general-purpose rings (`tisci_cmd_get_resource_range()`); R5F0_0 gets channel 44 and ring 744.
2. **Configure**: the channel's transmit ring (ring 44) and a completion ring (744), 16 eight-byte elements each, in normal non-cacheable DDR; the transmit and receive channels as block-copy channels (TI-SCI channel type 12) that return finished descriptors to the completion ring; and the PSI-L pairing of transmit thread `0x1000 + 44` to receive thread `0x9000 + 44`. Then enable both channels.
3. **Copy**: one CPPI5 TR descriptor with one type-15 TR, source and destination address and a length up to 64 KiB. Write its address into the transmit ring and ring the ring's doorbell; poll the completion ring's occupancy until the descriptor comes back, then pop it.

Linux calls it with `dmaCopy` over eRPC, which only copies from the bulk buffer into a DSP's job block. `r5f0_0 dma` uses it to put an int8 layer's input into the C7x's L2 SRAM, 16 KiB in about 34 µs, then has the C7x run the layer on its MMA ([below](#the-c7xs-matrix-multiply-accelerator)).

Two things on the way: Zephyr declares `tisci_cmd_ring_config()` but never implemented it (`zephyr/patches/0005` does), and the channel's realtime registers must not be touched before TI-SCI has configured the channel for this host. One early write there, to switch off a channel a previous run might have left on, was refused; the abort it left pending kept the core from ever reaching WFI, so Linux could no longer stop it (`can't stop rproc: -110`). Instead the core switches the channel off itself when Linux asks it to shut down.

### R5F caches

The J721E SoC code maps all of DDR as non-cacheable for the R5F, and leaves both caches off unless `CONFIG_CACHE_MANAGEMENT` is set. The image runs from DDR, so every instruction fetch went to DDR, and an eRPC call took about 1.3 ms.

`zephyr/r5/src/ai64_mpu.c` adds MPU regions at the first init level, before the caches come on: device memory for the peripherals in the low 2 GiB, the TCMs, and the core's 16 MiB carveout as write-through, so that every write still reaches DDR. The vrings, the resource table, the bulk buffer, the fabric, and the `ipc_service` buffers and DMA rings stay non-cacheable. `prj.conf` turns on `CONFIG_CACHE_MANAGEMENT`.

`ai64 bench <rpmsg-dev> [count]` times eRPC calls. On the bench, an eRPC `say` round trip went from 1263 µs to 85 µs on R5F0_1, R5F1_0, and R5F1_1, and to 160 µs on R5F0_0, which also logs each call on its UART. For comparison, an RPMsg echo from RTU0, which busy-polls, takes 35 µs.

### Linux to Zephyr: the bulk buffer

`r5f0_0 chunk` passes data too large for an RPC argument:
1. Linux writes the bytes into a 64 KiB DDR buffer at `0xA2FEF000` through `/dev/mem`.
2. Linux calls `takeChunk(seq, length, crc)`.
3. Zephyr checks the CRC.

The buffer sits inside the memory Linux already reserves for R5F0_0 (`0xA2000000`-`0xA2FFFFFF`). `zephyr/r5/boards/beaglebone_ai64_j721e_main_r5f0_0.overlay` shrinks the board's DRAM region so that no part of the Zephyr image lands there. The layout is in `zephyr/include/ai64_shm.h`, which Linux, Zephyr, and the ICSSG cores include.

### Linux to RTU0: RPMsg text

- ICSSG0 RTU0 announces RPMsg channel `rpmsg-raw` on port 30.
- Linux writes plain text: `pwm <period_ns> <duty_ns>`, `steps <ms>`, `sweep <ms>`, `dead <ns>`, or `status`.
- RTU0 replies with one sentence that names who asked and what it did.
- RTU0 learns Linux's address from the first message it receives, so Linux sends `ping` first. RTU0 can't report anything that a fabric route asks it to do until Linux has sent that first message.
- The code is `pru/common/ai64_rtu_rpmsg.h` and the resource table in `pru/common/ai64_rsc_rpmsg.h`.

### The ICSSG firmware

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
- **The trace log is the exception.** Every core keeps a remoteproc trace buffer in its own data RAM, which Linux reads (`ai64 trace <remoteproc-name>`); writing a line is a few local stores. `TRACE=0 ./beagle-linux/scripts/build.sh pru` builds the trace calls out of the PRUs and Tx_PRUs. The RTUs always keep theirs.
- **One RTU talks over RPMsg and one over shared memory only**, so the template has a working example of each. RPMsg needs a vring interrupt in the overlay and a few KiB of data RAM. Both RTUs are fabric nodes, and both publish what they know in their fabric page.

ICSSG1 keeps TI's design instead: every core runs a task, and nothing on it talks. `ai64 sched` reads its registers, and only reads them.

### ICSSG0: PWM, three ways

Both slices make the same waveform, from the same request, with the same modulator. Only the PRU differs. A third way copies slice 0's A, with the ICSSG's eCAP. [`pru/pwm/README.md`](pru/pwm/README.md) has the details: the memory map, the four IPC records, and how each core uses them.

| | Slice 0: in software | Slice 1: the ICSSG PWM module |
| --- | --- | --- |
| Pins | A on P8.11, B on P8.12, R30 bits 17 and 16 | A on P9.12 (`PRG0_PWM2_A0`), B on P9.15 (`PRG0_PWM2_B0`) |
| What makes an edge | PRU0 writes R30 when IEP0, polled, reaches the edge's time | IEP1 CMP1 toggles A, CMP2 toggles B; CMP0 ends the period |
| What the PRU does | waits for every edge | after each compare, rearms it for that pin's next edge |
| Timing | within a few tens of nanoseconds; PRU0 counts the edges it was more than 100 ns late for | exact, on the 250 MHz IEP clock |

The waveform is A high for `D` from 40 ns into a period of `P`, and B high from `D + d` after that until `d` before the period ends: B is A's complement with dead band `d` on each side ([drawn here](#probe-the-pins)). Both start with a 1 µs dead band, stepping through 10, 15, and 20 kHz, and at each through 10, 25, 50, 75, and 90% duty, 5 ms a step on the GTC's time, so they change step with each other and with P9.25. The Tx_PRU of each slice makes the steps, sweeps the duty up and down every `sweep_ms` instead, or passes a fixed duty through.

The third way is ICSSG0's eCAP0 in APWM mode, on P8.15. It is a PWM generator in its own right: a 32-bit counter on the 250 MHz core clock that counts to a period and holds its pin high below a compare value, with shadow registers that load at a period's end. Slice 0's Tx_PRU gives it the period and high time it gives PRU0, during the eCAP period whose end is nearest the time A changes, and does nothing else with it. It has one output, so no B and no dead band, and its counter is not in phase with IEP0. [`pru/pwm/README.md`](pru/pwm/README.md#the-cores) has the details.

Two things about the J721E's PWM module took finding out on the board. An IEP compare only fires again once software clears its status bit (AM64x's ICSSG has `PWM_EFC_EN` to do that, and J721E's does not), and in the active state an output only toggles: its active level does nothing, and CMP0 does not bring it back. So each pin has one compare, which PRU1 moves between that pin's rise and its fall as soon as it fires; TI's AM65x PWM firmware works the same way. And the set's trip inputs must be masked: unmasked and unconnected, they keep the set in its trip state, and its pins at Z.

`ai64 pwm` shows both slices running about 15,000 periods a second while they step (10, 15, and 20 kHz, a third of the time each), 20,000 at their fixed default of 20 kHz after `steps 0`, and 50,000 after `fabric route ... g0rtu1 pwm 20000 5000`. With both RTUs and the whole fabric running, the software slice's latest edge was 24 ns late, and neither slice counted a late edge in 25 restarts of the demo.

### ICSSG1: the multicore scheduler

[`pru/multicore_scheduler/`](pru/multicore_scheduler/README.md) is TI's open-pru example `multicore_scheduler`, ported from ICSSG0 of the AM243x LaunchPad to ICSSG1 here. IEP0's CMP0 resets the counter every 100 ns, and on each 4 ns step after it one more compare fires and starts one core's task through that core's task manager: PRU0, PRU1, RTU1, RTU0, Tx_PRU1, Tx_PRU0. Each task sets PWM set 2's initial state high or low; PWM2 runs on IEP1, which nothing starts, so P8.17 (`PRG1_PWM2_A0`) follows it.

The port keeps TI's files, names, and timing. What J721E needed: no clock or pin setup in PRU0, since Linux has done both; and, with no `PWM_EFC_EN`, the compare flags cleared in software. Each task clears its own, each core clears its own once its task manager is on, and PRU0 maps every task's event before it starts IEP0, and clears all six flags once more two periods later. Without those, a compare that fires before its core's task manager sees it leaves a flag nothing clears, and that core's task never runs again. `MCS_STEP_NS` and `MCS_PERIOD_NS`, on the make command line, change the schedule.

`ai64 sched` reads PWM2_0's state over the bus, much slower than the tasks change it, and finds P8.17 high about 79% of the time, against 88% for the schedule. A logic analyzer at 500 MS/s or more shows the schedule itself: a 100 ns period, low from 8 to 12 ns and from 16 to 24 ns, high the rest of the time ([Probe the pins](#probe-the-pins)). With `MCS_STEP_NS=1000 MCS_PERIOD_NS=10000`, any analyzer does.

### Every core to every other: the fabric

Every link above goes through Linux, or joins two cores that sit next to each other. The fabric joins every demo core that talks directly to every other one, Linux included: ten nodes, 90 one-way paths, and no Linux in any of them. One header, `zephyr/include/ai64_fabric.h`, holds the layout and the whole protocol, and every image compiles it: Zephyr with GCC, the DSPs with TI's C6000 and C7000 compilers, the RTUs with clpru, and `ai64` with the Linux compiler. Each image only adds a few lines of glue: how to read the clock, and how it maps the memory.

| Node | Name | Core | When it looks at its inbox |
| --- | --- | --- | --- |
| 0 | `linux` | A72, while `ai64 fabric` runs | every 20 µs or so, between sleeps |
| 1–4 | `r5f0_0`, `r5f0_1`, `r5f1_0`, `r5f1_1` | Zephyr (`zephyr/r5/src/ai64_fabric.c`) | in a thread at the lowest priority, which yields after each pass |
| 5, 6 | `c66x0`, `c66x1` | C66x | every pass of its main loop |
| 7 | `c7x` | C7x | every pass of its main loop |
| 8 | `g0rtu0` | ICSSG0 RTU0 (`pru/pwm/firmware/pwm_rtu.c`) | every pass of its main loop, next to its RPMsg check |
| 9 | `g0rtu1` | ICSSG0 RTU1 (the same source) | every pass of its main loop |

The ICSSG0 PRUs and Tx_PRUs are not nodes: each slice's RTU talks for them ([The ICSSG firmware](#the-icssg-firmware)). Nor is ICSSG1, whose six cores all run scheduler tasks. The layout has room for twelve nodes (`AI64_FAB_SLOTS`), so two more can join without moving anything.

**What carries it.** Four pieces of SoC hardware that every one of those cores can reach:

- **DDR**, 128 KiB at `0xA9000000`, in the gap between the C7x's carveout and the IPC region above it. The overlay reserves 1 MiB there (`ai64-fabric@a9000000`, no-map) so that Linux keeps out, and `ai64-demo` refuses to start the cores on a kernel that booted without it. Each node owns an 8 KiB page: its inbox, and the counts it publishes for Linux to read. A spinlock tally follows the pages.
- **Mailbox cluster 11** (`0x31F8B000`) for doorbells. Linux's device tree leaves clusters 5–11 disabled; 5, 6, 8, and 9 hold messages from the device manager's firmware, and 11 is empty. FIFO n is node n's doorbell.
- **The GTC** (`0x00A90000`), the 200 MHz counter Linux's arch timer counts too. Every core reads the same clock, so a latency measured from one core's stamp to another's is real, one way.
- **Hardware spinlock 255** (`0x30E00000`), which no kernel driver uses, for the tally.

**Read your own page, write to the others.** An uncached read costs 0.1 to 0.5 µs, and a write is posted and costs almost nothing. So a node only ever reads its own page. A message goes into the receiver's page, and the stamps a tour collects go straight into its origin's page. The sender then writes its own node number into the receiver's mailbox FIFO, and the receiver, which polls only its FIFO's status register, reads its page when something has arrived. A FIFO holds four doorbells; a sender that finds it full skips the doorbell, and every node also scans its whole inbox once a millisecond.

**A message.** Each inbox has one 64-byte slot for every sender and message class, and only that sender writes it. The sender writes the message, then bumps its count for that class in a word of the receiver's page that only it writes, then rings. The receiver remembers the counts it last saw, so a changed count is a new message, and nothing is ever cleared. Every class allows one message in flight per sender and receiver, so a slot is never overwritten before it is read, and no core ever waits on another:

| Class | Sent by | What it does |
| --- | --- | --- |
| ping | every node, to every other, every 10 ms | carries the sender's GTC stamp and a value. The receiver stamps when it has read it, mixes its node number into the value, and sends it back. |
| pong | the receiver of a ping | the sender checks the value and times the round trip. The receiver's stamp minus the sender's is the one-way latency. A ping with no pong in 50 ms is a timeout, and that peer counts as down until a later ping comes back. |
| tour | R5F0_0, every 10 ms | goes round every node whose pings R5F0_0 sees answered, Linux included while its viewer runs, in an order that keeps changing the kind of core: c66x0, g0rtu0, r5f1_0, c7x, r5f0_1, c66x1, g0rtu1, r5f1_1, linux, and home. Each hop stamps it and mixes its number into the value, and R5F0_0 checks the value when the tour comes back. |
| route | `ai64 fabric route`, from Linux | the same as a tour, along the nodes you name and back to Linux. It can carry an action for the first PWM generator on the way: `pwm <period_ns> <duty_ns>` or `steps <ms>` for ICSSG0 RTU0 or RTU1, or R5F0_1's EHRPWM4, or `sweep <ms>` or `dead <ns>` for either RTU. Its count lives in a word of its own, so routes work while the viewer, also node 0, runs. |

A node that starts takes the counts it finds as already seen, and zeroes its own counters. A message sent to a stopped node is simply lost, and its sender times out, so `ai64-demo` can restart any core while the others keep going.

**The spinlock tally.** Every 10 ms each node takes hardware spinlock 255, adds one to a shared total and one to its own count, and frees the lock. The adds are plain loads and stores to uncached DDR, so without the lock two cores would lose each other's adds. The viewer takes the lock too, and checks that the ten counts sum to the total.

**Each core's view of the memory.**

| Core | The fabric | The GTC, mailbox, and spinlock |
| --- | --- | --- |
| R5F | MPU region 7, normal non-cacheable (`ai64_mpu.c`) | in the low 2 GiB the MPU maps as device memory |
| C66x | MAR register for `0xA9xxxxxx` cleared, which turns off the XMC prefetch it resets with | the mailbox and spinlock directly. The GTC, below `0x0C000000`, is the C66x's own internal space, so MPAX segment 2 maps a logical page at `0x7FFFF000` onto it (`c66x/main.c`). |
| C7x | 32 level-3 MMU pages, normal non-cacheable (`ai64_c7x_mmu.c`) | the device-memory GiB at `0` |
| RTU | its global address; a message moves in one or two `LBBO`/`SBBO` bursts | the same |
| Linux | `/dev/mem` | `/dev/mem` |

**What you see.** The `fabric` window, `ai64 fabric`, redraws once a second. On the Yocto image, with every core running:

```
fabric: one-way latency in us, from the row's node to the column's, over the last 1.0 s
         linux r5f0_0 r5f0_1 r5f1_0 r5f1_1  c66x0  c66x1    c7x g0rtu0 g0rtu1
linux        .    4.2    4.5    3.9    4.4    5.2    3.6    2.6    3.5    3.3
r5f0_0    45.4      .    4.5    4.6    4.8    5.8    4.0    3.2    3.7    3.8
r5f0_1    40.7    5.1      .    5.0    5.4    5.8    4.3    3.3    4.0    3.9
r5f1_0    46.1    5.1    5.2      .    5.3    5.9    4.4    3.2    4.0    4.3
r5f1_1    45.6    5.4    5.2    5.2      .    5.9    4.5    3.4    4.1    4.1
c66x0     46.7    4.2    4.4    4.5    4.3      .    3.6    2.6    3.1    3.3
c66x1     41.3    4.2    3.8    4.0    4.3    5.5      .    2.5    3.3    3.1
c7x       36.7    4.3    3.7    3.7    4.1    5.2    3.0      .    2.9    2.8
g0rtu0    39.1    4.3    4.1    4.3    4.5    5.3    3.9    2.7      .    3.4
g0rtu1    38.2    4.3    4.3    4.4    4.5    5.5    3.8    2.8    3.3      .

node    starts   polls/s  pongs/s    rtt us  rtt min to max us timeouts errors  passed/s
linux       32     13238      900      76.8     58.4 to  192.9        0      0       100
r5f0_0      20    338874      900      14.7      5.2 to   82.6        0      0         0
r5f0_1      20    393392      900      12.7      5.0 to   84.7        0      0       100
r5f1_0      20    392984      900      13.5      5.0 to   83.2        0      0       100
r5f1_1      20    392637      900      13.6      5.1 to   82.7        0      0       100
c66x0       17   1155155      900      13.7      4.9 to   83.3        0      0       100
c66x1       17   1176422      900      11.5      4.4 to   80.7        0      0       100
c7x         17   1041957      900      10.1      4.3 to   78.3        0      0       100
g0rtu0      19    587503      900      11.3      4.6 to   83.2        0      0       100
g0rtu1      19    588491      900      11.3      4.3 to   80.0        0      0       100

tour from r5f0_0 round every node that answers its pings: 100 laps/s, 103.2 us a lap (64.0 to 138.4), errors 0, timeouts 5
  r5f0_0 >16.4> c66x0 >4.3> g0rtu0 >5.3> r5f1_0 >4.3> c7x >5.6> r5f0_1 >9.9> c66x1 >3.8> g0rtu1 >6.9> r5f1_1 >32.7> linux >4.3> r5f0_0

tally under hardware spinlock 255: 25908 adds, and the 10 counts sum to 25908
```

- **The matrix.** Between any two of the R5Fs, DSPs, and RTUs a message takes 2.5 to 6 µs, reading it included. The C7x, whose main loop is shortest, is the quickest to notice one. Linux sleeps between looks, so a message to it takes about 35 to 50 µs.
- **The nodes.** `starts` counts each node's starts since the board booted. `rtt us` is the mean round trip to all of the node's peers, which Linux pulls up; the minimum and maximum are the fastest and slowest single round trips in the last second. `passed/s` is tour hops, one per node per lap.
- **The tour.** Each `>µs>` is one hop, from the last node reading the message to the next reading it. R5F0_0 started before the DSPs and the ICSSG cores in that `ai64-demo` run, so its first tours ran into cores that were restarting: those are the 5 timeouts.
- **The tally.** Every add from all ten nodes is in the total, so the lock held.

The merged console prints one `[Fabric]` line every 10 s instead (`ai64 fabric lines`).

**A route with an action.** On the board, while the demo runs:

```
$ sudo ai64 fabric route c7x r5f1_1 g0rtu1 c66x1 pwm 20000 5000
linux  -> c7x         8.3 us
c7x    -> r5f1_1      4.5 us
r5f1_1 -> g0rtu1      5.3 us   g0rtu1 set its PWM
g0rtu1 -> c66x1      39.7 us
c66x1  -> linux       1.7 us
5 hops, 59.6 us, value ok
```

ICSSG0 slice 1's PWM, on P9.12 and P9.15, then runs at 50 kHz, a fixed 25%, and RTU1 logs when PRU1 follows (`ai64 trace b006000.rtu`). The hop after an action is slower because the RTU builds and logs a line of text first. The first PWM generator on the route takes the action, so a route through R5F0_1 sets P9.25:

```
$ sudo ai64 fabric route c66x0 r5f0_1 g0rtu0 pwm 500000 125000
linux  -> c66x0      11.7 us
c66x0  -> r5f0_1      6.0 us   r5f0_1 set its PWM
r5f0_1 -> g0rtu0     22.5 us
g0rtu0 -> linux       2.5 us
4 hops, 42.8 us, value ok
```

P9.25 then runs at 2 kHz, a fixed 25%. A route's `steps 5` starts the steps again, one generator per route: `sudo ai64 fabric route r5f0_1 steps 5` for P9.25 and `sudo ai64 fabric route g0rtu1 steps 5` for slice 1, and `rtu0 steps 5` in the `linux` window for slice 0. Any order of nodes works, a node can appear more than once, and a route without an action just goes round. `sudo ai64 fabric reset` frees the spinlock and zeroes the tally, for a core that was stopped while it held the lock; `ai64-demo` runs it before it starts the cores.

**Restarts.** `ai64-demo` ran 10 times in a row on the Debian image and 15 times on the Yocto image, then 5 more on a freshly built Yocto image, stopping and starting all 19 demo cores each time while the others kept talking. Every run came back with all ten nodes up, RTU0 answering over RPMsg, both ICSSG0 PWM slices running with no late edges, all six ICSSG1 tasks firing, no fabric timeouts or errors in the 10 s after, the tally agreeing, and no kernel errors. Routes with `pwm` actions changed the ICSSG PWM and P9.25 the same way on both.

**What it costs.** Every node except Linux now spins on its inbox, so no core sits in WFI any more. The ICSSG0 RTUs' polling does not show in their slices' edges: with both RTUs and the whole fabric running, the software slice's latest edge was 24 ns late. And a core that is always on the bus must not be stopped in the middle of an access: putting a busy R5F in reset on the BeagleBoard kernel wedged the device manager (`ti-sci ... Mbox timedout in resp(caller: k3_r5_rproc_stop ...)`), and the SoC locked up a few seconds later. TI's kernel sends each core `RP_MBOX_SHUTDOWN` first, and the Zephyr app parks in WFI with its interrupts masked; on the BeagleBoard kernel `ai64-demo` now sends it with `ai64 shutdown`, as it already did for the DSPs.

### Restarting an R5F core

The patched `ti_k3_r5_remoteproc` (below) stops a core by holding it in reset, without powering it off. Two things outlive that, and Zephyr's drivers expect a fresh power-up:

- **The VIM**, the core's interrupt controller. Taking an interrupt makes its priority level active until the handler ends it. A core stopped inside a handler leaves that level active, and the VIM then holds back every interrupt at that priority, the system timer's included. The core boots, logs a few lines, and stops dead.
- **The system timer**, a DMTIMER outside the core. It keeps counting, with a match interrupt pending from the last run.

`zephyr/r5/src/ai64_restart.c` runs before the system timer starts. It ends any interrupt left in service, clears the VIM's status, and stops the timer and clears its pending match. Without it, about one restart in six left a core hung, and after many restarts most of them were. With it, 30 restarts of all four cores in a row all came up.

### The DSPs: trace and jobs

Each DSP runs a small image from `dsp/`. It fills in a resource table with one entry, a trace buffer, writes `hello` there, and then logs one line every 10 seconds, timed with the core's cycle counter. When the buffer fills up, the log starts again after the hello line. Besides its own mailbox ([why](#restarting-the-c7x)), a DSP only touches what the [fabric](#every-core-to-every-other-the-fabric) uses. The C66x images set up no cache; the C7x image turns on its MMU and caches first ([below](#the-c7x-mmu-and-caches)).

| Core | remoteproc | Image | Where it runs | Boot address |
| --- | --- | --- | --- | --- |
| C66x 0 | `4d80800000.dsp` | `ai64-c66x.elf` | its own L2 SRAM, local `0x00800000` | `0x00800000` (1 KiB aligned) |
| C66x 1 | `4d81800000.dsp` | `ai64-c66x.elf` | its own L2 SRAM, local `0x00800000` | `0x00800000` |
| C7x | `64800000.dsp` | `ai64-c7x.elf` | its DDR carveout, `0xa8200000` | `0xa8200000` (2 MiB aligned) |

Both C66x cores run the same image: each sees its own L2 SRAM at `0x00800000`, and `DNUM` tells them apart. The C66x images run from L2 SRAM, and only reach DDR for the fabric, because the board's device tree crosses the two C66x DDR carveouts: `4d80800000.dsp` gets `memory@a7000000` for DMA but `memory@a6100000` for firmware, and `4d81800000.dsp` the other way round.

The compilers are TI's C6000 CGT 8.3.12 and C7000 CGT 1.2.0.STS, the newest versions TI serves without a login (`containers/dsp-cgt/`).

The windows read the trace buffers with `ai64 trace`. It finds the buffer from the resource table in the core's firmware file, and reads it through `/dev/mem`, so it needs neither root nor debugfs.

### Linux to the DSPs: FIR jobs

Each DSP polls a 64 KiB job block in its own L2 SRAM (`dsp/include/ai64_dsp_job.h`). Linux writes the filter taps, the input samples, and the job size through `/dev/mem`, then bumps a request number. The DSP runs a 16-bit fixed-point FIR filter, writes the output and its cycle count, and copies the request number into a done field. On a C66x, L1D snoops writes to L2 SRAM from outside, so neither side needs cache maintenance. The C7x maps the block's first 4 KiB, the header both sides poll, non-cacheable, and writes L1D back around each job.

| Core | Job block (Linux address) | Core's own address |
| --- | --- | --- |
| C66x 0 | `0x4d80838000` | `0x00838000` |
| C66x 1 | `0x4d81838000` | `0x00838000` |
| C7x | `0x64800000` | `0x64800000` |

`sudo ai64 dsp <remoteproc-name> [count] [taps] [runs]` sends jobs with random data, checks each output against the same filter run on the A72, and prints the timings:

```
$ sudo ai64 dsp 4d80800000.dsp
4d80800000.dsp: 20 FIR jobs, 4096 samples x 32 taps, all match the A72
  DSP filter 28222 cycles = 20.9 us; A72 waited 21.8 us; empty job 0.9 us
  the same filter on one A72 core: 188.1 us
```

```
$ sudo ai64 dsp 64800000.dsp
64800000.dsp: 20 FIR jobs, 4096 samples x 32 taps, all match the A72
  DSP filter 9177 cycles = 9.2 us; A72 waited 9.7 us; empty job 1.3 us
  the same filter on one A72 core: 199.0 us
```

| Core | 4096 samples × 32 taps | 8192 × 64 |
| --- | --- | --- |
| One A72 core, plain C at `-O2` | about 190 µs | about 790 µs |
| C66x, plain C at `-O3` (before) | 92 µs | 336 µs |
| C66x, `DDOTP4H` intrinsics | 21 µs | 72 µs |
| C7x, one tap per step (before) | 19 µs | 60 µs |
| C7x, `VDOTP2HW` on 128 outputs | 9.2 µs | 26 µs |

Both DSPs run their own path in `ai64_dsp_job.h`, with the plain C loop left for the last few outputs. Each keeps the taps outside and the outputs inside, so one tap load serves many outputs:

- **C66x.** One pass over the outputs per 8 taps, 8 outputs at a time, adding into a table of 32-bit sums. Four aligned 64-bit loads hold 16 samples; shifting them by one to three samples gives each output its 4 samples, and each `DDOTP4H` is two 4-tap dot products. The outputs go in chunks of 2048, so that a chunk's sums and samples stay in the 32 KiB L1D across the passes; without that, 8192 × 64 took 101 µs. With the taps inside instead, each group of 8 outputs paid for starting and ending the tap loop, and 4096 × 32 took 35 µs.
- **C7x.** 128 outputs at a time, two taps per step, with the sums in registers. `VDOTP2HW` multiplies each pair of samples by a pair of taps and adds the two products into one 32-bit lane. A 32-sample load from `x[n + k]` gives the even outputs their pairs for taps `k` and `k + 1`, and the load one sample on gives the odd outputs theirs. With 32 outputs a block, 4096 × 32 took 17.8 µs; 128 spreads the cost of the tap loop. The plain C loop took 225 µs: the compiler vectorizes across the taps, so a 32-tap filter is two vectors per output, and loop overhead costs more than the multiplies. The C7x reads its L2 SRAM much slower than cached DDR, so it copies the input into its own DDR first; without that copy the same job takes 38 µs.

`ai64 dsp` refuses a DSP that is not running. Reading a powered-off DSP's L2 SRAM hangs the SoC.

### The C7x MMU and caches

`dsp/c7x/ai64_c7x_mmu.c` builds identity-mapped page tables and turns the MMU on before anything else runs. It sizes the caches as TI's vision_apps does: L1D all cache (32 KiB), and 64 KiB of the 512 KiB L2 as cache, which leaves 448 KiB of SRAM from `0x64800000`.

| Region | Size | Attributes | Holds |
| --- | --- | --- | --- |
| `0x00000000` | 1 GiB | device | the mailbox and every other peripheral |
| `0x40000000` | 1 GiB | device, apart from the L2 SRAM pages below | |
| `0x64800000` | 4 KiB | normal, non-cacheable | the job block header |
| `0x64801000` | 60 KiB | normal, write-back | the rest of the job block |
| `0xa8000000` | 2 MiB | normal, non-cacheable | the DMA pool, then the trace buffer and resource table at `0xa8100000` |
| `0xa8200000` | 14 MiB | normal, write-back | code, data, stack, and the page tables |
| `0xa9000000` | 128 KiB | normal, non-cacheable | the fabric |

Anything else faults. The register values and their order follow the secure startup in TI's PDK (`ti/csl/arch/c7x/src/Mmu.c`), which flushes no cache and no TLB before the MMU goes on. One difference: the table base goes in `TBR0` as well as `TBR0_S`. With only `TBR0_S`, the core stops the moment the MMU is on, even though the core starts in secure supervisor mode.

The trace buffer and the resource table moved from the code region into the non-cacheable block, because Linux reads them while the core runs. The MMU alone took the plain C loop only from 225 to 194 µs. Almost all of the gain came from the vector code.

### The C7x's Matrix Multiply Accelerator

The TDA4VM's C7x has a Matrix Multiply Accelerator (MMA) beside it: given a 64-element int8 vector A, it multiplies it by a 64 × 64 int8 matrix B held in one of its two B banks, and writes the 64 sums, 32-bit, to a row of one of its two C banks; a C bank holds 16 such rows. Its X stage reads a row back out through a transfer buffer, shifted, rounded, and saturated to 8 bits, with ReLU if asked for. The C7x drives it with instructions of its own, which the C7000 compiler's `c7x_mma.h` exposes as intrinsics: `__HWAOPEN` with a configuration, `__HWALDB` for a row of B, `__HWALDA` and `__HWAOP` for a row of A, `__HWAXFER` and `__HWARCV` for a row of results.

`dsp/c7x/ai64_c7x_mma.c` uses it for an int8 fully connected layer, 64 inputs to 64 outputs, on a batch of up to 256 rows: `y = round((a x b) >> shift)`, saturated to int8, or with ReLU to uint8 (0 to 255). Linux hands it the job through the C7x's job block, like the FIR jobs:

```
$ sudo ai64 mma
C7x: 20 int8 layers, 256 rows x 64 inputs x 64 outputs (1048576 multiply-adds), all match the A72
  on the MMA:   1944 cycles = 1.94 us, 540 multiply-adds a cycle; A72 waited 3.0 us
  in C on the C7x: 161099 cycles = 161.10 us, 7 multiply-adds a cycle; A72 waited 162.4 us
  the same layer on one A72 core: 511.4 us
```

Every run has new data, shift, and ReLU, and both C7x results are checked byte for byte against the A72. The kernel:

- **Loads B once**, 64 `__HWALDB`, into B bank 0.
- **Goes 16 rows at a time, alternating C banks.** Each row is one `__HWALDA` and one `__HWAOPXFER`, which computes the row into one C bank while it transfers the same row of the block before out of the other, and `__HWARCV` takes it back to the C7x. The first block only computes; the last is read out after a fence.
- **Feeds A and B from the Streaming Engines**, SE0 and SE1, which fetch the rows ahead of use. With plain loads from L2 SRAM each row cost about 40 cycles, 11,000 for the batch; streamed, about 3, 1,900 for the batch, much of that the job's cache maintenance.
- **Builds and runs on a PC too.** TI's C7000 Host Emulation, in the compiler package, models the MMA: the same file compiles with `g++ -D__C7X_HOSTEM__` against `host_emulation/`, where `__HWAADV()` steps the model (on the C7x it is empty). Every configuration here was worked out there first, against a plain C reference.

Two things the hardware does that the emulator does not:

- **Signed outputs with ReLU.** Asked for int8 outputs with ReLU, the MMA leaves a sum that saturates below −128 at −128, where the emulator, and the header's description, apply ReLU after saturation and give 0. So with ReLU the layer's outputs are uint8, as a quantized network keeps them after a ReLU anyway.
- **Timing.** The emulator only moves when told to; on the C7x the transfers and receives wait for their data, so the kernel needs no delays of its own beyond the fence.

The plain C version is what the compiler makes of three loops for the C7x's own vector units, with no intrinsics: about 80 times slower here than the MMA, and still three times faster than an A72 core.

### Restarting the C7x

Both kernels' DSP drivers stop a C66x by asserting its local reset. A C71x has no local reset, so the driver powers the whole module off instead, and the device manager only allows that once the core is idle. Otherwise it refuses: `module-reset assert failed (-ENODEV)`, then `can't stop rproc: -19`, and the C7x keeps running.

TI's kernel, the one the Yocto image uses, sends `RP_MBOX_SHUTDOWN` through the core's mailbox before a stop, waits for `RP_MBOX_SHUTDOWN_ACK`, and then for the core to report that it is idle. The BeagleBoard kernel on the Debian image sends nothing. So both DSP images poll their receive FIFO, answer `RP_MBOX_SHUTDOWN` with the ACK, and execute `IDLE` with interrupts off (`dsp/include/ai64_dsp_mbox.h`), and `ai64 shutdown <remoteproc-name>` puts that message in the FIFO from Linux. On the BeagleBoard kernel, `ai64-demo` runs it before it stops each DSP. A C7x running an image without the handler still cannot be stopped; `ai64-demo` says so, and a reboot loads the new image.

The Zephyr images answer the same message, by masking their interrupts and parking in WFI. On the BeagleBoard kernel `ai64-demo` sends it to each R5F too, because a core that is busy on the bus when it is put in reset can wedge the device manager ([what it costs](#every-core-to-every-other-the-fabric)). Neither kernel's R5F driver starts a core again after a stop without the patch that keeps it powered. On TI's kernel, the C66x needs a patch as well: it never reports WFI after it has acknowledged. [Two 6.18 kernels](#two-618-kernels) lists every difference.

### Two 6.18 kernels

The Debian image and the Yocto image both run Linux 6.18, but not the same kernel:

- **BeagleBoard 6.18** (`6.18.55-arm64-k3-r59`), on the Debian image, is BeagleBoard.org's `arm64-k3` build. Its remoteproc and PRU drivers lack the additions TI keeps in its own tree.
- **TI 6.18** (`6.18.51-ti`), on the Yocto image, is TI's staging kernel, branch `ti-linux-6.18.y` of `ti-linux-kernel`, at the commit meta-ti pins. It carries TI's own remoteproc and PRU changes, and the image boots it with TI's U-Boot 2026.01. meta-ti selects both with `TI_PREFERRED_BSP = "ti-6_18"`. meta-beagle's own default for this board is BeagleBoard's TI-based 6.12.

Where they differ for this demo:

| | BeagleBoard 6.18 (Debian) | TI 6.18 (Yocto) |
| --- | --- | --- |
| RPMsg to an ICSSG core | No `.kick` and no vring interrupt in `pru_rproc`, so RTU0's RPMsg firmware does not start. The patched module adds them back. | Has both. Stock `pru_rproc`. |
| Shutdown request before a stop (`RP_MBOX_SHUTDOWN`) | Sends none. `ai64 shutdown` sends it to each R5F and DSP before `ai64-demo` stops it. | Sends it to every R5F and DSP, then waits for the ACK and for the core to report WFI. |
| Stopping an R5F | Powers the core off, which the device manager refuses. The core is stuck until a power cycle. The patched module keeps it powered. | The same power-off. Patch `0001` in `meta-ai64` keeps it powered. |
| Stopping a C66x | Asserts the local reset. Works as shipped. | Times out waiting for WFI, which a C66x never reports: without patch `0002`, `can't stop rproc: -110` and the core keeps running. The patch resets it once it has acknowledged. |
| Stopping the C7x | Works only after `ai64 shutdown`. | Works as shipped, through the shutdown request. |
| How the fixes get in | Two out-of-tree modules, built against the board's headers and installed in `/lib/modules/<kernel>/updates/`. The kernel marks them with taint flag `O`. Rebuild after every kernel upgrade. | Two patches applied to the kernel source in the Yocto build. |
| Device tree | BeagleBoard-DeviceTrees `v6.18.x`. It adds a pinmux file for BeagleBoard's overlays. | The kernel's own. The remote-core, ICSSG, and carveout nodes are the same as BeagleBoard's. |

Neither device tree gives any ICSSG core a vring interrupt, so on both images the overlay gives ICSSG0 RTU0 one, and neither reserves the fabric's DDR, so the overlay does that too. The firmware images, the overlay, and `ai64` are the same for both. `ai64-demo` tells the kernels apart by `-k3-` in `uname -r`, and only runs `ai64 shutdown` on the BeagleBoard one.

### Why Debian needs two patched modules

The BeagleBoard 6.18 kernel needs two fixes for this demo. `build.sh kmod` builds both against the board's own headers, and `install-beagle.sh` puts them in `/lib/modules/<kernel>/updates/`, which the kernel prefers.

**`pru_rproc`.** RPMsg is two ring buffers in shared memory, plus a doorbell in each direction. On a PRU core the Linux-to-PRU doorbell is the remoteproc `.kick` operation, which raises an ICSSG system event; the PRU-to-Linux doorbell is a "vring" interrupt. BeagleBoard's 6.18 `pru_rproc` has neither, and Linux refuses to start RPMsg firmware with `.kick method not defined`. `beagle-linux/kernel/pru_rproc/` is that driver with TI's kick and vring handler added back, the same patch as the PocketBeagle 2 demo, and one more change: a core whose firmware has no RPMsg (every ICSSG core here but ICSSG0 RTU0) starts without a vring interrupt. Neither image's J721E device tree has a vring interrupt for any ICSSG core, so the overlay gives RTU0 one, from ICSSG0's interrupt controller (system event 20, host interrupt 4).

**`ti_k3_r5_remoteproc`.** The stock driver can start an R5F core once. Stopping it then powers the core module off through the device manager, which refuses (`module-reset assert failed, ret = -19`) and leaves the core stuck in transition. Every later start fails with `module-reset deassert failed, ret = -19` until a power cycle. `beagle-linux/kernel/k3_r5_rproc/` is the 6.18.55 driver with that power-off taken out: stopping a core asserts its reset and leaves it powered, the way TI's kernel handles AM64x cores (TI's 6.18 still powers J721E cores off, hence the Yocto patch). With it, `ai64-demo` can restart all four R5F cores as often as you like.

If you don't need RPMsg on an ICSSG core, shared memory, as RTU1 and the ICSSG1 cores use, works with the stock `pru_rproc`. If you start an R5F only once per boot, the stock R5F driver is fine.

### Pin muxing

- **The Linux overlay.** The ICSSG pins are set by `beagle-linux/overlays/k3-j721e-beagleboneai64-ai64-demo.dtso`, written with `J721E_IOPAD()` from BeagleBoard-DeviceTrees `ti/k3-pinctrl.h`. That header isn't copied into this repo; `build.sh` fetches a pinned commit. Each pad hangs on the ICSSG core that drives it or runs its PWM set, so `pru_rproc` applies it when the core probes. The overlay also carries RTU0's vring interrupt.
- **The Zephyr pins.** Each R5F core muxes its own pins from its devicetree overlay: the SPI7 pins in `ai64_r5f0_0.elf`, P9.25's EHRPWM4_B in `ai64_r5f0_1.elf`, P8.18 (as GPIO2_4) in `ai64_r5f1_0.elf`; the UART2 console pins come from the upstream board file, on R5F0_0. R5F1_1 uses no pins. The R5F can write those pad registers only because Linux has already unlocked them.

The full pin list is `PINMUX.md`.

## Quick start: Yocto image

`yocto/` holds a kas configuration and the `meta-ai64` layer that build the demo into a Poky image. A first build takes about three hours on a 16-thread machine and needs about 150 GB of disk. From the repo root:

```
wget -O kas-container https://raw.githubusercontent.com/siemens/kas/5.5/kas-container
chmod +x kas-container
KAS_BUILD_DIR=yocto/build ./kas-container build yocto/kas/ai64-poky.yml
```

- The machine (`beaglebone-ai64`), kernel, and layers come from TI's `kas/poky-wrynose-config.yml`, and the distro is Poky. That file lives in TI's oe-layersetup repo on git.ti.com; `yocto/layers/meta-ai64/kas/ti-poky-wrynose-config.yml` is a copy of it, with its two templates folded in, so kas does not clone oe-layersetup. The copy names the commit it came from. `yocto/kas/ai64-poky.yml` holds everything of ours on top.
- The kernel is TI's 6.18 (`linux-ti-staging`, 6.18.51), with TI's U-Boot 2026.01: `ai64-poky.yml` sets `TI_PREFERRED_BSP = "ti-6_18"` in place of meta-beagle's default, BeagleBoard's 6.12. It is a different kernel from the Debian image's 6.18; [Two 6.18 kernels](#two-618-kernels) has the differences. It has the PRU kick, so `pru_rproc` needs no patch, and it sends `RP_MBOX_SHUTDOWN` before it stops a core, which the Zephyr and DSP images answer. Both trees come from TI's GitHub mirrors, `TexasInstruments/ti-linux-kernel` and `TexasInstruments/ti-u-boot`, because git.ti.com does not always answer.
- Two kernel patches in `yocto/layers/meta-ai64/recipes-kernel/linux/` still let the cores restart. One keeps a stopped R5F core powered, the same change as the Debian module; without it the second `ai64-demo` fails with `module-reset deassert failed, ret = -19`. The other lets a C66x be reset once it has acknowledged the shutdown, because a C66x never reports WFI and the stop otherwise fails with `-ETIMEDOUT`.
- The image leaves out TI's Ethernet firmware (`ti-eth-fw-j721e`), which meta-ti installs by default as R5F0_0's boot firmware. It runs the main-domain CPSW9G switch for TI's J721E EVM. The BeagleBone AI-64's Ethernet port is on the MCU-domain CPSW2G, which Linux drives itself, and the Debian image doesn't ship it either. Left in, the kernel boots it on R5F0_0 at startup, and it does not answer the shutdown request, so `ai64-demo` could not stop the core.
- The image builds `ai64`, the four R5F images, the twelve ICSSG images, the two DSP images, and the overlay itself. You don't need `build.sh` for it.
- Set `KAS_CONTAINER_ENGINE` to `podman` or `docker` if you need to choose one.

Flash the timestamped `yocto/build/deploy-ti/images/beaglebone-ai64/ai64-demo-image-beaglebone-ai64.rootfs-<timestamp>.wic.xz`. The login is `root` with no password, on the serial console or over SSH (dropbear, DHCP on the Ethernet port), and `/root` holds `ai64`, `ai64-demo`, and the firmware. Then `./ai64-demo --tmux`.

On a board, the image with TI's 6.18 ran `ai64-demo` 20 times in a row (15 times with the firmware copied in, 5 from a freshly built image), stopping and starting all 19 demo cores each time, with every core up, the fabric clean after each run, and no kernel errors, and gave the same PWM, fabric, and DSP timings as the Debian image.

`ai64-poky.yml` points the kernel, U-Boot, `ti-linux-firmware` (the boot firmware), and `core-secdev-k3` (image signing) at TI's GitHub mirrors, which carry the commits meta-ti pins. The one fetch left on git.ti.com is the PRU Software Support Package, which TI does not mirror. That server sometimes fails a first request and answers the next; if the fetch fails, run the build again, or copy its clone from another Yocto tree's `downloads/git2/` into `yocto/downloads/git2/`, `.done` file included.

## Build details

`./beagle-linux/scripts/build.sh [target]` builds the container images under `containers/`, then compiles inside them as your own user. It uses podman if installed, otherwise docker; set `AI64_CONTAINER_ENGINE` to pick one.

| Target | Output | Notes |
| --- | --- | --- |
| `linux` | `linux-app/ai64`, `beagle-linux/overlays/*.dtbo` | Debian 13 aarch64 cross compiler. Fetches EmbeddedRPC 1.14.0 and BeagleBoard-DeviceTrees. |
| `pru` | `pru/build/ai64-icssg*.elf`, twelve images from the two projects in `pru/` | TI PRU compiler 2.3.3 and the PRU Software Support Package, cloned from git.ti.com. Prints each image's RAM use. `TRACE=0` builds the PRUs' and Tx_PRUs' trace out. |
| `zephyr` | `build/zephyr/r5/zephyr/ai64_r5f0_0.elf` and `build/zephyr/r5fX_Y/zephyr/ai64_r5fX_Y.elf`, all four from one sysbuild build | Zephyr SDK 1.0.1. Zephyr is pinned in `zephyr/west.yml` and checked out at `zephyr/upstream`, with `zephyr/patches/` applied. |
| `dsp` | `dsp/build/ai64-c66x.elf`, `dsp/build/ai64-c7x.elf` | TI C6000 CGT 8.3.12 and C7000 CGT 1.2.0.STS. |
| `kmod` | `beagle-linux/kernel/pru_rproc/pru_rproc.ko`, `beagle-linux/kernel/k3_r5_rproc/ti_k3_r5_remoteproc.ko` | Needs `AI64_SSH`. Reads the kernel version from the board, or from `AI64_KREL`. |
| `bundle` | `out/ai64-beagle/` | Everything `install-beagle.sh` needs. Run it after the other targets. |
| `erpcgen` | `zephyr/erpc/shim/` | Builds erpcgen and regenerates the shim from `zephyr/erpc/ai64.erpc`. Only needed after changing the IDL. |
| `all` (default) | `linux`, `pru`, `zephyr`, and `dsp` | |

### Common problems

**Debian setup**

- **`ssh USER@bbai64.local` doesn't connect.**
  - Give a fresh card its two automatic reboots, about three minutes.
  - Try the address from your router's DHCP list, or the USB-C link at `192.168.7.2`.
  - The serial console (`ttyS2`, 115200) works without any network.
- **`REMOTE HOST IDENTIFICATION HAS CHANGED` after reflashing the card.** The new image has new SSH host keys. Run `ssh-keygen -R bbai64.local` and connect again.
- **`install-beagle.sh` says it could not install tmux.** The board has no internet. Run plain `ai64-demo` instead of `ai64-demo --tmux`, or connect Ethernet and run `install-beagle.sh` again.
- **`install-beagle.sh` says a module `was not built for <kernel>`.** The modules in the bundle were built for a different kernel. Run `AI64_SSH=USER@BOARD ./beagle-linux/scripts/build.sh kmod`, then `bundle`, and copy it again.
- **`ai64-demo` says `RPMsg devices did not show up`.** Look at `dmesg | grep -i -E "remoteproc|rpmsg|kick|vring"` on the board.
  - `.kick method not defined` means the stock `pru_rproc` is loaded; see the last entry below.
  - A missing `/lib/firmware/ai64_r5f0_0.elf` or `ai64-icssg0-rtu0.elf` means `install-beagle.sh` didn't finish; run it again.
- **`ai64 pads` marks an ICSSG pin DIFFERENT, or a PWM pin stays low.** The board booted an older overlay. Run `install-beagle.sh` from a current bundle and reboot.
- **`ai64-demo` says the stock `ti_k3_r5_remoteproc` cannot start an R5F core again.** The patched R5F module is not loaded, usually after a kernel upgrade. Rebuild with `build.sh kmod`, run `bundle`, install, and reboot. Until then a reboot reloads the R5F cores.
- **`dmesg` shows `module-reset deassert failed, ret = -19`.** An R5F core was stopped with the stock module loaded and is now stuck. Power-cycle the board; a reboot from Linux is not always enough.
- **`ai64-demo` says it cannot stop the C7x.** The running C7x image predates the shutdown handler; see [Restarting the C7x](#restarting-the-c7x). Reboot once to load the new image.
- **`ai64-demo` says `/proc/device-tree/reserved-memory/ai64-fabric@a9000000 is missing`.** The board booted without the demo's overlay, so the fabric's DDR belongs to Linux, and `ai64-demo` will not start cores that would write over it. Run `install-beagle.sh` and reboot, and check the `fdtoverlays` line of the default label in `/boot/firmware/extlinux/extlinux.conf`.
- **`ai64 fabric` says another viewer is running.** Linux is node 0, and only one process can be: close the `fabric` window, or the merged console's `[Fabric]` viewer, first. `ai64 fabric route` works alongside either.
- **P9.25 stays low, or `ai64 pads` marks `0x1a4` DIFFERENT.** R5F0_1 has not booted yet, or another overlay gives ball W26 and EHRPWM4 to Linux, most likely the stock `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`. Check the `fdtoverlays` line of the default label in `/boot/firmware/extlinux/extlinux.conf`.

**Builds**

- **"Permission denied" deleting `build/`, `.west/`, or `modules/`.** These files came from a checkout whose containers wrote them as another uid. Clear them once:
  - podman: `podman unshare rm -rf build .west modules bootloader tools zephyr/upstream`
  - docker: `sudo rm -rf` the same paths
- **Don't run `west init -l .` from the repo root.** That puts the workspace in the parent directory. The script runs `west init -l zephyr` instead.
- **`dmesg` shows `Direct firmware load for j7-main-r5f0_0-fw failed with error -2` at boot.** That is expected. The kernel tries to boot each R5F core under its default name, finds nothing, and leaves it off. `ai64-demo` starts them with `ai64_r5f0_0.elf` and so on. The same goes for the C66x and C7x cores.
- **`kmod` fails to copy `/usr/src/linux-headers-...`.** The BeagleBoard image ships the headers for its kernel. If yours doesn't have them, run `sudo apt install linux-headers-$(uname -r)` on the board.
- **`git.ti.com` times out while fetching the PRU Software Support Package.** That server sometimes fails a request and answers the next; `build.sh` tries three times. If all three fail, run the build again later, or clone the package yourself at the commit pinned in `build.sh` into `third_party/pru-software-support-package`.
- **RTU0 never appears, and `dmesg` says `.kick method not defined`.** The stock `pru_rproc` is loaded. This is usually after a kernel upgrade. Rebuild with `build.sh kmod`, run `bundle`, and install again.

## Repository

`LAYOUT.md` is the directory map. `PINMUX.md` is the pin list. `media/` holds the images in this README.

## License

MIT; see `LICENSE`. `beagle-linux/kernel/pru_rproc/` and `beagle-linux/kernel/k3_r5_rproc/` are GPL-2.0-only because they are derived from the Linux drivers. `pru/multicore_scheduler/` is BSD-3-Clause, as TI's open-pru example it is ported from.
