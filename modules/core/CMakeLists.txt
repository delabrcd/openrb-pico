# core/ — pure portable utilities, ZERO dependencies (header-only). The bottom of the
# stack: Result, SpscRing, function_ref, static_vector, the shared orb_c_api.h C-seam
# header. Nothing here may include the SDK / RTOS / TinyUSB.
#
# INTERFACE library: there are no .cpp, so this target only carries usage requirements
# (the include roots). The two include dirs make BOTH include styles resolve for every
# consumer that links orb_core:
#   * ${CMAKE_SOURCE_DIR}/modules  -> layer-prefixed "core/result.hpp", "osal/task.hpp", ...
#   * this dir                     -> flat "orb_c_api.h"
add_library(orb_core INTERFACE)
target_include_directories(orb_core INTERFACE
    ${CMAKE_SOURCE_DIR}/modules
    ${CMAKE_CURRENT_SOURCE_DIR})
