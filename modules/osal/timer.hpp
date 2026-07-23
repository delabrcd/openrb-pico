/*
 * osal (OS Abstraction Layer): this header is part of the firmware's ONLY dependency on
 * FreeRTOS. inc/osal/ is the portability boundary — only this osal layer (and the hal layer) may
 * include FreeRTOS/pico-sdk; the rest of the codebase uses orb::osal::* and never touches
 * the RTOS directly. Porting to another RTOS means rewriting inc/osal/, nothing else.
 *
 * orb::osal::Timer — a tiny RAII wrapper that owns a FreeRTOS software timer's control
 * block (StaticTimer_t) and creates it statically (no heap), the sibling of
 * orb::osal::Task<N> (inc/osal/task.hpp) and orb::osal::Queue<T,Depth> (inc/osal/queue.hpp).
 * It removes the "declare a StaticTimer_t + a TimerHandle_t + call xTimerCreateStatic +
 * keep the handle around for xTimerChangePeriod" boilerplate that midi.cpp repeated
 * for its drum-disconnect timer (see docs/features/cpp-overhaul.md D1).
 *
 * Same discipline as Task/Queue: the storage member is the FreeRTOS
 * StaticTimer_t buffer, construction does nothing kernel-touching (constant-initialized
 * -> lands in BSS, no global ctor / static-init-order concern), and create() does the actual
 * xTimerCreateStatic once the kernel is up enough. Give each instance static storage
 * duration (it must outlive the timer).
 *
 *   using namespace std::chrono_literals;
 *   static orb::osal::Timer s_disconnect_timer;
 *   s_disconnect_timer.create<SerialMidi, &SerialMidi::on_disconnect_timeout>(
 *       "midi_disc", 90s, false, midi);          // false == one-shot
 *   s_disconnect_timer.change_period(1s);         // re-arm, no block
 *
 * pvTimerGetTimerID(handle) / id() retrieve the stashed void* id if the callback needs
 * to find per-timer state through the handle rather than a file-static -- note the
 * member-pointer create() overload below consumes that slot to stash &obj, so id() is
 * unavailable on a timer created that way.
 */
#pragma once

#include <chrono>

#include "FreeRTOS.h"
#include "timers.h"

#include "chrono.hpp"
#include "core/section.hpp"  // ORB_FAST (RAM placement for the callback trampoline)

namespace orb::osal {

class Timer {
   public:
    // Create the timer. Call once, before/while the scheduler runs. Never null for a static
    // create. `auto_reload` true == periodic, false == one-shot. `id` is stashed as the timer's
    // pvTimerID (retrievable via id()). `period` is any std::chrono duration.
    template <typename Rep, typename Period>
    TimerHandle_t create(const char *name, std::chrono::duration<Rep, Period> period,
                         bool auto_reload, TimerCallbackFunction_t callback, void *id = nullptr) {
        handle_ = xTimerCreateStatic(name, to_ticks(period), auto_reload ? pdTRUE : pdFALSE, id,
                                     callback, &ctrl_);
        return handle_;
    }

    // Typed member-function callback: bind an object + a compile-time member pointer so the
    // timer body is `obj.Method()` with NO FreeRTOS type or void* at the call site. FreeRTOS
    // still receives a plain TimerCallbackFunction_t (its mandated C ABI); the timer's
    // pvTimerID slot carries &obj and is dereferenced only in trampoline() below -- the one
    // place a raw pointer is touched. Because Method is a compile-time constant,
    // (obj.*Method)() lowers to a direct call, identical codegen to a free-function callback.
    // NOTE: this overload consumes the pvTimerID slot to stash &obj, so id() is unavailable on
    // a timer created this way (mutually exclusive with the void* id overload above).
    template <typename T, void (T::*Method)(), typename Rep, typename Period>
    TimerHandle_t create(const char *name, std::chrono::duration<Rep, Period> period,
                         bool auto_reload, T &obj) {
        handle_ = xTimerCreateStatic(name, to_ticks(period), auto_reload ? pdTRUE : pdFALSE, &obj,
                                     &trampoline<T, Method>, &ctrl_);
        return handle_;
    }

    // Set a new period and (re)start the timer (xTimerChangePeriod also starts a dormant
    // timer -> "cancel + re-arm"). `block` is the max time to wait on the timer command queue
    // (default = don't block, for hot paths).
    template <typename Rep, typename Period>
    bool change_period(std::chrono::duration<Rep, Period> new_period,
                       std::chrono::milliseconds block = std::chrono::milliseconds::zero()) {
        return xTimerChangePeriod(handle_, to_ticks(new_period), to_ticks(block)) == pdPASS;
    }

    TimerHandle_t handle() const { return handle_; }
    void *id() const { return pvTimerGetTimerID(handle_); }

   private:
    // The sole place the FreeRTOS timer-callback ABI + void* id are handled: recover the typed
    // object from the timer id and dispatch to its member. Never propagates outward. RAM-placed
    // (ORB_FAST) so a member-fn timer callback is flash-stall-free and the whole leaf chain is
    // RAM-resident, matching the free-function ORB_FAST callbacks it replaced.
    template <typename T, void (T::*Method)()>
    static void ORB_FAST(trampoline)(TimerHandle_t handle) {
        (static_cast<T *>(pvTimerGetTimerID(handle))->*Method)();
    }

    StaticTimer_t ctrl_;                 // timer control block
    TimerHandle_t handle_ = nullptr;
};

}  // namespace orb::osal

