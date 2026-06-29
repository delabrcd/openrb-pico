/*
 * osal (OS Abstraction Layer): this header is part of the firmware's ONLY dependency on
 * FreeRTOS. inc/osal/ is the portability boundary — only this osal layer (and the hal layer) may
 * include FreeRTOS/pico-sdk; the rest of the codebase uses orb::osal::* and never touches
 * the RTOS directly. Porting to another RTOS means rewriting inc/osal/, nothing else.
 *
 * orb::osal::Mutex / orb::osal::ScopedLock — a tiny RAII wrapper that owns a FreeRTOS
 * mutex's control block (StaticSemaphore_t) and creates it statically (no heap), plus a
 * scope-guard that take()s on construction and give()s on destruction. Sibling of
 * orb::osal::Task<N> (inc/osal/task.hpp), orb::osal::Queue<T,Depth> (inc/osal/queue.hpp)
 * and orb::osal::Timer (inc/osal/timer.hpp); see docs/features/cpp-overhaul.md D1.
 *
 * Same discipline as the other wrappers: the storage member is the FreeRTOS
 * StaticSemaphore_t buffer, construction does nothing kernel-touching (constant-initialized
 * -> lands in BSS, no global ctor / static-init-order concern), and create() does the
 * actual xSemaphoreCreateMutexStatic once the kernel is up enough. Give each instance
 * static storage duration.
 *
 *   static orb::osal::Mutex s_lock;
 *   s_lock.create();
 *   ...
 *   {
 *       ScopedLock guard(s_lock);   // take on construct, give on scope exit
 *       ... critical section ...
 *   }
 *
 * NB (per spec): this is a foundational wrapper. It is intentionally NOT wired into the
 * device TX fifo this phase — whether that multi-writer path stays a tu_fifo+OSAL mutex,
 * a guarded Queue, or is restructured is an open design question (cpp-overhaul.md
 * D3, open Q1). This header just provides the primitive, correctly.
 *
 * No heap, no exceptions: take()/give() report success by bool; failures are not thrown.
 */
#pragma once

#include "FreeRTOS.h"
#include "semphr.h"

namespace orb::osal {

class Mutex {
   public:
    // Create the mutex. Call once, before/while the scheduler runs. Never null for a
    // static create.
    SemaphoreHandle_t create() {
        handle_ = xSemaphoreCreateMutexStatic(&ctrl_);
        return handle_;
    }

    // Take (lock). `ticks_to_wait` is the max block time (portMAX_DELAY to wait
    // forever, 0 to poll). Returns true if the mutex was obtained.
    bool take(TickType_t ticks_to_wait = portMAX_DELAY) {
        return xSemaphoreTake(handle_, ticks_to_wait) == pdTRUE;
    }

    // Give (unlock). Returns true on success (false if not held by this task, etc.).
    bool give() { return xSemaphoreGive(handle_) == pdTRUE; }

    SemaphoreHandle_t handle() const { return handle_; }

   private:
    StaticSemaphore_t ctrl_;                 // mutex control block
    SemaphoreHandle_t handle_ = nullptr;
};

// RAII guard: takes the mutex on construction (blocking forever by default) and gives it
// back on destruction. Non-copyable, non-movable. held() reports whether the take
// succeeded when a bounded timeout was requested.
class ScopedLock {
   public:
    explicit ScopedLock(Mutex &m, TickType_t ticks_to_wait = portMAX_DELAY)
        : mutex_(m), held_(m.take(ticks_to_wait)) {}
    ~ScopedLock() {
        if (held_) mutex_.give();
    }

    ScopedLock(const ScopedLock &) = delete;
    ScopedLock &operator=(const ScopedLock &) = delete;

    bool held() const { return held_; }

   private:
    Mutex &mutex_;
    bool held_;
};

}  // namespace orb::osal

