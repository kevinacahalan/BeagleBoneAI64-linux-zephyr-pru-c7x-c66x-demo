#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the demo in the container images under containers/.
#   containers/linux/Containerfile    Linux userspace and the two kernel modules
#   containers/pru-cgt/Containerfile  TI CGT 2.3.3, then the ICSSG0 firmware
#   containers/zephyr/Containerfile   Zephyr SDK for the main-domain R5Fs
#   containers/dsp-cgt/Containerfile  TI C6000 and C7000 CGT for the DSP images
# The Poky image stays at yocto/kas/ai64-poky.yml. This script does not flash.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CACHE=$ROOT/.cache
PSSP=$ROOT/third_party/pru-software-support-package
PSSP_REV=28107daa1d184059f67676ba49d0fcaebb0f6a96
ERPC=$ROOT/third_party/erpc
ERPC_REV=1.14.0
BBDT=$ROOT/third_party/BeagleBoard-DeviceTrees
BBDT_REV=550f7711a4696d5f5e92505289379b754e871313

IMAGE_LINUX=ai64-linux-userspace
IMAGE_PRU=ai64-pru-cgt
IMAGE_ZEPHYR=ai64-zephyr-sdk
IMAGE_DSP=ai64-dsp-cgt

# The R5F cores that run Zephyr, as west board names. One sysbuild build of
# zephyr/r5 makes an image for each (zephyr/r5/sysbuild.cmake), each with its
# own job and overlay from zephyr/r5/boards/. The first is the default image.
R5_CORES=(main_r5f0_0 main_r5f0_1 main_r5f1_0 main_r5f1_1)

# Where sysbuild puts core $1's ELF: build/zephyr/<image>/zephyr/.
r5_elf() {
	local core=${1#main_} image
	image=$core
	[[ $1 == "${R5_CORES[0]}" ]] && image=r5
	echo "build/zephyr/$image/zephyr/ai64_$core.elf"
}

# AI64_CONTAINER_ENGINE is podman or docker. Unset, use podman when it
# is installed, otherwise docker. --security-opt label=disable is podman.
select_engine() {
	local engine=${AI64_CONTAINER_ENGINE:-}
	if [[ -z $engine ]]; then
		if command -v podman >/dev/null 2>&1; then
			engine=podman
		elif command -v docker >/dev/null 2>&1; then
			engine=docker
		else
			echo "Install podman or docker." >&2
			exit 1
		fi
	fi
	case $engine in
		podman|docker) ;;
		*)
			echo "AI64_CONTAINER_ENGINE must be podman or docker." >&2
			exit 1
			;;
	esac
	if ! command -v "$engine" >/dev/null 2>&1; then
		echo "AI64_CONTAINER_ENGINE is $engine, and that program is not installed." >&2
		exit 1
	fi
	AI64_CONTAINER_ENGINE=$engine
}

engine_build() {
	if [[ $AI64_CONTAINER_ENGINE == podman ]]; then
		podman build "$@"
	else
		docker build "$@"
	fi
}

# Run as the host user, so everything written into the repo stays owned by
# you. Without this, docker leaves root-owned files, and rootless podman
# leaves files owned by a subordinate uid that you cannot delete.
engine_run() {
	if [[ $AI64_CONTAINER_ENGINE == podman ]]; then
		podman run --rm --security-opt label=disable \
			--userns=keep-id --user "$(id -u):$(id -g)" -e HOME=/tmp "$@"
	else
		docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp "$@"
	fi
}

build_image() {
	local tag=$1 file=$2
	select_engine
	engine_build -t "$tag" -f "$file" "$(dirname "$file")"
}

# at_rev DIR REV: DIR is a git checkout whose HEAD is REV (a commit or tag).
at_rev() {
	local head want
	head=$(git -C "$1" rev-parse HEAD 2>/dev/null) || return 1
	want=$(git -C "$1" rev-parse "$2^{commit}" 2>/dev/null) || return 1
	[[ $head == "$want" ]]
}

fetch_erpc() {
	if [[ -f $ERPC/zephyr/module.yml ]] && at_rev "$ERPC" "$ERPC_REV"; then
		return
	fi
	rm -rf "$ERPC"
	git clone --depth 1 --branch "$ERPC_REV" https://github.com/EmbeddedRPC/erpc.git "$ERPC"
}

# BeagleBoard-DeviceTrees v6.18.x provides ti/k3-pinctrl.h. The overlay
# Makefile includes that header. This repository does not vendor a copy.
fetch_bbdt() {
	if [[ -f $BBDT/src/arm64/ti/k3-pinctrl.h ]] && at_rev "$BBDT" "$BBDT_REV"; then
		return
	fi
	rm -rf "$BBDT"
	git clone --depth 1 https://github.com/beagleboard/BeagleBoard-DeviceTrees.git "$BBDT"
	git -C "$BBDT" fetch --depth 1 origin "$BBDT_REV"
	git -C "$BBDT" checkout "$BBDT_REV"
}

