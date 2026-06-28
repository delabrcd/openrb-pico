#include "orb_log.h"

#include <stdarg.h>
#include <stdio.h>

#include "dlog.h"
#include "hardware/structs/sio.h"
#include "hardware/timer.h"
#include "usb_log.h"

// Maximum rendered line length (prefix + message + color + CRLF). dlog's producer
// truncates at 160 bytes anyway, so staying at/under that keeps behavior identical
// whether or not the line passes through unchanged.
#define ORB_LOG_LINE_MAX 192u

// --- Runtime level table -----------------------------------------------------
// Plain uint8_t, written from core0 config, read from both cores. A torn/stale
// read only admits or drops a single line -- benign, so no lock (matches dlog's
// lock-free model). Initialized to the compile-time floor so logging is fully on
// up to ORB_LOG_LEVEL until reconfigured.
static volatile uint8_t g_log_level = ORB_LOG_LEVEL;
static volatile uint8_t g_cat_level[CAT_COUNT] = {
    [0 ... CAT_COUNT - 1] = ORB_LOG_LEVEL,
};

static const char *const orb_level_name[] = {
    "?????",  // 0 / NONE -- never rendered
    "ERR",    // 1 ERROR
    "WARN",   // 2 WARN
    "INFO",   // 3 INFO
    "DEBUG",  // 4 DEBUG
    "TRACE",  // 5 TRACE
};

static const char *const orb_cat_name[CAT_COUNT] = {
    "SYS", "HOST", "DEV", "DRUM", "MIDI", "RECOV", "USBLOG", "TUSB",
};

#if ORB_LOG_COLOR
// Per-level SGR: red ERR, yellow WARN, none INFO, dim DEBUG/TRACE.
static const char *const orb_level_color[] = {
    "",          // 0
    "\x1b[31m",  // ERROR  -> red
    "\x1b[33m",  // WARN   -> yellow
    "",          // INFO   -> default (no color)
    "\x1b[2m",   // DEBUG  -> dim
    "\x1b[2m",   // TRACE  -> dim
};
#endif

// --- Public config -----------------------------------------------------------
void orb_log_set_level(int level) {
    if (level < 0) level = 0;
    if (level > LOG_LEVEL_TRACE) level = LOG_LEVEL_TRACE;
    g_log_level = (uint8_t)level;
    for (int c = 0; c < CAT_COUNT; c++) g_cat_level[c] = (uint8_t)level;
}

void orb_log_set_cat_level(orb_cat_t cat, int level) {
    if ((unsigned)cat >= CAT_COUNT) return;
    if (level < 0) level = 0;
    if (level > LOG_LEVEL_TRACE) level = LOG_LEVEL_TRACE;
    g_cat_level[cat] = (uint8_t)level;
}

int orb_log_get_level(void) { return (int)g_log_level; }

void orb_log_init(void) {
    dlog_init();
    dlog_set_sink(usb_log_write);
}

// --- Core emit ---------------------------------------------------------------
// Runtime gate: one array load + compare. cat clamped so a bad enum can't index
// out of bounds.
static inline int orb_log_admit(int level, int cat) {
    if ((unsigned)cat >= CAT_COUNT) return level <= (int)g_log_level;
    return level <= (int)g_cat_level[cat];
}

void orb_log_emit(int level, int cat, const char *fmt, ...) {
    // Snapshot the producer-side clock + core id at the TOP, before any work, so
    // deferred drain time never pollutes the timestamp (gotcha #2: timerawl is the
    // lock-free clock on both cores; cpuid is dlog's same source).
    const uint32_t raw = timer_hw->timerawl;
    const uint32_t core = sio_hw->cpuid & 1u;

    if (!orb_log_admit(level, cat)) return;
    if ((unsigned)cat >= CAT_COUNT) cat = CAT_SYS;
    if (level < LOG_LEVEL_ERROR) level = LOG_LEVEL_ERROR;
    if (level > LOG_LEVEL_TRACE) level = LOG_LEVEL_TRACE;

    const uint32_t sec = raw / 1000000u;
    const uint32_t frac = raw % 1000000u;

#if ORB_LOG_COLOR
    const char *col = orb_level_color[level];
    const char *rst = col[0] ? "\x1b[0m" : "";
#else
    const char *col = "";
    const char *rst = "";
#endif

    char line[ORB_LOG_LINE_MAX];
    int p = snprintf(line, sizeof line, "%s[%4lu.%06lu][%-5s][C%lu][%-6s] ", col,
                     (unsigned long)sec, (unsigned long)frac, orb_level_name[level],
                     (unsigned long)core, orb_cat_name[cat]);
    if (p < 0) return;
    if (p >= (int)sizeof line) p = (int)sizeof line - 1;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + p, sizeof line - (size_t)p, fmt, ap);
    va_end(ap);
    if (m > 0) {
        p += m;
        if (p >= (int)sizeof line) p = (int)sizeof line - 1;
    }

    // Trailing reset + CRLF (call sites no longer hand-write \r\n).
    snprintf(line + p, sizeof line - (size_t)p, "%s\r\n", rst);

    // ONE producer call == one ring head store (batch publish, drop-on-full
    // preserved). "%s" keeps any '%' in the rendered text literal.
    dlog_printf("%s", line);
}

// --- Hex dump ----------------------------------------------------------------
// 16 bytes per emitted line; each line carries the normal prefix and is leveled +
// categorized. Deferred (goes through the ring), never blocks the caller.
void orb_log_hexdump(int level, int cat, const void *data, uint32_t len) {
    if (!orb_log_admit(level, cat)) return;
    const uint8_t *buf = (const uint8_t *)data;
    char hex[3 * 16 + 1];
    for (uint32_t i = 0; i < len; i += 16u) {
        uint32_t n = (len - i < 16u) ? (len - i) : 16u;
        int q = 0;
        for (uint32_t j = 0; j < n; j++)
            q += snprintf(hex + q, sizeof hex - (size_t)q, "%02X ", buf[i + j]);
        orb_log_emit(level, cat, "%s", hex);
    }
}

// --- TinyUSB passthrough -----------------------------------------------------
int orb_log_tusb_printf(const char *fmt, ...) {
    if (LOG_LEVEL_DEBUG > (int)g_cat_level[CAT_TUSB]) return 0;
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    // No prefix: TinyUSB output arrives as partial line fragments. Pass straight
    // through to the ring, bucketed as CAT_TUSB.
    dlog_printf("%s", tmp);
    return n;
}
