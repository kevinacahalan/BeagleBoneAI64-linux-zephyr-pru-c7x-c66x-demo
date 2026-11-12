SUMMARY = "/dev/ai64-la: the logic analyzer's ring, mapped for reading"
DESCRIPTION = "An out-of-tree kernel module that maps the ring the ai64-demo overlay reserves as Normal memory, so ai64 la reads it in bursts (beagle-linux/kernel/ai64_la/)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit module

FILESEXTRAPATHS:prepend := "${@os.path.abspath(os.path.join(os.path.dirname(d.getVar('FILE')), '..', '..', '..', '..', '..'))}/:"

SRC_URI = "file://beagle-linux/kernel/ai64_la;subdir=src"

S = "${UNPACKDIR}/src/beagle-linux/kernel/ai64_la"

COMPATIBLE_MACHINE = "beaglebone-ai64"

# ai64-demo loads it; nothing loads it at boot.
RPROVIDES:${PN} += "kernel-module-ai64-la"
