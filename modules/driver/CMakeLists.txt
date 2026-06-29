# driver/ — device classes bound to USB hardware behind the extern "C" TinyUSB seam:
# the Xbox device (xbox_device_driver) + host controller (xbox_controller_driver), the
# USB descriptors, the host-driver registration table (host_drivers), and the vendored
# Pico-PIO-USB dcd/hcd glue compiled into the project.
#
# INTERFACE library (see modules/log/CMakeLists.txt). Downward module edges to protocol/
# service/log/hal/core (driver headers reference xbox_one_protocol.h, adapter.h,
# packet_queue.h, orb_log.h). The vendored USB stack this layer drives -- TinyUSB
# host/device/bsp, usb_midi_host, Pico-PIO-USB (the dcd/hcd glue + pio_usb headers) -- is
# linked at the executable, where these INTERFACE sources are compiled.
add_library(orb_driver INTERFACE)
target_sources(orb_driver INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/xbox_controller_driver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/xbox_device_driver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_descriptors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host_drivers.cpp
    # PIO-USB glue (from the PIO example) -- pulled from our vendored TinyUSB tree.
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c)
target_include_directories(orb_driver INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_driver INTERFACE
    orb_core orb_protocol orb_service orb_log orb_hal)
