/*
 * osal (OS Abstraction Layer): part of the firmware's ONLY dependency on FreeRTOS (see
 * mutex.hpp). orb::osal::ScopedCritical — an RAII guard over a FreeRTOS critical section.
 *
 * Under the RP2040 SMP port, taskENTER_CRITICAL() acquires the kernel's spinlock AND masks
 * interrupts on the calling core, so it provides TRUE cross-core mutual exclusion (unlike a
 * per-core IRQ mask such as hal::ScopedIrqMask, which only stops preemption on its own core).
 * Use it for a SHORT, bounded guarded section that two cores can enter — e.g. a lock-free
 * flag's check-then-set, which M0+ cannot do as an atomic RMW (no LDREX/STREX).
 *
 * Cost: the other core spins on the spinlock only for the few instructions the holder is
 * inside the section, and the holder runs with IRQs masked for that same span. Keep the body
 * tiny (a couple of loads/stores) — NEVER do I/O, logging, or anything blocking inside. In
 * particular this is safe to use briefly on core1 (the PIO-USB core) at rare events like
 * USB mount/umount, but must stay far away from the per-frame streaming path.
 *
 *   {
 *       orb::osal::ScopedCritical guard;   // enter on construct, exit on scope end
 *       won = !flag.load(rlx);
 *       if (won) flag.store(true, rlx);    // atomic across both cores
 *   }                                      // <- IRQs/ spinlock released here
 *
 * Task context ONLY (taskENTER_CRITICAL, not the *_FROM_ISR variant). All current callers
 * run in task context (the USB host/device tasks, the drum/serial task, the timer daemon).
 */
#pragma once

#include "FreeRTOS.h"
#include "task.h"

namespace orb::osal {

class ScopedCritical {
   public:
    ScopedCritical() { taskENTER_CRITICAL(); }
    ~ScopedCritical() { taskEXIT_CRITICAL(); }

    ScopedCritical(const ScopedCritical &) = delete;
    ScopedCritical &operator=(const ScopedCritical &) = delete;
};

}  // namespace orb::osal
