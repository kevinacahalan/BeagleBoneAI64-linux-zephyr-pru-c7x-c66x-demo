# SPDX-License-Identifier: MIT
#
# One build, four images. The default image is this app on R5F0_0 (west build
# -b beaglebone_ai64/j721e/main_r5f0_0 --sysbuild zephyr/r5); this adds the
# same app for the other three main-domain R5F cores. Each image takes its
# job and devices from boards/<board>.conf and .overlay, and lands in
# build/<image>/zephyr/ai64_<core>.elf.

# The images share this source directory, so sysbuild reads this file again
# for each one it adds; add them once.
if(TARGET r5f0_1)
  return()
endif()

foreach(core r5f0_1 r5f1_0 r5f1_1)
  ExternalZephyrProject_Add(
    APPLICATION ${core}
    SOURCE_DIR  ${APP_DIR}
    BOARD       beaglebone_ai64/j721e/main_${core}
  )
endforeach()
