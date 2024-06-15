function(add_board_target PROJECT_NAME BOARD_NAME SOURCES)
    set(COMMON_COMPILE_DEFS -DCFG_TUSB_CONFIG_FILE="${CMAKE_SOURCE_DIR}/inc/custom_config.h")
    set(TARGET_NAME ${PROJECT_NAME}_${BOARD_NAME})

    add_executable(${TARGET_NAME} ${SOURCES})
    target_compile_definitions(${TARGET_NAME} PUBLIC ${COMMON_COMPILE_DEFS} -DORB_BOARD_ID=ORB_BOARD_ID_${BOARD_NAME})
    target_link_libraries(${TARGET_NAME} PUBLIC pico_pio_usb tinyusb_bsp tinyusb_host tinyusb_device usb_midi_host)
    pico_add_extra_outputs(${TARGET_NAME})
endfunction()