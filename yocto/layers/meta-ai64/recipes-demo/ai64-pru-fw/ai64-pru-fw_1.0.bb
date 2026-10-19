SUMMARY = "BeagleBone AI-64 ICSSG0 PRU0, RTU0, and Tx_PRU0 firmware"
DESCRIPTION = "Compiles the three ICSSG0 slice-0 images with TI CGT and the PRU software support package. Installs ai64-icssg0-pru0.elf, ai64-icssg0-rtu0.elf, and ai64-icssg0-txpru0.elf."
# The demo is MIT. The PRU Software Support Package it links is BSD-3-Clause.
LICENSE = "MIT & BSD-3-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302 \
    file://${COMMON_LICENSE_DIR}/BSD-3-Clause;md5=550794465ba0ec5312d6919e203a55f9"

DEPENDS = "ti-cgt-pru-native"

FILESEXTRAPATHS:prepend := "${@os.path.abspath(os.path.join(os.path.dirname(d.getVar('FILE')), '..', '..', '..', '..', '..'))}/:"

SRC_URI = " \
    file://pru/Makefile;subdir=src \
    file://pru/include;subdir=src \
    file://pru/icssg0;subdir=src \
    file://zephyr/include/ai64_shm.h;subdir=src \
    git://git.ti.com/git/pru-software-support-package/pru-software-support-package.git;protocol=https;nobranch=1;destsuffix=pssp;name=pssp \
"
SRCREV_pssp = "28107daa1d184059f67676ba49d0fcaebb0f6a96"

S = "${UNPACKDIR}/src"
PRU_CGT = "${STAGING_DATADIR_NATIVE}/ti-cgt-pru"

COMPATIBLE_MACHINE = "beaglebone-ai64"
PACKAGE_ARCH = "${MACHINE_ARCH}"
# clpru records the build directory in debug info. The PRU does not use that path.
INSANE_SKIP:${PN} += "arch buildpaths"

FW = "ai64-icssg0-pru0.elf ai64-icssg0-rtu0.elf ai64-icssg0-txpru0.elf"

do_configure[noexec] = "1"

do_compile () {
    rm -rf ${S}/pru/build
    oe_runmake -C ${S}/pru \
        PRU_CGT="${PRU_CGT}" \
        PSSP="${UNPACKDIR}/pssp"
    for f in ${FW}; do
        test -f ${S}/pru/build/$f
    done
}

do_install () {
    install -d ${D}${nonarch_base_libdir}/firmware
    for f in ${FW}; do
        install -m 0644 ${S}/pru/build/$f ${D}${nonarch_base_libdir}/firmware/$f
    done
}

FILES:${PN} = "${nonarch_base_libdir}/firmware/ai64-icssg0-*.elf"
