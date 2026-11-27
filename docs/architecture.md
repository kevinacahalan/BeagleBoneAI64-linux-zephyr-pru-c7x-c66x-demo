# Architecture

This document holds the full per-core diagrams for the demo. For the high-level overview, see [`README.md`](../README.md#how-it-works).

Every core in the main demo has its own block below. The architecture is organized into three views so nothing is dropped and the ICSSG1 alternatives do not clutter the core demo:

- **Thick arrows (`==>`)** trace the 5-core-type real-time pipeline (`P8.31` → `P9.25`).
- **Solid arrows (`-->` / `<-->`)** show host control, IPC, slice internals, and peripheral ownership.
- **Dashed arrows (`-.->`)** show TI-SCI service requests to the MCU device manager, the `P9.15` → `P8.31` physical jumper, optional links, and read-only polling by Linux.
- Linux also loads and restarts every coprocessor via `remoteproc` (omitted for clarity).

## 1. Main demo architecture

```mermaid
flowchart LR
  classDef host fill:#1f4e8c,stroke:#3b76c4,stroke-width:1px,color:#ffffff
  classDef r5 fill:#1e5631,stroke:#3a8252,stroke-width:1px,color:#ffffff
  classDef dsp fill:#4c2c72,stroke:#764fa6,stroke-width:1px,color:#ffffff
  classDef g0 fill:#7c4d0f,stroke:#b07323,stroke-width:1px,color:#ffffff
  classDef dm fill:#3b424c,stroke:#66707d,stroke-width:1px,color:#ffffff
  classDef periph fill:#2d333b,stroke:#6e7681,stroke-width:1px,stroke-dasharray:3 2,color:#e6edf3

  ai64["Linux ai64<br/>2x Cortex-A72"]:::host
  dm["Device Manager<br/>2x MCU R5F (TI-SCI)"]:::dm

  subgraph G0["ICSSG0 (6 cores)"]
    subgraph S0["Slice 0 cores"]
      rtu0["RTU0"]:::g0
      tx0["Tx_PRU0<br/>modulator"]:::g0
      pru0["PRU0<br/>SW edges"]:::g0
    end
    subgraph S1["Slice 1 cores"]
      rtu1["RTU1"]:::g0
      tx1["Tx_PRU1<br/>modulator"]:::g0
      pru1["PRU1<br/>HW PWM + GPI"]:::g0
    end
    iep0["IEP0<br/>counts ns"]:::periph
    r30["PRU0 R30<br/>P8.11 A, P8.12 B"]:::periph
    ecap["eCAP0 APWM<br/>P8.15 (A copy)"]:::periph
    iep1["IEP1<br/>CMP0, CMP1, CMP2"]:::periph
    pwm2["PWM module set 2<br/>P9.12 A, P9.15 B"]:::periph
  end

  p831["P8.31<br/>jumpered input"]:::periph

  subgraph ZEPH["Zephyr, 4x R5F (one sysbuild)"]
    r500["R5F0_0<br/>SPI7, UDMA"]:::r5
    r501["R5F0_1<br/>EHRPWM4"]:::r5
    r510["R5F1_0<br/>1 kHz blink"]:::r5
    r511["R5F1_1<br/>monitor"]:::r5
    ipc(("ipc_service<br/>icmsg, per pair")):::r5
    r500 <--> ipc
    r501 <--> ipc
    r510 <--> ipc
    r511 <--> ipc
  end

  uart2["UART2 console<br/>P8.34 TX, P8.22 RX"]:::periph
  spi7["SPI7<br/>P9.31 CLK<br/>P9.28 CS0<br/>P9.30 MOSI"]:::periph
  udma["NAVSS UDMA<br/>memcpy channel"]:::periph
  ep["EHRPWM4<br/>P9.25"]:::periph
  gpio2["GPIO2<br/>P8.18"]:::periph

  subgraph DSPS["3x DSPs (bare-metal)"]
    c660["C66x 0<br/>FIR, even FFT"]:::dsp
    c661["C66x 1<br/>FIR, odd FFT"]:::dsp
    c7x["C7x<br/>FIR, classifier"]:::dsp
    mma["C7x MMA<br/>matrix engine"]:::dsp
  end

  %% Linux links
  ai64 <-->|"RPMsg text"| rtu0
  ai64 <-->|"eRPC, bulk, shutdown"| r500
  ai64 <-->|"eRPC, trace, shutdown"| r501
  ai64 <-->|"eRPC, trace, shutdown"| r510
  ai64 <-->|"eRPC, trace, shutdown"| r511
  ai64 <-->|"FIR, trace, shutdown"| c660
  ai64 <-->|"FIR, trace, shutdown"| c661
  ai64 <-->|"FIR/layer, trace, shutdown"| c7x

  %% Device Manager (TI-SCI)
  dm <-.->|"IRQ routes, UDMA"| r500
  dm <-.->|"IRQ routes, PWM clk"| r501
  dm <-.->|"IRQ routes, GPIO pwr"| r510
  dm <-.->|"IRQ routes"| r511
  dm -.->|"clock, power"| ep

  %% ICSSG0 Slice 0
  rtu0 -->|"pwm_ctl"| tx0
  tx0 -->|"pwm_next"| pru0
  tx0 -->|"period, duty"| ecap
  pru0 -->|"sets up, polls"| iep0
  pru0 -->|"writes edges"| r30

  %% ICSSG0 Slice 1 + Pipeline input
  rtu1 -->|"pwm_ctl"| tx1
  tx1 -->|"pwm_next"| pru1
  pru1 -->|"sets up, rearms"| iep1
  pru1 -->|"sets up"| pwm2
  iep1 -->|"CMP1/2 toggle"| pwm2
  pwm2 -.->|"P9.15 jumper"| p831
  p831 ==>|"2 MHz R31 shift"| pru1
  pru1 ==>|"shared RAM windows"| rtu1
  rtu1 ==>|"window doorbell"| r500

  %% R5F peripherals and Pipeline compute
  r500 -->|"console"| uart2
  r500 -->|"SPI driver"| spi7
  r510 -->|"GPIO, k_timer"| gpio2
  r500 ==>|"TR descriptors"| udma
  udma ==>|"even windows to L2"| c660
  udma ==>|"odd windows to L2"| c661
  udma -->|"DDR to L2 (layer)"| c7x
  c660 ==>|"64 FFT features"| c7x
  c661 ==>|"64 FFT features"| c7x
  c7x ==>|"a layer, 16 rows a block"| mma
  c7x ==>|"class, mailbox"| r501
  r501 ==>|"PWM driver"| ep
```

See [The pipeline](pipeline.md), [`zephyr/r5/README.md`](../zephyr/r5/README.md), [`pru/README.md`](../pru/README.md), and [`dsp/README.md`](../dsp/README.md) for per-core details.

## 2. Shared-memory fabric (12 nodes, 132 paths)

`RTU1` takes requests only over the fabric; `RTU0` accepts both fabric and RPMsg. In ICSSG0, the fabric stops at the RTUs so nothing outside ICSSG0 can perturb `Tx_PRU`/`PRU` timing. Nodes 10 and 11 (`ICSSG1 RTU0`/`RTU1`) participate in pings, tours, and the spinlock tally when `--icssg1 la` runs (default) and stay silent with `--icssg1 scheduler` or `off`.

```mermaid
flowchart LR
  classDef host fill:#1f4e8c,stroke:#3b76c4,stroke-width:1px,color:#ffffff
  classDef r5 fill:#1e5631,stroke:#3a8252,stroke-width:1px,color:#ffffff
  classDef dsp fill:#4c2c72,stroke:#764fa6,stroke-width:1px,color:#ffffff
  classDef g0 fill:#7c4d0f,stroke:#b07323,stroke-width:1px,color:#ffffff
  classDef g1opt fill:#5c203c,stroke:#b04a78,stroke-width:1px,stroke-dasharray:4 2,color:#ffffff
  classDef fab fill:#1b3b5a,stroke:#4a86c8,stroke-width:1.5px,color:#ffffff

  fab(["Shared-Memory Fabric<br/>DDR inboxes, mbox 11<br/>200 MHz GTC<br/>spinlock 255"]):::fab

  subgraph H["Host"]
    l["Linux ai64 - node 0"]:::host
  end
  subgraph R["Zephyr R5Fs"]
    r500["R5F0_0 - node 1"]:::r5
    r501["R5F0_1 - node 2"]:::r5
    r510["R5F1_0 - node 3"]:::r5
    r511["R5F1_1 - node 4"]:::r5
  end
  subgraph D["DSPs"]
    c660["C66x 0 - node 5"]:::dsp
    c661["C66x 1 - node 6"]:::dsp
    c7x["C7x - node 7"]:::dsp
  end
  subgraph G0F["ICSSG0"]
    rtu0["RTU0 - node 8"]:::g0
    rtu1["RTU1 - node 9"]:::g0
    s0["Slice 0: Tx_PRU0, PRU0"]:::g0
    s1["Slice 1: Tx_PRU1, PRU1"]:::g0
  end
  subgraph G1F["ICSSG1 RTUs (--icssg1 la; silent otherwise)"]
    g1r0["RTU0 - node 10"]:::g1opt
    g1r1["RTU1 - node 11"]:::g1opt
  end

  l <--> fab
  r500 <--> fab
  r501 <--> fab
  r510 <--> fab
  r511 <--> fab
  c660 <--> fab
  c661 <--> fab
  c7x <--> fab
  fab <-->|"requests, status"| rtu0
  fab <-->|"requests, status"| rtu1
  rtu0 <-->|"slice RAM only"| s0
  rtu1 <-->|"slice RAM + shared RAM ring"| s1
  fab <-.->|"ping, tour"| g1r0
  fab <-.->|"ping, tour"| g1r1
```

See [The fabric](fabric.md) for the protocol, memory layout, and measured latencies.

## 3. ICSSG1 options (alternative firmware sets)

ICSSG1 is independent of the main demo and runs one of two mutually exclusive firmware sets (or stays stopped with `--icssg1 off`):

### Option A: Logic analyzer (`--icssg1 la`, default — 3 of 6 cores active)

```mermaid
flowchart TB
  classDef host fill:#1f4e8c,stroke:#3b76c4,stroke-width:1px,color:#ffffff
  classDef g1la fill:#7a284e,stroke:#b04a78,stroke-width:1px,color:#ffffff
  classDef periph fill:#2d333b,stroke:#6e7681,stroke-width:1px,stroke-dasharray:3 2,color:#e6edf3
  classDef fab fill:#1b3b5a,stroke:#4a86c8,stroke-width:1.5px,color:#ffffff

  subgraph CAP["Capture path (1 core)"]
    direction LR
    pins["10 GPI pins<br/>open reads low"]:::periph -->|"R31 every N cyc"| lapru["PRU0<br/>sampler"]:::g1la
    lapru -->|"starts, reads t0"| g1iep["IEP0<br/>counts ns"]:::periph
    lapru -->|"XOUT 64 B"| lax2v["XFR2VBUS<br/>slice write engine"]:::periph
    lax2v -->|"writes"| ddr[("DDR ring<br/>32 MiB")]:::periph
    ddr -->|"/dev/ai64-la<br/>or /dev/mem"| lahost["Linux ai64<br/>web /la, MCP, CLI"]:::host
    lahost -.->|"reads count"| g1iep
  end

  subgraph OTHER["Other ICSSG1 cores (not in capture)"]
    direction LR
    g1r0["RTU0"]:::g1la --> g1fab["Fabric nodes 10-11<br/>ping, tour, tally"]:::fab
    g1r1["RTU1"]:::g1la --> g1fab
    g1free["PRU1, Tx_PRU0, Tx_PRU1<br/>stopped (free)"]:::periph
  end
```

See [`pru/logic_analyzer/README.md`](../pru/logic_analyzer/README.md).

### Option B: TI multicore scheduler (`--icssg1 scheduler` — all 6 cores active)

```mermaid
flowchart LR
  classDef host fill:#1f4e8c,stroke:#3b76c4,stroke-width:1px,color:#ffffff
  classDef g1mcs fill:#2c5e63,stroke:#4f9da6,stroke-width:1px,color:#ffffff
  classDef periph fill:#2d333b,stroke:#6e7681,stroke-width:1px,stroke-dasharray:3 2,color:#e6edf3

  mcs_iep["IEP0<br/>100 ns, step 4 ns"]:::periph -->|"CMP7 then CMP2-6"| mcs_tm["6 Task Managers<br/>one per core"]:::periph
  mcs_tm -->|"zero-delay trigger"| mcs_cores["All 6 ICSSG1 cores<br/>PRU0/1, RTU0/1, Tx_PRU0/1"]:::g1mcs
  mcs_cores -->|"PRU0 sets up, starts"| mcs_iep
  mcs_cores -->|"each arms its TM"| mcs_tm
  mcs_cores -->|"writes init state"| mcs_pwm["PWM2_0 init state<br/>P8.17"]:::periph
  mcshost["Linux ai64 sched"]:::host -.->|"reads"| mcs_iep
  mcshost -.->|"reads"| mcs_tm
  mcshost -.->|"reads"| mcs_pwm
```

See [`extra-demos/README.md`](../extra-demos/README.md) and [`extra-demos/icssg1-multicore-scheduler/README.md`](../extra-demos/icssg1-multicore-scheduler/README.md).
