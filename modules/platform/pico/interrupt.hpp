/*
 * platform::pico — interrupt-mask guard (boundary: may include the SDK).
 *
 * save_and_disable_interrupts()/restore_interrupts() operate on the CALLING core's PRIMASK
 * only (the two M0+ cores have independent masks) and take NO cross-core spinlock, so this
 * never stalls the other core or its PIO-USB SOF. Save/restore (not unconditional enable)
 * makes the guard correctly nestable. Satisfies orb::hal::ScopedIrqMask.
 */
#ifndef ORB_PLATFORM_PICO_INTERRUPT_HPP
#define ORB_PLATFORM_PICO_INTERRUPT_HPP

#include <cstdint>

#include "hardware/sync.h"

namespace orb::platform::pico {

class IrqGuard {
   public:
    IrqGuard() noexcept : saved_(save_and_disable_interrupts()) {}
    ~IrqGuard() noexcept { restore_interrupts(saved_); }

    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;

   private:
    uint32_t saved_;
};

}  // namespace orb::platform::pico

#endif  // ORB_PLATFORM_PICO_INTERRUPT_HPP
