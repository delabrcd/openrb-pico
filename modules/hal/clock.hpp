/*
 * orb::hal — time interface (portable; NO SDK). The contract for a monotonic clock and a
 * busy-wait delay. Both are core-critical: the core1 PIO-USB path forbids any SDK call
 * that takes a spinlock or yields, so the clock must be lock-free and the delay must be a
 * pure busy-wait (never a scheduler sleep). See docs/architecture/modern-cpp.md and the
 * core1 gotchas in docs/FREERTOS-PORT.md.
 */
#pragma once

#include <concepts>
#include <cstdint>

namespace orb::hal {

// A free-running monotonic microsecond counter, readable LOCK-FREE from either core. 32
// bits at 1 MHz wraps ~71.6 min; callers use unsigned subtraction to measure elapsed time
// across a wrap. (Distinct from any millisecond/64-bit time that takes a lock.)
template <typename T>
concept MonotonicClock = requires(const T clk) {
    { clk.now_us() } -> std::convertible_to<uint32_t>;
};

// A pure busy-wait delay, safe to call on the timing-critical core (no sleep/yield/lock).
// For pre-scheduler and core1 use where vTaskDelay/sleep_ms would deadlock or stall SOF.
template <typename T>
concept BusyDelay = requires(const T d, uint32_t n) {
    { d.delay_us(n) };
    { d.delay_ms(n) };
};

}  // namespace orb::hal

