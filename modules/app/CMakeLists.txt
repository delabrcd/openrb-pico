# app/ — orchestration: main (boot/recovery/clock + the core0/core1 wiring), the task
# bodies (app_tasks), and the FreeRTOS hooks. The top of the stack; it links every layer
# below, so the executable need only link orb_app to transitively pull the whole tree
# (all INTERFACE sources + every dependency's include dirs + the vendored USB/RTOS libs).
#
# INTERFACE library (see modules/log/CMakeLists.txt). Downward edges to all lower module
# layers make orb_app the single aggregation point: the executable links orb_app and the
# whole module graph (every INTERFACE source + include dir) funnels in. freertos_hooks.cpp
# implements the kernel callbacks; the FreeRTOS kernel itself is linked at the executable.
#
# main.cpp is BOARD-DEPENDENT (#if ORB_BOARD_ID ... + orb_bsp.h), so like service/midi.cpp
# it is exported via ORB_APP_BOARD_SOURCES and compiled per-board into each executable
# (see AddBoardTarget.cmake) rather than carried as an INTERFACE source here.
add_library(orb_app INTERFACE)
target_sources(orb_app INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/app_tasks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/device_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_hooks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/recovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/system.cpp)
target_include_directories(orb_app INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_app INTERFACE
    orb_service orb_driver orb_protocol orb_log
    orb_osal orb_hal orb_platform_pico orb_board orb_core)

# Board-dependent TUs (ORB_BOARD_ID / orb_bsp.h): compiled per-executable, not here.
# host_controller.cpp joins main.cpp here (not the INTERFACE target_sources above) because
# it touches orb_bsp.h's ORB_BOARD_ID-gated host_recovery() split.
set(ORB_APP_BOARD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host_controller.cpp
    CACHE INTERNAL "board-dependent app TUs compiled per-executable")
