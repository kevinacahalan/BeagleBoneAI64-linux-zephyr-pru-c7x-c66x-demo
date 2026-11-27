# Troubleshooting

Common issues when flashing, building, installing, or running the demo on BeagleBone AI-64.

## Debian setup & SSH

- **`ssh USER@bbai64.local` doesn't connect.**
  - Give a fresh SD card about 3 minutes for its two automatic first-boot reboots (applying `sysconf.txt` and growing the root partition/filesystem).
  - Try the board's IP from your router's DHCP lease table, or `192.168.7.2` over a USB-C cable to your PC.
  - Use the 3-pin debug serial console (`ttyS2`, 115200 8N1) if networking doesn't come up.
- **`REMOTE HOST IDENTIFICATION HAS CHANGED` after reflashing.**
  - A freshly flashed card generates new SSH host keys. Clear the old entry with `ssh-keygen -R bbai64.local` and reconnect.
- **`install-beagle.sh` says it could not install `tmux`.**
  - The board couldn't reach apt repositories (no internet). Run plain `ai64-demo` (merged console) instead of `ai64-demo --tmux`, or connect Ethernet and re-run `install-beagle.sh`.
- **`dmesg` shows `Direct firmware load for j7-main-r5f0_0-fw failed with error -2` at boot.**
  - Expected. The kernel attempts to auto-boot the R5F, C66x, and C7x cores under default names, finds none, and leaves them off so `ai64-demo` controls every core's lifecycle with `ai64_*.elf`.

## Kernel modules & device-tree overlays

- **`install-beagle.sh` says a module `was not built for <kernel>`.**
  - The `.ko` files in `out/ai64-beagle/` were built against a different kernel release. Rebuild against the running board and re-bundle:
    ```bash
    AI64_SSH=USER@BOARD ./beagle-linux/scripts/build.sh kmod
    ./beagle-linux/scripts/build.sh bundle
    ```
- **`ai64-demo` says `RPMsg devices did not show up`, or `RTU0` never appears.**
  - Check `dmesg | grep -i -E "remoteproc|rpmsg|kick|vring"` on the board:
    - `.kick method not defined`: stock `pru_rproc` is loaded (commonly after `apt upgrade`). Rebuild `kmod`, re-bundle, re-run `install-beagle.sh`, and reboot.
    - Missing `/lib/firmware/ai64_r5f0_0.elf` or `ai64-icssg0-rtu0.elf`: `install-beagle.sh` did not finish; run it again.
- **`ai64-demo` says `/proc/device-tree/reserved-memory/ai64-fabric@a9000000 is missing`.**
  - The board booted without the demo's device-tree overlay, so Linux owns the fabric's DDR region and `ai64-demo` refuses to start cores that would corrupt it. Run `install-beagle.sh`, check the `fdtoverlays` line of the default label in `/boot/firmware/extlinux/extlinux.conf`, and reboot.
- **`ai64 pads` marks an ICSSG pin `DIFFERENT`, or a PWM pin stays low.**
  - The board booted an older overlay. Install a fresh bundle with `install-beagle.sh` and reboot.
- **`P9.25` stays low, or `ai64 pads` marks `0x1a4` `DIFFERENT`.**
  - Either `R5F0_1` has not booted yet, or another overlay (most likely the stock `k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo`) claims ball W26 / EHRPWM4 for Linux. Check `fdtoverlays` in `/boot/firmware/extlinux/extlinux.conf`.
- **`ai64-demo` says stock `ti_k3_r5_remoteproc` cannot restart an R5F core.**
  - The patched R5F module isn't loaded (usually after a kernel upgrade). Rebuild with `build.sh kmod`, bundle, install, and reboot. Until then, rebooting the board reloads the R5Fs once.
- **`dmesg` shows `module-reset deassert failed, ret = -19`.**
  - An R5F core was stopped while the stock `ti_k3_r5_remoteproc` module was loaded and is now stuck in transition. **Power-cycle** the board; a soft `reboot` from Linux is not always enough.

## Running the demo & cores

- **`ai64-demo` says it cannot stop the C7x.**
  - The currently running C7x image predates the shutdown handler ([Restarting the C7x](../dsp/README.md#restarting-the-c7x)). Reboot once to load the updated image.
- **`ai64 fabric` says another viewer is running.**
  - Linux is fabric node 0, and only one viewer can own it at a time. Close the `fabric` tmux window (or the merged console's `[Fabric]` viewer) first. `ai64 fabric route` works alongside either.

## Host builds

- **"Permission denied" deleting `build/`, `.west/`, or `modules/`.**
  - Those directories were written under a container UID mapping. Clean them once:
    - Podman: `podman unshare rm -rf build .west modules bootloader tools zephyr/upstream`
    - Docker: `sudo rm -rf build .west modules bootloader tools zephyr/upstream`
- **Don't run `west init -l .` from the repo root.**
  - That places `.west/` in the parent directory. `build.sh` runs `west init -l zephyr` instead.
- **`build.sh kmod` fails to copy `/usr/src/linux-headers-...`.**
  - The BeagleBoard image ships the headers for its kernel. If yours does not, install them on the board with `sudo apt install linux-headers-$(uname -r)`.
- **`git.ti.com` times out while fetching the PRU Software Support Package.**
  - `git.ti.com` intermittently drops first requests; `build.sh` retries three times. If all three fail, retry later or manually clone the commit pinned in `beagle-linux/scripts/build.sh` into `third_party/pru-software-support-package`.
