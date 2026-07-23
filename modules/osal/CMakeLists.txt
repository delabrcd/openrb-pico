# osal/ — the OS-abstraction interface (Task, Queue, Mutex, SwTimer). Header-only
# wrappers over the FreeRTOS kernel; the only RTOS-aware layer besides platform/.
#
# INTERFACE library exposing the modules/ root so the layer-prefixed "osal/task.hpp"
# resolves. The FreeRTOS kernel itself (FreeRTOS-Kernel-Heap4, an INTERFACE-source lib)
# and its FreeRTOSConfig.h directory (FREERTOS_CONFIG_FILE_DIRECTORY -> modules/osal/) are
# linked at the executable (see cmake/AddBoardTarget.cmake), which is where every source
# is compiled -- so the osal headers' "FreeRTOS.h"/"task.h" includes resolve there. Linking
# the kernel here too would pull EXTRA pico-sdk transitive sources into the closure (e.g.
# pico_stdio) and bloat the image, so the SDK libs stay at the single executable, matching
# the pre-restructure link set exactly.
add_library(orb_osal INTERFACE)
target_include_directories(orb_osal INTERFACE ${CMAKE_SOURCE_DIR}/modules)
