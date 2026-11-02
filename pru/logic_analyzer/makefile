# SPDX-License-Identifier: MIT
# The logic analyzer: ICSSG1, both slices. Builds each core's image with the
# makefile in its firmware/beaglebone-ai64/<core>/ti-pru-cgt directory.

BOARD := beaglebone-ai64
CORES := icss_g1_rtu_pru0_fw icss_g1_tx_pru0_fw icss_g1_pru0_fw \
	icss_g1_rtu_pru1_fw icss_g1_tx_pru1_fw icss_g1_pru1_fw

.PHONY: all clean $(CORES)
all: $(CORES)

$(CORES):
	$(MAKE) -C firmware/$(BOARD)/$@/ti-pru-cgt

clean:
	for c in $(CORES); do $(MAKE) -C firmware/$(BOARD)/$$c/ti-pru-cgt clean || exit 1; done
