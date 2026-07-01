# board/ — board pin maps + the ORB_BOARD_ID selector header (orb_bsp.h). Header-only.
#
# These headers are board-conditional: orb_bsp.h #errors unless ORB_BOARD_ID is defined,
# and it is the per-board compile def applied to the EXECUTABLE (see AddBoardTarget.cmake).
# The two files that include orb_bsp.h (service/midi.cpp, app/main.cpp) are therefore the
# ONLY board-dependent TUs and are compiled per-board straight into each executable, not
# into the shared object libraries. This INTERFACE target just exposes the include dir so
# those flat includes ("orb_bsp.h", "pins_*.h") resolve.
add_library(orb_board INTERFACE)
target_include_directories(orb_board INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# actuators.cpp is BOARD-DEPENDENT (#if ORB_BOARD_ID + orb_bsp.h / pin map), so like
# app/main.cpp + service/midi.cpp it is compiled per-board straight into each executable
# rather than as a shared INTERFACE source (see cmake/AddBoardTarget.cmake).
set(ORB_BOARD_BOARD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/actuators.cpp
    CACHE INTERNAL "board-dependent board TUs compiled per-executable")
