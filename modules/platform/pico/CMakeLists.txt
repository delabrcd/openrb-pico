# platform/pico/ — THE pico-sdk consumer for the HAL value types (GpioOut, Clock, Uart,
# CriticalSection). Header-only concrete implementations that include hardware/* + pico/*.
#
# Conceptually THE pico-sdk consumer for the HAL: hal/ -> platform/pico/ is the only path
# down to hardware/* + pico/*. The actual pico-sdk libraries are linked at the executable
# (cmake/AddBoardTarget.cmake), where the whole firmware is compiled, because the pico-sdk
# targets are INTERFACE-source libs -- linking them here would pull EXTRA transitive sdk
# sources (pico_stdlib drags pico_stdio/pico_stdio_uart) into the image and grow it past
# the pre-restructure size. So this target only exposes the modules/ include root (so
# "platform/pico/gpio.hpp" resolves); the hardware/* headers resolve through the
# executable's vendored links, exactly as before the restructure.
add_library(orb_platform_pico INTERFACE)
target_include_directories(orb_platform_pico INTERFACE ${CMAKE_SOURCE_DIR}/modules)
