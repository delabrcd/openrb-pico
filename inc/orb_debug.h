#ifndef ORB_DEBUG_H
#define ORB_DEBUG_H

// #define OPENRB_DEBUG_ENABLED

#ifdef OPENRB_DEBUG_ENABLED
#include <stdio.h>

static inline void print_buf(uint8_t const *buf, uint32_t bufsize) {
    for (uint32_t i = 0; i < bufsize; i++) printf("%02X ", buf[i]);
}

#define OPENRB_DEBUG(...) printf(__VA_ARGS__)
#define OPENRB_DEBUG_BUF(_x, _n) print_buf((uint8_t const *)(_x), _n)

#else

#define OPENRB_DEBUG(...)
#define OPENRB_DEBUG_BUF(...)

#endif
#endif