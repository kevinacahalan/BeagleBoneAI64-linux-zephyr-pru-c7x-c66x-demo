SUMMARY = "BeagleBone AI-64 Zephyr R5F demo firmware"
DESCRIPTION = "Builds the R5F app in zephyr/r5 for each main-domain core in R5_CORES (board beaglebone_ai64/j721e/<core>) with the Zephyr SDK and a west workspace fetched during the build. Installs each ELF as /lib/firmware/ai64_<core>.elf, not the kernel's default name, so the cores stay off until ai64-demo starts them."
# The demo is MIT, EmbeddedRPC is BSD-3-Clause, and Zephyr is Apache-2.0.
LICENSE = "MIT & BSD-3-Clause & Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302 \
    file://third_party/erpc/LICENSE;md5=b66f32a90f9577a5a3255c21d79bc619"

DEPENDS = "cmake-native ninja-native gperf-native dtc-native python3-native python3-pip-native git-native"

FILESEXTRAPATHS:prepend := "${@os.path.abspath(os.path.join(os.path.dirname(d.getVar('FILE')), '..', '..', '..', '..', '..'))}/:"

SRC_URI = " \
    file://zephyr/west.yml \
    file://zephyr/r5 \
    file://zephyr/include \
    file://zephyr/erpc \
    file://zephyr/scripts \
    file://zephyr/patches \
    git://github.com/EmbeddedRPC/erpc.git;protocol=https;nobranch=1;destsuffix=src/third_party/erpc;name=erpc \
    https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz;name=sdk;subdir=sdk \
    https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz;name=armtc;subdir=sdk/zephyr-sdk-1.0.1/gnu \
"
SRC_URI[sdk.sha256sum] = "ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb"
SRC_URI[armtc.sha256sum] = "21b85981cb5a1818d9bc53d82af80f208946ec038b982ff1907287572ed3a634"
SRCREV_erpc = "e8d5684a920e023a49cec875c04ad05ddc8521e6"

S = "${UNPACKDIR}/src"
ZEPHYR_SDK = "${UNPACKDIR}/sdk/zephyr-sdk-1.0.1"

# Same list as R5_CORES in beagle-linux/scripts/build.sh.
R5_CORES = "main_r5f0_0 main_r5f0_1 main_r5f1_0 main_r5f1_1"

COMPATIBLE_MACHINE = "beaglebone-ai64"
PACKAGE_ARCH = "${MACHINE_ARCH}"
# The Zephyr ELF records the toolchain directory. The R5F does not use that path.
INSANE_SKIP:${PN} += "arch buildpaths"

do_configure () {
    rm -rf ${S}/zephyr
    cp -a ${UNPACKDIR}/zephyr ${S}/zephyr
    test -f ${S}/third_party/erpc/LICENSE
    test -x ${ZEPHYR_SDK}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc || \
        test -x ${ZEPHYR_SDK}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc
}

do_compile[network] = "1"
do_compile () {
    export PATH="${STAGING_BINDIR_NATIVE}:${PATH}"
    export HOME="${WORKDIR}/home"
    mkdir -p "${HOME}"
    export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK}"
    export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
    export CMAKE_BUILD_PARALLEL_LEVEL="${@oe.utils.parallel_make(d)}"
    export GIT_CONFIG_COUNT=1
    export GIT_CONFIG_KEY_0=safe.directory
    export GIT_CONFIG_VALUE_0='*'
    pip3 install --target ${WORKDIR}/py west==1.5.0
    export PYTHONPATH="${WORKDIR}/py${PYTHONPATH:+:$PYTHONPATH}"
    export PATH="${WORKDIR}/py/bin:${PATH}"
    # The repo root may already have a .west workspace, and WORKDIR is under
    # the repo. west walks parents, so init this tree outside the repo, in a
    # private directory so two builds cannot collide.
    ws=$(mktemp -d /tmp/ai64-r5-west.XXXXXX)
    cp -a ${S}/. "$ws/"
    cd "$ws"
    west init -l zephyr
    west update
    "$ws/zephyr/scripts/apply-patches.sh"
    if [ -f zephyr/upstream/scripts/requirements-base.txt ]; then
        pip3 install --target ${WORKDIR}/py -r zephyr/upstream/scripts/requirements-base.txt
    fi
    for core in ${R5_CORES}; do
        name=ai64_${core#main_}.elf
        west build -b beaglebone_ai64/j721e/$core zephyr/r5 -d "$ws/zbuild-$core" -p always
        test -f "$ws/zbuild-$core/zephyr/$name"
        install -m 0644 "$ws/zbuild-$core/zephyr/$name" ${WORKDIR}/$name
    done
    rm -rf "$ws"
}

do_install () {
    install -d ${D}${nonarch_base_libdir}/firmware
    for core in ${R5_CORES}; do
        name=ai64_${core#main_}.elf
        install -m 0644 ${WORKDIR}/$name ${D}${nonarch_base_libdir}/firmware/$name
    done
}

FILES:${PN} = "${nonarch_base_libdir}/firmware/ai64_r5f*.elf"
