# hal/ — hardware-abstraction INTERFACE: concepts (GpioOutput, Uart, Clock, Irq) plus
# the platform-selection header hal/platform.hpp, which binds the concepts to the
# concrete platform/pico/ types. Header-only.
#
# Linking orb_platform_pico PUBLICly is the layering edge that says "the HAL is realised
# by platform/pico/": consumers of hal/ transitively get the pico implementation (and,
# through it, the pico-sdk include path). Above hal/, code depends on hal/ -- never on
# platform/pico/ directly.
add_library(orb_hal INTERFACE)
target_include_directories(orb_hal INTERFACE ${CMAKE_SOURCE_DIR}/modules)
target_link_libraries(orb_hal INTERFACE orb_platform_pico)
