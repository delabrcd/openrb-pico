/*
 * orb::hal — time interface (portable; NO SDK). The contract for a monotonic clock and a
 * busy-wait delay. Both are core-critical: the core1 PIO-USB path forbids any SDK call
 * that takes a spinlock or yields, so the clock must be lock-free and the delay must be a
 * pure busy-wait (never a scheduler sleep). See docs/architecture/modern-cpp.md and the
 * core1 gotchas in docs/FREERTOS-PORT.md.
 */
#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>

namespace orb::hal {

// A monotonic steady clock exposing a chrono-style now() whose time_point is backed by a
// 32-bit microsecond rep, so `now() - earlier` is unsigned-modular and measures elapsed time
// correctly across the ~71.6 min wrap. Lock-free, readable from either core.
template <typename T>
concept MonotonicClock = requires(const T clk) {
    typename T::time_point;
    { clk.now() } -> std::same_as<typename T::time_point>;
};

// A pure busy-wait delay, safe on the timing-critical core (no sleep/yield/lock). Accepts any
// std::chrono duration. For pre-scheduler and core1 use where vTaskDelay/sleep would deadlock.
template <typename T>
concept BusyDelay = requires(const T d, std::chrono::microseconds us) {
    { d.delay(us) };
};

}  // namespace orb::hal

