# Extra demos

Demos the main demo no longer runs, kept for reference. Each still builds with the rest of the repo, and `ai64-demo` runs one in place of the default when asked.

| Demo | Runs on | Start it with | What it shows |
| --- | --- | --- | --- |
| [`icssg1-multicore-scheduler/`](icssg1-multicore-scheduler/README.md) | ICSSG1's six cores | `ai64-demo --icssg1 scheduler` | TI's open-pru `multicore_scheduler`, ported to J721E: IEP0 compares start a task on each core in turn, 4 ns apart, and the tasks drive P8.17. `ai64 sched` reads the schedule back. |

An extra demo's ICSSG project is laid out like the ones in `pru/`, includes the same `pru/pru_rules.mak`, and is built by `pru/Makefile` into `pru/build/`, so `./beagle-linux/scripts/build.sh pru`, the Debian bundle, and the Yocto image all carry its images. Its pins stay in the demo's overlay.
