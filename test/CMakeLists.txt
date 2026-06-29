# HOST unit-test build for the portable logic layer (protocol module).
#
# This is intentionally a STANDALONE host build -- it uses the machine's native
# g++/clang, NOT the ARM cross-toolchain / Pico SDK. It is never referenced by the
# firmware's top-level CMakeLists, so the firmware build is unaffected.
#
#   cmake -S test -B test/build
#   cmake --build test/build
#   ./test/build/protocol_tests
#
# It compiles the REAL src/xbox_one_protocol.cpp for the host and links it against
# minimal stubs (test/host_stubs) plus the doctest tests, so the tests exercise the
# actual protocol functions.

cmake_minimum_required(VERSION 3.16)
project(openrb_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # protocol TU uses __attribute__((packed)) etc.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(DOCTEST_DIR "${REPO_ROOT}/external/doctest")
if(NOT EXISTS "${DOCTEST_DIR}/doctest/doctest.h")
  message(FATAL_ERROR
    "doctest submodule not found at ${DOCTEST_DIR}. Run: git submodule update --init external/doctest")
endif()

add_executable(protocol_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/test_xbox_one_protocol.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs/host_stubs.cpp
  ${REPO_ROOT}/modules/protocol/xbox_one_protocol.cpp
)

# The protocol TU now lives in modules/protocol/ and pulls headers from the protocol/log/
# core layers (flat includes), so put each of those module dirs on the host include path.
# magic_enum is needed because OPENRB_DEBUG_ENABLED defaults to 1 (debug logging compiled
# in), so modules/log/orb_enum.hpp pulls <magic_enum/magic_enum.hpp>.
target_include_directories(protocol_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host_stubs       # bsp/board_api.h stub -- MUST resolve first
  ${REPO_ROOT}/modules                         # layer-prefixed includes (core/..., osal/...)
  ${REPO_ROOT}/modules/protocol                # xbox_one_protocol.h, identifiers.h
  ${REPO_ROOT}/modules/log                     # orb_log.h / orb_debug.h / orb_enum.hpp
  ${REPO_ROOT}/modules/core                    # orb_c_api.h
  ${REPO_ROOT}/external/magic_enum/include     # <magic_enum/magic_enum.hpp> (debug path)
  ${DOCTEST_DIR}                               # #include "doctest/doctest.h"
)

# Keep firmware-style debug logging compiled in (default) so the LOG_*/orb_debug.h
# path through the protocol TU is actually built and linked against the no-op stubs.
target_compile_options(protocol_tests PRIVATE -Wall -Wextra)

# Mirror the firmware's DEFAULT profile feature gates (the orb_buildconfig -D's the board
# executables get) so the host test exercises the same configuration. Values match the
# headers' #ifndef fallbacks, so the build is unchanged -- this just makes them explicit.
# (ORB_LOG_LEVEL 4 == LOG_LEVEL_DEBUG in modules/log/orb_log.h.)
target_compile_definitions(protocol_tests PRIVATE
  OPENRB_DEBUG_ENABLED=1
  ORB_LOG_LEVEL=4
  ORB_LOG_COLOR=0
  ORB_HIHAT_MODE=0)

enable_testing()
add_test(NAME protocol_tests COMMAND protocol_tests)
