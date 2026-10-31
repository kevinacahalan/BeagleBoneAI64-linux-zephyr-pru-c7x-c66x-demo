# ai64, the Linux side

`ai64` is one program with a subcommand for each window `ai64-demo` opens, and for each check you can run by hand. Each subcommand is a file of its own, with a `<name>_main()` that `ai64.c` calls; `ai64 -h` lists them.

| Subcommand | File | What it does |
| --- | --- | --- |
| `linux <fifo-dir>` | `linux_window.c` | the `linux` window: the startup script, then the commands you type, passed to each core's window |
| `erpc [core] <rpmsg-dev> <fifo>` | `erpc_window.c` | an R5F core's window: eRPC calls to Zephyr on that core; R5F0_0's also writes the bulk buffer for `chunk` and `dma` |
| `peer <label> <rpmsg-dev> <fifo>` | `rtu0_window.c` | the `rtu0` window: RPMsg text to and from ICSSG0 RTU0 |
| `pwm [seconds\|once]` | `pwm.c` | the `pwm` window: what each of the four PWM generators runs, from its status record |
| `sched [seconds\|once]` | `sched.c` | the `sched` window: ICSSG1's schedule, read from its registers |
| `fabric ...` | `fabric.c` | the `fabric` window, Linux's node on the fabric, and `fabric route` |
| `pipe ...` | `pipe.c` | the `pipe` window, and `pipe start`, which trains the pipeline's layer |
| `web [port]` | `web.c`, `web_page.S`, `web/index.html` | the `web` window: the live page, and its `/status.json` |
| `cores` | `cores.c` | every demo core: label, remoteproc name, firmware, fabric node |
| `pads` | `pads.c` | checks the pin mux of every demo pin |
| `trace <core>` | `trace.c` | follows a core's remoteproc trace log through `/dev/mem` |
| `shutdown <core>` | `shutdown.c` | sends a core `RP_MBOX_SHUTDOWN`, as TI's kernel does before a stop |
| `dsp <core> ...` | `dsp.c` | FIR jobs on a DSP, checked against the A72 |
| `mma [rows] [runs]` | `mma.c` | the int8 layer on the C7x's MMA and in C, checked against the A72 |
| `bench <rpmsg-dev>` | `bench.c` | times eRPC round trips to one R5F core |

What they share:

- **`cores.c`**, the table of every demo core: its label (`r5f0_1`, `g0rtu1`, `c7x`), remoteproc name, firmware, group, fabric node, Linux's mailbox FIFO to it, and, for a DSP, its job block and L2 SRAM. `ai64 cores` prints it; `ai64-demo` starts the cores from it, and `ai64 web` sends it to the page. A `<core>` argument is a label or a remoteproc name.
- **`util.c`**: `/dev/mem` mappings, remoteproc's sysfs, the clock, and the reply FIFO a window writes its result lines to for the `linux` window.
- **`ai64.h`**, which declares both, and every subcommand.
- **`ai64_erpc_linux.cpp`**, the eRPC client over the RPMsg file descriptor, on the generated code in `zephyr/erpc/shim/`.

`beagle-linux/scripts/build.sh linux` cross-compiles it in the container (`Makefile`); the Yocto recipe builds the same `Makefile`. Everything that maps `/dev/mem` needs root.
