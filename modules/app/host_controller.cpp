/*
 * orb::app::HostController — board-dependent bodies for the core1 USB-HOST controller
 * loop (P4 slice 3 of the rearchitect/di-classes DI pass). Moved VERBATIM out of
 * modules/app/main.cpp: host_out_packet / g_host_last_rx_us / g_host_rx_count / xboxh_send
 * / xboxh_mount_cb / xboxh_umount_cb / handle_controller_packet_running /
 * xboxh_packet_received_cb / xboxh_packet_sent_cb / configure_host / the kHostRecov*
 * constants / host_recovery_task / usb_host_task. Only the substitutions needed to hang
 * off HostController member state were applied -- no new indirection, no blocking calls,
 * same core1-safe primitives (timer_hw->timerawl, tight_loop_contents(), tuh_task_ext).
 *
 * The single orb::core::SeamAnchor<HostController> is bound once, from
 * orb::app::bind_usb_seams() (system.cpp), before tuh_init() runs on core1 -- exactly the
 * drums MIDI seam's pattern (drums_midi_seam.cpp).
 */
#include "host_controller.hpp"

#include <cstdint>
#include <utility>  // std::to_underlying

#include "orb_bsp.h"  // ORB_BOARD_ID, ORB_BOARD_ID_CUSTOM_REV_0_1

#include <bsp/board_api.h>
#include <pico/stdlib.h>       // tight_loop_contents
#include "hardware/dma.h"
#include "hardware/timer.h"    // timer_hw
#include "host/usbh.h"         // tuh_task_ext / tuh_configure / tuh_init
#include "pio_usb_configuration.h"

#include "drums_midi_seam.h"        // drums_read_midi_host
#include "instrument_manager.h"     // instruments_e
#include "orb_log.h"
#include "usb_log.h"                // usb_log_task
#include "xbox_controller_driver.h"  // xboxh_send_report/reinit/error_streak + the weak cb decls
#include "xbox_one_protocol.h"       // fill_drum_input_from_controller / get_command_name

#include "core/seam_anchor.hpp"