fetch_pssp() {
	if [[ -f $PSSP/include/j721e/pru_intc.h ]] && at_rev "$PSSP" "$PSSP_REV"; then
		return
	fi
	# git.ti.com often fails a first request and answers the next one.
	local try
	for try in 1 2 3; do
		rm -rf "$PSSP"
		if timeout 300 git clone --depth 1 https://git.ti.com/git/pru-software-support-package/pru-software-support-package.git "$PSSP"; then
			break
		fi
		if [[ $try -eq 3 ]]; then
			echo "Could not clone the PRU Software Support Package from git.ti.com, three tries." >&2
			echo "That server is sometimes slow to answer; try again later, or clone it" >&2
			echo "yourself at commit $PSSP_REV into $PSSP and run this again." >&2
			exit 1
		fi
		echo "git.ti.com did not answer (try $try of 3), trying again" >&2
		sleep 5
	done
	git -C "$PSSP" fetch --depth 1 origin "$PSSP_REV"
	git -C "$PSSP" checkout "$PSSP_REV"
}

build_linux() {
	build_image "$IMAGE_LINUX" "$ROOT/containers/linux/Containerfile"
	fetch_erpc
	fetch_bbdt
	engine_run \
		-v "$ROOT:/work" -w /work \
		"$IMAGE_LINUX" \
		bash -lc 'make -C linux-app clean all && make -C beagle-linux/overlays clean all'
}

# PSSP ships rpmsg_lib.lib prebuilt, so the PRU build only needs the headers
# and that library from the checkout. TRACE=0 in the environment builds the
# trace out of the real-time PRU and Tx_PRU cores (pru/Makefile); the PRU
# build starts clean, so switching it takes effect.
build_pru() {
	build_image "$IMAGE_PRU" "$ROOT/containers/pru-cgt/Containerfile"
	fetch_pssp
	engine_run \
		-v "$ROOT:/work" -w /work \
		"$IMAGE_PRU" \
		sh -c 'rm -rf pru/build && make -C pru "$@"' _ \
		PRU_CGT=/opt/ti-cgt-pru PSSP=/work/third_party/pru-software-support-package \
		TRACE="${TRACE:-1}"
}

# Both modules have to match the board kernel. Headers are copied from the
# board once; the modules themselves are cross-compiled in the Linux image.
#   pru_rproc            adds the virtio kick RTU0's RPMsg needs
#   ti_k3_r5_remoteproc  lets a stopped R5F core start again
build_kmod() {
	local host=${AI64_SSH:-}
	local krel headers
	if [[ -z $host ]]; then
		echo "Set AI64_SSH to the board's ssh login, for example AI64_SSH=debian@192.168.7.2." >&2
		echo "The board must have linux-headers for its running kernel installed." >&2
		exit 1
	fi
	krel=${AI64_KREL:-$(ssh "$host" uname -r)}
	headers=$CACHE/linux-headers-$krel
	echo "building pru_rproc and ti_k3_r5_remoteproc for $krel"
	build_image "$IMAGE_LINUX" "$ROOT/containers/linux/Containerfile"
	if [[ ! -f $headers/Makefile ]]; then
		echo "fetching kernel headers from $host"
		mkdir -p "$headers"
		rsync -a "$host:/usr/src/linux-headers-$krel/" "$headers/"
	fi
	engine_run \
		-v "$ROOT:/work" \
		-v "$headers:/headers:ro" \
		-w /work \
		"$IMAGE_LINUX" \
		bash -lc 'set -euo pipefail
			rm -rf /tmp/kheaders
			cp -a /headers /tmp/kheaders
			# Headers from the board contain aarch64 host tools. Rebuild the two
			# the module build actually runs.
			gcc -O2 -I/tmp/kheaders/scripts/include -o /tmp/kheaders/scripts/basic/fixdep /tmp/kheaders/scripts/basic/fixdep.c
			gcc -O2 -I/tmp/kheaders/scripts/mod -I/tmp/kheaders/scripts/include \
				-o /tmp/kheaders/scripts/mod/modpost \
				/tmp/kheaders/scripts/mod/modpost.c \
				/tmp/kheaders/scripts/mod/file2alias.c \
				/tmp/kheaders/scripts/mod/sumversion.c \
				/tmp/kheaders/scripts/mod/symsearch.c
			# Clean first. The headers sit at /tmp/kheaders for every kernel
			# version, so kbuild would otherwise keep a module built for the
			# previous kernel.
			for m in pru_rproc k3_r5_rproc; do
				make -C /tmp/kheaders M=/work/beagle-linux/kernel/$m ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- clean
				make -C /tmp/kheaders M=/work/beagle-linux/kernel/$m ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
			done
			'
}

