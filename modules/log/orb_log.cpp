#include "orb_log.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "dlog.h"
#include "hardware/structs/sio.h"
#include "hardware/timer.h"
#include "usb_log.h"

// Logging front end (modern C++ port of the former orb_log.c). Sits on top of the
// SPSC ring exposed by dlog.h: snapshots the producer-side clock + core id, renders
// one prefixed line into a stack buffer, and publishes it with a SINGLE dlog_printf
// call. Core1-safe: only timer_hw->timerawl + sio_hw->cpuid are touched, no locks,
// no sleep (docs/FREERTOS-PORT.md gotcha #2).
//
// dlog_printf / dlog_init / dlog_set_sink (dlog.h) and usb_log_write (usb_log.h) are
// producer seams into the dlog SPSC layer -- called directly; that is the boundary to
// dlog. The LOG_* macros expand to orb_log_emit, a plain C++ free function (below);
// orb_log_tusb_printf is the TinyUSB vendor hook and keeps C linkage. All three are
// DEFINED inside namespace orb::log so they reach the internal state -- orb_log_tusb_printf
// additionally carries extern "C" to keep its unmangled global name for the stack.

namespace orb::log {
namespace {

// Maximum rendered line length (prefix + message + color + CRLF). dlog's producer
// truncates at 160 bytes anyway, so staying at/under that keeps behavior identical
// whether or not the line passes through unchanged.
constexpr std::size_t kLineMax = 192u;

// --- Runtime level table -----------------------------------------------------
// atomic<uint8_t> with relaxed load/store: on M0+ that lowers to a plain ldrb/strb
// (no RMW), matching the old volatile uint8_t cost. Written from core0 config, read
// from both cores; a torn/stale read only admits or drops a single line -- benign,
// so no lock (matches dlog's lock-free model). Initialized to the compile-time floor
// so logging is fully on up to ORB_LOG_LEVEL until reconfigured. constinit -> these
// land in .data/.bss with no global constructor.
constinit std::atomic<uint8_t> g_log_level{ORB_LOG_LEVEL};

static_assert(CAT_COUNT == 8, "update g_cat_level initializer to match CAT_COUNT");
constinit std::array<std::atomic<uint8_t>, CAT_COUNT> g_cat_level{
    {{ORB_LOG_LEVEL}, {ORB_LOG_LEVEL}, {ORB_LOG_LEVEL}, {ORB_LOG_LEVEL},
     {ORB_LOG_LEVEL}, {ORB_LOG_LEVEL}, {ORB_LOG_LEVEL}, {ORB_LOG_LEVEL}}};

inline uint8_t load_cat(int cat) {
    return g_cat_level[static_cast<std::size_t>(cat)].load(std::memory_order_relaxed);
}

constexpr std::array<const char*, 6> kLevelName{
    "?????",  // 0 / NONE -- never rendered
    "ERR",    // 1 ERROR
    "WARN",   // 2 WARN
    "INFO",   // 3 INFO
    "DEBUG",  // 4 DEBUG
    "TRACE",  // 5 TRACE
};

constexpr std::array<const char*, CAT_COUNT> kCatName{
    "SYS", "HOST", "DEV", "DRUM", "MIDI", "RECOV", "USBLOG", "TUSB",
};

#if ORB_LOG_COLOR
// Per-level SGR: red ERR, yellow WARN, none INFO, dim DEBUG/TRACE.
constexpr std::array<const char*, 6> kLevelColor{
    "",          // 0
    "\x1b[31m",  // ERROR  -> red
    "\x1b[33m",  // WARN   -> yellow
    "",          // INFO   -> default (no color)
    "\x1b[2m",   // DEBUG  -> dim
    "\x1b[2m",   // TRACE  -> dim
};
#endif

// Runtime gate: one array load + compare. cat clamped so a bad enum can't index out
// of bounds.
inline bool admit(int level, int cat) {
    if (static_cast<unsigned>(cat) >= CAT_COUNT)
        return level <= static_cast<int>(g_log_level.load(std::memory_order_relaxed));
    return level <= static_cast<int>(load_cat(cat));
}

// Core renderer. Snapshot the producer-side clock + core id at the TOP, before any
// work, so deferred drain time never pollutes the timestamp (gotcha #2: timerawl is
// the lock-free clock on both cores; cpuid is dlog's same source).
void emit_v(int level, int cat, const char* fmt, va_list ap) {
    const uint32_t raw = timer_hw->timerawl;
    const uint32_t core = sio_hw->cpuid & 1u;

    if (!admit(level, cat)) return;
    if (static_cast<unsigned>(cat) >= CAT_COUNT) cat = CAT_SYS;
    if (level < LOG_LEVEL_ERROR) level = LOG_LEVEL_ERROR;
    if (level > LOG_LEVEL_TRACE) level = LOG_LEVEL_TRACE;

    const uint32_t sec = raw / 1000000u;
    const uint32_t frac = raw % 1000000u;

#if ORB_LOG_COLOR
    const char* col = kLevelColor[static_cast<std::size_t>(level)];
    const char* rst = col[0] ? "\x1b[0m" : "";
#else
    const char* col = "";
    const char* rst = "";
#endif

    std::array<char, kLineMax> line;
    int p = std::snprintf(line.data(), line.size(), "%s[%4lu.%06lu][%-5s][C%lu][%-6s] ",
                          col, static_cast<unsigned long>(sec), static_cast<unsigned long>(frac),
                          kLevelName[static_cast<std::size_t>(level)],
                          static_cast<unsigned long>(core),
                          kCatName[static_cast<std::size_t>(cat)]);
    if (p < 0) return;
    if (p >= static_cast<int>(line.size())) p = static_cast<int>(line.size()) - 1;

    int m = std::vsnprintf(line.data() + p, line.size() - static_cast<std::size_t>(p), fmt, ap);
    if (m > 0) {
        p += m;
        if (p >= static_cast<int>(line.size())) p = static_cast<int>(line.size()) - 1;
    }

    // Trailing reset + CRLF (call sites no longer hand-write \r\n).
    std::snprintf(line.data() + p, line.size() - static_cast<std::size_t>(p), "%s\r\n", rst);

    // ONE producer call == one ring head store (batch publish, drop-on-full
    // preserved). "%s" keeps any '%' in the rendered text literal.
    dlog_printf("%s", line.data());
}

// Variadic forwarder so internal callers (hexdump) share the renderer.
[[gnu::format(printf, 3, 4)]] void emit(int level, int cat, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit_v(level, cat, fmt, ap);
    va_end(ap);
}

// 16 bytes per emitted line; each line carries the normal prefix and is leveled +
// categorized. Deferred (goes through the ring), never blocks the caller.
void hexdump(int level, int cat, std::span<const std::byte> bytes) {
    if (!admit(level, cat)) return;
    std::array<char, 3 * 16 + 1> hex;
    for (std::size_t i = 0; i < bytes.size(); i += 16u) {
        const std::size_t n = (bytes.size() - i < 16u) ? (bytes.size() - i) : 16u;
        int q = 0;
        for (std::size_t j = 0; j < n; j++)
            q += std::snprintf(hex.data() + q, hex.size() - static_cast<std::size_t>(q), "%02X ",
                               static_cast<unsigned>(bytes[i + j]));
        emit(level, cat, "%s", hex.data());
    }
}

}  // namespace

// --- Public config -----------------------------------------------------------
void init() {
    dlog_init();
    dlog_set_sink(usb_log_write);
}

void set_level(int level) {
    if (level < 0) level = 0;
    if (level > LOG_LEVEL_TRACE) level = LOG_LEVEL_TRACE;
    g_log_level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    for (auto& cat : g_cat_level) cat.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

void set_cat_level(orb_cat_t cat, int level) {
    if (static_cast<unsigned>(cat) >= CAT_COUNT) return;
    if (level < 0) level = 0;
    if (level > LOG_LEVEL_TRACE) level = LOG_LEVEL_TRACE;
    g_cat_level[cat].store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

int get_level() { return static_cast<int>(g_log_level.load(std::memory_order_relaxed)); }

// --- Vendor seam -------------------------------------------------------------
// orb_log_tusb_printf is the permanent TinyUSB CFG_TUSB_DEBUG_PRINTF vendor hook. It
// stays extern "C" (the stack calls it by its unmangled global symbol) and is defined
// inside namespace orb::log -- with C language linkage the namespace is irrelevant to the
// symbol, so this matches the global header declaration while reaching the internal state.
extern "C" int orb_log_tusb_printf(const char* fmt, ...) {
    if (LOG_LEVEL_DEBUG > static_cast<int>(load_cat(CAT_TUSB))) return 0;
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    // No prefix: TinyUSB output arrives as partial line fragments. Pass straight
    // through to the ring, bucketed as CAT_TUSB.
    dlog_printf("%s", tmp);
    return n;
}

}  // namespace orb::log

// --- Producers ---------------------------------------------------------------
// orb_log_emit / orb_log_hexdump are the plain C++ free functions the LOG_* / OPENRB_DEBUG
// macros expand into. The macros call them UNQUALIFIED from every namespace, so the header
// declares them at global scope -- hence they are defined here at global scope too (a
// matching definition), reaching the renderer through orb::log::.
void orb_log_emit(int level, int cat, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    orb::log::emit_v(level, cat, fmt, ap);
    va_end(ap);
}

void orb_log_hexdump(int level, int cat, const void* data, uint32_t len) {
    orb::log::hexdump(level, cat,
                      std::span<const std::byte>{static_cast<const std::byte*>(data), len});
}
