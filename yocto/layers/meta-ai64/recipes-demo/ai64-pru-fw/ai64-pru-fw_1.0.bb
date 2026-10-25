SUMMARY = "BeagleBone AI-64 ICSSG firmware: the PWM project on ICSSG0 and the multicore scheduler on ICSSG1"
DESCRIPTION = "Compiles the twelve ICSSG images with TI CGT and the PRU software support package, from the two open-pru style projects in pru/: pwm (ICSSG0: PWM in software on slice 0, with the ICSSG PWM module on slice 1) and multicore_scheduler (ICSSG1: a port of open-pru's examples/multicore_scheduler on all six cores)."
# The demo is MIT. The PRU Software Support Package it links, and the
# multicore scheduler ported from open-pru, are BSD-3-Clause.
LICENSE = "MIT & BSD-3-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302 \
    file://${COMMON_LICENSE_DIR}/BSD-3-Clause;md5=550794465ba0ec5312d6919e203a55f9"

DEPENDS = "ti-cgt-pru-native"

FILESEXTRAPATHS:prepend := "${@os.path.abspath(os.path.join(os.path.dirname(d.getVar('FILE')), '..', '..', '..', '..', '..'))}/:"

SRC_URI = " \
    file://pru/Makefile;subdir=src \
    file://pru/pru_rules.mak;subdir=src \
    file://pru/common;subdir=src \
    file://pru/pwm;subdir=src \
    file://pru/multicore_scheduler;subdir=src \
    file://zephyr/include/ai64_shm.h;subdir=src \
    file://zephyr/include/ai64_fabric.h;subdir=src \
    file://zephyr/include/ai64_pipe.h;subdir=src \
    git://git.ti.com/git/pru-software-support-package/pru-software-support-package.git;protocol=https;nobranch=1;destsuffix=pssp;name=pssp \
"
SRCREV_pssp = "28107daa1d184059f67676ba49d0fcaebb0f6a96"

S = "${UNPACKDIR}/src"
PRU_CGT = "${STAGING_DATADIR_NATIVE}/ti-cgt-pru"

COMPATIBLE_MACHINE = "beaglebone-ai64"
PACKAGE_ARCH = "${MACHINE_ARCH}"
# clpru records the build directory in debug info. The PRU does not use that path.
INSANE_SKIP:${PN} += "arch buildpaths"

FW = "ai64-icssg0-pru0.elf ai64-icssg0-rtu0.elf ai64-icssg0-txpru0.elf \
    ai64-icssg0-pru1.elf ai64-icssg0-rtu1.elf ai64-icssg0-txpru1.elf \
    ai64-icssg1-pru0.elf ai64-icssg1-rtu0.elf ai64-icssg1-txpru0.elf \
    ai64-icssg1-pru1.elf ai64-icssg1-rtu1.elf ai64-icssg1-txpru1.elf"

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

FILES:${PN} = "${nonarch_base_libdir}/firmware/ai64-icssg*.elf"
