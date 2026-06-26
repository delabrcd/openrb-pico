#ifndef ORB_DEBUG_H
#define ORB_DEBUG_H

#define OPENRB_DEBUG_ENABLED 0

#if OPENRB_DEBUG_ENABLED
#include <stdint.h>
#include <stdio.h>

#include "dlog.h"

// Deferred hex dump: format a chunk at a time so we don't pay a vsnprintf per
// byte, and emit through the per-core deferred ring (never blocks the caller).
static inline void print_buf(uint8_t const *buf, uint32_t bufsize) {
    char line[3 * 16 + 1];
    uint32_t p = 0;
    for (uint32_t i = 0; i < bufsize; i++) {
        p += (uint32_t)snprintf(line + p, sizeof(line) - p, "%02X ", buf[i]);
        if (p >= sizeof(line) - 3) {
            dlog_printf("%s", line);
            p = 0;
        }
    }
    if (p) dlog_printf("%s", line);
}

#define OPENRB_DEBUG(...) dlog_printf(__VA_ARGS__)
#define OPENRB_DEBUG_BUF(_x, _n) print_buf((uint8_t const *)(_x), _n)

#else

#define OPENRB_DEBUG(...)
#define OPENRB_DEBUG_BUF(...)

#endif
#endif