namespace orb::app {
namespace {

// FIRST_XBOX_CONTROLLER_IDX removed -- unused constant (see main.cpp history).
inline constexpr uint8_t kHostControllerId = 1u;

// Runtime (no-reboot) host recovery, run from the core1 host loop.
//
// ROOT CAUSE: the Xbox controller sits one repeater hop behind the CH334R hub, so
// TinyUSB only learns it dropped via the hub's interrupt-IN port-status-change
// reports. When SOF stops (debugger halts both cores, or any transient glitch) the
// CH334R wedges: it stops reporting port changes AND keeps its upstream D+ pull-up
// asserted, so the root port never goes SE0. Result: TinyUSB sees neither a hub
// disconnect nor a downstream port change -- the controller stays "mounted" but its
// interrupt-IN transfers quietly error/time out (xboxh_xfer_cb ignores result and
// just re-arms), so it lingers as a silent zombie that never re-enumerates.
//
// FIX: detect the silence on core1 and pulse the hub's active-low RESET# (the exact
// boot-time reset_usb_hub() recovery). Asserting RESET# drops the hub's upstream
// pull-up -> the root port finally sees SE0 -> Pico-PIO-USB raises a disconnect ->
// TinyUSB's process_removed_device() tears down the hub and, recursively, every
// downstream device (the controller umounts cleanly), then re-enumerates the whole
// tree on release. No host re-init / tuh_deinit needed -- we lean on the normal,
// well-tested attach/detach path; only the wedged silicon needs the RESET# kick.
//
// This is the PREFERRED recovery (works pre- AND post-auth, no chip reboot);
// RebootRecovery::service (core0, see recovery.hpp) defers to it via RecoveryState and only
// watchdog-reboots if this gives up. Custom board only -- FEATHER has no hub.
#if ORB_BOARD_ID == ORB_BOARD_ID_CUSTOM_REV_0_1
// "Wedged" is detected two ways (measured on hardware):
//  1. FAST -- a wedged CH334R fails the controller's interrupt-IN poll continuously
//     (~80 failures/s), while a healthy idle pad produces NO completions between its
//     sparse packets (NAKs don't complete). So a run of consecutive IN failures is a
//     ~1 s, false-positive-free wedge signal (xboxh_in_error_streak()).
//  2. BACKSTOP -- in case a wedge ever stops the IN poll entirely instead of failing
//     it, also trigger on prolonged silence. The threshold must exceed the pad's idle
//     CMD_STATUS keep-alive (~20 s, which refreshes host_last_rx_us_), so 30 s clears
//     the heartbeat with margin. In active play input streams sub-second, so neither
//     path fires spuriously there.
inline constexpr uint32_t kHostRecovErrStreak = 100u;     // ~1.25 s of continuous IN failures -> wedged (fast)
inline constexpr uint32_t kHostRecovSilenceUs = 30000000u; // no packet at all this long -> wedged (backstop)
inline constexpr uint32_t kHostRecovGraceUs = 3000000u;    // wait this long between resets for re-enumeration
inline constexpr uint8_t  kHostRecovMax = 3u;              // bounded so a genuine unplug can't thrash the hub forever
#endif

orb::core::SeamAnchor<HostController> g_host;

}  // namespace

void bind_host_controller(HostController& hc) { g_host.bind(hc); }

// ---- HostController method bodies (moved verbatim from main.cpp) -----------------------

void HostController::on_mount(std::uint8_t dev_addr, std::uint8_t instance) {
    LOG_INFO(CAT_HOST, "Controller %d Connected", instance);
    adapter_.set_seen(true);  // a controller mounted this boot (may still be a zombie)
    // Always follow the most-recently-connected controller. On a replug the device
    // gets a new instance/address (and, if the old umount is missed or races, the
    // stale slot can linger), so adopting only when idx==UINT8_MAX would leave us
    // forwarding from the wrong slot -- the controller re-enumerates (LED on) but
    // its inputs get filtered by on_packet_received. We only track one active
    // controller, so taking over on every mount is correct and replug-safe.
    adapter_.set_controller(instance, dev_addr);
    // Grant a fresh silence window so the runtime recovery (run()) doesn't
    // mistake the gap between mount and the first input report for a wedged hub.
    host_last_rx_us_.store(timer_hw->timerawl, std::memory_order_relaxed);
}

void HostController::on_umount(std::uint8_t dev_addr, std::uint8_t instance) {
    (void)dev_addr;
    LOG_INFO(CAT_HOST, "Controller %d Disconnected", instance);
    if (instance == adapter_.controller_idx()) {
        adapter_.clear_controller(instance);
        adapter_.set_alive(false);  // require a fresh heartbeat from the next mount
    }
}

void HostController::handle_controller_packet_running(const XboxPacket& data) {
    switch (data.frame().command) {
        case frame_command_e::CMD_ANNOUNCE:
            // Controller re-attached and is announcing -- it won't stream input until
            // the host re-inits it. Defer to the core1 loop (the init blocks on tx).
            adapter_.request_reinit();
            break;

        case frame_command_e::CMD_GUIDE_BTN:
            tx_fifo_.write(data);
            break;

        case frame_command_e::CMD_INPUT:
            fill_drum_input_from_controller(&data, &host_out_packet_,
                                            std::to_underlying(instruments_e::DRUMS));
            tx_fifo_.write(host_out_packet_);
            break;
        default:
            break;
    }
}

void HostController::on_packet_received(std::uint8_t idx, const XboxPacket& data,
                                         std::uint8_t ndata) {
    if (idx != adapter_.controller_idx()) return;
    if (ndata < sizeof(frame_t)) return;
    adapter_.set_alive(true);  // a real packet arrived -> controller is alive, not a zombie
    host_last_rx_us_.store(timer_hw->timerawl,
                            std::memory_order_relaxed);  // feed the runtime-recovery silence timer (core1)
    host_rx_count_.store(host_rx_count_.load(std::memory_order_relaxed) + 1u,
                         std::memory_order_relaxed);  // tick so recovery can detect a fresh heartbeat
    LOG_TRC(CAT_WIRE, "IN FROM CONTROLLER: %s", get_command_name(std::to_underlying(data.frame().command)));
    switch (adapter_.state()) {
        case adapter_state_t::STATE_AUTHENTICATING:
            tx_fifo_.write(data);
            break;
        // case adapter_state_t::STATE_POWER_OFF:
        //     break;
        case adapter_state_t::STATE_RUNNING:
            handle_controller_packet_running(data);
            break;
        default:
            break;
    }
}

void HostController::on_packet_sent(std::uint8_t idx, const XboxPacket& data,
                                     std::uint8_t ndata) {
    (void)idx;
    (void)data;
    (void)ndata;
    LOG_TRC(CAT_WIRE, "Sent Controller %d bytes (%s)", ndata, get_command_name(std::to_underlying(data.frame().command)));
}

void HostController::configure_host() {
    LOG_INFO(CAT_HOST, "configuring usb host stack");

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = orb::board::pin_usb_host_dp;

    // find an unused channel
    pio_cfg.tx_ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(pio_cfg.tx_ch);
    tuh_configure(kHostControllerId, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(kHostControllerId);
    LOG_INFO(CAT_HOST, "finished configuring usb host");

    actuators_.set_usb_host(true);
}

#if ORB_BOARD_ID == ORB_BOARD_ID_CUSTOM_REV_0_1
void HostController::host_recovery() {
    uint32_t now = timer_hw->timerawl;

    // A fresh heartbeat means a healthy controller: exit recovery and refill the budget.
    uint32_t rxc = host_rx_count_.load(std::memory_order_relaxed);
    if (rxc != last_rx_count_) {
        last_rx_count_ = rxc;
        recovering_ = false;
        gave_up_ = false;
        attempts_ = 0;
    }

    bool wedged = xboxh_in_error_streak() >= kHostRecovErrStreak ||
                  (uint32_t)(now - host_last_rx_us_.load(std::memory_order_relaxed)) > kHostRecovSilenceUs;

    // Detect a loss: a controller mounted this boot but has since gone wedged. A clean
    // unplug also looks like this, so attempts are bounded (gave_up_) until input returns.
    if (!recovering_ && !gave_up_ && adapter_.seen() && wedged) {
        recovering_ = true;
        attempts_ = 0;
        last_attempt_us_ = now - kHostRecovGraceUs;  // act on the first pass
    }

    if (recovering_) {
        if (attempts_ >= kHostRecovMax) {
            // Couldn't bring it back -> stand down and let the reboot path (last resort)
            // decide. Stay quiet until a real packet refills the budget (got input above).
            recovering_ = false;
            gave_up_ = true;
        } else if ((uint32_t)(now - last_attempt_us_) >= kHostRecovGraceUs) {
            attempts_++;
            last_attempt_us_ = now;
            host_last_rx_us_.store(now, std::memory_order_relaxed);  // suppress the silence backstop during re-enumeration
            xboxh_clear_error_streak();    // fresh count; GRACE lets re-enum land before it re-trips
            LOG_WARN(CAT_RECOV, "HOST RECOVERY: controller wedged -> hub reset %u/%u",
                     (unsigned)attempts_, (unsigned)kHostRecovMax);
            actuators_.reset_usb_hub();  // RESET# pulse (busy_wait_ms, core1-safe); TinyUSB re-enumerates
        }
    }

    recovery_state_.set_engaged(recovering_);  // gate the core0 watchdog-reboot path
}
#else
void HostController::host_recovery() {}  // no hub / no RESET# pin on this board
#endif

// USB host task. Pinned to core1 and the ONLY task that runs there, so the
// PIO-USB bit-banged signalling sees ~no FreeRTOS context switches. The SMP
// scheduler launches core1 itself in vTaskStartScheduler(), so there is no more
// multicore_launch_core1()/launch_core1_robust() (and thus no early-launch FIFO
// handshake race -- ROOT CAUSE #1 is now owned by the FreeRTOS port).
void HostController::run() {
    // Settle before bringing up PIO-USB (matches the Pico-PIO-USB examples'
    // sleep_ms(10)); lock-free timer wait to avoid any alarm-pool dependency.
    uint32_t t = timer_hw->timerawl;
    while ((uint32_t)(timer_hw->timerawl - t) < 10000u) tight_loop_contents();
    configure_host();
    last_reinit_us_ = 0;  // lock-free timer (timerawl) -- board_millis() spinlock hangs core1
    while (true) {
        // tuh_task_ext(10): block on the host event queue but wake at least every 10ms
        // so the reinit/host-tx/midi/usb_log/recovery work below still runs when the bus
        // is idle. (Under OPT_OS_FREERTOS a plain tuh_task() blocks forever.)
        tuh_task_ext(10, false);
        // A running controller that re-announced needs its init re-sent (Issue: a
        // post-auth replug leaves it announcing forever, never streaming input).
        // Debounce so we re-init at most ~2x/s instead of on every announce.
        if (adapter_.take_reinit()) {
            uint32_t now = timer_hw->timerawl;
            uint8_t idx, addr;
            if (adapter_.controller(&idx, &addr) &&
                (uint32_t)(now - last_reinit_us_) > 500000u) {
                last_reinit_us_ = now;
                LOG_WARN(CAT_RECOV, "Controller re-announced -> re-init");
                xboxh_reinit_controller(addr, idx);
            }
        }

        host_recovery();

        // Drain host-TX packets enqueued by core0 device handlers; submit on core1 (the
        // core that owns the host stack).
        XboxPacket txp;
        while (host_tx_.recv(txp)) {
            uint8_t idx, addr;
            if (adapter_.controller(&idx, &addr)) {
                xboxh_send_report(addr, idx, &txp, txp.length);
            }
        }

        // Read USB-host MIDI on core1 and hand parsed notes to core0 via the queue.
        drums_read_midi_host();

        usb_log_task();        // drain the log ring out to the USB flash drive
    }
}

}  // namespace orb::app

// --- extern "C" TinyUSB host controller seam ---------------------------------------------
// TinyUSB calls these by C symbol; each reaches the single HostController instance (owned
// by orb::app::System) through the SeamAnchor bound at init. Direct concrete call, no
// vtable -- core1 hot path.

extern "C" void xboxh_mount_cb(uint8_t dev_addr, uint8_t instance) {
    if (orb::app::g_host) orb::app::g_host->on_mount(dev_addr, instance);
}

extern "C" void xboxh_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (orb::app::g_host) orb::app::g_host->on_umount(dev_addr, instance);
}

extern "C" void xboxh_packet_received_cb(uint8_t idx, const XboxPacket *data, const uint8_t ndata) {
    if (orb::app::g_host) orb::app::g_host->on_packet_received(idx, *data, ndata);
}

extern "C" void xboxh_packet_sent_cb(uint8_t idx, const XboxPacket *data, const uint8_t ndata) {
    if (orb::app::g_host) orb::app::g_host->on_packet_sent(idx, *data, ndata);
}
