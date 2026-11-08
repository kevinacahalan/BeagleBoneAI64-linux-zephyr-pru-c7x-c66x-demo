# Board tests

These check a board running the demo, from the PC, over SSH:

```
AI64_SSH=USER@BOARD tests/run.sh check
AI64_SSH=USER@BOARD tests/run.sh soak [runs]
AI64_SSH=USER@BOARD tests/run.sh routes
AI64_SSH=USER@BOARD tests/run.sh stress
```

`run.sh` copies `board/` to `/tmp/ai64-tests` on the board and runs the test there as root, with `sudo` if `USER` is not root, so it may ask for the board's password. The board needs the demo installed: `install-beagle.sh` on Debian, or the Yocto image. Set `AI64_SSH_OPTS` to pass options to `ssh` and `scp`, for example `-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no` for a card that was just written and has new host keys.

Each check prints a line, `ok` or `FAIL`, and the test ends with `PASS` or how many failed, and exits non-zero if any did.

| Test | What it does |
| --- | --- |
| `check` | checks the demo as it runs now; start it first with `ai64-demo --tmux` |
| `soak [runs]` | restarts the demo `runs` times (5 unless you say), every core each time, and checks each run; then checks the kernel log for stops that timed out, oopses, and BUGs |
| `routes` | sends fabric routes with actions through cores of every kind, and checks that `ai64 pwm` shows each PWM generator running what was asked; then puts the steps back and starts the pipeline again |
| `stress` | the logic analyzer against a fast test signal: R5F0_1's EHRPWM4 on P9.25, jumpered to ch12, at periods from 2 µs down to 40 ns; at each, `ai64 la rate` checks that RTU1 copied every record PRU1 wrote (at least 10 million a second at 200 ns) and a reader took every one, and at the fastest that no two records came closer than the PRU's loop allows and that edge mode counts its late edges; then raw mode, PRU1 sampling ch12 every 8 ns, at 10 million edges a second with no gap in the stream, and at 62.5 MHz, past it, holes marked and every period between them 16 ns; a 50 ms burst of ch10 to ch12 at 62.5 MHz, with all but 0.1% of the blocks and every edge, and the mode put back after; dump mode at 125 MS/s, followed continuously, at 10 million edges a second and never a buffer behind; and slice 1's dead band at 24 ns read as exactly 24 every time; then edge mode again, and puts the steps back and starts the pipeline again |

What `check`, and each run of `soak`, looks at:

- **The `linux` window's startup script**: RTU0 echoes `ping` over RPMsg; each R5F core answers `say` over eRPC; R5F0_0 checks the bulk buffer and writes SPI7; R5F1_0 passes `spi` to R5F0_0 over `ipc_service`; the monitor lost no pings; R5F0_0's UDMA feeds the C7x's MMA, and the result matches the A72; the pipeline trains to 100%; and no line says `failed`, `link down`, or `did not`.
- **Every core in `ai64 cores`** runs, with its own firmware.
- **The pipeline** (`ai64 pipe once`): no window named wrong since training, and no stage lost a window or failed one.
- **The fabric**, from its window: the spinlock tally agrees, and no node timed out or saw an error in the last second.
- **The DSPs**: FIR jobs on both C66x cores and the C7x, and the MMA layer, all match the A72, while the pipeline runs on the same cores.
- **The logic analyzer** on ICSSG1 (`ai64 la`): both streams with nothing lost, the square wave at 1 kHz, the jumpered PWM channels at 15 kHz on average over their steps, slice 1's dead band at 1000 ns (`la measure`), Tx_PRU0's trigger on the square wave firing about 1000 times a second, a mark reaching the stream through the fabric, and the MCP server's `la_measure` answering over JSON-RPC. It needs [the jumpers](../pru/logic_analyzer/README.md#the-channels).
- **The pads** (`ai64 pads`), and one web server.

The tests are shell scripts for `/bin/sh` on the board, BusyBox's included, and need `tmux`, which the demo needs anyway. `board/lib.sh` has the checks; add one there, and it runs in both `check` and `soak`.
