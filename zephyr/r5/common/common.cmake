# SPDX-License-Identifier: MIT
#
# The part every R5F core runs, added to each core's app: its CMakeLists.txt
# includes this after project(). See common/src/ai64_r5.h.

# EmbeddedRPC 1.14.0, cloned by build.sh. It is not used as a Zephyr module:
# that module's Kconfig selects SERIAL from a symbol that depends on the full
# C++ library, and the loop fails configuration. The library sources are
# compiled here, with erpc_malloc/erpc_free on the Zephyr heap. Transport is
# the existing OpenAMP RPMsg endpoint, not the module's UART or RPMsg-Lite
# ports.
set(AI64_R5_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(ERPC ${AI64_R5_ROOT}/../../third_party/erpc/erpc_c)

# "r5f0_0" for board beaglebone_ai64/j721e/main_r5f0_0. The code names its
# core in logs with it, and in replies with AI64_CORE_NAME, "R5F0_0".
string(REGEX MATCH "r5f[0-9]_[0-9]" AI64_CORE "${BOARD_QUALIFIERS}")
if(NOT AI64_CORE)
  message(FATAL_ERROR "zephyr/r5 builds for a main-domain R5F core, not ${BOARD}${BOARD_QUALIFIERS}")
endif()
get_filename_component(AI64_APP_DIR ${APPLICATION_SOURCE_DIR} NAME)
if(NOT AI64_APP_DIR STREQUAL AI64_CORE)
  message(FATAL_ERROR "zephyr/r5/${AI64_APP_DIR} is built for ${AI64_CORE}: it runs on main_${AI64_APP_DIR} only")
endif()
string(TOUPPER "${AI64_CORE}" AI64_CORE_NAME)
target_compile_definitions(app PRIVATE AI64_CORE="${AI64_CORE}" AI64_CORE_NAME="${AI64_CORE_NAME}")

# Its number among the four R5F cores (common/src/ai64_ipc.h), and its node
# on the fabric (zephyr/include/ai64_fabric.h), one more.
set(AI64_SELF_r5f0_0 0)
set(AI64_SELF_r5f0_1 1)
set(AI64_SELF_r5f1_0 2)
set(AI64_SELF_r5f1_1 3)
math(EXPR AI64_FAB_NODE "${AI64_SELF_${AI64_CORE}} + 1")
target_compile_definitions(app PRIVATE AI64_SELF=${AI64_SELF_${AI64_CORE}} AI64_FAB_NODE=${AI64_FAB_NODE})

target_include_directories(app PRIVATE
  ${AI64_R5_ROOT}/../include
  ${AI64_R5_ROOT}/../../dsp/include
  ${AI64_R5_ROOT}/../erpc/shim
  ${AI64_R5_ROOT}/common/src
  ${ERPC}/infra
  ${ERPC}/port
  ${ERPC}/setup
  ${LIBMETAL_INCLUDE_DIR}
  ${OPENAMP_INCLUDE_DIR}
  ${PLATFORM_DIR}
)

target_sources(app PRIVATE
  ${AI64_R5_ROOT}/common/src/rpmsg.c
  ${AI64_R5_ROOT}/common/src/linux_calls.c
  ${AI64_R5_ROOT}/common/src/ipc.c
  ${AI64_R5_ROOT}/common/src/fabric.c
  ${AI64_R5_ROOT}/common/src/restart.c
  ${AI64_R5_ROOT}/common/src/mpu.c
  ${AI64_R5_ROOT}/common/src/tisci.c
  ${AI64_R5_ROOT}/common/src/sproxy.c
  ${AI64_R5_ROOT}/common/src/erpc.cpp
  ${AI64_R5_ROOT}/common/src/erpc_port_zephyr.cpp
  ${AI64_R5_ROOT}/../erpc/shim/c_ai64_server.cpp
  ${AI64_R5_ROOT}/../erpc/shim/ai64_server.cpp
  ${AI64_R5_ROOT}/../erpc/shim/ai64_interface.cpp
  ${ERPC}/infra/erpc_basic_codec.cpp
  ${ERPC}/infra/erpc_crc16.cpp
  ${ERPC}/infra/erpc_framed_transport.cpp
  ${ERPC}/infra/erpc_message_buffer.cpp
  ${ERPC}/infra/erpc_message_loggers.cpp
  ${ERPC}/infra/erpc_pre_post_action.cpp
  ${ERPC}/infra/erpc_server.cpp
  ${ERPC}/infra/erpc_simple_server.cpp
  ${ERPC}/infra/erpc_utils.cpp
  ${ERPC}/setup/erpc_server_setup.cpp
  ${ERPC}/setup/erpc_setup_mbf_dynamic.cpp
)
