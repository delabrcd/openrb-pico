/*
 * HOST stub for TinyUSB's <bsp/board_api.h>.
 *
 * src/xbox_one_protocol.cpp includes "bsp/board_api.h" only to call board_millis()
 * (used as the triggered_time stamp in fill_guitar_input_from_hid_report). The real
 * header drags in the whole TinyUSB board layer, none of which is portable. For the
 * host test build we put this directory FIRST on the include path so this minimal
 * shim wins, declaring just the one symbol the protocol TU needs. The definition
 * (a settable fake clock) lives in host_stubs.cpp.
 */
#ifndef OPENRB_HOST_STUB_BSP_BOARD_API_H
#define OPENRB_HOST_STUB_BSP_BOARD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t board_millis(void);

#ifdef __cplusplus
}
#endif

#endif  // OPENRB_HOST_STUB_BSP_BOARD_API_H
