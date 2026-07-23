#pragma once

#include <cstdint>

// Unified deferred logging front end. Sits ON TOP OF the per-core SPSC ring in
// dlog.cpp and adds: 5 log levels, category tags, a producer-side
// timestamp, a column-aligned line format, and optional inline ANSI color.
//
// Format on produce: each call snapshots timer_hw->timerawl + cpuid on the
// producing core, renders prefix+message into one stack buffer, and pushes it to
// the calling core's ring with a SINGLE dlog producer call (one head store --
// preserves dlog's batch-publish + drop-on-full discipline). Safe to call from
// the timing-critical core1 PIO-USB task: only timerawl + cpuid are touched, no
// locks, no sleep/board_millis (see docs/architecture.md gotcha #2).
//
// Implementation: src/orb_log.cpp, modern C++ in `namespace orb::log`. The LOG_*
// macros below are the public API and expand to orb_log_emit(...) -- a plain C++
// free function (every caller is C++). orb_log_tusb_printf is the permanent TinyUSB
// CFG_TUSB_DEBUG_PRINTF vendor seam and keeps C linkage. The runtime config API
// (init/set_level/...) is C++-only -- callers are C++.

// --- Levels (ordered; higher == more verbose; 0 == off) ----------------------
#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL_TRACE 5

// Compile-time floor: macros above this expand to nothing (zero-cost -- arguments
// are never evaluated). Override on the command line to compile out logging.
#ifndef ORB_LOG_LEVEL
#define ORB_LOG_LEVEL LOG_LEVEL_DEBUG
#endif

// Inline ANSI color in the log stream. Default OFF: the firmware emits PLAIN text so
// both persisted sinks (the .mon/uart.log capture and LOG.TXT on the stick) stay clean
// and greppable. Color is applied host-side at view time by scripts/uart.sh, which
// colorizes by level only when writing to a terminal. Set to 1 to bake SGR codes into
// the UART stream instead (usb_log still strips them before LOG.TXT).
#ifndef ORB_LOG_COLOR
#define ORB_LOG_COLOR 0
#endif

// --- Categories --------------------------------------------------------------
// Scoped enum (enum class) per the modern-C++ idiom; the enumerators keep their CAT_*
// names so the many LOG_*(CAT_X, ...) call sites read unchanged. An explicit uint8_t
// underlying type matches the per-category atomic<uint8_t> level table.
namespace orb::log {
enum class OrbCat : uint8_t {
    CAT_SYS = 0,
    CAT_HOST,
    CAT_DEV,
    CAT_DRUM,
    CAT_MIDI,
    CAT_RECOV,
    CAT_USBLOG,
    CAT_TUSB,
    CAT_WIRE,  // raw USB packet traffic (per-frame IN/OUT + hex dumps) -- its own category so
               // it can be muted independently of CAT_DEV/CAT_HOST flow, even at the TRACE floor
    CAT_COUNT
};
}  // namespace orb::log

// Bring the CAT_* enumerators to global scope so every existing LOG_*(CAT_X, ...) /
// OPENRB_DEBUG call site keeps compiling unchanged in spelling (the ORB_LOG_/LOG_HEXDUMP
// macros static_cast the category to int for the int-taking producers below).
using enum orb::log::OrbCat;

// --- Runtime control ---------------------------------------------------------
// Above the compile-time floor, an atomic<uint8_t> table gates at run time
// (relaxed load/store -> plain ldrb/strb on M0+, no RMW). Written from core0
// (config), read from both cores; a stale read merely admits/drops one line --
// benign, so no lock (consistent with dlog's lock-free philosophy). C++-only API;
// the current callers (main) are C++.
#ifdef __cplusplus
namespace orb::log {
void init();                                  // wraps dlog_init() + usb_log sink
void set_level(int level);                  // global runtime floor (all cats)
void set_cat_level(OrbCat cat, int level);  // per-category override
int get_level();                               // current global runtime floor
}  // namespace orb::log
#endif

// --- Producers / vendor seam -------------------------------------------------
// orb_log_emit / orb_log_hexdump: plain C++ free functions the LOG_* macros expand
// into; they forward into orb::log.
// orb_log_tusb_printf: the permanent TinyUSB CFG_TUSB_DEBUG_PRINTF target -- a vendor
// seam, so it keeps `extern "C"` (the stack calls it by its unmangled C symbol). Buckets
// fragments as CAT_TUSB at DEBUG but does NOT prepend a per-call prefix -- TinyUSB emits
// partial line fragments, so a prefix per call would inject mid-line.
void orb_log_emit(int level, int cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void orb_log_hexdump(int level, int cat, const void *data, uint32_t len);
extern "C" int orb_log_tusb_printf(const char *fmt, ...);

// --- Macro API ---------------------------------------------------------------
#define ORB_LOG_(lvl, cat, ...) orb_log_emit((lvl), static_cast<int>(cat), __VA_ARGS__)

#if ORB_LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERR(cat, ...) ORB_LOG_(LOG_LEVEL_ERROR, (cat), __VA_ARGS__)
#else
#define LOG_ERR(cat, ...) ((void)0)
#endif

#if ORB_LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(cat, ...) ORB_LOG_(LOG_LEVEL_WARN, (cat), __VA_ARGS__)
#else
#define LOG_WARN(cat, ...) ((void)0)
#endif

#if ORB_LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(cat, ...) ORB_LOG_(LOG_LEVEL_INFO, (cat), __VA_ARGS__)
#else
#define LOG_INFO(cat, ...) ((void)0)
#endif

#if ORB_LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DBG(cat, ...) ORB_LOG_(LOG_LEVEL_DEBUG, (cat), __VA_ARGS__)
#else
#define LOG_DBG(cat, ...) ((void)0)
#endif

#if ORB_LOG_LEVEL >= LOG_LEVEL_TRACE
#define LOG_TRC(cat, ...) ORB_LOG_(LOG_LEVEL_TRACE, (cat), __VA_ARGS__)
#else
#define LOG_TRC(cat, ...) ((void)0)
#endif

// Deferred hex dump (replaces OPENRB_DEBUG_BUF/print_buf). `level` is an argument;
// when it is a compile-time constant the guard folds away, so a dump above the
// floor costs nothing.
#define LOG_HEXDUMP(cat, level, ptr, len)                       \
    do {                                                        \
        if ((level) <= ORB_LOG_LEVEL)                           \
            orb_log_hexdump((level), static_cast<int>(cat), (ptr), (len)); \
    } while (0)

