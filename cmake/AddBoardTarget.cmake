# add_board_target(<project> <BOARD_NAME>) -- create one firmware executable for a board.
#
# The layered module libraries (modules/<layer>/) are INTERFACE libs whose .cpp are
# INTERFACE sources; linking orb_app drags the whole tree (every layer's sources + include
# dirs + the vendored USB/RTOS libs) into THIS executable, where it is all compiled once --
# exactly as the old flat SOURCES list did (so code size is unchanged by the restructure).
#
# The ONLY per-board variation is ORB_BOARD_ID, which reaches exactly two TUs through
# orb_bsp.h: service/midi.cpp and app/main.cpp. Those are exported by their modules as
# ORB_SERVICE_BOARD_SOURCES / ORB_APP_BOARD_SOURCES and compiled HERE, straight into each
# executable, so each board gets its own ORB_BOARD_ID while the rest of the tree stays
# board-agnostic. (They also give the executable its own concrete sources.)
function(add_board_target PROJECT_NAME BOARD_NAME)
    set(TARGET_NAME ${PROJECT_NAME}_${BOARD_NAME})

    add_executable(${TARGET_NAME}
        ${ORB_APP_BOARD_SOURCES}
        ${ORB_SERVICE_BOARD_SOURCES})

    # orb_app -> the whole module graph (INTERFACE sources + usage requirements + the
    # vendored libs it transitively links). fatfs -> the FatFs archive. The vendored
    # USB/RTOS libs are also listed explicitly (deduped) for robust final symbol resolution,
    # exactly the set the old single target listed.
    target_link_libraries(${TARGET_NAME} PRIVATE
        orb_app
        fatfs
        pico_pio_usb tinyusb_bsp tinyusb_host tinyusb_device usb_midi_host
        FreeRTOS-Kernel-Heap4)

    # Opt-in deep-backtrace debug build (-DORB_DEBUG_BUILD=ON, see CMakeLists.txt /
    # BUILDING.md). PRIVATE so these flags apply to this executable's compilation -- which,
    # because the whole firmware funnels in as INTERFACE sources, is every one of our TUs
    # plus the vendored sources, matching the previous single-target build.
    if(ORB_DEBUG_BUILD)
        target_compile_options(${TARGET_NAME} PRIVATE
            -Og -g3 -fno-omit-frame-pointer -funwind-tables -fasynchronous-unwind-tables)
    endif()

    # Embedded C++ policy (see docs/features/cpp-overhaul.md). Scoped to C++ TUs with a
    # generator expression so the C sources don't warn on the C++-only flags.
    #   -fno-exceptions / -fno-rtti / -fno-threadsafe-statics : no unwinder, no RTTI, no
    #       hidden __cxa_guard lock on function-local statics (unacceptable on core1).
    #   -Wno-volatile : silence the vendored FreeRTOS portmacro.h volatile compound-ops.
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics -Wno-volatile>)

    # CFG_TUSB_CONFIG_FILE is board-independent (same custom_config.h for both boards, now
    # in modules/driver/). ORB_BOARD_ID selects the board pin map in orb_bsp.h.
    target_compile_definitions(${TARGET_NAME} PUBLIC
        -DCFG_TUSB_CONFIG_FILE="${CMAKE_SOURCE_DIR}/modules/driver/custom_config.h"
        -DORB_BOARD_ID=ORB_BOARD_ID_${BOARD_NAME})

    pico_add_extra_outputs(${TARGET_NAME})
endfunction()
