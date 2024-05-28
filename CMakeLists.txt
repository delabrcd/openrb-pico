cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD adafruit_feather_rp2040)
set(PICO_SDK_PATH ${CMAKE_SOURCE_DIR}/external/pico-sdk)

include(pico_sdk_import.cmake)

project(openrb-pico C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


set(PICO_PIO_USB_PATH external/Pico-PIO-USB)

# SET TUSB_DEBUG
set(LOG 0)
pico_sdk_init()

add_subdirectory(external)
include_directories(inc)

set(SOURCES
    src/main.c
    src/drums.c
    src/packet_queue.c
    src/host_drivers.c
    src/usb_descriptors.c
    src/xbox_controller_driver.c
    src/xbox_device_driver.c
    src/xbox_one_protocol.c
    src/wla_identifiers.c
    src/instrument_manager.c
    src/midi.c
    src/guitar.c
    # got this hack from PIO example to fix some missing dependencies - should be fixed in later PICO SDK versions
    # https://github.com/sekigon-gonnoc/Pico-PIO-USB/blob/0f747aaa0c16f750bdfa2ba37ec25d6c8e1bc117/examples/host_hid_to_device_cdc/CMakeLists.txt#L8
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PUBLIC -DCFG_TUSB_CONFIG_FILE="${CMAKE_SOURCE_DIR}/inc/custom_config.h")

target_link_libraries(${PROJECT_NAME} PUBLIC pico_pio_usb tinyusb_bsp tinyusb_host tinyusb_device usb_midi_host)

pico_add_extra_outputs(${PROJECT_NAME})