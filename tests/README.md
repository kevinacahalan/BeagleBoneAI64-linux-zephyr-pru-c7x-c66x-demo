# Board tests

These check a board running the demo, from the PC, over SSH:

```
AI64_SSH=USER@BOARD tests/run.sh check
AI64_SSH=USER@BOARD tests/run.sh soak [runs]
AI64_SSH=USER@BOARD tests/run.sh routes
```

`run.sh` copies `board/` to `/tmp/ai64-tests` on the board and runs the test there as root, with `sudo` if `USER` is not root, so it may ask for the board's password. The board needs the demo installed: `install-beagle.sh` on Debian, or the Yocto image. Set `AI64_SSH_OPTS` to pass options to `ssh` and `scp`, for example `-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no` for a card that was just written and has new host keys.

Each check prints a line, `ok` or `FAIL`, and the test ends with `PASS` or how many failed, and exits non-zero if any did.

| Test | What it does |
| --- | --- |
| `check` | checks the demo as it runs now; start it first with `ai64-demo --tmux` |
| `soak [runs]` | restarts the demo `runs` times (5 unless you say), every core each time, and checks each run; then checks the kernel log for stops that timed out, oopses, and BUGs |
| `routes` | sends fabric routes with actions through cores of every kind, and checks that `ai64 pwm` shows each PWM generator running what was asked; then puts the steps back and starts the pipeline again |

What `check`, and each run of `soak`, looks at:

- **The `linux` window's startup script**: RTU0 echoes `ping` over RPMsg; each R5F core answers `say` over eRPC; R5F0_0 checks the bulk buffer and writes SPI7; R5F1_0 passes `spi` to R5F0_0 over `ipc_service`; the monitor lost no pings; R5F0_0's UDMA feeds the C7x's MMA, and the result matches the A72; the pipeline trains to 100%; and no line says `failed`, `link down`, or `did not`.
- **Every core in `ai64 cores`** runs, with its own firmware.
- **The pipeline** (`ai64 pipe once`): no window named wrong since training, and no stage lost a window or failed one.
- **The fabric**, from its window: the spinlock tally agrees, and no node timed out or saw an error in the last second.
- **The DSPs**: FIR jobs on both C66x cores and the C7x, and the MMA layer, all match the A72, while the pipeline runs on the same cores.
- **The pads** (`ai64 pads`), and one web server.

The tests are shell scripts for `/bin/sh` on the board, BusyBox's included, and need `tmux`, which the demo needs anyway. `board/lib.sh` has the checks; add one there, and it runs in both `check` and `soak`.
