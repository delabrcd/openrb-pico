/*
 * platform::pico — time implementation (boundary: may include the SDK).
 *
 * Uses timer_hw->timerawl directly (NOT board_millis()/time_us_64(), which take a
 * spinlock and are therefore banned on core1) for a lock-free microsecond clock, and
 * busy_wait_* for a pure busy-delay safe on the PIO-USB core and pre-scheduler. Satisfies
 * orb::hal::MonotonicClock + orb::hal::BusyDelay (asserted in hal/platform.hpp).
 */
#pragma once

#include <chrono>
#include <cstdint>

#include "hardware/timer.h"
#include "pico/time.h"

namespace orb::platform::pico {

class Clock {
   public:
    // 32-bit microsecond rep so `now() - earlier` is unsigned-modular across the ~71.6 min
    // wrap (see hal/clock.hpp). NOT microseconds(int64) -- that would break wraparound.
    using rep = std::uint32_t;
    using period = std::micro;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<Clock, duration>;
    static constexpr bool is_steady = true;

    // Lock-free, both-core-safe. 1 MHz raw timer low word.
    time_point now() const { return time_point{duration{timer_hw->timerawl}}; }

    // Pure busy-wait -- no scheduler/SDK sleep, safe on core1 and before the scheduler.
    template <typename Rep, typename Period>
    void delay(std::chrono::duration<Rep, Period> d) const {
        busy_wait_us(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
    }
};

}  // namespace orb::platform::pico

