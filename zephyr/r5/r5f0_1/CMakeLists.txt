# SPDX-License-Identifier: MIT
#
# R5F0_1: EHRPWM4 on P9.25, and the pipeline's last stage.
# The part every core runs is in ../common/ (common.cmake).
cmake_minimum_required(VERSION 3.20.0)

# Every core's settings and devicetree bindings, then this core's own.
set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/common.conf ${CMAKE_CURRENT_LIST_DIR}/prj.conf)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../common)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ai64_r5f0_1 C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE
  src/main.c
  src/pwm.c
  src/pipe_pwm.c
)
