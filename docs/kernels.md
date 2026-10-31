# Two 6.18 kernels

The Debian image and the Yocto image both run Linux 6.18, but not the same kernel:

- **BeagleBoard 6.18** (`6.18.55-arm64-k3-r59`), on the Debian image, is BeagleBoard.org's `arm64-k3` build. Its remoteproc and PRU drivers lack the additions TI keeps in its own tree.
- **TI 6.18** (`6.18.51-ti`), on the Yocto image, is TI's staging kernel, branch `ti-linux-6.18.y` of `ti-linux-kernel`, at the commit meta-ti pins. It carries TI's own remoteproc and PRU changes, and the image boots it with TI's U-Boot 2026.01. meta-ti selects both with `TI_PREFERRED_BSP = "ti-6_18"`. meta-beagle's own default for this board is BeagleBoard's TI-based 6.12.

Where they differ for this demo:

| | BeagleBoard 6.18 (Debian) | TI 6.18 (Yocto) |
| --- | --- | --- |
| RPMsg to an ICSSG core | No `.kick` and no vring interrupt in `pru_rproc`, so RTU0's RPMsg firmware does not start. The patched module adds them back. | Has both. Stock `pru_rproc`. |
| Shutdown request before a stop (`RP_MBOX_SHUTDOWN`) | Sends none. `ai64 shutdown` sends it to each R5F and DSP before `ai64-demo` stops it. | Sends it to every R5F and DSP, then waits for the ACK and for the core to report WFI. |
| Stopping an R5F | Powers the core off, which the device manager refuses. The core is stuck until a power cycle. The patched module keeps it powered. | The same power-off. Patch `0001` in `meta-ai64` keeps it powered. |
| Stopping a C66x | Asserts the local reset. Works as shipped. | Times out waiting for WFI, which a C66x never reports: without patch `0002`, `can't stop rproc: -110` and the core keeps running. The patch resets it once it has acknowledged. |
| Stopping the C7x | Works only after `ai64 shutdown`. | Works as shipped, through the shutdown request. |
| How the fixes get in | Two out-of-tree modules, built against the board's headers and installed in `/lib/modules/<kernel>/updates/`. The kernel marks them with taint flag `O`. Rebuild after every kernel upgrade. | Two patches applied to the kernel source in the Yocto build. |
| Device tree | BeagleBoard-DeviceTrees `v6.18.x`. It adds a pinmux file for BeagleBoard's overlays. | The kernel's own. The remote-core, ICSSG, and carveout nodes are the same as BeagleBoard's. |

Neither device tree gives any ICSSG core a vring interrupt, so on both images the overlay gives ICSSG0 RTU0 one, and neither reserves the fabric's DDR, so the overlay does that too. The firmware images, the overlay, and `ai64` are the same for both. `ai64-demo` tells the kernels apart by `-k3-` in `uname -r`, and only runs `ai64 shutdown` on the BeagleBoard one.

## Why Debian needs two patched modules

The BeagleBoard 6.18 kernel needs two fixes for this demo. `build.sh kmod` builds both against the board's own headers, and `install-beagle.sh` puts them in `/lib/modules/<kernel>/updates/`, which the kernel prefers.

**`pru_rproc`.** RPMsg is two ring buffers in shared memory, plus a doorbell in each direction. On a PRU core the Linux-to-PRU doorbell is the remoteproc `.kick` operation, which raises an ICSSG system event; the PRU-to-Linux doorbell is a "vring" interrupt. BeagleBoard's 6.18 `pru_rproc` has neither, and Linux refuses to start RPMsg firmware with `.kick method not defined`. `beagle-linux/kernel/pru_rproc/` is that driver with TI's kick and vring handler added back, the same patch as the PocketBeagle 2 demo, and one more change: a core whose firmware has no RPMsg (every ICSSG core here but ICSSG0 RTU0) starts without a vring interrupt. Neither image's J721E device tree has a vring interrupt for any ICSSG core, so the overlay gives RTU0 one, from ICSSG0's interrupt controller (system event 20, host interrupt 4).

**`ti_k3_r5_remoteproc`.** The stock driver can start an R5F core once. Stopping it then powers the core module off through the device manager, which refuses (`module-reset assert failed, ret = -19`) and leaves the core stuck in transition. Every later start fails with `module-reset deassert failed, ret = -19` until a power cycle. `beagle-linux/kernel/k3_r5_rproc/` is the 6.18.55 driver with that power-off taken out: stopping a core asserts its reset and leaves it powered, the way TI's kernel handles AM64x cores (TI's 6.18 still powers J721E cores off, hence the Yocto patch). With it, `ai64-demo` can restart all four R5F cores as often as you like.

If you don't need RPMsg on an ICSSG core, shared memory, as RTU1 and the ICSSG1 cores use, works with the stock `pru_rproc`. If you start an R5F only once per boot, the stock R5F driver is fine.
