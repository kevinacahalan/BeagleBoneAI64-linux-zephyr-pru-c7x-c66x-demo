# The fabric

The demo's other links go through Linux, or join two cores that sit next to each other. The fabric joins every demo core that talks directly to every other one, Linux included: twelve nodes and 132 one-way paths, none of them relayed through Linux. One header, `zephyr/include/ai64_fabric.h`, holds the layout and the whole protocol, and every image compiles it: Zephyr with GCC, the DSPs with TI's C6000 and C7000 compilers, the RTUs with clpru, and `ai64` with the Linux compiler. Each image only adds a few lines of glue: how to read the clock, and how it maps the memory.

| Node | Name | Core | When it looks at its inbox |
| --- | --- | --- | --- |
| 0 | `linux` | A72, while `ai64 fabric` runs | every 20 µs or so, between sleeps |
| 1–4 | `r5f0_0`, `r5f0_1`, `r5f1_0`, `r5f1_1` | Zephyr (`zephyr/r5/common/src/fabric.c`) | in a thread at the lowest priority, which yields after each pass |
| 5, 6 | `c66x0`, `c66x1` | C66x | every pass of its main loop |
| 7 | `c7x` | C7x | every pass of its main loop |
| 8 | `g0rtu0` | ICSSG0 RTU0 (`pru/pwm/firmware/pwm_rtu.c`) | every pass of its main loop, next to its RPMsg check |
| 9 | `g0rtu1` | ICSSG0 RTU1 (the same source) | every pass of its main loop |
| 10, 11 | `g1rtu0`, `g1rtu1` | ICSSG1 RTU0 and RTU1 (`pru/logic_analyzer/firmware/la_rtu.c`) | every 100 µs |

The ICSSG0 PRUs and Tx_PRUs are not nodes: each slice's RTU talks for them ([The ICSSG firmware](../pru/README.md#the-icssg-firmware)). ICSSG1's RTUs are nodes and nothing else: the logic analyzer's PRU0 writes to DDR on its own ([ICSSG1: a logic analyzer](../pru/logic_analyzer/README.md#how-it-works)), and they poll every 100 µs, so as not to load ICSSG1's port to the SoC while it does. With the multicore scheduler on ICSSG1 instead (`ai64-demo --icssg1 scheduler`), nodes 10 and 11 do not answer, as a stopped node. The layout's twelve slots (`AI64_FAB_SLOTS`) are all used.

## What carries it

Four pieces of SoC hardware that every one of those cores can reach:

- **DDR**, 128 KiB at `0xA9000000`, in the gap between the C7x's carveout and the IPC region above it. The overlay reserves 1 MiB there (`ai64-fabric@a9000000`, no-map) so that Linux keeps out, and `ai64-demo` refuses to start the cores on a kernel that booted without it. Each node owns an 8 KiB page: its inbox, and the counts it publishes for Linux to read. A spinlock tally follows the pages.
- **Mailbox cluster 11** (`0x31F8B000`) for doorbells. Linux's device tree leaves clusters 5–11 disabled; 5, 6, 8, and 9 hold messages from the device manager's firmware, and 11 is empty. FIFO n is node n's doorbell.
- **The GTC** (`0x00A90000`), the 200 MHz counter Linux's arch timer counts too. Every core reads the same clock, so a latency measured from one core's stamp to another's is real, one way. A message carries the counter's low 32 bits, which wrap every 21.47 s; every difference is taken in 32 bits, so it comes out right as long as it is under that, and the 50 ms timeout keeps it so.
- **Hardware spinlock 255** (`0x30E00000`), which no kernel driver uses, for the tally.

## Read your own page, write to the others

An uncached read costs 0.1 to 0.5 µs, and a write is posted and costs almost nothing. So a node only ever reads its own page. A message goes into the receiver's page, and the stamps a tour collects go straight into its origin's page. The sender then writes its own node number into the receiver's mailbox FIFO, and the receiver, which polls only its FIFO's status register, reads its page when something has arrived. A FIFO holds four doorbells; a sender that finds it full skips the doorbell, and every node also scans its whole inbox once a millisecond.

## A message

Each inbox has one 64-byte slot for every sender and message class, and only that sender writes it. The sender writes the message, then bumps its count for that class in a word of the receiver's page that only it writes, then rings. The receiver remembers the counts it last saw, so a changed count is a new message, and nothing is ever cleared. Every class allows one message in flight per sender and receiver, so a slot is never overwritten before it is read, and no core ever waits on another:

