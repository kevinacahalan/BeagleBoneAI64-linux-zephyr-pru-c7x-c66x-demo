SUMMARY = "BeagleBone AI-64 C66x and C7x firmware"
DESCRIPTION = "Compiles the DSP images in dsp/ with TI's C6000 and C7000 compilers. Installs ai64-c66x.elf (for both C66x cores) and ai64-c7x.elf. Both include the fabric header from zephyr/include."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "ti-cgt-dsp-native"

FILESEXTRAPATHS:prepend := "${@os.path.abspath(os.path.join(os.path.dirname(d.getVar('FILE')), '..', '..', '..', '..', '..'))}/:"

SRC_URI = " \
    file://dsp;subdir=src \
    file://zephyr/include/ai64_fabric.h;subdir=src \
    file://zephyr/include/ai64_pipe.h;subdir=src \
    file://zephyr/include/ai64_shm.h;subdir=src \
"

S = "${UNPACKDIR}/src"

COMPATIBLE_MACHINE = "beaglebone-ai64"
PACKAGE_ARCH = "${MACHINE_ARCH}"
# The DSP images are not Arm code, and the TI tools record the build directory.
INSANE_SKIP:${PN} += "arch buildpaths"

FW = "ai64-c66x.elf ai64-c7x.elf"

do_configure[noexec] = "1"

do_compile () {
    rm -rf ${S}/dsp/build
    oe_runmake -C ${S}/dsp \
        C6X_CGT="${STAGING_DATADIR_NATIVE}/ti-cgt-c6x" \
        C7X_CGT="${STAGING_DATADIR_NATIVE}/ti-cgt-c7x"
    for f in ${FW}; do
        test -f ${S}/dsp/build/$f
    done
}

do_install () {
    install -d ${D}${nonarch_base_libdir}/firmware
    for f in ${FW}; do
        install -m 0644 ${S}/dsp/build/$f ${D}${nonarch_base_libdir}/firmware/$f
    done
}

FILES:${PN} = "${nonarch_base_libdir}/firmware/ai64-c66x.elf ${nonarch_base_libdir}/firmware/ai64-c7x.elf"
