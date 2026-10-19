# Let the main-domain R5F cores and the C66x cores start again after a stop
# (see the patches).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://0001-remoteproc-k3-r5-keep-split-mode-cores-powered-on-stop.patch \
    file://0002-remoteproc-k3-reset-a-c66x-that-acked-shutdown.patch \
"
