# SPDX-License-Identifier: MIT
#
# The ICSSG firmware: one project per ICSSG, each laid out the way TI's
# open-pru projects are (pru_rules.mak has the details):
#
#   pwm/                  ICSSG0. Slice 0 makes PWM in software, slice 1
#                         with the ICSSG's PWM module; in each slice the
#                         RTU talks, the Tx_PRU modulates, the PRU makes
#                         the edges.
#   multicore_scheduler/  ICSSG1. A port of open-pru's multicore_scheduler:
#                         IEP0 compares start a task on each of the six
#                         cores in turn.
#
# The images land in build/ as ai64-icssg<N>-<core><slice>.elf. PRU_CGT and
# PSSP are set by beagle-linux/scripts/build.sh. TRACE=0 builds the trace
# out of the real-time cores.

PROJECTS := pwm multicore_scheduler

.PHONY: all clean $(PROJECTS)
all: $(PROJECTS)

$(PROJECTS):
	$(MAKE) -C $@

clean:
	for p in $(PROJECTS); do $(MAKE) -C $$p clean || exit 1; done
	rm -rf build
