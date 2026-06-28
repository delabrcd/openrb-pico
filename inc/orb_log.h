#ifndef ORB_LOG_H_
#define ORB_LOG_H_

#include <stdint.h>

// Unified deferred logging front end. Sits ON TOP OF the per-core SPSC ring in
// src/dlog.c (unchanged) and adds: 5 log levels, category tags, a producer-side
// timestamp, a column-aligned line format, and optional inline ANSI color.
//
// Format on produce: each call snapshots timer_hw->timerawl + cpuid on the
// producing core, renders prefix+message into one stack buffer, and pushes it to
// the calling core's ring with a SINGLE dlog producer call (one head store --
// preserves dlog's batch-publish + drop-on-full discipline). Safe to call from
// the timing-critical core1 PIO-USB task: only timerawl + cpuid are touched, no
// locks, no sleep/board_millis (see docs/FREERTOS-PORT.md gotcha #2).

#ifdef __cplusplus
extern "C" {
#endif

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
typedef enum {
    CAT_SYS = 0,
    CAT_HOST,
    CAT_DEV,
    CAT_DRUM,
    CAT_MIDI,
    CAT_RECOV,
    CAT_USBLOG,
    CAT_TUSB,
    CAT_COUNT
} orb_cat_t;

// --- Runtime control ---------------------------------------------------------
// Above the compile-time floor, a plain uint8_t table gates at run time. Written
// from core0 (config), read from both cores; a stale read merely admits/drops one
// line -- benign, so no lock (consistent with dlog's lock-free philosophy).
void orb_log_init(void);                         // wraps dlog_init() + usb_log sink
void orb_log_set_level(int level);               // global runtime floor (all cats)
void orb_log_set_cat_level(orb_cat_t cat, int level);  // per-category override
int orb_log_get_level(void);                     // current global runtime floor

// --- Producers (called by the macros below; also directly by the TinyUSB hook) -
void orb_log_emit(int level, int cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void orb_log_hexdump(int level, int cat, const void *data, uint32_t len);

// TinyUSB CFG_TUSB_DEBUG_PRINTF target. Buckets fragments as CAT_TUSB at DEBUG but
// does NOT prepend a per-call prefix -- TinyUSB emits partial line fragments, so a
// prefix per call would inject mid-line. v1: gated passthrough.
int orb_log_tusb_printf(const char *fmt, ...);

// --- Macro API ---------------------------------------------------------------
#define ORB_LOG_(lvl, cat, ...) orb_log_emit((lvl), (cat), __VA_ARGS__)

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
            orb_log_hexdump((level), (cat), (ptr), (len));      \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif  // ORB_LOG_H_
