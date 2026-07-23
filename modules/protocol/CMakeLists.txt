# protocol/ — the Xbox One wire protocol: packet builders/parsers (xbox_one_protocol)
# and the WLA controller identifiers. Portable logic that is also exercised by the host
# unit tests (see test/CMakeLists.txt, which compiles xbox_one_protocol.cpp natively).
#
# INTERFACE library (see modules/log/CMakeLists.txt for why our modules are INTERFACE).
# Downward module edges: orb_core (orb_c_api.h in the public header) and orb_log
# (xbox_one_protocol.h includes orb_debug.h). board_millis() via bsp/board_api.h and the
# <pico.h> in wla_identifiers.cpp resolve through the executable's vendored links.
add_library(orb_protocol INTERFACE)
target_sources(orb_protocol INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/xbox_one_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wla_identifiers.cpp)
target_include_directories(orb_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_protocol INTERFACE orb_core orb_log)
