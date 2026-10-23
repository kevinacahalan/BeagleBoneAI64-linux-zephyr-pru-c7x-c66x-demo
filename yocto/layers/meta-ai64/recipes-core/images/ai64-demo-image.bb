SUMMARY = "BeagleBone AI-64 demo image with Beagle bring-up"
DESCRIPTION = "systemd image for BeagleBone AI-64. extlinux selects the demo overlay, the Beagle USB gadgets enumerate, and /root holds ai64, ai64-demo, and the firmwares. Login is the default root account. ai64-demo is on PATH."

require recipes-core/images/core-image-minimal.bb

IMAGE_INSTALL:append = " \
    ai64-demo \
    ai64-pru-fw \
    ai64-r5-fw \
    ai64-dsp-fw \
    ai64-boot \
    ai64-sysconf \
    bb-usb-gadgets \
    kernel-modules \
    systemd \
    systemd-networkd \
"

IMAGE_FEATURES:append = " ssh-server-dropbear"

# j721e.inc recommends TI's Ethernet switch firmware, which the kernel then
# boots on R5F0_0 at startup. It drives the CPSW9G, which this board does
# not use, and it does not answer the shutdown request, so ai64-demo could
# not stop the core to load Zephyr.
BAD_RECOMMENDATIONS += "ti-eth-fw-j721e"

# C.UTF-8. core-image-minimal installs no locale at all, and tmux (ai64-demo
# --tmux) refuses to start without a UTF-8 one.
IMAGE_LINGUAS = "c"

# FAT, then the ext4 root extlinux.conf mounts as mmcblk1p2, then swap.
WKS_FILE = "ai64-beagle.wks"

IMAGE_BOOT_FILES:append = " \
    Image \
    k3-j721e-beagleboneai64.dtb;ti/k3-j721e-beagleboneai64.dtb \
    extlinux.conf;extlinux/extlinux.conf \
    sysconf.txt;sysconf.txt \
    k3-j721e-beagleboneai64-ai64-demo.dtbo;overlays/k3-j721e-beagleboneai64-ai64-demo.dtbo \
"

do_image_wic[depends] += "ai64-demo:do_deploy ai64-boot:do_deploy"

# Root is the account this image already ships. Do not add another
# user, and do not set or clear a password here.

ai64_fstab() {
    cat >> ${IMAGE_ROOTFS}${sysconfdir}/fstab <<'EOF'
/dev/mmcblk1p1  /boot/firmware  vfat  user,uid=0,gid=0,defaults  0  2
/dev/mmcblk1p3  none            swap  sw  0  0
EOF
    install -d ${IMAGE_ROOTFS}/boot/firmware
}

# What root sees on login. /usr/bin/ai64-demo stays on PATH.
# The same binaries and firmwares also stay in /usr/bin and /lib/firmware.
ai64_root_demo() {
    home=${IMAGE_ROOTFS}/root
    fw=${IMAGE_ROOTFS}${nonarch_base_libdir}/firmware
    install -d -m 0700 -o 0 -g 0 ${home}
    install -m 0755 -o 0 -g 0 ${IMAGE_ROOTFS}${bindir}/ai64 ${home}/ai64
    install -m 0755 -o 0 -g 0 ${IMAGE_ROOTFS}${bindir}/ai64-demo ${home}/ai64-demo
    for f in ai64-icssg0-pru0.elf ai64-icssg0-rtu0.elf ai64-icssg0-txpru0.elf \
        ai64-icssg0-pru1.elf ai64-icssg0-rtu1.elf ai64-icssg0-txpru1.elf \
        ai64-icssg1-pru0.elf ai64-icssg1-rtu0.elf ai64-icssg1-txpru0.elf \
        ai64-icssg1-pru1.elf ai64-icssg1-rtu1.elf ai64-icssg1-txpru1.elf \
        ai64_r5f0_0.elf ai64_r5f0_1.elf ai64_r5f1_0.elf ai64_r5f1_1.elf \
        ai64-c66x.elf ai64-c7x.elf; do
        install -m 0644 -o 0 -g 0 ${fw}/$f ${home}/$f
    done
    cat > ${home}/README.md <<'EOF'
ai64-demo starts the demo.
ai64 is the Linux program it runs.
ai64_r5f0_0.elf, ai64_r5f0_1.elf, ai64_r5f1_0.elf, and ai64_r5f1_1.elf are
the Zephyr firmwares for the four main-domain R5F cores.
The six ai64-icssg0-*.elf files are ICSSG0's PWM project: software PWM on
slice 0, the ICSSG's PWM module on slice 1. The six ai64-icssg1-*.elf files
are ICSSG1's multicore scheduler, a port of TI's open-pru example.
ai64-c66x.elf and ai64-c7x.elf are the C66x and C7x images.

From this directory:

./ai64-demo
EOF
    chmod 0644 ${home}/README.md
    chown 0:0 ${home}/README.md
}

ROOTFS_POSTPROCESS_COMMAND:append = " ai64_fstab; ai64_root_demo;"

COMPATIBLE_MACHINE = "beaglebone-ai64"
