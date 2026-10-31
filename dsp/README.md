# The DSPs

The two C66x cores and the C7x: what each image runs, the jobs Linux hands them, the C7x's MMU and its Matrix Multiply Accelerator, and how Linux stops them.

## The source

Each image is laid out like an R5F app ([`zephyr/r5/README.md`](../zephyr/r5/README.md)): `main.c` says what the core runs, starts each part, and polls them all from one loop, and each part is a file of its own.

| File | C66x (`c66x/`) | C7x (`c7x/`) |
| --- | --- | --- |
| `main.c` | the trace log, the shutdown request, the loop | the same, after `mmu.c` turns the MMU on |
| `jobs.c` | Linux's job block: FIR jobs | Linux's job block: FIR jobs and the int8 layer |
| `fabric.c` | the fabric node, through an MPAX window for the GTC | the fabric node |
| `pipe.c` | the pipeline's FFT: C66x 0 the even windows, C66x 1 the odd | the pipeline's classifier |
| `mmu.c`, `regs.asm` | | the MMU's page tables and cache sizes, and the control register accessors |
| `mma.c` | | the int8 layer on the Matrix Multiply Accelerator, and in plain C |
| `c66x.cmd`, `c7x.cmd` | the linker script | the linker script |

`include/` has what both share: the resource table and trace log (`ai64_dsp_trace.h`), the shutdown handler (`ai64_dsp_mbox.h`), and the job block (`ai64_dsp_job.h`), which `ai64` on Linux includes too. `make` builds both images into `build/`; `beagle-linux/scripts/build.sh dsp` runs it in the container with TI's compilers.

## The DSPs: trace and jobs

