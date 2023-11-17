# Set Arduino library paths
set(ARDUINO_SDK_PATH ${CMAKE_SOURCE_DIR})

# Include necessary directories
include_directories(
    ${ARDUINO_SDK_PATH}/cores/arduino
    ${ARDUINO_SDK_PATH}/variants/adafruit_feather_usb_host
    # Add any other necessary paths
)