build_dsp() {
	build_image "$IMAGE_DSP" "$ROOT/containers/dsp-cgt/Containerfile"
	engine_run \
		-v "$ROOT:/work" -w /work \
		"$IMAGE_DSP" \
		make -C dsp C6X_CGT=/opt/ti/ti-cgt-c6000_8.3.12 C7X_CGT=/opt/ti/ti-cgt-c7000_1.2.0.STS
}

# zephyr/erpc/shim/ is generated from zephyr/erpc/ai64.erpc and checked in.
# Rebuild it only after changing the IDL.
build_erpcgen() {
	build_image "$IMAGE_LINUX" "$ROOT/containers/linux/Containerfile"
	fetch_erpc
	engine_run \
		-v "$ROOT:/work" -w /work \
		"$IMAGE_LINUX" \
		bash -lc 'set -euo pipefail
			make -C third_party/erpc/erpcgen -j"$(nproc)" >/dev/null
			gen=$(find third_party/erpc -type f -name erpcgen -perm -u+x | head -n 1)
			rm -rf zephyr/erpc/shim
			mkdir -p zephyr/erpc/shim
			"$gen" -g c -o zephyr/erpc/shim/ zephyr/erpc/ai64.erpc
			ls zephyr/erpc/shim'
}

build_zephyr() {
	build_image "$IMAGE_ZEPHYR" "$ROOT/containers/zephyr/Containerfile"
	fetch_erpc
	engine_run \
		-v "$ROOT:/work" -w /work \
		-e CORE="${R5_CORES[0]}" \
		"$IMAGE_ZEPHYR" \
		bash -c 'set -euo pipefail
			git config --global --add safe.directory "*"
			if [[ ! -d .west ]]; then
				west init -l zephyr
			fi
			# west update checks out the manifest revision and drops local
			# edits. apply-patches.sh reapplies zephyr/patches/ when that
			# directory has any.
			west update
			zephyr/scripts/apply-patches.sh
			west build --sysbuild -b "beaglebone_ai64/j721e/$CORE" zephyr/r5 \
				-d build/zephyr -p always'
}

# Gather what install-beagle.sh needs into out/ai64-beagle, to copy to the board.
bundle() {
	local out=$ROOT/out/ai64-beagle f core
	local files=(
		linux-app/ai64
		beagle-linux/scripts/ai64-demo
		beagle-linux/scripts/install-beagle.sh
		pru/build/ai64-icssg0-pru0.elf
		pru/build/ai64-icssg0-rtu0.elf
		pru/build/ai64-icssg0-txpru0.elf
		pru/build/ai64-icssg0-pru1.elf
		pru/build/ai64-icssg0-rtu1.elf
		pru/build/ai64-icssg0-txpru1.elf
		pru/build/ai64-icssg1-pru0.elf
		pru/build/ai64-icssg1-rtu0.elf
		pru/build/ai64-icssg1-txpru0.elf
		pru/build/ai64-icssg1-pru1.elf
		pru/build/ai64-icssg1-rtu1.elf
		pru/build/ai64-icssg1-txpru1.elf
		beagle-linux/kernel/pru_rproc/pru_rproc.ko
		beagle-linux/kernel/k3_r5_rproc/ti_k3_r5_remoteproc.ko
		beagle-linux/overlays/k3-j721e-beagleboneai64-ai64-demo.dtbo
		dsp/build/ai64-c66x.elf
		dsp/build/ai64-c7x.elf
	)
	for core in "${R5_CORES[@]}"; do
		files+=("$(r5_elf "$core")")
	done
	for f in "${files[@]}"; do
		if [[ ! -f $ROOT/$f ]]; then
			echo "missing $f. Run ./beagle-linux/scripts/build.sh first, and kmod for the .ko files." >&2
			exit 1
		fi
	done
	rm -rf "$out"
	mkdir -p "$out"
	for f in "${files[@]}"; do
		cp -p "$ROOT/$f" "$out/"
	done
	echo "bundle in $out"
	echo "  scp -r out/ai64-beagle USER@BOARD:"
	echo "  ssh -t USER@BOARD 'sudo ./ai64-beagle/install-beagle.sh && sudo reboot'"
}

case ${1:-all} in
	bundle) bundle ;;
	pru) build_pru ;;
	kmod) build_kmod ;;
	linux) build_linux ;;
	zephyr) build_zephyr ;;
	dsp) build_dsp ;;
	erpcgen) build_erpcgen ;;
	all)
		build_linux
		build_pru
		build_zephyr
		build_dsp
		;;
	*)
		echo "usage: $0 [all|linux|pru|zephyr|dsp|kmod|bundle|erpcgen]" >&2
		exit 2
		;;
esac
