cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ai64_r5 C CXX)

# EmbeddedRPC 1.14.0, cloned by build.sh. It is not used as a Zephyr module:
# that module's Kconfig selects SERIAL from a symbol that depends on the full
# C++ library, and the loop fails configuration. The library sources are
# compiled here, with erpc_malloc/erpc_free on the Zephyr heap. Transport is
# the existing OpenAMP RPMsg endpoint, not the module's UART or RPMsg-Lite
# ports.
set(ERPC ${CMAKE_CURRENT_LIST_DIR}/../../third_party/erpc/erpc_c)

# "r5f0_0" for board beaglebone_ai64/j721e/main_r5f0_0. The code names its
# core in replies and logs with it.
string(REGEX MATCH "r5f[0-9]_[0-9]" AI64_CORE "${BOARD_QUALIFIERS}")
if(NOT AI64_CORE)
  message(FATAL_ERROR "zephyr/r5 builds for a main-domain R5F core, not ${BOARD}${BOARD_QUALIFIERS}")
endif()
target_compile_definitions(app PRIVATE AI64_CORE="${AI64_CORE}")

# Its node number on the fabric (zephyr/include/ai64_fabric.h).
set(AI64_FAB_NODE_r5f0_0 1)
set(AI64_FAB_NODE_r5f0_1 2)
set(AI64_FAB_NODE_r5f1_0 3)
set(AI64_FAB_NODE_r5f1_1 4)
target_compile_definitions(app PRIVATE AI64_FAB_NODE=${AI64_FAB_NODE_${AI64_CORE}})

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../erpc/shim
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${ERPC}/infra
  ${ERPC}/port
  ${ERPC}/setup
  ${LIBMETAL_INCLUDE_DIR}
  ${OPENAMP_INCLUDE_DIR}
  ${PLATFORM_DIR}
)

target_sources(app PRIVATE
  src/main.c
  src/ai64_service.c
  src/ai64_restart.c
  src/ai64_mpu.c
  src/ai64_tisci.c
  src/ai64_fabric.c
  src/ai64_erpc.cpp
  src/erpc_port_zephyr.cpp
  ../erpc/shim/c_ai64_server.cpp
  ../erpc/shim/ai64_server.cpp
  ../erpc/shim/ai64_interface.cpp
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
