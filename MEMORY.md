# Memory map

Every piece of memory more than one core touches, with who writes it and how each core caches it. The addresses are SoC addresses unless a row says otherwise. The headers are the source of truth; this collects them in one place:

- [`zephyr/include/ai64_shm.h`](zephyr/include/ai64_shm.h): the bulk buffer, the GTC, the PWM status record;
- [`zephyr/include/ai64_fabric.h`](zephyr/include/ai64_fabric.h): the fabric;
- [`zephyr/include/ai64_pipe.h`](zephyr/include/ai64_pipe.h): the pipeline;
- [`dsp/include/ai64_dsp_job.h`](dsp/include/ai64_dsp_job.h): the DSP job blocks;
- [`pru/pwm/firmware/pwm_map.h`](pru/pwm/firmware/pwm_map.h): ICSSG0's data RAM.

Most of it has build-time checks: the R5F's `mpu.c` checks the regions it maps against these headers, and the structures check their own sizes.

## DDR

The board's device tree reserves each core's carveout, and the demo's overlay reserves 1 MiB at `0xa9000000` for what the cores share, so Linux keeps out of both.

| Address | Size | What | Written by | Read by |
| --- | --- | --- | --- | --- |
| `0xa2000000` | 16 MiB | R5F0_0's carveout: vrings (1 MiB), resource table (1 MiB), then the image | Linux (vrings), R5F0_0 | Linux, R5F0_0 |
| `0xa2fef000` | 64 KiB | R5F0_0's bulk buffer (`struct ai64_bulk`), the end of its carveout | Linux | R5F0_0, and its UDMA |
| `0xa3000000` | 16 MiB | R5F0_1's carveout, laid out the same way | Linux, R5F0_1 | Linux, R5F0_1 |
| `0xa4000000` | 16 MiB | R5F1_0's carveout | Linux, R5F1_0 | Linux, R5F1_0 |
| `0xa5000000` | 16 MiB | R5F1_1's carveout | Linux, R5F1_1 | Linux, R5F1_1 |
| `0xa6000000`, `0xa7000000` | 16 MiB each | the C66x cores' carveouts; the images run from L2 SRAM and leave them unused ([why](dsp/README.md#the-dsps-trace-and-jobs)) | | |
| `0xa8000000` | 1 MiB | the C7x's DMA pool | | |
| `0xa8100000` | 1 MiB | the C7x's trace buffer and resource table | the C7x | Linux |
| `0xa8200000` | 14 MiB | the C7x's image: code, data, stack, page tables | Linux (load), the C7x | the C7x |
| `0xa9000000` | 96 KiB | the fabric: one 8 KiB page per node, twelve slots, ten used | each node's own page, its inbox by every sender | every node |
| `0xa9018000` | | the fabric's spinlock tally | every node, under hardware spinlock 255 | Linux |
| `0xa9020000` | 384 KiB | free, inside the reservation | | |
| `0xa9080000` | 4 KiB | R5F0_0's UDMA rings and descriptor (the `ipc_service` slot for core 0 to itself) | R5F0_0, the UDMA | R5F0_0, the UDMA |
| `0xa9081000` | 60 KiB | the R5F cores' `ipc_service` buffers: 4 KiB each way per pair (`common/ai64_ipc.dtsi`) | the sending core | the receiving core |
| `0xa9090000` | 64 KiB | the pipeline's block (`struct ai64_pipe`): Linux's control record, each stage's numbers, the score, the result ring, the weights, the feature log | Linux (control, weights), each stage (its own numbers), R5F0_1 (score), the C7x (results, log) | everyone; Linux shows it |
| `0xa90a0000` | 384 KiB | free, inside the reservation | | |

## On-chip RAM