| Class | Sent by | What it does |
| --- | --- | --- |
| ping | every node, to every other, every 10 ms | carries the sender's GTC stamp and a value. The receiver stamps when it has read it, mixes its node number into the value, and sends it back. |
| pong | the receiver of a ping | the sender checks the value and times the round trip. The receiver's stamp minus the sender's is the one-way latency. A ping with no pong in 50 ms is a timeout, and that peer counts as down until a later ping comes back. |
| tour | R5F0_0, every 10 ms | goes round every node whose pings R5F0_0 sees answered, Linux included while its viewer runs, in an order that keeps changing the kind of core: c66x0, g0rtu0, r5f1_0, c7x, g1rtu0, r5f0_1, c66x1, g0rtu1, r5f1_1, g1rtu1, linux, and home. Each hop stamps it and mixes its number into the value, and R5F0_0 checks the value when the tour comes back. |
| route | `ai64 fabric route`, from Linux | the same as a tour, along the nodes you name and back to Linux. It can carry an action for the first PWM generator on the way: `pwm <period_ns> <duty_ns>` or `steps <ms>` for ICSSG0 RTU0 or RTU1, or R5F0_1's EHRPWM4, or `sweep <ms>` or `dead <ns>` for either RTU. Its count lives in a word of its own, so routes work while the viewer, also node 0, runs. |

A node that starts takes the counts it finds as already seen, and zeroes its own counters. A message sent to a stopped node is simply lost, and its sender times out, so `ai64-demo` can restart any core while the others keep going.

## The spinlock tally

Every 10 ms each node takes hardware spinlock 255, adds one to a shared total and one to its own count, and frees the lock. The adds are plain loads and stores to uncached DDR, so without the lock two cores would lose each other's adds. The viewer takes the lock too, and checks that the twelve counts sum to the total.

## Each core's view of the memory


| Core | The fabric | The GTC, mailbox, and spinlock |
| --- | --- | --- |
| R5F | MPU region 7, normal non-cacheable (`mpu.c`) | in the low 2 GiB the MPU maps as device memory |
| C66x | MAR register for `0xA9xxxxxx` cleared, which turns off the XMC prefetch it resets with | the mailbox and spinlock directly. The GTC, below `0x0C000000`, is the C66x's own internal space, so MPAX segment 2 maps a logical page at `0x7FFFF000` onto it (`dsp/c66x/fabric.c`). |
| C7x | 32 level-3 MMU pages, normal non-cacheable (`dsp/c7x/mmu.c`) | the device-memory GiB at `0` |
| RTU | its global address; a message moves in one or two `LBBO`/`SBBO` bursts | the same |
| Linux | `/dev/mem` | `/dev/mem` |

## What you see

The `fabric` window, `ai64 fabric`, redraws once a second. On the Yocto image, with every core running:

```
fabric: one-way latency in us, from the row's node to the column's, over the last 1.0 s
         linux r5f0_0 r5f0_1 r5f1_0 r5f1_1  c66x0  c66x1    c7x g0rtu0 g0rtu1 g1rtu0 g1rtu1
linux        .    7.2    7.2    5.1    5.1   10.4    8.3    3.3    4.1    4.4   51.2   57.5
r5f0_0    61.7      .    6.7    5.5    5.5    8.2    7.4    3.8    4.7    5.6   56.5   53.7
r5f0_1    40.6    6.4      .    5.3    5.2    8.4    9.3    3.3    4.5    4.9   57.2   54.4
r5f1_0    53.9    6.8    6.8      .    5.5    8.2    9.0    3.5    4.4    4.7   53.7   61.0
r5f1_1    50.0    7.0    6.5    5.2      .    8.9    9.7    3.2    4.1    4.3   54.3   57.5
c66x0     50.1    5.8    6.7    4.1    4.5      .    7.8    2.7    3.4    3.7   55.2   50.1
c66x1     44.7    6.0    5.9    4.2    4.5    7.2      .    2.5    3.8    3.9   46.9   57.2
c7x       57.4    5.6    4.8    3.9    4.1    6.7    7.5      .    3.0    3.3   50.5   52.3
g0rtu0    57.5    6.4    5.8    4.5    4.7    7.3    9.7    2.7      .    3.8   56.9   53.7
g0rtu1    57.8    6.0    5.5    4.6    4.4    9.0    8.7   10.3    3.6      .   54.4   53.6
g1rtu0    74.3    6.4    8.5    4.9    6.9   10.6   10.5    2.7    5.8    4.0      .   51.7
g1rtu1    54.7    6.4    7.7    4.4    7.1    5.4    8.0    2.6    5.4    3.9   57.6      .

node    starts   polls/s  pongs/s    rtt us  rtt min to max us timeouts errors  passed/s
linux       11     12331     1100      87.5     59.4 to 1637.3        0      0       100
r5f0_0      11    285821     1100      29.6      5.9 to 1590.4        0      0         0
r5f0_1      11    314315     1100      23.4      4.9 to 1268.6        0      0       100
r5f1_0      11    380581     1101      24.0      4.9 to 1132.5        0      0       100
r5f1_1      11    385050     1100      23.4      4.6 to 1345.5        0      0       100
c66x0       11   1104419     1100      22.5      3.9 to  842.8        0      0       100
c66x1       11   1099597     1100      22.2      4.2 to 1398.2        0      0       100
c7x         11   1058320     1100      20.8      4.0 to 1465.9        0      0       100
g0rtu0      11    612849     1100      23.0      4.1 to 1285.6        0      0       100
g0rtu1      11    583335     1101      23.7      4.4 to 1496.4        0      0       100
g1rtu0      11      9699     1100     107.5    102.5 to 1232.1        0      0       100
g1rtu1      11      9719     1099     105.7    102.5 to  929.9        0      0       100

tour from r5f0_0 round every node that answers its pings: 100 laps/s, 238.6 us a lap (105.7 to 1206.7), errors 0, timeouts 6
  r5f0_0 >8.2> c66x0 >4.5> g0rtu0 >5.6> r5f1_0 >4.1> c7x >85.7> g1rtu0 >7.3> r5f0_1 >68.0> c66x1 >4.9> g0rtu1 >7.6> r5f1_1 >6.5> g1rtu1 >11.6> linux >4.5> r5f0_0

tally under hardware spinlock 255: 111209 adds, and the 12 counts sum to 111209
```

