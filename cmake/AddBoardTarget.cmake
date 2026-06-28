function(add_board_target PROJECT_NAME BOARD_NAME SOURCES)
    set(COMMON_COMPILE_DEFS -DCFG_TUSB_CONFIG_FILE="${CMAKE_SOURCE_DIR}/inc/custom_config.h")
    set(TARGET_NAME ${PROJECT_NAME}_${BOARD_NAME})

    add_executable(${TARGET_NAME} ${SOURCES})

    # Opt-in deep-backtrace debug build (-DORB_DEBUG_BUILD=ON, see CMakeLists.txt /
    # BUILDING.md). PRIVATE so these flags apply ONLY to this target's own sources
    # (src/*) -- NOT to the SDK / TinyUSB / FreeRTOS interface libraries, which are
    # separate targets and stay at their default optimization. The options are appended
    # after any earlier -O flag, and GCC honours the last -O on the command line, so
    # -Og overrides the default optimization for our sources. -g is kept (file:line
    # already resolves today); -g3 adds macro info; the frame-pointer / unwind-table
    # flags give gdb deep, reliable multi-frame backtraces.
    if(ORB_DEBUG_BUILD)
        target_compile_options(${TARGET_NAME} PRIVATE
            -Og -g3 -fno-omit-frame-pointer -funwind-tables -fasynchronous-unwind-tables)
    endif()

    target_compile_definitions(${TARGET_NAME} PUBLIC ${COMMON_COMPILE_DEFS} -DORB_BOARD_ID=ORB_BOARD_ID_${BOARD_NAME})
    target_link_libraries(${TARGET_NAME} PUBLIC pico_pio_usb tinyusb_bsp tinyusb_host tinyusb_device usb_midi_host
        FreeRTOS-Kernel-Heap4)
    pico_add_extra_outputs(${TARGET_NAME})
endfunction()