| Address (SoC) | Core's own address | Size | What | Written by | Read by |
| --- | --- | --- | --- | --- | --- |
| `0x4d80800000` | `0x00800000` on C66x 0 | 192 KiB | C66x 0's image: code, data, trace buffer | Linux (load), C66x 0 | Linux (trace) |
| `0x4d80830000` | `0x00830000` | 32 KiB | C66x 0's pipeline input: a doorbell word and four window slots | R5F0_0's UDMA | C66x 0 |
| `0x4d80838000` | `0x00838000` | 64 KiB | C66x 0's job block | Linux (jobs), C66x 0 (results) | both |
| `0x4d81800000` … `0x4d81838000` | the same | | C66x 1's, laid out the same way | | |
| `0x64800000` | the same on the C7x | 64 KiB | the C7x's job block; its first 4 KiB is the header both sides poll | Linux (jobs), the C7x (results) | both |
| `0x64810000` | the same | 4 KiB | the pipeline's features: 16 rows, their tags, and each C66x's newest window | C66x 0 (even rows), C66x 1 (odd rows) | the C7x |
| `0x64811000` | the same | 12 KiB | the pipeline's weights, then the layer's outputs | the C7x | the C7x's MMA |
| `0x0b000000` | `0x0000` on slice 0's cores | 8 KiB | ICSSG0 slice 0's data RAM: PRU0, Tx_PRU0, RTU0, and their four IPC records (`pwm_map.h`) | each core its own part and records | the slice's three cores; Linux (trace) |
| `0x0b002000` | `0x0000` on slice 1's cores | 8 KiB | ICSSG0 slice 1's data RAM, the same way | | |
| `0x0b010000` | `0x10000` on ICSSG0's cores | 8 KiB used | the pipeline's window ring: 16 slots of 512 bytes, then PRU1's counts | PRU1 (samples), RTU1 (stamps) | R5F0_0's UDMA |
| `0x0b100000` | | | ICSSG1's RAMs: only the six scheduler cores use them | | |

## Registers every core shares

| Address | What | Who uses it |
| --- | --- | --- |
| `0x00a90000` | the GTC, the 200 MHz counter every core stamps with | everyone; the C66x through an MPAX window at `0x7ffff000` |
| `0x30e00000` | hardware spinlocks; lock 255 guards the fabric's tally | every fabric node |
| `0x31f81000` | mailbox cluster 1: Linux and R5F0_0, R5F0_1 (RPMsg kicks, shutdown) | Linux, the R5F cores |
| `0x31f82000` | cluster 2: Linux and R5F1_0, R5F1_1 | the same |
| `0x31f83000` | cluster 3: Linux and the C66x cores | Linux, the C66x cores |
| `0x31f84000` | cluster 4: Linux and the C7x | Linux, the C7x |
| `0x31f8a000` | cluster 10: the R5F cores' `ipc_service` doorbells; FIFO 15 is the C7x's doorbell to R5F0_1 for each class | the R5F cores, the C7x |
| `0x31f8b000` | cluster 11: the fabric's doorbells (FIFO n for node n), R5F0_0's `ipc_service` doorbells (FIFOs 12 to 14), and FIFO 15, RTU1's doorbell to R5F0_0 for each window | every fabric node |

Clusters 5 to 9 are not the demo's: the device manager's firmware listens on cluster 7, and leaves messages in 5, 6, 8, and 9 ([why that matters](zephyr/r5/README.md#r5f-to-r5f-ipc_service)).

## How each core sees shared memory

| Core | Its own memory | Shared DDR (fabric, `ipc_service`, pipeline, bulk buffer) | Registers |
| --- | --- | --- | --- |
| R5F | its carveout, write-through cached, the vrings and resource table not (`common/src/mpu.c`) | MPU regions, normal non-cacheable | the low 2 GiB, device memory |
| C66x | its L2 SRAM; L1D snoops writes from outside | the MAR registers of the fabric's and the C7x's pages cleared, so nothing is cached or prefetched | directly, but the GTC through MPAX |
| C7x | its DDR image write-back; the job block past its header write-back (`dsp/c7x/mmu.c`) | MMU pages, normal non-cacheable | the low 1 GiB, device memory |
| ICSSG | its data RAM and its ICSSG's shared RAM: no cache | the RTUs only, with `LBBO`/`SBBO` bursts; a PRU or Tx_PRU stays inside its ICSSG | the RTUs, and each Tx_PRU's one GTC read a period |
| Linux | | `/dev/mem`, mapped uncached (`O_SYNC`) | the same |
