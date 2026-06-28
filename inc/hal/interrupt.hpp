/*
 * orb::hal — interrupt-masking interface (portable; NO SDK).
 *
 * A scoped, RAII per-core interrupt mask: masking THIS core's interrupts on construction
 * and restoring the prior state on destruction. This is a brief same-core critical section
 * (e.g. serializing the multi-producer core0 dlog ring) -- it is NOT a cross-core lock and
 * must never take a spinlock or stall the other core (the core1 PIO-USB timing forbids
 * it). The guard must be non-copyable so the mask is released exactly once.
 */
#ifndef ORB_HAL_INTERRUPT_HPP
#define ORB_HAL_INTERRUPT_HPP

#include <concepts>
#include <type_traits>

namespace orb::hal {

// A type usable as `{ IrqGuard g; ...critical... }`: default-constructible (masks on
// construct, restores in its destructor) and non-copyable (single release).
template <typename T>
concept ScopedIrqMask = std::is_default_constructible_v<T> &&
                        std::is_nothrow_destructible_v<T> && !std::is_copy_constructible_v<T>;

}  // namespace orb::hal

#endif  // ORB_HAL_INTERRUPT_HPP
