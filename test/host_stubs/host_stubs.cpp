/*
 * HOST-only link stubs for the symbols src/xbox_one_protocol.cpp pulls in that are not
 * portable / live in other (hardware) TUs:
 *
 *   - board_millis()        : TinyUSB board clock -> a settable fake counter.
 *   - orb_log_emit()        : orb::log front-end producer (orb_log.h)  -> no-op.
 *   - orb_log_hexdump()     : orb::log hex-dump producer               -> no-op.
 *   - orb_log_tusb_printf() : TinyUSB CFG_TUSB_DEBUG_PRINTF vendor seam -> no-op.
 *
 * orb_log_emit / orb_log_hexdump are plain C++ free functions in orb_log.h; board_millis
 * and orb_log_tusb_printf are vendor C-linkage seams (extern "C"). The protocol TU
 * references the LOG_* macros transitively via orb_debug.h, so they must resolve at link
 * time even though the functions under test never emit a line.
 */
#include <cstdarg>
#include <cstdint>

#include "host_test_support.h"

std::uint32_t g_host_fake_millis = 0;
std::uint32_t g_host_fake_us = 0;

extern "C" {

uint32_t board_millis(void) { return g_host_fake_millis; }

int orb_log_tusb_printf(const char* /*fmt*/, ...) { return 0; }

}  // extern "C"

void orb_log_emit(int /*level*/, int /*cat*/, const char* /*fmt*/, ...) {}

void orb_log_hexdump(int /*level*/, int /*cat*/, const void* /*data*/, uint32_t /*len*/) {}
