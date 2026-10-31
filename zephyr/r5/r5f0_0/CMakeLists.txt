# SPDX-License-Identifier: MIT
#
# R5F0_0: SPI7 on header P9, the bulk buffer Linux writes, a UDMA channel, and
# the pipeline's move into the C66x cores. It also sends the fabric's tours.
# The part every core runs is in ../common/ (common.cmake).
# sysbuild.cmake adds the other three cores' apps to this build.

cmake_minimum_required(VERSION 3.20.0)

# Every core's settings and devicetree bindings, then this core's own.
set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/common.conf ${CMAKE_CURRENT_LIST_DIR}/prj.conf)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../common)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ai64_r5f0_0 C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

# The fabric's tours start here (zephyr/include/ai64_fabric.h).
target_compile_definitions(app PRIVATE AI64_FAB_TOURS=1)

target_sources(app PRIVATE
  src/main.c
  src/spi.c
  src/bulk.c
  src/dma.c
  src/pipe_dma.c
)
