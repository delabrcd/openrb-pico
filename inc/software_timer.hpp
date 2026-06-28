/*
 * SoftwareTimer — a tiny RAII wrapper that owns a FreeRTOS software timer's control
 * block (StaticTimer_t) and creates it statically (no heap), the sibling of
 * StaticTask<N> (inc/static_task.hpp) and StaticQueue<T,Depth> (inc/static_queue.hpp).
 * It removes the "declare a StaticTimer_t + a TimerHandle_t + call xTimerCreateStatic +
 * keep the handle around for xTimerChangePeriod" boilerplate that src/midi.c repeated
 * for its drum-disconnect timer (see docs/features/cpp-overhaul.md D1).
 *
 * Same discipline as StaticTask/StaticQueue: the storage member is the FreeRTOS
 * StaticTimer_t buffer, construction does nothing kernel-touching (trivial type, lands
 * in BSS, no global-ctor / static-init-order concern), and create() does the actual
 * xTimerCreateStatic once the kernel is up enough. Give each instance static storage
 * duration (it must outlive the timer).
 *
 *   static SoftwareTimer s_disconnect_timer;
 *   s_disconnect_timer.create("midi_disc", pdMS_TO_TICKS(FIFTEEN_MINUTES),
 *                             false, on_disconnect_timeout_cb);  // false == one-shot
 *   s_disconnect_timer.change_period(pdMS_TO_TICKS(ONE_SECOND), 0);  // re-arm, no block
 *
 * The timer callback stays a free `extern "C"` function (it is a C-linkage symbol the
 * kernel's timer-service task calls — keep any __not_in_flash_func placement on it).
 * pvTimerGetTimerID(handle) / id() retrieve the stashed void* id if the callback needs
 * to find per-timer state through the handle rather than a file-static.
 */
#ifndef OPENRB_SOFTWARE_TIMER_HPP
#define OPENRB_SOFTWARE_TIMER_HPP

#include "FreeRTOS.h"
#include "timers.h"

namespace orb {

class SoftwareTimer {
   public:
    // Create the timer. Call once, before/while the scheduler runs. Never null for a
    // static create. `auto_reload` true == periodic, false == one-shot (matches the
    // uxAutoReload argument of xTimerCreateStatic). `id` is stashed as the timer's
    // pvTimerID and retrievable via id() / pvTimerGetTimerID() from the callback.
    TimerHandle_t create(const char *name, TickType_t period_ticks, bool auto_reload,
                         TimerCallbackFunction_t callback, void *id = nullptr) {
        handle_ = xTimerCreateStatic(name, period_ticks,
                                     auto_reload ? pdTRUE : pdFALSE, id, callback, &ctrl_);
        return handle_;
    }

    // Set a new period and (re)start the timer. xTimerChangePeriod also starts/restarts
    // a dormant timer, giving "cancel + re-arm" semantics. Returns true if the command
    // was queued to the timer-service task. `ticks_to_wait` is the block time on the
    // timer command queue (0 == don't block, for hot paths).
    bool change_period(TickType_t new_period_ticks, TickType_t ticks_to_wait) {
        return xTimerChangePeriod(handle_, new_period_ticks, ticks_to_wait) == pdPASS;
    }

    TimerHandle_t handle() const { return handle_; }
    void *id() const { return pvTimerGetTimerID(handle_); }

   private:
    StaticTimer_t ctrl_;                 // timer control block
    TimerHandle_t handle_ = nullptr;
};

}  // namespace orb

#endif  // OPENRB_SOFTWARE_TIMER_HPP
