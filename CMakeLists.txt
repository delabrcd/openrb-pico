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
# FREERTOS_CONFIG_FILE_DIRECTORY tells the kernel where inc/FreeRTOSConfig.h lives.
set(FREERTOS_KERNEL_PATH ${CMAKE_SOURCE_DIR}/external/FreeRTOS-Kernel)
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_SOURCE_DIR}/inc)

include(pico_sdk_import.cmake)
include(cmake/AddBoardTarget.cmake)
project(openrb-pico C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
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
include_directories(inc)

# FatFs (vendored in our TinyUSB fork) for writing the log to a USB flash drive.
# Pulled from external/tinyusb (PICO_TINYUSB_PATH), NOT pico-sdk's bundled tinyusb, so
# the build depends only on the tinyusb tree we pin (the SDK's lib/tinyusb is otherwise
# unused -- the USB stack already comes from external/tinyusb). The FatFs sources are
# byte-identical between the two (revision 80286).
set(FATFS_DIR ${PICO_TINYUSB_PATH}/lib/fatfs/source)
include_directories(${FATFS_DIR})
# FatFs has its own (lots of) warnings; don't let them fail our build.
set_source_files_properties(${FATFS_DIR}/ff.c ${FATFS_DIR}/ffunicode.c ${FATFS_DIR}/ffsystem.c
    PROPERTIES COMPILE_FLAGS "-w")

set(SOURCES
    src/main.cpp
    src/adapter_ctx.cpp
    src/app_queues.cpp
    src/app_tasks.cpp
    src/freertos_hooks.c
    src/dlog.cpp
    src/orb_log.c
    src/usb_log.cpp
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffsystem.c
    ${FATFS_DIR}/ffunicode.c
    src/drums.cpp
    src/packet_queue.c
    src/host_drivers.cpp
    src/usb_descriptors.cpp
    src/xbox_controller_driver.c
    src/xbox_device_driver.c
    src/xbox_one_protocol.c
    src/wla_identifiers.cpp
    src/instrument_manager.cpp
    src/midi.cpp
    src/guitar.cpp
    # got this hack from PIO example to fix some missing dependencies - should be fixed in later PICO SDK versions
    # https://github.com/sekigon-gonnoc/Pico-PIO-USB/blob/0f747aaa0c16f750bdfa2ba37ec25d6c8e1bc117/examples/host_hid_to_device_cdc/CMakeLists.txt#L8
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

set(COMMON_COMPILE_DEFS -DCFG_TUSB_CONFIG_FILE="${CMAKE_SOURCE_DIR}/inc/custom_config.h")

add_board_target(${PROJECT_NAME} FEATHER "${SOURCES}")
add_board_target(${PROJECT_NAME} CUSTOM_REV_0_1 "${SOURCES}")