/*
 * Watchdog-reboot recovery machinery (orb::app::RebootRecovery), moved verbatim out of
 * main.cpp's recovery_reboot_task() + its reset-cause count-restore block. Pure move --
 * see modules/app/recovery.hpp for the class split rationale.
 */
#include "recovery.hpp"

#include <cstdint>

#include "adapter.h"  // adapter_state_t
#include "bsp/board_api.h"  // board_millis
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"  // watchdog_reboot / watchdog_caused_reboot
#include "dlog.h"
#include "orb_log.h"

namespace orb::app {

// Bounded auto-reboot warm-reset recovery, gated on authentication. A controller
// frozen across a warm reset can mount but stay a "zombie" -- enumerated yet silent
// (no heartbeat, LED off). Recovery is probabilistic, so we watchdog-reboot and retry
// until it comes back live (lands a functional controller within a reboot or two).
//
// The retry runs ONLY before auth: a controller is required to establish auth (and to
// re-establish it after an Xbox-initiated reset), so a zombie -- or a controller lost
// part-way through auth -- should be recovered. Once authenticated the controller is
// optional (the user may unplug it freely, drum input comes over serial), so we disarm
// and never reboot again. If nothing ever mounts there's no controller to recover, so
// we don't reboot then either. The attempt count rides through our watchdog reboots
// (scratch[7]) but resets on a fresh power-on / physical reset, so we never loop forever.
constexpr uint32_t kRecovScratch = 7u;    // scratch[4..6] are used by the SDK/bootrom watchdog path; [7] is free with pc=0 reboots
constexpr uint32_t kRecovMagic = 0x5A5A0000u;
constexpr uint32_t kRecovMax = 4u;
constexpr uint32_t kRecovSilentMs = 3000u; // a mounted-but-silent controller this long is a zombie

void RebootRecovery::restore_count_from_scratch() {
    // Recover our auto-reboot attempt count: only trust the scratch register if
    // *we* triggered this reboot via watchdog. A power-on / physical reset clears
    // it to 0 so the user always gets a fresh recovery budget.
    if (watchdog_caused_reboot() &&
        (watchdog_hw->scratch[kRecovScratch] & 0xFFFF0000u) == kRecovMagic) {
        recov_count_ = watchdog_hw->scratch[kRecovScratch] & 0xFFFFu;
    } else {
        recov_count_ = 0;
    }
    watchdog_hw->scratch[kRecovScratch] = 0;
    LOG_INFO(CAT_RECOV, "RECOVERY: attempt count = %lu", (unsigned long)recov_count_);
}

void RebootRecovery::service() {
    if (disarmed_) return;

    // Prefer the non-reboot runtime recovery: while core1 is actively pulsing the hub
    // RESET# to bring a lost controller back, hold off (and reset our debounce so the
    // reboot timer starts fresh once it stands down). Only fall back to a watchdog
    // reboot if that runtime recovery exhausts its attempts.
    if (recovery_state_.engaged()) { silent_since_ms_ = 0; return; }

    if (adapter_.state() >= adapter_state_t::STATE_RUNNING) {  // authenticated -> controller now optional
        disarmed_ = true;
        watchdog_hw->scratch[kRecovScratch] = 0;  // clear so the next reset starts fresh
        if (recov_count_) LOG_WARN(CAT_RECOV, "RECOVERY: authenticated after %lu reboot(s)",
                                    (unsigned long)recov_count_);
        return;
    }
    if (adapter_.alive()) { silent_since_ms_ = 0; return; }  // live -> healthy, nothing to do
    if (!adapter_.seen())  { silent_since_ms_ = 0; return; }  // none present -> nothing to recover

    // A controller mounted but isn't sending a heartbeat (zombie, or lost before auth).
    // Debounce a brief blip (re-enumeration) before acting.
    uint32_t now = board_millis();
    if (silent_since_ms_ == 0) silent_since_ms_ = now ? now : 1u;
    if ((uint32_t)(now - silent_since_ms_) < kRecovSilentMs) return;

    if (recov_count_ >= kRecovMax) {  // a zombie that won't thaw across retries
        disarmed_ = true;
        watchdog_hw->scratch[kRecovScratch] = 0;
        LOG_ERR(CAT_RECOV, "RECOVERY: controller stayed silent after %u reboots; replug needed", (unsigned)kRecovMax);
        return;
    }
    // Probabilistic: reboot and try again -- a later attempt usually lands a live one.
    watchdog_hw->scratch[kRecovScratch] = kRecovMagic | (recov_count_ + 1u);
    LOG_WARN(CAT_RECOV, "RECOVERY: controller silent pre-auth -> watchdog reboot %lu/%u",
             (unsigned long)(recov_count_ + 1u), (unsigned)kRecovMax);
    dlog_drain();  // flush the log before we go
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
}

}  // namespace orb::app
