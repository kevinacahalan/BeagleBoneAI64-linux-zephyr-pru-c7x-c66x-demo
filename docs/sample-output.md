# Sample Output

This page shows what a passing run of `ai64-demo --tmux` looks like across its tmux windows and remoteproc trace logs.

## Startup sequence

On startup, the `linux` window runs a short verification script before handing you a prompt:

1. Pings `ICSSG0 RTU0` over RPMsg.
2. Asks Zephyr on `R5F0_0` to `say` something, verify a `chunk` of the DDR bulk buffer, and write `spi`.
3. Asks `R5F0_1` to start P9.25's `steps`, and asks `RTU0` for its `status`.
4. Asks Zephyr on each of the other three R5F cores to `say` something.
5. Asks `R5F1_0` (which has no SPI peripheral) to write `spi`, which it forwards to `R5F0_0` over `ipc_service`.
6. Asks the monitor (`R5F1_1`) for every R5F core's `status`.
7. Has `R5F0_0` `dma` an int8 layer's input into the C7x's L2 SRAM for its MMA and checks the result against the A72.
8. Starts the pipeline, training the C7x's classifier on 3 s of P8.31 and handing P9.25 over to it.

The demo is up when the `linux` window prints `linux window ready. commands: ...`.

## Window output from a passing run

