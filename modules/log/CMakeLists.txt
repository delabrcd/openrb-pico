# log/ — the deferred, per-core logging stack: dlog (UART backend), orb_log (the public
# LOG_*/orb_debug API + ring buffers), usb_log (the MSC/FatFs log-to-flash sink), and the
# debug-gated orb_enum.hpp (magic_enum wrapper). Foundational: it sits BELOW protocol and
# everything above, because the protocol/service/driver headers log via orb_debug.h.
#
# INTERFACE library. The pico-sdk / TinyUSB / FreeRTOS libraries are themselves INTERFACE
# libraries whose SOURCES are injected into whatever concrete target links them, so the
# whole firmware is compiled into the executable (see the top-level CMakeLists + cmake/
# AddBoardTarget.cmake). Our modules therefore mirror that model: each is an INTERFACE lib
# carrying its .cpp as INTERFACE sources, its include dir, and link edges to the layers
# below -- those edges are the architecture, and they all funnel into the one executable,
# compiled once.
#
# Downward module edges: orb_core (orb_c_api.h + core/spsc_ring.hpp) and orb_hal
# (usb_log.cpp -> hal/platform.hpp). The vendored bits usb_log.cpp needs -- FatFs
# (ff.h/diskio.h; disk_* is implemented here), TinyUSB (tusb.h) -- and the hardware/*
# pokes in dlog/orb_log resolve through the executable's vendored links (those libs are
# linked once at the executable to keep the link closure identical to the pre-restructure
# build; see cmake/AddBoardTarget.cmake).
add_library(orb_log INTERFACE)
target_sources(orb_log INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/dlog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/orb_log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_log.cpp)
target_include_directories(orb_log INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_log INTERFACE orb_core orb_hal)
