# SPDX-License-Identifier: MIT
#
# One build, four images. west build --sysbuild -b
# beaglebone_ai64/j721e/main_r5f0_0 zephyr/r5/r5f0_0 builds this app, the
# default image, and this adds each other core's app for its own board.
# Each lands in build/<core>/zephyr/ai64_<core>.elf.

foreach(core r5f0_1 r5f1_0 r5f1_1)
  ExternalZephyrProject_Add(
    APPLICATION ${core}
    SOURCE_DIR  ${APP_DIR}/../${core}
    BOARD       beaglebone_ai64/j721e/main_${core}
  )
endforeach()
