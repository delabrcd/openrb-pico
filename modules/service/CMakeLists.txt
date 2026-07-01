# service/ — feature logic: the adapter state machine + context (adapter_ctx),
# InstrumentManager, the DrumEngine (drums), Guitar, the MIDI parser (midi), and the
# cross-layer plumbing those depend on: packet_queue (xbox packet queue) and app_queues
# (the inter-task queue instances). packet_queue/app_queues live HERE -- not in
# driver/app as the layer sketch first guessed -- because drums/guitar/instrument_manager
# (service) include them, so they must sit at or below service for the edges to point
# downward. driver/app reach them by linking service.
#
# INTERFACE library (see modules/log/CMakeLists.txt). Downward module edges: core/protocol
# /osal/log + board (the per-board midi.cpp needs orb_bsp.h). This layer is not yet
# SDK-free (P3 in docs/architecture/modern-cpp.md): guitar.cpp uses tusb.h + hid_host,
# drums.cpp uses usb_midi_host.h + board_api.h, and midi/drums poke hardware/* and <pico.h>
# directly -- all of which resolve through the executable's vendored links.
#
# midi.cpp is BOARD-DEPENDENT (includes orb_bsp.h -> needs ORB_BOARD_ID): it is NOT an
# INTERFACE source here but is exported via ORB_SERVICE_BOARD_SOURCES and compiled per-board
# straight into each executable (see AddBoardTarget.cmake), so every other TU stays
# board-agnostic.
add_library(orb_service INTERFACE)
target_sources(orb_service INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/drums.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/instrument_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/guitar.cpp)
target_include_directories(orb_service INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_service INTERFACE
    orb_core orb_protocol orb_osal orb_log orb_board)

# Board-dependent TU (orb_bsp.h / ORB_BOARD_ID): compiled per-executable, not here.
set(ORB_SERVICE_BOARD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/midi.cpp
    CACHE INTERNAL "board-dependent service TUs compiled per-executable")
