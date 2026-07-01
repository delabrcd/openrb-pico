#pragma once

// Watchdog-reboot recovery machinery, owned by orb::app::System. Two small classes:
//  - RecoveryState: the one genuinely cross-core flag (written core1, read core0).
//  - RebootRecovery: the core0 bounded auto-reboot recovery (was recovery_reboot_task()
//    + its file-static count/debounce state in main.cpp).
//
// See modules/app/recovery.cpp for the moved implementation + the kRecov* constants.

#include <atomic>
#include <cstdint>

#include "adapter_ctx.h"  // orb::service::AdapterState

namespace orb::app {

// Runtime-recovery liveness flag. Written on core1 (the hub-RESET# host recovery, currently
// host_recovery_task in main.cpp -> HostController later), read on core0 (RebootRecovery::
// service). Relaxed atomic single-word load/store only (M0+ has no LDREX/STREX -> an RMW
// emits an IRQ-masking libcall); single-writer, so no critical section.
class RecoveryState {
   public:
    bool engaged() const { return engaged_.load(std::memory_order_relaxed); }
    void set_engaged(bool v) { engaged_.store(v, std::memory_order_relaxed); }

   private:
    std::atomic<bool> engaged_{false};
};

// Bounded auto-reboot warm-reset recovery, gated on authentication (core0). A controller
// frozen across a warm reset can mount but stay a "zombie" -- enumerated yet silent (no
// heartbeat, LED off). Recovery is probabilistic, so we watchdog-reboot and retry until it
// comes back live. See recovery.cpp for the full rationale (moved verbatim from main.cpp's
// recovery_reboot_task()).
class RebootRecovery {
   public:
    RebootRecovery(orb::service::AdapterState& adapter, RecoveryState& recovery_state)
        : adapter_(adapter), recovery_state_(recovery_state) {}

    // Init-time: restore the attempt count from the watchdog scratch register (trusted only
    // if WE triggered this reboot). Call once in init().
    void restore_count_from_scratch();

    // core0 periodic (housekeeping): the bounded watchdog-reboot recovery, gated on auth.
    void service();

   private:
    orb::service::AdapterState& adapter_;
    RecoveryState& recovery_state_;
    std::uint32_t recov_count_ = 0;      // was the file-static g_recov_count
    bool disarmed_ = false;              // was recovery_reboot_task's static `disarmed`
    std::uint32_t silent_since_ms_ = 0;  // was recovery_reboot_task's static `silent_since_ms`
};

}  // namespace orb::app
