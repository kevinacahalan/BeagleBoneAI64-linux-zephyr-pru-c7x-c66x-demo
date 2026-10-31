# The fabric

The demo's other links go through Linux, or join two cores that sit next to each other. The fabric joins every demo core that talks directly to every other one, Linux included: ten nodes, 90 one-way paths, and no Linux in any of them. One header, `zephyr/include/ai64_fabric.h`, holds the layout and the whole protocol, and every image compiles it: Zephyr with GCC, the DSPs with TI's C6000 and C7000 compilers, the RTUs with clpru, and `ai64` with the Linux compiler. Each image only adds a few lines of glue: how to read the clock, and how it maps the memory.

| Node | Name | Core | When it looks at its inbox |
| --- | --- | --- | --- |
| 0 | `linux` | A72, while `ai64 fabric` runs | every 20 µs or so, between sleeps |
| 1–4 | `r5f0_0`, `r5f0_1`, `r5f1_0`, `r5f1_1` | Zephyr (`zephyr/r5/common/src/fabric.c`) | in a thread at the lowest priority, which yields after each pass |
| 5, 6 | `c66x0`, `c66x1` | C66x | every pass of its main loop |
| 7 | `c7x` | C7x | every pass of its main loop |
| 8 | `g0rtu0` | ICSSG0 RTU0 (`pru/pwm/firmware/pwm_rtu.c`) | every pass of its main loop, next to its RPMsg check |
| 9 | `g0rtu1` | ICSSG0 RTU1 (the same source) | every pass of its main loop |

The ICSSG0 PRUs and Tx_PRUs are not nodes: each slice's RTU talks for them ([The ICSSG firmware](../pru/README.md#the-icssg-firmware)). Nor is ICSSG1, whose six cores all run scheduler tasks. The layout has room for twelve nodes (`AI64_FAB_SLOTS`), so two more can join without moving anything.

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
| R5F | MPU region 7, normal non-cacheable (`mpu.c`) | in the low 2 GiB the MPU maps as device memory |
| C66x | MAR register for `0xA9xxxxxx` cleared, which turns off the XMC prefetch it resets with | the mailbox and spinlock directly. The GTC, below `0x0C000000`, is the C66x's own internal space, so MPAX segment 2 maps a logical page at `0x7FFFF000` onto it (`c66x/main.c`). |
| C7x | 32 level-3 MMU pages, normal non-cacheable (`dsp/c7x/mmu.c`) | the device-memory GiB at `0` |
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

**What it costs.** Every node except Linux now spins on its inbox, so no core sits in WFI any more. The ICSSG0 RTUs' polling does not show in their slices' edges: with both RTUs and the whole fabric running, the software slice's latest edge was 24 ns late. And a core that is always on the bus must not be stopped in the middle of an access: putting a busy R5F in reset on the BeagleBoard kernel wedged the device manager (`ti-sci ... Mbox timedout in resp(caller: k3_r5_rproc_stop ...)`), and the SoC locked up a few seconds later. TI's kernel sends each core `RP_MBOX_SHUTDOWN` first, and the Zephyr apps park in WFI with their interrupts masked; on the BeagleBoard kernel `ai64-demo` now sends it with `ai64 shutdown`, as it already did for the DSPs.