Each DSP runs a small image from `dsp/`. It fills in a resource table with one entry, a trace buffer, writes `hello` there, and then logs one line every 10 seconds, timed with the core's cycle counter. When the buffer fills up, the log starts again after the hello line. Besides its own mailbox ([why](#restarting-the-c7x)), a DSP only touches what the [fabric](../docs/fabric.md) and the [pipeline](../docs/pipeline.md#the-pipeline-p831-to-p925) use: the two C66x cores take turns at the pipeline's FFT, and the C7x is its classifier. The C66x images set up no cache; the C7x image turns on its MMU and caches first ([below](#the-c7x-mmu-and-caches)).

| Core | remoteproc | Image | Where it runs | Boot address |
| --- | --- | --- | --- | --- |
| C66x 0 | `4d80800000.dsp` | `ai64-c66x.elf` | its own L2 SRAM, local `0x00800000` | `0x00800000` (1 KiB aligned) |
| C66x 1 | `4d81800000.dsp` | `ai64-c66x.elf` | its own L2 SRAM, local `0x00800000` | `0x00800000` |
| C7x | `64800000.dsp` | `ai64-c7x.elf` | its DDR carveout, `0xa8200000` | `0xa8200000` (2 MiB aligned) |

Both C66x cores run the same image: each sees its own L2 SRAM at `0x00800000`, and `DNUM` tells them apart. The C66x images run from L2 SRAM, and only reach DDR for the fabric and the pipeline, because the board's device tree crosses the two C66x DDR carveouts: `4d80800000.dsp` gets `memory@a7000000` for DMA but `memory@a6100000` for firmware, and `4d81800000.dsp` the other way round.

The compilers are TI's C6000 CGT 8.3.12 and C7000 CGT 1.2.0.STS, the newest versions TI serves without a login (`containers/dsp-cgt/`).

The windows read the trace buffers with `ai64 trace`. It finds the buffer from the resource table in the core's firmware file, and reads it through `/dev/mem`, so it needs neither root nor debugfs.

## Linux to the DSPs: FIR jobs

Each DSP polls a 64 KiB job block in its own L2 SRAM (`dsp/include/ai64_dsp_job.h`). Linux writes the filter taps, the input samples, and the job size through `/dev/mem`, then bumps a request number. The DSP runs a 16-bit fixed-point FIR filter, writes the output and its cycle count, and copies the request number into a done field. On a C66x, L1D snoops writes to L2 SRAM from outside, so neither side needs cache maintenance. The C7x maps the block's first 4 KiB, the header both sides poll, non-cacheable, and writes L1D back around each job.

| Core | Job block (Linux address) | Core's own address |
| --- | --- | --- |
| C66x 0 | `0x4d80838000` | `0x00838000` |
| C66x 1 | `0x4d81838000` | `0x00838000` |
| C7x | `0x64800000` | `0x64800000` |

`sudo ai64 dsp <core> [count] [taps] [runs]` (`c66x0`, `c66x1`, `c7x`, or a remoteproc name) sends jobs with random data, checks each output against the same filter run on the A72, and prints the timings:

```
$ sudo ai64 dsp 4d80800000.dsp
4d80800000.dsp: 20 FIR jobs, 4096 samples x 32 taps, all match the A72
  DSP filter 28222 cycles = 20.9 us; A72 waited 21.8 us; empty job 0.9 us
  the same filter on one A72 core: 188.1 us
```

```
$ sudo ai64 dsp 64800000.dsp
64800000.dsp: 20 FIR jobs, 4096 samples x 32 taps, all match the A72
  DSP filter 9177 cycles = 9.2 us; A72 waited 9.7 us; empty job 1.3 us
  the same filter on one A72 core: 199.0 us
```

| Core | 4096 samples × 32 taps | 8192 × 64 |
| --- | --- | --- |
| One A72 core, plain C at `-O2` | about 190 µs | about 790 µs |
| C66x, plain C at `-O3` (before) | 92 µs | 336 µs |
| C66x, `DDOTP4H` intrinsics | 21 µs | 72 µs |
| C7x, one tap per step (before) | 19 µs | 60 µs |
| C7x, `VDOTP2HW` on 128 outputs | 9.2 µs | 26 µs |

Both DSPs run their own path in `ai64_dsp_job.h`, with the plain C loop left for the last few outputs. Each keeps the taps outside and the outputs inside, so one tap load serves many outputs:

- **C66x.** One pass over the outputs per 8 taps, 8 outputs at a time, adding into a table of 32-bit sums. Four aligned 64-bit loads hold 16 samples; shifting them by one to three samples gives each output its 4 samples, and each `DDOTP4H` is two 4-tap dot products. The outputs go in chunks of 2048, so that a chunk's sums and samples stay in the 32 KiB L1D across the passes; without that, 8192 × 64 took 101 µs. With the taps inside instead, each group of 8 outputs paid for starting and ending the tap loop, and 4096 × 32 took 35 µs.
- **C7x.** 128 outputs at a time, two taps per step, with the sums in registers. `VDOTP2HW` multiplies each pair of samples by a pair of taps and adds the two products into one 32-bit lane. A 32-sample load from `x[n + k]` gives the even outputs their pairs for taps `k` and `k + 1`, and the load one sample on gives the odd outputs theirs. With 32 outputs a block, 4096 × 32 took 17.8 µs; 128 spreads the cost of the tap loop. The plain C loop took 225 µs: the compiler vectorizes across the taps, so a 32-tap filter is two vectors per output, and loop overhead costs more than the multiplies. The C7x reads its L2 SRAM much slower than cached DDR, so it copies the input into its own DDR first; without that copy the same job takes 38 µs.

`ai64 dsp` refuses a DSP that is not running. Reading a powered-off DSP's L2 SRAM hangs the SoC.

## The C7x MMU and caches

`dsp/c7x/mmu.c` builds identity-mapped page tables and turns the MMU on before anything else runs. It sizes the caches as TI's vision_apps does: L1D all cache (32 KiB), and 64 KiB of the 512 KiB L2 as cache, which leaves 448 KiB of SRAM from `0x64800000`.

| Region | Size | Attributes | Holds |
| --- | --- | --- | --- |
| `0x00000000` | 1 GiB | device | the mailbox and every other peripheral |
| `0x40000000` | 1 GiB | device, apart from the L2 SRAM pages below | |
| `0x64800000` | 4 KiB | normal, non-cacheable | the job block header |
| `0x64801000` | 60 KiB | normal, write-back | the rest of the job block |
| `0xa8000000` | 2 MiB | normal, non-cacheable | the DMA pool, then the trace buffer and resource table at `0xa8100000` |
| `0xa8200000` | 14 MiB | normal, write-back | code, data, stack, and the page tables |
| `0xa9000000` | 128 KiB | normal, non-cacheable | the fabric |

Anything else faults. The register values and their order follow the secure startup in TI's PDK (`ti/csl/arch/c7x/src/Mmu.c`), which flushes no cache and no TLB before the MMU goes on. One difference: the table base goes in `TBR0` as well as `TBR0_S`. With only `TBR0_S`, the core stops the moment the MMU is on, even though the core starts in secure supervisor mode.

The trace buffer and the resource table moved from the code region into the non-cacheable block, because Linux reads them while the core runs. The MMU alone took the plain C loop only from 225 to 194 µs. Almost all of the gain came from the vector code.

## The C7x's Matrix Multiply Accelerator

The TDA4VM's C7x has a Matrix Multiply Accelerator (MMA) beside it: given a 64-element int8 vector A, it multiplies it by a 64 × 64 int8 matrix B held in one of its two B banks, and writes the 64 sums, 32-bit, to a row of one of its two C banks; a C bank holds 16 such rows. Its X stage reads a row back out through a transfer buffer, shifted, rounded, and saturated to 8 bits, with ReLU if asked for. The C7x drives it with instructions of its own, which the C7000 compiler's `c7x_mma.h` exposes as intrinsics: `__HWAOPEN` with a configuration, `__HWALDB` for a row of B, `__HWALDA` and `__HWAOP` for a row of A, `__HWAXFER` and `__HWARCV` for a row of results.

`dsp/c7x/mma.c` uses it for an int8 fully connected layer, 64 inputs to 64 outputs, on a batch of up to 256 rows: `y = round((a x b) >> shift)`, saturated to int8, or with ReLU to uint8 (0 to 255). Linux hands it the job through the C7x's job block, like the FIR jobs:

```
$ sudo ai64 mma
C7x: 20 int8 layers, 256 rows x 64 inputs x 64 outputs (1048576 multiply-adds), all match the A72
  on the MMA:   1944 cycles = 1.94 us, 540 multiply-adds a cycle; A72 waited 3.0 us
  in C on the C7x: 161099 cycles = 161.10 us, 7 multiply-adds a cycle; A72 waited 162.4 us
  the same layer on one A72 core: 511.4 us
```

Every run has new data, shift, and ReLU, and both C7x results are checked byte for byte against the A72. The kernel:

- **Loads B once**, 64 `__HWALDB`, into B bank 0.
- **Goes 16 rows at a time, alternating C banks.** Each row is one `__HWALDA` and one `__HWAOPXFER`, which computes the row into one C bank while it transfers the same row of the block before out of the other, and `__HWARCV` takes it back to the C7x. The first block only computes; the last is read out after a fence.
- **Feeds A and B from the Streaming Engines**, SE0 and SE1, which fetch the rows ahead of use. With plain loads from L2 SRAM each row cost about 40 cycles, 11,000 for the batch; streamed, about 3, 1,900 for the batch, much of that the job's cache maintenance.
- **Builds and runs on a PC too.** TI's C7000 Host Emulation, in the compiler package, models the MMA: the same file compiles with `g++ -D__C7X_HOSTEM__` against `host_emulation/`, where `__HWAADV()` steps the model (on the C7x it is empty). Every configuration here was worked out there first, against a plain C reference.

Two things the hardware does that the emulator does not:

- **Signed outputs with ReLU.** Asked for int8 outputs with ReLU, the MMA leaves a sum that saturates below −128 at −128, where the emulator, and the header's description, apply ReLU after saturation and give 0. So with ReLU the layer's outputs are uint8, as a quantized network keeps them after a ReLU anyway.
- **Timing.** The emulator only moves when told to; on the C7x the transfers and receives wait for their data, so the kernel needs no delays of its own beyond the fence.

The plain C version is what the compiler makes of three loops for the C7x's own vector units, with no intrinsics: about 80 times slower here than the MMA, and still three times faster than an A72 core.

## Restarting the C7x

Both kernels' DSP drivers stop a C66x by asserting its local reset. A C71x has no local reset, so the driver powers the whole module off instead, and the device manager only allows that once the core is idle. Otherwise it refuses: `module-reset assert failed (-ENODEV)`, then `can't stop rproc: -19`, and the C7x keeps running.

TI's kernel, the one the Yocto image uses, sends `RP_MBOX_SHUTDOWN` through the core's mailbox before a stop, waits for `RP_MBOX_SHUTDOWN_ACK`, and then for the core to report that it is idle. The BeagleBoard kernel on the Debian image sends nothing. So both DSP images poll their receive FIFO, answer `RP_MBOX_SHUTDOWN` with the ACK, and execute `IDLE` with interrupts off (`dsp/include/ai64_dsp_mbox.h`), and `ai64 shutdown <core>` puts that message in the FIFO from Linux. On the BeagleBoard kernel, `ai64-demo` runs it before it stops each DSP. A C7x running an image without the handler still cannot be stopped; `ai64-demo` says so, and a reboot loads the new image.

The Zephyr images answer the same message, by masking their interrupts and parking in WFI. On the BeagleBoard kernel `ai64-demo` sends it to each R5F too, because a core that is busy on the bus when it is put in reset can wedge the device manager ([what it costs](../docs/fabric.md)). Neither kernel's R5F driver starts a core again after a stop without the patch that keeps it powered. On TI's kernel, the C66x needs a patch as well: it never reports WFI after it has acknowledged. [Two 6.18 kernels](../docs/kernels.md#two-618-kernels) lists every difference.