Each `Linux -> X` line is a command Linux sent, and the indented lines underneath are the answers (which also appear in that core's own window). The blocks below were captured at different moments of the same run, and the PWM generators change step every 5 ms, so their instantaneous periods and duties vary between blocks.

```text
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
  R5F0_0: dma: its UDMA copied 16384 bytes from DDR to the C7x's L2 in 33.7 us (486 MB/s; the R5F reading them itself: 5466.3 us), then the C7x's MMA ran the layer: 2536 cycles, 3.6 us round trip; all match the A72
Linux: the pipeline, P8.31 to P9.25 (ai64 pipe start)
pipeline: P8.31 -> ICSSG0 PRU1 and RTU1 -> R5F0_0's UDMA -> C66x 0 and 1's FFT -> C7x's MMA -> R5F0_1 -> P9.25
training: 3.0 s of windows of P8.31, each labelled with the step the input was on
  4528 windows, 300 to 309 a class; 1344 across a step left out
  the int8 layer, checked here on the same windows: 4528 of 4528 right (100.0%), shift 9
the C7x has the weights; R5F0_1 sets P9.25 to each window's class (ai64 pipe shows the score)
linux window ready. commands: r5fX_Y say|status|spi|pwm|steps ... | r5f0_0 chunk|dma ... | rtu0 pwm|steps|sweep|dead|status|<text> | pipe [start|stop] | help

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
  following the pipeline: the class the C7x finds on P8.31 (ai64 pipe)
  now: 10.000 kHz, 50.0% duty
  P9.25 high 50000 ns of each 100000 ns period

== pipe (every 10 s, each stage of the pipeline and R5F0_1's score)
pipeline (running, P9.25 follows): stage                        windows/s   latency mean/max us  work mean/max us  lost   err
  ICSSG0 RTU1  stamps the window               1953.1        0.0 /      8.0       2.3 /    21.9     0     0
  R5F0_0       UDMA into C66x L2               1953.1       22.0 /     82.1      10.8 /    69.3     0     0
  C66x 0       FFT, even windows                976.5      109.0 /    168.1      88.5 /    90.7     0     0
  C66x 1       FFT, odd windows                 976.6      109.0 /    164.2      88.5 /    90.8     0     0
  C7x          MMA layer, class                1953.1      112.0 /    329.0       0.8 /     1.5     0     0
  R5F0_1       P9.25                           1953.1      125.5 /    355.9       3.7 /    70.7     0     0
  newest window 299187: 20 kHz 75%, score 54 against 44; P9.25 holds 20 kHz 75%, set 28433 times (29477 classes waited for a second window)
  score: 15063 right, 0 wrong (100.00%), 4468 across a step, in 10.0 s; since training 214122 right, 0 wrong

== la (every second, ICSSG1's 10 channels)
ICSSG1 logic analyzer, the last 1.004 s; IEP0 at 5493.118 s
  25 MS/s, channels 0 1 2 3 4 5 6 7 8 9; 1 sample a word, 106.7 MB/s into the ring, which holds 0.315 s; the reader keeps up
  read 1666667 blocks/s, 181567 edges/s; lost 0 blocks this interval, 113 in all

  ch  pin    GPI  wired to          level  edges/s     frequency   duty
   0  P8.03  19   ECAP P8.15        high       29936   14.968 kHz   50.1%
   1  P8.07  14   SPI7_CS P9.28     high           0      0.0 Hz   100.0%
   2  P8.08  13   SPI7_CLK P9.31    low            0      0.0 Hz     0.0%
   3  P8.09  16   SPI7_MOSI P9.30   low            0      0.0 Hz     0.0%
   4  P8.10  15   SQUARE P8.18      high        2000    999.9 Hz    50.0%
   5  P8.36  7    S0_A P8.11        high       29936   14.968 kHz   50.1%
   6  P8.38  8    S0_B P8.12        low        29936   14.968 kHz   46.8%
   7  P9.11  0    EHRPWM4 P9.25     high       29912   14.956 kHz   50.2%
   8  P9.13  1    S1_A P9.12        high       29935   14.968 kHz   50.1%
   9  P9.23  9    S1_B P9.15        low        29935   14.967 kHz   46.8%

== c66x0 (c66x1 is the same, with DNUM 1 and the odd windows)
4d80800000.dsp trace (ai64-c66x.elf), 2048 bytes at 0x4d80815480
hello from the c66x, DNUM 0
pipeline: FFT stage up, even windows
c66x up 10 s
c66x up 20 s

== c7x
64800000.dsp trace (ai64-c7x.elf), 2048 bytes at 0xa8100000
hello from the c7x, DNUM 1024, MMU on, L1DCFG 4, L2CFG 2
c7x up 10 s
c7x up 20 s
```

## Reading the numbers

- **`pwm` window:** shows what each generator's own core reports in shared memory (it does not measure the pins). Values are a snapshot of whichever step the generator was on at that instant, so they change every 10 s. `periods/s` averages the three frequencies (10, 15, and 20 kHz) over 10 s, so it reads about 15,000.
- **A and B complementary pairs:** on each ICSSG0 slice, B is high while A is low, minus 1000 ns of dead band on either side of A's pulse (where both outputs stay low so a half-bridge's switches are never on together) and the 40 ns offset where A starts each period. See [Probing & Signal Captures](probing.md#complementary-a-and-b-with-dead-band).
- **R5F round trips:** `R5F1_1` pings each peer over `ipc_service` every 100 ms (shared buffer + mailbox doorbell through the peer's icmsg work queue and request thread, ~80 µs round trip). In the `dma` line, `R5F0_0`'s UDMA copies 16 KiB in ~34 µs; the R5F reading the same buffer directly takes ~5.5 ms because that shared Linux buffer is mapped uncached.
- **`pipe` window:** each stage shows windows/s, end-to-end latency from the window's last sample on P8.31 until the stage finished, and the stage's own work time, followed by `R5F0_1`'s classification score against the true GTC-timed input step. The pipeline runs 1,953 windows/s (2,048 samples at 2 MHz, a new window every 1,024 samples). Core-to-core latency is ~126 µs (varying a few µs across runs); pin-to-pin delay is ~1.5 ms because each 1.024 ms window must be mostly the new step and `R5F0_1` waits for two consecutive windows to agree ([The pipeline](pipeline.md#the-pipeline-p831-to-p925)). `across a step` counts windows that straddle a transition and are excluded from scoring.
- **`RTU0`'s `runs request` counter:** printed when `PRU0` adopts a new request. ICSSG0 RTUs keep counting requests across restarts, so the number varies.
- **`la` window:** frequency and duty average over the whole 1 s interval, so the stepping PWM channels show their average (~15 kHz). `113 in all` is not a failure—those are ring blocks dropped since startup (~68 µs total), each recorded as a [marked hole](../pru/logic_analyzer/README.md#holes).
- **`fabric` window:** full latency matrix output is in [The fabric](fabric.md#what-you-see).
- **`/dev/rpmsg*` numbers:** device indices shown in window headers can differ from boot to boot.

## Remoteproc trace logs (`ai64 trace`)

`R5F0_0` logs to UART2 (`P8.34` TX / `P8.22` RX). The other three R5Fs and the ICSSG/DSP cores log to their remoteproc trace buffers, which you can inspect with `ai64 trace <core>` (passing either a label from `ai64 cores` such as `r5f0_1`, `g0rtu1`, `c7x`, or a remoteproc name like `5d00000.r5f`):

```bash
$ ai64 trace 5d00000.r5f
*** Booting Zephyr OS build a915b2f12675 ***
I: pwm: Zephyr set P9.25 (EHRPWM4_B) stepping 10, 15, 20 kHz x 10-90% duty, every 5 ms
I: eRPC server waiting on rpmsg
I: tisci: mailbox interrupt routed, router 213 input 433 -> output 228
I: tisci: mailbox interrupt routed, router 213 input 397 -> output 229
I: link to R5F0_0 up
I: link to R5F1_0 up
I: ai64 r5f0_1 hello
I: pipe: P9.25 from the C7x's classes, on its doorbell
I: fabric node 2 up, start 9
I: rpmsg channel rpmsg-raw addr 1024 is up
I: link to R5F1_1 up
I: pwm: Linux set P9.25 (EHRPWM4_B) stepping 10, 15, 20 kHz x 10-90% duty, every 5 ms
I: erpc say: Zephyr on R5F0_1 got: hello from linux
```

The first mailbox route is Linux's doorbell, the second is the other R5Fs', and `link ... up` lines show `ipc_service` coming online as peers boot. `R5F1_0` logs `blink: P8.18 toggles every 500 us`, and `R5F1_1` writes its monitor table into its trace every 10 s. `R5F0_0` prints equivalent lines on its UART console, plus `spi7 hello wrote 5 bytes` at boot and `dma: UDMA channel 44, transmit ring 44, completion ring 744` on its first copy.

The ICSSG cores emit short startup logs the same way:

```bash
$ ai64 trace b006000.rtu
ICSSG0 RTU1 up
ICSSG0 RTU1 is fabric node 9
hardware PWM on P9.12 (A) and P9.15 (B) runs request 14, now period 66664 ns, A high 59996 ns

$ ai64 trace b038000.pru
ICSSG0 PRU1 up: ICSSG PWM2_0 on IEP1, A on P9.12, B on P9.15
sampling P8.31 at 2 MHz, 2048 a window
first period: 100000 ns, duty 10000 ns

$ ai64 trace g1pru0
ICSSG1 PRU0 up: sampling every 10 cycles, lane 0
```
