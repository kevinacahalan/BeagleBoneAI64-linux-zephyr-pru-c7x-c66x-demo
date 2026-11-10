# The fabric

The demo's other links go through Linux, or join two cores that sit next to each other. The fabric joins every demo core that talks directly to every other one, Linux included: twelve nodes, 132 one-way paths, and no Linux in any of them. One header, `zephyr/include/ai64_fabric.h`, holds the layout and the whole protocol, and every image compiles it: Zephyr with GCC, the DSPs with TI's C6000 and C7000 compilers, the RTUs with clpru, and `ai64` with the Linux compiler. Each image only adds a few lines of glue: how to read the clock, and how it maps the memory.

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
         linux r5f0_0 r5f0_1 r5f1_0 r5f1_1  c66x0  c66x1    c7x g0rtu0 g0rtu1 g1rtu0 g1rtu1
linux        .    6.4    6.3    5.1    4.9    8.7    6.6    3.1    4.1    4.5    4.8    4.7
r5f0_0    46.5      .    7.4    5.3    5.7   10.7    9.9    4.0    4.9    5.4    5.7    5.4
r5f0_1    46.2    9.2      .    7.2    5.6    8.5   10.4    3.8    5.3    5.3    5.2    5.1
r5f1_0    49.9    7.2    6.7      .    5.2    9.3    9.8    3.6    4.9    4.9    5.1    5.0
r5f1_1    41.5    7.0    7.1    5.5      .    8.5    9.0    3.8    4.7    5.1    5.5    5.1
c66x0     34.6    5.8    7.2    4.7    4.7      .    8.0    2.9    4.0    4.4    4.6    4.4
c66x1     37.7    6.8    6.7    4.9    4.7    9.7      .    3.2    4.3    4.3    5.2    4.3
c7x       33.9    5.6    5.6    3.8    4.5    7.5    7.9      .    3.5    3.6    4.7    4.1
g0rtu0    42.8    6.6    5.7    4.9    5.1    8.3    7.8    3.1      .    4.3    4.8    4.8
g0rtu1    46.2    6.7    6.3    4.9    4.7    7.5    8.7    3.2    4.1      .    4.7    4.9
g1rtu0    45.6    6.0    6.7    4.9    5.4    8.2    9.3    3.1    4.0    4.3      .    4.8
g1rtu1    42.0    6.4    6.3    5.1    4.9    8.4    8.8    3.1    4.4    4.4    5.0      .

node    starts   polls/s  pongs/s    rtt us  rtt min to max us timeouts errors  passed/s
linux       15     13130     1100      77.8     37.7 to  156.3        0      0       100
r5f0_0      15    262788     1100      18.6      6.3 to  124.9        0      0         0
r5f0_1      15    257106     1100      16.3      5.9 to  110.3        0      0       100
r5f1_0      15    328239     1100      14.9      5.9 to  102.3        0      0       100
r5f1_1      15    360885     1100      13.9      4.9 to  101.1        0      0       100
c66x0       15    804658     1100      12.3      4.1 to  127.1        0      0       100
c66x1       15    802735     1100      13.3      4.8 to  170.9        0      0       100
c7x         15    801001     1100      10.7      4.2 to   96.9        0      0       100
g0rtu0      15    442011     1100      13.0      4.9 to   98.5        0      0       100
g0rtu1      15    420796     1100      13.6      5.3 to   97.4        0      0       100
g1rtu0       6    305480     1100      14.0      4.5 to  784.5        0      0       100
g1rtu1       6    289146     1100      13.6      4.5 to   99.0        0      0       100

tour from r5f0_0 round every node that answers its pings: 100 laps/s, 125.2 us a lap (71.3 to 192.1), errors 0, timeouts 7
  r5f0_0 >46.3> c66x0 >4.2> g0rtu0 >5.3> r5f1_0 >4.2> c7x >4.2> g1rtu0 >6.7> r5f0_1 >9.4> c66x1 >5.0> g0rtu1 >5.5> r5f1_1 >10.6> g1rtu1 >44.6> linux >6.4> r5f0_0

tally under hardware spinlock 255: 117315 adds, and the 12 counts sum to 117315
```

- **The matrix.** Between any two of the R5Fs, DSPs, and RTUs a message takes 3 to 11 µs, reading it included. (These were measured while ICSSG1's RTUs still polled every pass of their loop; now they poll every 100 µs, and their round trips average about 105 µs: 102.5 µs at the least, on the Debian image.) The C7x, whose main loop is shortest, is the quickest to notice one; the C66x cores, the slowest, at 7.5 to 10.7 µs. Linux sleeps between looks, so a message to it takes about 35 to 50 µs.
- **The nodes.** `starts` counts each node's starts since the board booted. `rtt us` is the mean round trip to all of the node's peers, which Linux pulls up; the minimum and maximum are the fastest and slowest single round trips in the last second. `passed/s` is tour hops, one per node per lap.
- **The tour.** Each `>µs>` is one hop, from the last node reading the message to the next reading it. R5F0_0 started before the DSPs and the ICSSG cores in that `ai64-demo` run, so its first tours ran into cores that were restarting: those are the 7 timeouts.
- **The tally.** Every add from all twelve nodes is in the total, so the lock held.

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

**Restarts.** `ai64-demo` ran 10 times in a row on the Debian image and 15 times on the Yocto image, then 5 more on a freshly built Yocto image, stopping and starting all 19 demo cores each time while the others kept talking. Every run came back with all ten nodes up, RTU0 answering over RPMsg, both ICSSG0 PWM slices running with no late edges, all six ICSSG1 tasks firing, no fabric timeouts or errors in the 10 s after, the tally agreeing, and no kernel errors. Routes with `pwm` actions changed the ICSSG PWM and P9.25 the same way on both. Those runs had ten nodes, with the multicore scheduler on ICSSG1; with the analyzer's two RTUs as nodes 10 and 11, `tests/run.sh soak` restarts every core and checks the same, all twelve up.

**What it costs.** Every node except Linux now spins on its inbox, so no core sits in WFI any more. The ICSSG0 RTUs' polling does not show in their slices' edges: with both RTUs and the whole fabric running, the software slice's latest edge was 24 ns late. And a core that is always on the bus must not be stopped in the middle of an access: putting a busy R5F in reset on the BeagleBoard kernel wedged the device manager (`ti-sci ... Mbox timedout in resp(caller: k3_r5_rproc_stop ...)`), and the SoC locked up a few seconds later. TI's kernel sends each core `RP_MBOX_SHUTDOWN` first, and the Zephyr apps park in WFI with their interrupts masked; on the BeagleBoard kernel `ai64-demo` now sends it with `ai64 shutdown`, as it already did for the DSPs.
