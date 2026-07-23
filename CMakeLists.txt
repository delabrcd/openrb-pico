cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD adafruit_feather_rp2040)
set(PICO_SDK_PATH ${CMAKE_SOURCE_DIR}/external/pico-sdk)

# Use our own vendored TinyUSB (delabrcd/tinyusb @ 0.20) instead of the one bundled
# in pico-sdk, via the SDK's PICO_TINYUSB_PATH override. 0.20 brings the upstream
# host control-transfer retry (USBH_CONTROL_RETRY_MAX). Must be set before
# pico_sdk_init().
set(PICO_TINYUSB_PATH ${CMAKE_SOURCE_DIR}/external/tinyusb)

# FreeRTOS SMP kernel (V11.2.0, portable/ThirdParty/GCC/RP2040). The import is pulled
# in after pico_sdk_init() below so it only applies to targets that explicitly link it.
# FREERTOS_CONFIG_FILE_DIRECTORY tells the kernel where FreeRTOSConfig.h lives
# (modules/osal/, alongside the osal wrappers that wrap the kernel).
set(FREERTOS_KERNEL_PATH ${CMAKE_SOURCE_DIR}/external/FreeRTOS-Kernel)
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_SOURCE_DIR}/modules/osal)

include(pico_sdk_import.cmake)
include(cmake/AddBoardTarget.cmake)
project(openrb-pico C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Opt-in deep-backtrace debug build. When ON, the project's OWN sources (the targets
# created by add_board_target) are compiled with -Og + full unwind info so gdb can
# produce deep multi-frame backtraces. OFF (default) keeps the normal optimized build
# we ship/test for timing. The SDK / TinyUSB / FreeRTOS libraries stay at their default
# optimization to limit blast radius (we rarely need to unwind through them, and it
# keeps the debug build fast). See BUILDING.md. Use scripts/build.sh debug to configure
# a separate build-debug/ tree with this ON so the optimized build/ keeps coexisting.
option(ORB_DEBUG_BUILD "Build the project's own sources with -Og + full unwind info for debugging" OFF)

# --- Feature-gate build profiles (see docs/architecture/modern-cpp.md, BUCKET B) ------
# These four knobs were hard-coded as preprocessor #defines in the headers. They REMAIN
# preprocessor macros on purpose -- each is consumed in `#if` and gates whole #includes /
# declarations (magic_enum in orb_enum.hpp, get_command_name in xbox_one_protocol.h, the
# LOG_* level floor in orb_log.h) -- a constexpr cannot drive `#if` or zero-cost
# compile-out. What changes: their VALUES now come from the build system and are pushed
# through orb_buildconfig (an INTERFACE lib linked by every board executable + the host
# test). The headers keep their `#ifndef X / #define X <default> / #endif` fallbacks, so a
# standalone/host compile still works and CMake merely pins the per-profile value.
#
# A plain `cmake -B build` with NO preset reproduces TODAY's values (debug ON, level
# DEBUG, color OFF, hihat 0) -> the default firmware image stays byte-identical.
# ORB_DEBUG_BUILD (above) is orthogonal: it tunes optimisation/unwind, not features.
option(ORB_DEBUG "Enable debug logging + magic_enum reflection (-> OPENRB_DEBUG_ENABLED)" ON)

set(ORB_LOG_LEVEL "DEBUG" CACHE STRING "Compile-time log level floor")
set_property(CACHE ORB_LOG_LEVEL PROPERTY STRINGS NONE ERROR WARN INFO DEBUG TRACE)

option(ORB_LOG_COLOR "Bake ANSI SGR color codes into the UART log stream (-> ORB_LOG_COLOR)" OFF)

set(ORB_HIHAT_MODE "0" CACHE STRING "Hi-hat feature mode: 0=off (default), 1=discover, 2=CC override")

# Map the ORB_LOG_LEVEL string to its LOG_LEVEL_* ordinal (must match modules/log/orb_log.h).
set(_ORB_LOG_ORD_NONE 0)
set(_ORB_LOG_ORD_ERROR 1)
set(_ORB_LOG_ORD_WARN 2)
set(_ORB_LOG_ORD_INFO 3)
set(_ORB_LOG_ORD_DEBUG 4)
set(_ORB_LOG_ORD_TRACE 5)
if(NOT DEFINED _ORB_LOG_ORD_${ORB_LOG_LEVEL})
    message(FATAL_ERROR
        "ORB_LOG_LEVEL must be one of NONE/ERROR/WARN/INFO/DEBUG/TRACE (got '${ORB_LOG_LEVEL}')")
endif()
set(_ORB_LOG_LEVEL_ORD ${_ORB_LOG_ORD_${ORB_LOG_LEVEL}})

# orb_buildconfig: header-less INTERFACE lib carrying ONLY the feature-gate -D's. Linked
# into each board executable (AddBoardTarget.cmake) and the host test (test/CMakeLists.txt)
# so the defs reach every TU funnelled in as INTERFACE sources -- exactly how ORB_BOARD_ID
# is propagated onto the executable.
add_library(orb_buildconfig INTERFACE)
target_compile_definitions(orb_buildconfig INTERFACE
    OPENRB_DEBUG_ENABLED=$<BOOL:${ORB_DEBUG}>
    ORB_LOG_LEVEL=${_ORB_LOG_LEVEL_ORD}
    ORB_LOG_COLOR=$<BOOL:${ORB_LOG_COLOR}>
    ORB_HIHAT_MODE=${ORB_HIHAT_MODE})


set(PICO_PIO_USB_PATH external/Pico-PIO-USB)

# TinyUSB 0.20's hw/bsp/rp2040/family.c (compiled into tinyusb_bsp) references
# PICO_DEFAULT_PIO_USB_DP_PIN inside board_init(). We never call board_init() -- we
# only use board_millis() -- so that's dead code; define the macro purely so it
# compiles. The value (custom-board host D+ pin) is never actually used.
add_compile_definitions(PICO_DEFAULT_PIO_USB_DP_PIN=16)

# SET TUSB_DEBUG
set(LOG 0)
pico_sdk_init()

# Bring in the FreeRTOS SMP kernel (defines FreeRTOS-Kernel + FreeRTOS-Kernel-Heap4
# INTERFACE targets). Must come after pico_sdk_init() so PICO_PLATFORM is set.
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

# Propagate the FreeRTOS kernel + our FreeRTOSConfig.h onto the TinyUSB include path so
# that, once CFG_TUSB_OS=OPT_OS_FREERTOS, the TinyUSB OSAL sources can #include <FreeRTOS.h>.
# tinyusb_common is INTERFACE and is linked by tinyusb_host/tinyusb_device, so this reaches
# every TinyUSB translation unit. (Harmless while still on OPT_OS_PICO — the kernel stays
# dormant until vTaskStartScheduler() is called.)
target_link_libraries(tinyusb_common INTERFACE FreeRTOS-Kernel-Heap4)

add_subdirectory(external)

# magic_enum (Neargye/magic_enum @ v0.9.7): header-only compile-time enum reflection,
# used debug-gated via modules/log/orb_enum.hpp to render enumerator names without
# hand-rolled tables. Only the include path is needed; the tables are pulled in solely by
# DEBUG TUs (OPENRB_DEBUG_ENABLED), so a release build links none of them. Include as
# <magic_enum/magic_enum.hpp>. Kept global so any module's debug build can reach it.
include_directories(external/magic_enum/include)

# FatFs (vendored in our TinyUSB fork) for writing the log to a USB flash drive.
# Pulled from external/tinyusb (PICO_TINYUSB_PATH), NOT pico-sdk's bundled tinyusb, so
# the build depends only on the tinyusb tree we pin (the SDK's lib/tinyusb is otherwise
# unused -- the USB stack already comes from external/tinyusb). The FatFs sources are
# byte-identical between the two (revision 80286). Its disk_* callbacks are implemented in
# modules/log/usb_log.cpp.
#
# fatfs is an INTERFACE library carrying the .c as INTERFACE sources so they compile INTO
# whichever executable links it -- exactly as the old flat SOURCES list did. That matters:
# compiled in the executable, the FatFs sources inherit the pico-sdk optimisation +
# -ffunction-sections flags (and dead FatFs functions get GC'd at link). Built as a
# standalone STATIC library they'd compile at -O0 with no section GC and bloat the image
# by ~5.7 KB. The include dir is exposed (ff.h/diskio.h) and the per-file -w (FatFs has
# many warnings) is applied below, same as before the restructure.
set(FATFS_DIR ${PICO_TINYUSB_PATH}/lib/fatfs/source)
add_library(fatfs INTERFACE)
target_sources(fatfs INTERFACE
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffsystem.c
    ${FATFS_DIR}/ffunicode.c)
target_include_directories(fatfs INTERFACE ${FATFS_DIR})
# FatFs has its own (lots of) warnings; don't let them fail our build.
set_source_files_properties(${FATFS_DIR}/ff.c ${FATFS_DIR}/ffunicode.c ${FATFS_DIR}/ffsystem.c
    PROPERTIES COMPILE_FLAGS "-w")

# The layered module tree. Each modules/<layer>/CMakeLists.txt defines one INTERFACE
# target whose target_link_libraries point ONLY at the layers below it -- those edges ARE
# the architecture made physical (docs/architecture/modern-cpp.md). Added bottom-up;
# orb_app aggregates the whole graph, so each executable links just orb_app and the whole
# firmware funnels in (the pico-sdk/TinyUSB/FreeRTOS libs are themselves INTERFACE-source
# libs, so the entire program compiles into the executable -- exactly as the old flat
# SOURCES list did, leaving code size unchanged by the move).
add_subdirectory(modules/core)
add_subdirectory(modules/osal)
add_subdirectory(modules/board)
add_subdirectory(modules/platform/pico)
add_subdirectory(modules/hal)
add_subdirectory(modules/log)
add_subdirectory(modules/protocol)
add_subdirectory(modules/service)
add_subdirectory(modules/driver)
add_subdirectory(modules/app)

add_board_target(${PROJECT_NAME} FEATHER)
add_board_target(${PROJECT_NAME} CUSTOM_REV_0_1)