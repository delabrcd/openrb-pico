/*
 * osal (OS Abstraction Layer): the ONE place std::chrono durations become FreeRTOS ticks.
 * Everything outside osal/ speaks std::chrono; TickType_t / pdMS_TO_TICKS / portMAX_DELAY
 * never appear beyond this layer (the portability boundary -- see task.hpp/timer.hpp/mutex.hpp).
 */
#pragma once

#include <chrono>

#include "FreeRTOS.h"
#include "task.h"  // TickType_t, portMAX_DELAY (via projdefs), configTICK_RATE_HZ

namespace orb::osal {

// Round a std::chrono duration UP to whole FreeRTOS ticks: a positive sub-tick timeout still
// waits at least one tick (never truncates a nonzero wait to zero). Derived from
// configTICK_RATE_HZ so it stays correct if the tick rate changes (it is 1 kHz today, so
// 1 tick == 1 ms and this is behavior-preserving vs the old pdMS_TO_TICKS(ms) calls).
template <typename Rep, typename Period>
constexpr TickType_t to_ticks(std::chrono::duration<Rep, Period> d) {
    if (d <= std::chrono::duration<Rep, Period>::zero()) return 0;
    using ticks_t = std::chrono::duration<TickType_t, std::ratio<1, configTICK_RATE_HZ>>;
    return std::chrono::ceil<ticks_t>(d).count();
}

}  // namespace orb::osal
