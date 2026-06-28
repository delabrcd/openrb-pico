#ifndef ORB_DEBUG_H
#define ORB_DEBUG_H

// Compatibility shim. The real logger now lives in orb_log.h; this header only
// keeps the legacy OPENRB_DEBUG / OPENRB_DEBUG_BUF macros working so the existing
// call sites compile and emit through the new pipeline unchanged.

#ifndef OPENRB_DEBUG_ENABLED
#define OPENRB_DEBUG_ENABLED 1
#endif

// Map the legacy all-or-nothing switch onto the compile-time level floor. When
// disabled, force the floor to NONE BEFORE orb_log.h so every LOG_* (and thus
// every OPENRB_DEBUG) compiles out to nothing.
#if !OPENRB_DEBUG_ENABLED
#define ORB_LOG_LEVEL LOG_LEVEL_NONE
#endif

#include "orb_log.h"

// Legacy aliases -> new front end. Call sites are NOT touched: any hand-written
// "\r\n" in their message is harmless (the front end appends its own line ending).
#define OPENRB_DEBUG(...) LOG_INFO(CAT_SYS, __VA_ARGS__)
#define OPENRB_DEBUG_BUF(_x, _n) LOG_HEXDUMP(CAT_SYS, LOG_LEVEL_DEBUG, (_x), (_n))

#endif  // ORB_DEBUG_H
