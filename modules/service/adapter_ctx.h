#pragma once

// Cross-core adapter state shared between core0 (USB device stack / device-RX
// callbacks) and core1 (USB host stack / controller mount-umount-RX). Previously a
// loose set of `volatile` globals in main.c; consolidated here as a clean modern-C++
// service: orb::service::AdapterState, reached through the orb::service::adapter()
// singleton accessor. No extern "C" facade -- every consumer is C++ and calls the
// object directly.
//
// Concurrency (RP2040, dual-core Cortex-M0+ where aligned 32-bit/byte loads/stores
// are atomic). std::atomic load()/store() compile to a plain ldr/str here -- we use
// ONLY load/store (never an atomic read-modify-write: M0+ has no LDREX/STREX, so
// fetch_add/exchange would emit an interrupt-masking libcall). The fields are
// independent signals with no cross-field happens-before requirement, so
// memory_order_relaxed reproduces exactly the previous `volatile` semantics (plain
// accesses, no barrier). std::atomic has a constexpr constructor, so the static
// instance is constant-initialised -> BSS, no global ctor.
//  - state: written core0, read both cores.
//  - controller idx+addr: written core1 (mount/umount), read core0 + core1. Packed into
//    ONE 32-bit word so a replug can't be observed half-updated (torn read); published
//    with a single store and a sentinel for "no controller".
//  - alive / seen: written core1, read core0.
//  - reinit: producer and consumer are both core1.

#include <atomic>
#include <cstdint>

#include "adapter.h"  // adapter_state_t

namespace orb::service {

class AdapterState {
   public:
    // Packed controller slot: (idx << 8) | addr, or kNone as the "no controller" sentinel.
    // One 32-bit word so core1 publishes a replug with a single store and core0 never sees
    // a half-updated idx/addr pair.
    static constexpr uint32_t kNone = UINT32_MAX;

    void reset() {  // set state=STATE_NONE, no controller, flags false
        state_.store(STATE_NONE, kRlx);
        controller_.store(kNone, kRlx);
        alive_.store(false, kRlx);
        seen_.store(false, kRlx);
        reinit_.store(false, kRlx);
    }

    adapter_state_t state() const { return state_.load(kRlx); }
    void set_state(adapter_state_t s) { state_.store(s, kRlx); }

    // controller slot (idx+addr packed). returns false if no controller currently tracked.
    bool controller(uint8_t* idx, uint8_t* addr) const {
        uint32_t packed = controller_.load(kRlx);  // single atomic load
        if (packed == kNone) return false;
        if (idx) *idx = static_cast<uint8_t>(packed >> 8);
        if (addr) *addr = static_cast<uint8_t>(packed & 0xFF);
        return true;
    }
    void set_controller(uint8_t idx, uint8_t addr) {
        controller_.store((static_cast<uint32_t>(idx) << 8) | addr, kRlx);  // single store
    }
    void clear_controller(uint8_t idx) {  // only clears if idx matches the tracked one
        uint32_t packed = controller_.load(kRlx);
        if (packed != kNone && static_cast<uint8_t>(packed >> 8) == idx)
            controller_.store(kNone, kRlx);
    }
    uint8_t controller_idx() const {  // convenience: tracked idx or UINT8_MAX
        uint32_t packed = controller_.load(kRlx);
        return packed == kNone ? UINT8_MAX : static_cast<uint8_t>(packed >> 8);
    }

    bool alive() const { return alive_.load(kRlx); }
    void set_alive(bool v) { alive_.store(v, kRlx); }
    bool seen() const { return seen_.load(kRlx); }
    void set_seen(bool v) { seen_.store(v, kRlx); }

    void request_reinit() { reinit_.store(true, kRlx); }  // producer: set reinit_pending
    bool take_reinit() {  // consumer (also core1): read-and-clear; plain load+store (not a RMW)
        bool prior = reinit_.load(kRlx);
        reinit_.store(false, kRlx);
        return prior;
    }

   private:
    static constexpr std::memory_order kRlx = std::memory_order_relaxed;

    std::atomic<adapter_state_t> state_{STATE_NONE};  // written core0, read both
    std::atomic<uint32_t> controller_{kNone};         // written core1, read both
    std::atomic<bool> alive_{false};                  // written core1, read core0
    std::atomic<bool> seen_{false};                   // written core1, read core0
    std::atomic<bool> reinit_{false};                 // core1 only
};

// The single cross-core adapter-state instance (defined in adapter_ctx.cpp).
AdapterState& adapter();

}  // namespace orb::service

