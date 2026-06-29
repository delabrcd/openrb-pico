/*
 * osal (OS Abstraction Layer): this header is part of the firmware's ONLY dependency on
 * FreeRTOS. inc/osal/ is the portability boundary — only this osal layer (and the hal layer) may
 * include FreeRTOS/pico-sdk; the rest of the codebase uses orb::osal::* and never touches
 * the RTOS directly. Porting to another RTOS means rewriting inc/osal/, nothing else.
 *
 * orb::osal::Task<StackWords> — a tiny C++ wrapper that owns a FreeRTOS task's stack and
 * TCB storage and creates it with a fixed core affinity. It removes the repetitive
 * "declare a StackType_t[] + a StaticTask_t + call xTaskCreateStaticAffinitySet"
 * boilerplate that otherwise multiplies now that the superloop is split into many tasks.
 *
 * Use one instance per long-lived task (it must outlive the task — give it static
 * storage duration). Stacks + TCBs land in BSS; the type is trivial (no constructor,
 * no global ctor / static-init-order concerns).
 *
 *   static orb::osal::Task<2048> host_task;
 *   host_task.start("usb_host", usb_host_entry, nullptr, prio, 1u << 1);
 */
#pragma once

#include "FreeRTOS.h"
#include "task.h"

namespace orb::osal {

template <size_t StackWords>
class Task {
   public:
    // Create the task, pinned to the cores in core_mask (bit N = core N), and return
    // its handle (never null for a static create). Call once, before/while the
    // scheduler runs.
    TaskHandle_t start(const char *name, TaskFunction_t entry, void *arg,
                       UBaseType_t priority, UBaseType_t core_mask) {
        handle_ = xTaskCreateStaticAffinitySet(entry, name, StackWords, arg, priority,
                                               stack_, &tcb_, core_mask);
        return handle_;
    }

    TaskHandle_t handle() const { return handle_; }

   private:
    StackType_t stack_[StackWords];
    StaticTask_t tcb_;
    TaskHandle_t handle_ = nullptr;
};

}  // namespace orb::osal

