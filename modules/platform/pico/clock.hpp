/*
 * platform::pico — time implementation (boundary: may include the SDK).
 *
 * Uses timer_hw->timerawl directly (NOT board_millis()/time_us_64(), which take a
 * spinlock and are therefore banned on core1) for a lock-free microsecond clock, and
 * busy_wait_* for a pure busy-delay safe on the PIO-USB core and pre-scheduler. Satisfies
 * orb::hal::MonotonicClock + orb::hal::BusyDelay (asserted in hal/platform.hpp).
 */
#ifndef ORB_PLATFORM_PICO_CLOCK_HPP
#define ORB_PLATFORM_PICO_CLOCK_HPP

#include <cstdint>

#include "hardware/timer.h"
#include "pico/time.h"

namespace orb::platform::pico {

class Clock {
   public:
    // Lock-free, both-core-safe. 1 MHz raw timer low word.
    uint32_t now_us() const { return timer_hw->timerawl; }

    // Pure busy-wait -- no scheduler/SDK sleep, safe on core1 and before the scheduler.
    void delay_us(uint32_t us) const { busy_wait_us(us); }
    void delay_ms(uint32_t ms) const { busy_wait_ms(ms); }
};

}  // namespace orb::platform::pico

#endif  // ORB_PLATFORM_PICO_CLOCK_HPP