- **The matrix.** Between any two of the R5Fs, DSPs, and ICSSG0's RTUs a message takes about 2.5 to 11 µs, reading it included. The C7x, whose main loop is shortest, is the quickest to notice one, at 2.5 to 4 µs but for one 10.3; the C66x cores, the slowest, at 5.4 to 10.6 µs. Linux sleeps between looks, so a message to it takes about 40 to 75 µs. ICSSG1's two RTUs look at their inbox only every 100 µs, so as not to load ICSSG1's port to the SoC while the analyzer writes through it, and a message to either waits about 50 µs, half of that, on average.
- **The nodes.** `starts` counts each node's starts since the board booted: this one had just run `tests/run.sh soak 10`. `rtt us` is the mean round trip to all of the node's peers, which Linux and ICSSG1's RTUs pull up; ICSSG1's own are about 106 µs. The minimum and maximum are the fastest and slowest single round trips in the last second: here every node had one of 0.8 to 1.6 ms in that second. An earlier capture, before ICSSG1 ran the logic analyzer, had none over 200 µs; where the slow ones come from has not been looked into.
- **The tour.** Each `>µs>` is one hop, from the last node reading the message to the next reading it; a hop into one of ICSSG1's RTUs waits for its next look, up to 100 µs. R5F0_0 started before the DSPs and the ICSSG cores in that `ai64-demo` run, so its first tours ran into cores that were restarting: those are the 6 timeouts.
- **The tally.** Every add from all twelve nodes is in the total, so the lock held.

The merged console prints one `[Fabric]` line every 10 s instead (`ai64 fabric lines`).

## A route with an action

On the board, while the demo runs:

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

## Restarts

`tests/run.sh soak 10` on the Yocto image ran `ai64-demo` 10 times in a row, stopping and starting every demo core each time while the others kept talking. Every run came back with all 16 demo cores running and the free ones stopped, RTU0 answering over RPMsg, both ICSSG0 PWM slices running, the pipeline trained with nothing wrong, the analyzer's channels reading the demo's signals, no fabric timeouts or errors, the tally agreeing, and no kernel errors after all ten. Before ICSSG1 held the analyzer, with ten nodes and the multicore scheduler there, `ai64-demo` ran 10 times on the Debian image and 20 on the Yocto image the same way, and routes with `pwm` actions changed the ICSSG PWM and P9.25 alike on both.

## What it costs

Every node except Linux now spins on its inbox, so no core sits in WFI any more. The ICSSG0 RTUs' polling does not show in their slices' edges: with both RTUs and the whole fabric running, the software slice's latest edge was 24 ns late. And a core that is always on the bus must not be stopped in the middle of an access: putting a busy R5F in reset on the BeagleBoard kernel wedged the device manager (`ti-sci ... Mbox timedout in resp(caller: k3_r5_rproc_stop ...)`), and the SoC locked up a few seconds later. TI's kernel sends each core `RP_MBOX_SHUTDOWN` first, and the Zephyr apps park in WFI with their interrupts masked; on the BeagleBoard kernel `ai64-demo` now sends it with `ai64 shutdown`, as it already did for the DSPs.
